// M01 战斗流程编排：单场战斗生命周期、阶段机、按帧驱动、结果组装。
// 不含任何战斗规则；关卡加载/卸载留给 Run 层适配（框架版直接创建世界）。
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Core/ACBattleSetup.h"
#include "Core/ACBattleTypes.h"
#include "Battle/ACBattleWorld.h"
#include "ACBattleSession.generated.h"

class UBattleWorld;

/**
 * 单场战斗的性能测量结果（D11 / §6.3，由 `BattlePerfReport` 调试命令打印）。
 * 判定阈值（§6.3）：`SpawnSeconds / max(1, InitialUnitCount) > 0.003`（3 ms/单位），
 * 或 `StepMaxSeconds` 出现可感知尖刺 → 引入对象池。
 */
struct AUTOCHESSBATTLE_API FACBattlePerfStats
{
    /** `UBattleWorld::Initialize` 的总耗时（秒）：建棋盘 + 全部单位 Actor 创建 + ASC 初始化 + 授予。 */
    double InitializeSeconds = 0.0;
    /** 初始化时生成的单位数（`PlayerUnits + EnemyUnits`；战斗中召唤的不计）。 */
    int32 InitialUnitCount = 0;

    /** 战斗中累计生成过的单位数（含召唤物），来自 `UBattleWorld::GetSpawnedUnitCount()`。 */
    int32 TotalSpawnedUnits = 0;

    /** 战斗总帧时（秒）：所有 `World->Step()` 调用耗时的累加。 */
    double StepTotalSeconds = 0.0;
    /** 单帧最长耗时（秒）：判断"有没有可感知尖刺"用的是它。 */
    double StepMaxSeconds = 0.0;
    /** `Step` 调用次数（≈ 战斗经历的真实帧数）。 */
    int64 StepCount = 0;

    /**
     * 战斗期间 `GUObjectArray` 的峰值对象数（"GC 峰值"的可测代理）。
     *
     * 为什么用这个而不是某个 "GCPeak" 计数器：引擎没有暴露"GC 峰值内存"这种量，
     * 而 D11 真正要回答的问题是"单位 Actor 化之后，UObject 数量会不会把 GC 拖爆" ——
     * 峰值存活对象数正是这个问题的分子。进程级物理内存峰值另由
     * `FPlatformMemory::GetStats().PeakUsedPhysical` 在报告时打印（它包含编辑器自身的占用，
     * 只能做量级参考，因此单独标注）。
     */
    int32 PeakObjectCount = 0;

    /** 每单位平均创建 + 初始化耗时（秒）：与 §6.3 的 3 ms 阈值直接比较的就是它。 */
    double GetSecondsPerUnit() const
    {
        return InitialUnitCount > 0 ? InitializeSeconds / static_cast<double>(InitialUnitCount) : 0.0;
    }
};

UCLASS()
class AUTOCHESSBATTLE_API UBattleSession : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FACBattleSetup& InSetup, const FACBattleLaunchOptions& InOptions,
                    const FACBattleDataContext& InDataContext);
    void Shutdown();

    /**
     * 真实帧驱动（阶段 0.5，D2 / §5.2）：**直接**把本帧增量交给 `UBattleWorld::Step`。
     *
     * 为什么这里不再有累积器与追赶循环：那是固定步时钟的产物。
     * §5.2「实现裁决」要求全场只有一个时间源（引擎世界时间），内核自己累加 DeltaTime
     * 就会造出第二个源，二者因帧率与暂停而漂移 —— 正是 D2 要消除的问题。
     * @param RealDeltaSeconds 引擎给的帧增量（秒）；非正数由 `Step` 直接忽略。
     */
    void Tick(float RealDeltaSeconds);

    void RequestAbandon();
    void PushCommand(const FACBattleCommand& Command);

    bool BuildResult(FACBattleResult& OutResult);

    /** 取走已构建的结果（一次性）；由 UBattleSubsystem 使用，避免重复构建。 */
    bool TryConsumeBuiltResult(FACBattleResult& OutResult);
    bool HasResult() const { return bResultReady; }

    EACPhase GetPhase() const { return Phase; }
    /** 注意：不能命名为 GetWorld()——UObject::GetWorld() 返回 UWorld*，签名冲突会导致 C2555。 */
    UBattleWorld* GetBattleWorld() const { return World; }
    bool IsFinished() const { return Phase == EACPhase::Result || Phase == EACPhase::Teardown; }

    /** 性能测量结果（D11 / §6.3）：`BattlePerfReport` 的唯一数据来源。 */
    const FACBattlePerfStats& GetPerfStats() const { return PerfStats; }

private:
    bool ValidateSetup(FString& OutError) const;
    void EnterPhase(EACPhase NextPhase);
    void TickPhaseMachine();

    FACBattleSetup Setup;
    FACBattleLaunchOptions Options;
    FACBattleDataContext DataContext;

    UPROPERTY()
    TObjectPtr<UBattleWorld> World = nullptr;

    EACPhase Phase = EACPhase::None;
    // 阶段 0.5（D2）：`FixedDt` / `RealAccumulator` 已删除 —— 不再有固定步累积器与追赶循环。
    bool bResultReady = false;
    FACBattleResult CachedResult;

    /** D11 性能测量（§6.3）：初始化耗时 / 战斗总帧时 / 对象数峰值。 */
    FACBattlePerfStats PerfStats;
};
