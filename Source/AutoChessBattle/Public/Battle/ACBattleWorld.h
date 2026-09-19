// M02 战斗世界与单位容器：一场战斗的唯一逻辑事实源。
// 固定步顺序见 TechDocs/00A §2；子系统在此集中持有与初始化。
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GameplayEffect.h"
#include "Templates/SubclassOf.h"
#include "Core/ACBattleTypes.h"
#include "Core/ACBattleSetup.h"
#include "Core/ACDataTypes.h"
#include "Events/ACBattleEventBus.h"
#include "Battle/ACBattleTime.h"
#include "Grid/ACBattleGrid.h"
#include "Stats/ACBattleStats.h"
#include "Combat/ACCombatResolver.h"
#include "Mental/ACMentalSystem.h"
// 阶段 3.2b：`Ability/ACAbilityExecutor.h` 与 `Timeline/ACTimelineSystem.h` 的依赖**已删除** ——
// 两条线（技能执行 `FAbilityExecutor` + 时间轴 `FTimelineSystem`）整体退场：
//   · 行动执行 → `Step` 里按 `UACSkillAbilityBase` / `UACBasicAttackAbility` 找句柄后
//     `ASC->TryActivateAbility`（失败 fallback 普攻），见 `ExecuteActionForUnit`；
//   · 时间轴抢攻 → 开局直接施加一个"有持续时间的 GE"（`ApplyPreBattleConfiguration`）；
//   · 时间轴后发 → 内核侧的待触发表 `PendingTimelineFires`（`TickPendingTimelineFires`）。
#include "Scheduler/ACActionScheduler.h"
#include "Targeting/ACTargetingSystem.h"
#include "Diagnostics/ACBattleDiagnostics.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleBoardActor.h"
#include "Battle/ACOperatorActor.h"
#include "Battle/ACEnemyActor.h"
#include "Battle/ACSummonActor.h"
#include "ACBattleWorld.generated.h"

// 阶段 0b（D8）：旧的单位类型别名（`U` + `BattleUnit`）已删除，本文件里单位一律显式写成 `AACBattleUnitBase`；
// 对外契约的指针化（弱指针 + Resolve）见 §5.1 与 Core/ACBattleSetup.h。
//
// 阶段 3.2a（§7 阶段 3.2）本文件的头文件依赖变化：
//   - 删：`Abnormal/ACAbnormalStates.h`、`Effects/ACEffectSystem.h`（两个类型都没了）；
//   - 增：`Templates/SubclassOf.h`（`TObjectPtr` / `TSubclassOf` 都只需要不完整类型即可声明，
//     因此不必把 GAS 与能力集的头文件拖进本头）。
//
// **阶段 4（§7 阶段 4，D3）**：`#include "Battle/ACBattleRng.h"` 已删除，`FBattleRng` 成员、
// `GetRng()` / `GetRngSeed()` / `ComputeStateHash()` 与 `RngSeed` 成员**一并删除**
// （C3：不追求确定性；随机直接用 `FMath::FRand`）。

/** 单位定义 Id → 能力 / GE 的授予清单（AutoChessBattle，见 FACBattleDataContext::AbilitySets）。 */
class UACAbilitySet;

/** 战斗内 GE 的类（时间轴条目 / 生成请求 / 一次性施加的值类型）。 */
class UGameplayEffect;

/** 数据上下文：由 UBattleDataSubsystem 组装，必须存活至战斗结束（定义资产由 AssetManager 常驻）。
 *
 *  阶段 3.2a（§7 阶段 3.2）：`EffectLibrary`（`UBattleEffectLibrary*`）与
 *  `AbnormalStates`（`TMap<FGameplayTag, FACAbnormalStateDef>`）两个成员**已随旧效果 / 异常状态
 *  两条线删除** —— 效果块不再是数据表（改为 GE 类 + 能力类），异常状态不再是"定义表 + 容器"
 *  （改为 `GrantedTags` + `StackLimitCount` 的 GE 实例）。 */
