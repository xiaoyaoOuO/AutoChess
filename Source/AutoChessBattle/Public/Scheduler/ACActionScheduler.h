// M12 行动调度：唯一时间基准（引擎世界时间，见 Battle/ACBattleTime.h）与就绪队列。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleTypes.h"
#include "Core/ACDataTypes.h"

class UBattleWorld;
class AACBattleUnitBase;

// 阶段 0b（D8）：单位参数一律显式写成 Actor 类型；本头只把 `AACBattleUnitBase&` / `AACBattleUnitBase*`
// 当函数参数用，前向声明足够，不需要完整类型。

// 阶段 0.5（D2 / §5.2）：原来的 `FBattleClock`（整数 tick 时钟）与 `FSecondTickDispatcher`
// （整秒派发器）已整体删除，时间一律走 `FACBattleTime`。"每秒"结算改成"每帧按 DeltaTime 结算"，
// 见 `UBattleWorld::TickFrame`。这里刻意**不留兼容层**：让编译器把所有旧的 tick 调用点暴露出来。

/**
 * 行动门控判定结果。
 *
 * 阶段 4：`Channeling` **已删除** —— 它唯一的产出点是
 * `Unit.GetActionState() == EACUnitActionState::Channeling`，而那个枚举项随引导线一起
 * 从 `EACUnitActionState` 删除（没有任何写入点），该分支恒为假。
 * `Stunned` 保留：它现在的判据是 ASC 上的 `State.Stun` 标签（`UACGE_State_Stun` 授予）。
 */
enum class EACSchedulerGate : uint8
{
    None,
    Dead,
    Stunned,
    Casting
};

/** 单个单位的行动槽（与单位同生命周期）。 */
struct AUTOCHESSBATTLE_API FACActionSlot
{
    FUnitId UnitId = InvalidUnitId;
    float Gauge = 0.f;
    /** 行动槽攒满时的**绝对时间**（`FACBattleTime::Now`）。只用于日志/快照，不参与判定。 */
    float ReadyTime = 0.f;
    bool bReady = false;
    EACSchedulerGate Gate = EACSchedulerGate::None;
};

/** 行动调度器（M12）：按攻速推进各单位行动槽，产出就绪行动队列。 */
class AUTOCHESSBATTLE_API FActionScheduler
{
public:
    void Initialize(UBattleWorld* InWorld);
    void Reset();

    /**
     * 推进行动条并产出就绪队列（按注册表顺序 = UnitId 升序，`ReadyTime` 记录本帧）。
     * @param DeltaTime 本帧增量（秒）。行动条按它累积 —— 这是设计要求的量，
     *                  见 §5.2「实现裁决」：帧增量直接用引擎给的 DeltaTime，不缓存不累加。
     */
    void Advance(float DeltaTime);

    const TArray<FUnitId>& GetReadyQueue() const { return ReadyQueue; }
    void Consume(FUnitId UnitId);
    /** 单位离场时清理其行动槽，避免槽位随召唤物无限增长。 */
    void RemoveSlot(FUnitId UnitId);

    static EACSchedulerGate EvaluateGate(const AACBattleUnitBase& Unit);

private:
    float ComputeGaugeDelta(const AACBattleUnitBase& Unit, float DeltaTime) const;
    int32 FindSlotIndex(FUnitId UnitId) const;

    UBattleWorld* World = nullptr;
    TArray<FACActionSlot> Slots;
    /** UnitId -> Slots 下标，避免每步线性扫描。 */
    TMap<FUnitId, int32> SlotIndex;
    TArray<FUnitId> ReadyQueue;
};
