#include "Mental/ACMentalSystem.h"
#include "Battle/ACBattleTime.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "Diagnostics/ACBattleDiagnostics.h"
#include "Events/ACBattleEventBus.h"
#include "Core/ACBattleTags.h"
// 阶段 3.2b：精神崩溃的眩晕改由 **GE** 施加（旧 `FAbilityExecutor::ApplyStun` 是内核 API，
// 随技能执行线一起删除）。因此这里需要能力基类（提供 statics 施加入口）与眩晕 GE 类。
#include "GAS/ACBattleAbility.h"
#include "GAS/Effects/ACGE_States.h"

void FMentalSystem::Initialize(UBattleWorld* InWorld)
{
    World = InWorld;
}

void FMentalSystem::Tick(float DeltaTime)
{
    if (World == nullptr)
    {
        return;
    }

    World->ForEachAliveSnapshot([this, DeltaTime](AACBattleUnitBase& Unit)
    {
        FMentalState& Mental = Unit.GetMental();

        // 敌方崩溃后的持续回复（每秒量 × 本帧增量）。
        if (Mental.RecoveryPerSecond > 0.f && !Mental.bNoRecovery && Mental.Current > 0.f)
        {
            GrantMental(Unit.GetUnitId(), Mental.RecoveryPerSecond * DeltaTime);
        }

        const float HpRatio = Unit.GetHealthRatio();
        float DecayPerSecond = 0.f;
        if (HpRatio < 0.2f)
        {
            DecayPerSecond = 10.f;     // -10/秒，覆盖 -5
        }
        else if (HpRatio < 0.5f)
        {
            DecayPerSecond = 5.f;
        }

        if (DecayPerSecond > 0.f && !Unit.IsDead())
        {
            // 阶段 0.5：原来是"每整秒扣一次固定值"，改成"速率 × 本帧增量"。
            // 这样掉精神速度与帧率无关（原实现掉一帧就少扣一次，是固定步时钟才能成立的口径）。
            ApplyMentalDamage(Unit.GetUnitId(), DecayPerSecond * DeltaTime, InvalidUnitId);
        }
    });
}

void FMentalSystem::ApplyMentalDamage(FUnitId Target, float Amount, FUnitId Source)
{
    if (World == nullptr || Amount <= 0.f)
    {
        return;
    }

    AACBattleUnitBase* Unit = World->FindUnit(Target);
    if (Unit == nullptr || Unit->IsDead())
    {
        return;
    }

    FMentalState& Mental = Unit->GetMental();
    const float OldMental = Mental.Current;
    Mental.Current = FMath::Clamp(Mental.Current - Amount, 0.f, Mental.MaxCurrent);

    // 时间只取一次（崩溃保护窗口与 LastBreakTime 必须是同一时刻）。
    const float NowSeconds = FACBattleTime::Now(*World);

    FBattleHookContext Context;
    Context.Source = Source;
    Context.Target = Target;
    Context.Time = FACBattleTime::ElapsedSeconds(*World);
    Context.FloatValue = Mental.Current;
    World->Events().Dispatch(BattleTags::Hook_MentalChanged, Context);

    // 埋点：精神值下降。
    // 为什么必须记：精神伤害在 FCombatResolver::ApplyDamage 里早退转交本系统（不扣血、不产生
    // Combat|Damage 记录），所以若不在这里记账，精神值变化在整个日志里就是空白 —— 而 §8.2 明确要求逐条比对
    // 战斗内数值变化，精神值崩溃会直接改变胜负。
    // ValueB 用实际变化量（Clamp 之后）而不是形式上的 Amount，与专注埋点同一口径。
    // 被截断的部分不该出现在日志里，否则日志值与数值表对不上。
    if (World != nullptr && !FMath::IsNearlyEqual(OldMental, Mental.Current))
    {
        FBattleLogRecord Record;
        Record.Time = FACBattleTime::ElapsedSeconds(*World);
        Record.Category = FName(TEXT("Mental"));
        Record.EventTag = FName(TEXT("MentalChanged"));
        Record.Source = Source;
        Record.Target = Target;
        Record.ValueA = Mental.Current;
        Record.ValueB = Mental.Current - OldMental;
        World->Log().Record(Record);
    }

    if (Mental.Current <= 0.f && !Mental.bImmuneToBreak)
    {
        // M10 §5.4：眩晕窗口内不重复触发崩溃（LastBreakTime 保护）。
        // 阶段 0.5：窗口直接用秒比较（原来是 SecondsToTicks(2.f) 的 tick 窗口）。
        const bool bRecentlyBroke = Mental.LastBreakTime >= 0.f
            && (NowSeconds - Mental.LastBreakTime) < 2.f;
        if (!bRecentlyBroke)
        {
            HandleBreak(*Unit, NowSeconds);
        }
    }
}

