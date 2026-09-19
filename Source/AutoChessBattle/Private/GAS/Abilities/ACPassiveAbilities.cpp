// 阶段 3.1b 新增（GAS 重构实施方案 §4.2 / §7 阶段 3.1）：事件触发被动能力的实现。
// 每个类的"内容来源 / 触发面差异 / MaxTriggers 落地"都写在头文件里，本文件只写执行步骤。
#include "GAS/Abilities/ACPassiveAbilities.h"

#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "Core/ACBattleTags.h"
#include "Diagnostics/ACBattleDiagnostics.h"
#include "GAS/Effects/ACGE_ContentEffects.h"
#include "GAS/Effects/ACGE_Costs.h"
#include "GAS/Effects/ACGE_States.h"

// =============================================================================
//  被动能力基类
// =============================================================================

UACPassiveAbilityBase::UACPassiveAbilityBase()
{
    // 唯一要设的东西是"不要在激活前门控目标"—— 它在头文件里以覆写形式给出
    //（`RequiresTargetInRange() → false`），因此这里没有额外代码。
    //
    // 为什么不在这个构造里加 `ActivationBlockedTags`：沉默 / 眩晕**不挡被动**
    //（旧实现里由 `FEffectSystem::HandleHook` 触发的效果块同样不看这些状态），
    // 往这里加标签会让"眩晕期间被动静默失效"，那是一处不易察觉的数值变更。
    //
    // 为什么不在这个构造里加 `AbilityTriggers`：每个被动的触发标签不同（`Hook.SkillCast` /
    // `Hook.Kill` / `Hook.AfterAttack` / `Hook.Death` / `Hook.BattleStart` / `Hook.Hit`），
    // 由各自子类在构造里调 `AddEventTrigger(...)` 登记 —— 基类如果默认登记一个标签，
    // 子类会很自然地"忘了改"，表现为"这个被动监听了别人的事件"。
}

// =============================================================================
//  索利瓦尔被动 · 嗜血
// =============================================================================

UACPassive_Solivar_BloodThirst::UACPassive_Solivar_BloodThirst()
{
    // 内容：`TriggerTag = Hook.SkillCast`（ACBattleContentDefinitions.cpp:258-260）。
    AddEventTrigger(BattleTags::Hook_SkillCast);

    // 触发次数不限：内容块**没有** `MaxTriggers`（每次放技能都该回血）。
}

void UACPassive_Solivar_BloodThirst::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                                     const FGameplayAbilityActorInfo* ActorInfo,
                                                     const FGameplayAbilityActivationInfo ActivationInfo,
                                                     const FGameplayEventData* TriggerEventData)
{
    // 事件载荷 → 钩子上下文（`Instigator` = 施法者、`Target` = 技能主目标）。
    const FBattleHookContext Context = MakeHookContext(TriggerEventData);

    AACBattleUnitBase* const Self = GetCasterUnit();
    if (Self == nullptr)
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 条件 ①：**是不是自己放的技能**（旧实现不按 owner 过滤，见头文件的说明）。
    if (Context.Source != Self->GetUnitId())
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 条件 ②：职业标签 `Unit.Class.Warrior`（内容块 :261-264 的 `HasTag` 条件）。
    // 旧实现判的是 `Context.PrimaryTarget`（技能目标）而不是施法者，见头文件的差异说明；
    // 新实现按内容本意判施法者自己。
    if (!Self->HasTag(BattleTags::Unit_Class_Warrior))
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 效果：治疗 8 → 清空专注（顺序与内容块的动作顺序一致：`ApplyHeal` 在前、`DrainFocus` 在后）。
    // 治疗量由 GE 承载（`UACGE_Passive_OP01_BloodThirst_Heal` 的 `FixedHealAmount = 8`，
    // 由 `UACHealExecution` 从 `Spec.Def` 读出），施加方不需要填 SetByCaller。
    ApplyEffectToUnit(*Self, *Self, UACGE_Passive_OP01_BloodThirst_Heal::StaticClass());

    // 清空专注复用 3.1a 的 `UACGE_Cost_ClearAll`（`Focus += -999`，靠属性集的夹取归零）——
    // 与索利瓦尔的技能消耗是同一个 GE，正是内容里"放完技能清空专注"的那一句
    //（ACBattleContentDefinitions.cpp:252-256 的 `DrainFocus(999)`）。
    ApplyEffectToUnit(*Self, *Self, UACGE_Cost_ClearAll::StaticClass());

    FinishAbility(/*bWasCancelled=*/false);
}

// =============================================================================
//  S 级强化 · 处刑（累计击杀 20 → 一次性永久加成）
// =============================================================================

