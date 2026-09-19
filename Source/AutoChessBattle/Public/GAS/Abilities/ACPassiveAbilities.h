// 阶段 3.1b 新增（GAS 重构实施方案 §4.2 的 `TriggerType = Hook` 行 / §7 阶段 3.1）：
// **事件触发的被动能力**（含词条 / 强化 / 装备常驻段 / 敌人特殊机制）。
//
// 承载形式：旧 `FACEffectBlock`（`TriggerType = Hook` + `TriggerTag`）由
// `FEffectSystem::HandleHook` 订阅 → 新为 `UGameplayAbility::AbilityTriggers`
// = `{ EGameplayAbilityTriggerSource::GameplayEvent, Hook.Xxx }`，由 `ASC->HandleGameplayEvent`
// 唤醒（§0 C10：只负责触发的走 GAS 事件）。
//
// ---------------------------------------------------------------------------
// 7 个被动 / 8 行内容清单的对应关系（"6 个被动"是 §7 的口径，逐行核对如下）
// ---------------------------------------------------------------------------
//   | 内容块                                     | 类                                    | 备注                         |
//   | ------------------------------------------ | ------------------------------------- | ---------------------------- |
//   | `Passive_OP01_BloodThirst`（索利瓦尔被动）    | `UACPassive_Solivar_BloodThirst`      | 治疗 8 + 清空专注             |
//   | `Upgrade_OP01_S1`（处刑，S 级强化）           | `UACPassive_Upgrade_OP01_S1`          | 击杀数 ≥ 20 后**一次**        |
//   | `Trait_DeadlyStrike`（处刑人，A 级词条）      | **不产出能力**（见下）                 | 只有一条常驻属性修饰          |
//   | `Trait_FocusSurge`（心流，B 级词条）          | `UACPassive_Trait_FocusSurge`         | 每次击杀回 15 专注            |
//   | `Equip_Gold_FlowBlade_OnAttack`（金·心流刃）  | `UACPassive_Equip_FlowBlade_OnAttack` | 普攻后回 2 专注               |
//   | `Enemy_Worm_DeathToxin`（腐殖蛆）             | `UACPassive_Enemy_Worm_DeathToxin`    | 死亡时周围 1 格 3 层中毒       |
//   | `Enemy_MotherNest_Spawn`（蠕虫母巢）          | `UACPassive_Enemy_MotherNest_Spawn`   | 开局召唤 4 只                 |
//   | `Enemy_Leech_Bite`（巨蛭）                    | `UACPassive_Enemy_Leech_Bite`         | 每次命中 2 层流血 / 5 秒       |
//
// **`Trait_DeadlyStrike` 为什么不产能力**（阶段 3.1b 的判断，依据是源文件）：
//   源块（`ACBattleContentDefinitions.cpp:299-318`）是 `TriggerType = Immediate` +
//   `TargetSelector = Self` + **唯一一条**动作 `AddDamageModifier(MulPct 0.2, Physical,
//   DurationSeconds = 0)` —— 即"开局无条件生效的永久增伤"，**没有** `TriggerTag`、
//   没有 `Conditions`、没有 `MaxTriggers`。`Immediate` 在旧系统里由
//   `FEffectSystem::RegisterBlock` 当场执行一次（不是事件触发），
//   等价的 GAS 形态是"开局施加一个常驻 GE"，那正是 3.1a 已产出的
//   `UACGE_Trait_DeadlyStrike`（常驻 `Attack × 1.2`）本身，**不需要任何能力**。
//   因此这里的结论是：**只产 GE、不产能力**；它的施加由 3.3 的 `UACAbilitySet::GrantedEffects`
//   或开局效果块完成（与 `Equip_White_Greatsword` / `Upgrade_OP01_C1` 这类 `Immediate` 块同一路径）。
//   ⚠️ 该 GE 的两条已知语义差异（作用面从"本次物理伤害"变成"攻击力"）写在
//   `ACGE_ContentEffects.h` 的类注释里，属 3.1a 已记录的遗留项。
//
// ---------------------------------------------------------------------------
// `MaxTriggers` / `Once` 怎么落地（本阶段选"实例计数器"，理由）
// ---------------------------------------------------------------------------
//   GAS 的 `AbilityTriggers` **没有**"最多触发 N 次"这个维度（只有 Tag + 触发源）。
//   两条可选路径：
//     (a) **实例计数器**：`InstancingPolicy = InstancedPerActor` 上存 `TriggerCount`，
//         每次触发前判上限（`UACBattleAbility::ConsumeTrigger`）—— **本阶段选它**；
//     (b) `bRetriggerInstancedAbility` + `EndAbility`：那条路解决的是
//         "InstancedPerActor 的能力还在激活中又被触发"（AbilitySystemComponent_Abilities.cpp:1811-1831），
//         而本阶段的能力全是**同步**的（`ActivateAbility` 里做完就 `EndAbility`），
//         根本不存在"还激活着"的窗口 —— 用它等于什么都没做。
//   与旧语义的逐点对照（旧 `FEffectSystem`，`ACEffectSystem.cpp:334-343 / 200-214`）：
//     - `TriggerPolicy == Once` → `TriggerCount > 0` 即不再触发  ⇔ `ConsumeTrigger(1)`；
//     - `MaxTriggers > 0`     → `TriggerCount >= MaxTriggers` 即不再触发 ⇔ `ConsumeTrigger(N)`；
//     - **`TriggerCount` 只在效果真的执行之后自增**（:200-214 的 `++Instances[PostIndex].TriggerCount`
//       在 `ExecuteBlock` 之后）⇒ **条件不满足不消耗次数**。
//       这一点在 `UACPassive_Upgrade_OP01_S1` 上是有区别的（"击杀数 ≥ 20"必须先把条件判完再计数），
//       因此本文件的顺序统一是"**先判条件 → 再 ConsumeTrigger → 再施加效果**"。
#pragma once

