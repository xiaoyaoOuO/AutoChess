#include "Flow/ACBattleSession.h"
#include "Battle/ACBattleTime.h"
#include "Core/ACBattleTags.h"
#include "Diagnostics/ACBattleDiagnostics.h"
#include "Diagnostics/ACBattleLogWriter.h"
#include "Misc/Guid.h"
// D11 性能测量点（§6.3）：`FPlatformTime::Seconds()`（wall-clock 秒）与
// `GUObjectArray`（存活 UObject 计数）。两者都在 Core/CoreUObject 里，不需要 GAS 依赖。
#include "HAL/PlatformTime.h"
#include "UObject/UObjectArray.h"

void UBattleSession::Initialize(const FACBattleSetup& InSetup, const FACBattleLaunchOptions& InOptions,
                                const FACBattleDataContext& InDataContext)
{
    Setup = InSetup;
    Options = InOptions;
    DataContext = InDataContext;
    // 阶段 0.5：不再从 RuleConfig 取固定步长（该字段已删），时间源是引擎世界时间。
    bResultReady = false;
    CachedResult = FACBattleResult();
    PerfStats = FACBattlePerfStats();

    EnterPhase(EACPhase::Prepare);

    FString Error;
    if (!ValidateSetup(Error))
    {
        UE_LOG(LogTemp, Error, TEXT("[Battle] Invalid setup: %s"), *Error);
        Phase = EACPhase::Result;
        CachedResult.BattleId = Setup.BattleId;
        CachedResult.Outcome = EACOutcome::Error;
        bResultReady = true;
        return;
    }

    World = NewObject<UBattleWorld>(this);
    // ---- D11 性能测量点 ①：Actor 创建 + ASC 初始化 ----
    // `World->Initialize` 内部做完了：建棋盘 → 逐个 `SpawnActorDeferred` + 填数据
    // （含 `InitCommon` 写属性集、`SetAbilitySet` / `SetExtraAbilities`）→ `FinishSpawning`
    // （`BeginPlay` 里的 `InitAbilityActorInfo`）→ 落格 → `GrantToOwner`。
    // 因此这一段耗时正是 §6.3 要测的"Actor 创建 + ASC 初始化"。
    PerfStats.InitialUnitCount = Setup.PlayerUnits.Num() + Setup.EnemyUnits.Num();
    const double InitializeStartSeconds = FPlatformTime::Seconds();
    World->Initialize(Setup, DataContext, Options);
    PerfStats.InitializeSeconds = FPlatformTime::Seconds() - InitializeStartSeconds;

    EnterPhase(EACPhase::BattleStart);
    EnterPhase(EACPhase::Combat);
}

void UBattleSession::Shutdown()
{
    if (World != nullptr)
    {
        World->Shutdown();
        World = nullptr;
    }
    Phase = EACPhase::Teardown;
}

bool UBattleSession::ValidateSetup(FString& OutError) const
{
    if (Setup.PlayerUnits.Num() == 0)
    {
        OutError = TEXT("PlayerUnits is empty");
        return false;
    }
    return true;
}

void UBattleSession::EnterPhase(EACPhase NextPhase)
{
    Phase = NextPhase;
}

void UBattleSession::Tick(float RealDeltaSeconds)
{
    if (Phase != EACPhase::Combat || World == nullptr)
    {
        return;
    }

    // 阶段 0.5（D2 / §5.2）：直接按帧推进，没有累积器、没有追赶循环、没有"丢掉多余时间"的告警。
    // 帧长本身就是时间步长（`Step` 内部对 DeltaTime <= 0 直接返回），
    // 因此"卡帧时战斗变慢"这件事由引擎的 DeltaTime 语义统一承担，而不是内核自己补步。
    //
    // ---- D11 性能测量点 ②：战斗总帧时 ----
    // 测点在 `Step` **外面**（§2.3 的行动执行流程一行未动）：三次只读的计数器更新，
    // 不改变 `Step` 的行为，也不引入第二份时间源（这里量的是 CPU 耗时，不是游戏时间）。
    const double StepStartSeconds = FPlatformTime::Seconds();
    World->Step(RealDeltaSeconds);
    const double StepSeconds = FPlatformTime::Seconds() - StepStartSeconds;

    PerfStats.StepTotalSeconds += StepSeconds;
    PerfStats.StepMaxSeconds = FMath::Max(PerfStats.StepMaxSeconds, StepSeconds);
    ++PerfStats.StepCount;
    // GC 峰值代理：每帧采样一次存活 UObject 数（`FUObjectArray::GetObjectArrayNum()` 是
    // 一次数组长度读取，热路径无压力），记峰值。
    PerfStats.PeakObjectCount = FMath::Max(PerfStats.PeakObjectCount, GUObjectArray.GetObjectArrayNum());

    TickPhaseMachine();
}

void UBattleSession::TickPhaseMachine()
{
    if (World == nullptr)
    {
        return;
    }

    if (!World->IsFinished())
    {
        return;
    }

    EnterPhase(EACPhase::CheckEnd);
    if (bResultReady)
    {
        return;
    }

    EnterPhase(EACPhase::Result);

    FACBattleResult Result;
    if (BuildResult(Result))
    {
        CachedResult = Result;
        bResultReady = true;
    }
    else
    {
        CachedResult.BattleId = Setup.BattleId;
        CachedResult.Outcome = EACOutcome::Error;
        bResultReady = true;
    }

    EnterPhase(EACPhase::Teardown);
    if (World != nullptr)
    {
        World->Shutdown();
    }
}

