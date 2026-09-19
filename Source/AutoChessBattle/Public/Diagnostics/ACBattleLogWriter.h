// M04 日志落盘（GAS 重构实施方案 §5.3）：把 FBattleLogBuffer + FACBattleStatsSnapshot 写成磁盘产物。
//
// 为什么要有这个文件：决策 D3（放弃确定性）与 D7（删除无头模式）之后，"跑同一场战斗再比对日志"
// 是唯一的回归与排障手段，而 FBattleLogBuffer 此前只进内存、从不落盘（见 ACBattleDiagnostics.h 的注释）。
// 本阶段只**补上落盘能力**，不改战斗内核的任何既有行为。
//
// 产物（同目录、同基名，外部脚本可按文件名做稳定排序）：
//   Battle_<BattleId>_<UTC 时间戳>.jsonl        第 1 行 = 元信息，其后每行 = 一条日志记录
//   Battle_<BattleId>_<UTC 时间戳>_stats.json   统计快照（格式化 JSON，便于人工阅读）
//   Battle_<BattleId>_<UTC 时间戳>_units.json   逐单位 FACUnitBattleResult（§8.1 的基线锚点）
//
// JSONL 每条记录的字段（与 FBattleLogRecord 一一对应，方便脚本按结构体名取字段）：
//   type          固定 "event"
//   seq           缓冲内递增序号（Record() 内部赋值，跨场不重复）
//   time          记录发生时的**绝对时间（秒）**（0 = 调用方未标记时间）
//   category      分类（例如 "Combat" / "Effect"）
//   event         事件标签（例如 "Damage" / "Death"）
//   sourceUnitId  来源单位（InvalidUnitId = 0 表示无来源）
//   targetUnitId  目标单位（InvalidUnitId = 0 表示无目标）
//   effectBlockId 效果块 ID（无则空串）
//   valueA/valueB 两个浮点载荷（语义由 category/event 决定）
//   intValue      整型载荷（语义由 category/event 决定）
//
// 为什么统计快照单独成一个文件，而不是 JSONL 的末行：
//   1) JSONL 要保持"一行一条同构记录"，比对脚本可以用同一套 schema 逐行解析，不必特判最后一行；
//   2) 统计是嵌套对象（含 6 个 TMap），混进事件流会让"最后一条记录"的 schema 变得不确定；
//   3) 统计文件要格式化输出才方便人工排障，而 JSONL 必须保持单行（换行即记录分隔符）。
//   三份产物共用同一个基名，脚本用 Battle_*.jsonl / Battle_*_stats.json / Battle_*_units.json 配对即可。
//
// 统计文件的字段：{"type":"stats", "schema", "battleId", "file", "recordCount", "droppedRecordCount",
//                  "stats": { …FACBattleStatsSnapshot 的同名字段… }}
//   —— 字段名与结构体字段逐字相同（PascalCase）；阶段 4（D3）起 `FinalStateHash` 字段
//      已从 `FACBattleStatsSnapshot` 上删除，因此文件里也不会再出现它。
//      另外显式抹掉 `UnitResults`（逐单位明细由 _units.json 承载，见下）。
//   —— JSONL 的行内字段用 camelCase（sourceUnitId 等）；两种命名各自自洽，脚本按文件名区分产物即可。
//
// 逐单位文件的字段：{"type":"units", "schema", "battleId", "file", "unitCount",
//                    "units": [ { …FACUnitBattleResult 的同名字段（含 DefinitionId）… } ]}
//   —— 为什么需要第三个产物（§8.1 第 2 项）：行为基线要求导出"逐单位 FACUnitBattleResult"，
//      而 FBattleLogRecord 只有 ValueA / ValueB / IntValue 三个载荷位，装不下
//      RemainingBaseHP / DamageDealt / DamageTaken / HealingDone 四项 —— 硬塞进去只会把
//      日志 schema 撑成"按 category 决定字段语义"的复合体，比对脚本再也没法逐行同构解析。
//      元信息里的双方定义 ID 列表只是**参战名单**，不含任何数值，替代不了这份结果。
//   —— DefinitionId 必须保留：UnitId 是整型句柄（D8 后依然保留，见 §5.1），
//      DefinitionId 是 Run 层回写（ACRunPostBattle 按 DefinitionId 匹配 FACRunOperator）
//      与 Actor 化前后跨版本比对的**唯一稳定锚点**。
//   —— **数据来源（阶段 0b 起）**：不再由调用方单独传数组，改为直接取
//      `FACBattleStatsSnapshot::UnitResults`（唯一权威副本，由 UBattleWorld::CollectUnitResults
//      按 UnitId 升序填充）。这样"结果结构里的 Units"与"_units.json"不可能分叉。
//   —— 顺序稳定性：本文件不排序，原样沿用 `UnitResults` 的顺序，
//      因此**依赖 UBattleWorld::CollectUnitResults 已按 UnitId 升序排序**（该函数末尾有 Sort）。
//
// 失败策略（硬性要求）：目录创建失败 / 写文件失败只记 UE_LOG(Warning) 并返回 false，
// 绝不 check/ensure —— 日志是排障手段，不能反过来打断战斗。
// 返回值口径：只有"本次应产出的文件全部写成功"才返回 true；某一份失败而 .jsonl 成功时返回 false，
// 此时 OutLogPath 仍然有效 —— 所以"有没有日志可看"要看 OutLogPath 是否为空，不要看返回值。
//
// schema 版本：acbattle.log.v4（阶段 0.5 抬升）。历次原因：
//   v1 → v2（阶段 0.1b）：产物集合从两份变三份，新增 _units.json；既有字段口径未改。
//   v2 → v3（阶段 0b）：**逐单位结果的数据来源从"调用方传参"收敛到
//     `FACBattleStatsSnapshot::UnitResults`**。这不改任何字段名，但改了"要不要一起读
//     _units.json"的判定依据（现在它与 _stats.json 必定同源），且 `_stats.json` 内层
//     新增/移除了一个键（见下），因此必须抬版本让比对脚本拒绝跨版本混比。
//   v3 → v4（阶段 0.5）：**时间单位从 tick 改成秒**（D2 / §5.2）。这是一次纯粹的
//     报文口径变更，比对脚本必须知道，否则会拿 v3 的 tick 序列去比 v4 的秒序列：
//       - JSONL 事件行的 `tick` 键 → **`time`**（值从整数 tick 变成浮点秒）；
//       - 元信息行的 `ticks` 键 → **`battleSeconds`**（同样是整数 → 浮点秒）。
//     抬版本的理由与 v2→v3 相同：字段口径变了就必须让脚本拒绝跨版本混比。
//   —— `_stats.json` 内层的 `stats` 对象**刻意剔除 `UnitResults`**：它与 `_units.json`
//      内容完全一致，重复写会让同一份数据在磁盘上出现两处（一旦只有一处被改就会自相矛盾），
//      而 _stats.json 的定位是"标量统计量 + 6 张聚合表"，逐单位明细归 _units.json。
//
// 数值表示：整数写整数；浮点走引擎 JSON writer 的 %.17g（float 提升成 double 后会有小数长尾，
// 例如 0.1f 会写成 0.10000000149011612）。同一数值必然产生同一文本，因此文本比对依然稳定，
// 只是人工阅读时数字偏长——这是引擎 writer 的固定行为，不为此自写格式化。
#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Core/ACBattleSetup.h"
#include "Core/ACBattleTypes.h"
#include "Diagnostics/ACBattleDiagnostics.h"

