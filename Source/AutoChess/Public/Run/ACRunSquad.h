// Run 层：编队与干员（《自走棋系统结构说明》§5.4 / §5.11 / §5.12 / §5.13）。
//
// 本文件是"局内棋子"的唯一事实源：
//   - 编成（上场 / 备战）
//   - 等级（重复招募 D→S）与个性强化 / 通用词条的效果块集合
//   - 装备镶嵌
//   - 锻体碎片与复活惩罚带来的永久属性修饰
//   - 战斗后的血量持久化（基础血量）与阵亡标记
//
// 它**不做**战斗判定，也**不读**战斗世界：战斗侧需要的一切都在 StartBattle 之前被
// ACRunBattleAssembler 翻译成 FACPlayerUnitSpec 快照（见 ACRunStatUtils.h 的口径说明）。
#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "Templates/SubclassOf.h"
#include "Core/ACBattleTypes.h"
#include "Run/ACRunTypes.h"
// 必须包含：本文件的 FACRunRecruitOutcome 是 USTRUCT，其 GENERATED_BODY() 与
// UPROPERTY 的反射样板都由这个头文件提供。少了它就会出现
// "error C4430: 缺少类型说明符" + "error C2039: StaticStruct 不是成员" 这对组合错误。
#include "ACRunSquad.generated.h"

class UACOperatorLibrary;
class UACRunConfig;
class UOperatorDefinition;
class UGameplayEffect;
class UGameplayAbility;
struct FACRunEquipment;
struct FACRunFragment;

/** 一次招募的结果（供上层做提示/埋点）。 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunRecruitOutcome
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Squad")
    bool bSuccess = false;

    /** true = 该干员已在编队中，本次是升级而非新增。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Squad")
    bool bLeveledUp = false;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Squad")
    int32 NewLevel = 0;

    /** true = 已无空位（上场与备战都满）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Squad")
    bool bNoRoom = false;
};

/**
 * 编队。所有"查/改"都以 OperatorId（FName）为键，不用下标——
 * 下标会随上场/备战互换而失效，而 OperatorId 是稳定的（同 ID 即同干员，重复招募只升级）。
 */
class AUTOCHESS_API FACRunSquad
{
public:
    void Initialize(const UACRunConfig* InConfig, const UACOperatorLibrary* InLibrary);

    // ---- 查询 ----
    int32 Num() const { return Operators.Num(); }
    const TArray<FACRunOperator>& GetAll() const { return Operators; }
    const TArray<FName>& GetActiveIds() const { return ActiveOperatorIds; }
    const TArray<FName>& GetBenchIds() const { return BenchOperatorIds; }

    const FACRunOperator* Find(const FName OperatorId) const;
    FACRunOperator* FindMutable(const FName OperatorId);

    bool HasAnyAlive() const;
    void GetDeadOperatorIds(TArray<FName>& OutIds) const;

    // ---- 编成 ----
    /**
     * 招募 / 升级。
     * 规则（《系统结构说明》§5.11）：新干员为 D 级（Level 0）；重复招募升一级，最高 S 级。
     * 升级到 C/S 时自动选定个性强化（取候选列表第一项，保证自动演示无需 UI 输入；
     * 正式版本应改为三选一 UI，见 ACRunSquad.cpp 的 TODO）。
     */
    FACRunRecruitOutcome RecruitOperator(FName OperatorId);

    /** 移除一名干员（回收 / 事件效果）。 */
    bool RemoveOperator(FName OperatorId);

    /** 是否还能再收一人（上场或备战有空位）。 */
    bool HasRoomForNewOperator() const;

    /** 上场↔备战互换（名额已满时的常见操作）。 */
    bool SwapActive(FName ActiveId, FName BenchId);
    /** 把备战席干员提到上场（需要空位）。 */
    bool PromoteToActive(FName OperatorId);
    /** 把上场干员放到备战席（需要空位）。 */
    bool DemoteToBench(FName OperatorId);

