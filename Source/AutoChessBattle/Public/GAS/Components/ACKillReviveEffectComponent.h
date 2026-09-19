// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 的 `KillUnit` / `ReviveUnit` / `GrantSoulCrystal` /
// `AddSearchableSecret` 四行 + §3.3 的 `GAS/Components/ACKillReviveEffectComponent.{h,cpp}` /
// §7 阶段 3.1）：击杀 / 复活 / 结算产出的 EffectComponent。
//
// 对应旧动作（ACEffectSystem.cpp:756-770）：
//     KillUnit           → `World->KillUnit(PrimaryTarget, EACDamageReason::Execute)`   （:756-758）
//     ReviveUnit         → `World->ReviveUnit(PrimaryTarget)`                            （:760-762）
//     GrantSoulCrystal   → `World->Stats().AddSoulCrystal(RoundToInt(Value))`            （:764-766）
//     AddSearchableSecret→ `World->Stats().AddSearchableSecret(RoundToInt(Value))`       （:768-770）
//
// `UBattleWorld::KillUnit` / `ReviveUnit` 的签名见 ACBattleWorld.h:72-73；
// 统计入口 `FBattleStatsCollector::AddSoulCrystal` / `AddSearchableSecret` 见
// Diagnostics/ACBattleDiagnostics.h:152-153。
//
// 钩子选择与 `UACSummonEffectComponent` 相同：`OnGameplayEffectApplied`
// （引擎文件 GameplayEffectComponent.h:66，注释推荐优先用它）。理由同样写在那边：
// "每次施加/叠加发生一次、不随周期重复"正是这四个动作的语义。
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectComponent.h"
#include "Core/ACBattleTypes.h"
#include "ACKillReviveEffectComponent.generated.h"

/** 支持的四个动作（对应旧 `EACActionType` 的四项）。 */
UENUM()
enum class EACKillReviveOperation : uint8
{
    /** 旧 `EACActionType::KillUnit`（`Cause = EACDamageReason::Execute`）。 */
    KillUnit,
    /** 旧 `EACActionType::ReviveUnit`。 */
    ReviveUnit,
    /** 旧 `EACActionType::GrantSoulCrystal`（写 `FBattleStatsCollector`）。 */
    GrantSoulCrystal,
    /** 旧 `EACActionType::AddSearchableSecret`（写 `FBattleStatsCollector`）。 */
    AddSearchableSecret
};

/**
 * 在 GE 应用时执行"击杀 / 复活 / 给魂晶 / 加可搜索秘密"。
 *
 * 一个组件承载四个动作（而不是四个组件类）的理由：它们的触发时机、目标解析、
 * 以及"从 spec 取 World"的脚手架完全一样，差别只有最后一行调用；
 * 拆成四个类会把同一段脚手架复制四份，而这段脚手架正是"最容易写错、也最难排查"的部分
 * （取错 World 的表现是"什么都没发生"，不会报错）。
 */
UCLASS(DisplayName = "AC 击杀/复活/结算产出（KillUnit / ReviveUnit / GrantSoulCrystal / AddSearchableSecret）")
class AUTOCHESSBATTLE_API UACKillReviveEffectComponent : public UGameplayEffectComponent
{
    GENERATED_BODY()

public:
    /** 要做哪件事。 */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Effect")
    EACKillReviveOperation Operation = EACKillReviveOperation::KillUnit;

    /**
     * 数量（仅 `GrantSoulCrystal` / `AddSearchableSecret` 用）。
     * 旧代码取 `FMath::RoundToInt(Action.Value.Get(Context.Tier))`（ACEffectSystem.cpp:765 / 769）。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Effect")
    int32 Amount = 1;

    /**
     * 操作对象是"效果的目标"还是"效果的施加者"。
     *
     * 旧动作的目标由**效果块的选择器**决定（`TargetSelector`，默认 `Self`），
     * 动作本身只拿到已解析的 `PrimaryTarget`（ACEffectSystem.cpp:756-762）。
     * 因此这里给两个选项，由内容决定：
     *   `true`  → 作用于**承效者**（spec 的 target，对应旧 `PrimaryTarget`）；
     *   `false` → 作用于**施加者**（对应旧块的 `TargetSelector = Self` 且 Source == PrimaryTarget 的场景）。
     * 取不到目标时不做事、只告警（不能拿 `InvalidUnitId` 去 `KillUnit`，那会静默无效）。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Effect")
    bool bAffectTarget = true;

    /**
     * 每次施加/叠加时执行一次。
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
