#include "Game/ACRunPlayerController.h"
#include "Run/ACRunSubsystem.h"
#include "Components/InputComponent.h"
#include "Diagnostics/ACBattleLogWriter.h"
// 阶段 4 的三条调试命令（BattleDumpGAS / BattleDumpUnits / BattlePerfReport）需要：
//   · `UBattleSubsystem` / `UBattleSession` —— 取"当前那场战斗"的世界与性能统计；
//   · `UBattleWorld` / `AACBattleUnitBase` —— 遍历单位与它的组件；
//   · ASC / GE —— 导出活动 GE 明细（层数 / 剩余时长 / 授予标签）；
//   · `AACBattleBoardActor` —— 把格子坐标换成世界坐标打出来（判断 Actor 位置是否正确）。
#include "Flow/ACBattleSubsystem.h"
#include "Flow/ACBattleSession.h"
#include "Battle/ACBattleWorld.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleBoardActor.h"
#include "Battle/ACAbilitySetComponent.h"
#include "Battle/Components/ACUnitGridComponent.h"
#include "Battle/Components/ACUnitPresentationComponent.h"
#include "Battle/ACBattleTime.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Abilities/GameplayAbility.h"
#include "Engine/GameInstance.h"
// `BattlePerfReport` 的"进程物理内存峰值"（量级参考）：`FPlatformMemory::GetStats()`。
#include "HAL/PlatformMemory.h"

UACRunSubsystem* AACRunPlayerController::GetRunSubsystem() const
{
    UGameInstance* GameInstance = GetGameInstance();
    return GameInstance != nullptr ? GameInstance->GetSubsystem<UACRunSubsystem>() : nullptr;
}

void AACRunPlayerController::BeginPlay()
{
    Super::BeginPlay();

    // 打开控制台提示（放在日志里而不是屏幕上，避免和 HUD 抢可视区）。
    UE_LOG(LogTemp, Log, TEXT("[Run] 控制台命令：AutoChess.Run.Status / Step / Auto 1 / Battle / Start [Seed] / Abandon"));
    PrintRunStatus();
}

void AACRunPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();

    // 不依赖 DefaultInput.ini 里的 ActionMapping：直接把按键绑成"执行控制台命令"。
    // 好处是零配置即可用，且与控制台走完全相同的代码路径。
    if (IsValid(InputComponent))
    {
        InputComponent->BindKey(EKeys::F1, IE_Pressed, this, &AACRunPlayerController::RunStatus);
        InputComponent->BindKey(EKeys::F2, IE_Pressed, this, &AACRunPlayerController::RunStep);
        InputComponent->BindKey(EKeys::F3, IE_Pressed, this, &AACRunPlayerController::RunBattle);
        // F4：开新局（固定种子）。用 lambda 是为了避开 exec 函数的 FString 签名限制。
        InputComponent->BindKey(EKeys::F4, IE_Pressed, this, &AACRunPlayerController::RunStartDefault);
    }
}

void AACRunPlayerController::RunStartDefault()
{
    RunStart(FString());
}

void AACRunPlayerController::PrintRunStatus()
{
    const UACRunSubsystem* Run = GetRunSubsystem();
    if (Run == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Run] 找不到 UACRunSubsystem（GameInstance 未就绪？）"));
        return;
    }

    // 分行写日志：控制台里多行 FString 不好读，逐行输出更实用。
    TArray<FString> Lines;
    Run->BuildStatusText().ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
    for (const FString& Line : Lines)
    {
        UE_LOG(LogTemp, Log, TEXT("[Run] %s"), *Line);
    }
}

// ---------------------------------------------------------------------------
// 控制台命令实现
// ---------------------------------------------------------------------------

void AACRunPlayerController::RunStatus()
{
    PrintRunStatus();
}

void AACRunPlayerController::RunStep()
{
    UACRunSubsystem* Run = GetRunSubsystem();
    if (Run == nullptr)
    {
        return;
    }

    // 手动推进时先关掉自动演示，避免"命令按一下、自动又推一步"的竞态观感。
    Run->SetAutoDemo(false);

    // 与控制台/自动演示共用同一套推进逻辑（UACRunSubsystem::AdvanceOneStep）。
    // 战斗中需要真实时间推进固定步，这里只给提示，不强行 Step（避免绕过固定步累积器）。
    if (Run->GetPhase() == EACRunPhase::Battle)
    {
        UE_LOG(LogTemp, Log, TEXT("[Run] 战斗中，请等待自动结算或输入 AutoChess.Run.Abandon。"));
    }
    else
    {
        Run->AdvanceOneStep();
    }

    PrintRunStatus();
}

