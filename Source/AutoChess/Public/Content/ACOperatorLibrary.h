// Run 层内容视图：把 `UACBattleContentLibrary` 的代码侧定义整理成**局内循环需要的样子**。
//
// 为什么要有这一层：
//   战斗内核需要的是"属性 + 技能 + 普攻"；Run 层需要的是"招募池 / 品级 / 强化候选 / 兜底属性"。
//   两者读同一份定义，但关注点不同。本类只做**投影**，不持有内容（内容由 UACBattleContentLibrary 持有），
//   也不做任何规则计算。
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/ACBattleTypes.h"
#include "Core/ACDataTypes.h"
#include "Run/ACRunTypes.h"
// 阶段 3.3：`ResolveUpgradeChoice` 按值返回 `FACContentUpgradeOption`（强化候选的
// GE / 能力类数组），因此需要完整类型 —— 只靠前向声明无法按值返回。
#include "Content/ACBattleContentLibrary.h"
#include "ACOperatorLibrary.generated.h"

class UEnemyDefinition;
class UOperatorDefinition;

/** 干员模板：Run 侧需要的字段 + 定义资产指针。 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACOperatorTemplate
{
    GENERATED_BODY()

    /** 供 Run 层做属性合成与装备槽查询；强引用（GC 安全）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Content")
    TObjectPtr<UOperatorDefinition> Definition = nullptr;

    /** 招募品级 0..4（D/C/B/A/S）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Content")
    int32 RecruitTier = 0;

    /** 职业标签，用于展示与后续词条库筛选。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Content")
    FGameplayTag ClassTag;
};

/**
 * 内容视图子系统（GameInstance 级）。
 * 依赖方向：本类 → UACBattleContentLibrary → UBattleDataSubsystem → 战斗内核。
 */
UCLASS()
class AUTOCHESS_API UACOperatorLibrary : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    /** 强制内容就绪（幂等）；ACRunSubsystem 会在开局前调用。 */
    void EnsureContentReady();

    // ---- 查询 ----
    const TArray<FName>& GetOperatorIds() const { return OperatorIds; }
    const FACOperatorTemplate* FindOperatorTemplate(FName OperatorId) const;
    const UOperatorDefinition* FindOperatorDefinition(FName OperatorId) const;
    const UEnemyDefinition* FindEnemyDefinition(FName EnemyDefinitionId) const;

    /** 按品级筛选干员（招募池）。 */
    void GetOperatorIdsByTier(int32 Tier, TArray<FName>& OutIds) const;

    /** 从全部干员里按确定性随机抽 Count 个不重复 ID（招募刷新用）。 */
    void PickRandomOperators(FRandomStream& Stream, int32 Count, TArray<FName>& OutIds) const;

    /**
     * 该干员在指定等级档位（0=C … 3=S）应注册的个性强化候选
     * （阶段 3.3：从 `FName` 效果块 Id 改成 `FACContentUpgradeOption`）。
     */
    FACContentUpgradeOption ResolveUpgradeChoice(FName OperatorId, int32 TierIndex) const;

    /** 等级基础属性（无定义时回退兜底块）。 */
    void GetStatsByLevel(FName OperatorId, int32 Level, FACStatBlock& OutBlock) const;

    /** 兜底属性块：**没有任何定义资产时**也能让战斗跑起来。 */
    const FACStatBlock& GetDefaultStatBlock() const { return DefaultStatBlock; }

private:
    UACBattleContentLibrary* GetContent() const;

    UPROPERTY()
    TMap<FName, FACOperatorTemplate> OperatorTemplates;
    UPROPERTY()
    TArray<FName> OperatorIds;

    /** 无定义资产时的兜底属性（示例值：能打死人，也不至于秒杀）。 */
    UPROPERTY()
    FACStatBlock DefaultStatBlock;
};
