#include "Battle/ACBattleTime.h"
#include "Battle/ACBattleWorld.h"
#include "Engine/World.h"

namespace
{
    /**
     * "拿不到 UWorld"的告警只打一次的闸门。
     *
     * 为什么需要它：`UBattleWorld` 是 UObject，正常挂在会话/子系统下必然有 Outer 链到 UWorld；
     * 拿不到说明**装配出了问题**，而不是运行期的常态。真出问题时，`Now()` 每帧会被几十处调用，
     * 逐次告警会把整份日志淹没在同一个错误里（而日志是放弃确定性之后唯一的排障手段）。
     * 只打一次，信号仍然是"有"，噪声不随帧数增长。
     */
    bool GWarnedMissingWorld = false;
}

float FACBattleTime::Now(const UWorld* World)
{
    if (World == nullptr)
    {
        if (!GWarnedMissingWorld)
        {
            GWarnedMissingWorld = true;
            UE_LOG(LogTemp, Warning,
                   TEXT("[Battle] FACBattleTime::Now: 拿不到 UWorld，时间返回 0；"
                        "所有到期判定会退化为'立即到期'（本告警只打一次）。"));
        }
        return 0.f;
    }

    // 唯一时间源：引擎世界时间。不要在这里加偏移、不要缓存到成员里。
    return World->GetTimeSeconds();
}

float FACBattleTime::Now(const UBattleWorld& World)
{
    return Now(World.GetWorld());
}

float FACBattleTime::ElapsedSeconds(const UBattleWorld& World)
{
    // 减一次记录在 Initialize 的起点，得到"战斗内相对时间"。
    // 两个量都来自同一个源，因此不会漂移；也不是累加器（累加器会随帧率漂移）。
    return Now(World) - World.GetBattleStartWorldTime();
}

bool FACBattleTime::IsExpired(const UBattleWorld& World, float ExpireTime)
{
    return IsExpiredAt(Now(World), ExpireTime);
}

bool FACBattleTime::IsExpiredAt(float NowSeconds, float ExpireTime)
{
    // 负值 = 永不过期（护盾 / 修饰器 / 召唤物 / 时间轴条目共用这一条约定）。
    if (ExpireTime < 0.f)
    {
        return false;
    }
    return NowSeconds >= ExpireTime;
}
