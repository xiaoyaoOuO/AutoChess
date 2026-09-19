// 阶段 1 新增，阶段 3.1b 补完（GAS 重构实施方案 §0 C9 / §4.1 / §5.1 / §6.3 / §7 阶段 3.1）：
// `UACBattleAbility` 的实现。设计理由与"双发"分工见头文件，这里只写实现细节。
//
// 本文件里有两处**照抄旧实现**的代码，抄的时候一行数值/顺序都没动（C6/D10）：
//   - `IsInRange` / `MoveOneStepTowards`：来源 `Private/ACAbilityExecutor.cpp:106-111 / 203-252`
//     （§3.2 会删 `ACAbilityExecutor.{h,cpp}`，本阶段**只复制、不删原件**）；
//   - `GrantFocusToUnit`：来源 `FAbilityExecutor::GrantFocus`（`ACAbilityExecutor.cpp:592-630`），
//     只有"改值"这一步从"手写属性"换成"施加 GE"（§4.1 的 `GrantFocus` → GE Modifier）。
#include "GAS/ACBattleAbility.h"

#include "AbilitySystemComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameplayEffect.h"

#include "Battle/ACBattleTime.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "Core/ACBattleTags.h"
#include "Core/ACHexGridStatics.h"
#include "Flow/ACBattleSession.h"
#include "Flow/ACBattleSubsystem.h"
#include "GAS/ACBattleAttributeSet.h"
#include "GAS/Effects/ACGE_ContentEffects.h"
#include "Grid/ACBattleGrid.h"

// ---------------------------------------------------------------------------
// 结构化日志埋点（口径照抄 `ACAbilityExecutor.cpp:13-46`）
//
// 技能施放：Category=`Ability`、**EventTag 直接用 SkillId**（不设固定常量）。
// 这样脚本按 category=Ability 取行、按 event 分组就能逐技能比对，不必再维护第二张映射表。
// 专注变化：单独一个 Category=`Focus`，理由同上（旧实现也是两个 Category）。
// ---------------------------------------------------------------------------
namespace
{
    /** 技能施放埋点的分类。 */
    const FName LogCategory_Ability(TEXT("Ability"));
    /** 专注变化的分类与事件标签（旧 `ACAbilityExecutor.cpp:30-31` 逐字一致）。 */
    const FName LogCategory_Focus(TEXT("Focus"));
    const FName LogEvent_FocusChanged(TEXT("FocusChanged"));
}

UACBattleAbility::UACBattleAbility()
{
    // 单机项目：不参与网络复制与客户端预测（实施方案 §7 阶段 1）。
    //
    // 这三项都在 `UGameplayAbility` 的 protected 段（GameplayAbility.h:715 / 735 / 503），
    // 只能在构造函数里设。它们必须**在能力的子类构造函数之前**设好 —— 子类若改了其中一项，
    // 那是子类的显式决定（例如将来某个"本地表现向"的能力），不会与本约定冲突。

    // ServerOnly 在单机下等于"同步执行、不预测"：TryActivateAbility 返回时能力已经跑完，
    // 因此用 TryActivateAbility 替换 FAbilityExecutor 时，"施放即完成"的时序不变（§2.3）。
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;

    // 引导类技能要自持 UAbilityTask 与实例状态（§4.1 的 CastType = Channel 映射），
    // 只有 InstancedPerActor 能装下这些；NonInstanced 自 5.5 起被引擎标记为废弃。
    // 另外它还是被动能力"触发次数计数器"能跨激活存活的前提（见 ConsumeTrigger 的注释）。
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

    // 没有输入通道要复制（自走棋的行动由调度器与 AI 决定，不来自按键）。
    bReplicateInputDirectly = false;

    // ⚠️ `ActivationBlockedTags` 在**基类里刻意留空**：
    // 沉默 / 眩晕的门控是"内容相关"的（铁壁的守望与索利瓦尔的巨斧裂体都要求
    // `State.Silence` / `State.Stun` 挡下，但普攻过去由 `FAbilityExecutor::CanAct`
    // 单独判过，语义并不相同 —— 见 §4.1 的映射表）。
    // 因此由**具体的能力**（`UACSkillAbilityBase`）在自己的构造函数里加，
    // 而不是在这里一刀切：基类加会让普攻也吃到"沉默不能普攻"这条并不存在的新规则。
}

// ---------------------------------------------------------------------------
// 世界 / 施法者
// ---------------------------------------------------------------------------

