// M04 日志落盘实现：格式与取舍见 ACDiagnostics/ACBattleLogWriter.h 顶部说明。
#include "Diagnostics/ACBattleLogWriter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Templates/Sorting.h"

namespace
{
    const TCHAR* const LogSchemaVersion = TEXT("acbattle.log.v4");

#if UE_BUILD_SHIPPING
    /** 落盘开关的默认值：发行构建关闭，避免每场战斗都往玩家磁盘写文件。 */
    bool GFileLoggingEnabled = false;
#else
    /** 落盘开关的默认值：编辑器 / 开发构建打开（本阶段的主要用途就是录基线、做日志对照）。 */
    bool GFileLoggingEnabled = true;
#endif

    /** 把 JSON 对象序列化成一行紧凑 JSON（不带换行；换行即 JSONL 的记录分隔符，由调用方追加）。 */
    bool SerializeCondensed(const TSharedRef<FJsonObject>& Object, FString& OutLine)
    {
        OutLine.Reset();
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutLine);
        return FJsonSerializer::Serialize(Object, Writer, /*bCloseWriter=*/true);
    }

    /** 统计文件用格式化输出：数字与 TMap 嵌套多，单行看不清（JSONL 不受影响）。 */
    bool SerializePretty(const TSharedRef<FJsonObject>& Object, FString& OutText)
    {
        OutText.Reset();
        const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutText);
        return FJsonSerializer::Serialize(Object, Writer, /*bCloseWriter=*/true);
    }

    /** FName 列表 → JSON 字符串数组（单位定义 ID 列表用）。 */
    TArray<TSharedPtr<FJsonValue>> MakeNameArray(const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Names.Num());
        for (const FName& Name : Names)
        {
            Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
        }
        return Values;
    }

    /** 元信息行：一场战斗只写一条，且必须是文件的第一行。 */
    TSharedRef<FJsonObject> BuildMetaObject(const FACBattleLogMetaInput& Meta, const FDateTime& UtcNow,
                                           const FString& FileName, int32 RecordCount, int32 DroppedRecordCount)
    {
        const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("type"), TEXT("meta"));
        Object->SetStringField(TEXT("schema"), LogSchemaVersion);
        Object->SetStringField(TEXT("battleId"), Meta.BattleId);
        Object->SetStringField(TEXT("file"), FileName);
        // UTC 墙钟时间用于"这局是什么时候跑的"；文件名里的时间戳是同一个值，便于交叉核对。
        Object->SetStringField(TEXT("wallClockUtc"), UtcNow.ToIso8601());
        Object->SetNumberField(TEXT("dataVersion"), Meta.DataVersion);
        Object->SetNumberField(TEXT("startTimeSeconds"), Meta.StartTimeSeconds);
        Object->SetNumberField(TEXT("endTimeSeconds"), Meta.EndTimeSeconds);

        Object->SetNumberField(TEXT("battleSeconds"), static_cast<double>(Meta.BattleSeconds));
        Object->SetStringField(TEXT("outcome"), Meta.Outcome);
        Object->SetArrayField(TEXT("playerUnits"), MakeNameArray(Meta.PlayerUnitDefinitionIds));
        Object->SetArrayField(TEXT("enemyUnits"), MakeNameArray(Meta.EnemyUnitDefinitionIds));
        Object->SetNumberField(TEXT("recordCount"), RecordCount);
        Object->SetNumberField(TEXT("droppedRecordCount"), DroppedRecordCount);
        
        return Object;
    }

    /** 一条日志记录行。字段名与 FBattleLogRecord 一一对应，脚本不必再查映射表。 */
    TSharedRef<FJsonObject> BuildEventObject(const FBattleLogRecord& Record)
    {
        const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("type"), TEXT("event"));
        Object->SetNumberField(TEXT("seq"), Record.Sequence);
        Object->SetNumberField(TEXT("time"), static_cast<double>(Record.Time));
        Object->SetStringField(TEXT("category"), Record.Category.ToString());
        Object->SetStringField(TEXT("event"), Record.EventTag.ToString());
        Object->SetNumberField(TEXT("sourceUnitId"), Record.Source);
        Object->SetNumberField(TEXT("targetUnitId"), Record.Target);
        Object->SetStringField(TEXT("effectBlockId"), Record.EffectBlockId.ToString());
        Object->SetNumberField(TEXT("valueA"), Record.ValueA);
        Object->SetNumberField(TEXT("valueB"), Record.ValueB);
        Object->SetNumberField(TEXT("intValue"), Record.IntValue);
        return Object;
    }

    /** 统计快照文件：外层套一层自描述字段，内层直接把 FACBattleStatsSnapshot 交给反射转换。 */
    TSharedRef<FJsonObject> BuildStatsObject(const FACBattleStatsSnapshot& Stats, const FACBattleLogMetaInput* Meta,
                                             const FString& FileName, int32 RecordCount, int32 DroppedRecordCount)
    {
        // TMap 的迭代顺序不稳定（同 §5.1 记的遍历顺序问题）。这里先把 6 张表按 key 排序，
        // 于是"源表遍历顺序 → JSON 插入顺序 → 序列化输出"整条链路都确定下来：
        // 内容相同的两场战斗能产出**逐字节相同**的统计文件，比对脚本不必先做规范化。
        FACBattleStatsSnapshot Sorted = Stats;
        Sorted.KillCountByTag.KeySort(FNameLexicalLess());
        Sorted.AbnormalStacksApplied.KeySort(FNameLexicalLess());
        Sorted.KillsBySourceUnit.KeySort(TLess<int32>());
        Sorted.DamageDealtByUnit.KeySort(TLess<int32>());
        Sorted.DamageTakenByUnit.KeySort(TLess<int32>());
        Sorted.HealingDoneByUnit.KeySort(TLess<int32>());

        const TSharedRef<FJsonObject> StatsObject = MakeShared<FJsonObject>();
        // SkipStandardizeCase 的两个作用（都是为了"键与代码对得上"）：
        //   1) FName 键（State.Bleed / Hook.Battle.End 之类）原样保留，不被改成 state.Bleed；
        //   2) 字段名原样保留，与 FACBattleStatsSnapshot 的字段名逐字对应，脚本不用再查映射表。
        const bool bConverted = FJsonObjectConverter::UStructToJsonObject(
            FACBattleStatsSnapshot::StaticStruct(), &Sorted, StatsObject, 0, 0, nullptr,
            EJsonObjectConversionFlags::SkipStandardizeCase);
        if (!bConverted)
        {
            UE_LOG(LogTemp, Warning, TEXT("[BattleLog] FACBattleStatsSnapshot 转 JSON 失败，统计内容可能为空。"));
        }

        // 反射转换会把结构体所有字段都写出来，因此这里显式抹掉一类字段：
        //   UnitResults（阶段 0b）：逐单位明细已由 _units.json 承载，且两份内容逐字相同。
        //   在 _stats.json 里再写一份，就等于同一事实在磁盘上有两个副本，随时可能自相矛盾；
        //   _stats.json 的定位是"标量统计量 + 6 张聚合表"。
        // 阶段 4（D3）：原来还要抹掉 `FinalStateHash`（随 D3 删除的哈希字段）。
        //   那个字段已从 `FACBattleStatsSnapshot` 上删除，反射转换不会再产出它，
        //   因此这一行也随之删除 —— 对不存在的键调用 RemoveField 是纯噪声。
        StatsObject->RemoveField(TEXT("UnitResults"));

        const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("type"), TEXT("stats"));
        Object->SetStringField(TEXT("schema"), LogSchemaVersion);
        Object->SetStringField(TEXT("battleId"), Meta != nullptr ? Meta->BattleId : FString());
        Object->SetStringField(TEXT("file"), FileName);
        Object->SetNumberField(TEXT("recordCount"), RecordCount);
        Object->SetNumberField(TEXT("droppedRecordCount"), DroppedRecordCount);
        Object->SetObjectField(TEXT("stats"), StatsObject);
        return Object;
    }

    /**
     * 逐单位结果文件：外层套一层自描述字段，内层是 FACUnitBattleResult 的数组。
     * 走 FJsonObjectConverter 而不是手工拼键名：字段名永远与结构体逐字一致，加字段不会漏改这里。
     * 数据来源是 `FACBattleStatsSnapshot::UnitResults`（阶段 0b 收敛，见头文件"数据来源"），
     * 顺序原样沿用（= UBattleWorld::CollectUnitResults 按 UnitId 升序排好的顺序）。
     */
    TSharedRef<FJsonObject> BuildUnitsObject(const TArray<FACUnitBattleResult>& UnitResults,
                                             const FACBattleLogMetaInput* Meta, const FString& FileName)
    {
        TArray<TSharedPtr<FJsonValue>> UnitValues;
        UnitValues.Reserve(UnitResults.Num());

        for (const FACUnitBattleResult& UnitResult : UnitResults)
        {
            const TSharedRef<FJsonObject> UnitObject = MakeShared<FJsonObject>();
            // SkipStandardizeCase：字段名保持 PascalCase（UnitId / DefinitionId / RemainingBaseHP …），
            // 与 _stats.json 的内层命名、以及 C++ 字段名三者一致，脚本不用再查映射表。
            const bool bConverted = FJsonObjectConverter::UStructToJsonObject(
                FACUnitBattleResult::StaticStruct(), &UnitResult, UnitObject, 0, 0, nullptr,
                EJsonObjectConversionFlags::SkipStandardizeCase);
            if (!bConverted)
            {
                // 单条转换失败只丢这一条：整份结果文件作废的代价远大于少一个单位（其余照写）。
                UE_LOG(LogTemp, Warning, TEXT("[BattleLog] FACUnitBattleResult(%d) 转 JSON 失败，该单位不进逐单位产物。"),
                       UnitResult.UnitId);
                continue;
            }
            UnitValues.Add(MakeShared<FJsonValueObject>(UnitObject));
        }

        const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("type"), TEXT("units"));
        Object->SetStringField(TEXT("schema"), LogSchemaVersion);
        Object->SetStringField(TEXT("battleId"), Meta != nullptr ? Meta->BattleId : FString());
        Object->SetStringField(TEXT("file"), FileName);
        Object->SetNumberField(TEXT("unitCount"), UnitValues.Num());
        Object->SetArrayField(TEXT("units"), UnitValues);
        return Object;
    }
}

