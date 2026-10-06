#pragma once

#include "CoreMinimal.h"
#include "GAS/ACBattleAbility.h"
#include "ACSkillAbilityBase.generated.h"

/**
 * Instant 技能能力的公共基类。
 *
 * 子类只需要做三件事：
 *   ① 构造函数里填 `SkillId`（埋点用）与 `CostGameplayEffectClass`（§4.1 的 CostMode → Cost GE）；
 *   ② 需要时覆写 `GetTargetSelector` / `GetEffectiveRangeOverride`（默认 = 内容里最常见的
 *      `PrimaryTarget` + 施法者射程）；
 *   ③ 实现 `ApplySkillEffects()`。
 */
UCLASS(Abstract)
class AUTOCHESSBATTLE_API UACSkillAbilityBase : public UACBattleAbility
{
    GENERATED_BODY()

public:
    UACSkillAbilityBase();

    /**
     * 技能 ID。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Skill")
    FName SkillId;
    
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Skill")
    FName PresentationCueId;
    
    virtual bool IsReadyToActivate(const AACBattleUnitBase& Caster) const override;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;

    /**
     * 子类实现：把技能效果施加到解析出的目标上。
     *
     * @param Targets 已解析的目标（`Targets[0]` 是主目标；可能为空 = 无目标技能）。
     *                默认实现什么都不做，并打一条 Warning —— "技能没有效果"通常是子类忘了实现，
     *                静默通过会让"技能放了但没伤害"变成一场没有线索的排查。
     */
    virtual void ApplySkillEffects(const TArray<FUnitId>& Targets);

    /** 本次施放的主目标（`Targets[0]`；无目标时为 `InvalidUnitId`）。 */
    FUnitId GetPrimaryTargetId() const { return PrimaryTargetId; }

private:
    /** 本次激活解析出的主目标，供 `ApplySkillEffects` 的实现读取（每次激活开头重设）。 */
    FUnitId PrimaryTargetId = InvalidUnitId;

    //技能动作动画蒙太奇
    UPROPERTY(EditAnywhere)
    TObjectPtr<UAnimMontage> SkillMontage;

    UPROPERTY(EditAnywhere)
    TArray<TSubclassOf<UGameplayEffect>> SkillEffects;
};
