#include "Battle/Components/ACUnitGridComponent.h"
#include "Battle/ACBattleBoardActor.h"
#include "GameFramework/Actor.h"

UACUnitGridComponent::UACUnitGridComponent()
{
    // 位置只在 SetCell / SetOnBoard 时变化，不需要每帧 tick（逻辑仍由 UBattleWorld::Step 驱动）。
    PrimaryComponentTick.bCanEverTick = false;
}

void UACUnitGridComponent::SetBoardActor(AACBattleBoardActor* InBoardActor)
{
    BoardActor = InBoardActor;

    // 补一次同步：调用方可能先落格、后注入棋盘（阶段 0b 的池化复活路径就是这种顺序）。
    if (bOnBoard)
    {
        SyncOwnerLocationToCell();
    }
}

void UACUnitGridComponent::SetCell(FACHexCoord InCell)
{
    Cell = InCell;
    SyncOwnerLocationToCell();
}

void UACUnitGridComponent::SetOnBoard(bool bInOnBoard)
{
    bOnBoard = bInOnBoard;
}

void UACUnitGridComponent::SyncOwnerLocationToCell()
{
    // 没有棋盘就只记录格子：世界坐标无从换算，位置保持不动（降级可用）。
    if (BoardActor == nullptr)
    {
        return;
    }

    if (AActor* Owner = GetOwner())
    {
        Owner->SetActorLocation(BoardActor->CellToWorld(Cell));
    }
}