/**
 * 元信息输入：只放"必须由调用方提供"的字段。
 * 记录条数 / 丢弃条数由 writer 自己从缓冲里取，避免调用方与缓冲各算一份而对不上。
 *
 * 注意：**故意不含** FinalStateHash / RngSeed / RngDrawCount ——
 * 阶段 4（D3）起这三个字段在**内存结构**里也已经删除（`FACBattleLogMeta`），
 * 因此它们既不在契约里、也不在磁盘上；回归与排障完全依赖日志内容本身（§8.1 / §8.2）。
 */
struct AUTOCHESSBATTLE_API FACBattleLogMetaInput
{
    /** 战斗标识：FACBattleSetup::BattleId 的 GUID Digits 形式（32 位十六进制、无连字符），同时进文件名。 */
    FString BattleId;

    /** 内容数据版本（UBattleRuleConfig::DataVersion）。 */
    int32 DataVersion = 0;

    /** 战斗内相对时间（秒）：起始固定 0，结束 = `FACBattleTime::ElapsedSeconds`。 */
    double StartTimeSeconds = 0.0;
    double EndTimeSeconds = 0.0;

    /**
     * 战斗内相对时长（秒）。阶段 0.5：由 `int64 Ticks` 改成秒，JSON 键名同步改成 `battleSeconds`
     * （键名不改的话，脚本会拿 tick 的整数语义去读一个秒值）。
     */
    float BattleSeconds = 0.f;

