// M10 精神值：第二资源条 + 崩溃控制链。
// 规则：HP<50% 每秒 -5；HP<20% 每秒 -10（覆盖）；归零眩晕 2 秒；
//       我方回复 60（HP<20% 时 40）；敌方立即 +40，之后每秒 +5。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleTypes.h"

class UBattleWorld;
class AACBattleUnitBase;

// 阶段 0b（D8）：单位参数一律显式写成 Actor 类型；本头只把 `AACBattleUnitBase&` / `AACBattleUnitBase*`
// 当函数参数用，故前向声明足够。**不能** include Battle/ACBattleUnitBase.h ——
// 它反过来 include 本文件（FMentalState 是按值成员），会形成环。

/** 单位精神值状态：当前值、上限及免疫/禁止回复等标志。 */
struct AUTOCHESSBATTLE_API FMentalState
{
    float Current = 100.f;
    float MaxBase = 100.f;
    float MaxCurrent = 100.f;
    /** 上次崩溃时刻（绝对时间，秒）；**负值 = 从未崩溃**。 */
    float LastBreakTime = -1.f;
    bool bImmuneToBreak = false;
    bool bNoRecovery = false;
    float RecoveryPerSecond = 0.f;
};

/** 精神值系统（M10）：低血量持续掉精神、归零崩溃眩晕、按阵营差异化回复。 */
class AUTOCHESSBATTLE_API FMentalSystem
{
public:
    void Initialize(UBattleWorld* InWorld);

    /**
     * 每帧结算（阶段 0.5：由 `TickSecond(int64 CurrentTick)` 改名并改语义）。
     * @param DeltaTime 本帧增量（秒）。"每秒 -10 / -5 / +5"这类速率乘它得到本帧增量
     *                  （§5.2 实现裁决：帧增量直接用引擎给的 DeltaTime，不缓存不累加）。
     */
    void Tick(float DeltaTime);

    /** 精神伤害：只扣精神值，不扣生命。 */
    void ApplyMentalDamage(FUnitId Target, float Amount, FUnitId Source);

    /** 回复 / 注入。 */
    void GrantMental(FUnitId Target, float Amount);

    /** 临时修改上限（X「思维、情感、行动」降低 60 等）。 */
    void SetMaxOverride(FUnitId Target, float NewMax, float ExpireTime);

    void SetFlag(FUnitId Target, bool bImmuneToBreak, bool bNoRecovery);

    float GetCurrent(FUnitId Target) const;
    float GetMax(FUnitId Target) const;
    float GetRatio(FUnitId Target) const;

private:
    /** @param NowSeconds 崩溃发生的**绝对时间**（秒），用于崩溃保护窗口与延迟回复的计算。 */
    void HandleBreak(AACBattleUnitBase& Unit, float NowSeconds);
    UBattleWorld* World = nullptr;
};
