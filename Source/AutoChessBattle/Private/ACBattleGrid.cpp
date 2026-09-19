#include "Grid/ACBattleGrid.h"

void FBattleGrid::Initialize()
{
    Occupants.Init(InvalidUnitId, UACHexGridStatics::Rows * UACHexGridStatics::Cols);
}

int32 FBattleGrid::CellIndex(FACHexCoord Cell)
{
    return static_cast<int32>(Cell.Row) * UACHexGridStatics::Cols + static_cast<int32>(Cell.Col);
}

bool FBattleGrid::IsValidCell(FACHexCoord Cell) const
{
    return UACHexGridStatics::IsValidCell(Cell);
}

EACGridZone FBattleGrid::GetZone(FACHexCoord Cell) const
{
    return UACHexGridStatics::GetZone(Cell);
}

bool FBattleGrid::IsPlayableCell(FACHexCoord Cell) const
{
    return UACHexGridStatics::IsPlayableCell(Cell);
}

void FBattleGrid::SetOccupant(FACHexCoord Cell, FUnitId UnitId)
{
    if (IsValidCell(Cell))
    {
        Occupants[CellIndex(Cell)] = UnitId;
    }
}

void FBattleGrid::ClearOccupant(FACHexCoord Cell)
{
    if (IsValidCell(Cell))
    {
        Occupants[CellIndex(Cell)] = InvalidUnitId;
    }
}

void FBattleGrid::ClearOccupantByUnit(FUnitId UnitId)
{
    for (int32 Index = 0; Index < Occupants.Num(); ++Index)
    {
        if (Occupants[Index] == UnitId)
        {
            Occupants[Index] = InvalidUnitId;
        }
    }
}

FUnitId FBattleGrid::GetOccupant(FACHexCoord Cell) const
{
    return IsValidCell(Cell) ? Occupants[CellIndex(Cell)] : InvalidUnitId;
}

bool FBattleGrid::IsOccupied(FACHexCoord Cell) const
{
    return GetOccupant(Cell) != InvalidUnitId;
}

bool FBattleGrid::FindNearestFreeCell(FACHexCoord Origin, FACHexCoord& OutCell) const
{
    TArray<FACHexCoord> Candidates = UACHexGridStatics::GetCellsInRadius(Origin, UACHexGridStatics::Rows);
    Candidates.RemoveAll([this](const FACHexCoord& Cell)
    {
        return !IsPlayableCell(Cell) || IsOccupied(Cell);
    });

    if (Candidates.Num() == 0)
    {
        return false;
    }

    Candidates.Sort([Origin](const FACHexCoord& A, const FACHexCoord& B)
    {
        const int32 DistanceA = UACHexGridStatics::Distance(Origin, A);
        const int32 DistanceB = UACHexGridStatics::Distance(Origin, B);
        if (DistanceA != DistanceB)
        {
            return DistanceA < DistanceB;
        }
        return (A.Row != B.Row) ? (A.Row < B.Row) : (A.Col < B.Col);
    });

    OutCell = Candidates[0];
    return true;
}

