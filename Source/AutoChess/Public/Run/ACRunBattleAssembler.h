// Run 层：战斗编排（把局内状态翻译成战斗输入）。
//
// 这是整条链路上**唯一的翻译层**，也是最值得读的一个文件：
//
//   FACRunMapNode + FACRunSquad + FACRunShopState
//            │  ← 局内状态（跨战斗存活）
//            ▼
//   FACBattleSetup + FACBattleLaunchOptions
//            │  ← 战斗输入快照（只在这一场有效）
//            ▼
//   UBattleSubsystem::StartBattle → UBattleSession → UBattleWorld → …
//
// 三条硬约定：
//   1. **只做翻译，不做判定**：射程/伤害/索敌全部在战斗内核，这里一行规则都不写；
//   2. **快照语义**：战斗开始后 Run 层再改数据，也不会影响本场（这是"事实源唯一"的另一半）；
//   3. **遭遇的确定性**：遭遇由 `RunSeed + NodeId` 派生出的 `Node.EncounterSeed` 决定，
//      因此"同一局同一节点"永远得到同一批敌人。
//      ⚠️ 阶段 4（D3 / §5.3）：这里原来还有半句"战斗内核的 RngSeed 也由它派生" ——
//      **那句已作废**：战斗内核不再有随机流（C3），`FACBattleSetup::RngSeed` 字段已删除。
//      保留下来的只有"遭遇生成"这一层（§5.3 明文保留 `FRandomStream(Node.EncounterSeed)`）。
#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "Templates/SubclassOf.h"
#include "Core/ACBattleSetup.h"
#include "Run/ACRunTypes.h"

class UACRunConfig;
class UACOperatorLibrary;
class UGameplayEffect;
class FACRunSquad;              // 见 ACRunSquad.h：编队是带逻辑的类，不是 USTRUCT
struct FACRunEncounter;
struct FACRunEquipment;
struct FACRunMapNode;

/** 战斗编排静态工具。不持有状态——状态全在 Run 层与战斗内核各自的事实源里。 */
class AUTOCHESS_API FACRunBattleAssembler
{
public:
    /**
     * 节点 → 遭遇（敌人编队）。
     * 完全由 `Node.EncounterSeed` 决定（该值在 `FACRunMap::Generate` 里由 RunSeed + NodeId 派生），
     * 因此"同一局的同一个节点"永远得到同一批敌人。
     * 非战斗节点（商人/篝火/遗物/中转站/起点）返回空遭遇，调用方应跳过战斗。
     */
    static FACRunEncounter GenerateEncounter(const FACRunMapNode& Node);

    /**
     * 组装一场战斗的完整输入。
     * @param Encounter  本次遭遇（由 GenerateEncounter 生成）
     * @param Node       本次节点（决定奖励与先攻；阶段 4 起不再参与战斗种子）
     * @param Squad      编队（只取上场干员；备战席不参战——《系统结构说明》§5.4）
     * @param Config     参数表（落位表、战斗选项）
     * @param Library    干员库（属性合成、职业标签、强化候选）
     * @param EquipmentInventory 装备库存（把已装备项的 GE / 被动能力 / 时间轴条目写进 spec）
     * @param OutSetup   输出：战斗输入快照
     * @param OutOptions 输出：启动选项
     *
     * ⚠️ 阶段 4（D3）：形参 `int32 RunSeed` **已删除**。它在旧实现里唯一的用途是
     *    `HashCombine(RunSeed, Node.EncounterSeed)` 派生战斗内核的 `RngSeed`；
     *    内核不再有随机流、`FACBattleSetup::RngSeed` 字段也没了，这个参数就没有消费者了。
     *    遭遇生成的种子走 `Node.EncounterSeed`（在 `GenerateEncounter` 里用），与此无关。
     */
    static void BuildBattleSetup(const FACRunEncounter& Encounter,
                                 const FACRunMapNode& Node,
                                 const FACRunSquad& Squad,
                                 const UACRunConfig& Config,
                                 const UACOperatorLibrary& Library,
                                 const TArray<FACRunEquipment>& EquipmentInventory,
                                 FACBattleSetup& OutSetup,
                                 FACBattleLaunchOptions& OutOptions);

    /** 造一个敌方单位快照（落位取自 Config 的敌方落位表，越界时顺延）。 */
    static FACEnemyUnitSpec MakeEnemyUnitSpec(FName EnemyDefinitionId,
                                              const UACRunConfig& Config,
                                              int32 EnemyIndex,
                                              const TArray<TSubclassOf<UGameplayEffect>>& EnemyEffects);

    /** 启动选项（由 Config 翻译）。 */
    static void BuildLaunchOptions(const UACRunConfig& Config, FACBattleLaunchOptions& OutOptions);

private:
    /** 我方落位（越界顺延到落位表最后一项）。 */
    static FACHexCoord GetPlayerCell(const UACRunConfig& Config, int32 ActiveIndex);
};
