#include "GA_Executioner.h"

void UGA_Executioner_C::OnGiveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
{
	//获得C级强化的时候，减少focus上限。并且添加事件监听，累计击杀多少人->获得对应的强化
	Super::OnGiveAbility(ActorInfo, Spec);

	for (auto Effect : OnGiveAbilityEffects)
	{
		if (Effect)
		{
			ApplyEffectToUnit(*GetCasterUnit(), *GetCasterUnit(), Effect, 1);
		}
	}

	//添加事件监听
}

void UGA_Executioner_C::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                        const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo,
                                        const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	
}

void UGA_Executioner_S::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
}
