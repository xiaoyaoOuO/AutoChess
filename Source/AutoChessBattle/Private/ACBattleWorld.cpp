#include "Battle/ACBattleWorld.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleTime.h"
#include "Battle/ACAbilitySetComponent.h"
#include "Core/ACBattleTags.h"
#include "Engine/World.h"
// 阶段 2：复活时清理单位身上有持续时间的 GE（见本文件 `RemoveDurationGameplayEffects`）。
// `AbilitySystemComponent.h` 与 `GameplayEffect.h` 走显式包含：本文件需要
// `UAbilitySystemComponent::GetActiveEffects` / `GetActiveGameplayEffect` /
// `TickComponent`（阶段 3.2a：每帧驱动 ASC）与 `UGameplayEffect::DurationPolicy`，
// 它们虽然被 `ACBattleUnitBase.h` 间接带进来，但依赖写显式，将来头文件瘦身时不会突然编译不过。
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
// 阶段 3.2a：能力 / 常驻 GE 的清单与"效果块 Id → GE 类"过渡表都住在数据上下文里，
// 而 `UACAbilitySet` 的完整类型是 `GiveTo` 所必需的（`TSubclassOf<UGameplayAbility>` 的数组操作）。
#include "GAS/ACAbilitySet.h"
// 阶段 3.2b（行动执行改"能力驱动"）：
//   `GAS/ACBattleAbility.h`    —— `MoveOneStepTowards`（超距时走一格）、`GrantFocusToUnit`（坦克受击回专注）、
//                                `MakeEventPayload`（广播钩子 GameplayEvent 时的载荷映射）；
//   `ACSkillAbilityBase.h` / `ACBasicAttackAbility.h` —— 内核要**按基类分类**能力句柄
//                                （技能 vs 普攻），因此需要这两个类的完整定义。
#include "GAS/ACBattleAbility.h"
#include "GAS/Abilities/ACBasicAttackAbility.h"
#include "GAS/Abilities/ACSkillAbilityBase.h"
// `UBattleWorld::FindFromActor` 的路径：Actor → UWorld → UGameInstance → UBattleSubsystem → UBattleSession。
// 为什么不能走 `Cast<UBattleWorld>(Actor->GetOuter())`：`UWorld::SpawnActor` 内部是
// `NewObject<AActor>(LevelToSpawnIn, ...)`，单位 Actor 的 Outer 是 `ULevel`，那个 Cast 恒为 nullptr，
// 会让所有"从 GE/Execution 反查战斗世界"的调用静默失败（伤害不落地、召唤不生成，且无报错）。
#include "Engine/GameInstance.h"
#include "Flow/ACBattleSubsystem.h"
#include "Flow/ACBattleSession.h"

// ---------------------------------------------------------------------------
// 阶段 0a（GAS 重构实施方案 §7 0a）：单位从 UObject 变成 Actor。
// 本文件是**唯一**改逻辑的地方：
//   - 生成路径 NewObject<AACBattleUnitBase>(this) → UWorld::SpawnActorDeferred（延迟构造）；
//   - 注册表 TArray<TObjectPtr<AACBattleUnitBase>> + TMap<FUnitId,TObjectPtr<AACBattleUnitBase>> UnitLookup；
//   - Shutdown 从"Reset 数组"改成真的 Destroy()，否则 Actor 会泄漏到关卡里。
// 阶段 0b：单位类型别名已整体删除，全部写成显式 AACBattleUnitBase；
// FindUnit(FUnitId) 的签名与语义保持不变（D8 只把它当内部查表入口），
// 新增弱指针解引用统一入口 Resolve() / ResolveOrWarn()（D8，§5.1）。
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// 阶段 3.2a（GAS 重构实施方案 §7 阶段 3.2）：本文件是"删除旧系统"的主战场。
// 被删掉的调用（原来都以 `EffectSystem.` 开头）与它们的替代：
//   - `EffectSystem.Initialize(this, DataContext.EffectLibrary)`
//       → 整个效果系统没了；常驻内容走 `UACAbilitySet` + 能力里的 GE 施加。
//   - `TimelineSystem.OnTriggered` 里的 `ExecuteBlock(Entry.EffectBlockId)`
//       → **阶段 3.2b 已收口**：抢攻改"开局施加一个带时长的 GE"、后发改内核待触发表
//         （`PendingTimelineFires` / `TickPendingTimelineFires`），`FTimelineSystem` 已删除。
//   - 三处 `GetStates().Initialize(...)` / `GetStates().Tick(...)`
//       → 状态层数由 ASC 的标签计数承担（`AACBattleUnitBase::GetStateStacks`）。
//   - `EffectSystem.RegisterBlock(...)`（定义被动 / 生成请求 / 强化 / 装备 / 词条 / 外部注入）
//       → 全部收口到两条路：定义自带的走 `UACAbilitySetComponent::GrantToOwner()`；
//         按局内进度追加的走 `ApplyGameplayEffectsToUnit`（**阶段 3.3 起直接收 GE 类**，
//         3.2a 那张 `FACBattleDataContext::EffectBlockGEs` 过渡映射表已随字段类型改造删除）。
//   - `EffectSystem.ExecuteBlock(Command.CardId)`（秘术卡）
//       → 同上，改走 `ApplyGameplayEffectsToUnit`。
//   - `EffectSystem.TickPeriodic(Now)` / `EffectSystem.UnregisterAllByOwner` / `MixStateInto`
//       → 周期结算改由 GE 的 `Period` + `UACPeriodicDamageExecution`（引擎侧定时器）；
//         随单位销毁一起消失（GE 挂在 ASC 上，Actor 销毁即释放）；状态哈希的"效果实例"维度消失。
//
// 阶段 3.2b（§7 阶段 3.2 的技能执行线 + 时间轴线）本文件又删掉两条：
//   - `AbilityExecutor.RequestAction / TickChannels / TickStep / OnTakeHit / Initialize`
//       → `ExecuteActionForUnit`（技能 → 普攻 → 移动，含 fallback）+ `MaintainTarget`（索敌）
//         + 每帧的专注回复；`Ability/ACAbilityExecutor.{h,cpp}` 已整体删除。
//   - `TimelineSystem.Initialize / Update / OnTriggered / RegisterEntry`
//       → 开局直接施加抢攻 GE + `PendingTimelineFires`；`FTimelineSystem` 已整体删除。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 结构化日志埋点（阶段 0.1b）
//
// 为什么埋在 World 里：死亡 / 复活 / 胜负这三件事的**唯一决定点**都在这里
// （ResolveDeaths 的 MarkDead、ReviveUnit、CheckOutcome），别处只是请求"标记待死"。
// 阶段 0 的 Actor 化会把这三段搬运到 Actor 生命周期回调里，埋点跟着走，语义不变。
//
// 日志仍用整型 FUnitId：§5.1 已裁决日志与钩子上下文属"战斗内瞬时结构"，
// 整数才能人工读、才能跨场对齐（弱指针打印出来是内存地址）。
// ---------------------------------------------------------------------------
namespace
{
    /** 日志分类：单位的生（复活）与死。 */
    const FName LogCategory_Unit(TEXT("Unit"));
    const FName LogEvent_Death(TEXT("Death"));
    const FName LogEvent_Revive(TEXT("Revive"));

    /** 日志分类：战斗级状态变化（目前只有胜负判定）。 */
    const FName LogCategory_Battle(TEXT("Battle"));
    const FName LogEvent_Outcome(TEXT("Outcome"));

    /**
     * 日志分类与事件标签：时间轴（阶段 3.2b 从 `FTimelineSystem` 搬来，口径逐字一致）。
     *
     * 为什么埋点必须跟着搬：`Timeline | Preemptive` / `Timeline | PostEffect` 这两条记录
     * 是 §8.2「日志对照」里唯一能证明"抢攻生效了 / 后发到点了"的证据，
     * 丢掉它们会让"时间轴"这一整块在日志里变成空白。
     */
    const FName LogCategory_Timeline(TEXT("Timeline"));
    const FName LogEvent_Preemptive(TEXT("Preemptive"));
    const FName LogEvent_PostEffect(TEXT("PostEffect"));

    void LogWorldEvent(UBattleWorld& World, FName Category, FName EventTag, FUnitId Source, FUnitId Target,
                       int32 IntValue = 0)
    {
        FBattleLogRecord Record;
        Record.Time = FACBattleTime::ElapsedSeconds(World);
        Record.Category = Category;
        Record.EventTag = EventTag;
        Record.Source = Source;
        Record.Target = Target;
        Record.IntValue = IntValue;
        World.Log().Record(Record);
    }