void AACRunPlayerController::RunAuto(const FString& Args)
{
    UACRunSubsystem* Run = GetRunSubsystem();
    if (Run == nullptr)
    {
        return;
    }

    const bool bEnable = Args.IsEmpty() || Args.TrimStartAndEnd().StartsWith(TEXT("1"));
    Run->SetAutoDemo(bEnable);
    UE_LOG(LogTemp, Log, TEXT("[Run] 自动演示：%s"), bEnable ? TEXT("开") : TEXT("关"));
}

void AACRunPlayerController::RunBattle()
{
    UACRunSubsystem* Run = GetRunSubsystem();
    if (Run == nullptr)
    {
        return;
    }

    // 快捷验证：保证有一局在进行，并立刻把第一个战斗节点跑起来。
    if (Run->GetPhase() == EACRunPhase::Idle || Run->GetPhase() == EACRunPhase::Settled)
    {
        Run->StartRun(0);
    }
    Run->SetAutoDemo(false);
    Run->SelectFirstAvailableNode();
    Run->ResolveCurrentNode();
    PrintRunStatus();
}

void AACRunPlayerController::RunStart(const FString& Args)
{
    UACRunSubsystem* Run = GetRunSubsystem();
    if (Run == nullptr)
    {
        return;
    }

    const int32 Seed = Args.IsEmpty() ? 0 : FCString::Atoi(*Args);
    Run->StartRun(Seed);
    PrintRunStatus();
}

void AACRunPlayerController::RunAbandon()
{
    if (UACRunSubsystem* Run = GetRunSubsystem())
    {
        Run->AbandonBattle();
    }
}

void AACRunPlayerController::BattleDumpBaseline()
{
    UACRunSubsystem* Run = GetRunSubsystem();
    if (Run == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] 找不到 UACRunSubsystem，无法录制基线。"));
        return;
    }

    // 手动录制时显式打开落盘开关：开发构建默认就是开的，这里仅把"被谁关掉过"的情况兜回来，
    // 保证基线录制在任何运行参数下都能拿到文件。
    FACBattleLogWriter::SetFileLoggingEnabled(true);

    // 与 RunStep 同样的理由：先关自动演示，避免"手动触发一场，自动演示又插一脚"。
    Run->SetAutoDemo(false);

    // 用 UACRunSubsystem::AdvanceOneStep 从当前阶段推进到"战斗已开打"：
    //   Idle/Settled → 开新局；Map → 选路；NodeResolving → 结算节点；Shop → 商店决策；
    //   Battle → 返回 false（正是要停下的地方），循环条件因此同时是终止条件。
    // 之所以要循环：地图第一跳常常是起点/商人等非战斗节点，按一次走不到战斗。
    constexpr int32 MaxAdvanceSteps = 16;
    int32 Steps = 0;
    while (Run->GetPhase() != EACRunPhase::Battle && Steps < MaxAdvanceSteps)
    {
        if (!Run->AdvanceOneStep())
        {
            break;
        }
        ++Steps;
    }

    if (Run->GetPhase() == EACRunPhase::Battle)
    {
#if UE_BUILD_SHIPPING
        // 发行构建里 UBattleSession::BuildResult 的落盘段整体不编译（见那里的注释）：这里如实说明，
        // 免得有人在发行版里等一个永远不会出现的文件。
        UE_LOG(LogTemp, Warning, TEXT("[Battle] 发行构建不落盘：本场战斗不会产生日志文件。"));
#else
        UE_LOG(LogTemp, Log,
                TEXT("[Battle] 基线录制已开战，战斗结束后日志写入 %s（文件名 Battle_<BattleId>_<UTC>.jsonl）。"),
                *FACBattleLogWriter::GetLogDirectory());
#endif
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] 基线录制失败：%d 步内没有推进到战斗节点。"), MaxAdvanceSteps);
    }

    PrintRunStatus();
}

