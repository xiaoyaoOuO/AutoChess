#include "Run/ACRunBattleAssembler.h"
#include "Run/ACRunConfig.h"
#include "Run/ACRunMap.h"
#include "Run/ACRunSquad.h"
#include "Run/ACRunStatUtils.h"
#include "Content/ACOperatorLibrary.h"

namespace
{
    const FName EnemyId_Worm(TEXT("EN_Worm"));
    const FName EnemyId_HuskGrub(TEXT("EN_HuskGrub"));
    const FName EnemyId_GreatLeech(TEXT("EN_GreatLeech"));
    const FName EnemyId_HardShellBeetle(TEXT("EN_HardShellBeetle"));
    const FName EnemyId_DevourerWorm(TEXT("EN_DevourerWorm"));
    const FName EnemyId_MotherNest(TEXT("EN_MotherNest"));

    /** 从池子里按数量随行数增长的规则随机取敌人（同一 ID 可出现多次）。*/
    void PickEnemies(FRandomStream& Stream, const TArray<FName>& Pool, int32 Count, TArray<FName>& OutIds)
    {
        if (Pool.Num() == 0 || Count <= 0)
        {
            return;
        }
        for (int32 Index = 0; Index < Count; ++Index)
        {
            OutIds.Add(Pool[Stream.RandRange(0, Pool.Num() - 1)]);
        }
    }
}

FACHexCoord FACRunBattleAssembler::GetPlayerCell(const UACRunConfig& Config, int32 ActiveIndex)
{
    const int32 RowCount = Config.PlayerDeployRows.Num();
    const int32 ColCount = Config.PlayerDeployCols.Num();
    if (RowCount == 0 || ColCount == 0)
    {
        return FACHexCoord(8, 2 + ActiveIndex);
    }
    const int32 Index = FMath::Max(0, ActiveIndex);
    return FACHexCoord(Config.PlayerDeployRows[FMath::Clamp(Index, 0, RowCount - 1)],
                       Config.PlayerDeployCols[FMath::Clamp(Index, 0, ColCount - 1)]);
}

FACEnemyUnitSpec FACRunBattleAssembler::MakeEnemyUnitSpec(FName EnemyDefinitionId,
                                                          const UACRunConfig& Config,
                                                          int32 EnemyIndex,
                                                          const TArray<TSubclassOf<UGameplayEffect>>& EnemyEffects)
{
    FACEnemyUnitSpec Spec;
    Spec.DefinitionId = EnemyDefinitionId;
    Spec.TierIndex = 0;                       // 敌人定义只有一个属性档，靠编排与数量调节强度。
    Spec.Effects = EnemyEffects;

    const int32 RowCount = Config.EnemyDeployRows.Num();
    const int32 ColCount = Config.EnemyDeployCols.Num();
    const int32 Index = FMath::Max(0, EnemyIndex);
    if (RowCount > 0 && ColCount > 0)
    {
        Spec.SpawnCell = FACHexCoord(Config.EnemyDeployRows[FMath::Clamp(Index, 0, RowCount - 1)],
                                     Config.EnemyDeployCols[FMath::Clamp(Index, 0, ColCount - 1)]);
    }
    else
    {
        Spec.SpawnCell = FACHexCoord(1, 2 + (Index % 4));
    }

    // BaseStats 留空：由 AACBattleUnitBase 经 UEnemyDefinition::StatsByLevel 取（定义优先于快照）。
    Spec.BaseStats.InitDefaults();
    return Spec;
}

void FACRunBattleAssembler::BuildLaunchOptions(const UACRunConfig& Config, FACBattleLaunchOptions& OutOptions)
{
    // 阶段 0.5（D2 / §5.2）：MaxCatchUpSteps 已随追赶逻辑一起删除（会话现在直接按帧 Step）。
    // 阶段 4（D7 / §5.4）：`OutOptions.bHeadless = false;` 也**已删除** ——
    // 字段本身（`FACBattleLaunchOptions::bHeadless`）不存在了，C4 定的是"所有战斗都带表现"。
    OutOptions.MaxBattleSeconds = Config.BattleMaxSeconds;
    OutOptions.bAllowMysticCards = Config.bAllowMysticCards;
    OutOptions.LogLevel = 1;
}