bool FBattleGrid::FindPathToRange(FACHexCoord From, FACHexCoord Target, int32 DesiredRange, TArray<FACHexCoord>& OutPath) const
{
    OutPath.Reset();
    if (!IsValidCell(From) || !IsValidCell(Target))
    {
        return false;
    }
    if (UACHexGridStatics::Distance(From, Target) <= DesiredRange)
    {
        return true; // 已在射程内，无需移动
    }

    const int32 TotalCells = UACHexGridStatics::Rows * UACHexGridStatics::Cols;
    TArray<int32> Previous;
    Previous.Init(INDEX_NONE, TotalCells);

    TArray<FACHexCoord> Queue;
    Queue.Reserve(TotalCells);
    Queue.Add(From);
    Previous[CellIndex(From)] = CellIndex(From);

    FACHexCoord Found = From;
    bool bFound = false;

    int32 Head = 0;
    while (Head < Queue.Num() && !bFound)
    {
        const FACHexCoord Current = Queue[Head++];
        const TArray<FACHexCoord> Neighbors = UACHexGridStatics::GetNeighbors(Current);

        for (const FACHexCoord& Next : Neighbors)
        {
            if (!IsPlayableCell(Next))
            {
                continue;
            }
            const int32 NextIndex = CellIndex(Next);
            if (Previous[NextIndex] != INDEX_NONE)
            {
                continue;
            }
            if (IsOccupied(Next))
            {
                continue;
            }

            Previous[NextIndex] = CellIndex(Current);
            Queue.Add(Next);

            if (UACHexGridStatics::Distance(Next, Target) <= DesiredRange)
            {
                Found = Next;
                bFound = true;
                break;
            }
        }
    }

    if (!bFound)
    {
        return false;
    }

    // 回溯路径。
    TArray<FACHexCoord> Reversed;
    int32 Cursor = CellIndex(Found);
    const int32 StartIndex = CellIndex(From);
    while (Cursor != StartIndex && Cursor != INDEX_NONE)
    {
        const int32 Row = Cursor / UACHexGridStatics::Cols;
        const int32 Col = Cursor % UACHexGridStatics::Cols;
        Reversed.Add(FACHexCoord(Row, Col));
        Cursor = Previous[Cursor];
    }

    for (int32 Index = Reversed.Num() - 1; Index >= 0; --Index)
    {
        OutPath.Add(Reversed[Index]);
    }
    return OutPath.Num() > 0;
}

bool FBattleGrid::MoveUnitOneStep(FUnitId UnitId, FACHexCoord From, FACHexCoord To)
{
    if (!IsValidCell(From) || !IsValidCell(To) || !IsPlayableCell(To) || IsOccupied(To))
    {
        return false;
    }
    ClearOccupant(From);
    SetOccupant(To, UnitId);
    return true;
}

void FBattleGrid::QueryCells(const FACGridQuery& Query, TArray<FACHexCoord>& OutCells) const
{
    OutCells.Reset();
    if (!IsValidCell(Query.Origin))
    {
        return;
    }

    switch (Query.Type)
    {
    case EACGridQueryType::Single:
        OutCells.Add(Query.Origin);
        break;

    case EACGridQueryType::Neighbors1:
        OutCells = UACHexGridStatics::GetNeighbors(Query.Origin);
        break;

    case EACGridQueryType::Neighbors2:
        OutCells = UACHexGridStatics::GetCellsInRadius(Query.Origin, 2);
        break;

    case EACGridQueryType::RadiusN:
        OutCells = UACHexGridStatics::GetCellsInRadius(Query.Origin, FMath::Max(0, Query.Radius));
        break;

    case EACGridQueryType::SameRow:
        OutCells = UACHexGridStatics::GetRow(Query.Origin.Row);
        break;

    case EACGridQueryType::FrontCone:
        OutCells = UACHexGridStatics::GetFrontCone(Query.Origin, Query.Facing, FMath::Max(1, Query.Radius));
        break;

    case EACGridQueryType::WholeBoard:
        OutCells.Reserve(UACHexGridStatics::Rows * UACHexGridStatics::Cols);
        for (int32 Row = 0; Row < UACHexGridStatics::Rows; ++Row)
        {
            for (int32 Col = 0; Col < UACHexGridStatics::Cols; ++Col)
            {
                OutCells.Add(FACHexCoord(Row, Col));
            }
        }
        break;

    default:
        break;
    }

    if (!Query.bIncludeOrigin)
    {
        OutCells.Remove(Query.Origin);
    }
    if (!Query.bIncludeBuffer)
    {
        OutCells.RemoveAll([](const FACHexCoord& Cell)
        {
            return UACHexGridStatics::GetZone(Cell) == EACGridZone::Buffer;
        });
    }
}

FUnitId FBattleGrid::GetFirstOccupiedInRadius(FACHexCoord Origin, int32 Radius) const
{
    const TArray<FACHexCoord> Cells = UACHexGridStatics::GetCellsInRadius(Origin, Radius);
    for (const FACHexCoord& Cell : Cells)
    {
        const FUnitId Occupant = GetOccupant(Cell);
        if (Occupant != InvalidUnitId)
        {
            return Occupant;
        }
    }
    return InvalidUnitId;
}
