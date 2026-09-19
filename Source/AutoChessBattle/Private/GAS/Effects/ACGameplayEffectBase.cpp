// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 / §4.4 / §7 阶段 3.1）：原生 GE 基类实现。
//
// 本文件的全部写法都对应一处引擎事实，逐条写在下面；**没核实过的符号一个都没写**。
#include "GAS/Effects/ACGameplayEffectBase.h"

#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UACGameplayEffectBase::UACGameplayEffectBase()
{
    // ---------------------------------------------------------------------
    // 单机约定
    // ---------------------------------------------------------------------
    // ⚠️ 这里**没有**"关掉复制"的一行：`UGameplayEffect` 在 5.6 里没有 `bReplicate` 字段
    // （复制模式归 `UAbilitySystemComponent::SetReplicationMode` / `EGameplayEffectReplicationMode` 管，
    //  声明见 AbilitySystemComponent.h:81 / 259 / 262）。写一个不存在的字段会直接编译失败，
    // 因此"单机不复制"由 ASC 侧保证，GE 侧不做多余声明。

    // `bExecutePeriodicEffectOnApplication`（GameplayEffect.h:2239，默认 true）：
    // 必须关掉。旧口径是"施加当帧不结算、一个周期后才结算"
    // （ACAbnormalStates.cpp:177 `NextTickTime = NowSeconds + TickInterval`），
    // 引擎默认会在施加瞬间先执行一次，那会让每种异常状态多打一次伤害 —— 数值就变了（违反 D10）。
    bExecutePeriodicEffectOnApplication = false;

    // `PeriodicInhibitionPolicy`（GameplayEffect.h:2243）：解除抑制后不重置周期。
    // 单机阶段 3 不用抑制（Inhibition），写上是为了"不会因为将来有人用抑制而出现额外一次结算"。
    PeriodicInhibitionPolicy = EGameplayEffectPeriodInhibitionRemovedPolicy::NeverReset;

    // 表现侧默认口径（阶段 4 接 GameplayCue 时生效，这里只定基调）。
    bRequireModifierSuccessToTriggerCues = true;   // GameplayEffect.h:2291
    bSuppressStackingCues = true;                  // GameplayEffect.h:2295（只有第一层播 cue）

    // 默认时长策略 = Instant（引擎枚举默认值就是 Instant，见 GameplayEffect.h:663-671）。
    // "即时型"是绝大多数具体 GE 的形态（伤害/治疗/护盾/给专注），
    // 时长型 GE（状态/装备/强化）由子类显式调 MakeHasDuration* / MakeInfinite。
    DurationPolicy = EGameplayEffectDurationType::Instant;

    // 默认堆叠 = 不叠加（引擎默认 `EGameplayEffectStackingType::None`，
    // 见 GameplayEffectTypes.h:176-184）。只有状态类 GE 会改它。
    StackingType = EGameplayEffectStackingType::None;

    // ---------------------------------------------------------------------
    // 标签组件
    // ---------------------------------------------------------------------
    // 必须在**构造函数**里 `CreateDefaultSubobject`：`UGameplayEffect::PostInitProperties`
    // 会用 `GetDefaultSubobjects` 检查"原生 GE 的 GEComponent 有没有漏加进 GEComponents"
    // （GameplayEffect.cpp:179-194），只有默认子对象才会出现在那份列表里；
    // 而 PostInitProperties 也会替我们把它们加进 `GEComponents`（那是 protected 成员）。
    GrantedTagsComponent = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("TargetTagsComponent"));
    AssetTagsComponent = CreateDefaultSubobject<UAssetTagsGameplayEffectComponent>(TEXT("AssetTagsComponent"));
}

// ---------------------------------------------------------------------------
// 属性修饰
// ---------------------------------------------------------------------------

