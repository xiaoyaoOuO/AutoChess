// 阶段 1 新增，**阶段 3.2a 起真正生效**（GAS 重构实施方案 §2.1 / §3.3 / §7 阶段 1）：
// 能力 / GE 授予清单（数据资产）。
//
// 为什么是数据资产而不是写在单位类里：§0 C5 与 §1 D12 定了"**基类只放共有项**，
// 干员特有内容走 `UACAbilitySet`"。干员会越来越多，把技能/被动写进 Actor 类层次意味着
// 每加一个干员就要动一次代码；做成数据资产后，内容侧只需给资产填数组。
//
// 授予链：`UBattleWorld::RegisterUnit` → `UACAbilitySetComponent::SetAbilitySet` → `GrantToOwner`
// → `GiveTo`。内容侧的清单建在 `ACBattleContentDefinitions.cpp` 的 `BuildAbilitySets()` 里。
// 另有"追加授予"通道（装备 / 强化 / 词条带来的能力）走 `UACAbilitySetComponent::SetExtraAbilities`
// —— 两条通道共用 `GiveAbilitiesTo` 这一份实现。
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "Templates/SubclassOf.h"
#include "ACAbilitySet.generated.h"

class UAbilitySystemComponent;
class UGameplayAbility;
class UGameplayEffect;

/**
 * 一个单位"出生时该拿到什么"的完整清单。
 *
 * 装配方式：挂在 `UACAbilitySetComponent` 上，由该组件在合适的时机调 `GiveTo(ASC)`（阶段 2/3）。
 */
UCLASS(BlueprintType)
class AUTOCHESSBATTLE_API UACAbilitySet : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    /** 生成时授予的能力（技能 / 普攻）。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|GAS")
    TArray<TSubclassOf<UGameplayAbility>> GrantedAbilities;

    /** 生成时施加的常驻效果（被动 / 装备 / 词条 / 强化）。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|GAS")
    TArray<TSubclassOf<UGameplayEffect>> GrantedEffects;

    /** 每个能力的授予等级（与 GrantedAbilities 等长；缺省按 1 处理）。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|GAS")
    TArray<int32> GrantedAbilityLevels;

    /**
     * 把本清单授予给定的 ASC（**已实现**，实现见 `Private/ACAbilitySet.cpp`）。
     *
     * 行为：
     *   ① 能力：按 `GrantedAbilities` 顺序逐个授予，等级取 `GrantedAbilityLevels` 同索引项
     *      （缺项 / 非正 → 1）；具体授予细节收口在下面的 `GiveAbilitiesTo` 静态函数里，
     *      与"追加授予"通道共用同一份实现。
     *   ② 效果：按 `GrantedEffects` 顺序逐个施加为常驻 GE（被动 / 装备 / 词条 / 强化）。
     *      施加前先 `MakeEffectContext()` 再 `MakeOutgoingSpec(GE, 1.f, Context)`，
     *      最后 `ApplyGameplayEffectSpecToSelf` —— 这是引擎推荐的写法，
     *      比 `ApplyGameplayEffectToSelf` 多一次 Context 构造，换来"施加者/因果链可追溯"。
     *   ③ 幂等：由调用方保证 —— **唯一生效的调用点是 `UBattleWorld::RegisterUnit`**
     *      （`BeginPlay` 里那次因为清单还没注入而空转）。本函数自身不做去重。
     *
     * 为什么签名收 `UAbilitySystemComponent&` 而不是指针：能调用到这个函数就说明"有 ASC 要授予"，
     * 允许传 nullptr 只会让每个实现都在开头写一遍 `if (!ASC) return;`。
     */
    void GiveTo(UAbilitySystemComponent& ASC) const;

    /**
     * 把**一组能力**授予 ASC —— 本文件里"能力授予"细节的唯一实现处。
     *
     * 为什么抽成静态函数：`UACAbilitySetComponent` 还有一条**追加授予**通道
     * （装备 / 强化 / 词条带来的被动能力，见 `UACAbilitySetComponent::SetExtraAbilities`），
     * 那不是一份清单资产，但必须走**同一套** `GiveAbility` 细节
     * （`FGameplayAbilitySpec(Class, Level, InputID, SourceObject)` 四个实参的口径、
     *  等级表的越界处理、空位跳过）。抄第二遍必然分叉。
     *
     * @param Abilities    要授予的能力类（空位会被跳过）
     * @param Levels       与 `Abilities` 按下标对应的等级表；短了或非正一律按 1
     * @param SourceObject 写进 `FGameplayAbilitySpec::SourceObject` 的对象（清单或组件自身）
     */
    static void GiveAbilitiesTo(UAbilitySystemComponent& ASC,
                                const TArray<TSubclassOf<UGameplayAbility>>& Abilities,
                                const TArray<int32>& Levels,
                                UObject* SourceObject);
};
