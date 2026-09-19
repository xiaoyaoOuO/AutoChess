#include "Run/ACRunPostBattle.h"
#include "Run/ACRunConfig.h"
#include "Run/ACRunEconomy.h"
#include "Run/ACRunSquad.h"
#include "Content/ACBattleContentLibrary.h"

namespace
{
    /** 节点胜利的魂晶奖励（《经济系统详细设计》§8.1 的"战斗胜利/事件"通道，数值见 UACRunConfig）。 */
    int32 GetNodeVictoryReward(const UACRunConfig& Config, EACRunNodeType NodeType)
    {
        switch (NodeType)
        {
        case EACRunNodeType::Combat:    return Config.VictoryRewardCombat;
        case EACRunNodeType::Elite:     return Config.VictoryRewardElite;
        case EACRunNodeType::Challenge: return Config.VictoryRewardChallenge;
        case EACRunNodeType::Boss:      return Config.VictoryRewardBoss;
        case EACRunNodeType::Campfire:  return Config.CampfireSoulCrystal;
        default:                        return 0;
        }
    }

    /** 节点掉落的装备品级；-1 = 不掉落。 */
    int32 GetNodeLootTier(const UACRunConfig& Config, EACRunNodeType NodeType)
    {
        switch (NodeType)
        {
        case EACRunNodeType::Combat:    return 0;                                // 白
        case EACRunNodeType::Elite:     return 1;                                // 蓝
        case EACRunNodeType::Relic:     return FMath::Max(0, Config.RelicEquipmentMaxTier);
        case EACRunNodeType::Challenge: return 2;                                // 金
        case EACRunNodeType::Boss:      return 3;                                // 红
        default:                        return INDEX_NONE;
        }
    }

    FText MakeNodeLootName(EACRunNodeType NodeType)
    {
        switch (NodeType)
        {
        case EACRunNodeType::Boss:      return FText::FromString(TEXT("首领遗物"));
        case EACRunNodeType::Challenge: return FText::FromString(TEXT("挑战奖励"));
        case EACRunNodeType::Elite:     return FText::FromString(TEXT("精英战利品"));
        case EACRunNodeType::Relic:     return FText::FromString(TEXT("遗物"));
        default:                        return FText::FromString(TEXT("战利品"));
        }
    }
}

FACRunBattleOutcome FACRunPostBattle::ApplyResult(const FACBattleResult& Result,
                                                  const FACRunMapNode& Node,
                                                  FACRunSquad& Squad,
                                                  FACRunEconomy& Economy,
                                                  const UACRunConfig& Config,
                                                  const UACBattleContentLibrary& Content,
                                                  FRandomStream& Rng,
                                                  const TArray<FName>& UnitsToOperators)
{
    FACRunBattleOutcome Outcome;
    Outcome.NodeType = Node.Type;
    Outcome.bVictory = (Result.Outcome == EACOutcome::Victory);
    Outcome.BattleSeconds = Result.Log.BattleSeconds;
    // 阶段 4（D3）：`Outcome.StateHash = Result.Log.FinalStateHash;` **已删除** ——
    // 两个字段都不存在了（放弃确定性，C3 / §5.3）。

    // -----------------------------------------------------------------------
    // ① + ② 血量持久化与阵亡标记
    //     FACBattleResult::Units 的顺序与 FACBattleSetup::PlayerUnits 一致
    //     （UBattleWorld::CollectUnitResults 按 UnitId 升序收集 = 生成顺序），
    //     因此用"参战顺序 → 干员 ID"的映射表按下标对齐。
    // -----------------------------------------------------------------------
    for (int32 Index = 0; Index < Result.Units.Num(); ++Index)
    {
        const FACUnitBattleResult& UnitResult = Result.Units[Index];
        if (!UnitsToOperators.IsValidIndex(Index))
        {
            continue;
        }

        const FName OperatorId = UnitsToOperators[Index];
        if (OperatorId.IsNone())
        {
            continue;
        }

        float RemainingBaseHP = UnitResult.RemainingBaseHP;
        if (!UnitResult.bDead)
        {
            // 战斗中可能被治疗到超过基础血量上限（护盾/临时上限），持久化时收敛回基础值域。
            const float MaxHP = Squad.GetDerivedMaxHP(OperatorId);
            RemainingBaseHP = FMath::Clamp(RemainingBaseHP, 1.f, FMath::Max(1.f, MaxHP));
        }

        Squad.RecordBattleResult(OperatorId, UnitResult.bDead, RemainingBaseHP);
    }

    // -----------------------------------------------------------------------
    // ③ 奖励结算
    // -----------------------------------------------------------------------
    Outcome.SoulCrystalFromBattle = Result.SoulCrystalDelta;
    if (Result.SoulCrystalDelta != 0)
    {
        Economy.AddSoulCrystal(Result.SoulCrystalDelta);
    }

    if (Outcome.bVictory)
    {
        int32 NodeReward = GetNodeVictoryReward(Config, Node.Type);
        // 战斗内每击杀一名敌人的保底收益（对应 UACRunConfig::SoulCrystalPerKill，默认 0）。
        if (Config.SoulCrystalPerKill > 0)
        {
            int32 TotalKills = 0;
            for (const TPair<int32, int32>& Pair : Result.Stats.KillsBySourceUnit)
            {
                TotalKills += Pair.Value;
            }
            NodeReward += TotalKills * Config.SoulCrystalPerKill;
        }

        if (NodeReward != 0)
        {
            Economy.AddSoulCrystal(NodeReward);
        }
        Outcome.SoulCrystalFromNode = NodeReward;

        const int32 LootTier = GetNodeLootTier(Config, Node.Type);
        if (LootTier != INDEX_NONE)
        {
            TArray<FACRunEquipment> Pool;
            Content.GetEquipmentByTier(LootTier, Pool);
            if (Pool.Num() > 0)
            {
                FACRunEquipment Loot = Pool[Rng.RandRange(0, Pool.Num() - 1)];
                Loot.EquippedToOperatorId = NAME_None;
                Loot.DisplayName = FText::FromString(
                    FString::Printf(TEXT("%s · %s"), *MakeNodeLootName(Node.Type).ToString(), *Loot.DisplayName.ToString()));
                Outcome.Loot.Add(Loot);
            }
            else
            {
                UE_LOG(LogTemp, Warning,
                       TEXT("[Run] 掉落池为空（品级 %d）：请检查 ACBattleContent::BuildEquipmentPools。"), LootTier);
            }
        }
    }

    Outcome.SoulCrystalGained = Outcome.SoulCrystalFromBattle + Outcome.SoulCrystalFromNode;

    Squad.GetDeadOperatorIds(Outcome.DeadOperatorIds);
    Outcome.bSquadWiped = !Squad.HasAnyAlive();

    return Outcome;
}
