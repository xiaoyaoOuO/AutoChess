// 阶段 3.1b 新增（GAS 重构实施方案 §4.1 / §7 阶段 3.1）：技能能力基类实现。
// 逐项对照表见头文件；下面每个步骤的注释都标了旧 `ExecuteSkill` 的行号。
#include "GAS/Abilities/ACSkillAbilityBase.h"

#include "Battle/ACBattleTime.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "Core/ACBattleTags.h"

UACSkillAbilityBase::UACSkillAbilityBase()
{
    // 沉默 / 眩晕门控（§4.1 的映射表：`CanCastSkill` 的沉默眩晕门控 → `ActivationBlockedTags`）。
    //
    // 旧实现（`ACAbilityExecutor.cpp:81-104`）：`IsSilenced(Unit) || Unit.IsStunned()` → 不放技能。
    // 其中 `IsSilenced` 读的是 `State.Silence` 状态、`IsStunned` 读的是 `State.Stun` 状态
    //（`Unit.GetStates().Has(BattleTags::State_Stun)`），因此两个标签一一对应。
    //
    // ⚠️ 与普攻的差别是**刻意的**：普攻只挡眩晕（旧的 `CanAct` 不判沉默），
    // 把沉默也加到普攻上会创造一条旧版本没有的规则。
    ActivationBlockedTags.AddTag(BattleTags::State_Silence);
    ActivationBlockedTags.AddTag(BattleTags::State_Stun);

    // 引导型技能要在这里改 `InstancingPolicy`（§4.1）—— 本阶段没有引导内容，见头文件。
    // 前摇（`WindUpSeconds > 0`）同理：本阶段不引入 `UAbilityTask_WaitDelay`。
}

bool UACSkillAbilityBase::IsReadyToActivate(const AACBattleUnitBase& Caster) const
{
    // 旧判据逐项搬来（`ACAbilityExecutor.cpp:99-107`，`FAbilityExecutor::CanCastSkill`）：
    //   `Focus.Max <= 0`          → 无专注条（奎尔 / 克里斯蒂娜 / 格斯 D 级）→ 不放技能；
    //   `Focus.Current < Focus.Max` → 没满 → 不放（A15：满则释放，不支持主动保留）。
    //
    // 两条判据收在 `AACBattleUnitBase::IsFocusFull()` 一处：那是 A15 在全仓的**唯一定义**，
    // 内核与能力都读它，不各自再写一遍 `Max > 0 && Current >= Max`（那种重复迟早会分叉）。
    // `Focus` / `FocusMax` 现在是**属性集**的两个属性（§4.1 把 `FFocusState` 迁到了属性集，
    // 阶段 3.2b 正式删掉那个结构体）；该访问器只读形参单位、不碰任何实例状态，
    // 因此本函数在内核按 **CDO** 调用时同样成立。
    return Caster.IsFocusFull();
}

