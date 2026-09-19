// 阶段 3.1a 新增（GAS 重构实施方案 §4.4 / §7 阶段 3.1）：7 种异常状态 GE 的实现。
//
// 每个类的数值都在下面用两行注明来源（内容定义 + CSV 条目），逐项可核对（C6/D10）。
#include "GAS/Effects/ACGE_States.h"

#include "Core/ACBattleTags.h"
#include "GAS/ACBattleAttributeSet.h"
#include "GAS/Effects/ACPeriodicDamageExecution.h"

// ---------------------------------------------------------------------------
// 效果分类标签（原生标签注册，幂等；写法与 Core/ACBattleTags.cpp:6 一致）
// ---------------------------------------------------------------------------
namespace BattleEffectTags
{
    UE_DEFINE_GAMEPLAY_TAG(Effect_Debuff,    "Effect.Debuff");
    UE_DEFINE_GAMEPLAY_TAG(Effect_Removable, "Effect.Removable");
    UE_DEFINE_GAMEPLAY_TAG(Effect_Buff,      "Effect.Buff");
}

// ---------------------------------------------------------------------------
// UACGE_StateBase
// ---------------------------------------------------------------------------

UACGE_StateBase::UACGE_StateBase()
{
    // 周期结算通道（7 种状态共用同一个 Execution 类）。
    // 参数不在 Execution 上，而在 GE 类自己的字段上（`ConfigurePeriodicDamage` 写的五项），
    // 由执行体从 `Spec.Def` 读 —— 因此没有周期伤害的状态（伤口/腐蚀/冰冻/静电紊乱）
    // 也会挂这条 Execution，它读到 `bEnablePeriodicDamage = false` 后只做"扣层"那一步。
    AddExecution(UACPeriodicDamageExecution::StaticClass());
}

void UACGE_StateBase::ConfigureState(const FGameplayTag& StateTag, int32 MaxStacks,
                                     EGameplayEffectStackingDurationPolicy StackDurationPolicy,
                                     EGameplayEffectStackingPeriodPolicy StackPeriodPolicy,
                                     float DurationSeconds, float TickInterval)
{
    // 1) 授予标签（§4.4：`StateTag` → `GrantedTags`）。
    //    授予之后 `ASC->HasMatchingGameplayTag(State.Xxx)` 即"是否处于该状态"，
    //    层数走 `ASC->GetTagCount(...)` / 活动 GE 的 `GetStackCount()`。
    AddGrantedTag(StateTag);

    // 2) 周期（旧 `TickInterval` → GE 的 `Period`）。
    SetPeriodSeconds(TickInterval);

    // 3) 时长（旧 `DurationSeconds`；`0` = 无时限 → `Infinite`）。
    if (DurationSeconds > 0.f)
    {
        MakeHasDuration(DurationSeconds);
    }
    else
    {
        MakeInfinite();
    }

    // 4) 叠加策略（§4.4 映射表）—— 阶段 4：自研的 `EACStackPolicy` 已删除，
    //    这里直接收引擎的 `StackDurationRefreshPolicy` + `StackPeriodResetPolicy`。
    //    `StackLimitCount <= 0` = 无上限（引擎在 `StackLimitCount <= 0` 时不做裁剪，
    //    GameplayEffect.cpp:4054），与旧 `MaxStacks = 0` 逐字对应。
    //    引擎侧的一个事实（旧 `RefreshOnly` 的写法依据，保留在注释里备查）：
    //    `StackLimitCount = 1` 时重复施加不会涨层，只会按 `RefreshOnSuccessfulApplication` 刷新时长。
    ConfigureStacking(MaxStacks, StackDurationPolicy, StackPeriodPolicy);
}

void UACGE_StateBase::ConfigurePerStackModifier(const FGameplayAttribute& Attribute, float PerStackValue)
{
    // 引擎会把幅度乘上层数：`FGameplayEffectSpec::GetModifierMagnitude(Idx, bFactorInStackCount)`
    // → `GameplayEffectUtilities::ComputeStackedModifierMagnitude`（GameplayEffectTypes.cpp:113-131）
    // 对非 Override 运算是 `(Mag - Bias) * Stack + Bias`，`Additive` 的 Bias = 0，
    // 因此 `Additive(-10)` 在 3 层时正好是 -30。
    AddAdditiveModifier(Attribute, PerStackValue);
}

void UACGE_StateBase::ConfigureClassificationTags(bool bIsDebuff, bool bRemovable)
{
    // §4.4：`bIsDebuff` → `AssetTags` 打标 `Effect.Debuff`；`bRemovable` → `Effect.Removable`。
    // 打的是 **AssetTags**（GE 自己有、不传给目标），因此不会给单位加标签、
    // 驱散能力按 `ASC->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyEffectTags(...))` 查即可。
    if (bIsDebuff)
    {
        AddAssetTag(BattleEffectTags::Effect_Debuff);
    }
    if (bRemovable)
    {
        AddAssetTag(BattleEffectTags::Effect_Removable);
    }
}

