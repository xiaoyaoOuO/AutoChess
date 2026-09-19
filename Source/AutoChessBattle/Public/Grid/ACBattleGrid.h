// M05 六边形棋盘与空间查询：占位、路径、形状查询。
// 几何数学一律走 UACHexGridStatics；本类只管理运行时状态。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleTypes.h"
#include "Core/ACHexGridStatics.h"

enum class EACGridQueryType : uint8
{
    Single,
    Neighbors1,
    Neighbors2,
    RadiusN,
    SameRow,
    FrontCone,
    WholeBoard
};

/** 一次棋盘形状查询请求（单格/邻域/半径/整行/锥形等）。 */
struct AUTOCHESSBATTLE_API FACGridQuery
{
    FACHexCoord Origin;
    EACGridQueryType Type = EACGridQueryType::Single;
    int32 Radius = 1;
    EACFacing Facing = EACFacing::Up;
    bool bIncludeOrigin = true;
    bool bIncludeBuffer = false;
};

/** 运行时网格。单位占用以 FUnitId 记录；0 = 空。 */
class AUTOCHESSBATTLE_API FBattleGrid
{
public:
    void Initialize();

    static int32 CellIndex(FACHexCoord Cell);

    bool IsValidCell(FACHexCoord Cell) const;
    EACGridZone GetZone(FACHexCoord Cell) const;
    bool IsPlayableCell(FACHexCoord Cell) const;

    void SetOccupant(FACHexCoord Cell, FUnitId UnitId);
    void ClearOccupant(FACHexCoord Cell);
    void ClearOccupantByUnit(FUnitId UnitId);
    FUnitId GetOccupant(FACHexCoord Cell) const;
    bool IsOccupied(FACHexCoord Cell) const;

    /** 距离目标最近的空闲可停留格（并列取 (Row,Col) 最小）。 */
    bool FindNearestFreeCell(FACHexCoord Origin, FACHexCoord& OutCell) const;

    /**
     * BFS 最短路径，走到"距 Target <= DesiredRange"的格子。
     * 返回 true 时 OutPath 为从 From 出发的逐步格子（不含 From）；已在射程内返回 true + 空路径。
     */
    bool FindPathToRange(FACHexCoord From, FACHexCoord Target, int32 DesiredRange, TArray<FACHexCoord>& OutPath) const;

    /** 移动一格：成功返回新格（同时更新占用）。 */
    bool MoveUnitOneStep(FUnitId UnitId, FACHexCoord From, FACHexCoord To);

    void QueryCells(const FACGridQuery& Query, TArray<FACHexCoord>& OutCells) const;

    /** 全场范围内距原点最近的敌方占位（用于补位/密集区判断的辅助）。 */
    FUnitId GetFirstOccupiedInRadius(FACHexCoord Origin, int32 Radius) const;

private:
    TArray<FUnitId> Occupants;   // 长度 Rows * Cols
};