void UACSkillAbilityBase::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                          const FGameplayAbilityActivationInfo ActivationInfo,
                                          const FGameplayEventData* TriggerEventData)
{
    UBattleWorld* const World = GetBattleWorld();
    AACBattleUnitBase* const Caster = GetCasterUnit();
    if (World == nullptr || Caster == nullptr)
    {
        // 没有世界 / 施法者不是战斗单位：不该发生（技能只授予单位），但也不崩。
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }

    // 目标解析（§4.1：能力内调 `FTargetingSystem`，不用 TargetActor；§0 C9）。
    // `CanActivateAbility` 已经解析过并判过射程，这里再解析一次拿**当前**值。
    TArray<FUnitId> Targets;
    if (!ResolveTargets(*Caster, Targets))
    {
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }
    PrimaryTargetId = (Targets.Num() > 0) ? Targets[0] : InvalidUnitId;

    // 旧 :353-356：钩子上下文（Source=施法者、Target=主目标、Time=战斗内相对时间）。
    FBattleHookContext SkillHookContext;
    SkillHookContext.Source = Caster->GetUnitId();
    SkillHookContext.Target = PrimaryTargetId;
    SkillHookContext.Time = FACBattleTime::ElapsedSeconds(*World);

    // 旧 :359-364：**敌方技能可打断**。
    // 判定顺序不能反：先过可打断的门，再过 BeforeSkillCast 的可取消门 ——
    // 旧实现就是这个顺序，反过来会让"本该被打断的技能先广播了 BeforeSkillCast"。
    if (IsEnemySkillCastInterruptible(*Caster)
        && DispatchHook(BattleTags::Hook_EnemySkillCast, SkillHookContext, Caster).bInterrupt)
    {
        Caster->SetActionState(EACUnitActionState::Idle);
        // 旧 :362 只 Dispatch（没有玩法被动订阅 `Hook.SkillInterrupted`），
        // 因此这里传 nullptr = 只走 EventBus，不新增触发面。
        DispatchHook(BattleTags::Hook_SkillInterrupted, SkillHookContext, nullptr);
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }

    // 旧 :365-369：`Hook.BeforeSkillCast` 的可取消语义（订阅者可用 `bCancel` 或
    // `bCancelled` 否决整次施放）。
    if (DispatchHook(BattleTags::Hook_BeforeSkillCast, SkillHookContext, Caster).bCancel
        || SkillHookContext.bCancelled)
    {
        Caster->SetActionState(EACUnitActionState::Idle);
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }

    // 旧 :371-373：进入施放状态（Instant → Casting；引导 → Channeling，本阶段无引导内容）。
    Caster->SetActionState(EACUnitActionState::Casting);

    // 旧 :376-387：专注消耗。**走 Cost GE**（`CostGameplayEffectClass`），
    // 三种模式（ClearAll / Fixed / DrainPerSecond）由内容选不同的 Cost 类表达，
    // 能力侧不再为模式写分支（§4.1 的映射表就是这么定的）。
    // `CommitBattleCost` 内部做两件事：`CommitAbility`（消耗 + 冷却）+ 把专注夹回合法区间，
    // 后者是引擎不会替我们做的（原因见基类实现里的长注释）。
    if (!CommitBattleCost())
    {
        Caster->SetActionState(EACUnitActionState::Idle);
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }

    // 旧 :389-391：施放钩子。**静电紊乱的触发点之一**（`Hook.SkillCast`）。
    // `Hook.AllySkillCast` 的订阅者按 Source 阵营过滤（实现 OnAllySkillCast）。
    DispatchHook(BattleTags::Hook_SkillCast, SkillHookContext, Caster);
    DispatchHook(BattleTags::Hook_AllySkillCast, SkillHookContext, Caster);

    // 旧 :397-403：埋点。位置与旧实现完全一致 ——
    // 在"取消 / 打断判定之后、效果执行之前"，
    // 因此被取消的技能不产生这条记录，而"谁对谁放了哪个技能"这句话的判定点不变。
    LogAbilityEvent(SkillId, Caster->GetUnitId(), PrimaryTargetId);

    // 旧 :405：效果（旧为效果块，新为 GE，由子类实现）。
    ApplySkillEffects(Targets);

    // 旧 :422：Instant 技能施放完即回到 Idle。
    // ⚠️ 焦点消耗已经由 `CommitBattleCost` 落库，这里**不要**再动专注：
    //   旧实现是"消耗在设状态之后、钩子之前"，顺序一致。
    Caster->SetActionState(EACUnitActionState::Idle);
    FinishAbility(/*bWasCancelled=*/false);
}

void UACSkillAbilityBase::ApplySkillEffects(const TArray<FUnitId>& Targets)
{
    // 基类不做任何事，但要留痕：`SkillId` 非空却在施放时什么都没发生，通常是子类忘了重写。
    // 用 Warning 而不是静默：这是内容/代码配置错误，不是运行期正常分支。
    UE_LOG(LogTemp, Warning,
           TEXT("[Battle][GAS] UACSkillAbilityBase::ApplySkillEffects 未被重写（SkillId=%s，目标数=%d）—— 技能不产生任何效果。"),
           *SkillId.ToString(), Targets.Num());
}