// ---------------------------------------------------------------------------
// 7 种状态（数值来源逐行标注）
// ---------------------------------------------------------------------------

UACGE_State_Bleed::UACGE_State_Bleed()
{
    ConfigureState(BattleTags::State_Bleed, /*MaxStacks=*/20,
                   EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication,
                   EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication,
                   /*DurationSeconds=*/0.f, /*TickInterval=*/1.f);
    ConfigurePeriodicDamage(/*PerStack=*/0.5f, /*bPercentOfMaxHP=*/true, EACDamageType::Technical,
                            /*bConsumeStackOnTick=*/true);
    ConfigureClassificationTags(/*bIsDebuff=*/true, /*bRemovable=*/true);
}

UACGE_State_Wound::UACGE_State_Wound()
{
    ConfigureState(BattleTags::State_Wound, /*MaxStacks=*/0,
                   EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication,
                   EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication,
                   /*DurationSeconds=*/0.f, /*TickInterval=*/1.f);
    // 无周期伤害（旧 `bPeriodicDamage = false` / `DamagePerStack = 0`），但**要扣层**
    // （旧 `bConsumeStackOnTick = true`，口径见 ACBattleContentDefinitions.cpp:886-891 的注释）。
    ConfigurePeriodicDamage(/*PerStack=*/0.f, /*bPercentOfMaxHP=*/false, EACDamageType::Physical,
                            /*bConsumeStackOnTick=*/true);
    ConfigureClassificationTags(/*bIsDebuff=*/true, /*bRemovable=*/true);
}

UACGE_State_Poison::UACGE_State_Poison()
{
    ConfigureState(BattleTags::State_Poison, /*MaxStacks=*/0,
                   EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication,
                   EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication,
                   /*DurationSeconds=*/0.f, /*TickInterval=*/1.f);
    ConfigurePeriodicDamage(/*PerStack=*/1.f, /*bPercentOfMaxHP=*/false, EACDamageType::Technical,
                            /*bConsumeStackOnTick=*/true);
    ConfigureClassificationTags(/*bIsDebuff=*/true, /*bRemovable=*/true);
}

UACGE_State_Burn::UACGE_State_Burn()
{
    ConfigureState(BattleTags::State_Burn, /*MaxStacks=*/0,
                   EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication,
                   EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication,
                   /*DurationSeconds=*/5.f, /*TickInterval=*/1.f);
    ConfigurePeriodicDamage(/*PerStack=*/1.f, /*bPercentOfMaxHP=*/false, EACDamageType::Technical,
                            /*bConsumeStackOnTick=*/false);
    ConfigureClassificationTags(/*bIsDebuff=*/true, /*bRemovable=*/true);
}

UACGE_State_Corrosion::UACGE_State_Corrosion()
{
    ConfigureState(BattleTags::State_Corrosion, /*MaxStacks=*/0,
                   EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication,
                   EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication,
                   /*DurationSeconds=*/5.f, /*TickInterval=*/1.f);
    // 无周期伤害、不消耗层数。
    ConfigurePeriodicDamage(/*PerStack=*/0.f, /*bPercentOfMaxHP=*/false, EACDamageType::Technical,
                            /*bConsumeStackOnTick=*/false);
    ConfigureClassificationTags(/*bIsDebuff=*/true, /*bRemovable=*/true);

    // ⚠️ CSV ST_05 说"抗性减少 1×层数"，但旧内容定义**没有**这条修饰
    //    （ACBattleContentDefinitions.cpp:901 明写"抗性减少由效果块（AddStatModifier + RES）表达"）。
    //    加了它就等于凭空引入旧版本没有的数值（违反 C6/D10），因此这里**刻意注释掉**：
    //        ConfigurePerStackModifier(UACBattleAttributeSet::GetResistanceAttribute(), -1.f);
    //    同理：效果块侧的 `AddStatModifier + RES` 在阶段 2 起已经是 no-op
    //    （ACEffectSystem.cpp:656-678 的 Warning 分支），所以"腐蚀当前无实际减抗"是**既有**缺陷，
    //    不是本次迁移引入的。已写入报告的遗留问题。
}

UACGE_State_Freeze::UACGE_State_Freeze()
{
    ConfigureState(BattleTags::State_Freeze, /*MaxStacks=*/3,
                   EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication,
                   EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication,
                   /*DurationSeconds=*/5.f, /*TickInterval=*/1.f);
    // 无周期伤害、不消耗层数。
    ConfigurePeriodicDamage(/*PerStack=*/0.f, /*bPercentOfMaxHP=*/false, EACDamageType::Technical,
                            /*bConsumeStackOnTick=*/false);
    ConfigureClassificationTags(/*bIsDebuff=*/true, /*bRemovable=*/true);

    // CSV ST_06 + 内容定义注释："每层减少敌方 10 点攻速，最多三层" → 每层 -10 ASPD。
    ConfigurePerStackModifier(UACBattleAttributeSet::GetAttackSpeedAttribute(), -10.f);
}