void FMentalSystem::GrantMental(FUnitId Target, float Amount)
{
    if (World == nullptr || Amount <= 0.f)
    {
        return;
    }

    AACBattleUnitBase* Unit = World->FindUnit(Target);
    if (Unit == nullptr || Unit->IsDead())
    {
        return;
    }

    FMentalState& Mental = Unit->GetMental();
    if (Mental.bNoRecovery)
    {
        return;
    }

    const float OldMental = Mental.Current;
    Mental.Current = FMath::Clamp(Mental.Current + Amount, 0.f, Mental.MaxCurrent);

    const float NowSeconds = FACBattleTime::Now(*World);

    FBattleHookContext Context;
    Context.Target = Target;
    Context.Time = FACBattleTime::ElapsedSeconds(*World);
    Context.FloatValue = Mental.Current;
    World->Events().Dispatch(BattleTags::Hook_MentalChanged, Context);

    // 埋点：精神值回复（口径与 ApplyMentalDamage 对称，见那里的说明）。
    if (!FMath::IsNearlyEqual(OldMental, Mental.Current))
    {
        FBattleLogRecord Record;
        Record.Time = FACBattleTime::ElapsedSeconds(*World);
        Record.Category = FName(TEXT("Mental"));
        Record.EventTag = FName(TEXT("MentalChanged"));
        Record.Target = Target;
        Record.ValueA = Mental.Current;
        Record.ValueB = Mental.Current - OldMental;
        World->Log().Record(Record);
    }
}

void FMentalSystem::SetMaxOverride(FUnitId Target, float NewMax, float ExpireTime)
{
    if (World == nullptr)
    {
        return;
    }

    AACBattleUnitBase* Unit = World->FindUnit(Target);
    if (Unit == nullptr)
    {
        return;
    }

    FMentalState& Mental = Unit->GetMental();
    Mental.MaxCurrent = FMath::Max(0.f, NewMax);
    Mental.Current = FMath::Min(Mental.Current, Mental.MaxCurrent);
    // ExpireTime 的恢复由 M07 修饰器或效果块在到期时调用 SetMaxOverride(MaxBase) 完成。
    (void)ExpireTime;
}

void FMentalSystem::SetFlag(FUnitId Target, bool bImmuneToBreak, bool bNoRecovery)
{
    if (World == nullptr)
    {
        return;
    }
    if (AACBattleUnitBase* Unit = World->FindUnit(Target))
    {
        Unit->GetMental().bImmuneToBreak = bImmuneToBreak;
        Unit->GetMental().bNoRecovery = bNoRecovery;
    }
}

float FMentalSystem::GetCurrent(FUnitId Target) const
{
    const AACBattleUnitBase* Unit = World != nullptr ? World->FindUnit(Target) : nullptr;
    return Unit != nullptr ? Unit->GetMental().Current : 0.f;
}

float FMentalSystem::GetMax(FUnitId Target) const
{
    const AACBattleUnitBase* Unit = World != nullptr ? World->FindUnit(Target) : nullptr;
    return Unit != nullptr ? Unit->GetMental().MaxCurrent : 0.f;
}