    /**
     * 移除单位身上**有持续时间**的 GE（`DurationPolicy == HasDuration`），保留 `Infinite` 的。
     *
     * 用途：复活前清临时状态（替代阶段 1 的 `FBattleStatPipeline::ClearBattleTemp`，见 `ReviveUnit`）。
     *
     * 实现要点（都是刻意的，不是随手写的）：
     *   ① 先**收句柄再移除**，不在遍历中改容器 —— `RemoveActiveGameplayEffect` 会触发 GE 移除回调，
     *      回调里可能再应用新的 GE，边遍历边删是典型的迭代器失效场景；
     *   ② `GetActiveEffects(FGameplayEffectQuery())`：空查询匹配全部（引擎的
     *      `FGameplayEffectQuery::Matches` 在各项条件都为空时直接返回 true），因此不需要造标签；
     *   ③ 用 `RemoveActiveGameplayEffect_NoReturn`：单机项目没有权威性检查的问题，
     *      且它是引擎里语义最直白的"按句柄移除"（内部直达 `ActiveGameplayEffects.RemoveActiveGameplayEffect`）；
     *   ④ 每一步都判空：`Spec.Def` 可能为空（异步加载中的 GE 资产），`GetActiveGameplayEffect` 也可能返回空。
     */
    void RemoveDurationGameplayEffects(AACBattleUnitBase& Unit)
    {
        UAbilitySystemComponent* const ASC = Unit.GetAbilitySystemComponent();
        if (ASC == nullptr)
        {
            return;
        }

        const TArray<FActiveGameplayEffectHandle> Handles = ASC->GetActiveEffects(FGameplayEffectQuery());
        if (Handles.Num() == 0)
        {
            return;
        }

        TArray<FActiveGameplayEffectHandle> ToRemove;
        ToRemove.Reserve(Handles.Num());
        for (const FActiveGameplayEffectHandle& Handle : Handles)
        {
            const FActiveGameplayEffect* const ActiveEffect = ASC->GetActiveGameplayEffect(Handle);
            if (ActiveEffect == nullptr || ActiveEffect->Spec.Def == nullptr)
            {
                continue;
            }
            if (ActiveEffect->Spec.Def->DurationPolicy == EGameplayEffectDurationType::HasDuration)
            {
                ToRemove.Add(Handle);
            }
        }

        for (const FActiveGameplayEffectHandle& Handle : ToRemove)
        {
            // 句柄可能在上一轮移除的回调里已经失效（例如某个 GE 的 OnRemoved 顺手移掉了另一个），
            // 因此逐个再查一次；拿不到就跳过，不告警（这是正常竞态，不是数据损坏）。
            if (ASC->GetActiveGameplayEffect(Handle) != nullptr)
            {
                ASC->RemoveActiveGameplayEffect(Handle);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void UBattleWorld::Initialize(const FACBattleSetup& InSetup, const FACBattleDataContext& InDataContext,
                              const FACBattleLaunchOptions& InOptions)
{
    Setup = InSetup;
    Options = InOptions;
    DataContext = InDataContext;

    // 时间基准（阶段 0.5，§5.2 实现裁决）：只在这里记一次战斗起始的**引擎世界时刻**，
    // 之后所有"战斗内相对时间"都由它算出（FACBattleTime::ElapsedSeconds）。
    // 原来这里的 Clock.Initialize(FixedDt) 已随固定步时钟一起删除。
    BattleStartWorldTime = FACBattleTime::Now(*this);

    // 阶段 4（D3 / §5.3）：这里原来的 `RngSeed = Setup.RngSeed …; Rng.Initialize(RngSeed);`
    // **已删除** —— 战斗内核不再有随机流（C3：随机直接用 `FMath::FRand` / `FMath::RandRange`），
    // 契约里的 `FACBattleSetup::RngSeed` 字段也一并删除。

    EventBus.Initialize();
    GridSystem.Initialize();
    CombatResolver.Initialize(this);
    // 阶段 3.2a：原来的 `EffectSystem.Initialize(this, DataContext.EffectLibrary)` 已删除 ——
    // 效果系统整条线（效果块表 + 条件求值 + 动作执行 + 实例注册 + 频率守卫）被 GE 取代：
    //   · 常驻内容（被动 / 装备 / 词条 / 强化）→ `UACAbilitySet::GrantedAbilities` / `GrantedEffects`；
    //   · 运行时效果（技能 / 普攻附加 / 秘术卡）→ 能力里 `ApplyEffectToUnit` 施加 GE；
    //   · 周期结算 → GE 的 `Period` + `UACPeriodicDamageExecution`（引擎侧定时器）。
    // 因此这里不再需要任何"初始化"：挂点就是每个单位的 ASC。
    Scheduler.Initialize(this);
    TargetingSystem.Initialize(this);
    AISystem.Initialize(this);
    MentalSystem.Initialize(this);
    StatsCollector.Reset();
    LogBuffer.Reset();

    // Actor 化（阶段 0a）：注册表同时持有顺序数组与 Id 查表，清理时必须成对。
    Units.Reset();
    UnitLookup.Reset();
    Commands.Reset();
    SystemSubscriptions.Reset();
    DelayedMentalRecovers.Reset();
    // 阶段 3.2b：后发待触发表与时间轴一起重建（跨场必须清空，否则上一场的后发会在本场触发）。
    PendingTimelineFires.Reset();
    Outcome = EACOutcome::None;
    NextUnitId = 1;
    ResolveMissedCount = 0;   // D8 自检计数：跨场重置，否则上一场的悬垂会污染本场统计

    // 时间轴回调（阶段 3.2a 的临时占位）**已删除**：阶段 3.2b 把抢攻 / 后发两条线直接落地 ——
    //   抢攻 → `ApplyPreBattleConfiguration` 里"开局施加一个带时长的 GE"；
    //   后发 → `PendingTimelineFires` + `TickPendingTimelineFires`。
    // 现在不再需要任何"触发回调"：GE 自带时长（到期由引擎摘掉），后发条目就是数据。

    // 系统级钩子：静电紊乱（每次动作）与坦克受击回专注。
    SystemSubscriptions.Add(EventBus.Subscribe(BattleTags::Hook_BeforeAttack, FName(TEXT("System.StaticDisorder")),
                                               InvalidUnitId, 0, FBattleHookDelegate::CreateUObject(this, &UBattleWorld::HandleActionTriggered)));
    SystemSubscriptions.Add(EventBus.Subscribe(BattleTags::Hook_UnitMoved, FName(TEXT("System.StaticDisorder.Move")),
                                               InvalidUnitId, 0, FBattleHookDelegate::CreateUObject(this, &UBattleWorld::HandleActionTriggered)));
    SystemSubscriptions.Add(EventBus.Subscribe(BattleTags::Hook_SkillCast, FName(TEXT("System.StaticDisorder.Skill")),
                                               InvalidUnitId, 0, FBattleHookDelegate::CreateUObject(this, &UBattleWorld::HandleActionTriggered)));
    SystemSubscriptions.Add(EventBus.Subscribe(BattleTags::Hook_TakeDamage, FName(TEXT("System.TankFocus")),
                                               InvalidUnitId, 0, FBattleHookDelegate::CreateUObject(this, &UBattleWorld::HandleTakeDamage)));

    // 1) 生成战场棋盘 Actor（坐标变换唯一出口，§2.1）。
    //    先建棋盘：单位落格时 UACUnitGridComponent 需要它把格坐标换成世界位置。
    //    GetWorld() 不可用（Outer 链上找不到 UWorld）时告警并跳过，绝不 check 崩溃。
    UWorld* const GameWorld = GetWorld();
    if (GameWorld == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] UBattleWorld::Initialize: no UWorld; board actor and units are skipped."));
    }
    else
    {
        FActorSpawnParameters BoardSpawnParams;
        BoardSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        BoardSpawnParams.ObjectFlags |= RF_Transient;   // 战场棋盘不参与关卡存档
        BoardActor = GameWorld->SpawnActor<AACBattleBoardActor>(AACBattleBoardActor::StaticClass(),
                                                                FTransform::Identity, BoardSpawnParams);
        if (BoardActor == nullptr)
        {
            // 生成失败也要继续：单位仍会记录格子，只是不做世界坐标同步（降级可用）。
            UE_LOG(LogTemp, Warning, TEXT("[Battle] Failed to spawn AACBattleBoardActor; units keep cell coords only."));
        }
    }

    // 2) 生成双方单位。
    //    Actor 化流程（§2.2）：SpawnActorDeferred → 填数据 → FinishSpawning → 落格 → 注册。
    //    ⚠️ FinishSpawning 必须在数据填好之后：BeginPlay 在此时才触发。
    //       PlaceUnitOnGrid / SetFacing / RegisterUnit 的相对顺序与改造前一致。
    for (const FACPlayerUnitSpec& Spec : Setup.PlayerUnits)
    {
        AACBattleUnitBase* Unit = SpawnUnitActor(ResolveUnitClass(EACTeam::Player, EACUnitKind::Operator));
        if (Unit == nullptr)
        {
            continue;   // 生成失败已告警；跳过该单位而不是崩溃
        }

        const UOperatorDefinition* Definition = Cast<UOperatorDefinition>(FindUnitDefinition(Spec.DefinitionId));
        Unit->SetUnitId(NextUnitId++);
        Unit->SetDebugId(Unit->GetUnitId());     // 阶段 0a：DebugId 与 UnitId 同值，保证日志可读
        Unit->InitializeFromPlayerSpec(Spec, Definition);
        // 阶段 3.3（内容语义保真）：把 Run 层"按局内进度"带来的被动能力注入能力集组件。
        // ⚠️ 必须在 `RegisterUnit`（= 授予点）之前：`GrantToOwner` 会把清单与这份追加清单
        //    在**同一次**里授完，因此单位一出生就同时拥有"天生被动"与"选了强化/装了装备才有的被动"。
        //    放在 `FinishSpawning` 之前也可以（组件此时已存在，授予发生在更晚的 RegisterUnit）。
        if (UACAbilitySetComponent* const SetComponent = Unit->GetAbilitySetComponent())
        {
            SetComponent->SetExtraAbilities(Spec.GrantedAbilities);
        }
        // 阶段 3.2a：`Unit->GetStates().Initialize(this, Unit->GetUnitId(), &DataContext.AbnormalStates)`
        // 已删除 —— 状态容器不存在了。异常状态的"定义"现在写在状态 GE 类里
        // （`StackLimitCount` / `Period` / `GrantedTags`，见 `GAS/Effects/ACGE_States.*`）。
        // 单位侧不需要任何"状态初始化"：身上没有状态 GE 就等于没有任何状态。
        Unit->FinishSpawning(FTransform::Identity);

        if (UACUnitGridComponent* UnitGrid = Unit->GetGridComponent())
        {
            // §2.1：棋盘由 World 注入单位，单位不自行寻找棋盘。
            UnitGrid->SetBoardActor(BoardActor);
        }

        PlaceUnitOnGrid(*Unit, Spec.SpawnCell);
        Unit->SetFacing(EACFacing::Up);
        RegisterUnit(*Unit);
    }

    for (const FACEnemyUnitSpec& Spec : Setup.EnemyUnits)
    {
        AACBattleUnitBase* Unit = SpawnUnitActor(ResolveUnitClass(EACTeam::Enemy, EACUnitKind::Enemy));
        if (Unit == nullptr)
        {
            continue;
        }

        const UEnemyDefinition* Definition = Cast<UEnemyDefinition>(FindUnitDefinition(Spec.DefinitionId));
        Unit->SetUnitId(NextUnitId++);
        Unit->SetDebugId(Unit->GetUnitId());
        Unit->InitializeFromEnemySpec(Spec, Definition);
        // 阶段 3.2a：状态容器的 `Initialize` 已删除，理由同上面玩家单位那一处。
        // 阶段 3.3：敌方契约里没有再追加的被动能力（敌人内容全部走 `AbilitySets`），
        // 只有"本场额外 GE"（`FACEnemyUnitSpec::Effects`），由 `ApplyPreBattleConfiguration` 施加。
        Unit->FinishSpawning(FTransform::Identity);

        if (UACUnitGridComponent* UnitGrid = Unit->GetGridComponent())
        {
            UnitGrid->SetBoardActor(BoardActor);
        }

        PlaceUnitOnGrid(*Unit, Spec.SpawnCell);
        Unit->SetFacing(EACFacing::Down);
        RegisterUnit(*Unit);
    }

    // 3) 战前配置与开局效果。
    ApplyPreBattleConfiguration();

    // 4) 开局钩子。
    FBattleHookContext StartContext;
    StartContext.Time = FACBattleTime::ElapsedSeconds(*this);
    EventBus.Dispatch(BattleTags::Hook_BattleStart, StartContext);

    // 4b) **同一时刻把 `Hook.BattleStart` 作为 GameplayEvent 广播给全场单位的 ASC**（阶段 3.2b）。
    //
    // 为什么必须广播（这是"被动能不能触发"的关键一处）：
    //   ① `UACPassive_Enemy_MotherNest_Spawn` 监听 `Hook.BattleStart`，而它的上下文里
    //      `Source` 是 `InvalidUnitId`（没有任何"目标单位"可以单发），只发给 Source 等于没发；
    //   ② 真正决定"谁会被唤醒"的是"这条 GameplayEvent 有没有到达它的 ASC"，而不是上下文里的
    //      Source/Target —— 这与旧 `FEffectSystem::HandleHook`「不按 owner 过滤、遍历所有
    //      同标签效果块」的语义一致（母巢以外的单位监听同一标签也会被唤醒，这是旧行为）。
    BroadcastHookGameplayEvent(BattleTags::Hook_BattleStart, StartContext);

    CheckOutcome();
}

void UBattleWorld::Shutdown()
{
    // M02 §4：先广播 OnBattleEnd，再清理注册表与订阅。
    FBattleHookContext EndContext;
    EndContext.Time = FACBattleTime::ElapsedSeconds(*this);
    EndContext.IntValue = static_cast<int32>(Outcome);
    EventBus.Dispatch(BattleTags::Hook_BattleEnd, EndContext);

    EventBus.Shutdown();
    SystemSubscriptions.Reset();

    // Actor 化（阶段 0a）：单位与棋盘都是关卡里的真实 Actor，必须 Destroy()。
    // 先快照再从注册表里摘掉、最后才 Destroy：EndPlay 里将来会做反注册（阶段 0b），
    // 那时 Destroy 会回调进本类，先摘表可以保证回调看到的是一个干净的空注册表。
    TArray<AACBattleUnitBase*> UnitsToDestroy;
    UnitsToDestroy.Reserve(Units.Num());
    for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
    {
        if (Unit != nullptr)
        {
            UnitsToDestroy.Add(Unit);
        }
    }
    Units.Reset();
    UnitLookup.Reset();
    for (AACBattleUnitBase* Unit : UnitsToDestroy)
    {
        if (IsValid(Unit))
        {
            Unit->Destroy();
        }
    }

    if (BoardActor != nullptr)
    {
        // 单位先于棋盘销毁：单位的位置同步依赖棋盘，反过来则会让组件里的棋盘指针提前失效。
        if (IsValid(BoardActor))
        {
            BoardActor->Destroy();
        }
        BoardActor = nullptr;
    }

    Commands.Reset();
    DelayedMentalRecovers.Reset();
    IterationBuffer.Reset();
    ReadyQueueBuffer.Reset();
}

void UBattleWorld::Step(float DeltaTime)
{
    // §5.2 实现裁决 + §8.2 第 3 项不变量：DeltaTime <= 0 直接返回（不推进、不结算）。
    if (DeltaTime <= 0.f || IsFinished())
    {
        return;
    }

    // 1) 命令（秘术 / 放弃 / 内部请求）。
    ConsumeCommands();

    // 2) 每帧结算（原"整秒派发"，见 TickFrame 的说明）。
    //    时间基准换了，但**顺序一字未改**：先状态 -> 回复 -> 过期清理 -> 周期效果 -> 精神值 -> 延迟回复。
    TickFrame(DeltaTime);

    // 3) 时间轴后发（抢攻已在 `Initialize` → `ApplyPreBattleConfiguration` 里施加完毕）。
    //    位置与旧 `TimelineSystem.Update()` 逐位一致：在调度推进之前。
    TickPendingTimelineFires();

    // 4) 行动调度（唯一使用 DeltaTime 做累积的地方：行动条本身）。
    Scheduler.Advance(DeltaTime);

    // 5) 执行行动（阶段 3.2b：由 `FAbilityExecutor::RequestAction` 改为**能力驱动**）。
    // 先快照就绪队列：行动内的能力可能召唤单位并改动调度器状态。
    ReadyQueueBuffer.Reset();
    ReadyQueueBuffer.Append(Scheduler.GetReadyQueue());
    for (const FUnitId UnitId : ReadyQueueBuffer)
    {
        if (AACBattleUnitBase* Unit = FindUnit(UnitId))
        {
            if (Unit->IsAlive())
            {
                ExecuteActionForUnit(*Unit);
            }
        }
        Scheduler.Consume(UnitId);
    }

    // 6) 专注按帧回复（旧 `FAbilityExecutor::TickStep` 的 `RegenPerSecond` 分支）。
    //
    // 为什么是"每帧 × DeltaTime"而不是"GE 的 `Period = 1` 秒"：旧口径是**平滑累积**
    //（`Focus.Current += FocusRegen * DeltaTime`，夹取到 `[0, FocusMax]`），
    // 而 GE 的 `Period` 会变成"每秒跳一次"：两者的数值轨迹在帧率变化、
    // 以及"这一秒内刚好攒满触发 `Hook.FocusFull`"的边界上都不同。因此保留按帧调用 ——
    // 改值本身仍然走 GE（`GrantFocusToUnit` 内部的 `UACGE_FocusGain`），"改值走 GE"这条契约不破。
    //
    // ⚠️ `FocusRegen` 的默认值是 0（`MakeStatBlock` 的敌人档位就是 0），因此正常内容下
    //    这段是 no-op；**保留它是为了将来配了 `FocusRegen` 的内容不会静默失效**。
    // ⚠️ 用快照遍历：`GrantFocusToUnit` 会派发 `Hook.FocusChanged`，订阅者理论上可以生成单位。
    ForEachAliveSnapshot([DeltaTime](AACBattleUnitBase& Unit)
    {
        const float RegenPerSecond = Unit.GetStat(EACStat::FocusRegen);
        if (RegenPerSecond > 0.f)
        {
            UACBattleAbility::GrantFocusToUnit(Unit, RegenPerSecond * DeltaTime);
        }
    });

    // 7) 死亡处理。
    ResolveDeaths();

    // 8) 结束判定。
    CheckOutcome();
}

void UBattleWorld::TickFrame(float DeltaTime)
{
    // 阶段 0.5：本函数由原 `ExecuteSecondTick(int64 CurrentTick)` 改名而来。
    // 语义变化：① 从"整秒边界跑一次"变成"每帧跑一次"；② 所有"每秒量"乘 DeltaTime；
    //          ③ 所有到期判定改用绝对时间（`FACBattleTime::Now`）而不是 tick 计数。
    // 顺序严格保持原样（见 TechDocs/00A §2），这样"谁先谁后"的行为差异只在时间粒度上。

    // 时间只在本帧取一次：同一帧内多处判定共用同一时刻，
    // 逐处各取会让同一帧内的先后判定出现不等的时间戳（浮点上虽然极小，但不必要）。
    const float NowSeconds = FACBattleTime::Now(*this);

    // -----------------------------------------------------------------------
    // §2.3 第 2 步：驱动每个单位的 ASC
    // -----------------------------------------------------------------------
    // 为什么必须在这里手动调：单位 Actor 关闭了 `PrimaryActorTick`（§2.2），
    // 而 ASC 是 ActorComponent —— 它的 `TickComponent` 由所在 Actor 的 tick 驱动，
    // 因此没有人会替我们调它。`UAbilitySystemComponent::TickComponent` 内部做两件事
    //（AbilitySystemComponent_Abilities.cpp:119-139）：tick 可 Tick 的属性集、tick 能力任务；
    // 经 `UGameplayTasksComponent::TickComponent` 还会 tick 里的 `UGameplayTask`。
    //
    // ⚠️ **周期结算与时长到期不依赖这次调用**（这一点值得写清楚，免得读者以为漏调就会丢 DPS）：
    //    引擎在施加 / 更新活动 GE 时用 `Owner->GetWorld()->GetTimerManager()` 起了定时器
    //    （GameplayEffect.cpp:4228-4252 `SetTimer(..., AppliedEffectSpec.GetPeriod(), true)`），
    //    到期走的是 TimerManager，与 ASC 是否被 tick 无关。
    //    手动 Tick 的意义是"与 §2.3 的目标顺序一致"（先驱动 GAS，再结算内核），
    //    而不是"替代定时器"。
    //
    // 顺序放在 `ForEachAliveSnapshot` 的第一轮：与 §2.3 的
    // "各单位 ASC->TickComponent → HP 回复 → 频率计数清零"一致。
    ForEachAliveSnapshot([DeltaTime](AACBattleUnitBase& Unit)
    {
        if (UAbilitySystemComponent* const ASC = Unit.GetAbilitySystemComponent())
        {
            // ⚠️ 只对**已注册**的组件调 `TickComponent`：`UActorComponent::TickComponent`
            // 开头就是 `check(bRegistered)`（ActorComponent.cpp:1766），
            // 而单位在延迟构造期生成，未 `FinishSpawning` 的单位还没注册组件。
            // 这里用 `IsRegistered()` 而不是"相信流程"，是因为单位生成路径有三条
            //（玩家 / 敌人 / 召唤），任何一条时序改动都会把 check 变成硬崩。
            if (ASC->IsRegistered())
            {
                ASC->TickComponent(DeltaTime, ELevelTick::LEVELTICK_All, nullptr);
            }
        }
    });

    // 阶段 3.2a：原来的
    //   `ForEachAliveSnapshot([](AACBattleUnitBase& Unit){ Unit.GetStates().Tick(NowSeconds); })`
    //   `EffectSystem.TickPeriodic(NowSeconds)`
    // 两轮遍历**已删除**：
    //   · 状态的周期伤害 / 层数消耗改由 `UACPeriodicDamageExecution` 在 GE 的 `Period` 上结算；
    //   · 状态到期改由 GE 自己的时长负责（`RemoveActiveGameplayEffect` 由引擎定时器触发）；
    //   · 施加当帧不结算这一条旧口径由 `UACGameplayEffectBase` 的
    //     `bExecutePeriodicEffectOnApplication = false` 保证。

    ForEachAliveSnapshot([DeltaTime](AACBattleUnitBase& Unit)
    {
        // HP 回复：HpRegen 的口径是"每秒回复 MaxHP 的百分之几"，因此乘 DeltaTime 得到本帧增量。
        const float Regen = Unit.GetStat(EACStat::HpRegen);
        if (Regen > 0.f)
        {
            Unit.AddCurrentHP(Unit.GetMaxHP() * Regen / 100.f * DeltaTime);
        }
    });

    CombatResolver.RemoveExpiredShields();

    // 阶段 3.2a：`CombatResolver.RemoveExpiredDamageModifiers()` 也**已删除** ——
    // 增伤 / 减伤规则（`FACDamageModifier`）的唯一注册者是旧 `FEffectSystem` 的
    // `AddDamageModifier` 动作，随效果系统一起消失；§4.6 的裁决是"GE 的 `Modifiers` 只放
    // 真正要改属性的修饰、伤害走 Execution"，因此不再有第二张"伤害修饰器表"需要清理。

    // 阶段 2（§7 阶段 2）：这里原来的 `FBattleStatPipeline::RemoveExpired(Sheet, Now)` +
    // `Recompute(Sheet)`（对存活单位一遍、对死亡单位一遍）**已删除**。
    // 原因：修饰器数组已经不存在了 —— 属性修饰改由 GAS 承担（§4.3），
    // "到期"由 GE 自己的时长负责、"重算"由属性聚合器自动完成（C8：不写 MMC）。

    MentalSystem.Tick(DeltaTime);

    // 延迟精神回复（我方崩溃后眩晕结束回复 60 / 40）。
    // 到期判定用绝对时间：`DueTime` 在入队时由 `Now + 2` 算好，这里只比较。
    for (int32 Index = DelayedMentalRecovers.Num() - 1; Index >= 0; --Index)
    {
        if (FACBattleTime::IsExpiredAt(NowSeconds, DelayedMentalRecovers[Index].DueTime))
        {
            MentalSystem.GrantMental(DelayedMentalRecovers[Index].Target, DelayedMentalRecovers[Index].Amount);
            DelayedMentalRecovers.RemoveAt(Index);
        }
    }
}

// ---------------------------------------------------------------------------
// 命令
// ---------------------------------------------------------------------------

void UBattleWorld::EnqueueCommand(const FACBattleCommand& Command)
{
    FACBattleCommand Copy = Command;
    Copy.IssuedAtTime = FACBattleTime::Now(*this);
    Commands.Add(Copy);
}

bool UBattleWorld::DequeueCommand(FACBattleCommand& OutCommand)
{
    if (Commands.Num() == 0)
    {
        return false;
    }
    OutCommand = Commands[0];
    Commands.RemoveAt(0);
    return true;
}

void UBattleWorld::ConsumeCommands()
{
    FACBattleCommand Command;
    while (DequeueCommand(Command))
    {
        switch (Command.Type)
        {
        case EACCommandType::AbandonBattle:
            Outcome = EACOutcome::Abandoned;
            break;

        case EACCommandType::UseMysticCard:
        {
            // M01 §5.3：Run 层开关关闭时忽略秘术命令（A14）。
            if (!Options.bAllowMysticCards)
            {
                break;
            }

            // D8 §5.1「必须处理的坑」第 1 条：命令队列跨帧存活，玩家可能在目标单位死后才提交命令，
            // 因此**必须**经 ResolveOrWarn 解引用：目标已销毁时打 Warning 并丢弃该条命令，
            // 不要静默跳过（静默跳过会让"秘术卡点了没反应"变成无法排障的问题）。
            // TargetUnitId == InvalidUnitId 属于正常业务（不指定目标，由效果块选择器自己解析），
            // 这条路径不受影响、也不告警。
            if (Command.TargetUnitId != InvalidUnitId && ResolveOrWarn(Command.TargetUnitId) == nullptr)
            {
                UE_LOG(LogTemp, Warning,
                       TEXT("[Battle] 丢弃秘术命令 %s：目标单位 %d 已销毁。"),
                       *Command.CardId.ToString(), Command.TargetUnitId);
                break;
            }

            // 阶段 3.2a：原来的 `EffectSystem.ExecuteBlock(Command.CardId, Context)` 已删除。
            // 阶段 3.3：牌面效果改由**命令自带 GE 类**表达（`FACBattleCommand::Effects`）——
            // 3.2a 那种"拿 `CardId` 去过渡映射表查"的写法随映射表一起删除。
            // 卡片内容当前是空的（`Run` 层从来没有往 `CardId` 里填过任何东西），
            // 因此 `Effects` 为空 → `ApplyGameplayEffectsToUnit` 直接返回，是**空操作** ——
            // 与旧实现"库里没有这个块就什么都不做"（`Library->FindBlock` 返回 nullptr 分支）逐字等价。
            // 目标语义也保持不变：施加到 `TargetUnitId`（InvalidUnitId 时无从施加，直接跳过）。
            if (AACBattleUnitBase* const CardTarget = FindUnit(Command.TargetUnitId))
            {
                ApplyGameplayEffectsToUnit(*CardTarget, Command.Effects);
            }
            break;
        }

        case EACCommandType::Debug:
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 单位
// ---------------------------------------------------------------------------

const UUnitDefinitionBase* UBattleWorld::FindUnitDefinition(FName DefinitionId) const
{
    const UUnitDefinitionBase* const* Found = DataContext.UnitDefinitions.Find(DefinitionId);
    return Found != nullptr ? *Found : nullptr;
}

UBattleWorld* UBattleWorld::FindFromActor(const AActor* Actor)
{
    if (Actor == nullptr)
    {
        return nullptr;
    }

    // ① 快路径：单位 Actor 的 Outer 直接是 UBattleWorld。
    //    当前装配下**不成立**（SpawnActor 把 Outer 设成 ULevel），保留它是因为零开销、
    //    且将来若有人显式指定 Outer 就自动生效。
    if (UBattleWorld* const OuterWorld = Cast<UBattleWorld>(Actor->GetOuter()))
    {
        return OuterWorld;
    }

    // ② 实际生效路径：Actor → UWorld → UGameInstance → UBattleSubsystem → UBattleSession → UBattleWorld。
    const UWorld* const GameWorld = Actor->GetWorld();
    if (GameWorld == nullptr)
    {
        return nullptr;
    }
    UGameInstance* const GameInstance = GameWorld->GetGameInstance();
    if (GameInstance == nullptr)
    {
        return nullptr;
    }
    const UBattleSubsystem* const Subsystem = GameInstance->GetSubsystem<UBattleSubsystem>();
    if (Subsystem == nullptr)
    {
        return nullptr;
    }
    const UBattleSession* const Session = Subsystem->GetSession();
    return (Session != nullptr) ? Session->GetBattleWorld() : nullptr;
}

void UBattleWorld::PlaceUnitOnGrid(AACBattleUnitBase& Unit, FACHexCoord PreferredCell)
{
    FACHexCoord Cell = PreferredCell;
    if (!GridSystem.IsPlayableCell(Cell) || GridSystem.IsOccupied(Cell))
    {
        if (!GridSystem.FindNearestFreeCell(Cell, Cell))
        {
            // 无处落位：不上场，避免与其它单位共用非法格导致棋盘与位置不一致。
            Unit.SetCell(PreferredCell);
            Unit.bOnBoard = false;
            Unit.SyncGridComponent();   // 阶段 0a：bOnBoard 是公开字段，改完要手动镜像给网格组件
            UE_LOG(LogTemp, Warning, TEXT("[Battle] No free cell for unit %d, kept off board."), Unit.GetUnitId());
            return;
        }
    }
    Unit.SetCell(Cell);
    Unit.bOnBoard = true;
    Unit.SyncGridComponent();           // 同上：落格后把 Cell + bOnBoard 写进网格组件
    if (Unit.bOccupyCell)
    {
        GridSystem.SetOccupant(Cell, Unit.GetUnitId());
    }
}

// ---------------------------------------------------------------------------
// 能力 / GE 授予（阶段 3.2a：替代旧的 `EffectSystem.RegisterBlock`）
// ---------------------------------------------------------------------------

UACAbilitySet* UBattleWorld::FindAbilitySet(FName DefinitionId) const
{
    if (DefinitionId.IsNone())
    {
        return nullptr;
    }
    const TObjectPtr<UACAbilitySet>* Found = DataContext.AbilitySets.Find(DefinitionId);
    return Found != nullptr ? Found->Get() : nullptr;
}

bool UBattleWorld::ApplyGameplayEffectToUnit(AACBattleUnitBase& Unit, TSubclassOf<UGameplayEffect> EffectClass)
{
    if (EffectClass.Get() == nullptr)
    {
        return false;
    }

    UAbilitySystemComponent* const ASC = Unit.GetAbilitySystemComponent();
    if (ASC == nullptr)
    {
        return false;
    }

    // context 由**承受者自己的 ASC** 生成：`MakeEffectContext` 用 OwnerActor/AvatarActor 填
    // instigator，单位 Actor 两者都是自己。这正是 `UACShieldExecution` / `UACSummonEffectComponent`
    // / `AggregateBySource` 状态叠加需要的"来源单位"。
    //
    // ⚠️ 与能力的 `UACBattleAbility::ApplyEffectToUnit` 的唯一差别：那里 source 与 target 是两个
    //    不同的单位（`SourceASC->MakeEffectContext()`），而这里施加方就是承受者
    //    （旧的 `RegisterBlock(BlockId, Owner, Tier)` 与"时间轴条目挂在自己身上"都是这个语义）。
    const FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
    const FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(EffectClass, /*Level=*/1.f, Context);
    if (!SpecHandle.IsValid() || !SpecHandle.Data.IsValid())
    {
        return false;
    }
    ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data);
    return true;
}

void UBattleWorld::ApplyGameplayEffectsToUnit(AACBattleUnitBase& Unit, const TArray<TSubclassOf<UGameplayEffect>>& Effects)
{
    // 空数组是常态（大多数装备 / 词条没有战斗内 GE），不告警。
    // 阶段 3.3 起这里**没有**"内容映射表为空"这个前置条件了 —— 那张过渡表已删除，
    // 契约里拿到的就是类本身。
    if (Effects.Num() == 0)
    {
        return;
    }

    for (const TSubclassOf<UGameplayEffect>& EffectClass : Effects)
    {
        if (EffectClass.Get() == nullptr)
        {
            // 内容侧数组里留了空位（编辑器里加了一行但没选类）。静默跳过：
            // 这是最常见的"配了一半"的形态，逐条告警会随单位数量线性放大。
            continue;
        }

        // 施加动作收口到 `ApplyGameplayEffectToUnit`（与时间轴抢攻 / 后发、预战配置共用同一份实现）。
        ApplyGameplayEffectToUnit(Unit, EffectClass);
    }
}

// ---------------------------------------------------------------------------
// 行动执行（阶段 3.2b：`FAbilityExecutor::RequestAction` 的能力驱动替代）
// ---------------------------------------------------------------------------

FGameplayAbilitySpecHandle UBattleWorld::FindReadyAbilityHandle(UAbilitySystemComponent& ASC, const UClass* AbilityClass,
                                                                const AACBattleUnitBase& Caster)
{
    if (AbilityClass == nullptr)
    {
        return FGameplayAbilitySpecHandle();
    }

    // 引擎 API（已核实）：
    //   `UAbilitySystemComponent::GetActivatableAbilities()` 返回 `TArray<FGameplayAbilitySpec>&`
    //   —— 声明在 `AbilitySystemComponent.h:1128`（const 版）/ `:1134`（非 const 版），
    //      实现就是 `return ActivatableAbilities.Items;`（`FGameplayAbilitySpecContainer::Items`，
    //      `GameplayAbilitySpec.h:307`）。因此**不需要**自己去摸 `ActivatableAbilities` 容器。
    //   `FGameplayAbilitySpec::Ability` 是 `TObjectPtr<UGameplayAbility>`（`GameplayAbilitySpec.h:198`），
    //      且注释明写 "Always the CDO" —— 这就是"`IsReadyToActivate` 只能读形参"的根据。
    //   `FGameplayAbilitySpec::Handle` 在 `GameplayAbilitySpec.h:194`；
    //   `FGameplayAbilitySpecHandle::IsValid()` 在 `GameplayAbilitySpecHandle.h:25`（默认构造 = INDEX_NONE）。
    const TArray<FGameplayAbilitySpec>& Specs = ASC.GetActivatableAbilities();
    for (const FGameplayAbilitySpec& Spec : Specs)
    {
        // 按**基类**分类：技能 = `UACSkillAbilityBase` 子类；普攻 = `UACBasicAttackAbility`。
        // 两者都派生自 `UACBattleAbility`，而被动（`UACPassiveAbilityBase`）也是 ——
        // 因此这里必须用**具体基类**判，不能用 `UACBattleAbility`（那会把被动也当成技能）。
        const UACBattleAbility* const Ability = Cast<UACBattleAbility>(Spec.Ability.Get());
        if (Ability == nullptr || !Ability->IsA(AbilityClass))
        {
            continue;
        }
        if (!Ability->IsReadyToActivate(Caster))
        {
            continue;
        }
        return Spec.Handle;
    }
    return FGameplayAbilitySpecHandle();
}

void UBattleWorld::MaintainTarget(AACBattleUnitBase& Unit)
{
    // 来源：旧 `FAbilityExecutor::RequestAction` 的 :145-191。分成两半，逐项等价：
    //   ① 治疗型普攻（蓝血）：永远不选敌方，改选友方最低血（M11 §5.1 / M14 §1.3）——
    //      旧实现每次行动都重选并把结果写回 `CurrentTargetId`，这里照做；
    //   ② 普通单位：目标无效则 `AcquireTarget` 重新索敌，且"目标真的变了"时派发
    //      `Hook.TargetChanged`（旧判据：`新 != 旧 && 新 != InvalidUnitId`）。
    //
    // ⚠️ **这段为什么必须留在内核（本阶段最容易漏掉的一处）**：
    //   新能力**只解析、不索敌**。`UACBattleAbility::CanActivateAbility` → `ResolveTargets`
    //   → `FTargetingSystem::ResolveSelector(PrimaryTarget)` 读的是 `Unit.GetCurrentTargetId()`
    //   （`ACTargetingSystem.cpp:303-308` 的 `PrimaryTarget` 分支），目标失效时它**不会**
    //   替单位重新选一个（这正是"普攻/技能"与"索敌"的分工）。因此"目标无效则重新索敌 +
    //   派发 `Hook.TargetChanged`"这件事没有任何能力承担 —— 把它留在内核是唯一正确的落点；
    //   漏掉它，单位在第一个目标死掉之后就**永远不会再攻击**（表现为"打着打着全体发呆"）。
    const FACAttackPatternDef& Pattern = Unit.GetAttackPattern();

    if (Pattern.bHealAttack && Pattern.TargetTeam == Unit.GetTeam())
    {
        // 注意 `EACTeam` 没有 "Ally" 枚举项（Player/Enemy/Neutral），"友方" = 与自身同阵营。
        TArray<FUnitId> AllyTargets;
        TargetingSystem.ResolveSelector(EACSelectorType::LowestHpPercentAlly, Unit, 1, AllyTargets);
        if (AllyTargets.Num() > 0)
        {
            Unit.SetCurrentTargetId(AllyTargets[0]);
        }
        // 治疗型**不**走下面的"重新索敌 + Hook.TargetChanged"：旧实现同样在 :170 直接 return。
        return;
    }

    const FUnitId CurrentTarget = Unit.GetCurrentTargetId();
    if (CurrentTarget != InvalidUnitId && TargetingSystem.IsTargetValid(Unit, CurrentTarget))
    {
        return;   // 锁定仍有效：什么都不做（旧实现在 :174 的条件判断里就是这个意思）
    }

    const FUnitId NewTarget = TargetingSystem.AcquireTarget(Unit);
    Unit.SetCurrentTargetId(NewTarget);

    if (NewTarget != CurrentTarget && NewTarget != InvalidUnitId)
    {
        FBattleHookContext TargetContext;
        TargetContext.Source = Unit.GetUnitId();
        TargetContext.Target = NewTarget;
        TargetContext.Time = FACBattleTime::ElapsedSeconds(*this);
        TargetContext.IntValue = CurrentTarget;   // 旧值：换了目标时"从谁换过来"
        EventBus.Dispatch(BattleTags::Hook_TargetChanged, TargetContext);
    }
}

void UBattleWorld::ExecuteActionForUnit(AACBattleUnitBase& Unit)
{
    // §2.3 第 5 步的实现。与旧 `FAbilityExecutor::RequestAction` 的对应关系（**顺序也逐一对应**）：
    //   | 旧（ACAbilityExecutor.cpp）                  | 新                                            |
    //   | :121-124 `CanAct` 门控                        | 下面那三行（死亡 / 待死 / 眩晕标签）             |
    //   | :126-145 技能优先（专注满 + 目标可解析 + 射程） | ① `IsReadyToActivate` + `TryActivateAbility`   |
    //   | :174-187 索敌 + `Hook.TargetChanged`          | ② `MaintainTarget`                            |
    //   | :147-207 普攻 / 移动                          | ③ 普攻（技能失败即 fallback）④ 移动              |
    // ⚠️ "索敌"排在技能判定**之后**是旧实现的顺序（不是笔误）：旧技能用的是"上一次留下的
    //    `CurrentTargetId`"，目标失效时技能本就会判不过并落到普攻 —— 把索敌提前会让
    //    "开场第一次行动"多出一次旧版本没有的目标解析（见 `MaintainTarget` 的说明）。
    UAbilitySystemComponent* const ASC = Unit.GetAbilitySystemComponent();
    if (ASC == nullptr)
    {
        return;
    }

    // 行动资格：旧 `CanAct` 只保留了"死亡 / 待死 / 眩晕"三条 ——
    // 剩下那条"行动状态属于 Idle|Moving|Attacking"是**调度器**的职责（没被 gate 挡住的单位
    // 才会进就绪队列），内核再判一次只会让"到底谁挡住了这次行动"变得难查。
    // 眩晕改查 ASC 标签：`UACGE_State_Stun` 是 `HasDuration` GE，到期由引擎摘掉、标签自动消失，
    // 内核不再需要任何计时器或到期解除逻辑（旧 `FStunEntry` + `TickStep` 已删除）。
    if (Unit.IsDead() || Unit.IsPendingDeath() || Unit.HasStateTag(BattleTags::State_Stun))
    {
        return;
    }

    // ① 技能：`IsReadyToActivate`（技能基类 = A15"专注满"）通过才尝试。
    //    `TryActivateAbility` 内部还会走 `CanActivateAbility`（阻塞标签 / 冷却 / Cost / 目标射程），
    //    因此这里的判定与引擎的门控不重叠：前者是"该不该去试"，后者是"试了能不能成"。
    const FGameplayAbilitySpecHandle SkillHandle = FindReadyAbilityHandle(*ASC, UACSkillAbilityBase::StaticClass(), Unit);
    if (SkillHandle.IsValid() && ASC->TryActivateAbility(SkillHandle))
    {
        return;   // 技能放出去了：本帧行动结束
    }

    // ② 索敌与目标维护（旧实现同样在技能判定之后、普攻之前，见函数头的顺序表）。
    //    技能失败之后这里才动手，因此"技能为什么失败"不会被一次重新索敌掩盖掉。
    MaintainTarget(Unit);

    // ③ 普攻。**这一段是 §2.3 明确要求必须覆盖的核心分支**：
    //    `TryActivateAbility` 在 `CanActivateAbility` 判不过时返回 **false**
    //    （`AbilitySystemComponent.h:1062-1068` 的注释：会先检查 cost 与 requirements），
    //    而技能失败的原因可能是"主目标这一帧死了 / 它跑出了射程 / 被沉默挡住 / Cost 付不起"——
    //    那些情况下单位**仍然应该普攻**，否则它会站在原地直到技能条件重新满足
    //    （表现为"专注满了但技能放不出，于是整场不动"，这是改造前不存在的行为）。
    //    ⚠️ 反向**不**成立：普攻失败后不再回头试技能 —— 技能前提是"专注满"，同一帧内不会改变，
    //       再试一次只会是无意义的重复判定。
    const FGameplayAbilitySpecHandle BasicAttackHandle =
        FindReadyAbilityHandle(*ASC, UACBasicAttackAbility::StaticClass(), Unit);
    if (BasicAttackHandle.IsValid() && ASC->TryActivateAbility(BasicAttackHandle))
    {
        return;   // 普攻放出去了
    }

    // ④ 移动：连普攻都放不出（超距 / 无合法目标）→ 朝当前目标走一格。
    //    `MoveOneStepTowards` 内部已经在"已在期望射程内"时返回 false（`FindPathToRange` 在
    //    `Distance <= DesiredRange` 时返回"空路径"，见 `ACBattleGrid.cpp:100-103`），
    //    因此这里**不需要**再自己判一次距离 —— 旧实现的 `IsInRange` 判断在它内部等价表达。
    const FUnitId TargetId = Unit.GetCurrentTargetId();
    if (TargetId != InvalidUnitId)
    {
        UACBattleAbility::MoveOneStepTowards(*this, Unit, TargetId);
    }
}

int32 UBattleWorld::BroadcastHookGameplayEvent(FGameplayTag HookTag, const FBattleHookContext& Context)
{
    if (!HookTag.IsValid())
    {
        return 0;
    }

    int32 TriggeredCount = 0;
    // 快照遍历（不是 `ForEachAlive`）：母巢的 `Hook.BattleStart` 被动会在响应里生成 4 只蠕虫，
    // 直接遍历 `Units` 数组会让迭代器失效（新增元素可能触发重新分配）。
    ForEachAliveSnapshot([this, HookTag, &Context, &TriggeredCount](AACBattleUnitBase& Unit)
    {
        UAbilitySystemComponent* const ASC = Unit.GetAbilitySystemComponent();
        if (ASC == nullptr)
        {
            return;
        }
        // 载荷里的 `Instigator` / `Target` 由整型句柄还原成 Actor —— 映射只有一份实现
        //（`UACBattleAbility::MakeEventPayload` 的 static 重载），内核与能力不会分叉。
        FGameplayEventData Payload = UACBattleAbility::MakeEventPayload(*this, Context);
        Payload.EventTag = HookTag;

        // `HandleGameplayEvent`（`AbilitySystemComponent.h:756`）按 Tag 找
        // `GameplayEventTriggeredAbilities` 里登记的能力并逐条触发，返回触发条数。
        TriggeredCount += ASC->HandleGameplayEvent(HookTag, &Payload);
    });
    return TriggeredCount;
}

void UBattleWorld::RegisterUnit(AACBattleUnitBase& Unit)
{
    // Actor 化（阶段 0a）：顺序数组与 Id 查表必须在这里成对增加，
    // 否则 FindUnit 与 ForEachAlive 会看到不一致的注册表。
    Units.Add(&Unit);
    UnitLookup.Add(Unit.GetUnitId(), &Unit);
    Unit.SetSpawnTime(FACBattleTime::Now(*this));

    // -----------------------------------------------------------------------
    // 定义自带的内容：能力 + 常驻 GE（阶段 3.2a）
    // -----------------------------------------------------------------------
    // 替代原来的：
    //   `for (const FName& BlockId : Definition->PassiveEffectBlockIds) EffectSystem.RegisterBlock(...)`
    // 阶段 3.2b：旧字段 `PassiveEffectBlockIds` 本身**已删除**（与 `FACSkillDef` 一起收口）——
    // 被动内容的唯一入口就是 `UACAbilitySet`（能力类 + GE 类），定义资产上不再有第二份名单。
    //
    // ⚠️ 为什么在这里授予、而不是在 `Initialize` 的 `FinishSpawning` 之前：
    //   `GrantToOwner` 里的 `GiveAbility` → `OnGiveAbility` → `CanActivateAbility` 会用到
    //   ActorInfo 的 Avatar。被动能力全部覆写 `RequiresTargetInRange() → false`（不解析目标），
    //   但其基类仍会走"取世界 / 取施法者"；而 `PlaceUnitOnGrid` 决定了单位所在的格 ——
    //   格子没落定就授予，等于让第一个被授予的能力在一个位置未定的单位上做判定。
    //   `RegisterUnit` 在三条生成路径里都在 `PlaceUnitOnGrid` **之后**，因此这里是"数据 + 位置都就绪"
    //   的第一个时刻，也紧贴着"单位正式参战"的语义。
    if (UACAbilitySetComponent* const AbilitySetComponent = Unit.GetAbilitySetComponent())
    {
        UACAbilitySet* const AbilitySet = FindAbilitySet(Unit.GetDefinitionId());
        if (AbilitySet == nullptr)
        {
            // ⚠️ 阶段 3.2b 起这是**内容漏配**，不再是正常状态：
            //   "行动"完全由能力驱动（`ExecuteActionForUnit`：技能能力 → 普攻能力 → 移动），
            //   没有清单的单位**连普攻都不会**，会站在原地一动不动。
            //   静默跳过会让"某个单位整场不动"变成一场没有线索的排查，所以留一条 Warning。
            UE_LOG(LogTemp, Warning,
                   TEXT("[Battle][GAS] 单位定义 %s（UnitId=%d）在 AbilitySets 里没有清单，"
                        "它将一个能力都没有、整场无法行动。"),
                   *Unit.GetDefinitionId().ToString(), Unit.GetUnitId());
        }
        AbilitySetComponent->SetAbilitySet(AbilitySet);
        AbilitySetComponent->GrantToOwner();
    }

    FBattleHookContext SpawnContext;
    SpawnContext.Target = Unit.GetUnitId();
    SpawnContext.Time = FACBattleTime::ElapsedSeconds(*this);
    EventBus.Dispatch(BattleTags::Hook_Spawn, SpawnContext);
}

TSubclassOf<AACBattleUnitBase> UBattleWorld::ResolveUnitClass(EACTeam Team, EACUnitKind Kind) const
{
    // 召唤物 / 分身不分阵营，一律走召唤物基类。
    if (Kind == EACUnitKind::Summon || Kind == EACUnitKind::Clone)
    {
        return AACSummonActor::StaticClass();
    }
    if (Team == EACTeam::Player && Kind == EACUnitKind::Operator)
    {
        return AACOperatorActor::StaticClass();
    }
    if (Team == EACTeam::Enemy && Kind == EACUnitKind::Enemy)
    {
        return AACEnemyActor::StaticClass();
    }

    // 其他组合（内容侧将来可用 Definition 覆盖）：回落到单位基类。
    // 基类**刻意不标 Abstract**，正是为了让这条回落能真的 SpawnActor 成功。
    return AACBattleUnitBase::StaticClass();
}

AACBattleUnitBase* UBattleWorld::SpawnUnitActor(TSubclassOf<AACBattleUnitBase> UnitClass)
{
    UWorld* const GameWorld = GetWorld();
    if (GameWorld == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] SpawnUnitActor skipped: UBattleWorld has no UWorld."));
        return nullptr;
    }

    // 阶段 0a：三个内容侧基类（AACOperatorActor / AACEnemyActor / AACSummonActor）都还是 Abstract 空壳，
    // 而 ULevel::SpawnActor 会直接拒绝 Abstract 类（返回 nullptr 并告警）。
    // 因此这里把 Abstract 的解析结果回落到可实例化的 AACBattleUnitBase ——
    // 内容侧开始继承它们（阶段 0b/0c）之后，ResolveUnitClass 的映射表才会真正生效。
    if (UnitClass.Get() == nullptr || UnitClass->HasAnyClassFlags(CLASS_Abstract))
    {
        UnitClass = AACBattleUnitBase::StaticClass();
    }

    // 为什么不直接用 UWorld::SpawnActorDeferred<T>：该模板内部自建 FActorSpawnParameters，
    // 无法传入 ObjectFlags，而战斗单位必须是 RF_Transient（不参与关卡存档，§2.2）。
    // 这里用 SpawnActor + bDeferConstruction，语义与 SpawnActorDeferred 完全一致：
    // 出生后不跑构造脚本 / BeginPlay，等调用方填完数据再 FinishSpawning。
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    SpawnParams.ObjectFlags |= RF_Transient;
    SpawnParams.bDeferConstruction = true;

    AACBattleUnitBase* Unit = Cast<AACBattleUnitBase>(GameWorld->SpawnActor(UnitClass, &FTransform::Identity, SpawnParams));
    if (Unit == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] Failed to spawn battle unit actor (%s)."), *GetNameSafe(UnitClass.Get()));
    }
    return Unit;
}

FUnitId UBattleWorld::SpawnUnit(const FACUnitSpawnRequest& Request)
{
    AACBattleUnitBase* Unit = SpawnUnitActor(ResolveUnitClass(Request.Team, Request.Kind));
    if (Unit == nullptr)
    {
        // 生成失败：返回 InvalidUnitId，语义与"查不到这个单位"一致，调用方据此跳过。
        return InvalidUnitId;
    }

    Unit->SetUnitId(NextUnitId++);
    Unit->SetDebugId(Unit->GetUnitId());

    TArray<float> InheritedStats;
    const TArray<float>* InheritedPtr = nullptr;
    if (Request.bInheritFromOwner)
    {
        if (const AACBattleUnitBase* Owner = FindUnit(Request.OwnerUnitId))
        {
            FBattleStatPipeline::SnapshotValues(Owner->GetStatsSheet(), InheritedStats);
            InheritedPtr = &InheritedStats;
        }
    }

    Unit->InitializeFromSpawnRequest(Request, FindUnitDefinition(Request.DefinitionId), InheritedPtr);
    // 阶段 3.2a：状态容器的 `Initialize` 已删除（理由同玩家 / 敌人两处）。

    if (Request.DurationSeconds > 0.f)
    {
        // 召唤物到期：绝对时间 = 现在 + 持续秒数（原来走 SecondsToTicks，现在直接加秒）。
        Unit->SetExpireTime(FACBattleTime::Now(*this) + Request.DurationSeconds);
    }

    Unit->FinishSpawning(FTransform::Identity);

    if (UACUnitGridComponent* UnitGrid = Unit->GetGridComponent())
    {
        UnitGrid->SetBoardActor(BoardActor);
    }

    PlaceUnitOnGrid(*Unit, Request.PreferredCell);
    RegisterUnit(*Unit);

    // 生成请求自带的 GE（召唤物的 `FACSummonSpec::OnSpawnEffects`）。
    // 阶段 3.3：契约字段已是 `TArray<TSubclassOf<UGameplayEffect>>`，
    // 原来这里经过渡映射表按 `FName` 查表的环节整体消失。
    ApplyGameplayEffectsToUnit(*Unit, Request.Effects);

    return Unit->GetUnitId();
}

void UBattleWorld::SpawnSummonFromSpec(FName SummonSpecId, FUnitId OwnerUnitId)
{
    const FACSummonSpec* Spec = DataContext.SummonSpecs.Find(SummonSpecId);
    if (Spec == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] SummonSpec not found: %s"), *SummonSpecId.ToString());
        return;
    }

    AACBattleUnitBase* Owner = FindUnit(OwnerUnitId);

    FACUnitSpawnRequest Request;
    Request.Kind = EACUnitKind::Summon;
    Request.Team = Owner != nullptr ? Owner->GetTeam() : EACTeam::Player;
    Request.DefinitionId = Spec->DefinitionId;
    Request.OwnerUnitId = OwnerUnitId;
    Request.TierIndex = 0;
    Request.PreferredCell = Owner != nullptr ? Owner->GetCell() : FACHexCoord(7, 0);
    Request.Facing = Owner != nullptr ? Owner->GetFacing() : EACFacing::Up;
    Request.bInheritFromOwner = (Spec->InheritMode == EACSummonInheritMode::Snapshot);
    Request.InheritRatio = Spec->InheritRatio;
    Request.DurationSeconds = Spec->DurationSeconds;
    Request.bOccupyCell = Spec->bOccupyCell;
    Request.bSelectable = Spec->bSelectable;
    Request.bCountsAsKill = Spec->bCountsAsKill;
    Request.Effects = Spec->OnSpawnEffects;

    if (const FACStatBlock* BaseStats = DataContext.SummonBaseStats.Find(Spec->DefinitionId))
    {
        Request.BaseStats = *BaseStats;
    }

    SpawnUnit(Request);
}

void UBattleWorld::EnqueueDelayedMentalRecover(FUnitId Target, float Amount, float DueTime)
{
    // 由 FMentalSystem::HandleBreak 登记：眩晕结束（2 秒）后一次性回复精神值。
    // 阶段 0.5：到期判定改按绝对时间（秒），因此不再有"每秒遍历一次、误差 < 1 秒"的粒度损失 ——
    // 现在是每帧检查，实际误差只剩一帧。
    if (Target == InvalidUnitId || Amount <= 0.f)
    {
        return;
    }

    FDelayedMentalRecover Entry;
    Entry.Target = Target;
    Entry.Amount = Amount;
    Entry.DueTime = DueTime;
    DelayedMentalRecovers.Add(Entry);
}

void UBattleWorld::KillUnit(FUnitId UnitId, EACDamageReason Cause)
{
    if (AACBattleUnitBase* Unit = FindUnit(UnitId))
    {
        Unit->SetLastDeathCause(Cause);
        Unit->MarkPendingDeath(InvalidUnitId);
    }
}

void UBattleWorld::ReviveUnit(FUnitId UnitId)
{
    AACBattleUnitBase* Unit = FindUnit(UnitId);
    if (Unit == nullptr)
    {
        return;
    }
    // M02 §1.3：可恢复"同一步内已标记死亡"的单位，因此 pending 与 dead 都要接受。
    if (!Unit->IsDead() && !Unit->IsPendingDeath())
    {
        return;
    }

    // 复活前清理已过期的临时状态，避免带着过期护盾/修饰器回到场上。
    CombatResolver.RemoveExpiredShields();
    Unit->GetShields().Clear();

    // 阶段 2（§7 阶段 2）：这里原来的 `FBattleStatPipeline::RemoveExpired` /
    // `ClearBattleTemp` / `Recompute`（三个调用）**已删除**，换成"清理该单位身上**有持续时间的** GE"。
    //
    // 取舍说明（任务 4 要求"选一种并注释"）：
    //   - **不选** `ASC->RemoveActiveEffectsWithTags(...)`：那需要先给 GE 定一套"临时效果"标签口径，
    //     而本阶段一个 GE 都还没有，定出来的标签必然是凭空猜的（阶段 3 很可能推翻）；
    //   - **选**"遍历该单位的活跃 GE，按 `DurationPolicy` 判断"。语义上它最接近旧的
    //     `ClearBattleTemp`（旧口径正是"清掉 BattleTemp 这一档的临时修饰"），
    //     而且判据来自 GE 资产自身（`HasDuration` vs `Infinite`），不依赖任何新约定。
    //
    // 为什么保留 `Infinite` 的 GE：那是 `EACModScope::Permanent` / `BattlePermanent` 的对应物
    //（§4.3：三档 scope 收敛为 GE `DurationPolicy`），旧代码的 `ClearBattleTemp` 也只清 BattleTemp 一档。
    // 于是"永久增益不因复活而丢"这条旧行为被原样保住。
    //
    // ⚠️ 本阶段**没有任何 GE 会落进来**，因此这是一段"将来 GE 生效后才有实际作用"的安全实现：
    //    遍历到空数组即返回，不 `check`、不告警（复活本身是正常玩法，不是异常路径）。
    RemoveDurationGameplayEffects(*Unit);

    Unit->ClearPendingDeath();
    Unit->Revive();
    PlaceUnitOnGrid(*Unit, Unit->GetCell());
    FBattleHookContext Context;
    Context.Target = UnitId;
    Context.Time = FACBattleTime::ElapsedSeconds(*this);
    EventBus.Dispatch(BattleTags::Hook_Revive, Context);

    // 埋点：只记 Target（复活没有"来源单位"这一概念；复活由效果块 / 组件发起，不是某个单位干的事）。
    // 记在 Hook_Revive 之后：钩子里的逻辑（重新施加开局增益等）已经跑完，这一条代表"复活已完成"。
    LogWorldEvent(*this, LogCategory_Unit, LogEvent_Revive, InvalidUnitId, UnitId);
}

AACBattleUnitBase* UBattleWorld::FindUnit(FUnitId UnitId) const
{
    if (UnitId == InvalidUnitId)
    {
        return nullptr;
    }
    // Actor 化（阶段 0a）：UnitIndex（下标表）已被 UnitLookup（Id → Actor）取代，
    // 查找从"查下标再索引数组"变成一次 O(1) 查表，签名与语义不变。
    const TObjectPtr<AACBattleUnitBase>* Found = UnitLookup.Find(UnitId);
    return Found != nullptr ? Found->Get() : nullptr;
}

// ---------------------------------------------------------------------------
// D8 弱指针解引用统一入口（§5.1）
// ---------------------------------------------------------------------------

AACBattleUnitBase* UBattleWorld::Resolve(const TWeakObjectPtr<AACBattleUnitBase>& WeakUnit) const
{
    // 弱指针无效是**常态**（例如"无来源伤害"的 Source 本来就是空的），
    // 所以这里刻意不打日志：真正的异常由 ResolveOrWarn 负责告警。
    return WeakUnit.Get();
}

AACBattleUnitBase* UBattleWorld::ResolveOrWarn(const TWeakObjectPtr<AACBattleUnitBase>& WeakUnit)
{
    if (!WeakUnit.IsValid())
    {
        // 走到这里说明调用方期望有一个单位：弱指针非空但已失效 = Actor 已销毁（悬垂），
        // 是需要留痕的异常。打印类型名而不是地址，便于人工定位是哪类单位。
        ++ResolveMissedCount;
        UE_LOG(LogTemp, Warning, TEXT("[Battle] ResolveOrWarn: %s 已被销毁（悬垂弱指针），按无目标处理。"),
               *WeakUnit->GetName());
        return nullptr;
    }
    return WeakUnit.Get();
}

AACBattleUnitBase* UBattleWorld::ResolveOrWarn(FUnitId UnitId)
{
    if (UnitId == InvalidUnitId)
    {
        // "没给目标"是正常业务（例如无来源的环境伤害），不算异常，不打日志。
        return nullptr;
    }

    AACBattleUnitBase* Unit = FindUnit(UnitId);
    if (Unit == nullptr)
    {
        // 有效 UnitId 却查不到 = 该单位已被销毁，而某个跨帧存活的句柄还在引用它。
        // 命令队列消费（ConsumeCommands）就依赖这条告警来暴露"残留的失效目标"。
        ++ResolveMissedCount;
        UE_LOG(LogTemp, Warning, TEXT("[Battle] ResolveOrWarn: UnitId %d 已不在注册表中（目标已销毁），按无目标处理。"),
               UnitId);
    }
    return Unit;
}

void UBattleWorld::ForEachAlive(TFunctionRef<void(AACBattleUnitBase&)> Func) const
{
    for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
    {
        if (Unit != nullptr && Unit->IsAlive())
        {
            Func(*Unit);
        }
    }
}

void UBattleWorld::ForEachUnit(TFunctionRef<void(AACBattleUnitBase&)> Func) const
{
    for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
    {
        if (Unit != nullptr)
        {
            Func(*Unit);
        }
    }
}

void UBattleWorld::ForEachAliveSnapshot(TFunctionRef<void(AACBattleUnitBase&)> Func)
{
    // 先快照 UnitId：回调内可能 Spawn/Kill（效果块 SummonUnit 等），直接遍历 Units 会悬垂。
    IterationBuffer.Reset();
    IterationBuffer.Reserve(Units.Num());
    for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
    {
        if (Unit != nullptr && Unit->IsAlive())
        {
            IterationBuffer.Add(Unit->GetUnitId());
        }
    }

    for (const FUnitId UnitId : IterationBuffer)
    {
        if (AACBattleUnitBase* Unit = FindUnit(UnitId))
        {
            if (Unit->IsAlive())
            {
                Func(*Unit);
            }
        }
    }
}

int32 UBattleWorld::GetAliveCount(EACTeam Team) const
{
    int32 Count = 0;
    for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
    {
        if (Unit != nullptr && Unit->IsAlive() && Unit->GetTeam() == Team)
        {
            ++Count;
        }
    }
    return Count;
}

bool UBattleWorld::IsTeamWiped(EACTeam Team) const
{
    for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
    {
        if (Unit == nullptr || !Unit->IsAlive() || Unit->GetTeam() != Team)
        {
            continue;
        }
        // 失败判定只统计"参战干员"；敌方全灭统计所有单位。
        if (Team == EACTeam::Player && Unit->GetKind() != EACUnitKind::Operator)
        {
            continue;
        }
        return false;
    }
    return true;
}

void UBattleWorld::RemoveUnitFromRegistry(FUnitId UnitId)
{
    const TObjectPtr<AACBattleUnitBase>* Found = UnitLookup.Find(UnitId);
    if (Found == nullptr)
    {
        return;
    }

    // Actor 化（阶段 0a）：
    //   ① 原 UnitIndex 的"只修补被删位置之后的下标"优化随 UnitIndex 一起删除——
    //      UnitLookup 是 Id→Actor 的直接映射，删一个键即可，不需要任何下标修补；
    //   ② Units 用 RemoveAll 按指针删，保持其余元素的相对顺序（= 参战顺序）；
    //   ③ 必须显式 Destroy()：UObject 时代"摘出注册表"就等于丢掉最后一个强引用、由 GC 回收，
    //      Actor 不会因为离开数组而消失，不 Destroy 就会一直留在关卡里。
    AACBattleUnitBase* Unit = Found->Get();
    UnitLookup.Remove(UnitId);
    if (Unit != nullptr)
    {
        Units.RemoveAll([Unit](const TObjectPtr<AACBattleUnitBase>& Entry)
        {
            return Entry.Get() == Unit;
        });
        if (IsValid(Unit))
        {
            Unit->Destroy();
        }
    }

    // 同步释放行动槽，避免槽位随召唤物无限增长。
    Scheduler.RemoveSlot(UnitId);
}

// ---------------------------------------------------------------------------
// 死亡处理
// ---------------------------------------------------------------------------

void UBattleWorld::ResolveDeaths()
{
    // 1) 到期的召唤物（阶段 0.5：判定改按绝对时间，负值 = 永不过期）。
    {
        const float NowSeconds = FACBattleTime::Now(*this);
        for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
        {
            if (Unit != nullptr && Unit->IsAlive() && Unit->GetExpireTime() >= 0.f
                && FACBattleTime::IsExpiredAt(NowSeconds, Unit->GetExpireTime()))
            {
                Unit->MarkPendingDeath(InvalidUnitId);
            }
        }
    }

    // 2) 统一处理待死亡单位（注册表顺序 = UnitId 升序）。
    TArray<FUnitId> Deaths;
    for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
    {
        if (Unit != nullptr && Unit->IsPendingDeath())
        {
            Deaths.Add(Unit->GetUnitId());
        }
    }

    for (const FUnitId DeadId : Deaths)
    {
        AACBattleUnitBase* Unit = FindUnit(DeadId);
        if (Unit == nullptr || Unit->IsDead())
        {
            continue;
        }

        const FUnitId KillerId = Unit->GetLastDamageSourceId();
        Unit->MarkDead();

        // 埋点：Source = 击杀者（可能为 InvalidUnitId，例如召唤物到期或环境致死），
        //       Target = 死者，IntValue = EACDamageReason（为什么死）。
        // 位置紧跟 MarkDead()：这是"待死 -> 已死"的唯一翻转点，先于下面所有钩子，
        // 因此无论钩子做什么，这条记录都已经成立（不会记到被取消的状态）。
        LogWorldEvent(*this, LogCategory_Unit, LogEvent_Death, KillerId, DeadId,
                      static_cast<int32>(Unit->GetLastDeathCause()));

        GridSystem.ClearOccupantByUnit(DeadId);
        EventBus.UnsubscribeAllByOwner(DeadId);
        // 阶段 3.2a：原来的 `EventBus.UnsubscribeAllByOwner(DeadId)` 下面还有
        // `EffectSystem.UnregisterAllByOwner(DeadId)` —— 效果实例表已随效果系统删除。
        // 现在"内容随死亡一起消失"由 GAS 保证：GE 挂在单位的 ASC 上，
        // 单位 Actor 一销毁，ASC 与它的活动 GE 一起释放（`RemoveUnitFromRegistry` → `Destroy()`）。

        if (Unit->bCountsAsKill && Unit->GetTeam() == EACTeam::Enemy)
        {
            StatsCollector.RecordKill(KillerId, DeadId, Unit->GetTags());
        }

        // 击杀归属只对敌方单位成立（召唤物/分身按 bCountsAsKill 由内容声明）。
        if (Unit->GetTeam() == EACTeam::Enemy && KillerId != InvalidUnitId)
        {
            FBattleHookContext KillContext;
            KillContext.Source = KillerId;
            KillContext.Target = DeadId;
            KillContext.Time = FACBattleTime::ElapsedSeconds(*this);
            KillContext.IntValue = static_cast<int32>(Unit->GetKind());
            EventBus.Dispatch(BattleTags::Hook_Kill, KillContext);

            // 玩法被动的唤醒（阶段 3.2b）：**广播给全场**，不是只发给击杀者。
            // 依据：旧 `FEffectSystem::HandleHook` 不按 owner 过滤 —— 任何单位死亡都会遍历到
            // 所有监听 `Hook.Kill` 的效果块，因此"心流"（`UACPassive_Trait_FocusSurge`，每次击杀
            // 回 15 专注）的触发面是"持有该词条的单位"而不是"击杀者本人"；
            // S+ 强化"处刑"（`UACPassive_Upgrade_OP01_S1`）同理，它靠 `Context.Source`（= 击杀者）
            // 自己判"这一杀算不算我的"，不依赖投递范围。只发给 Source 会把触发面**收窄**，
            // 那是一条旧版本没有的行为变更。
            BroadcastHookGameplayEvent(BattleTags::Hook_Kill, KillContext);
        }

        FBattleHookContext DeathContext;
        DeathContext.Source = KillerId;
        DeathContext.Target = DeadId;
        DeathContext.Time = FACBattleTime::ElapsedSeconds(*this);
        DeathContext.IntValue = static_cast<int32>(Unit->GetLastDeathCause());
        EventBus.Dispatch(BattleTags::Hook_Death, DeathContext);

        // 同上：`Hook.Death` 也**广播给全场**（腐殖蛆的 `UACPassive_Enemy_Worm_DeathToxin`
        // 监听它，并用 `Context.Target == 自己` 判"死的是不是我"，触发面同样不由投递范围决定）。
        // ⚠️ 广播用快照遍历：被动在响应里可能生成/移除单位（`ForEachAliveSnapshot` 的注释）。
        //    这里紧邻下面那个 `ForEachAliveSnapshot`，但两者**不嵌套**（顺序执行），
        //    因此不会互相践踏 `IterationBuffer`。
        BroadcastHookGameplayEvent(BattleTags::Hook_Death, DeathContext);

        // ⚠️ **死者本人要单独补发一次**（这是"广播给存活单位"覆盖不到的一处，必须补）：
        //   `BroadcastHookGameplayEvent` 走 `ForEachAliveSnapshot`，而上面那行 `MarkDead()`
        //   已经把死者翻成"不存活"，因此它**收不到**这条事件。
        //   而腐殖蛆的死亡毒爆正是"**自己**死亡时对周围 1 格施加 3 层中毒"
        //  （`UACPassive_Enemy_Worm_DeathToxin` 判 `Context.Target == 自己`）——
        //   漏掉这一次补发，那只蛆死了就是白死，毒爆永远不会发生。
        //   为什么不是"把广播改成遍历全部单位（含已死）"：旧实现在派发 `Hook.Death` **之前**
        //   就调了 `EffectSystem.UnregisterAllByOwner(DeadId)`（3.2a 的注释保留了这条顺序），
        //   也就是说**除死者之外的其他尸体不会**响应钩子 —— 遍历全部单位会把触发面放大到
        //   "所有还没被清理掉的尸体"，那是一处新的行为差异。这里按"存活全场 + 死者本人"投递，
        //   正好等于旧口径。
        UACBattleAbility::SendHookToUnit(*this, Unit, BattleTags::Hook_Death, DeathContext);

        // owner 死亡联动（召唤物策略）。使用快照遍历：策略回调可能继续标记死亡。
        ForEachAliveSnapshot([this, DeadId, Unit](AACBattleUnitBase& Other)
        {
            if (&Other == Unit || Other.GetOwnerUnitId() != DeadId)
            {
                return;
            }
            const FACSummonSpec* Spec = DataContext.SummonSpecs.Find(Other.GetDefinitionId());
            if (Spec != nullptr && Spec->OwnerDeathPolicy == EACOwnerDeathPolicy::Destroy)
            {
                Other.MarkPendingDeath(InvalidUnitId);
            }
        });
    }

    // 3) 清理死亡召唤物 / 分身；干员与敌人保留用于结果与回放。
    TArray<FUnitId> Removals;
    for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
    {
        if (Unit != nullptr && Unit->IsDead()
            && Unit->GetKind() != EACUnitKind::Operator
            && Unit->GetKind() != EACUnitKind::Enemy)
        {
            Removals.Add(Unit->GetUnitId());
        }
    }
    for (const FUnitId RemoveId : Removals)
    {
        RemoveUnitFromRegistry(RemoveId);
    }

    EventBus.FlushDeferred();
}

// ---------------------------------------------------------------------------
// 战前配置 / 开局效果
// ---------------------------------------------------------------------------

void UBattleWorld::ApplyPreBattleConfiguration()
{
    TArray<FUnitId> PlayerUnitIds;
    for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
    {
        if (Unit != nullptr && Unit->GetTeam() == EACTeam::Player && Unit->GetKind() == EACUnitKind::Operator)
        {
            PlayerUnitIds.Add(Unit->GetUnitId());
        }
    }

    int32 PlayerIndex = 0;
    for (const FACPlayerUnitSpec& Spec : Setup.PlayerUnits)
    {
        const FUnitId UnitId = PlayerUnitIds.IsValidIndex(PlayerIndex) ? PlayerUnitIds[PlayerIndex] : InvalidUnitId;

        if (AACBattleUnitBase* Unit = FindUnit(UnitId))
        {
            // 个性强化 / 装备 / 词条带来的战斗内常驻 GE。
            //
            // 阶段 3.3：字段已是 `TArray<TSubclassOf<UGameplayEffect>>`（§3.2 表），
            // 因此这里**直接施加类**，不再经过渡映射表按 `FName` 查表 ——
            // 拼错内容从"运行期静默跳过"变成"编译期找不到类"。
            //
            // 语义与旧实现逐项对齐：
            //   · `TierIndex` 的档位不再传：GE 的数值是**内容常量**（`UACGE_Upgrade_OP01_C1` 的
            //     `FocusMax -20` 等，逐项抄自旧 `BuildEffectBlocks` 的四档同值 `Tier(x)`），
            //     四档同值时不存在信息丢失；真出现"按档位不同"的内容时，用 `SetByCaller` 传档位值。
            //   · 顺序：强化 → 装备 → 词条，与旧代码一字不差。
            ApplyGameplayEffectsToUnit(*Unit, Spec.UpgradeEffects);
            ApplyGameplayEffectsToUnit(*Unit, Spec.EquipmentEffects);
            ApplyGameplayEffectsToUnit(*Unit, Spec.TraitEffects);
            // 说明：`Spec.GrantedAbilities`（装备 / 强化 / 词条带来的被动能力）**不在这里**施加 ——
            // 它属于"单位出生就该拿到"的授予，已在 `RegisterUnit` → `UACAbilitySetComponent::GrantToOwner`
            // 里与 `UACAbilitySet` 一起授完（见 `Initialize` 的玩家生成循环）。
            // 两边分工的唯一判据是"要不要先被授予才能在事件上触发"，而不是"哪一层写的"。
        }
        ++PlayerIndex;
    }

    // -----------------------------------------------------------------------
    // 敌方"本场额外 GE"（`FACEnemyUnitSpec::Effects`，例如"本场敌人开局有 5 层静电紊乱"）
    // -----------------------------------------------------------------------
    // ⚠️ 阶段 3.3 顺带修掉一处**契约与实现不一致**：`FACEnemyUnitSpec::EffectBlockIds`
    //    （现在的 `Effects`）在改造前的 `Initialize` / `ApplyPreBattleConfiguration` 里
    //    **从来没有任何消费者** —— 字段存在、注释写着用途、但一支敌人也不会拿到它。
    //    本阶段把它接上：与玩家侧同一口径（按参战顺序对齐 + 逐个 `ApplyGameplayEffectsToUnit`）。
    //    `Encounter.EnemyEffects` 当前恒为空（`GenerateEncounter` 不填），
    //    因此这一步**今天不改变任何战斗行为**，只是让"能配的真的生效"。
    {
        TArray<FUnitId> EnemyUnitIds;
        for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
        {
            if (Unit != nullptr && Unit->GetTeam() == EACTeam::Enemy)
            {
                EnemyUnitIds.Add(Unit->GetUnitId());
            }
        }

        int32 EnemyIndex = 0;
        for (const FACEnemyUnitSpec& Spec : Setup.EnemyUnits)
        {
            const FUnitId UnitId = EnemyUnitIds.IsValidIndex(EnemyIndex) ? EnemyUnitIds[EnemyIndex] : InvalidUnitId;
            if (AACBattleUnitBase* Unit = FindUnit(UnitId))
            {
                ApplyGameplayEffectsToUnit(*Unit, Spec.Effects);
            }
            ++EnemyIndex;
        }
    }

    // 外部注入（学派强化 / 秘术预置）。
    for (const FACExternalModifierSpec& Modifier : Setup.ExternalModifiers)
    {
        if (Modifier.TargetUnitIndex < 0)
        {
            ForEachAlive([this, &Modifier](AACBattleUnitBase& Unit)
            {
                if (Unit.GetTeam() == EACTeam::Player)
                {
                    ApplyGameplayEffectsToUnit(Unit, Modifier.Effects);
                }
            });
        }
        else if (PlayerUnitIds.IsValidIndex(Modifier.TargetUnitIndex))
        {
            // TargetUnitIndex 语义为"参战顺序索引"，只对玩家编队生效（ACBattleSetup.h）。
            const FUnitId TargetId = PlayerUnitIds[Modifier.TargetUnitIndex];
            if (AACBattleUnitBase* const TargetUnit = FindUnit(TargetId))
            {
                ApplyGameplayEffectsToUnit(*TargetUnit, Modifier.Effects);
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[Battle] ExternalModifier TargetUnitIndex %d out of range (%d players)."),
                   Modifier.TargetUnitIndex, PlayerUnitIds.Num());
        }
    }

    // -----------------------------------------------------------------------
    // 开局时间轴条目（抢攻 / 后发）——阶段 3.2b：`FTimelineSystem` 两条线的落地
    // -----------------------------------------------------------------------
    // §4.5 的映射：
    //   抢攻 = 一个"有持续时间的 GE"（施加即生效，到期由引擎摘掉 → 内核**没有**状态要维护）；
    //   后发 = "第 X 秒触发一次"，等价物是 `{ GameplayEvent, Hook.PostEffectTrigger }`，
    //          但当前没有任何被动监听那个标签，因此它的承载是内核侧待触发表（`PendingTimelineFires`）。
    // 旧实现（`FTimelineEntry` + 每帧 `Update`）里的"条目表 / 作用域解析 / 到期标记 / 改写 API"
    // 全部消失：那些都是在手工模拟"GE 的时长"与"一次性触发"这两件引擎本来就会做的事。
    //
    // ⚠️ 阶段 3.3：字段 `FACStartingTimelineSpec::Effect` 现在是 `TSubclassOf<UGameplayEffect>`，
    //    因此这里**直接施加类**，不再经 `FACBattleDataContext::EffectBlockGEs` 过渡映射表。
    // ⚠️ `DurationSeconds` 字段**已删除**：两个时间轴 GE 的时长都写在 GE 类上
    //    （心流刃抢攻 4s / 超导线圈后发 5s，与内容里的旧 `DurationSeconds` 一致，C6/D10），
    //    内核从来不读它 —— 删掉它是"让能配的就能生效"。
    for (const FACStartingTimelineSpec& TimelineSpec : Setup.StartingTimeline)
    {
        if (TimelineSpec.Effect.Get() == nullptr)
        {
            // 内容侧没给类（编辑器里加了一行但没选 GE）。静默跳过：
            // 与 `ApplyGameplayEffectsToUnit` 对空位的口径一致。
            continue;
        }

        // 作用对象：`TargetUnitIndex >= 0` → 参战顺序索引；`< 0` → 全队（我方）。
        // 这与旧实现在 `FTimelineEntry::Scope` 上的口径一致（`Self` / `Team`）。
        TArray<FUnitId> Targets;
        if (TimelineSpec.TargetUnitIndex >= 0)
        {
            if (!PlayerUnitIds.IsValidIndex(TimelineSpec.TargetUnitIndex))
            {
                // 旧实现同样在这种情况下打 Warning（`StartingTimeline TargetUnitIndex out of range`）。
                UE_LOG(LogTemp, Warning, TEXT("[Battle] StartingTimeline TargetUnitIndex %d out of range (%d players)."),
                       TimelineSpec.TargetUnitIndex, PlayerUnitIds.Num());
                continue;
            }
            Targets.Add(PlayerUnitIds[TimelineSpec.TargetUnitIndex]);
        }
        else
        {
            Targets = PlayerUnitIds;
        }

        if (Targets.Num() == 0)
        {
            // 与旧实现同一句告警（`:1317` 的那条）。
            UE_LOG(LogTemp, Warning, TEXT("[Battle] StartingTimeline has no owner and no player units; entry skipped."));
            continue;
        }

        if (TimelineSpec.bPreemptive)
        {
            // 抢攻：**注册即生效**（GE 自带时长，例如心流刃抢攻 GE 是 `HasDuration = 4s`）。
            for (const FUnitId TargetId : Targets)
            {
                if (AACBattleUnitBase* const TargetUnit = FindUnit(TargetId))
                {
                    ApplyGameplayEffectToUnit(*TargetUnit, TimelineSpec.Effect);
                }
            }

            // 埋点与事件：口径与旧 `FTimelineSystem::FireEntry` 逐字一致
            //（Category=Timeline、EventTag=Preemptive、Source=Owner、ValueA=目标数）。
            // Source 取 `Targets[0]`：旧实现在 Team 作用域下 `Entry.Owner` 也正是首个参战干员。
            LogTimelineTriggered(Targets[0], LogEvent_Preemptive, Targets.Num());

            FBattleEvent Event;
            Event.Tag = BattleTags::Hook_PreemptiveTrigger;
            Event.Source = Targets[0];
            Event.IntValue = Targets.Num();
            EventBus.Broadcast(Event);
        }
        else
        {
            // 后发：登记"第 TriggerSeconds 秒触发一次"。
            // 时刻用**绝对时间**（战斗起始世界时刻 + 相对秒数），与 `Step` 里的 `FACBattleTime::Now` 同源。
            // `FMath::Max(0.f, ...)`：负的 TriggerSeconds 视为"第一帧就触发"（旧实现里
            // `TriggerTime <= Now` 的条目同样会在第一次 `Update` 立刻触发）。
            const float TriggerTime = BattleStartWorldTime + FMath::Max(0.f, TimelineSpec.TriggerSeconds);
            for (const FUnitId TargetId : Targets)
            {
                FPendingTimelineFire Pending;
                Pending.TriggerTime = TriggerTime;
                Pending.EffectClass = TimelineSpec.Effect;
                Pending.Owner = TargetId;
                PendingTimelineFires.Add(Pending);
            }
        }
    }
}

void UBattleWorld::LogTimelineTriggered(FUnitId Owner, FName EventTag, int32 TargetCount)
{
    // 口径照抄旧 `ACTimelineSystem.cpp:25-41` 的 `LogTimelineFired`：
    // 目标不逐个展开成多条记录（Team/All 作用域一次十几个目标），`ValueA` 记目标数即可比对。
    FBattleLogRecord Record;
    Record.Time = FACBattleTime::ElapsedSeconds(*this);
    Record.Category = LogCategory_Timeline;
    Record.EventTag = EventTag;
    Record.Source = Owner;
    Record.ValueA = static_cast<float>(TargetCount);
    LogBuffer.Record(Record);
}

void UBattleWorld::TickPendingTimelineFires()
{
    if (PendingTimelineFires.Num() == 0)
    {
        return;
    }

    // 时间只取一次：同一帧内所有条目按同一时刻判定（旧 `FTimelineSystem::Update` 的口径）。
    const float NowSeconds = FACBattleTime::Now(*this);

    // 从后往前遍历 + `RemoveAt`：删除不会打乱尚未处理的元素。
    for (int32 Index = PendingTimelineFires.Num() - 1; Index >= 0; --Index)
    {
        const FPendingTimelineFire& Pending = PendingTimelineFires[Index];
        if (NowSeconds < Pending.TriggerTime)
        {
            continue;
        }

        // 施加给登记时记下的单位。单位可能已经死亡 / 离场 —— `FindUnit` 返回 nullptr 就跳过，
        // 这与旧实现"作用域 `Self` 时所有者还活着才施加"（`ResolveTargets` 里判 `IsAlive`）等价。
        int32 AppliedCount = 0;
        if (AACBattleUnitBase* const Target = FindUnit(Pending.Owner))
        {
            if (Target->IsAlive() && ApplyGameplayEffectToUnit(*Target, Pending.EffectClass))
            {
                AppliedCount = 1;
            }
        }

        // 埋点（Category=Timeline、EventTag=PostEffect、Source=Owner、ValueA=实际施加数）——
        // 这是"后发到点了"在日志里的唯一证据，§8.2 的日志对照靠它。
        LogTimelineTriggered(Pending.Owner, LogEvent_PostEffect, AppliedCount);

        // 广播 `Hook.PostEffectTrigger`：这是"后发到点了"对外的唯一事件。
        // 阶段 4：原来这里注明"表现层的 `TimelineTriggered` Cue 挂在这条事件上
        //（`ACPresentationBridge::MapEventTagToCue`）"—— 表现桥（`UBattlePresentationBridge`
        // + `EACPresentationCueType`）已随 §4.7 整体删除，表现改由 `GameplayCue` 承担
        //（GE 的应用 / 移除由 `GameplayCueManager` 自动派发，不再需要一份"事件标签 → Cue 类型"的映射表）。
        // 这条事件本身保留：它是"第 N 秒触发了一个能力"的通用挂点（`Hook.PostEffectTrigger`）。
        FBattleEvent Event;
        Event.Tag = BattleTags::Hook_PostEffectTrigger;
        Event.Source = Pending.Owner;
        Event.IntValue = AppliedCount;
        EventBus.Broadcast(Event);

        PendingTimelineFires.RemoveAt(Index);
    }
}

// ---------------------------------------------------------------------------
// 特殊机制钩子
// ---------------------------------------------------------------------------

FBattleHookResult UBattleWorld::HandleActionTriggered(FBattleHookContext& Context)
{
    FBattleHookResult Result;
    AACBattleUnitBase* Unit = FindUnit(Context.Source);
    if (Unit == nullptr)
    {
        return Result;
    }

    // -----------------------------------------------------------------------
    // 静电紊乱：每次动作受 1 × 层数 的技术伤害（框架内置的系统级钩子）
    // -----------------------------------------------------------------------
    // 阶段 3.2a：层数改从 ASC 的标签计数读（旧 `Unit->GetStates().GetStacks(...)` 已删除）。
    // 这与状态 GE 的叠加语义一致：`UACGE_State_StaticDisorder` 走 `AggregateBySource`，
    // 施加 N 层后 `GetTagCount(State.StaticDisorder) == N`。
    const int32 Stacks = Unit->GetStateStacks(BattleTags::State_StaticDisorder);
    if (Stacks > 0)
    {
        FDamageRequest Request;
        // -------------------------------------------------------------------
        // ⚠️ **来源单位在本阶段无法还原**（一处已知的能力缺口，写在这里以免被当成 bug 排查）
        // -------------------------------------------------------------------
        // 旧实现读的是 `FAbnormalStateInstance::Source`（"谁施加的这个状态"）。
        // 新链路里这条信息落在 GE 的效果上下文 instigator 上，取法是
        // `ASC->GetActiveEffects(MakeQuery_MatchAnyOwningTags(State.StaticDisorder))
        //  → FActiveGameplayEffect::Spec.GetEffectContext().GetInstigator()`
        //（`GetActiveEffectsWithAllTags` 只给句柄，`FGameplayEffectQuery` 要按 owning tags 匹配，
        //  `GameplayEffect.h:1505`；`FActiveGameplayEffect::GetEffectContext` 在 :1101）。
        //
        // 但**当前没有任何内容会施加这个状态**：唯一施加者是旧 `BuildAbnormalStates` +
        // 某个施加状态的效果块，而全仓从来没有任何效果块写过 `ApplyAbnormalState(State.StaticDisorder)`
        //（只有 `State.Poison` / `State.Bleed` 被真正施加过）。也就是说 `Stacks` 恒为 0，
        // 这段代码是**不可达的**，为了它去接一条 GE 反查链只会引入没人验证过的代码。
        //
        // 因此本阶段记 `InvalidUnitId`：伤害照旧落在承受者身上（`Target` 正确），
        // 只有"伤害归属给谁"这一列是空的。
        // ⚠️ 阶段 3.2b 的状态：`Hook.Xxx` 的 GameplayEvent 广播**已经接上了**（本文件
        // `BroadcastHookGameplayEvent` + 各派发点），但"谁给谁挂上 `State.StaticDisorder`"
        // 在全仓仍然**没有任何施加方**（旧代码里也没有），所以这段依旧不可达。
        // 补它需要先有内容（一个施加该状态的技能/装备），不属于本次删除+接线的范围。
        Request.Source = InvalidUnitId;
        Request.Target = Unit->GetUnitId();
        Request.DamageType = EACDamageType::Technical;
        Request.Reason = EACDamageReason::Dot;
        Request.RawAmount = static_cast<float>(Stacks);
        Request.bCanCrit = false;
        Request.SourceEffectBlockId = FName(TEXT("StaticDisorder"));
        CombatResolver.ApplyDamage(Request);
    }
    return Result;
}

FBattleHookResult UBattleWorld::HandleTakeDamage(FBattleHookContext& Context)
{
    if (AACBattleUnitBase* Unit = FindUnit(Context.Target))
    {
        // -------------------------------------------------------------------
        // 坦克受击回专注（旧 `FAbilityExecutor::OnTakeHit`，`ACAbilityExecutor.cpp:687-697`）
        // -------------------------------------------------------------------
        // 逐项搬来，只有两处承载形式变了：
        //   · `Unit.HasTag(BattleTags::Unit_Class_Tank)` —— 完全没变（身份标签一直挂在单位上）；
        //   · 改值从"旧 `FAbilityExecutor::GrantFocus` 手写 `FFocusState`"换成
        //     `UACBattleAbility::GrantFocusToUnit` —— 它把"改值走 GE（`UACGE_FocusGain`）+
        //     `Hook.FocusFull` / `Hook.FocusChanged` 派发 + `Focus | FocusChanged` 埋点"
        //     三件事打包在一起，正是旧 `GrantFocus` 的等价物。
        // 量取 `RuleConfig::TankFocusOnHit`（默认 1.f；配置缺失时同样回退 1.f，与旧实现一致）。
        if (Unit->HasTag(BattleTags::Unit_Class_Tank))
        {
            const float Gain = (DataContext.RuleConfig != nullptr) ? DataContext.RuleConfig->TankFocusOnHit : 1.f;
            if (Gain > 0.f)
            {
                UACBattleAbility::GrantFocusToUnit(*Unit, Gain);
            }
        }
    }
    return FBattleHookResult();
}

// ---------------------------------------------------------------------------
// 结果
// ---------------------------------------------------------------------------

void UBattleWorld::CheckOutcome()
{
    if (Outcome != EACOutcome::None)
    {
        return;
    }

    const bool bPlayerWiped = IsTeamWiped(EACTeam::Player);
    const bool bEnemyWiped = IsTeamWiped(EACTeam::Enemy);

    if (bPlayerWiped && bEnemyWiped)
    {
        Outcome = EACOutcome::Defeat;   // A8：同时全灭判负
        // 埋点：三处设定点各记一条——是"结局由哪一支判定先命中"的直接证据，
        // 只记一条最终值就无法区分"我方全灭"与"超时"这类同样产出 Defeat/Timeout 的分支。
        LogWorldEvent(*this, LogCategory_Battle, LogEvent_Outcome, InvalidUnitId, InvalidUnitId,
                      static_cast<int32>(Outcome));
        return;
    }
    if (bEnemyWiped)
    {
        Outcome = EACOutcome::Victory;
        LogWorldEvent(*this, LogCategory_Battle, LogEvent_Outcome, InvalidUnitId, InvalidUnitId,
                      static_cast<int32>(Outcome));
        return;
    }
    if (bPlayerWiped)
    {
        Outcome = EACOutcome::Defeat;
        LogWorldEvent(*this, LogCategory_Battle, LogEvent_Outcome, InvalidUnitId, InvalidUnitId,
                      static_cast<int32>(Outcome));
        return;
    }

    const float MaxSeconds = DataContext.RuleConfig != nullptr ? DataContext.RuleConfig->MaxBattleSeconds : 300.f;
    if (MaxSeconds > 0.f && FACBattleTime::ElapsedSeconds(*this) >= MaxSeconds)
    {
        Outcome = EACOutcome::Timeout;  // A9
        LogWorldEvent(*this, LogCategory_Battle, LogEvent_Outcome, InvalidUnitId, InvalidUnitId,
                      static_cast<int32>(Outcome));
    }
}

void UBattleWorld::CollectUnitResults(TArray<FACUnitBattleResult>& OutResults)
{
    OutResults.Reset();
    for (const TObjectPtr<AACBattleUnitBase>& Unit : Units)
    {
        if (Unit == nullptr || Unit->GetTeam() != EACTeam::Player || Unit->GetKind() != EACUnitKind::Operator)
        {
            continue;
        }

        FACUnitBattleResult Result;
        Result.UnitId = Unit->GetUnitId();
        Result.DefinitionId = Unit->GetDefinitionId();
        Result.bDead = Unit->IsDead();
        Result.RemainingBaseHP = Result.bDead ? 0.f : FMath::Clamp(Unit->GetCurrentHP(), 0.f, Unit->GetBaseMaxHP());

        const FACBattleStatsSnapshot& StatsSnapshot = StatsCollector.GetSnapshot();
        const float* Dealt = StatsSnapshot.DamageDealtByUnit.Find(Result.UnitId);
        const float* Taken = StatsSnapshot.DamageTakenByUnit.Find(Result.UnitId);
        const float* Healed = StatsSnapshot.HealingDoneByUnit.Find(Result.UnitId);
        Result.DamageDealt = Dealt != nullptr ? *Dealt : 0.f;
        Result.DamageTaken = Taken != nullptr ? *Taken : 0.f;
        Result.HealingDone = Healed != nullptr ? *Healed : 0.f;

        OutResults.Add(Result);
    }
    // 按 UnitId 升序：UnitId 单调递增且不复用，因此这个顺序等价于"参战顺序"，跨两次运行稳定。
    // **排序与筛选口径自 0.1b 起保持不变**（只收 Player + Operator，按 UnitId 升序）。
    OutResults.Sort([](const FACUnitBattleResult& A, const FACUnitBattleResult& B)
    {
        return A.UnitId < B.UnitId;
    });

    // D8 §5.1：逐单位结果的唯一权威副本落到统计快照上（`FACBattleStatsSnapshot::UnitResults`）。
    // 为什么在这里回写而不是让调用方自己再抄一份：本函数是**唯一**会产出这份数组的地方
    // （筛选 + 排序都只在这里做），回写能保证"结果结构里的 Units"与"统计快照里的 UnitResults"
    // 永远同序同源，`_units.json` 也就不可能与 `FACBattleResult::Units` 分叉。
    // 因此本函数不再是 const —— 它是结果的**生产点**，而不是纯查询。
    StatsCollector.SetUnitResults(OutResults);
}

// ---------------------------------------------------------------------------
// 阶段 4（D3 / §5.3）：ComputeStateHash() **已删除**。
//
// 它是“放弃确定性”这条决策里最后一个消费者：它的三个调用点（UBattleSession::BuildResult、
// FACBattleStatsSnapshot::FinalStateHash、FACRunBattleOutcome::StateHash）已随字段一并删除。
// 回归与排障改由三份日志产物承担（§8.1 / §8.2）—— 它们不依赖确定性，
// 而“同一 DebugId 不出现两次存活”这类不变量检查直接读日志就能做（§8.2 第 2 项）。
// ---------------------------------------------------------------------------