UACGE_State_StaticDisorder::UACGE_State_StaticDisorder()
{
    ConfigureState(BattleTags::State_StaticDisorder, /*MaxStacks=*/0,
                   EGameplayEffectStackingDurationPolicy::NeverRefresh,
                   EGameplayEffectStackingPeriodPolicy::NeverReset,
                   /*DurationSeconds=*/10.f, /*TickInterval=*/1.f);
    ConfigurePeriodicDamage(/*PerStack=*/0.f, /*bPercentOfMaxHP=*/false, EACDamageType::Technical,
                            /*bConsumeStackOnTick=*/false);
    ConfigureClassificationTags(/*bIsDebuff=*/true, /*bRemovable=*/true);
}

UACGE_State_Bleed_Leech::UACGE_State_Bleed_Leech()
{
    // 与 UACGE_State_Bleed 唯一的差别：时长 5 秒（来源：Enemy_Leech_Bite 块，见头文件说明）。
    ConfigureState(BattleTags::State_Bleed, /*MaxStacks=*/20,
                   EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication,
                   EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication,
                   /*DurationSeconds=*/5.f, /*TickInterval=*/1.f);
    ConfigurePeriodicDamage(/*PerStack=*/0.5f, /*bPercentOfMaxHP=*/true, EACDamageType::Technical,
                            /*bConsumeStackOnTick=*/true);
    ConfigureClassificationTags(/*bIsDebuff=*/true, /*bRemovable=*/true);
}

// ---------------------------------------------------------------------------
// 眩晕（阶段 3.2b 新增：旧内核 API `FAbilityExecutor::ApplyStun` 的 GE 承接）
// ---------------------------------------------------------------------------

FName UACGE_State_Stun::GetDurationDataName()
{
    // 与基类 `MakeHasDurationByCaller` 的默认键名一致（那里写的是同一个字符串字面量）。
    // 集中在这里是为了让施加方（`FMentalSystem::HandleBreak`）不写字符串。
    return FName(TEXT("Data.DurationSeconds"));
}

UACGE_State_Stun::UACGE_State_Stun()
{
    // 1) 时长：**由施加方给**（SetByCaller），因为"眩晕几秒"是每次施加各自的瞬时数据
    //（旧 `ApplyStun(Target, Seconds, Source)` 的第二个形参）。键名见上面的访问器。
    MakeHasDurationByCaller(GetDurationDataName());

    // 2) 授予 `State.Stun` 标签 —— 这就是"被眩晕"的唯一真相。
    //    门控它的地方一共三处，全都查这个标签（已核实）：
    //      · 调度器门控 `FActionScheduler::EvaluateGate`（行动条不推进）；
    //      · AI `FBattleAISystem::Decide`（返回 Skip）；
    //      · 能力门控 `ActivationBlockedTags`（普攻与技能能力都含 `State.Stun`）。
    AddGrantedTag(BattleTags::State_Stun);

    // 3) 叠加策略：**层数上限 1 + 重复施加刷新时长**。
    //    A18 的旧语义是"叠加眩晕取最晚到期时刻"（`ACAbilityExecutor.cpp:614-615` 的
    //    `Max(Existing.EndTime, Now + Seconds)`）。对本处恒定的 2 秒时长来说，
    //    "从当前时刻重新计时"与那个 `Max` **完全等价**（`Now + 2` 永不早于旧到期时刻），
    //    因此 `RefreshOnSuccessfulApplication` 就是它的逐项等价物。
    //    `StackLimitCount = 1`：不允许"2 秒眩晕叠成 4 秒"—— 旧实现也只会刷新那一条记录。
    ConfigureStacking(/*InStackLimitCount=*/1,
                      EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication,
                      EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication);

    // 4) 分类标签：眩晕是负面且可被驱散的（与 7 个状态 GE 同一口径，
    //    即 `UACGE_StateBase::ConfigureClassificationTags(true, true)` 的两条）。
    //    这里不继承 `UACGE_StateBase`（理由见头文件），因此直接把那两行写出来。
    AddAssetTag(BattleEffectTags::Effect_Debuff);
    AddAssetTag(BattleEffectTags::Effect_Removable);

    // 5) **不设 `Period`**：眩晕没有周期结算。
    //    顺带说明为什么不需要"防止执行体空跑"：引擎只在「Instant」或「有 Period」时才跑
    //    `Executions`（§4.6 裁决 ④，`GameplayEffect.cpp:2936-2943`），
    //    本 GE 既不是 Instant 也没有 Period，因此它一条 Execution 都不会跑 ——
    //    这正是 `UACGE_StateBase` 那套"每秒跑一次执行体"的机制在这里不需要的原因。
}
