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
    virtual void BeginPlay() override;
    virtual void SetupInputComponent() override;

    /** 打印完整状态到屏幕与控制台日志。 */
    UFUNCTION(BlueprintCallable, Category = "Run")
    void PrintRunStatus();

    // ---- 控制台命令（AutoChess.Run.*）----

    /** 打印状态。 */
    UFUNCTION(Exec, Category = "AutoChess|Run")
    void RunStatus();

    /** 推进一个自动演示动作（与自动演示共用同一套内部逻辑）。 */
    UFUNCTION(Exec, Category = "AutoChess|Run")
    void RunStep();

    /** 开/关自动演示：RunAuto 1 / RunAuto 0。 */
    UFUNCTION(Exec, Category = "AutoChess|Run")
    void RunAuto(const FString& Args);

    /** 直接开一场战斗（不经过地图）：用于单独验证 AutoChessBattle 的链路。 */
    UFUNCTION(Exec, Category = "AutoChess|Run")
    void RunBattle();

    /** 开新局：RunStart [Seed]。 */
    UFUNCTION(Exec, Category = "AutoChess|Run")
    void RunStart(const FString& Args);

    /** 放弃当前战斗。 */
    UFUNCTION(Exec, Category = "AutoChess|Run")
    void RunAbandon();

    // ---- 战斗调试命令（落盘 / 基线录制，见 GAS 重构实施方案 §8.1）----

    /**
     * 录制行为基线：在**当前局内**沿地图推进到下一个战斗节点并开战，
     * 战斗结束后由 UBattleSession::BuildResult 把结构化日志写到 Saved/BattleLogs/。
     *
     * 控制台输入：BattleDumpBaseline
     * 注意：引擎按**裸函数名**分发 exec 函数（见 UObject::CallFunctionByNameWithArguments），
     * 带点号的 "Battle.DumpBaseline" 不是合法函数名，因此这个入口由 BattleDumpBaseline 承载。
     */
    UFUNCTION(Exec, Category = "AutoChess|Battle")
    void BattleDumpBaseline();

    /**
     * 导出当前战斗世界里**每个单位 ASC 的活动 GE 明细**（阶段 4 §7 的 `BattleDumpGAS`）。
     *
     * 为什么需要它：GE 是"现在到底有什么在生效"的唯一事实源，而它住在引擎的
     * `FActiveGameplayEffectsContainer` 里 —— 控制台里看不到。三份日志产物记的是
     * "发生了什么"（伤害 / 状态 / 死亡），本命令补的是"**此刻**身上挂着什么"
     *（剩余时长 / 层数 / 来源），两者合起来才够排障。
     *
     * 输出内容（每单位一段）：
     *   · 单位标识：`UnitId` / `DefinitionId` / 队伍与类别；
     *   · 已授予能力清单（`GetActivatableAbilities()`）—— 判断"被动到底授没授上"看这里；
     *   · 每条活动 GE：GE 类名、层数（`GetStackCount`）、剩余秒数（`HasDuration` 才有）、
     *     授予标签（`GrantedTags`）—— 抢攻是否在生效就是看有没有 `Effect.Trigger.Preemptive`。
     */
    UFUNCTION(Exec, Category = "AutoChess|Battle")
    void BattleDumpGAS();

    /**
     * 导出当前战斗世界里的**单位清单与状态**（阶段 4 §7 的 `BattleDumpUnits`）。
     *
     * 输出内容（每单位一行）：`UnitId` / `DefinitionId` / 队伍 / 类别 / 格子 / **世界坐标**
     * / 生命比例 / 护盾 / 专注 / 精神 / 行动状态 / 当前目标 / 三个组件的就绪情况
     *（网格组件是否拿到棋盘、表现组件是否有效、ASC 是否已 `InitAbilityActorInfo`）。
     *
     * "Actor 化之后位置到底对不对"这类问题（网格 → 世界坐标的变换、单位是否掉出棋盘）
     * 只有把世界坐标打出来才能一眼判断 —— 日志里的格子坐标（`Row/Col`）看不出这个。
     */
    UFUNCTION(Exec, Category = "AutoChess|Battle")
    void BattleDumpUnits();

    /**
     * 打印 D11 性能测量结果（§6.3 的 `BattlePerfReport`）。
     *
     * 三个指标与判定阈值：
     *   · **Actor 创建 + ASC 初始化**：`UBattleWorld::Initialize` 的总耗时与**每单位**平均值 ——
     *     §6.3 的阈值是"每单位 > 3 ms → 引入对象池"；
     *   · **战斗总帧时**：`World->Step()` 的累计耗时 + 单帧最长耗时（尖刺看这一列）；
     *   · **GC 峰值**：战斗期间 `GUObjectArray` 的峰值对象数（另有进程物理内存峰值作量级参考）。
     * 测点在 `UBattleSession`（`Initialize` / `Tick`）里，**不在 `UBattleWorld::Step` 内部** ——
     * §2.3 的行动执行流程一行未动。
     */
    UFUNCTION(Exec, Category = "AutoChess|Battle")
    void BattlePerfReport();

private:
    UACRunSubsystem* GetRunSubsystem() const;

    /**
     * 取"当前正在跑的那场战斗"的世界（没有战斗时返回 nullptr）。
     * 路径：`UGameInstance → UBattleSubsystem → UBattleSession → UBattleWorld`。
     * 三条调试命令共用它，免得同一段"逐级判空"抄三遍。
     */
    class UBattleWorld* GetActiveBattleWorld() const;

    /** F4 快捷键入口（exec 函数带 FString 参数，不能直接绑按键）。 */
    void RunStartDefault();
};