    /**
     * 开战前整理编成（每次开战前调用一次）：
     *   ① 阵亡干员不再占据上场名额；
     *   ② 只要上场有空位，就按确定性顺序从备战席补入存活干员。
     * 这样"打完一场有人倒下"之后，下一场自动由备战席顶上，无需 UI 介入也能持续运行。
     */
    void PrepareForBattle();

    // ---- 养成 ----
    bool EquipItem(FName OperatorId, const FACRunEquipment& Equipment);
    bool UnequipItem(FName OperatorId, FName EquipmentId);
    /** 锻体：施加永久属性修饰。 */
    bool ApplyPermanentModifier(FName OperatorId, const FACRunStatModifier& Modifier);
    bool ApplyFragment(FName OperatorId, const FACRunFragment& Fragment);
    /** 复活：清阵亡标记 + 满血 + 施加永久属性惩罚。 */
    bool ReviveOperator(FName OperatorId, float PenaltyPercent);

    /** 篝火/事件治疗：按最大生命值的百分比恢复基础血量（不改变阵亡状态）。 */
    bool HealOperator(FName OperatorId, float PercentOfMaxHP);

    // ---- 战斗结果回流 ----
    /** 记录本场结束时的基础血量（< 0 会被夹到 0）。 */
    void RecordBattleResult(FName OperatorId, bool bDead, float RemainingBaseHP);

    // ---- 派生属性 ----
    /**
     * 合成参战基础属性：等级基础值 + 永久修饰（锻体/复活/装备属性）。
     * Definition 由 UACOperatorLibrary 提供；无资产时用库里的兜底属性。
     */
    void ComputeBaseStats(const FName OperatorId, FACStatBlock& OutBlock) const;

    /** 该干员当前的派生最大生命值（= 合成基础属性里的 MaxHP；用于满血判定与 UI）。 */
    float GetDerivedMaxHP(const FName OperatorId) const;

    /** 战斗落位：按上场顺序从 Config 的落位表取格子。 */
    FACHexCoord GetFormationCell(int32 ActiveIndex) const;

    /** 装备槽上限（D=1 … S=4）。 */
    int32 GetEquipSlotMax(const FName OperatorId) const;

    /** 该干员当前应注册的战斗内 GE（个性强化 + 通用词条，按等级档位解析）。 */
    void CollectUpgradeEffects(const FName OperatorId, TArray<TSubclassOf<UGameplayEffect>>& OutEffects) const;

    /**
     * 该干员当前应授予的**被动能力**（阶段 3.3 新增）。
     *
     * 汇总两处来源，顺序固定：① 个性强化带来的能力（`FACRunOperator::UpgradeGrantedAbilities`）；
     * ② 通用词条带来的能力（`TraitGrantedAbilities`）。
     * ⚠️ **装备带来的能力不在这里** —— 装备库不在编队里（它在 `UACRunSubsystem::EquipmentInventory`），
     * 由 `FACRunBattleAssembler` 在装配时按 `EquippedToOperatorId` 过滤后追加
     *（那里本来就在遍历装备库存算属性与时间轴条目，多合并一件事不增加新的遍历）。
     */
    void CollectGrantedAbilities(const FName OperatorId, TArray<TSubclassOf<UGameplayAbility>>& OutAbilities) const;

    /** 调试/日志用摘要。 */
    FString ToDebugString() const;

private:
    /** 按 OperatorId 排序的确定性顺序（同 ID 唯一，故这里只是稳定排序手段）。 */
    void SortOperators();
    void RebuildRosterIndex();
    void ApplyLevelUp(FACRunOperator& Operator);

    const UACRunConfig* Config = nullptr;
    const UACOperatorLibrary* Library = nullptr;

    /** 全部干员（上场 + 备战）。 */
    TArray<FACRunOperator> Operators;
    TArray<FName> ActiveOperatorIds;
    TArray<FName> BenchOperatorIds;
};
