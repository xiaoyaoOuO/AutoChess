#pragma once

#include "CoreMinimal.h"
#include "GAS/ACBattleAbility.h"
#include "GameplayEffect.h"
#include "Core/ACDataTypes.h"
#include "ACBasicAttackAbility.generated.h"

class UGameplayEffect;

/**
 * 普攻能力：一个单位"行动条到点且目标在射程内"时执行的那一次攻击。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACBasicAttackAbility : public UACBattleAbility
{
    GENERATED_BODY()

public:
    UACBasicAttackAbility();

    /**
     * 段级附加效果（新承载：GE 类数组）。
     *
     * - 旧的 `FACAttackSegment::OnHitEffectBlockIds` 是 `TArray<FName>`（效果块 ID），
     *   而 GE 是**类**，两者不是同一层次的标识 —— 这也正是 §4.2 "效果块 → GE" 的全部含义。
     * - **阶段 3.3**：契约侧的 `FACAttackSegment::OnHitEffects` 已经是同类型的 `TSubclassOf<UGameplayEffect>` 数组，
     *   内容侧可以直接填类；当前内容里它为空（旧实现也从来是空的），
     *   因此 `ApplySegmentOnHitEffects` 是空操作，行为与旧实现
     *   "`OnHitEffectBlockIds.Num() == 0` 时什么都不做"（`ACAbilityExecutor.cpp:309`）等价。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|BasicAttack")
    TArray<TSubclassOf<UGameplayEffect>> OnHitEffects;

    // ---- §4.1：普攻的"目标 / 射程"来自攻击模式，见 .cpp ----

    virtual EACSelectorType GetTargetSelector(const AACBattleUnitBase& Caster) const override;
    virtual float GetEffectiveRangeOverride(const AACBattleUnitBase& Caster) const override;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;

private:
    /**
     * 取本次攻击用的段列表：`Pattern.Segments` 为空时用"100% 攻击力物理伤害"的兜底段
     *（旧 `ExecuteBasicAttack` 的 `static TArray<FACAttackSegment> DefaultSegments`，:273-278）。
     */
    static const TArray<FACAttackSegment>& GetSegments(const FACAttackPatternDef& Pattern);

    /**
     * 段级附加效果：把 `OnHitEffects` 逐个施加到**目标**身上。
     * 旧的对应实现是 `World->Effects().ExecuteBlocks(Segment.OnHitEffectBlockIds, EffectContext)`
     *（`ACAbilityExecutor.cpp:311-314`），其中 `EffectContext.Source = 攻击者`、
     * `PrimaryTarget = 目标` —— 本函数用同样的来源/目标组 spec。
     */
    void ApplySegmentOnHitEffects(AACBattleUnitBase& Caster, AACBattleUnitBase& Target);

    /**
     * 普攻命中后的专注回复（旧 `FAbilityExecutor::OnBasicAttackHit`，`ACAbilityExecutor.cpp:637-651`）。
     *
     * A11：射手普攻回专注。**读属性集**（阶段 2 起专注的权威在 `UACBattleAttributeSet`）：
     *   ① 先读 `EACStat::FocusPerAttack`（= 属性集 `FocusPerAttack` 的当前值，
     *      `AACBattleUnitBase::GetStat` 就是读它）；
     *   ② 为 0 且**远程**时回退到规则配置 `UBattleRuleConfig::ShooterFocusPerAttack`
     *      （旧实现回退 5.f，配置缺失时这里也回退 5.f，三级口径一致）。
     * 旧实现读的是 `FFocusState::PerAttackGain`（成员变量），那个字段已随专注迁属性集而废；
     * "为 0 才回退"这条判据本身逐字保留。
     */
    static void OnBasicAttackHit(AACBattleUnitBase& Caster);
};