// ===========================================================================
// 调试命令：BattleDumpGAS / BattleDumpUnits / BattlePerfReport（阶段 4 §7）
//
// 三条命令共用"取当前战斗世界"这一个辅助。
// 为什么路径这么绕（GameInstance → BattleSubsystem → Session → World）：
// `UBattleWorld` 是 `UBattleSession` 的 `NewObject` 子对象，外部唯一合法的持有链就是这一条；
// 而**不能用** `Cast<UBattleWorld>(SomeActor->GetOuter())` —— 单位 Actor 的 Outer 是 `ULevel`，
// 那个 Cast 恒为 nullptr（见 `UBattleWorld::FindFromActor` 的说明）。
// ===========================================================================

UBattleWorld* AACRunPlayerController::GetActiveBattleWorld() const
{
    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance == nullptr)
    {
        return nullptr;
    }
    UBattleSubsystem* Battle = GameInstance->GetSubsystem<UBattleSubsystem>();
    if (Battle == nullptr)
    {
        return nullptr;
    }
    UBattleSession* Session = Battle->GetSession();
    return Session != nullptr ? Session->GetBattleWorld() : nullptr;
}

void AACRunPlayerController::BattleDumpGAS()
{
    UBattleWorld* World = GetActiveBattleWorld();
    if (World == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] BattleDumpGAS：当前没有进行中的战斗。"));
        return;
    }

    const float NowSeconds = FACBattleTime::Now(*World);
    int32 UnitCount = 0;
    int32 EffectCount = 0;

    UE_LOG(LogTemp, Log, TEXT("[Battle][GAS] ===== BattleDumpGAS @ %.3fs ====="),
           FACBattleTime::ElapsedSeconds(*World));

    // `ForEachUnit`（不是 ForEachAlive）：已死但保留在注册表里的单位也要看 ——
    // "死了还挂着状态 GE"正是本命令要能发现的一类问题。
    World->ForEachUnit([&UnitCount, &EffectCount, NowSeconds](AACBattleUnitBase& Unit)
    {
        ++UnitCount;
        UAbilitySystemComponent* const ASC = Unit.GetAbilitySystemComponent();
        UE_LOG(LogTemp, Log, TEXT("[Battle][GAS] --- Unit %d (%s) team=%d kind=%d ASC=%s ---"),
               Unit.GetUnitId(), *Unit.GetDefinitionId().ToString(),
               static_cast<int32>(Unit.GetTeam()), static_cast<int32>(Unit.GetKind()),
               ASC != nullptr ? TEXT("yes") : TEXT("NO"));

        if (ASC == nullptr)
        {
            return;
        }

        // ---- 已授予能力（判断"被动到底授没授上"看这一段）----
        // `GetActivatableAbilities()` 返回 `TArray<FGameplayAbilitySpec>&`
        //（AbilitySystemComponent.h:1128），`Spec.Ability` 是 **CDO**（GameplayAbilitySpec.h:198）。
        const TArray<FGameplayAbilitySpec>& Specs = ASC->GetActivatableAbilities();
        UE_LOG(LogTemp, Log, TEXT("[Battle][GAS]     granted abilities: %d"), Specs.Num());
        for (const FGameplayAbilitySpec& Spec : Specs)
        {
            const UGameplayAbility* const Ability = Spec.Ability.Get();
            UE_LOG(LogTemp, Log, TEXT("[Battle][GAS]       - %s (level=%d, active=%s)"),
                   Ability != nullptr ? *Ability->GetName() : TEXT("<null>"),
                   Spec.Level, Spec.IsActive() ? TEXT("yes") : TEXT("no"));
        }

        // ---- 活动 GE ----
        // `GetActiveEffects(FGameplayEffectQuery())` 的空查询匹配全部
        //（`FGameplayEffectQuery::Matches` 在各项条件都为空时直接返回 true）。
        const TArray<FActiveGameplayEffectHandle> Handles = ASC->GetActiveEffects(FGameplayEffectQuery());
        UE_LOG(LogTemp, Log, TEXT("[Battle][GAS]     active effects: %d"), Handles.Num());
        for (const FActiveGameplayEffectHandle& Handle : Handles)
        {
            const FActiveGameplayEffect* const Active = ASC->GetActiveGameplayEffect(Handle);
            if (Active == nullptr || Active->Spec.Def == nullptr)
            {
                // spec 的 Def 可能为空（异步加载中的 GE 资产）—— 跳过而不是猜。
                continue;
            }
            ++EffectCount;

            const UGameplayEffect* const Def = Active->Spec.Def;
            // `GetTimeRemaining` 对永不过期的 GE 返回 -1；因此只对 `HasDuration` 调用。
            const float Remaining = Def->DurationPolicy == EGameplayEffectDurationType::HasDuration
                ? FMath::Max(0.f, Active->GetTimeRemaining(NowSeconds))
                : -1.f;

            // 授予标签里带 `Effect.Trigger.Preemptive` 的就是"抢攻正在生效"（阶段 4 起
            // 这个标签是抢攻的唯一判据，见 `Core/ACBattleTags.h` 的说明）。
            UE_LOG(LogTemp, Log,
                   TEXT("[Battle][GAS]       - %s stacks=%d remaining=%.2fs durationPolicy=%d tags=[%s]"),
                   *Def->GetName(), Active->Spec.GetStackCount(), Remaining,
                   static_cast<int32>(Def->DurationPolicy),
                   *Def->GetGrantedTags().ToStringSimple());
        }
    });

    UE_LOG(LogTemp, Log, TEXT("[Battle][GAS] ===== end: %d units, %d active effects ====="),
           UnitCount, EffectCount);
}