// ---------------------------------------------------------------------------
// 开关与路径
// ---------------------------------------------------------------------------

bool FACBattleLogWriter::IsFileLoggingEnabled()
{
    return GFileLoggingEnabled;
}

void FACBattleLogWriter::SetFileLoggingEnabled(bool bEnabled)
{
    GFileLoggingEnabled = bEnabled;
}

FString FACBattleLogWriter::GetLogDirectory()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BattleLogs"));
}

FString FACBattleLogWriter::BuildBaseFileName(const FString& BattleId, const FDateTime& UtcNow)
{
    // 全部字段零填充：字典序 = 时间序，Windows 资源管理器与脚本的排序结果一致。
    const FString Stamp = FString::Printf(TEXT("%04d%02d%02dT%02d%02d%02d%03dZ"),
                                          UtcNow.GetYear(), UtcNow.GetMonth(), UtcNow.GetDay(),
                                          UtcNow.GetHour(), UtcNow.GetMinute(), UtcNow.GetSecond(),
                                          UtcNow.GetMillisecond());

    // BattleId 正常是 GUID 的 Digits 形式（纯十六进制），但仍过一遍白名单：
    // 万一将来换成含 '/' ':' 的 ID，不至于写到一个意料之外的路径上去。
    FString SafeId;
    SafeId.Reserve(BattleId.Len());
    for (const TCHAR Ch : BattleId)
    {
        if (FChar::IsAlnum(Ch) || Ch == TEXT('-') || Ch == TEXT('_'))
        {
            SafeId.AppendChar(Ch);
        }
    }
    if (SafeId.IsEmpty())
    {
        SafeId = TEXT("NoBattleId");
    }

    return FString::Printf(TEXT("Battle_%s_%s"), *SafeId, *Stamp);
}

