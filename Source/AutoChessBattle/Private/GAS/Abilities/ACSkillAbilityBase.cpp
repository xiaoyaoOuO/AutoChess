// 阶段 3.1b 新增（GAS 重构实施方案 §4.1 / §7 阶段 3.1）：技能能力基类实现。
// 逐项对照表见头文件；下面每个步骤的注释都标了旧 `ExecuteSkill` 的行号。
#include "GAS/Abilities/ACSkillAbilityBase.h"

#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "Battle/ACBattleTime.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "Core/ACBattleTags.h"

UACSkillAbilityBase::UACSkillAbilityBase()
{
    ActivationBlockedTags.AddTag(BattleTags::State_Silence);
    ActivationBlockedTags.AddTag(BattleTags::State_Stun);
}

bool UACSkillAbilityBase::IsReadyToActivate(const AACBattleUnitBase& Caster) const
{
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
    
    TArray<FUnitId> Targets;
    if (!ResolveTargets(*Caster, Targets))
    {
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }
    PrimaryTargetId = (Targets.Num() > 0) ? Targets[0] : InvalidUnitId;
    
    FBattleHookContext SkillHookContext;
    SkillHookContext.Source = Caster->GetUnitId();
    SkillHookContext.Target = PrimaryTargetId;
    SkillHookContext.Time = FACBattleTime::ElapsedSeconds(*World);
    
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
    
    if (DispatchHook(BattleTags::Hook_BeforeSkillCast, SkillHookContext, Caster).bCancel
        || SkillHookContext.bCancelled)
    {
        Caster->SetActionState(EACUnitActionState::Idle);
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }
    
    Caster->SetActionState(EACUnitActionState::Casting);
    
    if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
    {
        Caster->SetActionState(EACUnitActionState::Idle);
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }
    
    DispatchHook(BattleTags::Hook_SkillCast, SkillHookContext, Caster);
    DispatchHook(BattleTags::Hook_AllySkillCast, SkillHookContext, Caster);
    
    LogAbilityEvent(SkillId, Caster->GetUnitId(), PrimaryTargetId);

    // 旧 :405：效果（旧为效果块，新为 GE，由子类实现）。
    ApplySkillEffects(Targets);
}

void UACSkillAbilityBase::ApplySkillEffects(const TArray<FUnitId>& Targets)
{
}