void AACRunPlayerController::BattleDumpUnits()
{
    UBattleWorld* World = GetActiveBattleWorld();
    if (World == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] BattleDumpUnits：当前没有进行中的战斗。"));
        return;
    }

    // 棋盘是"格坐标 → 世界坐标"的唯一出口（§2.1）：单位侧不得自行换算，调试命令也一样。
    const AACBattleBoardActor* const Board = World->GetBoardActor();

    int32 UnitCount = 0;
    UE_LOG(LogTemp, Log, TEXT("[Battle][Units] ===== BattleDumpUnits @ %.3fs (board=%s) ====="),
           FACBattleTime::ElapsedSeconds(*World), Board != nullptr ? TEXT("yes") : TEXT("NO"));

    World->ForEachUnit([&UnitCount, Board](AACBattleUnitBase& Unit)
    {
        ++UnitCount;

        const FACHexCoord Cell = Unit.GetCell();
        // 棋盘缺失时算不出世界坐标（降级可用：单位仍记录格子），此时填零并靠上面的 board 标记识别。
        const FVector WorldLocation = Board != nullptr ? Board->CellToWorld(Cell) : FVector::ZeroVector;

        const UACUnitGridComponent* const Grid = Unit.GetGridComponent();
        const UACUnitPresentationComponent* const Presentation = Unit.GetPresentationComponent();
        const UACAbilitySetComponent* const SetComponent = Unit.GetAbilitySetComponent();
        UAbilitySystemComponent* const ASC = Unit.GetAbilitySystemComponent();

        const float FocusMax = Unit.GetFocusMax();

        UE_LOG(LogTemp, Log,
               TEXT("[Battle][Units] Unit %d %s team=%d kind=%d cell=(%d,%d) world=(%.1f, %.1f, %.1f) "
                    "hp=%.0f%% shield=%.1f focus=%.1f/%.0f mental=%.0f/%.0f action=%d target=%d"),
               Unit.GetUnitId(), *Unit.GetDefinitionId().ToString(),
               static_cast<int32>(Unit.GetTeam()), static_cast<int32>(Unit.GetKind()),
               Cell.Row, Cell.Col, WorldLocation.X, WorldLocation.Y, WorldLocation.Z,
               Unit.GetHealthRatio() * 100.f, Unit.GetShields().GetTotal(),
               Unit.GetFocusCurrent(), FocusMax,
               Unit.GetMental().Current, Unit.GetMental().MaxCurrent,
               static_cast<int32>(Unit.GetActionState()), Unit.GetCurrentTargetId());

        // 组件状态：这三行回答的是"Actor 化之后装配到底齐不齐"
        //（网格拿到棋盘没有、表现组件在不在、能力清单注入没有）。
        UE_LOG(LogTemp, Log,
               TEXT("[Battle][Units]     components: grid=%s(board=%s) presentation=%s abilitySet=%s "
                    "(set=%s extras=%d) ASC=%s dead=%s pendingDeath=%s"),
               Grid != nullptr ? TEXT("yes") : TEXT("NO"),
               Grid != nullptr && Grid->GetBoardActor() != nullptr ? TEXT("bound") : TEXT("none"),
               Presentation != nullptr ? TEXT("yes") : TEXT("NO"),
               SetComponent != nullptr ? TEXT("yes") : TEXT("NO"),
               SetComponent != nullptr && SetComponent->AbilitySet != nullptr ? TEXT("yes") : TEXT("none"),
               SetComponent != nullptr ? SetComponent->ExtraAbilities.Num() : 0,
               ASC != nullptr ? TEXT("yes") : TEXT("NO"),
               Unit.IsDead() ? TEXT("yes") : TEXT("no"),
               Unit.IsPendingDeath() ? TEXT("yes") : TEXT("no"));
    });

    UE_LOG(LogTemp, Log, TEXT("[Battle][Units] ===== end: %d units ====="), UnitCount);
}

