
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
    // 单机项目：不参与网络复制与客户端预测。
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
    
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
    
    bReplicateInputDirectly = false;
}

// ---------------------------------------------------------------------------
// 世界 / 施法者
// ---------------------------------------------------------------------------

UBattleWorld* UACBattleAbility::FindBattleWorldFromActor(const AActor* Actor)
{
    return UBattleWorld::FindFromActor(Actor);
}

UBattleWorld* UACBattleAbility::GetBattleWorld() const
{
    // 用 Avatar 而不是 ASC：`CanActivateAbility` 会在 CDO 上被调用，那时没有 CurrentActorInfo。
    return FindBattleWorldFromActor(GetAvatarActorFromActorInfo());
}

AACBattleUnitBase* UACBattleAbility::GetCasterUnit() const
{
    // Avatar 是"能力的物理执行者"；本方案里 Owner 与 Avatar 是同一个 Actor（§2.2），
    // 因此这里用 Avatar 取施法者单位是准确的，且能在将来"能力挂在别的 Actor 上"时自然降级。
    return Cast<AACBattleUnitBase>(GetAvatarActorFromActorInfo());
}


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

    // 被动能力不在这里判目标
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


bool UACBattleAbility::IsInRange(const AACBattleUnitBase& Self, const AACBattleUnitBase& Target, float RangeOverride) const
{
    // `RangeOverride <= 0` = 用施法者射程
    const float Range = RangeOverride > 0.f ? RangeOverride : Self.GetBaseRange();
    // 六边形距离：UACHexGridStatics（六边形几何，GAS 没有对应物，§11 B）。
    const int32 Distance = UACHexGridStatics::Distance(Self.GetCell(), Target.GetCell());
    return static_cast<float>(Distance) <= Range;
}

bool UACBattleAbility::MoveOneStepTowards(UBattleWorld& World, AACBattleUnitBase& Unit, FUnitId TargetId)
{
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

    // ② 钩子（可取消/可改值）
    FBattleHookContext MoveContext;
    MoveContext.Source = Unit.GetUnitId();
    MoveContext.Time = FACBattleTime::ElapsedSeconds(World);
    DispatchHook(World, BattleTags::Hook_UnitMoved, MoveContext, &Unit);

    Unit.SetActionState(EACUnitActionState::Idle);
    return true;
}


FGameplayEventData UACBattleAbility::MakeEventPayload(const UBattleWorld& World, const FBattleHookContext& Context)
{
    FGameplayEventData Payload;

    // `Instigator` / `Target` 收的是 `AActor*`，因此必须把整型句柄还原成单位
    const AACBattleUnitBase* const SourceUnit = World.FindUnit(Context.Source);
    const AACBattleUnitBase* const TargetUnit = World.FindUnit(Context.Target);
    Payload.Instigator = static_cast<const AActor*>(SourceUnit);
    Payload.Target = static_cast<const AActor*>(TargetUnit);
    Payload.EventMagnitude = Context.FloatValue;
    
    Payload.InstigatorTags = Context.Tags;
    return Payload;
}

FGameplayEventData UACBattleAbility::MakeEventPayload(const FBattleHookContext& Context) const
{
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

    // ② 玩法被动（只负责触发）：同一份上下文发给指定单位的 ASC。
    //    顺序与条件都在这里定死：EventBus 先派发（订阅者可能改值 / 否决），
    //    GameplayEvent 后发，且被否决的动作不再触发被动 ——
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
    // 被动能力要用时间就直接取 `FACBattleTime::Now(*GetBattleWorld())`；
    return Context;
}

// GameplayEffect 施加

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

    // context 的 instigator 取来源 ASC（MakeEffectContext 内部用 OwnerActor/AvatarActor）。这不是可选项：AggregateBySource 的状态 GE
    // 靠 instigator 判定"同源才叠加"，
    // 没有 instigator 时同一状态的多次施加会变成多个独立实例。
    const FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();

    // Level 固定 1：本项目的 GE 数值都是内容常量或 SetByCaller，没有按能力等级缩放的修饰
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
        SpecHandle.Data->SetSetByCallerMagnitude(Pair.Key, Pair.Value);
    }
    
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


//TODO:不应该放在Ability基类，应该作为一个effect，在特定的GA里施加
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
        ApplyEffectToUnit(Unit, Unit, UACGE_FocusGain::StaticClass(),
                          MakeSetByCaller(UACGE_FocusGain::GetFocusGainDataName(), Amount));
    }
    
    const float After = ASC->GetNumericAttribute(FocusAttribute);
    const float FocusMax = Unit.GetStat(EACStat::FocusMax);

    FBattleHookContext Context;
    Context.Target = Unit.GetUnitId();
    Context.Time = FACBattleTime::ElapsedSeconds(*World);
    Context.FloatValue = After;
    
    // `FocusMax <= 0`（无专注条的单位）永远不派发，与旧 `GrantFocus` 的 `Old < Focus.Max` 一致。
    if (FocusMax > 0.f && Before < FocusMax && After >= FocusMax)
    {
        World->Events().Dispatch(BattleTags::Hook_FocusFull, Context);
    }

    if (!FMath::IsNearlyEqual(Before, After))
    {
        World->Events().Dispatch(BattleTags::Hook_FocusChanged, Context);

        // 埋点：Source = InvalidUnitId（专注是单位自身资源，没有"来源单位"）、
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

// 事件触发（被动）用

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
    return Caster.GetTeam() == EACTeam::Enemy;
}

void UACBattleAbility::FinishAbility(bool bWasCancelled)
{
    EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, /*bReplicateEndAbility=*/true, bWasCancelled);
}
