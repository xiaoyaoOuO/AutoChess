// M04 日志、调试与批测（框架版）：统计采集 + 结构化日志缓冲。
// 规则：日志失败不阻塞战斗；统计同时服务战斗结果与委托埋点。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleSetup.h"
#include "Core/ACBattleTypes.h"

/** 单条结构化日志记录（后续可序列化为 JSONL）。 */
struct AUTOCHESSBATTLE_API FBattleLogRecord
{
    /** 记录发生的**绝对时间**（秒）；字段名随 tick→秒 一起从 `Tick` 改成 `Time`（§5.2）。 */
    float Time = 0.f;
    int32 Sequence = 0;
    FName Category;
    FName EventTag;
    FUnitId Source = InvalidUnitId;
    FUnitId Target = InvalidUnitId;
    FName EffectBlockId;
    float ValueA = 0.f;
    float ValueB = 0.f;
    int32 IntValue = 0;
};

/** 日志缓冲（内存侧只负责缓存与排序；写盘见 Diagnostics/ACBattleLogWriter.h）。 */
class AUTOCHESSBATTLE_API FBattleLogBuffer
{
public:
    // 默认容量：262144 条（约 15 MB）。
    // 为什么从 16384 抬高：埋点覆盖到"每帧回专注 / 每秒周期伤害"之后，一场 6v6、30 秒的战斗
    // 记录量下界就在 10 万条量级，16384 必然溢出 —— 而日志是放弃确定性（D3）与无头模式（D7）之后
    // 唯一的回归与排障手段（GAS 重构实施方案 §5.3 / §8.2），被截断的基线等于没有基线。
    // 溢出不会静默：DroppedCount 会写进 .jsonl 元信息的 droppedRecordCount，比对前先看它是否为 0。
    void Reset(int32 Capacity = 262144)
    {
        Records.Reset();
        MaxRecords = FMath::Max(64, Capacity);
        Sequence = 0;
        DroppedCount = 0;
    }

    void Record(const FBattleLogRecord& Record)
    {
        if (Records.Num() >= MaxRecords)
        {
            ++DroppedCount;
            return;
        }
        FBattleLogRecord Copy = Record;
        Copy.Sequence = ++Sequence;
        Records.Add(Copy);
    }

    /**
     * 具名辅助：调用方只关心"谁对谁做了什么"，不必逐字段构造 FBattleLogRecord。
     * OccurredAtTime 必须由调用方传入（缓冲自己拿不到时间源，不去猜时间）；
     * 省略时写 0，表示"这条记录没有标记时间"。
     */
    void RecordEvent(FName Category, FName EventTag, FUnitId Source, FUnitId Target,
                     float ValueA = 0.f, float ValueB = 0.f, FName EffectBlockId = NAME_None,
                     float OccurredAtTime = 0.f, int32 IntValue = 0)
    {
        FBattleLogRecord Entry;
        Entry.Time = OccurredAtTime;
        Entry.Category = Category;
        Entry.EventTag = EventTag;
        Entry.Source = Source;
        Entry.Target = Target;
        Entry.EffectBlockId = EffectBlockId;
        Entry.ValueA = ValueA;
        Entry.ValueB = ValueB;
        Entry.IntValue = IntValue;
        // 统一走 Record()：容量上限与序号递增只有一处实现，不会两条路径不一致。
        Record(Entry);
    }

    /**
     * 把当前缓冲写成 <InDirectory>/<BaseFileName>.jsonl（只写日志记录，不写元信息与统计）。
     * 实现在 ACDiagnostics 对应的 LogWriter.cpp 里（避免头文件互相包含）。
     * 本函数**不看落盘开关**（手动调用就是要写，调用方自己用 FACBattleLogWriter::IsFileLoggingEnabled() 判定）；
     * 只有 IO 失败（目录/文件写不了）才告警并返回 false，调用方必须忽略返回值继续跑。
     */
    bool SaveToFile(const FString& InDirectory, const FString& BaseFileName) const;

    const TArray<FBattleLogRecord>& GetAll() const { return Records; }
    int32 GetDroppedCount() const { return DroppedCount; }
    int32 Num() const { return Records.Num(); }

private:
    TArray<FBattleLogRecord> Records;
    int32 MaxRecords = 16384;
    int32 Sequence = 0;
    int32 DroppedCount = 0;
};

/** 战斗统计采集器（委托埋点 + 结果输出）。 */
class AUTOCHESSBATTLE_API FBattleStatsCollector
{
public:
    void Reset()
    {
        Snapshot = FACBattleStatsSnapshot();
    }

