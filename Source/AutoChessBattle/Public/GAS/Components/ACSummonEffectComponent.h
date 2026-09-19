// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 的 `SummonUnit` 行 / §3.3 的
// `GAS/Components/ACSummonEffectComponent.{h,cpp}` / §7 阶段 3.1）：召唤 EffectComponent。
//
// 对应旧的 `EACActionType::SummonUnit`（ACEffectSystem.cpp:749-754）：
//     `World->SpawnSummonFromSpec(Action.SubId, Context.Source);`
// 旧的"一次动作招 4 个体"是用 4 个动作表达的（ACBattleContentDefinitions.cpp:424-432 的注释明说
// "为避免为'一次动作招 4 个体'引入新机制，这里用 4 个动作"）—— 本组件直接用 `SummonCount` 表达，
// 语义等价、且不必靠"同一个 SubId 执行 4 次"这种巧合。
//
// ---------------------------------------------------------------------------
// 已核实的 `UGameplayEffectComponent` 钩子（引擎文件：GameplayEffectComponent.h）
// ---------------------------------------------------------------------------
//   * `virtual bool CanGameplayEffectApply(const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) const`
//     —— GameplayEffectComponent.h:47
//   * `virtual bool OnActiveGameplayEffectAdded(FActiveGameplayEffectsContainer&, FActiveGameplayEffect&) const`
//     —— GameplayEffectComponent.h:54
//   * `virtual void OnGameplayEffectExecuted(FActiveGameplayEffectsContainer&, FGameplayEffectSpec&, FPredictionKey&) const`
//     —— GameplayEffectComponent.h:60
//   * `virtual void OnGameplayEffectApplied(FActiveGameplayEffectsContainer&, FGameplayEffectSpec&, FPredictionKey&) const`
//     —— GameplayEffectComponent.h:66
//   * `virtual void OnGameplayEffectChanged()` —— GameplayEffectComponent.h:71
//   （`UGameplayEffectComponent` 的类声明在 GameplayEffectComponent.h:31-90；它是 `Abstract, Const,
//     DefaultToInstanced, EditInlineNew, CollapseCategories, Within=GameplayEffect`。）
//
// **本组件选 `OnGameplayEffectApplied`**，理由（引擎注释里就写着）：
//   「Called when a Gameplay Effect is initially applied, or stacked. … This call does not happen
//     periodically, nor through replication. One should favor this function over
//     OnActiveGameplayEffectAdded & OnGameplayEffectExecuted」（GameplayEffectComponent.h:62-66）。
//   "召唤"要的正是"每次施加/叠加时发生一次、不随周期重复"的语义。
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectComponent.h"
#include "ACSummonEffectComponent.generated.h"

/**
 * 在 GE 应用时召唤单位。
 *
 * 配置：`SummonSpecId`（对应 `FACSummonSpec::DefinitionId`，例如 `SUM_Wormling`）
 * 与 `SummonCount`（召唤数量）。
 *
 * 装配方式：由 GE 在构造函数里 `AddComponent<UACSummonEffectComponent>()`（见
 * `UACGE_Enemy_MotherNest_Spawn`，ACGE_ContentEffects.cpp）。
 */
UCLASS(DisplayName = "AC 召唤单位（SummonUnit）")
class AUTOCHESSBATTLE_API UACSummonEffectComponent : public UGameplayEffectComponent
{
    GENERATED_BODY()

public:
    /**
     * 召唤的规格 Id（旧 `FACEffectAction::SubId`）。
     * 对应 `UBattleWorld::SpawnSummonFromSpec(FName SummonSpecId, FUnitId OwnerUnitId)` 的第一个参数
     * （ACBattleWorld.h:146）。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Summon")
    FName SummonSpecId;

    /**
     * 召唤数量。
     * 旧内容用"同一个 SubId 执行 4 次"表达 4 只（ACBattleContentDefinitions.cpp:424-432），
     * 本字段把它变成一个显式数量，语义等价。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Summon")
    int32 SummonCount = 1;

    /**
     * 每次施加/叠加时召唤一次（引擎注释推荐的钩子，见文件头）。
     *
     * 钩子签名逐字对应 GameplayEffectComponent.h:66：
     *   `virtual void OnGameplayEffectApplied(FActiveGameplayEffectsContainer& ActiveGEContainer,
     *                                         FGameplayEffectSpec& GESpec,
     *                                         FPredictionKey& PredictionKey) const {}`
     * 三个参数本组件都用不到（不读容器、不改 spec、不预测），但**签名必须逐字一致**才构成重写。
     */
    virtual void OnGameplayEffectApplied(FActiveGameplayEffectsContainer& ActiveGEContainer,
                                         FGameplayEffectSpec& GESpec,
                                         FPredictionKey& PredictionKey) const override;
};