const TCHAR* FACBattleLogWriter::OutcomeToString(EACOutcome Outcome)
{
    switch (Outcome)
    {
    case EACOutcome::Victory:   return TEXT("Victory");
    case EACOutcome::Defeat:    return TEXT("Defeat");
    case EACOutcome::Abandoned: return TEXT("Abandoned");
    case EACOutcome::Timeout:   return TEXT("Timeout");
    case EACOutcome::Error:     return TEXT("Error");
    case EACOutcome::None:
    default:                    return TEXT("None");
    }
}

// ---------------------------------------------------------------------------
// 落盘
// ---------------------------------------------------------------------------

bool FACBattleLogWriter::SaveToDirectory(const TArray<FBattleLogRecord>& Records, int32 DroppedRecordCount,
                                        const FACBattleStatsSnapshot* Stats,
                                        const FACBattleLogMetaInput* Meta,
                                        const FString& InDirectory, const FString& BaseFileName,
                                        const FDateTime& UtcNow, FString& OutLogPath)
{
    OutLogPath.Reset();

    if (BaseFileName.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("[BattleLog] 文件基名为空，本场不落盘。"));
        return false;
    }

    const FString Directory = InDirectory.IsEmpty() ? GetLogDirectory() : InDirectory;

    // 目录可能不存在（首次运行 / Saved 被清过）。MakeDirectory(Tree=true) 在目录已存在时返回 true，
    // 所以这里的判定不会误报；失败只告警 —— 日志写不出来不能反过来打断战斗。
    if (!IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true))
    {
        UE_LOG(LogTemp, Warning, TEXT("[BattleLog] 无法创建日志目录 %s，本场不落盘。"), *Directory);
        return false;
    }

    const FString InPath = FPaths::Combine(Directory, BaseFileName + TEXT(".jsonl"));
    const FString FileName = FPaths::GetCleanFilename(InPath);

    // 记录先攒进 RecordsText：元信息行要等循环跑完才能序列化（丢弃数在循环里还会累加），
    // 但拼装时它仍然排在文件的第一行 —— 脚本读到第一行就能确定"这是哪场战斗、什么口径"。
    FString RecordsText;
    RecordsText.Reserve(Records.Num() * 256 + 512);

    for (const FBattleLogRecord& Record : Records)
    {
        FString Line;
        if (!SerializeCondensed(BuildEventObject(Record), Line))
        {
            // 单条失败不中断整场：记成"丢弃"并继续（日志不完整好过没有日志），丢弃数会写进元信息。
            ++DroppedRecordCount;
            continue;
        }
        RecordsText += Line;
        RecordsText += TEXT("\n");
    }

    FString Buffer;
    Buffer.Reserve(RecordsText.Len() + 1024);
    if (Meta != nullptr)
    {
        FString MetaLine;
        if (SerializeCondensed(BuildMetaObject(*Meta, UtcNow, FileName, Records.Num(), DroppedRecordCount), MetaLine))
        {
            Buffer += MetaLine;
            Buffer += TEXT("\n");
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[BattleLog] 元信息行序列化失败，只写日志记录。"));
        }
    }
    Buffer += RecordsText;

    // ForceUTF8WithoutBOM：带 BOM 的文件会让严格的 JSON 解析器（含多数脚本库）直接报错。
    if (!FFileHelper::SaveStringToFile(Buffer, *InPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogTemp, Warning, TEXT("[BattleLog] 日志写入失败 %s（%d 条记录）。"), *InPath, Records.Num());
        return false;
    }

    // .jsonl 已经落盘：即使统计文件随后失败，这个路径也必须交回调用方（它是主要产物）。
    OutLogPath = InPath;

    bool bAllArtifactsWritten = true;
    if (Stats != nullptr)
    {
        const FString StatsPath = FPaths::Combine(Directory, BaseFileName + TEXT("_stats.json"));
        FString StatsText;
        const TSharedRef<FJsonObject> StatsObject =
            BuildStatsObject(*Stats, Meta, FileName, Records.Num(), DroppedRecordCount);
        if (!SerializePretty(StatsObject, StatsText))
        {
            UE_LOG(LogTemp, Warning, TEXT("[BattleLog] 统计快照序列化失败，跳过 %s。"), *StatsPath);
            bAllArtifactsWritten = false;
        }
        else if (!FFileHelper::SaveStringToFile(StatsText, *StatsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogTemp, Warning, TEXT("[BattleLog] 统计快照写入失败 %s。"), *StatsPath);
            bAllArtifactsWritten = false;
        }
    }

    // 逐单位结果（§8.1 基线要求）。数据来源是 Stats->UnitResults（阶段 0b 收敛）：
    // Stats 为 nullptr 或 UnitResults 为空都视为"没有单位结果" ——
    // 写一个 units 为空的文件只会让比对脚本误以为"这场确实没有参战单位"。
    if (Stats != nullptr && Stats->UnitResults.Num() > 0)
    {
        const FString UnitsPath = FPaths::Combine(Directory, BaseFileName + TEXT("_units.json"));
        FString UnitsText;
        const TSharedRef<FJsonObject> UnitsObject = BuildUnitsObject(Stats->UnitResults, Meta, FileName);
        if (!SerializePretty(UnitsObject, UnitsText))
        {
            UE_LOG(LogTemp, Warning, TEXT("[BattleLog] 逐单位结果序列化失败，跳过 %s。"), *UnitsPath);
            bAllArtifactsWritten = false;
        }
        else if (!FFileHelper::SaveStringToFile(UnitsText, *UnitsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogTemp, Warning, TEXT("[BattleLog] 逐单位结果写入失败 %s。"), *UnitsPath);
            bAllArtifactsWritten = false;
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[BattleLog] 已落盘 %s（%d 条记录，丢弃 %d 条）。"),
           *InPath, Records.Num(), DroppedRecordCount);
    return bAllArtifactsWritten;
}