UACPassive_Upgrade_OP01_S1::UACPassive_Upgrade_OP01_S1()
{
    // 内容：`TriggerTag = Hook.Kill`、`MaxTriggers = 1`（ACBattleContentDefinitions.cpp:290-291）。
    AddEventTrigger(BattleTags::Hook_Kill);
}

void UACPassive_Upgrade_OP01_S1::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                                 const FGameplayAbilityActorInfo* ActorInfo,
                                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                                 const FGameplayEventData* TriggerEventData)
{
    AACBattleUnitBase* const Self = GetCasterUnit();
    UBattleWorld* const World = GetBattleWorld();
    if (Self == nullptr || World == nullptr)
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // ⚠️ 顺序：**先判条件、再计数**。
    // 旧系统里 `TriggerCount` 是在 `ExecuteBlock` **之后**才自增的
    //（ACEffectSystem.cpp:200-214），也就是说"条件不满足"不会消耗那唯一一次机会；
    // 反过来写会让"第 1 次击杀没满 20"直接烧掉 `MaxTriggers = 1`，这个被动永远失效。
    const int32 KillCount = World->Stats().GetKillsBySource(Self->GetUnitId());
    if (KillCount < RequiredKills)
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    if (!ConsumeTrigger(MaxTriggers))
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 效果：永久 `CritValue + 50` + `ATK + 80`（`UACGE_Upgrade_OP01_S1`，3.1a，Infinite）。
    ApplyEffectToUnit(*Self, *Self, UACGE_Upgrade_OP01_S1::StaticClass());

    FinishAbility(/*bWasCancelled=*/false);
}

// =============================================================================
//  B 级词条 · 心流（每次击杀回 15 专注）
// =============================================================================

UACPassive_Trait_FocusSurge::UACPassive_Trait_FocusSurge()
{
    // 内容：`TriggerTag = Hook.Kill`（ACBattleContentDefinitions.cpp:330-331）。
    AddEventTrigger(BattleTags::Hook_Kill);
}

void UACPassive_Trait_FocusSurge::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                                  const FGameplayAbilityActorInfo* ActorInfo,
                                                  const FGameplayAbilityActivationInfo ActivationInfo,
                                                  const FGameplayEventData* TriggerEventData)
{
    AACBattleUnitBase* const Self = GetCasterUnit();
    if (Self == nullptr)
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 效果：`Focus += 15`（`UACGE_Trait_FocusSurge`，3.1a，Instant）。
    //
    // 走 `GrantFocusToUnit` 而不是直接 `ApplyEffectToUnit`：旧实现在这条路径上是
    // `EACActionType::GrantFocus`（ACEffectSystem.cpp:706-708）→ `FAbilityExecutor::GrantFocus`，
    // 而 `GrantFocus` **不只是改值** —— 它还会派发 `Hook.FocusFull` / `Hook.FocusChanged`
    // 并写一条 `Focus | FocusChanged` 埋点（ACAbilityExecutor.cpp:608-629）。
    // `GrantFocusToUnit` 正是那三段行为的等价物（改值走 GE、钩子与埋点照旧），
    // 因此这里用它才能与旧实现逐项对齐。
    GrantFocusToUnit(*Self, FocusSurgeAmount);

    FinishAbility(/*bWasCancelled=*/false);
}

// =============================================================================
//  金·心流刃（常驻段）：自己普攻后回 2 专注
// =============================================================================

UACPassive_Equip_FlowBlade_OnAttack::UACPassive_Equip_FlowBlade_OnAttack()
{
    // 内容：`TriggerTag = Hook.Attack.After`（ACBattleContentDefinitions.cpp:367-369）。
    AddEventTrigger(BattleTags::Hook_AfterAttack);
}

void UACPassive_Equip_FlowBlade_OnAttack::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                                          const FGameplayAbilityActorInfo* ActorInfo,
                                                          const FGameplayAbilityActivationInfo ActivationInfo,
                                                          const FGameplayEventData* TriggerEventData)
{
    const FBattleHookContext Context = MakeHookContext(TriggerEventData);

    AACBattleUnitBase* const Self = GetCasterUnit();
    if (Self == nullptr)
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 触发面：自己普攻之后（见头文件的差异说明）。
    // 这条判据同时让本被动对"3.3 把 Hook.AfterAttack 发给谁"不敏感：
    // 无论发给攻击者还是广播给全场，语义都是"自己打完才回专注"。
    if (Context.Source != Self->GetUnitId())
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 效果：`Focus += 2`（`UACGE_FocusGain_2`，本阶段新增的 Instant GE）。
    // ⚠️ **不用**常驻 `FocusPerAttack += 2`（那会与射手普攻回专注重复计数，见头文件裁决 ①）。
    //
    // 为什么这里走 `GrantFocusToUnit` 而不是直接 `ApplyEffectToUnit`：
    //   旧实现在这条路径上是 `EACActionType::GrantFocus`（ACEffectSystem.cpp:706-708）→
    //   `FAbilityExecutor::GrantFocus`，它**会**派发 `Hook.FocusFull` / `Hook.FocusChanged`
    //   并写 `Focus | FocusChanged` 埋点。`GrantFocusToUnit` 是那三段行为的等价物
    //（改值走 GE、钩子与埋点照旧），因此用它。
    //   量取内容的 2（`ACBattleContentDefinitions.cpp:363` 的 `GainFocus.Value = Tier(2.f)`）。
    GrantFocusToUnit(*Self, FlowBladeFocusGain);

    FinishAbility(/*bWasCancelled=*/false);
}