UBattleWorld* UACBattleAbility::FindBattleWorldFromActor(const AActor* Actor)
{
    // 口径已上移到 `UBattleWorld::FindFromActor`（唯一事实源）：
    // GE 的 Execution / Component 也要用它，放在能力头里会让属性集/GE 反向依赖能力基类。
    return UBattleWorld::FindFromActor(Actor);
}

UBattleWorld* UACBattleAbility::GetBattleWorld() const
{
    // 用 Avatar 而不是 ASC：`CanActivateAbility` 会在 CDO 上被调用，那时没有 CurrentActorInfo。
    // 但这仍然是一条"实例态"路径，**只应在能力已激活时调用**；
    // CDO 语境请用 `FindBattleWorldFromActor(Caster)`（形参里的 ActorInfo）。
    return FindBattleWorldFromActor(GetAvatarActorFromActorInfo());
}

AACBattleUnitBase* UACBattleAbility::GetCasterUnit() const
{
    // Avatar 是"能力的物理执行者"；本方案里 Owner 与 Avatar 是同一个 Actor（§2.2），
    // 因此这里用 Avatar 取施法者单位是准确的，且能在将来"能力挂在别的 Actor 上"时自然降级。
    return Cast<AACBattleUnitBase>(GetAvatarActorFromActorInfo());
}

// ---------------------------------------------------------------------------
// 目标解析（C9 / §6.3）
// ---------------------------------------------------------------------------

bool UACBattleAbility::ResolveTargets(const AACBattleUnitBase& Caster, TArray<FUnitId>& OutTargets) const
{
    OutTargets.Reset();

    UBattleWorld* const World = FindBattleWorldFromActor(&Caster);
    if (World == nullptr)
    {
        // false 是"解析没做"，不是"没有目标"：把装配错误伪装成合法结果会更难查。
        return false;
    }

    // §5.1：目标列表用整型 `FUnitId`（每帧多次解析的热路径）。
    World->Targeting().ResolveSelector(GetTargetSelector(Caster), Caster, GetSelectorRadius(Caster), OutTargets);
    return true;
}

bool UACBattleAbility::ResolveTargets(TArray<FUnitId>& OutTargets) const
{
    const AACBattleUnitBase* const Caster = GetCasterUnit();
    if (Caster == nullptr)
    {
        OutTargets.Reset();
        return false;
    }
    return ResolveTargets(*Caster, OutTargets);
}

bool UACBattleAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                          const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags,
                                          FGameplayTagContainer* OptionalRelevantTags) const
{
    // 引擎侧：标签门控（`ActivationBlockedTags` = 沉默/眩晕）、冷却、Cost、输入阻断
    // （实现见 GameplayAbility.cpp:424-523；注意它在 CDO 上被调用，不能读实例状态）。
    if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags))
    {
        return false;
    }

    // 被动能力不在这里判目标（触发时刻才决定，见 RequiresTargetInRange 的注释）。
    if (!RequiresTargetInRange())
    {
        return true;
    }

    const AACBattleUnitBase* const Caster = (ActorInfo != nullptr)
        ? Cast<AACBattleUnitBase>(ActorInfo->AvatarActor.Get())
        : nullptr;
    if (Caster == nullptr)
    {
        return false;
    }

    TArray<FUnitId> Targets;
    if (!ResolveTargets(*Caster, Targets))
    {
        // 解析没做（没有战斗世界）：能力不该在此时被激活，直接拒绝而不是"当作没目标"。
        return false;
    }
    if (Targets.Num() == 0)
    {
        return false;   // 没有合法目标 → 不施放（旧 RequestAction 的 bHasTarget 判定）
    }

    UBattleWorld* const World = FindBattleWorldFromActor(Caster);
    if (World == nullptr)
    {
        return false;
    }
    const AACBattleUnitBase* const Primary = World->FindUnit(Targets[0]);
    if (Primary == nullptr)
    {
        // 目标可能已在同一步内被移除（旧 RequestAction 的注释专门写过这种情况）。
        return false;
    }

    // §4.1：`RangeOverride` / `bRequireTargetInRange` 的距离检查放 CanActivateAbility。
    return IsInRange(*Caster, *Primary, GetEffectiveRangeOverride(*Caster));
}

// ---------------------------------------------------------------------------
// 位置（逐字搬自 FAbilityExecutor，见文件头）
// ---------------------------------------------------------------------------

