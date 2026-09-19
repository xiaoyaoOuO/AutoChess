#include "Run/ACRunConfig.h"
#include "Run/ACRunMap.h"

namespace
{
    /** 越界读取：Index 超界时取最后一个有效元素，数组为空时返回 Fallback。 */
    int32 ReadPrice(const TArray<int32>& Prices, int32 Index, int32 Fallback)
    {
        if (Prices.Num() == 0)
        {
            return Fallback;
        }
        return Prices[FMath::Clamp(Index, 0, Prices.Num() - 1)];
    }
}

UACRunConfig::UACRunConfig()
{
    // 全部默认值集中在构造函数里，而不是字段初始化列表：
    //   ① 反射类型（GENERATED_BODY）在构造前不保证已就绪，构造里赋值最稳妥；
    //   ② 数值来源集中一处，便于对照策划文档逐条核对（注释里都是文档章节号）。
    //
    // ---- 地图（《自走棋系统结构说明》§5.1）----
    EntryBranchCount = 5;
    NodesPerRowMin = 4;
    NodesPerRowMax = 5;
    WaypointRow = 8;
    BossRow = 15;
    VisibleRowsAhead = 2;

    // ---- 经济（《经济系统详细设计》§7，【待定 E3/E4】项为可运行占位值）----
    StartingSoulCrystal = 20;
    VictoryRewardCombat = 8;
    VictoryRewardElite = 14;
    VictoryRewardChallenge = 18;
    VictoryRewardBoss = 40;
    SoulCrystalPerKill = 0;
    CampfireSoulCrystal = 5;
    CampfireHealPercent = 30.f;
    RelicEquipmentMaxTier = 2;
    SoulCrystalToFatoRate = 0.5f;
    SoulCrystalToFatoCap = 60;

    // ---- 招募（§7.1）----
    OperatorRecruitPrices = { 15, 25, 40, 60, 70 };
    OperatorRecyclePrices = { 9, 15, 30, 40, 50 };
    RecruitRefreshCosts = { 3, 5, 8 };
    RecruitSlotCount = 3;
    OperatorTierWeights = { 40.f, 30.f, 18.f, 9.f, 3.f };
    LevelUpRowStep = 3;

    // ---- 集市（§7.2）----
    EquipmentTierPrices = { 5, 10, 20, 30 };
    EquipmentRecyclePrices = { 3, 5, 12, 20 };
    MarketRefreshCosts = { 5, 10, 15 };
    MarketSlotCount = 6;
    EquipmentTierWeights = { 45.f, 33.f, 18.f, 4.f };

    // ---- 锻体 / 复活（§7.3 / §7.4）----
    FragmentPrice = 12;
    FragmentTierWeights = { 57.f, 30.f, 10.f, 3.f };
    FragmentOptionCount = 3;
    ReviveCosts = { 3, 8, 12 };
    ReviveRowStep = 5;
    ReviveStatPenaltyPercent = 1.f;

    // ---- 编队与站位（§5.4；棋盘 10×8：行 6–9 我方、行 0–3 敌方）----
    ActiveCapacity = 3;
    BenchCapacity = 3;
    PlayerDeployRows = { 8, 8, 7, 7, 6, 6 };
    PlayerDeployCols = { 2, 5, 3, 4, 3, 4 };
    EnemyDeployRows = { 1, 1, 2, 2, 0, 0 };
    EnemyDeployCols = { 2, 5, 3, 4, 3, 4 };

    // ---- 战斗启动选项（透传 FACBattleLaunchOptions）----
    // 阶段 0.5：不再有固定步长可配（时间来自引擎世界时间，见 ACRunConfig.h）。
    BattleMaxSeconds = 300.f;
    bAllowMysticCards = true;
    bPersistBaseHP = true;

    // ---- 示例驱动 ----
    bAutoDemoByDefault = true;
    AutoDemoStepIntervalSeconds = 0.6f;

    // ---- 节点类型权重（§5.2 的 8 类事件；权重为【建议】值）----
    ACRunNodeType::BuildDefaultWeights(RowNodeTypeWeights);
}

int32 UACRunConfig::GetOperatorRecruitPrice(int32 Tier) const
{
    return ReadPrice(OperatorRecruitPrices, Tier, 15);
}

int32 UACRunConfig::GetOperatorRecyclePrice(int32 Tier) const
{
    return ReadPrice(OperatorRecyclePrices, Tier, 9);
}

int32 UACRunConfig::GetEquipmentPrice(int32 Tier) const
{
    return ReadPrice(EquipmentTierPrices, Tier, 5);
}

int32 UACRunConfig::GetEquipmentRecyclePrice(int32 Tier) const
{
    return ReadPrice(EquipmentRecyclePrices, Tier, 3);
}

int32 UACRunConfig::GetRecruitRefreshCost(int32 RefreshCount) const
{
    return ReadPrice(RecruitRefreshCosts, RefreshCount, 3);
}

int32 UACRunConfig::GetMarketRefreshCost(int32 RefreshCount) const
{
    return ReadPrice(MarketRefreshCosts, RefreshCount, 5);
}

int32 UACRunConfig::GetReviveCost(int32 Row) const
{
    const int32 Tier = ReviveRowStep > 0 ? FMath::Max(0, Row) / ReviveRowStep : 0;
    return ReadPrice(ReviveCosts, Tier, 3);
}
