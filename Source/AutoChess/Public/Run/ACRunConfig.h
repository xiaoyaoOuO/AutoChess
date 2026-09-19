// Run 层参数表（局内循环的全部可调数值）。默认值在 ACRunConfig.cpp 的构造函数里集中赋值。
//
// 设计原则：
// 1. **单一来源**：Run 层任何数值都从这里读，禁止散落在逻辑里写死常量；
// 2. **可替换**：后续把它做成 DataAsset（`UACRunConfigAsset`）即可交给策划配表，
//    逻辑代码不需要改动——各系统只依赖本类的字段语义；
// 3. **来源可追溯**：每个字段注释标注来源。**【原文】** = 策划文档已明确；
//    【建议】= 本文档补出的实现方案，需策划确认；【待定】= 上游缺失（见
//    `doc/design/经济系统详细设计.md` §11 的 E1–E14），此处给出**可运行占位值**，
//    一旦策划定稿只需改 ACRunConfig.cpp 一处。
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Core/ACBattleTypes.h"
#include "Run/ACRunTypes.h"
#include "ACRunConfig.generated.h"

/** Run 层参数表：地图规模、节点权重、经济价格、商店概率、（局外）结算折算率。 */
UCLASS(BlueprintType)
class AUTOCHESS_API UACRunConfig : public UObject
{
    GENERATED_BODY()

public:
    UACRunConfig();

    // -----------------------------------------------------------------------
    // 地图（《自走棋系统结构说明》§5.1）
    // -----------------------------------------------------------------------

