
// 本阶段只实现数学：不接网格状态（那是 FBattleGrid 的职责），也不挂调试绘制组件（阶段 0c）。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/ACBattleTypes.h"
#include "ACBattleBoardActor.generated.h"

UCLASS()
class AUTOCHESSBATTLE_API AACBattleBoardActor : public AActor
{
    GENERATED_BODY()

public:
    AACBattleBoardActor();

    /** 棋盘原点：格 (0,0) 的格心落在世界坐标的哪里。 */
    UPROPERTY(EditAnywhere, Category="Battle|Board")
    FVector GridOrigin = FVector::ZeroVector;

    /** 六边形外接圆半径（格心到顶点的距离）。 */
    UPROPERTY(EditAnywhere, Category="Battle|Board")
    float HexSize = 100.f;

    /** 棋盘绕 Z 轴的旋转（度）。0 = 行方向沿世界 +X、列方向沿世界 +Y。 */
    UPROPERTY(EditAnywhere, Category="Battle|Board")
    float GridYaw = 0.f;

    /** 六边形偏移坐标 → 世界坐标（even-r 偶行右偏，与 UACHexGridStatics 的口径一致）。 */
    UFUNCTION(BlueprintCallable, Category="Battle|Board")
    FVector CellToWorld(FACHexCoord Cell) const;

    /** 世界坐标 → 最近的六边形格（CellToWorld 的逆运算）。 */
    UFUNCTION(BlueprintCallable, Category="Battle|Board")
    FACHexCoord WorldToCell(const FVector& WorldLocation) const;
};