struct AUTOCHESSBATTLE_API FACBattleDataContext
{
    const UBattleRuleConfig* RuleConfig = nullptr;

    /**
     * 单位定义 Id → 能力 / 常驻 GE 授予清单（阶段 3.2a 新增）。
     *
     * 为什么放在这里而不是 `UUnitDefinitionBase` 上：定义类型在 `AutoChessCore`，
     * `UACAbilitySet` 在 `AutoChessBattle`（依赖方向 Core ⇐ Battle），Core 放不下这个字段。
     * 完整裁决见 `UACAbilitySetComponent::SetAbilitySet` 与阶段报告。
     */
    TMap<FName, TObjectPtr<UACAbilitySet>> AbilitySets;

    // 阶段 3.3：`TMap<FName, TSubclassOf<UGameplayEffect>> EffectBlockGEs`（3.2a 的**过渡映射表**）
    // **已删除**。它存在的唯一理由是"`ACBattleSetup` / `Run/*` 的效果块字段还是 `FName`"，
    // 那些字段本阶段已改成类引用（§3.2 表），翻译层因此整体退场：
    // 现在契约里直接就是 `TSubclassOf<UGameplayEffect>`，`UBattleWorld::ApplyGameplayEffectsToUnit`
    // 拿到就能施加，中间没有任何"按名字查表"的环节 —— 拼错内容从"静默跳过"变成"编译不过"。

    TMap<FName, const UUnitDefinitionBase*> UnitDefinitions;
    TMap<FName, FACSummonSpec> SummonSpecs;
    TMap<FName, FACStatBlock> SummonBaseStats;
};