float FMentalSystem::GetRatio(FUnitId Target) const
{
    const float Max = GetMax(Target);
    return Max > 0.f ? GetCurrent(Target) / Max : 0.f;
}

void FMentalSystem::HandleBreak(AACBattleUnitBase& Unit, float NowSeconds)
{
    FMentalState& Mental = Unit.GetMental();
    Mental.LastBreakTime = NowSeconds;

    FBattleHookContext Context;
    Context.Target = Unit.GetUnitId();
    Context.Time = FACBattleTime::ElapsedSeconds(*World);
    World->Events().Dispatch(BattleTags::Hook_MentalBreak, Context);

    if (Unit.GetTeam() == EACTeam::Enemy)
    {
        World->Stats().RecordEnemyMentalBreak();
    }

    // 眩晕 2 秒（等级/行动门控由调度器与能力的 `ActivationBlockedTags` 处理）。
    //
    // 阶段 3.2b：**改施加一个 GE**。旧实现是内核 API `FAbilityExecutor::ApplyStun(2.f)`，
    // 它做了三件事：写 `Unit.SetStunned(true)`、把 `ActionState` 设成 `Stunned`、
    // 在自持的 `FStunEntry` 数组里记一个到期时刻（并靠 `TickStep` 每帧扫到期）。
    // 这三件事现在全部由 `UACGE_State_Stun` 承担：
    //   · `HasDuration`（时长由 SetByCaller `Data.DurationSeconds` 给 = 2 秒）→ 到期由引擎的
    //     GE 定时器摘掉，**内核不再需要任何"眩晕计时器 / 到期解除"**；
    //   · `GrantedTags = State.Stun` → "是否被眩晕"就是 `ASC->HasMatchingGameplayTag`，
    //     调度器门控（`FActionScheduler::EvaluateGate`）、AI（`FBattleAISystem::Decide`）、
    //     能力门控（普攻与技能能力的 `ActivationBlockedTags`）都查它。
    //
    // ⚠️ 重入眩晕的语义（A18"取最晚到期时刻"）：`ConfigureStacking(1, RefreshOnSuccessfulApplication, …)`
    //    让重复施加**从当前时刻重新计时**。对本处恒为 2 秒的时长来说，
    //    "refresh"与旧的 `Max(已有 EndTime, Now + 2)` 完全等价（`Now + 2` 永不早于旧到期时刻）。
    // ⚠️ 破崩溃保护窗口（`LastBreakTime` 的 2 秒）在 GE 之外仍然保留，判据一字未改 ——
    //    它挡的是"同一单位每帧反复崩溃"，与眩晕的施加形式无关。
    UACBattleAbility::ApplyEffectToUnit(
        Unit, Unit, UACGE_State_Stun::StaticClass(),
        UACBattleAbility::MakeSetByCaller(UACGE_State_Stun::GetDurationDataName(), 2.f));

    const bool bLowHP = Unit.GetHealthRatio() < 0.2f;
    if (Unit.GetTeam() == EACTeam::Player)
    {
        // 眩晕结束后回复；框架用延迟事件在 2 秒后处理。
        const float RecoverValue = bLowHP ? 40.f : 60.f;
        // 阶段 0.5：到期时刻 = 现在 + 2 秒（绝对时间）。
        World->EnqueueDelayedMentalRecover(Unit.GetUnitId(), RecoverValue, NowSeconds + 2.f);
    }
    else
    {
        GrantMental(Unit.GetUnitId(), 40.f);
        Mental.RecoveryPerSecond = 5.f;
    }

    if (Mental.bNoRecovery)
    {
        // 蚀火之种：不再回复、不再因精神值被眩晕。
        Mental.bImmuneToBreak = true;
        Mental.RecoveryPerSecond = 0.f;
    }
}
