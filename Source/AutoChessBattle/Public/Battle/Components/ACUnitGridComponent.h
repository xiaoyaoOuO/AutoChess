// 阶段 0a 新增（GAS 重构实施方案 §2.1 / §2.2）：
// 单位在棋盘上的位置载体。**"网格坐标 ↔ 世界坐标"的唯一出口**：
// 除本组件与 AACBattleBoardActor 之外，任何类都禁止自行做六边形/世界坐标换算（§2.1「禁止各单位自行换算」）。
// 阶段 0a 只做"记录格子 + 有棋盘时同步 Owner 的世界位置"，不接调试绘制、不接寻路。
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/ACBattleTypes.h"
#include "ACUnitGridComponent.generated.h"

class AACBattleBoardActor;

UCLASS(ClassGroup=(AutoChess), meta=(BlueprintSpawnableComponent))
class AUTOCHESSBATTLE_API UACUnitGridComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UACUnitGridComponent();

    /**
     * 注入棋盘 Actor（由 UBattleWorld 在生成单位之后调用）。
     * 为什么允许为空：没有棋盘时应"降级可用"——只记录格子，不换算、不动位置，而不是崩溃。
     */
    void SetBoardActor(AACBattleBoardActor* InBoardActor);

    FORCEINLINE FACHexCoord GetCell() const { return Cell; }

    /** 记录格子；若已注入棋盘，顺带把 Owner 的世界位置同步到该格中心。 */
    void SetCell(FACHexCoord InCell);

    FORCEINLINE bool IsOnBoard() const { return bOnBoard; }
    void SetOnBoard(bool bInOnBoard);

    FORCEINLINE AACBattleBoardActor* GetBoardActor() const { return BoardActor; }

protected:
    /** 当前所在格（偏移坐标）。 */
    UPROPERTY(VisibleAnywhere, Category="Battle|Grid")
    FACHexCoord Cell;

    /** 是否在场（与单位自身的 bOnBoard 镜像；阶段 0b 由本组件接管为唯一出口）。 */
    UPROPERTY(VisibleAnywhere, Category="Battle|Grid")
    bool bOnBoard = false;

    /** 棋盘 Actor（可空）。UPROPERTY 保证棋盘被销毁后引用自动置空，不会悬垂。 */
    UPROPERTY()
    TObjectPtr<AACBattleBoardActor> BoardActor = nullptr;

private:
    /** 把 Owner 的 ActorLocation 对齐到 Cell 对应的世界坐标；无棋盘时直接返回。 */
    void SyncOwnerLocationToCell();
};