    /** 起点连出的支路数。【原文】"起点连接 5 个节点"。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Map")
    int32 EntryBranchCount = 5;

    /** 常规行的节点数下限 / 上限。【原文】"之后每行约 4~5 个节点"。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Map")
    int32 NodesPerRowMin = 4;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Map")
    int32 NodesPerRowMax = 5;

    /** 中转站所在行（0 基）。【原文】"中途有一处中转站节点收束所有路线"。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Map")
    int32 WaypointRow = 8;

    /** BOSS 所在行（0 基）。【原文】"约 15 行之后通向 BOSS 节点"。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Map")
    int32 BossRow = 15;

    /** 视野行数：当前行 + 后续 N 行。【原文】"仅有同一行和接下来 2 行的节点显示"。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Map")
    int32 VisibleRowsAhead = 2;

    /** 常规行的节点类型权重。【建议】（"中转站/起点/终点"由行号决定，不走权重） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Map")
    TMap<EACRunNodeType, float> RowNodeTypeWeights;

    // -----------------------------------------------------------------------
    // 经济（《经济系统详细设计》§7）
    // -----------------------------------------------------------------------

    /** 开局魂晶。【待定 E4】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    int32 StartingSoulCrystal = 20;

    /** 战斗类节点的胜利魂晶奖励。【待定 E4】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    int32 VictoryRewardCombat = 8;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    int32 VictoryRewardElite = 14;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    int32 VictoryRewardChallenge = 18;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    int32 VictoryRewardBoss = 40;

    /** 战斗内每击杀一名敌人的额外魂晶（0 = 只靠节点奖励与效果块产出）。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    int32 SoulCrystalPerKill = 0;

    /** 篝火节点的魂晶收益。【建议】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    int32 CampfireSoulCrystal = 5;

    /** 篝火节点为全队恢复的基础血量比例（按最大生命值百分比）。【建议】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    float CampfireHealPercent = 30.f;

    /** 遗物节点赠送的装备品级上限。【建议】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    int32 RelicEquipmentMaxTier = 2;

    /** 剩余魂晶 → 法托的转化率（每 1 魂晶换多少法托）。【待定 E3】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    float SoulCrystalToFatoRate = 0.5f;

    /** 单局结算的魂晶转化上限。【待定 E3】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Economy")
    int32 SoulCrystalToFatoCap = 60;

    // -----------------------------------------------------------------------
    // 招募（《经济系统详细设计》§7.1）
    // -----------------------------------------------------------------------

    /** 招募价格，下标 = 品级 D/C/B/A/S。【原文】15/25/40/60/70。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Recruit")
    TArray<int32> OperatorRecruitPrices;

    /** 回收价格，下标 = 品级 D/C/B/A/S。【原文】9/15/30/40/50。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Recruit")
    TArray<int32> OperatorRecyclePrices;

    /** 招募刷新费，按"本节点已刷新次数"取下标。【原文】3/5/8，最多 3 次。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Recruit")
    TArray<int32> RecruitRefreshCosts;

    /** 每次刷新的干员栏位数。【原文】"刷新随机 3 名干员"。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Recruit")
    int32 RecruitSlotCount = 3;

    /** 干员品级出现权重，下标 = 品级。【待定 E8】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Recruit")
    TArray<float> OperatorTierWeights;

    /** 每推进多少行，品级权重向高品级偏移一档。【建议】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Recruit")
    int32 LevelUpRowStep = 3;

    // -----------------------------------------------------------------------
    // 集市（《经济系统详细设计》§7.2）
    // -----------------------------------------------------------------------

    /** 装备售价，下标 = 品级 白/蓝/金/红。【原文】5/10/20/30。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Market")
    TArray<int32> EquipmentTierPrices;

    /** 装备回收价，下标 = 品级 白/蓝/金/红。【原文】3/5/12/20。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Market")
    TArray<int32> EquipmentRecyclePrices;

    /** 集市刷新费。【原文】5/10/15，最多 3 次。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Market")
    TArray<int32> MarketRefreshCosts;

    /** 每次刷新的装备栏位数。【原文】"6 个装备 + 2 个策略"；策略（秘术）未定，本期只做装备。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Market")
    int32 MarketSlotCount = 6;

    /** 装备品级出现权重 白/蓝/金/红。【待定 E8】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Market")
    TArray<float> EquipmentTierWeights;

    // -----------------------------------------------------------------------
    // 锻体 / 复活（《经济系统详细设计》§7.3 / §7.4）
    // -----------------------------------------------------------------------

    /** 锻体单次价格。【原文】12 魂晶。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Growth")
    int32 FragmentPrice = 12;

    /** 碎片品阶概率 银/金/红/彩。【原文】57% / 30% / 10% / 3%。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Growth")
    TArray<float> FragmentTierWeights;

    /** 锻体三选一的候选数。【原文】"出现三个同品阶属性碎片"。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Growth")
    int32 FragmentOptionCount = 3;

    /** 复活价格，按"已通过行数 / ReviveRowStep"取下标。【原文】第一关 3 / 第二关 8 / 第三关 12。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Growth")
    TArray<int32> ReviveCosts;

    /** 每通过多少行进入下一档复活价。【建议】 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Growth")
    int32 ReviveRowStep = 5;

    /** 每次复活的永久属性惩罚（百分比，1.0 = 1%）。【原文】"永久降低 1% 属性"。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Growth")
    float ReviveStatPenaltyPercent = 1.f;

    // -----------------------------------------------------------------------
    // 编队与站位
    // -----------------------------------------------------------------------

    /** 上场名额。【原文】"3 个上场名额 + 3 个备战名额"。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Squad")
    int32 ActiveCapacity = 3;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Squad")
    int32 BenchCapacity = 3;

    /** 我方落位行（棋盘 10×8：行 6–9 为我方场地）。按上场顺序依次取用，越界顺延。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Squad")
    TArray<int32> PlayerDeployRows;

    /** 我方落位列。与 PlayerDeployRows 一一对应。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Squad")
    TArray<int32> PlayerDeployCols;

    /** 敌方落位行（行 0–3 为敌方场地）。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Squad")
    TArray<int32> EnemyDeployRows;

    /** 敌方落位列。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Squad")
    TArray<int32> EnemyDeployCols;

    // -----------------------------------------------------------------------
    // 战斗启动选项（透传给 FACBattleLaunchOptions）
    // -----------------------------------------------------------------------

    // 阶段 0.5（D2 / §5.2）：`BattleFixedDt` 已删除 —— 时间来自引擎世界时间（`FACBattleTime`），
    // 不再有"逻辑固定步长"这个配置项，因此也没有可透传给内核的步长。

    /** 单场战斗时限（秒）。对应《假设清单》A9 的 300s 超时判负。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Battle")
    float BattleMaxSeconds = 300.f;

    /** 是否允许秘术命令。对应《假设清单》A14。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Battle")
    bool bAllowMysticCards = true;

    /** 我方可否带"上一场剩余基础血量"进入下一场。关闭则每场满血（对照测试用）。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Battle")
    bool bPersistBaseHP = true;

    // -----------------------------------------------------------------------
    // 示例驱动（AACRunGameMode 的自动演示用）
    // -----------------------------------------------------------------------

    /** 演示模式：每帧自动推进一局（无 UI 输入也能看到完整循环）。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Demo")
    bool bAutoDemoByDefault = true;

    /** 演示模式下每次自动动作之间等待的真实秒数。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Run|Demo")
    float AutoDemoStepIntervalSeconds = 0.6f;

    // -----------------------------------------------------------------------
    // 读取辅助（越界一律回退，避免配置缺失导致崩溃）
    // -----------------------------------------------------------------------

    int32 GetOperatorRecruitPrice(int32 Tier) const;
    int32 GetOperatorRecyclePrice(int32 Tier) const;
    int32 GetEquipmentPrice(int32 Tier) const;
    int32 GetEquipmentRecyclePrice(int32 Tier) const;
    int32 GetRecruitRefreshCost(int32 RefreshCount) const;
    int32 GetMarketRefreshCost(int32 RefreshCount) const;
    int32 GetReviveCost(int32 Row) const;
    int32 GetFragmentPrice() const { return FragmentPrice; }
    int32 GetActiveCapacity() const { return FMath::Max(1, ActiveCapacity); }
    int32 GetBenchCapacity() const { return FMath::Max(0, BenchCapacity); }
    bool CanRefreshRecruit(int32 RefreshCount) const { return RecruitRefreshCosts.IsValidIndex(RefreshCount); }
    bool CanRefreshMarket(int32 RefreshCount) const { return MarketRefreshCosts.IsValidIndex(RefreshCount); }
};