UCLASS()
class AUTOCHESSBATTLE_API UBattleWorld : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FACBattleSetup& InSetup, const FACBattleDataContext& InDataContext,
                    const FACBattleLaunchOptions& InOptions = FACBattleLaunchOptions());
    void Shutdown();

    /**
     * 按帧推进战斗（阶段 0.5，D2 / §5.2 实现裁决）。
     *
     * @param DeltaTime 本帧增量（秒），**只做本帧增量用**：HP 回复 / 精神值恢复 / 专注结算 /
     *                  行动条推进都直接乘它。内核**不缓存、不累加**它（那会造出第二个时间源）。
     *                  绝对时间（到期判定）一律走 `FACBattleTime::Now(*this)`。
     *                  `DeltaTime <= 0` 或战斗已结束时直接返回（§8.2 第 3 项的不变量）。
     */
    void Step(float DeltaTime);

    // ---- 命令 ----
    void EnqueueCommand(const FACBattleCommand& Command);
    bool DequeueCommand(FACBattleCommand& OutCommand);

    // ---- 单位 ----
    FUnitId SpawnUnit(const FACUnitSpawnRequest& Request);
    void KillUnit(FUnitId UnitId, EACDamageReason Cause);
    void ReviveUnit(FUnitId UnitId);

    /**
     * 弱指针解引用统一入口：集中处理"Actor 已销毁"的空悬情况。
     * 为什么不能直接 Unreal 的 Get()：调用方需要区分"没给目标"与"目标已销毁"，
     * 前者是正常业务（例如无来源伤害），后者是需要告警的异常（命令队列里残留的失效目标）。
     *
     * 本重载**不打日志**：弱指针无效是常态（大量调用点的 Source/Target 本来就允许为空），
     * 逐次告警会把日志淹没。需要"该有目标却没有"的语义时用 ResolveOrWarn。
     */
    AACBattleUnitBase* Resolve(const TWeakObjectPtr<AACBattleUnitBase>& WeakUnit) const;

    /**
     * 带告警的弱指针解引用：语义与 Resolve 相同，但**期望这里一定有一个单位**。
     * 弱指针无效（非空但已销毁 = 悬垂）时打 `UE_LOG(Warning)`、累加 ResolveMissedCount
     * 并返回 nullptr，由调用方决定如何降级（例如丢弃该条命令）。
     */
    AACBattleUnitBase* ResolveOrWarn(const TWeakObjectPtr<AACBattleUnitBase>& WeakUnit);

    /**
     * 带告警的整型句柄解引用：`FACBattleCommand` 这类**跨帧存活的整型句柄**专用
     * （§5.1 逐处口径：命令队列消费时对失效目标告警后丢弃命令，不要静默跳过）。
     * InvalidUnitId（"没给目标"）不算异常、不告警，直接返回 nullptr。
     */
    AACBattleUnitBase* ResolveOrWarn(FUnitId UnitId);

    /** 到当前为止 ResolveOrWarn 命中"目标已销毁"的次数（Diagnostics 侧自检统计，§8.2 第 3 项）。 */
    int32 GetResolveMissedCount() const { return ResolveMissedCount; }
    /** 按整型句柄查 Actor（O(1) 查 UnitLookup）。
     *  D8（§5.1）**保留本签名**：整型 UnitId 是不复用、可跨场对齐的稳定句柄，
     *  内部注册表用它做键；弱指针解引用另有统一入口（见下面的 Resolve / ResolveOrWarn）。
     *  Actor 销毁时（RemoveUnitFromRegistry）会同步移除键，因此不会返回悬垂指针。 */
    AACBattleUnitBase* FindUnit(FUnitId UnitId) const;
    void ForEachAlive(TFunctionRef<void(AACBattleUnitBase&)> Func) const;
    /** 遍历注册表中所有单位（含已死亡但保留用于结果/复活的单位）。 */
    void ForEachUnit(TFunctionRef<void(AACBattleUnitBase&)> Func) const;
    int32 GetAliveCount(EACTeam Team) const;
    bool IsTeamWiped(EACTeam Team) const;

    /** 战场棋盘 Actor（坐标变换的唯一出口）；阶段 0a 起由 Initialize 生成、Shutdown 销毁。 */
    AACBattleBoardActor* GetBoardActor() const { return BoardActor; }

    // ---- 辅助 ----
    const UBattleRuleConfig* GetRuleConfig() const { return DataContext.RuleConfig; }
    const FACBattleDataContext& GetDataContext() const { return DataContext; }
    const UUnitDefinitionBase* FindUnitDefinition(FName DefinitionId) const;

    /**
     * 从任意 Actor（典型是单位 Actor）找回它所属的那场战斗。
     *
     * **为什么需要这个函数**：GAS 的 `UGameplayEffectExecutionCalculation` 与 `UGameplayEffectComponent`
     * 只拿得到 ASC / Actor，拿不到 `UBattleWorld`。而**不能**用 `Cast<UBattleWorld>(Actor->GetOuter())` ——
     * `UWorld::SpawnActor` 内部是 `NewObject<AActor>(LevelToSpawnIn, ...)`，单位 Actor 的 Outer 是 `ULevel`，
     * 那个 Cast **恒为 nullptr**，效果会被静默丢弃（伤害不落地、召唤不生成，且没有任何报错）。
     *
     * 正确路径：`Actor -> UWorld -> UGameInstance -> UBattleSubsystem -> UBattleSession -> UBattleWorld`。
     * 零开销的快路径（Outer 直接是 `UBattleWorld`）也保留，便于将来把单位的 Outer 指过去。
     *
     * 返回 nullptr 表示"这个 Actor 不属于任何进行中的战斗"（例如 CDO、已 Teardown），调用方必须判空。
     */
    static UBattleWorld* FindFromActor(const AActor* Actor);

    // ---- 时间 ----
    /**
     * 战斗起始的**引擎世界时刻**（`Initialize` 里只记录一次）。
     * 为什么记它：`World->GetTimeSeconds()` 带一个非零起点（关卡已经跑了多久），
     * 不能直接当作"战斗进行了几秒"。两者相减即战斗内相对时间，见 `FACBattleTime::ElapsedSeconds`。
     * **这不是累加器**：取值来自引擎，不随帧率漂移。
     */
    float GetBattleStartWorldTime() const { return BattleStartWorldTime; }

    // ---- 子系统 ----
    // 阶段 3.2a：`FEffectSystem& Effects()` 已删除 —— 效果块系统整条线（条件 / 动作 / 实例 /
    // 频率守卫）被 GE 取代：常驻内容走 `UACAbilitySet`，运行时效果走能力里的 GE 施加，
    // 周期结算走 GE 的 `Period` + `UACPeriodicDamageExecution`。
    // 阶段 3.2b：`FAbilityExecutor& Ability()` 与 `FTimelineSystem& Timeline()` 也**已删除** ——
    // 两条线分别改由"能力驱动"（`ExecuteActionForUnit`）与"GE + 待触发表"承担。
    // 阶段 4（D3）：`FBattleRng& GetRng()` 已删除 —— 战斗内核不再有随机流（C3）。
    FBattleEventBus& Events() { return EventBus; }
    FBattleGrid& Grid() { return GridSystem; }
    FCombatResolver& Combat() { return CombatResolver; }
    FActionScheduler& GetScheduler() { return Scheduler; }
    FTargetingSystem& Targeting() { return TargetingSystem; }
    FBattleAISystem& AI() { return AISystem; }
    FMentalSystem& Mental() { return MentalSystem; }
    FBattleStatsCollector& Stats() { return StatsCollector; }
    FBattleLogBuffer& Log() { return LogBuffer; }

    /**
     * 把 GameplayEvent 广播给全场存活单位的 ASC（阶段 3.2b）。
     *  用 `ForEachAliveSnapshot` 而不是 `ForEachAlive`：被动在响应里可能生成单位
     *   （母巢的 `Hook.BattleStart` 会召唤 4 只蠕虫），直接遍历 `Units` 数组会让迭代器失效
     * @return 被触发的能力条数（0 = 全场没有任何被动监听这个 Tag，属正常）。
     */
    int32 BroadcastHookGameplayEvent(FGameplayTag HookTag, const FBattleHookContext& Context);

    // ---- 特殊机制（框架版）----
    void SpawnSummonFromSpec(FName SummonSpecId, FUnitId OwnerUnitId);
    /** 延迟精神回复：`DueTime` 是**绝对时间**（`FACBattleTime::Now`），到期判定走秒。 */
    void EnqueueDelayedMentalRecover(FUnitId Target, float Amount, float DueTime);

    /**
     * 每帧结算入口
     */
    void TickFrame(float DeltaTime);

    // ---- 结果 ----
    EACOutcome GetOutcome() const { return Outcome; }
    void SetOutcome(EACOutcome InOutcome) { Outcome = InOutcome; }
    bool IsFinished() const { return Outcome != EACOutcome::None; }
    void CheckOutcome();
    /** 收集逐单位对外结果（只收 Player + Operator，按 UnitId 升序），并把同一份数组回写进
     *  `StatsCollector` 的 `FACBattleStatsSnapshot::UnitResults`（D8 §5.1：结果的唯一权威副本）。
     *  因此本函数**不是 const** —— 它是结果的生产点，而不是纯查询。
     *  筛选与排序口径自 0.1b 起保持不变；_units.json 消费的正是这里写下的数组。 */
    void CollectUnitResults(TArray<FACUnitBattleResult>& OutResults);

    /**
     * 本场**累计生成过**的单位数（含战斗中召唤出来的）。
     *
     * 用途：`BattlePerfReport`（D11 / §6.3）要报"本场 Actor 创建次数" ——
     * 那是判定"要不要引入对象池"的分子。`NextUnitId` 从 1 开始单调递增且不复用（§5.1），
     * 因此 `NextUnitId - 1` 正是这个计数，不需要另开一个计数器（两个计数器必然分叉）。
     */
    int32 GetSpawnedUnitCount() const { return NextUnitId - 1; }

    /**
     * 把一组 GE 类逐个施加到单位身上（**阶段 3.3 起直接收类引用**）。
     *
     * 替代 3.2a 的 `ApplyEffectBlocksToUnit(Unit, TArray<FName>)` + 过渡映射表：
     * 契约里的效果块字段已经是 `TArray<TSubclassOf<UGameplayEffect>>`，
     * 因此这里不再需要"按名字查表"，直接把类施加掉。
     *
     * 消费点一共三处，全在本文件内：外部注入（`FACExternalModifierSpec::Effects`）、
     * 生成请求（召唤物 `FACSummonSpec::OnSpawnEffects` → `FACUnitSpawnRequest::Effects`）、
     * 秘术卡命令（`FACBattleCommand::CardId`）。
     * 空数组直接返回（大多数装备 / 词条没有战斗内 GE，这是常态，不告警）。
     * ⚠️ 阶段 3.2b：原来还有第四类消费点 —— 旧技能路径 `FAbilityExecutor` 的三处
     *    "执行效果块"（普攻段级附加 / 技能效果 / 引导 tick）。那条线已随
     *    `Ability/ACAbilityExecutor.{h,cpp}` 整体删除，技能 / 普攻的效果现在由
     *    能力类内部的 GE 施加承担（`UACSkillAbilityBase::ApplySkillEffects` /
     *    `UACBasicAttackAbility::OnHitEffects`）。
     * ⚠️ 阶段 3.3：预战配置（强化 / 装备 / 词条）不再走本函数 —— 它们与"装备 / 强化带来的
     *    被动能力"一起在单位出生时经 `UACAbilitySetComponent` 授予（一次授予点），
     *    本函数只服务"战斗中某一刻对某个单位施加一组 GE"。
     */
    void ApplyGameplayEffectsToUnit(AACBattleUnitBase& Unit, const TArray<TSubclassOf<UGameplayEffect>>& Effects);