FACRunEncounter FACRunBattleAssembler::GenerateEncounter(const FACRunMapNode& Node)
{
    FACRunEncounter Encounter;
    Encounter.EncounterId = FName(*FString::Printf(TEXT("ENC_N%d_%d"), Node.NodeId, Node.Row));

    // 遭遇种子 = HashCombine(RunSeed, NodeId)，由 FACRunMap::Generate 写入节点。
    // 因此"地图上看到的节点"与"实际生成的敌人"必然同源。
    FRandomStream Stream(Node.EncounterSeed);

    // 难度随行数增长：敌人数按行数区间递增（第 0 行 2 只，此后递增，BOSS 行前最多 5 只）。
    const int32 Row = FMath::Max(0, Node.Row);
    const int32 NormalCount = FMath::Clamp(2 + Row / 3, 2, 5);

    switch (Node.Type)
    {
    case EACRunNodeType::Entrance:
        Encounter.DisplayName = FText::FromString(TEXT("入口（无战斗）"));
        break;

    case EACRunNodeType::Elite:
    {
        Encounter.DisplayName = FText::FromString(TEXT("精英 · 虫巢守卫"));
        const TArray<FName> Pool = { EnemyId_GreatLeech, EnemyId_HardShellBeetle, EnemyId_DevourerWorm };
        PickEnemies(Stream, Pool, Row >= 10 ? 2 : 1, Encounter.EnemyDefinitionIds);
        // 精英额外加 2 只小怪，避免"单挑精英"节奏过慢。
        PickEnemies(Stream, { EnemyId_Worm }, 2, Encounter.EnemyDefinitionIds);
        break;
    }

    case EACRunNodeType::Challenge:
    {
        Encounter.DisplayName = FText::FromString(TEXT("挑战 · 高危虫群"));
        const TArray<FName> Pool = { EnemyId_Worm, EnemyId_HuskGrub, EnemyId_GreatLeech };
        PickEnemies(Stream, Pool, NormalCount + 1, Encounter.EnemyDefinitionIds);
        break;
    }

    case EACRunNodeType::Boss:
        Encounter.DisplayName = FText::FromString(TEXT("首领 · 蠕虫母巢"));
        Encounter.EnemyDefinitionIds.Add(EnemyId_MotherNest);
        // 母巢自身还会在开局通过效果块召唤 4 只蠕虫幼体（见内容定义）。
        break;

    case EACRunNodeType::Waypoint:
    case EACRunNodeType::Merchant:
    case EACRunNodeType::Campfire:
    case EACRunNodeType::Relic:
        // 非战斗节点：显式留空，调用方应跳过战斗（保留函数是为了让"节点 → 遭遇"只有一条路径）。
        Encounter.DisplayName = FText::FromString(TEXT("非战斗节点"));
        break;

    case EACRunNodeType::Combat:
    default:
    {
        Encounter.DisplayName = FText::FromString(TEXT("遇敌 · 蚀隧虫群"));
        const TArray<FName> Pool = { EnemyId_Worm, EnemyId_HuskGrub, EnemyId_Worm };
        PickEnemies(Stream, Pool, NormalCount, Encounter.EnemyDefinitionIds);
        break;
    }
    }

    // 行数越高，敌人开局自带一点强化（用效果块表达，不新增代码）。
    return Encounter;
}

