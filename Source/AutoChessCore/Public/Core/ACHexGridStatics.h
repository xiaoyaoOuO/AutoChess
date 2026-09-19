// 六边形棋盘几何（even-r offset + cube）。
// 供战斗内核与 Run 层布阵 UI 共享，禁止在别处复制坐标数学。
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Core/ACBattleTypes.h"
#include "ACHexGridStatics.generated.h"

UENUM(BlueprintType)
enum class EACGridZone : uint8
{
    EnemyDeploy,
    Buffer,
    PlayerDeploy,
    Invalid
};

/** 六边形棋盘几何静态工具：合法性、区域、距离、邻格与坐标转换（纯函数，无运行时状态）。 */
UCLASS()
class AUTOCHESSCORE_API UACHexGridStatics : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    static constexpr int32 Rows = 10;
    static constexpr int32 Cols = 8;

    UFUNCTION(BlueprintPure, Category = "Battle|Hex")
    static bool IsValidCell(FACHexCoord Cell);

    UFUNCTION(BlueprintPure, Category = "Battle|Hex")
    static EACGridZone GetZone(FACHexCoord Cell);

    /** 非缓冲排且合法 = 可停留。 */
    UFUNCTION(BlueprintPure, Category = "Battle|Hex")
    static bool IsPlayableCell(FACHexCoord Cell);

    UFUNCTION(BlueprintPure, Category = "Battle|Hex")
    static int32 Distance(FACHexCoord A, FACHexCoord B);

    UFUNCTION(BlueprintPure, Category = "Battle|Hex")
    static TArray<FACHexCoord> GetNeighbors(FACHexCoord Cell);

    /** 正前方：朝向轴上最近的一格（Up = Row 减小），并列时取列差最小者。 */
    UFUNCTION(BlueprintPure, Category = "Battle|Hex")
    static FACHexCoord GetFrontCell(FACHexCoord Cell, EACFacing Facing);

    UFUNCTION(BlueprintPure, Category = "Battle|Hex")
    static FACHexCoord GetBackCell(FACHexCoord Cell, EACFacing Facing);

    /** 半径 N 的六边形区域（含内部）。 */
    UFUNCTION(BlueprintPure, Category = "Battle|Hex")
    static TArray<FACHexCoord> GetCellsInRadius(FACHexCoord Origin, int32 Radius);

    UFUNCTION(BlueprintPure, Category = "Battle|Hex")
    static TArray<FACHexCoord> GetRow(int32 Row);

    /** 朝向锥形：前方 1..Length 格及其相邻列。 */
    UFUNCTION(BlueprintPure, Category = "Battle|Hex")
    static TArray<FACHexCoord> GetFrontCone(FACHexCoord Origin, EACFacing Facing, int32 Length);

    // ---- 坐标转换（even-r offset <-> cube）----
    static void ToCube(FACHexCoord Cell, int32& OutX, int32& OutY, int32& OutZ);
    static FACHexCoord FromCube(int32 X, int32 Y, int32 Z);
};