// =============================================================================
//  腐殖蛆：死亡时周围 1 格 3 层中毒
// =============================================================================

UACPassive_Enemy_Worm_DeathToxin::UACPassive_Enemy_Worm_DeathToxin()
{
    // 内容：`TriggerTag = Hook.Death`、`MaxTriggers = 1`、`SelectorRadius = 1`
    //（ACBattleContentDefinitions.cpp:409-413）。
    AddEventTrigger(BattleTags::Hook_Death);
}

EACSelectorType UACPassive_Enemy_Worm_DeathToxin::GetTargetSelector(const AACBattleUnitBase& Caster) const
{
    // 内容：`TargetSelector = EACSelectorType::Neighbors1`（ACBattleContentDefinitions.cpp:411）。
    return EACSelectorType::Neighbors1;
}

int32 UACPassive_Enemy_Worm_DeathToxin::GetSelectorRadius(const AACBattleUnitBase& Caster) const
{
    // 内容：`SelectorRadius = 1`（ACBattleContentDefinitions.cpp:412）。
    return 1;
}

void UACPassive_Enemy_Worm_DeathToxin::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                                       const FGameplayAbilityActorInfo* ActorInfo,
                                                       const FGameplayAbilityActivationInfo ActivationInfo,
                                                       const FGameplayEventData* TriggerEventData)
{
    const FBattleHookContext Context = MakeHookContext(TriggerEventData);

    AACBattleUnitBase* const Self = GetCasterUnit();
    UBattleWorld* const World = GetBattleWorld();
    if (Self == nullptr || World == nullptr)
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 触发面：**死者是自己**（`Hook.Death` 的 Target 是死者，见 ACBattleWorld.cpp:995-1000）。
    // 旧实现不判这一条，任何单位死亡都会让每只蠕虫在自己周围放毒 —— 那是旧缺陷，见头文件。
    if (Context.Target != Self->GetUnitId())
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    if (!ConsumeTrigger(MaxTriggers))
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 目标：周围 1 格（`Neighbors1`，不含自己；走基类的 `ResolveTargets`）。
    TArray<FUnitId> Targets;
    if (!ResolveTargets(*Self, Targets))
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    for (const FUnitId TargetId : Targets)
    {
        AACBattleUnitBase* const Target = World->FindUnit(TargetId);
        if (Target == nullptr || !Target->IsAlive())
        {
            // `ResolveSelector` 已经过滤过存活；这里再判一次是因为中间可能发生了死亡
            //（解析与实际施加之间没有事务保护）。
            continue;
        }

        // 效果：3 层 `State.Poison`（`UACGE_State_Poison`，3.1a；`MaxStacks = 0` 不限、
        // `StackAndRefresh`、周期 1 秒、每层 1 点技术伤害 —— 与内容里的状态定义一致）。
        // 层数走 `FGameplayEffectSpec::SetStackCount`（引擎在首次施加时用它当起始层数，
        // 已有同源实例时累加），与旧 `Apply(StateTag, Source, Stacks = 3, ...)` 的层数口径一致。
        ApplyEffectToUnit(*Self, *Target, UACGE_State_Poison::StaticClass(), PoisonStacks);

        // 统计埋点：旧 `EACActionType::ApplyAbnormalState` 会记一条
        // `World->Stats().RecordAbnormalStacks(StateTag.GetTagName(), Stacks)`
        //（ACEffectSystem.cpp:652），`_stats.json` 的 `AbnormalStacksApplied` 表靠它。
        // 新路径下这条埋点没有别人替我们记（GE 的施加不经过 `FEffectSystem`），因此在这里补上，
        // 口径逐字一致：标签名 + **意图层数**（不是实际生效层数，旧的也是这么记的）。
        World->Stats().RecordAbnormalStacks(BattleTags::State_Poison.GetTag().GetTagName(), PoisonStacks);
    }

    FinishAbility(/*bWasCancelled=*/false);
}

// =============================================================================
//  蠕虫母巢：开局召唤 4 只
// =============================================================================