#include "CoreMinimal.h"
#include "GAS/ACBattleAbility.h"
#include "ACPassiveAbilities.generated.h"

/**
 * 被动能力的公共基类（阶段 3.1b）：唯一的职责是**关掉"启动时必须有目标且在射程内"这道门**。
 *
 * 为什么需要它（这是被动与技能/普攻的**结构性**差别）：
 *   `UACBattleAbility::CanActivateAbility` 对技能/普攻会做"解析目标 + 射程判定"，
 *   因为那两者的目标是**内容预先给定**的（技能打主目标、普攻打射程内的目标）——
 *   解析不出目标就说明"这次不该施放"。
 *   而被动的目标是**触发时刻才决定**的：
 *     - 腐殖蛆的"死者周围 1 格"要等死亡事件才知道；
 *     - 心流 / 处刑的效果落在自己身上，与事件载荷里的目标无关；
 *     - 母巢的 `Hook.BattleStart` 上下文里根本没有任何目标（`Source` / `Target` 都是 `InvalidUnitId`）。
 *   若不关掉这道门，这些被动会因为"默认选择器 `PrimaryTarget` 解析不出目标"而**永远不触发**，
 *   而且失败发生在 `CanActivateAbility` 里，日志上只表现为"事件没触发任何能力"，极难排查。
 *
 * 另外两点继承自 `UACBattleAbility`、对被动是**正确**的：
 *   - `ActivationBlockedTags` 为空 ⇒ 沉默 / 眩晕**不挡被动**。旧实现里效果块的触发同样不看
 *     这些状态（`FEffectSystem::HandleHook` 只判实例的到期与限频），因此等价。
 *   - `InstancingPolicy = InstancedPerActor` ⇒ 触发次数计数器能跨激活存活（见下）。
 */