private:
    void ConsumeCommands();
    void ResolveDeaths();
    void ApplyPreBattleConfiguration();
    void PlaceUnitOnGrid(AACBattleUnitBase& Unit, FACHexCoord PreferredCell);
    void RegisterUnit(AACBattleUnitBase& Unit);
    void RemoveUnitFromRegistry(FUnitId UnitId);

    // -----------------------------------------------------------------------
    // 行动执行（阶段 3.2b：`FAbilityExecutor::RequestAction` 的替代）
    // -----------------------------------------------------------------------

    /**
     * 一个单位的行动槽到点后执行的完整分支（§2.3 第 5 步，顺序与旧 `RequestAction` 一致）：
     *   ① 技能可放（`IsReadyToActivate` = 专注满）→ `TryActivateAbility(技能句柄)`；
     *   ② **技能失败 / 不可放 → 索敌（`MaintainTarget`）→ `TryActivateAbility(普攻句柄)`**
     *      —— 第二段里的"失败即 fallback"是 §2.3 明确要求覆盖的核心分支；
     *   ③ 普攻也放不出（超距 / 无合法目标）→ `MoveOneStepTowards`。
     */
    void ExecuteActionForUnit(AACBattleUnitBase& Unit);

    /**
     * 索敌与目标维护（旧 `FAbilityExecutor::RequestAction:174-187` 的等价物）。
     *
     * ⚠️ **这段必须留在内核**：新能力只"解析"目标、不"索敌" ——
     * `UACBattleAbility::CanActivateAbility` → `ResolveTargets` →
     * `FTargetingSystem::ResolveSelector(PrimaryTarget)` 读的是 `Unit.GetCurrentTargetId()`
     * （ACTargetingSystem.cpp:303-308），目标失效时它**不会**替单位重新选一个。
     * 漏掉本函数，单位在第一个目标死掉之后就再也不会攻击。
     */
    void MaintainTarget(AACBattleUnitBase& Unit);

    /**
     * 在单位的 ASC 上按**能力基类**找第一条"可放"的能力句柄。
     *
     * 用句柄而不是 `TryActivateAbilityByClass`：内核本来就要遍历一遍能力清单来做
     * "专注满才放"的判定（`IsReadyToActivate`），句柄是这次遍历顺手的产物；
     * 而 `TryActivateAbilityByClass` 每次都要按类再查一次（`GetActivatableAbilities` 线性扫描）。
     *
     * @param Caster 施法者单位 —— `IsReadyToActivate` 收它而不是用 `GetCasterUnit()`，
     *               因为内核拿到的 `Spec.Ability` 是 **CDO**（见该函数的注释）。
     * @return 无效句柄（`IsValid() == false`）= 该单位没有这类能力 / 都不满足前提。
     */
    FGameplayAbilitySpecHandle FindReadyAbilityHandle(UAbilitySystemComponent& ASC, const UClass* AbilityClass,
                                                       const AACBattleUnitBase& Caster);

    // -----------------------------------------------------------------------
    // 时间轴（阶段 3.2b：`FTimelineSystem` 的替代）
    // -----------------------------------------------------------------------

    /**
     * 后发待触发条目。
     *
     * 为什么用"内核侧列表"而不是 `FTimerManager::SetTimer`：到期判定要与
     * `FACBattleTime`（引擎 `UWorld::GetTimeSeconds`）**同源**，与"战斗结束时清理定时器"
     * 这件事解耦 —— 列表随 `UBattleWorld` 一起销毁，不存在"定时器在 World 之后还在跑"的问题；
     * 且 `Step` 里的顺序（先时间轴、再调度、再行动）与旧实现逐位一致，可读可调试。
     */
    struct FPendingTimelineFire
    {
        /** 触发时刻（**绝对时间**，秒 = `BattleStartWorldTime` + `TriggerSeconds`）。 */
        float TriggerTime = 0.f;
        /** 要施加的 GE（阶段 3.3 起直接来自 `FACStartingTimelineSpec::Effect`）。 */
        TSubclassOf<UGameplayEffect> EffectClass;
        /** 承受单位（`FACStartingTimelineSpec::TargetUnitIndex` 解析出的参战单位）。 */
        FUnitId Owner = InvalidUnitId;
    };

    /** 每帧处理到点的后发条目（旧 `FTimelineSystem::Update` 的后发那一半）。 */
    void TickPendingTimelineFires();

    /** 把一条时间轴触发写进结构化日志（口径与旧 `FTimelineSystem` 的埋点逐字一致）。 */
    void LogTimelineTriggered(FUnitId Owner, FName EventTag, int32 TargetCount);

    /**
     * 把 GE 施加到单位**自身**（施加以承受者自己的 ASC 为来源）。
     *
     * 这是**所有"施加一组现成 GE"路径共用的唯一施加点**：`ApplyGameplayEffectsToUnit`
     * （外部注入 / 生成请求 / 秘术卡）、时间轴抢攻 / 后发、以及预战配置（强化 / 装备 / 词条）
     * 都走它，免得同一段"MakeEffectContext + MakeOutgoingSpec + ApplyGameplayEffectSpecToSelf"
     * 在文件里出现多遍。施加失败返回 false（无 GE 类 / 无 ASC / spec 无效）。
     */
    bool ApplyGameplayEffectToUnit(AACBattleUnitBase& Unit, TSubclassOf<UGameplayEffect> EffectClass);

    /**
     * 取该单位定义对应的能力 / 常驻 GE 清单（阶段 3.2a）。
     * 没有配清单时返回 nullptr —— 这是**正常状态**（召唤物就没有清单），调用方静默跳过。
     */
    UACAbilitySet* FindAbilitySet(FName DefinitionId) const;

    /** 按队伍与类别选择要生成的 Actor 类（内容侧将来可由 Definition 覆盖）。 */
    TSubclassOf<AACBattleUnitBase> ResolveUnitClass(EACTeam Team, EACUnitKind Kind) const;

    /**
     * 延迟构造（Deferred）生成一个单位 Actor：出生后不跑 BeginPlay，等调用方填完数据再 FinishSpawning。
     * GetWorld() 不可用时返回 nullptr 并告警（不 check 崩溃）。
     */
    AACBattleUnitBase* SpawnUnitActor(TSubclassOf<AACBattleUnitBase> UnitClass);

    FBattleHookResult HandleActionTriggered(FBattleHookContext& Context);   // 静电紊乱：每次动作受 1×层数 伤害
    FBattleHookResult HandleTakeDamage(FBattleHookContext& Context);        // 坦克受击 +1 专注

