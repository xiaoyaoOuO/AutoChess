#include "Scheduler/ACActionScheduler.h"
#include "Battle/ACBattleTime.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
// 阶段 2：显式包含属性集头。虽然 `Battle/ACBattleUnitBase.h` 已经间接带进来了，
// 但本文件现在**真的**用它（`GetAttackSpeedHz(const UACBattleAttributeSet&)` 的解引用），
// 依赖写成显式的一行，将来某个头文件瘦身时不会突然编译不过。
#include "GAS/ACBattleAttributeSet.h"
#include "Stats/ACBattleStats.h"
#include "Core/ACBattleTags.h"

void FActionScheduler::Initialize(UBattleWorld* InWorld)
{
    World = InWorld;
    Slots.Reset();
    SlotIndex.Reset();
    ReadyQueue.Reset();
}

void FActionScheduler::Reset()
{
    Slots.Reset();
    SlotIndex.Reset();
    ReadyQueue.Reset();
}

int32 FActionScheduler::FindSlotIndex(FUnitId UnitId) const
{
    const int32* Found = SlotIndex.Find(UnitId);
    return (Found != nullptr && Slots.IsValidIndex(*Found)) ? *Found : INDEX_NONE;
}

void FActionScheduler::RemoveSlot(FUnitId UnitId)
{
    const int32 Index = FindSlotIndex(UnitId);
    if (Index == INDEX_NONE)
    {
        return;
    }

    Slots.RemoveAt(Index);
    SlotIndex.Remove(UnitId);
    // 修补被删除位置之后的索引。
    for (int32 Tail = Index; Tail < Slots.Num(); ++Tail)
    {
        SlotIndex.Add(Slots[Tail].UnitId, Tail);
    }
}

EACSchedulerGate FActionScheduler::EvaluateGate(const AACBattleUnitBase& Unit)
{
    if (Unit.IsDead() || Unit.IsPendingDeath())
    {
        return EACSchedulerGate::Dead;
    }
    // 阶段 3.2b：判据只剩**标签**一条 —— 旧的 `Unit.IsStunned()`（`bStunned` 成员）
    // 已随 `FAbilityExecutor::ApplyStun` / `FStunEntry` 一起删除。眩晕的唯一真相是
    // ASC 上由 `UACGE_State_Stun`（`HasDuration` + `GrantedTags = State.Stun`）授予的标签，
    // 到期由引擎摘掉 GE、标签自动消失，因此这里**不需要**任何"到期解除"的兜底。
    if (Unit.HasStateTag(BattleTags::State_Stun))
    {
        return EACSchedulerGate::Stunned;
    }
    // 阶段 4：`EACUnitActionState::Channeling` 分支**已删除** —— 枚举项本身消失
    //（引导线的写入口随 `FAbilityExecutor` 删除，全仓没有任何地方写这个取值）。
    // 保留 `Casting`：它是技能能力在 `ActivateAbility` 里真实写入的取值。
    if (Unit.GetActionState() == EACUnitActionState::Casting)
    {
        return EACSchedulerGate::Casting;
    }
    return EACSchedulerGate::None;
}

float FActionScheduler::ComputeGaugeDelta(const AACBattleUnitBase& Unit, float DeltaTime) const
{
    const FACAttackPatternDef& Pattern = Unit.GetAttackPattern();

    if (!Pattern.bUseAttackSpeed)
    {
        // 固定间隔型（蕾拉中尉：攻速固定 50，其余转攻击力）。
        return DeltaTime / FMath::Max(0.01f, Pattern.FixedIntervalSeconds);
    }

    // 阶段 2：攻速从属性集读（公式一字未改，`FBattleStatPipeline::GetAttackSpeedHz` 只换了数据来源）。
    // 属性集是单位的默认子对象，正常不为空；为空时按 0 处理 —— 那会让本单位的行动条完全不动，
    // 是**能立刻被发现**的症状，好过在这里崩溃或悄悄退回旧路径。
    const UACBattleAttributeSet* const Attributes = Unit.GetAttributeSet();
    const float AttackSpeedHz = (Attributes != nullptr ? FBattleStatPipeline::GetAttackSpeedHz(*Attributes) : 0.f)
        * FMath::Max(0.01f, Pattern.AttackSpeedGainScale);
    return DeltaTime * AttackSpeedHz;
}

void FActionScheduler::Advance(float DeltaTime)
{
    ReadyQueue.Reset();
    if (World == nullptr)
    {
        return;
    }

    // 注册表顺序（UnitId 升序）保证遍历确定性。
    // ReadyTime 取一次当前绝对时间：本帧内所有就绪槽共用同一时刻，
    // 逐单位各取一次会让同帧就绪的槽出现微小时间差（日志上看起来像不同帧）。
    const float NowSeconds = FACBattleTime::Now(*World);
    World->ForEachAlive([this, DeltaTime, NowSeconds](AACBattleUnitBase& Unit)
    {
        int32 SlotIdx = FindSlotIndex(Unit.GetUnitId());
        if (SlotIdx == INDEX_NONE)
        {
            FACActionSlot NewSlot;
            NewSlot.UnitId = Unit.GetUnitId();
            NewSlot.Gauge = 0.f;
            SlotIdx = Slots.Add(NewSlot);
            SlotIndex.Add(NewSlot.UnitId, SlotIdx);
        }
        FACActionSlot& Slot = Slots[SlotIdx];

        Slot.Gate = EvaluateGate(Unit);
        if (Slot.Gate != EACSchedulerGate::None || Slot.bReady)
        {
            return;
        }

        // 行动条累积是本阶段**唯一**允许的 `+= DeltaTime` 类成员（§5.2 实现裁决明确保留）：
        // 它是自走棋的核心机制（攒满 1.0 = 获得一次行动），不是时间源，因此不能用世界时间替代。
        Slot.Gauge += ComputeGaugeDelta(Unit, DeltaTime);
        if (Slot.Gauge >= 1.f)
        {
            Slot.Gauge -= 1.f;
            Slot.ReadyTime = NowSeconds;
            Slot.bReady = true;
            ReadyQueue.Add(Unit.GetUnitId());
        }
    });

    // ForEachAlive 按注册表顺序（UnitId 升序）遍历，就绪队列已按 (本帧, UnitId) 升序。
    // 无需再次排序；ReadyTime 记录用于日志/快照。
}

void FActionScheduler::Consume(FUnitId UnitId)
{
    const int32 Index = FindSlotIndex(UnitId);
    if (Index != INDEX_NONE)
    {
        Slots[Index].bReady = false;
    }
}