bool UACBattleAbility::IsInRange(const AACBattleUnitBase& Self, const AACBattleUnitBase& Target, float RangeOverride) const
{
    // 来源：`ACAbilityExecutor.cpp:106-111`。
    // `RangeOverride <= 0` = 用施法者射程（旧 `FACSkillDef::RangeOverride = -1` 的口径）。
    const float Range = RangeOverride > 0.f ? RangeOverride : Self.GetBaseRange();
    // 六边形距离：UACHexGridStatics（六边形几何，GAS 没有对应物，§11 B）。
    const int32 Distance = UACHexGridStatics::Distance(Self.GetCell(), Target.GetCell());
    return static_cast<float>(Distance) <= Range;
}

bool UACBattleAbility::MoveOneStepTowards(UBattleWorld& World, AACBattleUnitBase& Unit, FUnitId TargetId)
{
    // 来源：`ACAbilityExecutor.cpp:203-252`，逐字复制（类型已是 Actor，时间已是 FACBattleTime）。
    //
    // ⚠️ 阶段 3.2b：世界改成**形参**（`static`）。3.1b 的非静态版本靠 `GetBattleWorld()` 取世界，
    //    而内核（`UBattleWorld::Step` 的超距分支）手里只有 CDO，那条路会静默取到 nullptr。
    //    函数体与 3.1b 完全一致，只是 `World->` 变成 `World.`。
    AACBattleUnitBase* const Target = World.FindUnit(TargetId);
    if (Target == nullptr)
    {
        return false;
    }

    // 期望射程：内容给了 PreferredRange 就用它，否则用基础射程（与旧实现同口径）。
    const float DesiredRange = Unit.GetAttackPattern().PreferredRange > 0.f
        ? Unit.GetAttackPattern().PreferredRange
        : Unit.GetBaseRange();

    TArray<FACHexCoord> Path;
    if (!World.Grid().FindPathToRange(Unit.GetCell(), Target->GetCell(), FMath::FloorToInt(DesiredRange), Path))
    {
        return false;
    }
    if (Path.Num() == 0)
    {
        return false;   // 已在射程内：没有"再走一步"这回事
    }

    const FACHexCoord Next = Path[0];
    const FACHexCoord From = Unit.GetCell();
    if (!World.Grid().MoveUnitOneStep(Unit.GetUnitId(), From, Next))
    {
        return false;
    }
    Unit.SetCell(Next);
    Unit.SetActionState(EACUnitActionState::Moving);

    // ① 广播（给 `Listen` 的观察者）：带 `EACMoveReason::Pathing`，语义是"这次为什么移动"。
    FBattleEvent MoveEvent;
    MoveEvent.Tag = BattleTags::Hook_UnitMoved;
    MoveEvent.Source = Unit.GetUnitId();
    MoveEvent.IntValue = static_cast<int32>(EACMoveReason::Pathing);
    World.Events().Broadcast(MoveEvent);

    // ② 钩子（可取消/可改值）：**静电紊乱的两个触发点之一**（另一个是 BeforeAttack）——
    //    `UBattleWorld::HandleActionTriggered` 订阅了 `Hook.Unit.Moved`，漏发它静电紊乱就少扣一次血。
    //    这里也走 `DispatchHook`（双发）：EventBus 给内核订阅者，GameplayEvent 给**移动者自己**的 ASC。
    //    本阶段没有任何"移动触发"的被动内容，因此 GameplayEvent 那一路当前恒为 0 条 ——
    //    保留它是为了让"能力派发的每个钩子都同时唤醒被动"成为一条**没有例外**的规则，
    //    将来加移动类被动时不需要回来改能力。
    FBattleHookContext MoveContext;
    MoveContext.Source = Unit.GetUnitId();
    MoveContext.Time = FACBattleTime::ElapsedSeconds(World);
    // 走 static 版双发入口：本函数是 static（内核要调它），拿不到 `this`，
    // 因此世界显式传进去 —— 双发规则本身仍然只有一份实现（成员版转发到同一个函数）。
    DispatchHook(World, BattleTags::Hook_UnitMoved, MoveContext, &Unit);

    Unit.SetActionState(EACUnitActionState::Idle);
    return true;
}

// ---------------------------------------------------------------------------
// 钩子双发
// ---------------------------------------------------------------------------

