#include "Core/ACHexGridStatics.h"

namespace
{
    // even-r offset：偶数行向右错位。
    // x = col - (row + (row & 1)) / 2 ; z = row ; y = -x - z
    const int32 CubeDirX[6] = {  1,  1,  0, -1, -1,  0 };
    const int32 CubeDirY[6] = {  0, -1, -1,  0,  1,  1 };
    const int32 CubeDirZ[6] = { -1,  0,  1,  1,  0, -1 };
}

bool UACHexGridStatics::IsValidCell(FACHexCoord Cell)
{
    return Cell.Row >= 0 && Cell.Row < Rows && Cell.Col >= 0 && Cell.Col < Cols;
}

EACGridZone UACHexGridStatics::GetZone(FACHexCoord Cell)
{
    if (!IsValidCell(Cell))
    {
        return EACGridZone::Invalid;
    }
    if (Cell.Row <= 3)
    {
        return EACGridZone::EnemyDeploy;
    }
    if (Cell.Row <= 5)
    {
        return EACGridZone::Buffer;
    }
    return EACGridZone::PlayerDeploy;
}

bool UACHexGridStatics::IsPlayableCell(FACHexCoord Cell)
{
    const EACGridZone Zone = GetZone(Cell);
    return Zone == EACGridZone::EnemyDeploy || Zone == EACGridZone::PlayerDeploy;
}

void UACHexGridStatics::ToCube(FACHexCoord Cell, int32& OutX, int32& OutY, int32& OutZ)
{
    OutX = static_cast<int32>(Cell.Col) - (static_cast<int32>(Cell.Row) + (static_cast<int32>(Cell.Row) & 1)) / 2;
    OutZ = static_cast<int32>(Cell.Row);
    OutY = -OutX - OutZ;
}

FACHexCoord UACHexGridStatics::FromCube(int32 X, int32 Y, int32 Z)
{
    const int32 Row = Z;
    const int32 Col = X + (Z + (Z & 1)) / 2;
    return FACHexCoord(Row, Col);
}

int32 UACHexGridStatics::Distance(FACHexCoord A, FACHexCoord B)
{
    int32 AX, AY, AZ, BX, BY, BZ;
    ToCube(A, AX, AY, AZ);
    ToCube(B, BX, BY, BZ);
    return (FMath::Abs(AX - BX) + FMath::Abs(AY - BY) + FMath::Abs(AZ - BZ)) / 2;
}

TArray<FACHexCoord> UACHexGridStatics::GetNeighbors(FACHexCoord Cell)
{
    TArray<FACHexCoord> Result;
    Result.Reserve(6);

    int32 X, Y, Z;
    ToCube(Cell, X, Y, Z);
    for (int32 Dir = 0; Dir < 6; ++Dir)
    {
        const FACHexCoord Candidate = FromCube(X + CubeDirX[Dir], Y + CubeDirY[Dir], Z + CubeDirZ[Dir]);
        if (IsValidCell(Candidate))
        {
            Result.Add(Candidate);
        }
    }
    return Result;
}

FACHexCoord UACHexGridStatics::GetFrontCell(FACHexCoord Cell, EACFacing Facing)
{
    const int32 DesiredRow = (Facing == EACFacing::Up) ? (static_cast<int32>(Cell.Row) - 1) : (static_cast<int32>(Cell.Row) + 1);
    FACHexCoord Best = Cell;
    int32 BestColDistance = MAX_int32;

    for (const FACHexCoord& Neighbor : GetNeighbors(Cell))
    {
        if (static_cast<int32>(Neighbor.Row) != DesiredRow)
        {
            continue;
        }
        const int32 ColDistance = FMath::Abs(static_cast<int32>(Neighbor.Col) - static_cast<int32>(Cell.Col));
        if (ColDistance < BestColDistance)
        {
            BestColDistance = ColDistance;
            Best = Neighbor;
        }
    }
    return Best;
}

FACHexCoord UACHexGridStatics::GetBackCell(FACHexCoord Cell, EACFacing Facing)
{
    const EACFacing Opposite = (Facing == EACFacing::Up) ? EACFacing::Down : EACFacing::Up;
    return GetFrontCell(Cell, Opposite);
}

TArray<FACHexCoord> UACHexGridStatics::GetCellsInRadius(FACHexCoord Origin, int32 Radius)
{
    TArray<FACHexCoord> Result;
    const int32 ClampedRadius = FMath::Max(0, Radius);
    Result.Reserve((ClampedRadius * 2 + 1) * (ClampedRadius * 2 + 1));

    for (int32 RowOffset = -ClampedRadius; RowOffset <= ClampedRadius; ++RowOffset)
    {
        for (int32 ColOffset = -ClampedRadius; ColOffset <= ClampedRadius; ++ColOffset)
        {
            const FACHexCoord Candidate(static_cast<int32>(Origin.Row) + RowOffset, static_cast<int32>(Origin.Col) + ColOffset);
            if (IsValidCell(Candidate) && Distance(Origin, Candidate) <= ClampedRadius)
            {
                Result.Add(Candidate);
            }
        }
    }

    Result.Sort([](const FACHexCoord& A, const FACHexCoord& B)
    {
        return (A.Row != B.Row) ? (A.Row < B.Row) : (A.Col < B.Col);
    });
    return Result;
}

TArray<FACHexCoord> UACHexGridStatics::GetRow(int32 Row)
{
    TArray<FACHexCoord> Result;
    if (Row < 0 || Row >= Rows)
    {
        return Result;
    }
    Result.Reserve(Cols);
    for (int32 Col = 0; Col < Cols; ++Col)
    {
        Result.Add(FACHexCoord(Row, Col));
    }
    return Result;
}

TArray<FACHexCoord> UACHexGridStatics::GetFrontCone(FACHexCoord Origin, EACFacing Facing, int32 Length)
{
    TArray<FACHexCoord> Result;

    FACHexCoord Cursor = Origin;
    for (int32 Step = 0; Step < Length; ++Step)
    {
        Cursor = GetFrontCell(Cursor, Facing);
        if (!IsValidCell(Cursor) || Cursor == Origin)
        {
            break;
        }
        Result.AddUnique(Cursor);

        // 锥形宽度：每前进一格向两侧各扩一格（通过邻居展开）。
        TArray<FACHexCoord> Ring = GetNeighbors(Cursor);
        for (const FACHexCoord& Candidate : Ring)
        {
            const bool bOnFacingSide = (Facing == EACFacing::Up)
                ? (Candidate.Row < Cursor.Row || (Candidate.Row == Cursor.Row && Candidate.Col != Origin.Col))
                : (Candidate.Row > Cursor.Row || (Candidate.Row == Cursor.Row && Candidate.Col != Origin.Col));
            if (bOnFacingSide && IsValidCell(Candidate))
            {
                Result.AddUnique(Candidate);
            }
        }
    }

    Result.Sort([](const FACHexCoord& A, const FACHexCoord& B)
    {
        return (A.Row != B.Row) ? (A.Row < B.Row) : (A.Col < B.Col);
    });
    return Result;
}
