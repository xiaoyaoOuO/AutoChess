// M16 运行侧数据入口：注册定义资产并组装 FACBattleDataContext。
// 注意：资产内容由编辑器/DataAsset 提供；本类只做索引与默认值兜底。
//
// 阶段 3.2a（GAS 重构实施方案 §3.1「D 行」/ §7 阶段 3.2）：旧的"效果块库"通道
// （`SetEffectLibrary` / `SetCodeEffectLibrary` / `BuildMergedEffectLibrary` / `EffectLibrary`）
// 随 `FACEffectBlock` + `FEffectSystem` 一起删除，内容侧改为 `RegisterAbilitySet` 一条通道。
//
// **阶段 4（§7 阶段 3.3/4）**：3.2a 的过渡通道 `SetEffectBlockGEs`（`FName` 效果块 Id → GE 类）
// 与成员 `EffectBlockGEs` **已删除** —— `Core/ACBattleSetup.h` 与 `Run/*` 的效果块字段
// 已改成类引用，翻译层因此整体退场。本子系统现在只剩"注册定义 + 组装上下文"两件事。
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/SubclassOf.h"
#include "Core/ACDataTypes.h"
#include "Battle/ACBattleWorld.h"
#include "ACBattleDataSubsystem.generated.h"

class UACAbilitySet;

UCLASS()
class AUTOCHESSBATTLE_API UBattleDataSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "Battle|Data")
    void SetRuleConfig(UBattleRuleConfig* InRuleConfig);

    /**
     * 注册"单位定义 Id → 能力 / GE 授予清单"（阶段 3.2a）。
     *
     * 为什么键是 `DefinitionId` 而不是"定义对象指针"：内核拿到的就是
     * `AACBattleUnitBase` 上的 `DefinitionId`（`GetDefinitionId()`），而定义对象本身可能
     * 来自资产（`UUnitDefinitionBase` 子类）或代码侧（`NewObject(GetTransientPackage())`），
     * 用 Id 做键对两种来源一视同仁，也让本子系统不必持有定义对象的强引用。
     */
    UFUNCTION(BlueprintCallable, Category = "Battle|Data")
    void RegisterAbilitySet(FName DefinitionId, UACAbilitySet* InAbilitySet);

    // 阶段 3.3 已删除：`void SetEffectBlockGEs(const TMap<FName, TSubclassOf<UGameplayEffect>>&)`。
    // 它是 3.2a 的**过渡映射表**写入端（`FName` 效果块 Id → GE 类），服务于
    // "`FACPlayerUnitSpec::UpgradeEffectBlockIds` 等字段还是 `FName`"的那段时间。
    // 那些字段现在直接是 `TArray<TSubclassOf<UGameplayEffect>>`（见 `Core/ACBattleSetup.h`），
    // 因此这张表和它的两个端点（本函数 + `FACBattleDataContext::EffectBlockGEs`）一起删除。

    UFUNCTION(BlueprintCallable, Category = "Battle|Data")
    void RegisterUnitDefinition(UUnitDefinitionBase* InDefinition);

    UFUNCTION(BlueprintCallable, Category = "Battle|Data")
    void RegisterSummonDefinition(USummonDefinition* InDefinition);

    UFUNCTION(BlueprintPure, Category = "Battle|Data")
    const UBattleRuleConfig* GetRuleConfig() const;

    /** 组装战斗数据上下文；缺省时使用内置规则配置，保证无资产也能跑通框架。 */
    FACBattleDataContext BuildContext() const;

private:
    UPROPERTY() TObjectPtr<UBattleRuleConfig> RuleConfig = nullptr;
    UPROPERTY() TArray<TObjectPtr<UUnitDefinitionBase>> UnitDefinitions;
    UPROPERTY() TArray<TObjectPtr<USummonDefinition>> SummonDefinitions;

    /**
     * 定义 Id → 能力 / GE 授予清单。
     *
     * 为什么用 `UPROPERTY` 持强引用：清单是 `NewObject(GetTransientPackage())` 造出来的
     * 代码侧对象（`ACBattleContentDefinitions.cpp` 的既有模式），没有磁盘资产替它保命；
     * 本子系统是 `UGameInstanceSubsystem`（与 GameInstance 同生共死），
     * 持强引用既保证 GC 安全，也保证生命周期覆盖整场战斗。
     */
    UPROPERTY() TMap<FName, TObjectPtr<UACAbilitySet>> AbilitySets;

    // 阶段 3.3：过渡映射表成员 `TMap<FName, TSubclassOf<UGameplayEffect>> EffectBlockGEs` 已删除
    //（见 `SetEffectBlockGEs` 处的说明）。

    /** 缺省规则配置（懒创建）；由 GetRuleConfig 在无显式配置时兜底。 */
    UPROPERTY(Transient)
    TObjectPtr<UBattleRuleConfig> DefaultRuleConfig = nullptr;
};