FGameplayEventData UACBattleAbility::MakeEventPayload(const UBattleWorld& World, const FBattleHookContext& Context)
{
    FGameplayEventData Payload;

    // `Instigator` / `Target` 收的是 `AActor*`，因此必须把整型句柄还原成单位
    //（§5.1：钩子上下文的 Source/Target 是整型，战斗内瞬时数据不背弱指针）。
    const AACBattleUnitBase* const SourceUnit = World.FindUnit(Context.Source);
    const AACBattleUnitBase* const TargetUnit = World.FindUnit(Context.Target);
    Payload.Instigator = static_cast<const AActor*>(SourceUnit);
    Payload.Target = static_cast<const AActor*>(TargetUnit);
    Payload.EventMagnitude = Context.FloatValue;

    // 语义标签（伤害类型等）随钩子传给条件原语，这里一并带上，被动将来要用就有。
    // ⚠️ `IntValue` / `bBoolValue` 装不进 `FGameplayEventData`（没有对应载荷位），见头文件说明。
    Payload.InstigatorTags = Context.Tags;
    return Payload;
}

FGameplayEventData UACBattleAbility::MakeEventPayload(const FBattleHookContext& Context) const
{
    // 成员版只是转发（字段映射只有上面那一份，避免两处映射表分叉）。
    // 取不到世界时返回默认载荷：能力不该在没有世界的语境里激活，但也不该因此崩溃。
    UBattleWorld* const World = GetBattleWorld();
    return (World != nullptr) ? MakeEventPayload(*World, Context) : FGameplayEventData();
}

int32 UACBattleAbility::SendHookToUnit(UBattleWorld& World, AACBattleUnitBase* Recipient, FGameplayTag HookTag,
                                       const FBattleHookContext& Context)
{
    if (Recipient == nullptr || !HookTag.IsValid())
    {
        return 0;
    }
    UAbilitySystemComponent* const ASC = Recipient->GetAbilitySystemComponent();
    if (ASC == nullptr)
    {
        return 0;
    }

    FGameplayEventData Payload = MakeEventPayload(World, Context);
    Payload.EventTag = HookTag;

    // 底层入口（AbilitySystemComponent.h:756）：它按 Tag 找 `GameplayEventTriggeredAbilities`
    // 里登记的能力（登记发生在 `GiveAbility` → `OnGiveAbility`，AbilitySystemComponent_Abilities.cpp:557-572），
    // 命中后走 `TriggerAbilityFromGameplayEvent` → `InternalTryActivateAbility`（同文件 :2468-2500）。
    // 返回触发成功的能力条数（0 = 没有被动监听这个 Tag，属正常）。
    return ASC->HandleGameplayEvent(HookTag, &Payload);
}

int32 UACBattleAbility::SendHookToUnit(AACBattleUnitBase* Recipient, FGameplayTag HookTag,
                                       const FBattleHookContext& Context) const
{
    UBattleWorld* const World = GetBattleWorld();
    return (World != nullptr) ? SendHookToUnit(*World, Recipient, HookTag, Context) : 0;
}

int32 UACBattleAbility::BroadcastHookGameplayEvent(FGameplayTag HookTag, const FBattleHookContext& Context) const
{
    // 实现已上移到 `UBattleWorld`（"全场"这个概念属于内核，且内核必须能广播 —— 见头文件）。
    UBattleWorld* const World = GetBattleWorld();
    return (World != nullptr) ? World->BroadcastHookGameplayEvent(HookTag, Context) : 0;
}

FBattleHookResult UACBattleAbility::DispatchHook(UBattleWorld& World, FGameplayTag HookTag, FBattleHookContext& Context,
                                                 AACBattleUnitBase* EventRecipient)
{
    FBattleHookResult Result;
    if (!HookTag.IsValid())
    {
        return Result;
    }

    // ① **内核钩子（可取消 / 可改值）**：静电紊乱、坦克受击回专注、旧"监视"类效果块都挂在
    //    这条通道上，而且它们靠 `Context.FloatValue` / `bCancelled` / 返回值改变战局 —— 不能省。
    Result = World.Events().Dispatch(HookTag, Context);

    // ② **玩法被动（只负责触发）**：同一份上下文发给指定单位的 ASC。
    //    顺序与条件都在这里定死：EventBus 先派发（订阅者可能改值 / 否决），
    //    GameplayEvent 后发，且**被否决的动作不再触发被动** ——
    //    否则会出现"技能已被打断，被动照旧生效"这种自相矛盾的结果。
    if (!Result.bCancel && !Result.bInterrupt && !Context.bCancelled)
    {
        SendHookToUnit(World, EventRecipient, HookTag, Context);
    }
    return Result;
}

FBattleHookResult UACBattleAbility::DispatchHook(FGameplayTag HookTag, FBattleHookContext& Context,
                                                 AACBattleUnitBase* EventRecipient) const
{
    UBattleWorld* const World = GetBattleWorld();
    return (World != nullptr) ? DispatchHook(*World, HookTag, Context, EventRecipient) : FBattleHookResult();
}

