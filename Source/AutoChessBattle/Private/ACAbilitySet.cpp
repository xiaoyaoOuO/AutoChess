
#include "GAS/ACAbilitySet.h"

#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"

void UACAbilitySet::GiveAbilitiesTo(UAbilitySystemComponent& ASC,
                                    const TArray<TSubclassOf<UGameplayAbility>>& Abilities,
                                    const TArray<int32>& Levels,
                                    UObject* SourceObject)
{
    for (int32 Index = 0; Index < Abilities.Num(); ++Index)
    {
        const TSubclassOf<UGameplayAbility>& AbilityClass = Abilities[Index];
        if (AbilityClass.Get() == nullptr)
        {
            continue;
        }

        // 等级口径：`Levels` 与 `Abilities` 按下标对应；
        // 数组短了（内容只填等级不给全）或填了非正值时一律按 1 处理，
        const int32 Level = Levels.IsValidIndex(Index) && Levels[Index] > 0
            ? Levels[Index]
            : 1;

        ASC.GiveAbility(FGameplayAbilitySpec(AbilityClass, Level, INDEX_NONE, SourceObject));
    }
}

void UACAbilitySet::GiveTo(UAbilitySystemComponent& ASC) const
{
    // -----------------------------------------------------------------------
    // ① 能力：`FGameplayAbilitySpec(AbilityClass, Level, InputID, SourceObject)`
    // -----------------------------------------------------------------------
    GiveAbilitiesTo(ASC, GrantedAbilities, GrantedAbilityLevels, const_cast<UACAbilitySet*>(this));

    // -----------------------------------------------------------------------
    // ② 常驻效果：`MakeOutgoingSpec` + `ApplyGameplayEffectSpecToSelf`
    // -----------------------------------------------------------------------
    const FGameplayEffectContextHandle Context = ASC.MakeEffectContext();
    for (const TSubclassOf<UGameplayEffect>& EffectClass : GrantedEffects)
    {
        if (EffectClass.Get() == nullptr)
        {
            continue;
        }
        
        const FGameplayEffectSpecHandle SpecHandle = ASC.MakeOutgoingSpec(EffectClass, /*Level=*/1.f, Context);
        if (SpecHandle.IsValid() && SpecHandle.Data.IsValid())
        {
            ASC.ApplyGameplayEffectSpecToSelf(*SpecHandle.Data);
        }
    }
}