UCLASS(Abstract)
class AUTOCHESSBATTLE_API UACPassiveAbilityBase : public UACBattleAbility
{
    GENERATED_BODY()

public:
    UACPassiveAbilityBase();

    /** 见类注释：被动的目标在触发时刻才决定，不在激活前门控。 */
    virtual bool RequiresTargetInRange() const override { return false; }
};

/**
 * 索利瓦尔被动·嗜血：技能施放后回复自身 8 点生命，并清空自身专注。
 *
 * 来源：块 `Passive_OP01_BloodThirst`（`ACBattleContentDefinitions.cpp:239-266`）：
 *   `Hook.SkillCast` + 条件 `HasTag(Unit.Class.Warrior)` + 动作 `ApplyHeal(8)` + `DrainFocus(999)`。
 *
 * ⚠️ **触发面的两处明确化**（都被写进 3.1b 报告的对照表）：
 *   ① 旧 `FEffectSystem::HandleHook` 不按 owner 过滤 —— 任何单位放技能都会遍历到本块；
 *      新实现把"是不是自己放的技能"判在能力里（`Context.Source == 自己`）。
 *   ② 旧块的条件 `HasTag(Unit.Class.Warrior)` 判的是 **`Context.PrimaryTarget`**
 *      （`ACEffectSystem.cpp:505-507` 的 `HasTag` 分支读 `Target`），而钩子上下文里的 `Target`
 *      是**技能的主目标**（敌方单位）—— 也就是说旧代码里这个条件几乎永远为假
 *      （战士标签在施法者身上，不在被它打的目标身上）。
 *      新实现按内容作者的本意判**施法者**（`Context.Source`），即"战士放完技能后回血"。
 *      这一处是**行为修正**，不是等价复刻，已在报告里单独列出。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Solivar_BloodThirst : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Solivar_BloodThirst();

    /** 治疗量由 GE 承载（`UACGE_Passive_OP01_BloodThirst_Heal` 的 `FixedHealAmount = 8`）。 */
protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};

/**
 * 索利瓦尔 S 级强化·处刑：**累计击杀满 20 后一次性**获得永久 `CritValue + 50` / `ATK + 80`。
 *
 * 来源：块 `Upgrade_OP01_S1`（`ACBattleContentDefinitions.cpp:280-297`）：
 *   `Hook.Kill` + `MaxTriggers = 1` + 条件 `KillCount >= 20` + 两条 `AddStatModifier(BattlePermanent)`。
 * 效果本身由 `UACGE_Upgrade_OP01_S1`（3.1a）承担，本类只做"条件 + 一次性"。
 *
 * 击杀数的口径与旧实现**逐字一致**：`EACConditionType::KillCount` 读的是
 * `World->Stats().GetKillsBySource(Context.Source)`（`ACEffectSystem.cpp:548-550`），
 * 而 `Context.Source` 在旧的 hook 派发里被设成**块的所有者**（同文件 :182-185），
 * 因此语义是"**这个单位自己**的累计击杀"。新实现读 `GetKillsBySource(自己)`，同一个计数器
 * （由 `UBattleWorld::ResolveDeaths` → `StatsCollector.RecordKill` 维护，ACBattleWorld.cpp:979-982）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Upgrade_OP01_S1 : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Upgrade_OP01_S1();

    /** 触发门槛（内容值 20，`ACBattleContentDefinitions.cpp:294` 的 `KillCount.ValueA`）。 */
    static constexpr int32 RequiredKills = 20;

    /** 最多触发次数（内容值 1，块定义 `MaxTriggers = 1`）。 */
    static constexpr int32 MaxTriggers = 1;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};