    /** 结局（EACOutcome 的名字，见 writer 内的 OutcomeToString）。 */
    FString Outcome;

    /** 我方参战单位定义 ID（按参战顺序，与 FACBattleSetup::PlayerUnits 同序）。 */
    TArray<FName> PlayerUnitDefinitionIds;

    /** 敌方参战单位定义 ID（按生成顺序，与 FACBattleSetup::EnemyUnits 同序）。 */
    TArray<FName> EnemyUnitDefinitionIds;
};

/**
 * 日志写盘器：纯 IO、无实例状态，因此做成普通结构体而不是 UObject
 * （不需要反射与 GC 生命周期，也就不必引入 UHT 生成头）。
 *
 * 所有函数都是"失败不阻塞"：返回 false 只表示"这一场没落盘"，调用方必须忽略它继续跑。
 */
struct AUTOCHESSBATTLE_API FACBattleLogWriter
{
    /**
     * 落盘总开关。
     * 默认值：编辑器 / 开发构建为 true，发行构建（UE_BUILD_SHIPPING）为 false ——
     * 发行版每场战斗都写盘既没必要也会泄漏玩家的战斗记录。
     * 调用点还应自己套一层 `#if !UE_BUILD_SHIPPING`，让发行版连这次函数调用都不存在。
     */
    static bool IsFileLoggingEnabled();
    static void SetFileLoggingEnabled(bool bEnabled);

    /** 默认目录：<ProjectSaved>/BattleLogs（FPaths::ProjectSavedDir() + "BattleLogs"）。 */
    static FString GetLogDirectory();

    /**
     * 生成基名 Battle_<BattleId>_<UTC 时间戳>（不含扩展名）。
     * 时间戳是零填充的 UTC（YYYYMMDDThhmmssmmmZ），因此**字典序 = 时间序**，
     * 外部脚本直接按文件名排序就能得到稳定的时间顺序（毫秒也零填充，同秒内同样有序）。
     * BattleId 会先过滤掉文件名非法字符，过滤后为空则退化为 "NoBattleId"。
     */
    static FString BuildBaseFileName(const FString& BattleId, const FDateTime& UtcNow);

    /**
     * 结局 → 日志文本（写进元信息行的 "outcome"）。
     * 用这里的局部映射而不是 UEnum 反射：日志文本属于对外格式，集中在此处才能保证
     * 比对脚本面对的字符串集合是封闭的（枚举改名/加项不会悄悄改变已有文本）。
     */
    static const TCHAR* OutcomeToString(EACOutcome Outcome);

    /**
     * 低层入口：写到 <InDirectory>/<BaseFileName>.jsonl，若 Stats 非空则再写 <BaseFileName>_stats.json
     * 与 <BaseFileName>_units.json（逐单位结果取自 `Stats->UnitResults`）。
     * @param Records            日志记录（顺序即写入顺序）
     * @param DroppedRecordCount 缓冲因容量上限丢弃的记录数（写进元信息，避免"日志看起来是完整的"误判）
     * @param Stats              统计快照；nullptr = 不写统计文件，也不写逐单位文件
     *                           （逐单位结果的唯一来源就是它，见文件头"数据来源"）
     * @param Meta               元信息；nullptr = 不写元信息行
     * @param UtcNow             统一时间戳：同时用于元信息行的 wallClockUtc。必须与 BaseFileName 里的
     *                           时间戳来自同一次取时，否则"按文件名找内容"的交叉核对会差几毫秒。
     * @param OutLogPath         成功写出 .jsonl 时输出其完整路径（未写出则置空；其余产物失败时仍然有效）
     */
    static bool SaveToDirectory(const TArray<FBattleLogRecord>& Records, int32 DroppedRecordCount,
                                const FACBattleStatsSnapshot* Stats,
                                const FACBattleLogMetaInput* Meta,
                                const FString& InDirectory, const FString& BaseFileName,
                                const FDateTime& UtcNow, FString& OutLogPath);

    /**
     * 高层入口（战斗结束时的主调用点）：目录取 GetLogDirectory()，
     * 文件名由 Meta.BattleId + 当前 UTC 时间生成，日志 / 统计 / 逐单位结果三份一起落盘。
     *
     * 阶段 0b 起不再单独收逐单位数组：`Stats.UnitResults` 就是唯一权威副本，
     * 少一个入参就少一条"传进来的和结构体里的不一致"的可能。
     */
    static bool Save(const FBattleLogBuffer& Log, const FACBattleStatsSnapshot& Stats,
                     const FACBattleLogMetaInput& Meta, FString& OutLogPath);
};