UACPassive_Enemy_MotherNest_Spawn::UACPassive_Enemy_MotherNest_Spawn()
{
    // 内容：`TriggerTag = Hook.BattleStart`、`MaxTriggers = 1`（ACBattleContentDefinitions.cpp:434-436）。
    AddEventTrigger(BattleTags::Hook_BattleStart);
}

void UACPassive_Enemy_MotherNest_Spawn::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                                        const FGameplayAbilityActorInfo* ActorInfo,
                                                        const FGameplayAbilityActivationInfo ActivationInfo,
                                                        const FGameplayEventData* TriggerEventData)
{
    AACBattleUnitBase* const Self = GetCasterUnit();
    if (Self == nullptr)
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // `Hook.BattleStart` 的上下文里 `Source` / `Target` 都是 `InvalidUnitId`
    //（旧内核派发时只填了 Time，ACBattleWorld.cpp:261-263），因此这里**没有**可判的条件：
    // "该不该触发"完全由"这条事件有没有发给这个 ASC"决定（3.3 需要广播给全场，见头文件）。
    if (!ConsumeTrigger(MaxTriggers))
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 效果：`UACGE_Enemy_MotherNest_Spawn`（3.1a：Instant + `UACSummonEffectComponent`
    // 的 `SummonSpecId = SUM_Wormling` / `SummonCount = 4`）。
    // 召唤的归属单位：组件从 context 的 instigator 取（`ACSummonEffectComponent.cpp:45-58`），
    // 而我们的 `ApplyEffectToUnit` 把 instigator 设成**来源单位**（这里就是母巢自己），
    // 因此 4 只蠕虫的 `OwnerUnitId` = 母巢 —— 与旧 `SummonUnit` 动作拿 `Context.Source`
    //（块的所有者 = 母巢，ACEffectSystem.cpp:749-754）逐字一致。
    ApplyEffectToUnit(*Self, *Self, UACGE_Enemy_MotherNest_Spawn::StaticClass());

    FinishAbility(/*bWasCancelled=*/false);
}

// =============================================================================
//  巨蛭：每次命中挂 2 层流血 / 5 秒
// =============================================================================

UACPassive_Enemy_Leech_Bite::UACPassive_Enemy_Leech_Bite()
{
    // 内容：`TriggerTag = Hook.Hit`（ACBattleContentDefinitions.cpp:452-453）。
    // **没有** `MaxTriggers`（每次命中都要挂），因此不调 `ConsumeTrigger`。
    AddEventTrigger(BattleTags::Hook_Hit);
}

void UACPassive_Enemy_Leech_Bite::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                                  const FGameplayAbilityActorInfo* ActorInfo,
                                                  const FGameplayAbilityActivationInfo ActivationInfo,
                                                  const FGameplayEventData* TriggerEventData)
{
    const FBattleHookContext Context = MakeHookContext(TriggerEventData);

    AACBattleUnitBase* const Self = GetCasterUnit();
    UBattleWorld* const World = GetBattleWorld();
    if (Self == nullptr || World == nullptr)
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 触发面：自己是攻击者（`Hook.Hit` 的 Source = 攻击者、Target = 被命中者，
    // 见普攻能力的 `HitContext` 组装）。旧实现不判这一条，见头文件。
    if (Context.Source != Self->GetUnitId())
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 目标：主目标（内容 `TargetSelector = PrimaryTarget`）。
    // 注意**不能**用 `ResolveTargets`：那走的是 `Self->GetCurrentTargetId()`，
    // 而这里的"主目标"是**本次命中的目标**（事件的 Target），两者在多目标/多段攻击下可能不同。
    AACBattleUnitBase* const Target = World->FindUnit(Context.Target);
    if (Target == nullptr)
    {
        FinishAbility(/*bWasCancelled=*/false);
        return;
    }

    // 效果：2 层、5 秒的流血 → `UACGE_State_Bleed_Leech`（3.1a：与 `UACGE_State_Bleed`
    // 同一个状态标签、但时长写死 5 秒的独立 GE 类，数值与默认流血一致）。
    // 为什么必须用 `_Leech` 那个类：默认流血的时长是"无限"（内容 :867），
    // 而这条内容是"5 秒"—— 引擎重算时长只看 `Def->DurationMagnitude`，因此只能换 GE 类。
    ApplyEffectToUnit(*Self, *Target, UACGE_State_Bleed_Leech::StaticClass(), BleedStacks);

    // 统计埋点：与腐殖蛆同理，补上旧 `ApplyAbnormalState` 记的那条。
    // ⚠️ 标签名用 `State.Bleed`（`_Leech` 是同一个状态标签，内容 :446 写的就是 `State.Bleed`）。
    World->Stats().RecordAbnormalStacks(BattleTags::State_Bleed.GetTag().GetTagName(), BleedStacks);

    FinishAbility(/*bWasCancelled=*/false);
}