FBattleHookContext UACBattleAbility::MakeHookContext(const FGameplayEventData* TriggerEventData)
{
    FBattleHookContext Context;
    if (TriggerEventData == nullptr)
    {
        return Context;   // Source/Target 都是 InvalidUnitId，调用方判 Invalid 即可
    }

    // 反向映射（与 MakeEventPayload 逐项对应）：Actor → 整型句柄。
    if (const AACBattleUnitBase* const SourceUnit = Cast<AACBattleUnitBase>(TriggerEventData->Instigator.Get()))
    {
        Context.Source = SourceUnit->GetUnitId();
    }
    if (const AACBattleUnitBase* const TargetUnit = Cast<AACBattleUnitBase>(TriggerEventData->Target.Get()))
    {
        Context.Target = TargetUnit->GetUnitId();
    }
    Context.FloatValue = TriggerEventData->EventMagnitude;
    Context.Tags = TriggerEventData->InstigatorTags;

    // ⚠️ 时间：`FGameplayEventData` 不带时间戳，而 `FBattleHookContext::Time` 是**绝对时间**（秒）。
    // 这里**不猜**（填 0 会让日志出现"钩子在战斗开始那一刻发生"的假时间轴）。
    // 被动能力要用时间就直接取 `FACBattleTime::Now(*GetBattleWorld())`；
    // 当前 7 个被动没有一个读它。
    return Context;
}

// ---------------------------------------------------------------------------
// GameplayEffect 施加
// ---------------------------------------------------------------------------

bool UACBattleAbility::ApplyEffectToUnit(AACBattleUnitBase& SourceUnit, AACBattleUnitBase& TargetUnit,
                                        TSubclassOf<UGameplayEffect> EffectClass,
                                        const TMap<FName, float>& SetByCallerValues, int32 Stacks)
{
    if (EffectClass.Get() == nullptr)
    {
        return false;
    }

    UAbilitySystemComponent* const SourceASC = SourceUnit.GetAbilitySystemComponent();
    UAbilitySystemComponent* const TargetASC = TargetUnit.GetAbilitySystemComponent();
    if (SourceASC == nullptr || TargetASC == nullptr)
    {
        return false;
    }

    // context 的 instigator 取**来源 ASC**（`MakeEffectContext` 内部用 OwnerActor/AvatarActor，见
    // AbilitySystemComponent.cpp:470-481）。这不是可选项：`AggregateBySource` 的状态 GE
    // 靠 instigator 判定"同源才叠加"（GameplayEffect.cpp:3518-3522），
    // 没有 instigator 时同一状态的多次施加会变成多个独立实例（层数语义就丢了）。
    const FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();

    // Level 固定 1：本项目的 GE 数值都是内容常量或 SetByCaller，没有按能力等级缩放的修饰
    //（`FGameplayModifierInfo` 的 `FScalableFloat` 在等级 1 时就是原值）。
    const FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(EffectClass, /*Level=*/1.f, Context);
    if (!SpecHandle.IsValid() || !SpecHandle.Data.IsValid())
    {
        return false;
    }

    for (const TPair<FName, float>& Pair : SetByCallerValues)
    {
        if (Pair.Key.IsNone())
        {
            continue;
        }
        // ⚠️ 函数名是 `SetSetByCallerMagnitude`（GameplayEffect.h:1082），不是 `SetByCallerMagnitude`。
        SpecHandle.Data->SetSetByCallerMagnitude(Pair.Key, Pair.Value);
    }

    // 层数：旧 `ApplyAbnormalState(Stacks = 3)` 就是这条路径（GameplayEffect.h:1059）。
    // 首次施加时引擎用 `Spec.GetStackCount()` 当起始层数（GameplayEffect.cpp:4101-4144），
    // 已有同源实例时按"已有 + 本次"累加（同文件 :4051-4055）。
    SpecHandle.Data->SetStackCount(FMath::Max(1, Stacks));

    // 从**来源 ASC** 施加到**目标 ASC**：`ApplyGameplayEffectSpecToTarget`
    // （声明 AbilitySystemComponent.h:330），与 `ApplyGameplayEffectSpecToSelf` 的差别只在
    // "谁是施加方"，而"施加方"正是上面那条同源判定的依据。
    SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
    return true;
}

