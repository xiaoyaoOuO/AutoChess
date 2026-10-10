// Run 层的控制台入口（**开发期的人机接口**，正式版本会被战后 UI 取代）。
//
// 设计意图：让"没有 UI 资产"也能完整操作 Run 层。
// 每个 exec 函数都只是一行转调 UACRunSubsystem 的公开 API —— 控制台、UI、自动化脚本
// 走的是同一组 API，因此不会出现"只有 UI 能触发某条逻辑"的分叉。
//
// 用法（游戏内按 ` 打开控制台）：
//   AutoChess.Run.Status          打印当前状态（阶段/地图/编队/商店）
//   AutoChess.Run.Step            推进一次自动演示的"一个动作"（选路/结算/离开商店）
//   AutoChess.Run.Auto 1          开/关自动演示
//   AutoChess.Run.Battle          直接开一场战斗（跳过地图，用于快速验证战斗内核）
//   AutoChess.Run.Start 12345     用指定种子开新局
//   BattleDumpBaseline            推进到下一个战斗节点并开战，结束后把结构化日志落到 Saved/BattleLogs/
//                                 （方案里的 Battle.DumpBaseline；引擎按裸函数名分发 exec，故不能带点号）
//   BattleDumpGAS                 导出每个单位 ASC 的活动 GE 明细（阶段 4 §7）
//   BattleDumpUnits               导出单位位置 / 组件状态 / UnitId（阶段 4 §7）
//   BattlePerfReport              打印 D11 性能测量结果（Actor 创建 + ASC 初始化耗时 / 战斗总帧时 / GC 峰值）
//
// ⚠️ 上面三条 `Battle*` 命令都**不带点号**：引擎按裸函数名分发 `UFUNCTION(Exec)`
//（`UObject::CallFunctionByNameWithArguments`），`Battle.DumpGAS` 不是合法函数名、分发不到。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ACRunPlayerController.generated.h"

class UACRunSubsystem;

UCLASS()
class AUTOCHESS_API AACRunPlayerController : public APlayerController
{
    GENERATED_BODY()

public:
    virtual void SetupInputComponent() override;

private:
    UACRunSubsystem* GetRunSubsystem() const;

    /**
     * 取"当前正在跑的那场战斗"的世界（没有战斗时返回 nullptr）。
     * 路径：`UGameInstance → UBattleSubsystem → UBattleSession → UBattleWorld`。
     * 三条调试命令共用它，免得同一段"逐级判空"抄三遍。
     */
    class UBattleWorld* GetActiveBattleWorld() const;
};