/**
 * 通用 B 级词条·心流：每次击杀回复 15 点专注。
 *
 * 来源：块 `Trait_FocusSurge`（`ACBattleContentDefinitions.cpp:320-332`）：
 *   `Hook.Kill` + `GrantFocus(15)`（**没有** `MaxTriggers`，因此每次击杀都触发）。
 * 效果由 `UACGE_Trait_FocusSurge`（3.1a，Instant `Focus += 15`）承担。
 *
 * ⚠️ 触发面：旧实现同样**不按 owner 过滤**（任何敌人在任何地方死掉，持有该词条的单位都回 15 专注）。
 *   本能力也**不加**"必须是自己击杀"的过滤条件，以保持等价 ——
 *   代价是 3.3 必须把 `Hook.Kill` **广播给全场 ASC**（用
 *   `UACBattleAbility::BroadcastHookGameplayEvent`），否则触发面会收窄成"只有击杀者自己"。
 *   这条已写进报告的"钩子派发清单"与决策点。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Trait_FocusSurge : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Trait_FocusSurge();

    /** 每次击杀回复的专注量（内容值 15，`ACBattleContentDefinitions.cpp:326` 的 `GrantFocus.Value`）。 */
    static constexpr float FocusSurgeAmount = 15.f;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};

/**
 * 金·心流刃（常驻段）：**自己普攻之后**回 2 点专注。
 *
 * 来源：块 `Equip_Gold_FlowBlade_OnAttack`（`ACBattleContentDefinitions.cpp:360-369`）：
 *   `Hook.AfterAttack` + `GrantFocus(2)`。
 *
 * ⚠️ 两处关键裁决：
 *   ① **量走 Instant GE**（`UACGE_FocusGain_2`，`Focus += 2`），
 *      **不是**常驻 `FocusPerAttack += 2`。理由见 `ACGE_ContentEffects.h` 里 `UACGE_FocusGain`
 *      的类注释：后者会与"射手普攻按 `FocusPerAttack` 回专注"重复计数（2 点变 4 点）。
 *      ⇒ 3.1a 的 `UACGE_Equip_Gold_FlowBlade_OnAttack`（常驻 `FocusPerAttack += 2`）
 *      已被 **阶段 3.3 删除**：金·心流刃常驻段的唯一承载就是本能力 + `UACGE_FocusGain_2`。
 *   ② 触发面：旧实现**不按 owner 过滤**（任何单位普攻后，装备持有者都回 2 点专注 —— 明显不是
 *      内容本意）。新实现判 `Context.Source == 自己`，即"**自己**普攻后才回"。
 *      这是一处行为收紧（修正旧缺陷），已写进报告的对照表。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Equip_FlowBlade_OnAttack : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Equip_FlowBlade_OnAttack();

    /** 每次普攻回复的专注量（内容值 2，`ACBattleContentDefinitions.cpp:363` 的 `GainFocus.Value`）。 */
    static constexpr float FlowBladeFocusGain = 2.f;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};

/**
 * 敌人·腐殖蛆：**自己死亡时**对周围 1 格内的单位施加 3 层中毒（一次）。
 *
 * 来源：块 `Enemy_Worm_DeathToxin`（`ACBattleContentDefinitions.cpp:396-416`）：
 *   `Hook.Death` + `TargetSelector = Neighbors1` / `SelectorRadius = 1` + `MaxTriggers = 1`
 *   + 动作 `ApplyAbnormalState(State.Poison, Stacks = 3)`。
 *
 * ⚠️ 触发面：旧实现不按 owner 过滤（任何单位死亡都会让**每一只**蠕虫在**自己周围**放毒 ——
 *   因为"Self"取的是块的所有者），新实现判 `Context.Target == 自己`（死者是自己）。
 *   这是一处行为收紧（修正旧缺陷），已写进报告。
 *
 * ⚠️ 目标面：`Neighbors1` 选的是**周围 1 格内所有存活单位**（不分敌我，含友军），
 *   与旧实现走的是同一个 `FTargetingSystem::ResolveSelector`
 *   （`ACTargetingSystem.cpp:259-295`，`bIncludeOrigin = false`），因此"误伤友军"是**旧行为**，
 *   本阶段照旧（改它属于数值/规则变更，得单独裁决）。
 *   唯一不复制的是旧 `ExecuteBlock` 的"无目标时回落到 `Context.PrimaryTarget`"那条兜底
 *   （`ACEffectSystem.cpp:397-400`）：那里回落到的正是**死掉的自己**，给尸体挂中毒没有任何
 *   可观察效果（周期结算只遍历存活单位），因此不复制。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Enemy_Worm_DeathToxin : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Enemy_Worm_DeathToxin();

    /** 中毒层数（内容值 3，`ACBattleContentDefinitions.cpp:405` 的 `Poison.Stacks`）。 */
    static constexpr int32 PoisonStacks = 3;

    /** 最多触发次数（内容值 1，块定义 `MaxTriggers = 1`，`ACBattleContentDefinitions.cpp:413`）。 */
    static constexpr int32 MaxTriggers = 1;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;

    /** `Neighbors1`（含半径 1），与内容块逐字一致。 */
    virtual EACSelectorType GetTargetSelector(const AACBattleUnitBase& Caster) const override;
    virtual int32 GetSelectorRadius(const AACBattleUnitBase& Caster) const override;
};