void FACRunBattleAssembler::BuildBattleSetup(const FACRunEncounter& Encounter,
                                             const FACRunMapNode& Node,
                                             const FACRunSquad& Squad,
                                             const UACRunConfig& Config,
                                             const UACOperatorLibrary& Library,
                                             const TArray<FACRunEquipment>& EquipmentInventory,
                                             FACBattleSetup& OutSetup,
                                             FACBattleLaunchOptions& OutOptions)
{
    OutSetup = FACBattleSetup();

    // -----------------------------------------------------------------------
    // 1) 战斗标识
    //    阶段 4（D3 / §5.3）：这里原来的"战斗种子 = HashCombine(RunSeed, Node.EncounterSeed)"
    //    与 `OutSetup.RngSeed` **已删除** —— 战斗内核不再有随机流（C3），
    //    种子只用于 Run 层的遭遇生成（`Node.EncounterSeed`，见 `GenerateEncounter`）。
    //    BattleId 只用于结果配对与日志关联，不参与战斗逻辑，因此直接用 NewGuid。
    // -----------------------------------------------------------------------
    OutSetup.BattleId = FGuid::NewGuid();

    BuildLaunchOptions(Config, OutOptions);

    // -----------------------------------------------------------------------
    // 2) 我方：只上场干员参战（备战席不参战——《系统结构说明》§4.4）。
    //    属性 = Run 层合成的快照；
    //    战斗内内容 = 强化 / 词条 / 装备的 GE（`Effects`）+ 被动能力（`GrantedAbilities`）
    //                 + 抢攻 / 后发条目（`StartingTimeline`）。
    // -----------------------------------------------------------------------
    const TArray<FName>& ActiveIds = Squad.GetActiveIds();
    for (int32 ActiveIndex = 0; ActiveIndex < ActiveIds.Num(); ++ActiveIndex)
    {
        const FName OperatorId = ActiveIds[ActiveIndex];
        const FACRunOperator* Operator = Squad.Find(OperatorId);
        if (Operator == nullptr)
        {
            continue;
        }

        // 阵亡干员不参战（复活是战后的决定，不是战中的）。
        if (Operator->bDead)
        {
            continue;
        }

        FACPlayerUnitSpec Spec;
        Spec.DefinitionId = Operator->OperatorId;
        Spec.Level = Operator->Level;

        // TierIndex = "个性强化的数值档位"（0=C … 3=S）。
        // 阶段 3.3：它现在只用于**取候选**（`FACRunSquad` 选强化时），
        // 战斗内不再有"按档位取 FACTieredValue"这件事 —— GE 的数值是内容常量。
        Spec.TierIndex = Operator->TierIndex;

        // 基础属性：等级基础值 + 永久修饰（锻体 / 复活惩罚 / 装备属性）。
        Squad.ComputeBaseStats(OperatorId, Spec.BaseStats);

        // 站位：优先用干员记录里的落位（玩家布阵结果）；未布阵时按落位表推导。
        Spec.SpawnCell = Operator->FormationCell;
        Spec.Facing = Operator->Facing;

        // 上一场剩余的**基础血量**持久化（-1 = 本场满血）。
        Spec.CurrentBaseHP = Config.bPersistBaseHP ? Operator->CurrentBaseHP : -1.f;

        // 个性强化与通用词条的战斗内 GE（两条来源合并，顺序固定：强化 → 词条）。
        Squad.CollectUpgradeEffects(OperatorId, Spec.UpgradeEffects);
        // 个性强化与通用词条带来的**被动能力**（阶段 3.3：这是"处刑 / 心流"能保住
        // "触发条件 + 一次性语义"的唯一通道 —— 详见 `FACPlayerUnitSpec::GrantedAbilities`）。
        Squad.CollectGrantedAbilities(OperatorId, Spec.GrantedAbilities);

        // 装备：属性已折进永久修饰（上面 ComputeBaseStats）；
        // 战斗内 GE / 被动能力 / 抢攻后发条目在这里汇总。
        for (const FACRunEquipment& Equipment : EquipmentInventory)
        {
            if (Equipment.EquippedToOperatorId != OperatorId)
            {
                continue;
            }
            Spec.EquipmentEffects.Append(Equipment.Effects);
            // 装备带来的被动能力（金·心流刃常驻段"每次普攻回 2 专注"走这里）。
            Spec.GrantedAbilities.Append(Equipment.GrantedAbilities);

            // 抢攻/后发：TargetUnitIndex 的语义是"参战顺序索引"，正好是本条 spec 的下标，
            // 也就是战斗内 ApplyPreBattleConfiguration 解析时间轴的口径。
            const int32 SpecIndex = OutSetup.PlayerUnits.Num();
            for (const FACRunTimelineGrant& Grant : Equipment.TimelineGrants)
            {
                if (Grant.Effect.Get() == nullptr)
                {
                    // 内容侧没给 GE 类（编辑器里加了一行但没选类）。静默跳过：
                    // 与内核侧 `ApplyPreBattleConfiguration` 对空位的口径一致。
                    continue;
                }
                FACStartingTimelineSpec TimelineSpec;
                TimelineSpec.Effect = Grant.Effect;
                TimelineSpec.bPreemptive = Grant.bPreemptive;
                TimelineSpec.TriggerSeconds = Grant.TriggerSeconds;
                // 阶段 3.3：`DurationSeconds` 字段已删除 —— 时长写在 GE 类上
                //（心流刃抢攻 GE = HasDuration(4s) / 超导线圈后发 GE = HasDuration(5s)）。
                TimelineSpec.TargetUnitIndex = SpecIndex;
                OutSetup.StartingTimeline.Add(TimelineSpec);
            }
        }

        OutSetup.PlayerUnits.Add(Spec);
    }

    // 若全部干员阵亡（理论上会被 ACRunSubsystem 拦住），这里给出明确的空编队告警。
    if (OutSetup.PlayerUnits.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Run] 组装战斗时没有可参战干员（节点 #%d）。"), Node.NodeId);
    }

    // -----------------------------------------------------------------------
    // 3) 敌方：遭遇 → 单位快照
    // -----------------------------------------------------------------------
    for (int32 EnemyIndex = 0; EnemyIndex < Encounter.EnemyDefinitionIds.Num(); ++EnemyIndex)
    {
        OutSetup.EnemyUnits.Add(MakeEnemyUnitSpec(Encounter.EnemyDefinitionIds[EnemyIndex],
                                                  Config, EnemyIndex, Encounter.EnemyEffects));
    }

    (void)Library;   // 属性合成已由 FACRunSquad 通过 UACOperatorLibrary 完成，这里不再重复访问。
}
