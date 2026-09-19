// Run 层：战后流转（把战斗结果写回局内状态）。
//
// 战斗内核对外只有一个出口：`FACBattleResult`（见 Core/ACBattleSetup.h）。
// 本文件负责把它翻译成局内后果，顺序严格按《战斗系统详细设计》与策划文档：
//
//   ① 血量持久化：每名干员记录结束时的**基础血量**（不是当前血量，也不是临时护盾）
//   ② 阵亡标记：阵亡干员进入"待复活"名单（复活由玩家在战后决定，见 §5.13）
//   ③ 奖励结算：魂晶（战斗内产出 + 节点奖励）、装备掉落（遗物/精英/BOSS）
//   ④ 失败处理：全部参战干员同时阵亡 → 本局结束（《系统结构说明》§1.2 的失败条件）
//
// 这一层**不修改战斗世界**（战斗已经结束并 Shutdown），也不做任何数值计算——
// 数值全部来自 FACBattleResult 与 FACRunConfig。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleSetup.h"
#include "Run/ACRunTypes.h"
// 必须包含：FACRunBattleOutcome 是 USTRUCT，其反射样板由这个头文件提供（见 ACRunSquad.h 的同类说明）。
#include "ACRunPostBattle.generated.h"

class UACRunConfig;
class UACBattleContentLibrary;
class FACRunEconomy;            // 见 ACRunEconomy.h：经济系统是带逻辑的类
class FACRunSquad;              // 见 ACRunSquad.h：编队是带逻辑的类，不是 USTRUCT
struct FACRunEquipment;
struct FACRunMapNode;

/** 战后处理结果（供 UI / 日志 / 自动演示决策）。 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunBattleOutcome
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Battle")
    bool bVictory = false;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Battle")
    EACRunNodeType NodeType = EACRunNodeType::Combat;

    /** 节点奖励 + 战斗内产出的魂晶合计。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Battle")
    int32 SoulCrystalGained = 0;

    /** 魂晶来源拆分：战斗内效果块产出（GrantSoulCrystal 动作）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Battle")
    int32 SoulCrystalFromBattle = 0;

    /** 魂晶来源拆分：节点胜利/事件奖励。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Battle")
    int32 SoulCrystalFromNode = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Battle")
    TArray<FACRunEquipment> Loot;

    /** 本场结束后处于阵亡状态的干员。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Battle")
    TArray<FName> DeadOperatorIds;

    /** 全队阵亡（= 本局失败）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Battle")
    bool bSquadWiped = false;

    /** 本场战斗时长（战斗内相对时间，秒）。阶段 0.5：由 `int64 BattleTicks` 改成秒。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Battle")
    float BattleSeconds = 0.f;

    // 阶段 4（D3 / §5.3）：`int64 StateHash` 字段**已删除**。
    // 它是"确定性复现"的产物（旧 `FACBattleResult::Log.FinalStateHash` 的转发），
    // 随 `UBattleWorld::ComputeStateHash` 与 `FACBattleLogMeta` 的哈希字段一起退场。
    // 本结构里其它字段（胜负 / 魂晶来源拆分 / 掉落 / 阵亡名单 / 时长）全部保留。
};

class AUTOCHESS_API FACRunPostBattle
{
public:
    /**
     * 应用战斗结果。
     * @param Result              战斗内核的唯一输出
     * @param Node                本次节点（决定奖励幅度与掉落）
     * @param Squad               编队（会被写入血量与阵亡标记）
     * @param Economy             经济（会被写入魂晶）
     * @param Config              参数表
     * @param Content             内容库（掉落池）
     * @param Rng                 奖励掷骰用的随机流（由 Run 层持有，保证可复现）
     * @param UnitsToOperators   参战顺序 → 干员 ID（与 FACBattleSetup::PlayerUnits 同序）
     */
    static FACRunBattleOutcome ApplyResult(const FACBattleResult& Result,
                                           const FACRunMapNode& Node,
                                           FACRunSquad& Squad,
                                           FACRunEconomy& Economy,
                                           const UACRunConfig& Config,
                                           const UACBattleContentLibrary& Content,
                                           FRandomStream& Rng,
                                           const TArray<FName>& UnitsToOperators);
};
