// Run 层：地图与路线（《自走棋系统结构说明》§5.1）。
//
// 本文件负责"玩家在哪一格、能去哪一格"，不负责"格子上发生什么"——后者是 ACRunSubsystem
// 的节点事件分发（战斗/商人/篝火/遗物…）。
//
// 数据与算法分离：
//   - `FACRunMapNode`（见 ACRunTypes.h）是**唯一的地图数据**，生成器与手写资产产出同一结构；
//   - 生成算法只依赖 `FRandomStream` + `UACRunConfig`，因此 **同种子必然同地图**（可复现，批测友好）。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleTypes.h"
#include "Run/ACRunTypes.h"

class UACRunConfig;
struct FACRunMapNode;

/**
 * 地图运行时视图：持有状态、提供查询与推进。
 * 生成与推进都写成静态/成员函数而非隐藏在图结构里，是为了让"地图"可以被完整序列化（存档/回放）。
 */
struct AUTOCHESS_API FACRunMap
{
    /** 全部节点，下标即 NodeId。 */
    TArray<FACRunMapNode> Nodes;

    /** 起点节点 Id（Row 0）。 */
    int32 StartNodeId = INDEX_NONE;

    /** 当前所在节点；INDEX_NONE = 还没上路。 */
    int32 CurrentNodeId = INDEX_NONE;

    /** 本局最大行号（= BossRow）。 */
    int32 MaxRow = 0;

    void Reset();

    /**
     * 生成一张地图。保证：
     *   - Row 0 为唯一入口，Row MaxRow 为唯一 BOSS；
     *   - 每个节点至少有一条入边与一条出边（除入口/终点），不存在死路与孤岛；
     *   - 同 (Seed, Config) 必得同一张图。
     */
    void Generate(int32 Seed, const UACRunConfig& Config);

    const FACRunMapNode* Find(int32 NodeId) const;

    /** 可否从当前节点走到 NodeId（入口节点在 CurrentNodeId == INDEX_NONE 时可选）。 */
    bool CanMoveTo(int32 NodeId) const;

    /** 推进到指定节点；返回 false 表示不可达（调用方应忽略该输入）。 */
    bool MoveTo(int32 NodeId);

    /** 当前可选的下一跳（已按 NodeId 升序，可直接给 UI 或自动演示按序取用）。 */
    void GetSelectableNodes(TArray<int32>& OutNodeIds) const;

    /** 刷新 bVisible（当前行 + Config.VisibleRowsAhead 行）。 */
    void RefreshVisibility(const UACRunConfig& Config);

    /** 当前行号；未上路返回 0。 */
    int32 GetCurrentRow() const;

    /** 当前节点（未上路返回 nullptr）。 */
    const FACRunMapNode* GetCurrentNode() const { return Find(CurrentNodeId); }

    /** 是否是终点（BOSS）。 */
    bool IsAtBoss() const;

    /** 调试/日志用的单行描述。 */
    FString ToDebugString() const;

private:
    int32 AddNode(int32 Row, int32 SlotInRow, EACRunNodeType Type, int32 Seed);
    void LinkNodes(int32 FromId, int32 ToId);
};

namespace ACRunNodeType
{
    /** 节点类型的中文显示名。 */
    AUTOCHESS_API FText ToDisplayText(EACRunNodeType Type);

    /** 该节点类型的默认抽取权重（配置未覆盖时使用）。 */
    AUTOCHESS_API void BuildDefaultWeights(TMap<EACRunNodeType, float>& OutWeights);
}
