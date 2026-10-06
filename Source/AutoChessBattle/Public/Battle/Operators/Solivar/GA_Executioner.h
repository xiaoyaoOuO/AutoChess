#pragma once
#include "GAS/Abilities/ACSkillAbilityBase.h"
#include "GA_Executioner.generated.h"

UCLASS()
/**
 * 索利瓦尔 C 级强化·处刑
 */
class UGA_Executioner_C:public UACSkillAbilityBase
{
	GENERATED_BODY()

	/**索利瓦尔专注值上限减少20，使用巨斧裂体击杀敌方时直接补满全部专注值并为自己回复20%生命值，
	 *累计击败20个敌人永久获得50暴击和80攻击力，
	 *累计击败60人永久获得30%吸血，累计击败100敌人则巨斧裂体能够暴击，累计击败200敌人巨斧裂体将转为真实伤害（无视防御）*/
public:
	virtual void OnGiveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec) override;
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

private:
	//当技能被授予时，给施法者添加的效果
	UPROPERTY(EditAnywhere)
	TArray<TSubclassOf<UGameplayEffect>> OnGiveAbilityEffects;
};

UCLASS()
/**
 * 索利瓦尔 S 级强化,资深处刑
 */
class UGA_Executioner_S : public UACSkillAbilityBase
{
	GENERATED_BODY()
/**
 * 资深处刑者——战斗开始时专注条将直接充满，巨斧裂体获得强化，将直接斩杀生命值低于5%的敌人，且巨斧裂体将对选中目标所在的格子四周相邻格子造成60%伤害。
 * 选中目标如未能斩杀则降低敌方10%最大生命值上限（不可叠加）且初次造成该效果时造成额外造成敌方10%最大生命值的物理伤害。
 */
public:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