    void RecordDamageDealt(FUnitId Source, float Amount)
    {
        if (Source != InvalidUnitId && Amount > 0.f)
        {
            Snapshot.DamageDealtByUnit.FindOrAdd(Source) += Amount;
        }
    }

    void RecordDamageTaken(FUnitId Target, float Amount)
    {
        if (Target != InvalidUnitId && Amount > 0.f)
        {
            Snapshot.DamageTakenByUnit.FindOrAdd(Target) += Amount;
        }
    }

    void RecordHealing(FUnitId Source, float Amount)
    {
        if (Source != InvalidUnitId && Amount > 0.f)
        {
            Snapshot.HealingDoneByUnit.FindOrAdd(Source) += Amount;
        }
    }

    void RecordKill(FUnitId Source, int32 VictimUnitId, const FGameplayTagContainer& VictimTags)
    {
        if (Source != InvalidUnitId)
        {
            Snapshot.KillsBySourceUnit.FindOrAdd(Source) += 1;
        }
        for (const FGameplayTag& Tag : VictimTags)
        {
            Snapshot.KillCountByTag.FindOrAdd(Tag.GetTagName()) += 1;
        }
        (void)VictimUnitId;
    }

    void RecordEnemyMentalBreak() { ++Snapshot.EnemyMentalBreakCount; }

    void RecordAbnormalStacks(FName StateTagName, int32 Stacks)
    {
        if (!StateTagName.IsNone() && Stacks > 0)
        {
            Snapshot.AbnormalStacksApplied.FindOrAdd(StateTagName) += Stacks;
        }
    }

    void AddSoulCrystal(int32 Amount) { Snapshot.SoulCrystalDelta += Amount; }
    void AddSearchableSecret(int32 Count) { Snapshot.SearchableSecretCount += Count; }
    /** 战斗内相对时长（秒）：由 `FACBattleTime::ElapsedSeconds` 提供，不再是 tick 计数。 */
    void SetBattleSeconds(float Seconds) { Snapshot.BattleSeconds = Seconds; }

    /**
     * 逐单位结果的唯一写入口（D8 §5.1）。
     *
     * 为什么用显式 setter，而不是在调用方写 `World->Stats().GetMutableSnapshot().UnitResults = ...`：
     *   1) 结果数组是"整份替换"语义，setter 把这件事写进类型里 —— 调用方不可能只改其中一个元素，
     *      也就不会出现"改了一半、与 UBattleWorld::CollectUnitResults 的排序口径不一致"的中间态；
     *   2) `GetMutableSnapshot()` 是给"散装统计量"（如 BattleSeconds）用的后门，滥用它会绕过所有不变式；
     *      逐单位结果这种有明确生产点的数据，给一个具名入口更不容易被误用。
     * 调用点只有 `UBattleWorld::CollectUnitResults`（生产点）。
     */
    void SetUnitResults(const TArray<FACUnitBattleResult>& InUnitResults)
    {
        Snapshot.UnitResults = InUnitResults;
    }

    const TArray<FACUnitBattleResult>& GetUnitResults() const { return Snapshot.UnitResults; }

    int32 GetKillsBySource(FUnitId Source) const
    {
        const int32* Found = Snapshot.KillsBySourceUnit.Find(Source);
        return Found != nullptr ? *Found : 0;
    }

    float GetDamageDealt(FUnitId Source) const
    {
        const float* Found = Snapshot.DamageDealtByUnit.Find(Source);
        return Found != nullptr ? *Found : 0.f;
    }

    int32 GetSoulCrystalDelta() const { return Snapshot.SoulCrystalDelta; }
    int32 GetSearchableSecretCount() const { return Snapshot.SearchableSecretCount; }

    const FACBattleStatsSnapshot& GetSnapshot() const { return Snapshot; }
    FACBattleStatsSnapshot& GetMutableSnapshot() { return Snapshot; }

    /**
     * 取统计快照的副本。
     *
     * 阶段 4（D3 / §5.3）：形参 `int64 FinalStateHash` **已删除** —— 它唯一的作用就是把哈希
     * 塞进 `FACBattleStatsSnapshot::FinalStateHash`，而那个字段与 `UBattleWorld::ComputeStateHash`
     * 一起退场（C3：不追求确定性）。因此本函数退化成"返回一份副本"。
     * 之所以保留这个具名入口（而不是让调用方直接 `GetSnapshot()`）：调用点写的是
     * "我要一份可用于结果的快照"，这个语义值得有一个名字。
     */
    FACBattleStatsSnapshot ToSnapshot() const
    {
        return Snapshot;
    }

private:
    FACBattleStatsSnapshot Snapshot;
};