bool UACBattleAbility::ApplyEffectToUnit(AACBattleUnitBase& SourceUnit, AACBattleUnitBase& TargetUnit,
                                        TSubclassOf<UGameplayEffect> EffectClass, int32 Stacks)
{
    // 空表：`TMap` 的默认构造不分配存储，因此这条重载没有额外开销。
    static const TMap<FName, float> NoSetByCaller;
    return ApplyEffectToUnit(SourceUnit, TargetUnit, EffectClass, NoSetByCaller, Stacks);
}

TMap<FName, float> UACBattleAbility::MakeSetByCaller(FName DataName, float Value)
{
    TMap<FName, float> Values;
    if (!DataName.IsNone())
    {
        Values.Add(DataName, Value);
    }
    return Values;
}

// ---------------------------------------------------------------------------
// 专注（旧 FAbilityExecutor::GrantFocus 的等价物）
// ---------------------------------------------------------------------------

float UACBattleAbility::GrantFocusToUnit(AACBattleUnitBase& Unit, float Amount)
{
    UBattleWorld* const World = FindBattleWorldFromActor(&Unit);
    UAbilitySystemComponent* const ASC = Unit.GetAbilitySystemComponent();
    if (World == nullptr || ASC == nullptr)
    {
        return 0.f;
    }

    const FGameplayAttribute FocusAttribute = UACBattleAttributeSet::GetFocusAttribute();
    const float Before = ASC->GetNumericAttribute(FocusAttribute);

    if (!FMath::IsNearlyZero(Amount))
    {
        // 改值走 GE（§4.1：`GrantFocus` → "GE Modifier（Focus）"）。
        // SetByCaller 键名集中在 `UACGE_FocusGain::GetFocusGainDataName()` 一处，避免字符串写两遍。
        ApplyEffectToUnit(Unit, Unit, UACGE_FocusGain::StaticClass(),
                          MakeSetByCaller(UACGE_FocusGain::GetFocusGainDataName(), Amount));
    }

    // 前后各读一次属性而不是"用 Amount 算"：夹取（`Focus <= FocusMax`、`>= 0`）发生在属性集里，
    // 用形参算出来的差值会在"满专注时继续回专注"这类场景与真实值不符 ——
    // 旧实现同样只用实际差值（`Focus.Current - Old`）记日志，绝不记形参。
    const float After = ASC->GetNumericAttribute(FocusAttribute);
    const float FocusMax = Unit.GetStat(EACStat::FocusMax);

    FBattleHookContext Context;
    Context.Target = Unit.GetUnitId();
    Context.Time = FACBattleTime::ElapsedSeconds(*World);
    Context.FloatValue = After;

    // `Hook.FocusFull`：旧的判定是"旧值 < Max 且新值 >= Max"（**跨过**满值那一刻才派发）。
    // `FocusMax <= 0`（无专注条的单位）永远不派发，与旧 `GrantFocus` 的 `Old < Focus.Max` 一致。
    if (FocusMax > 0.f && Before < FocusMax && After >= FocusMax)
    {
        World->Events().Dispatch(BattleTags::Hook_FocusFull, Context);
    }

    if (!FMath::IsNearlyEqual(Before, After))
    {
        World->Events().Dispatch(BattleTags::Hook_FocusChanged, Context);

        // 埋点：Source = InvalidUnitId（专注是单位自身资源，没有"来源单位"）、
        //       Target = 单位、ValueA = 变化后的值、ValueB = **实际**变化量（可正可负）。
        // 专注未变的调用不记（否则日志会被每帧一次的空转淹没，也污染 §8.2 要比对的专注数值）。
        FBattleLogRecord Record;
        Record.Time = FACBattleTime::ElapsedSeconds(*World);
        Record.Category = LogCategory_Focus;
        Record.EventTag = LogEvent_FocusChanged;
        Record.Source = InvalidUnitId;
        Record.Target = Unit.GetUnitId();
        Record.ValueA = After;
        Record.ValueB = After - Before;
        World->Log().Record(Record);
    }

    return After - Before;
}

// ---------------------------------------------------------------------------
// 埋点
// ---------------------------------------------------------------------------

void UACBattleAbility::LogAbilityEvent(FName EventTag, FUnitId Source, FUnitId Target,
                                       float ValueA, float ValueB, int32 IntValue) const
{
    UBattleWorld* const World = GetBattleWorld();
    if (World == nullptr)
    {
        return;
    }

    FBattleLogRecord Record;
    Record.Time = FACBattleTime::ElapsedSeconds(*World);
    Record.Category = LogCategory_Ability;
    Record.EventTag = EventTag;
    Record.Source = Source;
    Record.Target = Target;
    Record.ValueA = ValueA;
    Record.ValueB = ValueB;
    Record.IntValue = IntValue;
    // `EffectBlockId` 刻意留空：技能 ID 已经承担"放了哪个技能"的语义，
    // 再记效果块 ID 会把一条技能施放拆成一串无法对应回来的记录（旧实现同样的取舍）。
    World->Log().Record(Record);
}