/**
 * 敌人·蠕虫母巢：战斗开始时召唤 4 只 `SUM_Wormling`（一次）。
 *
 * 来源：块 `Enemy_MotherNest_Spawn`（`ACBattleContentDefinitions.cpp:418-438`）：
 *   `Hook.BattleStart` + `MaxTriggers = 1` + 4 个 `SummonUnit(SubId = "SUM_Wormling")`。
 * 效果由 `UACGE_Enemy_MotherNest_Spawn`（3.1a：Instant + `UACSummonEffectComponent`
 * 的 `SummonSpecId = SUM_Wormling` / `SummonCount = 4`）承担。
 *
 * ⚠️ 触发面：旧内核在 `UBattleWorld::Initialize` 末尾派发 `Hook_BattleStart`
 *   （`ACBattleWorld.cpp:261-263`，`Source = InvalidUnitId`）—— **一次派发给所有订阅者**。
 *   新实现要唤醒母巢的 ASC，就需要 3.3 把该钩子**广播给全场 ASC**
 *   （`BroadcastHookGameplayEvent`）；若只发给"钩子上下文的 Source"，母巢永远收不到
 *   （`Hook.BattleStart` 的 Source 是 `InvalidUnitId`）。已写进报告。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Enemy_MotherNest_Spawn : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Enemy_MotherNest_Spawn();

    /** 最多触发次数（内容值 1，块定义 `MaxTriggers = 1`）。 */
    static constexpr int32 MaxTriggers = 1;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};

/**
 * 敌人·巨蛭：**自己命中后**给主目标挂 2 层、5 秒的流血。
 *
 * 来源：块 `Enemy_Leech_Bite`（`ACBattleContentDefinitions.cpp:440-457`）：
 *   `Hook.Hit` + `TargetSelector = PrimaryTarget`
 *   + 动作 `ApplyAbnormalState(State.Bleed, Stacks = 2, DurationSeconds = 5)`。
 * 效果由 `UACGE_State_Bleed_Leech`（3.1a：同状态标签、时长 5 秒的独立 GE 类）承担 ——
 * 注意默认的 `UACGE_State_Bleed` 时长是无限的，两者不能混用。
 *
 * ⚠️ 触发面：旧实现不按 owner 过滤（任何单位命中都会让巨蛭给"钩子上下文里的主目标"挂流血），
 *   新实现判 `Context.Source == 自己`（自己是攻击者）。行为收紧，已写进报告。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Enemy_Leech_Bite : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Enemy_Leech_Bite();

    /** 流血层数（内容值 2，`ACBattleContentDefinitions.cpp:447` 的 `Bleed.Stacks`）。 */
    static constexpr int32 BleedStacks = 2;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};