void UACGameplayEffectBase::AddModifier(const FGameplayAttribute& Attribute,
                                       TEnumAsByte<EGameplayModOp::Type> ModOp,
                                       const FACEffectMagnitude& Magnitude)
{
    if (!Attribute.IsValid())
    {
        // 内容写错（例如把 `EACStat::Count` 传了进来）时**不添加**这条修饰，而不是加一条作用于无效属性的。
        // 加进去的后果是引擎在聚合时对无效属性静默跳过，表现为"这条内容不生效"却查不出原因。
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] UACGameplayEffectBase::AddModifier: 属性句柄无效，已跳过（GE=%s）。"),
               *GetName());
        return;
    }

    // `FGameplayModifierInfo` 的字段名（GameplayEffect.h:542-575）：
    //   `Attribute` / `ModifierOp` / `ModifierMagnitude`（+ SourceTags / TargetTags / EvaluationChannelSettings）。
    // `ModifierOp` 的类型是 `TEnumAsByte<EGameplayModOp::Type>`（GameplayEffect.h:556），
    // 所以这里不能传 `EGameplayModOp::Additive` 之外的枚举（例如 `EGameplayEffectStackingType`）。
    FGameplayModifierInfo Info;
    Info.Attribute = Attribute;
    Info.ModifierOp = ModOp;
    Info.ModifierMagnitude = Magnitude.ToMagnitude();
    Modifiers.Add(Info);
}

void UACGameplayEffectBase::AddAdditiveModifier(const FGameplayAttribute& Attribute, float Value)
{
    AddModifier(Attribute, EGameplayModOp::Additive, FACEffectMagnitude::Literal(Value));
}

void UACGameplayEffectBase::AddMultiplierModifier(const FGameplayAttribute& Attribute, float Multiplier)
{
    AddModifier(Attribute, EGameplayModOp::MultiplyAdditive, FACEffectMagnitude::Literal(Multiplier));
}

void UACGameplayEffectBase::AddSetByCallerModifier(const FGameplayAttribute& Attribute,
                                                  TEnumAsByte<EGameplayModOp::Type> ModOp,
                                                  FName DataName)
{
    AddModifier(Attribute, ModOp, FACEffectMagnitude::SetByCaller(DataName));
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

void UACGameplayEffectBase::AddExecution(TSubclassOf<UGameplayEffectExecutionCalculation> CalculationClass)
{
    if (CalculationClass == nullptr)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] UACGameplayEffectBase::AddExecution: 计算类为空，已跳过（GE=%s）。"),
               *GetName());
        return;
    }

    // `FGameplayEffectExecutionDefinition` 的字段名（GameplayEffect.h:506-534）：
    //   `CalculationClass`（`TSubclassOf<UGameplayEffectExecutionCalculation>`）
    //   / `PassedInTags` / `CalculationModifiers` / `ConditionalGameplayEffects`。
    FGameplayEffectExecutionDefinition Definition;
    Definition.CalculationClass = CalculationClass;
    Executions.Add(Definition);
}

// ---------------------------------------------------------------------------
// 标签
// ---------------------------------------------------------------------------

void UACGameplayEffectBase::AddGrantedTag(const FGameplayTag& Tag)
{
    if (GrantedTagsComponent == nullptr || !Tag.IsValid())
    {
        return;
    }

    // `GetConfiguredTargetTagChanges()` 返回的是组件里那份 `FInheritedTagContainer` 的 **const 引用**
    // （TargetTagsGameplayEffectComponent.h:25），不能就地改，必须拷一份、加标签、再整体写回。
    FInheritedTagContainer Container = GrantedTagsComponent->GetConfiguredTargetTagChanges();
    Container.Added.AddTag(Tag);

    // 写回 + 立即落到 `UGameplayEffect::CachedGrantedTags`（组件实现里那一句 `ApplyTo`）。
    GrantedTagsComponent->SetAndApplyTargetTagChanges(Container);
}

void UACGameplayEffectBase::AddGrantedTags(const FGameplayTagContainer& Tags)
{
    for (const FGameplayTag& Tag : Tags)
    {
        AddGrantedTag(Tag);
    }
}

void UACGameplayEffectBase::AddAssetTag(const FGameplayTag& Tag)
{
    if (AssetTagsComponent == nullptr || !Tag.IsValid())
    {
        return;
    }

    FInheritedTagContainer Container = AssetTagsComponent->GetConfiguredAssetTagChanges();
    Container.Added.AddTag(Tag);
    AssetTagsComponent->SetAndApplyAssetTagChanges(Container);
}

// ---------------------------------------------------------------------------
// 时长 / 周期 / 堆叠
// ---------------------------------------------------------------------------

void UACGameplayEffectBase::MakeHasDuration(float DurationSeconds)
{
    DurationPolicy = EGameplayEffectDurationType::HasDuration;

    // `DurationMagnitude` 是 `FGameplayEffectModifierMagnitude`（GameplayEffect.h:2231）。
    // 用 `FScalableFloat` 包字面量：`GetValueAtLevel` 会按 GE 等级缩放，
    // 引擎自带的示例写法同样是 `FScalableFloat`（GameplayEffect.h:289 的构造）。
    DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DurationSeconds));
}