// ---------------------------------------------------------------------------
// Cost（§4.1：CostMode → Cost GE）
// ---------------------------------------------------------------------------

bool UACBattleAbility::HasFocusDrainCost() const
{
    // `GetCostGameplayEffect()` 返回 `CostGameplayEffectClass` 的 CDO（GameplayAbility.h:373），
    // 类没配时是 nullptr —— 那就是"无消耗"（旧 `EACFocusCostMode::None`）。
    const UGameplayEffect* const CostGE = GetCostGameplayEffect();
    if (CostGE == nullptr)
    {
        return false;
    }

    const FGameplayAttribute FocusAttribute = UACBattleAttributeSet::GetFocusAttribute();
    for (const FGameplayModifierInfo& Modifier : CostGE->Modifiers)
    {
        // 判据只看"运算 + 属性"，不看幅度符号：
        // `ACGE_Costs.h` 的三个 Cost GE **全部**只对 `Focus` 做 `Additive`（其中 ClearAll 是 -999），
        // 因此"对 Focus 做 Additive"就等价于"这是个扣专注的 Cost"。
        // 判幅度符号反而会把 `UACGE_Cost_Fixed`（幅度由施加方在 spec 上填）漏掉。
        if (Modifier.ModifierOp == EGameplayModOp::Additive && Modifier.Attribute == FocusAttribute)
        {
            return true;
        }
    }
    return false;
}

bool UACBattleAbility::CheckCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 FGameplayTagContainer* OptionalRelevantTags) const
{
    // 见头文件的裁决说明：`UACGE_Cost_ClearAll` 用 `Additive -999` 表达"清空"，
    // 而引擎的"付得起"判定是 `当前值 + 幅度 < 0 → 付不起`
    //（`FActiveGameplayEffectsContainer::CanApplyAttributeModifiers`，GameplayEffect.cpp:5191-5205），
    // `Focus` 上限只有 100 → 恒为 false → 技能一次都放不出来。
    //
    // 旧语义（`FAbilityExecutor::CanCastSkill` + `GrantFocus`）：施放前提是"专注满"
    //（在外面判），消耗本身是"有多少扣多少、夹到 0"，**不构成施放前提**。
    // 因此对"扣专注型 Cost"直接判为付得起，把"够不够"这件事交给夹取（`CommitBattleCost`）。
    // 其余 Cost GE（将来的固定消耗 / 每秒消耗）仍交回引擎判。
    if (HasFocusDrainCost())
    {
        return true;
    }
    return Super::CheckCost(Handle, ActorInfo, OptionalRelevantTags);
}