public:
    /** 遍历快照缓冲：避免在 ForEachAlive 回调内因 Spawn/Kill 而失效。
     *  各子系统在会派发钩子的遍历中都需要它，故对外可见。 */
    void ForEachAliveSnapshot(TFunctionRef<void(AACBattleUnitBase&)> Func);

private:
    FACBattleSetup Setup;
    FACBattleLaunchOptions Options;
    FACBattleDataContext DataContext;

    FBattleEventBus EventBus;
    FBattleGrid GridSystem;
    FCombatResolver CombatResolver;
    FActionScheduler Scheduler;
    FTargetingSystem TargetingSystem;
    FBattleAISystem AISystem;
    FMentalSystem MentalSystem;
    FBattleStatsCollector StatsCollector;
    FBattleLogBuffer LogBuffer;

    /**
     * 后发待触发列表（阶段 3.2b：`FTimelineSystem` 的后发线）。
     * 抢攻**不在这里** —— 它就是一个"有持续时间的 GE"，施加完就没有内核状态要维护了
     * （到期由引擎的 GE 定时器负责，`RemoveActiveGameplayEffect` 会自动摘掉）。
     */
    TArray<FPendingTimelineFire> PendingTimelineFires;

    /** Actor 化（阶段 0a）：单位是真的 AActor，UPROPERTY 保证由 GC 正确持有与销毁。 */
    UPROPERTY() TArray<TObjectPtr<AACBattleUnitBase>> Units;
    /** UnitId → 单位 Actor（取代原 UnitIndex 的下标表）：FindUnit 与反注册都走 O(1) 查表。
     *  键写成 int32 而不写 FUnitId：两者是同一个类型（Core/ACBattleTypes.h 的 using 别名），
     *  在 UPROPERTY 声明里与项目其余位置一样直接写底层类型，避免 UHT 解析别名。 */
    UPROPERTY() TMap<int32, TObjectPtr<AACBattleUnitBase>> UnitLookup;
    /** 战场棋盘 Actor（坐标变换唯一出口，§2.1）。 */
    UPROPERTY() TObjectPtr<AACBattleBoardActor> BoardActor = nullptr;
    TArray<FACBattleCommand> Commands;
    TArray<FBattleSubscriptionHandle> SystemSubscriptions;

    /** 遍历用快照缓冲（复用，避免热路径分配）。 */
    TArray<FUnitId> IterationBuffer;
    TArray<FUnitId> ReadyQueueBuffer;

    struct FDelayedMentalRecover
    {
        FUnitId Target = InvalidUnitId;
        float Amount = 0.f;
        /** 到期时刻（绝对时间，秒）。 */
        float DueTime = 0.f;
    };
    TArray<FDelayedMentalRecover> DelayedMentalRecovers;

    EACOutcome Outcome = EACOutcome::None;
    FUnitId NextUnitId = 1;
    // 阶段 4（D3）：`int32 RngSeed` 成员已删除。种子与状态哈希一起退场（C3）。

    /**
     * 战斗起始的引擎世界时刻（`Initialize` 记录一次）。
     * 它不是累加器：取值来自 `UWorld::GetTimeSeconds()`，只做"相对时间"的基准点（§5.2 实现裁决）。
     */
    float BattleStartWorldTime = 0.f;

    /** ResolveOrWarn 命中"目标已销毁"的累计次数。
     *  为什么留一个计数器而不是只打日志：§8.2 第 3 项要求"弱指针解引用前必须 IsValid()"
     *  在 Release 下退化为计数统计 —— 日志会被裁掉，计数不会。 */
    int32 ResolveMissedCount = 0;
};
