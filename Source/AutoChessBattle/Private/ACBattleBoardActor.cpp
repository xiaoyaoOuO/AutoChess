#include "Battle/ACBattleBoardActor.h"
#include "Core/ACHexGridStatics.h"
#include "Components/SceneComponent.h"

AACBattleBoardActor::AACBattleBoardActor()
{
    // §2.2：位置由格坐标控制，棋盘不 tick、不参与碰撞（也不使用导航网格）。
    PrimaryActorTick.bCanEverTick = false;
    SetActorEnableCollision(false);

    // 空根组件：阶段 0c 的调试绘制组件会挂到它下面。
    USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(SceneRoot);
}

FVector AACBattleBoardActor::CellToWorld(FACHexCoord Cell) const
{
    // 推导（不引入第二套约定）：
    //   UACHexGridStatics::ToCube 给出 even-r 偏移：q = col - (row + (row&1))/2，r = row。
    //   pointy-top 六边形的轴向坐标 → 世界坐标是：px = sqrt(3)*Size*(q + r/2)，py = 1.5*Size*r。
    //   把 q、r 代进去正好化简为：
    //       px = sqrt(3) * HexSize * (col - 0.5 * (row & 1))   ← 奇/偶行之间的半格错位
    //       py = 1.5   * HexSize * row
    //   所以下面的实现与 UACHexGridStatics 是同一套口径，半格错位的符号由 ToCube 直接推出。
    const float Sqrt3 = FMath::Sqrt(3.f);
    const float Row = static_cast<float>(Cell.Row);
    const float ColOffset = static_cast<float>(Cell.Col) - 0.5f * static_cast<float>(Cell.Row & 1);

    // 行方向 = 世界 +X，列方向 = 世界 +Y；Z 取棋盘原点高度（§2.1）。
    FVector Local;
    Local.X = Row * (1.5f * HexSize);
    Local.Y = ColOffset * (Sqrt3 * HexSize);
    Local.Z = 0.f;

    return GridOrigin + Local.RotateAngleAxis(GridYaw, FVector::UpVector);
}

FACHexCoord AACBattleBoardActor::WorldToCell(const FVector& WorldLocation) const
{
    // 逆运算用"遍历棋盘范围内所有候选格、取世界距离最近者"实现：
    // 六边形是凸多边形，格心最近即所属格，且不存在四舍五入在错位行上取错的边界情况。
    // 候选格范围直接用 UACHexGridStatics 的棋盘尺寸常量，棋盘 Actor 不再重复定义 Rows/Cols。
    FACHexCoord BestCell(0, 0);
    float BestDistanceSq = TNumericLimits<float>::Max();

    for (int32 Row = 0; Row < UACHexGridStatics::Rows; ++Row)
    {
        for (int32 Col = 0; Col < UACHexGridStatics::Cols; ++Col)
        {
            const FACHexCoord Candidate(Row, Col);
            const float DistanceSq = FVector::DistSquared(WorldLocation, CellToWorld(Candidate));
            if (DistanceSq < BestDistanceSq)
            {
                BestDistanceSq = DistanceSq;
                BestCell = Candidate;
            }
        }
    }

    return BestCell;
}