bool FACBattleLogWriter::Save(const FBattleLogBuffer& Log, const FACBattleStatsSnapshot& Stats,
                              const FACBattleLogMetaInput& Meta, FString& OutLogPath)
{
    // 只取一次时间：文件名时间戳与元信息里的 wallClockUtc 必须是同一个值，否则两者会差几毫秒，
    // "按文件名找内容"的交叉核对就不再成立（毫秒也进了文件名）。
    const FDateTime UtcNow = FDateTime::UtcNow();
    const FString BaseFileName = BuildBaseFileName(Meta.BattleId, UtcNow);
    // 逐单位结果不再单传：SaveToDirectory 自己从 Stats->UnitResults 取（阶段 0b 收敛到一处）。
    return SaveToDirectory(Log.GetAll(), Log.GetDroppedCount(), &Stats, &Meta, GetLogDirectory(),
                           BaseFileName, UtcNow, OutLogPath);
}

// ---------------------------------------------------------------------------
// FBattleLogBuffer::SaveToFile 的实现放在这里（而不是在 ACDiagnostics.h 里内联）：
// 这样 ACDiagnostics.h 无需 include 本文件，避免 Diagnostics ↔ LogWriter 头文件互相包含。
// 缓冲自己拿不到统计快照与元信息，所以这条路径只写日志记录；其余产物请走 FACBattleLogWriter::Save。
// ---------------------------------------------------------------------------

bool FBattleLogBuffer::SaveToFile(const FString& InDirectory, const FString& BaseFileName) const
{
    FString OutLogPath;
    return FACBattleLogWriter::SaveToDirectory(GetAll(), DroppedCount, /*Stats=*/nullptr,
                                               /*Meta=*/nullptr, InDirectory, BaseFileName, FDateTime::UtcNow(),
                                               OutLogPath);
}