void UBattleSession::RequestAbandon()
{
    FACBattleCommand Command;
    Command.Type = EACCommandType::AbandonBattle;
    Command.IssuedAtTime = World != nullptr ? FACBattleTime::Now(*World) : 0.f;
    PushCommand(Command);
}

void UBattleSession::PushCommand(const FACBattleCommand& Command)
{
    if (World != nullptr && Phase == EACPhase::Combat)
    {
        World->EnqueueCommand(Command);
    }
}

bool UBattleSession::BuildResult(FACBattleResult& OutResult)
{
    if (World == nullptr)
    {
        return false;
    }

    OutResult = FACBattleResult();
    OutResult.BattleId = Setup.BattleId;
    OutResult.Outcome = World->GetOutcome();
    // 逐单位结果：CollectUnitResults 是唯一生产点，它同时把结果回写进
    // FACBattleStatsSnapshot::UnitResults（D8 §5.1）。这里保留 OutResult.Units 这一份
    // 是**兼容既有消费方**（ACRunPostBattle 按下标与参战顺序对齐回写干员状态）——
    // 与 OutResult.Stats.UnitResults 同序同源，_units.json 从后者取，三处不会分叉。
    World->CollectUnitResults(OutResult.Units);
    OutResult.SoulCrystalDelta = World->Stats().GetSoulCrystalDelta();
    OutResult.SearchableSecretCount = World->Stats().GetSearchableSecretCount();

    // 阶段 0.5：战斗时长改用**战斗内相对时间（秒）**（原来是"已完成 tick 数"）。
    const float BattleSeconds = FACBattleTime::ElapsedSeconds(*World);
    World->Stats().SetBattleSeconds(BattleSeconds);
    // 阶段 4（D3）：`OutResult.Stats = World->Stats().ToSnapshot(StateHash)` 里的哈希参数
    // **已删除** —— `World->ComputeStateHash()` 与 `FACBattleStatsSnapshot::FinalStateHash`
    // 一起退场（C3：不追求确定性）。
    OutResult.Stats = World->Stats().ToSnapshot();

    // D11：把本场累计生成的单位数（含召唤物）从世界取回来，供 `BattlePerfReport` 打印。
    PerfStats.TotalSpawnedUnits = World->GetSpawnedUnitCount();

    OutResult.Log.DataVersion = DataContext.RuleConfig != nullptr ? DataContext.RuleConfig->DataVersion : 0;
    OutResult.Log.BattleSeconds = BattleSeconds;
    OutResult.Log.RecordCount = World->Log().Num();

#if !UE_BUILD_SHIPPING
    // ---- 结构化日志落盘----
    // 三层保护，避免发行版每场都写文件：
    //   ① 编译期：发行构建整段不编译；
    //   ② 运行期：默认开关（编辑器/开发构建为开），批量跑时可用 SetFileLoggingEnabled(false) 临时关掉；
    //   ③ 失败不阻塞：writer 内部对目录/写文件失败只记 Warning（返回值只表示产物是否齐全），
    if (FACBattleLogWriter::IsFileLoggingEnabled())
    {
        FACBattleLogMetaInput LogMeta;
        LogMeta.BattleId = Setup.BattleId.ToString(EGuidFormats::Digits);
        LogMeta.DataVersion = OutResult.Log.DataVersion;
        // 阶段 0.5：时间口径改秒（schema v4）。
        //   起始 = 0（战斗内相对时间的零点，与 FACBattleTime::ElapsedSeconds 的口径一致）；
        //   结束 = 战斗内相对时长；不再有 "CompletedTicks × FixedDt" 这个换算。
        LogMeta.StartTimeSeconds = 0.0;
        LogMeta.EndTimeSeconds = static_cast<double>(BattleSeconds);
        LogMeta.BattleSeconds = static_cast<double>(BattleSeconds);
        LogMeta.Outcome = FACBattleLogWriter::OutcomeToString(OutResult.Outcome);
        for (const FACPlayerUnitSpec& Spec : Setup.PlayerUnits)
        {
            LogMeta.PlayerUnitDefinitionIds.Add(Spec.DefinitionId);
        }
        for (const FACEnemyUnitSpec& Spec : Setup.EnemyUnits)
        {
            LogMeta.EnemyUnitDefinitionIds.Add(Spec.DefinitionId);
        }

        FString Path;
        // 返回值只表示"三份产物是否都写成功"（失败细节由 writer 自己告警），不参与战斗结果：
        // 只要 .jsonl 真的写出来了就记下路径，方便上层日志/UI 指向它。
        // 逐单位结果不再单独传：writer 从 OutResult.Stats.UnitResults 取（阶段 0b 收敛到一处），
        // 与 OutResult.Units 同序同源，因此 _units.json 不可能与结果结构体分叉。
        FACBattleLogWriter::Save(World->Log(), OutResult.Stats, LogMeta, Path);
        OutResult.Log.LogPath = Path;
    }
#endif

    return true;
}

bool UBattleSession::TryConsumeBuiltResult(FACBattleResult& OutResult)
{
    if (!bResultReady)
    {
        return false;
    }
    OutResult = CachedResult;
    bResultReady = false;
    return true;
}
