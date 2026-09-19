// M13 时间轴关键字：抢攻（战斗开始生效、持续 X 秒）/ 后发（第 X 秒触发）。
//
// ---------------------------------------------------------------------------
// 阶段 3.2b：**`FTimelineSystem` 整体删除，本文件只留一个快照结构体**
// ---------------------------------------------------------------------------
// 删除依据（§3.1 表 + §4.5 映射表 + §7 阶段 3.2）：
//   | 旧（FTimelineSystem）               | 新                                                       |
//   | ---------------------------------- | -------------------------------------------------------- |
//   | `FTimelineEntry` 抢攻               | 一个 `HasDuration` 的 GE（施加即生效，到期由引擎摘掉）        |
//   | `FTimelineEntry` 后发               | 内核侧待触发表（`UBattleWorld::PendingTimelineFires`）       |
//   | `IsActive`（条件原语）              | `ASC->HasMatchingGameplayTag(...)`（按 GE 授予的标签查）     |
//   | `ExtendPreemptive` / `AdvancePostEffect` / `RefreshPreemptive` | **未实现**（全仓无调用者，见下）    |
//   | `CaptureSnapshot`                  | 遍历 `ASC->GetActiveEffects(...)`；**阶段 4 后无消费者**（见下）|
//   | `FTimelineModifier`（按来源移除）    | 按 GE 来源（`SourceObject` / `SetByCaller` 标记）匹配        |
//
// ⚠️ 关于 §6.6（原 §6.2）的"时间轴改写 API"（`ExtendPreemptive` / `AdvancePostEffect` /
//    `RefreshPreemptive`）：方案要求"移除 + 按新时长重应用"，但在**接线并删除之前**
//    已用 grep 核实**全仓没有任何调用者**（`Run` 层与内容侧都没有用过），
//    因此本阶段**不实现**改写 API —— 为一个没有任何调用者的能力保留一条"移除 + 重应用"
//    的实现（还要处理来源匹配、句柄失效、上下文还原）是纯粹的负担。
//    将来真需要时，落点是"按 GE 的 owning tag 查活动 GE → 移除 → 用新时长重新施加"，
//    证据见阶段 3.2b 报告的「§6.6 时间轴改写处置」一节（附 grep 结果）。
//
// 阶段 4（§4.7 D4）：`UBattlePresentationBridge`（也就是下面这两个类型的**唯一**消费者）
// 已随表现桥整体删除。本文件的两个类型因此**暂时没有消费者**，但**保留**，理由：
//   · 它们是"抢攻 / 后发当前正在生效什么"这件事的只读形状，而这件事本身仍然存在
//     （判据在单位 ASC 的活动 GE 上：带 `Effect.Trigger.Preemptive` 的是抢攻，其余限时 GE 是后发）；
//   · §3.2 把 `FTimelineSnapshotEntry` 列为**保留项**，删除它属于"顺手扩大范围"；
//   · 将来接 UI 时的重建成本远高于留着这 25 行 —— 它是纯数据，不带任何逻辑或状态。
// 换句话说：这是"保留一个形状"，不是"保留一个系统"（`FTimelineSystem` 的类与实现都不在了）。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleTypes.h"

/** 时间轴关键字：抢攻（开局生效）/ 后发（第 X 秒触发）。 */
enum class EACTimelineKeyword : uint8
{
    Preemptive,
    PostEffect
};

/**
 * 时间轴快照条目（表现层只读）。
 *
 * 生成方式（阶段 3.2b）：遍历每个存活单位 ASC 的**活动 GE**，把"有持续时间的那些"各记一条。
 * 因此 `EntryId` 不再是稳定标识（旧实现是单调递增的条目编号），而是**本次快照内的序号**：
 * 它没有跨帧语义，UI 也不该拿它做 diff（要稳定标识请用 `Owner` + `Keyword`）。
 *
 * 阶段 4：生成它的那段代码（表现桥的匿名命名空间函数）已随 D4 删除，
 * 形状本身保留（见文件头的保留理由）。
 */
struct AUTOCHESSBATTLE_API FTimelineSnapshotEntry
{
    /** 本次快照内的序号（从 1 开始；**不是**跨帧稳定的条目 Id）。 */
    uint32 EntryId = 0;
    EACTimelineKeyword Keyword = EACTimelineKeyword::Preemptive;
    /** 剩余时长（秒）。 */
    float RemainingSeconds = 0.f;
    FUnitId Owner = InvalidUnitId;
};