void UACGameplayEffectBase::MakeHasDurationByCaller(FName DataName)
{
    DurationPolicy = EGameplayEffectDurationType::HasDuration;

    // SetByCaller 时长是引擎**显式支持**的路径：`FActiveGameplayEffectsContainer::ApplyGameplayEffectSpec`
    // 在 `AttemptCalculateDurationFromDef` 失败时会回退到按 SetByCaller 重算时长
    // （GameplayEffect.cpp:4204-4207）。因此"施加方决定时长"（旧效果动作的 `DurationSeconds` 覆盖）
    // 可以用这个通道表达，不需要在能力里改 spec 的 Duration。
    FSetByCallerFloat ByCaller;
    ByCaller.DataName = DataName;
    DurationMagnitude = FGameplayEffectModifierMagnitude(ByCaller);
}

void UACGameplayEffectBase::MakeInfinite()
{
    // 旧 `EACModScope::Permanent` / `BattlePermanent` → `DurationPolicy = Infinite`（§4.3 映射表）。
    DurationPolicy = EGameplayEffectDurationType::Infinite;

    // 清掉 DurationMagnitude：Infinite 时引擎根本不读它（GameplayEffect.cpp:1789-1792），
    // 留着会让编辑器校验报"配置矛盾"。赋默认构造值即"ScalableFloat 0"。
    DurationMagnitude = FGameplayEffectModifierMagnitude();
}

void UACGameplayEffectBase::SetPeriodSeconds(float Seconds)
{
    // `Period` 是 `FScalableFloat`（GameplayEffect.h:2235），不是 float。
    // 直接赋值 `FScalableFloat` 会走它的 `operator=`（ScalableFloat.h:108，防重复 handle），
    // 这里用构造函数写更明确。
    Period = FScalableFloat(Seconds);
}

void UACGameplayEffectBase::ConfigureStacking(int32 InStackLimitCount,
                                             EGameplayEffectStackingDurationPolicy InDurationPolicy,
                                             EGameplayEffectStackingPeriodPolicy InPeriodPolicy)
{
    // §4.4 映射表：旧 `EACStackPolicy` → 引擎三件套。落点在 `UGameplayEffect` 的 Stacking 段
    // （GameplayEffect.h:2372-2394）。
    //
    // ⚠️ 为什么 `StackingType` 一律取 `AggregateBySource` 而不取 `None`：
    //   `None` 的语义是"每次施加都是独立实例"（GameplayEffectTypes.h:178-179），
    //   那样旧容器的"同一状态只有一个实例、层数累加"就不成立了；
    //   `AggregateBySource` 才是"同一个 GE 的多次施加累加层数"（GameplayEffectTypes.h:180-181）。
    StackingType = EGameplayEffectStackingType::AggregateBySource;
    StackLimitCount = InStackLimitCount;
    StackDurationRefreshPolicy = InDurationPolicy;
    StackPeriodResetPolicy = InPeriodPolicy;

    // `StackExpirationPolicy` 固定 `ClearEntireStack`：
    //   旧容器是"到期就整个实例消失"（ACAbnormalStates.cpp:364-378 的 `RemoveExpired` 直接 `RemoveAt`），
    //   不是"掉一层再续时长"。`RemoveSingleStackAndRefreshDuration` 会让带时长的状态永不消失。
    StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::ClearEntireStack;
}

void UACGameplayEffectBase::ConfigurePeriodicDamage(float PerStack, bool bPercentOfMaxHP, EACDamageType DamageType,
                                                   bool bConsumeStackOnTick)
{
    // 五项一次灌进来，逐项对应旧 `FACAbnormalStateDef` 的字段名（Core/ACDataTypes.h:89-104）。
    // 读它们的是 `UACPeriodicDamageExecution`（从 `Spec.Def` 上读，见基类头文件的说明）。
    bEnablePeriodicDamage = (PerStack > 0.f);
    PeriodicDamagePerStack = PerStack;
    bPeriodicPercentOfMaxHP = bPercentOfMaxHP;
    PeriodicDamageType = DamageType;
    bPeriodicConsumeStackOnTick = bConsumeStackOnTick;
}