bool UACBattleAbility::CommitBattleCost()
{
    // 专注消耗**走 Cost GE**：`CommitAbility` 内部按 `CostGameplayEffectClass` 施加
    //（`CommitAbility` → `CommitCheck` → `CheckCost`（上面已覆写）→ `CommitExecute` → `ApplyCost`
    //  → `ApplyGameplayEffectToOwner`，实现见 GameplayAbility.cpp:559-576 / 1115-1122）。
    if (!CommitAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo))
    {
        return false;
    }

    if (!HasFocusDrainCost())
    {
        return true;
    }

    // 补上引擎不会替我们做的一件事：**把 Base 也夹回合法区间**。
    //
    // 引擎行为（已核实）：GE 的幅度落在属性的 **BaseValue** 上
    //（`ApplyModToAttribute` → `SetAttributeBaseValue`，GameplayEffect.cpp:3803-3848），
    // 而本项目的夹取写在 `PreAttributeChange` —— 引擎只在
    // `FGameplayAttribute::SetNumericValueChecked`（AttributeSet.cpp:100）里回调它，
    // 也就是说 `-999` 会原样写进 Base，只有 Current 被夹成 0。
    // 后果不是"数值难看"而是**功能坏掉**：之后每次回专注都在给 -999 还债
    //（`Focus += 5` 得到 -994 → 再被夹回 0），表现为"清空一次之后再也攒不满专注"，
    // 索利瓦尔的整套循环就断了。
    //
    // 修法：把 Current 的合法值**经 ASC** 写回 Base（`SetNumericAttributeBase` 会同时更新
    // 聚合器的 base，GameplayEffect.cpp:3839-3849；直接写 `FGameplayAttributeData` 会让
    // 聚合器留着旧 base，下一条修饰器求值时又算回负数）。
    // 夹取口径与旧 `FAbilityExecutor::DrainFocus` 的 `FMath::Clamp(Current + Amount, 0, Max)`
    // 以及属性集 `PreAttributeChange` 的 `Focus` 分支逐字一致。
    AACBattleUnitBase* const Caster = GetCasterUnit();
    UAbilitySystemComponent* const ASC = GetAbilitySystemComponentFromActorInfo();
    if (Caster == nullptr || ASC == nullptr || Caster->GetAttributeSet() == nullptr)
    {
        // 属性集缺失时**直接跳过夹取**而不是继续读属性：
        // `ASC->GetNumericAttributeBase` 内部会对"找不到属性集"触发 `ensureMsgf`
        //（AbilitySystemComponent.cpp:399 → GameplayEffect.cpp:3865），
        // 在单机战斗里那是纯噪声（没有属性集就没有 Cost GE 能落下来，也就没有要修的值）。
        return true;
    }

    const FGameplayAttribute FocusAttribute = UACBattleAttributeSet::GetFocusAttribute();
    const float FocusMax = Caster->GetStat(EACStat::FocusMax);

    // ⚠️ 判据必须落在 **Base** 上，不能落在 Current 上：
    //   GE 施加完之后 Current 已经被 `PreAttributeChange` 夹成 0 了（引擎只在
    //   `SetNumericValueChecked` 里回调它），因此"读 Current 再比一次"永远相等、
    //   修复会被静默跳过，而 Base 里那个 -999 原封不动地留着。
    //   这正是"看起来修了、实际没修"的典型写法，写在这里当反例。
    // `GetNumericAttributeBase` 读的就是 `FGameplayAttributeData::GetBaseValue`
    //（GameplayEffect.cpp:3859-3898 的 `GetAttributeBaseValue` 同源），
    // 因此不需要经 `GetAttributeSet()` 自己取。
    const float Current = ASC->GetNumericAttribute(FocusAttribute);
    const float CurrentBase = ASC->GetNumericAttributeBase(FocusAttribute);
    const float Clamped = (FocusMax > 0.f) ? FMath::Clamp(Current, 0.f, FocusMax) : 0.f;

    const bool bBaseOutOfRange = (CurrentBase < 0.f) || (FocusMax > 0.f && CurrentBase > FocusMax);
    if (bBaseOutOfRange)
    {
        // 用 Current 的合法值当新 Base：`DrainFocus` 的语义是"夹取后的当前值就是正确值"，
        // 而 Current 已经被属性集夹过一次了。
        ASC->SetNumericAttributeBase(FocusAttribute, Clamped);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 事件触发（被动）用
// ---------------------------------------------------------------------------

void UACBattleAbility::AddEventTrigger(FGameplayTag HookTag)
{
    if (!HookTag.IsValid())
    {
        // 无效标签会让引擎把它登记进 `GameplayEventTriggeredAbilities` 却在派发时永不命中，
        // 表现为"这个被动从来不触发"且毫无线索 —— 这类内容错误必须在装配期就喊出来。
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] %s: AddEventTrigger 收到无效 Hook 标签，已跳过。"), *GetName());
        return;
    }

    FAbilityTriggerData Trigger;
    Trigger.TriggerTag = HookTag;
    Trigger.TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;
    AbilityTriggers.Add(Trigger);
}

bool UACBattleAbility::ConsumeTrigger(int32 MaxTriggers)
{
    if (MaxTriggers > 0 && TriggerCount >= MaxTriggers)
    {
        return false;
    }
    ++TriggerCount;
    return true;
}

bool UACBattleAbility::IsEnemySkillCastInterruptible(const AACBattleUnitBase& Caster) const
{
    // 旧行为：只有敌方阵营的技能要过 `Hook.EnemySkillCast` 这道可打断的门
    //（`ACAbilityExecutor.cpp:359`：`Unit.GetTeam() == EACTeam::Enemy && Dispatch(...).bInterrupt`）。
    return Caster.GetTeam() == EACTeam::Enemy;
}

void UACBattleAbility::FinishAbility(bool bWasCancelled)
{
    // `EndAbility` 的五个形参见 GameplayAbility.h:628。
    // `bReplicateEndAbility = true`：与构造里 `NetExecutionPolicy = ServerOnly` 配套
    //（单机下没有客户端，这个标志实际不产生任何网络行为，保持与引擎默认调用路径一致）。
    EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, /*bReplicateEndAbility=*/true, bWasCancelled);
}
