// 阶段 0.5 新增（GAS 重构实施方案 §2.4 / §5.2「实现裁决」）：**全场唯一时间源**。
//
// 为什么要有这个文件：D2 弃用 1/30 固定步之后，时间必须只有一个来源，否则会出现
// "战斗时钟"与"世界时钟"两个源，二者因帧率与暂停而漂移 —— 那正是 D2 要消除的问题。
// 而且 GAS 的 GE 时长判定内部读的就是 `UWorld::GetTimeSeconds()`（GameplayEffect.cpp:5044-5050），
// 内核只有同源，阶段 1 接 GAS 之后才不会出现"GE 到期了、内核还认为没到期"。
//
// 三种时间口径，**不要混用**：
//   ① 绝对时间（到期判定）：`FACBattleTime::Now(World)` = `UWorld::GetTimeSeconds()`。
//      它带一个非零起点（关卡已运行的时间），因此**只能**用于比较，不能当作"战斗进行了多久"。
//   ② 战斗内相对时间（日志时间轴、界面显示）：`ElapsedSeconds(World)`
//      = `Now(World) - World.GetBattleStartWorldTime()`。
//   ③ 帧增量（HP 回复 / 精神值恢复 / 行动条推进）：直接用 `UBattleWorld::Step` 传进来的 `DeltaTime`，
//      **不缓存、不累加** —— 本文件刻意不提供任何"累积秒数"的接口，就是为了让累加器无从写起。
//
// 本头是**纯工具头**：只有静态函数、无 UObject / USTRUCT，因此不需要（也不应有）`*.generated.h`。
#pragma once

#include "CoreMinimal.h"

class UWorld;
class UBattleWorld;

struct AUTOCHESSBATTLE_API FACBattleTime
{
    /**
     * 绝对时间：引擎世界时间（秒）。这是全场唯一时间源。
     * @param World 引擎世界；为 nullptr 时返回 0 并只告警一次（见实现）。
     */
    static float Now(const UWorld* World);

    /**
     * 绝对时间：从战斗世界取引擎世界（内部走 `World.GetWorld()`）。
     * 拿不到 UWorld 时返回 0 并**只在第一次**打一条 Warning：这条路径理论上不该发生
     * （单位与棋盘都是真的 Actor，说明 World 是活的），真发生了也不能刷屏 ——
     * 每次判定都打一条会让日志淹没在同一个错误里。
     */
    static float Now(const UBattleWorld& World);

    /** 战斗内相对时间 = `Now(World) - BattleStartWorldTime`（后者在 `Initialize` 记录一次）。 */
    static float ElapsedSeconds(const UBattleWorld& World);

    /** 到期判定统一入口：`ExpireTime < 0` 表示"永不过期"。 */
    static bool IsExpired(const UBattleWorld& World, float ExpireTime);

    /**
     * 到期判定的纯函数形态：`NowSeconds >= ExpireTime`。
     * 为什么把判定抽出来而不是让各处直接写 `Now >= ExpireTime`：
     * "负值 = 永不过期"这条约定散落到几十个调用点，漏一处就是"本来永久的护盾 1 秒后消失"，
     * 而且这种 bug 不会崩、只会静默改变数值。收口到一处，约定只写一遍。
     */
    static bool IsExpiredAt(float NowSeconds, float ExpireTime);
};