void AACRunPlayerController::BattlePerfReport()
{
    UGameInstance* GameInstance = GetGameInstance();
    UBattleSubsystem* Battle = GameInstance != nullptr ? GameInstance->GetSubsystem<UBattleSubsystem>() : nullptr;
    UBattleSession* Session = Battle != nullptr ? Battle->GetSession() : nullptr;
    if (Session == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] BattlePerfReport：当前没有进行中的战斗（性能统计随会话存在）。"));
        return;
    }

    const FACBattlePerfStats& Perf = Session->GetPerfStats();
    const double MillisecondsPerUnit = Perf.GetSecondsPerUnit() * 1000.0;

    UE_LOG(LogTemp, Log, TEXT("[Battle][Perf] ===== BattlePerfReport（D11 / §6.3）====="));

    // ---- ① Actor 创建 + ASC 初始化 ----
    // 这一段是"要不要引入对象池"的核心判据：§6.3 的阈值是**每单位 3 ms**。
    UE_LOG(LogTemp, Log,
           TEXT("[Battle][Perf] ① 初始化：%.2f ms 总计 / %d 个单位 = %.3f ms 每单位（阈值 3.000 ms）→ %s"),
           Perf.InitializeSeconds * 1000.0, Perf.InitialUnitCount, MillisecondsPerUnit,
           MillisecondsPerUnit > 3.0 ? TEXT("超过阈值，建议引入对象池") : TEXT("在阈值内"));
    UE_LOG(LogTemp, Log,
           TEXT("[Battle][Perf]    （初始化段 = 建棋盘 + 全部单位 SpawnActorDeferred + 属性集初始化 + "
                "ASC InitAbilityActorInfo + 能力/GE 授予；战斗中累计生成 %d 个，含召唤物）"),
           Perf.TotalSpawnedUnits);

    // ---- ② 战斗总帧时 ----
    const double StepTotalMs = Perf.StepTotalSeconds * 1000.0;
    const double StepAvgMs = Perf.StepCount > 0 ? StepTotalMs / static_cast<double>(Perf.StepCount) : 0.0;
    UE_LOG(LogTemp, Log,
           TEXT("[Battle][Perf] ② 帧时：总 %.2f ms / %lld 帧，平均 %.3f ms，单帧最长 %.3f ms"),
           StepTotalMs, Perf.StepCount, StepAvgMs, Perf.StepMaxSeconds * 1000.0);

    // ---- ③ GC 峰值 ----
    // 引擎没有暴露"GC 峰值内存"这种量，因此用**峰值存活 UObject 数**回答
    // "Actor 化之后对象数会不会把 GC 拖爆"这个问题；进程物理内存峰值只能做量级参考
    //（它包含编辑器自身的占用），故单独标注。
    const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
    UE_LOG(LogTemp, Log,
           TEXT("[Battle][Perf] ③ GC：战斗期间峰值 UObject 数 = %d；进程物理内存峰值 = %.1f MB"
                "（含编辑器占用，仅作量级参考）"),
           Perf.PeakObjectCount, static_cast<double>(MemoryStats.PeakUsedPhysical) / (1024.0 * 1024.0));

    UE_LOG(LogTemp, Log, TEXT("[Battle][Perf] ===== end ====="));
}
