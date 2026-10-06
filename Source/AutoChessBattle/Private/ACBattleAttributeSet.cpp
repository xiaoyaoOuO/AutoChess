// 阶段 1 新增（GAS 重构实施方案 §4.3 / §7 阶段 1），阶段 2 起成为属性唯一权威：属性集实现。
// 本文件**不含任何战斗逻辑**：只有"逐项写入 `FACStatBlock`"与"属性变化钩子"两件事。
// 阶段 2 的读权威已反转：`AACBattleUnitBase::GetStat` 与本集的 CurrentValue 是同一个数。
#include "GAS/ACBattleAttributeSet.h"

#include "GameplayEffectExtension.h"
// `DOREPLIFETIME_*` 宏来自本头（Net/UnrealNetwork.h:288）。
// 为什么不靠 PCH 带进来：本模块用的是引擎共享 PCH（PCHUsageMode.UseExplicitOrSharedPCHs），
// 宏是否可见取决于 PCH 内容；显式包含一次，成本为零，但要省掉"某天 PCH 变了就编译不过"这种偶发问题。
#include "Net/UnrealNetwork.h"

namespace
{
    /**
     * 把属性句柄指向的 `FGameplayAttributeData` 取成可写指针。
     *
     * 为什么不用 `FGameplayAttribute::GetMutableAttribute`：**UE 5.6 没有这个函数**
     * （AttributeSet.h 的 `FGameplayAttribute` 只有 `GetGameplayAttributeData` 等只读重载，
     * 见 AttributeSet.h:120-123）。引擎自己的 `InitFromMetaDataTable` 也是这么做的：
     * 拿 `FStructProperty` → `ContainerPtrToValuePtr<FGameplayAttributeData>(this)`
     * （Private/AttributeSet.cpp:466-471）。
     */
    FGameplayAttributeData* GetAttributeDataFromAttribute(UAttributeSet* Set, const FGameplayAttribute& Attribute)
    {
        const FProperty* const Property = Attribute.GetUProperty();
        if (Set == nullptr || Property == nullptr || !FGameplayAttribute::IsGameplayAttributeDataProperty(Property))
        {
            return nullptr;
        }

        const FStructProperty* const StructProperty = CastField<FStructProperty>(Property);
        // `ContainerPtrToValuePtr` 的可写重载收 `void*`：`Set` 因此必须是非 const 指针
        // （本函数按"我要改它"的语义定义，调用方也只从初始化路径进来）。
        return StructProperty != nullptr ? StructProperty->ContainerPtrToValuePtr<FGameplayAttributeData>(Set) : nullptr;
    }
}

UACBattleAttributeSet::UACBattleAttributeSet()
{
    // 什么都没有做：属性初值全部由 `InitializeFromStatBlock` + 资源初始化显式写入。
    // **刻意不在构造函数里给默认值** —— 那会让"属性集值"与"写入方以为的初值"在
    // 忘记调初始化时静默分叉（两边都是"看起来合理"的数），初始化校验就失去意义。

    // 让 UHT 把属性集按"名字稳定的子对象"复制（引擎基类成员 `bNetAddressable` 默认 false，
    // 只有显式调用才打开；`IsNameStableForNetworking` 在 AttributeSet.cpp 里读的正是它）。
    // 属性集不是组件、没有 ActorComponent 的 `PrimaryComponentTick`，
    // 所以这里**没有** `PrimaryComponentTick.bCanEverTick = false` 这一行 —— 它不需要 Tick，
    // 引擎也不会给它 Tick（UAttributeSet 不是 tickable 的）。
    SetNetAddressable();
}

void UACBattleAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    // 单机项目也照写（框架约定 + `CPF_Net` 校验，见头文件说明）。
    // 23 项逐条列出，不用循环：属性名不是数据，循环就得先建一张"名字表"，反而多一处可漂移的映射。
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, Attack, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, Technique, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, Defense, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, Resistance, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, CritValue, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, CritDamageBonus, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, Lifesteal, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, AttackSpeed, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, DefensePen, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, ResistancePen, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, HealthRegen, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, FocusMax, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, FocusInit, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, FocusRegen, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, FocusPerAttack, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, MentalMax, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, Range, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, Health, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, Shield, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, Focus, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UACBattleAttributeSet, Mental, COND_None, REPNOTIFY_Always);
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------

void UACBattleAttributeSet::InitializeFromStatBlock(const FACStatBlock& Block)
{
    // 逐项显式写入：`EACStat` 索引 → 属性指针 → BaseValue/CurrentValue。
    // 不写 "for (i in 0..ACStatCount) 按枚举顺序硬塞进一个属性数组" 这种循环 ——
    // 那样映射关系藏在下标里，`EACStat` 中间插一项就会把后面全部错位，而且编译器不会说话。
    // 这里每一项都由 `GetAttributeForStat` 的显式 switch 决定，缺项时编译器报"未处理的枚举值"。
    for (int32 Index = 0; Index < ACStatCount; ++Index)
    {
        const EACStat Stat = static_cast<EACStat>(Index);

        // 口径与 `FBattleStatPipeline::SetBaseFromBlock`（Private/ACBattleStats.cpp:8-18，阶段 2 仍在）一致：
        // 数组缺项取 0（`FACStatBlock::Get` 也是这个口径）。
        SetAttributeDataValue(this, GetAttributeForStat(Stat), Block.Get(Stat));
    }
}

void UACBattleAttributeSet::InitializeResourceAttribute(const FGameplayAttribute& Attribute, float Value)
{
    SetAttributeDataValue(this, Attribute, Value);
}

void UACBattleAttributeSet::SetAttributeDataValue(UAttributeSet* Set, const FGameplayAttribute& Attribute, float Value)
{
    if (Set == nullptr || !Attribute.IsValid())
    {
        return;
    }

    if (FGameplayAttributeData* const AttributeData = GetAttributeDataFromAttribute(Set, Attribute))
    {
        AttributeData->SetBaseValue(Value);
        // CurrentValue 同时对齐：初始化时还没有任何 GE 修饰器，Base 与 Current 必然相等；
        // 只写 Base 会让"初始化后立刻读 Current"（阶段 2 的 `GetStat` / `GetCurrentHP` 就是读 Current）
        // 读到 0，与写入方以为的值不一致 —— 那是阶段 2 最容易踩的坑。
        // （运行期不走这里：那时由 GAS 聚合器按 Base + 修饰器算出 Current。）
        AttributeData->SetCurrentValue(Value);
    }
}

FGameplayAttribute UACBattleAttributeSet::GetAttributeForStat(EACStat Stat)
{
    switch (Stat)
    {
    case EACStat::MaxHP:            return GetMaxHealthAttribute();
    case EACStat::ATK:              return GetAttackAttribute();
    case EACStat::TECH:             return GetTechniqueAttribute();
    case EACStat::DEF:              return GetDefenseAttribute();
    case EACStat::RES:              return GetResistanceAttribute();
    case EACStat::CritValue:        return GetCritValueAttribute();
    case EACStat::CritDamageBonus:  return GetCritDamageBonusAttribute();
    case EACStat::Lifesteal:        return GetLifestealAttribute();
    case EACStat::ASPD:             return GetAttackSpeedAttribute();
    case EACStat::DEFPen:           return GetDefensePenAttribute();
    case EACStat::RESPen:           return GetResistancePenAttribute();
    case EACStat::HpRegen:          return GetHealthRegenAttribute();
    case EACStat::FocusMax:         return GetFocusMaxAttribute();
    case EACStat::FocusInit:        return GetFocusInitAttribute();
    case EACStat::FocusRegen:       return GetFocusRegenAttribute();
    case EACStat::FocusPerAttack:   return GetFocusPerAttackAttribute();
    case EACStat::MentalMax:        return GetMentalMaxAttribute();
    case EACStat::Range:            return GetRangeAttribute();
    case EACStat::Count:
    default:
        // 越界 / 哨兵：返回无效属性。调用方（InitializeFromStatBlock）会跳过它。
        return FGameplayAttribute();
    }
}

// ---------------------------------------------------------------------------
// 属性变化钩子
// ---------------------------------------------------------------------------

void UACBattleAttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
    Super::PreAttributeBaseChange(Attribute, NewValue);

    // 为什么必须有这个函数（它是本项目"专注清空"能工作的前提）：
    //
    //   GE 的属性修饰（`Additive`）落在 **BaseValue** 上（`ApplyModToAttribute` → `SetAttributeBaseValue`），
    //   而 `PreAttributeChange` **不会被这条路径调用** —— 它只由 `SetNumericValueChecked` 触发。
    //   于是"清空全部专注"（幅度 -999）会把 BaseValue 写成 -999：
    //     ① 之后每次回专注都在"还债"，技能永远攒不满；
    //     ② `FActiveGameplayEffectsContainer::CanApplyAttributeModifiers` 用的是 **BaseValue**
    //        （`GetNumericValueChecked`），而它对 `Additive` 做 `当前值 + 幅度 < 0 → 付不起` 的判定
    //        （`GameplayEffect.cpp:5201`），因此 `100 + (-999) < 0` 会让"清空专注"这个 Cost
    //        **恒被判付不起，技能一次都放不出来**。
    //
    //   在 Base 层夹取后，`-999` 会变成 `FocusMax - Focus`（恰好等于"清空"），上面两条同时消失。
    //   本函数只在 GE 修饰属性时被调，因此对 `FCombatResolver` 直写属性集的热路径零开销。
    if (FMath::IsNaN(NewValue))
    {
        NewValue = 0.f;
        return;
    }

    const FProperty* const Property = Attribute.GetUProperty();
    if (Property == nullptr)
    {
        return;
    }

    // 注意：本函数是 `const`，所以只能读其它属性的 **BaseValue**（不能用 GetCurrentValue，
    // 那需要非 const）。上限按 Base 判定是刻意的：上限本身也被 GE 修饰时（例如 `FocusMax -20`），
    // 用 Base 判定会让夹取"慢一步"，但下一帧重算时会归位，不会累积偏差。
    if (Property == GetFocusAttribute().GetUProperty())
    {
        const float FocusMaxBase = FocusMax.GetBaseValue();
        NewValue = (FocusMaxBase > 0.f) ? FMath::Clamp(NewValue, 0.f, FocusMaxBase) : 0.f;
    }
    else if (Property == GetHealthAttribute().GetUProperty())
    {
        const float MaxHealthBase = MaxHealth.GetBaseValue();
        NewValue = (MaxHealthBase > 0.f) ? FMath::Clamp(NewValue, 0.f, MaxHealthBase) : 0.f;
    }
    else if (Property == GetMentalAttribute().GetUProperty())
    {
        const float MentalMaxBase = MentalMax.GetBaseValue();
        NewValue = (MentalMaxBase > 0.f) ? FMath::Clamp(NewValue, 0.f, MentalMaxBase) : 0.f;
    }
    else if (Property == GetShieldAttribute().GetUProperty())
    {
        NewValue = FMath::Max(0.f, NewValue);
    }
    else
    {
        // 其余属性（含全部基础属性）：不允许为负。
        // 与 `PreAttributeChange` 的末尾保持一致（那里也是"其余属性 Max(0)"）。
        NewValue = FMath::Max(0.f, NewValue);
    }
}

void UACBattleAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
    Super::PreAttributeChange(Attribute, NewValue);

    // NaN 先挡掉：NaN 与任何数比较都是 false，会让下面的 Clamp 全部失效，
    // 之后 `Health = NaN` 会污染整场战斗且无法通过日志回溯（日志里它是 "nan"）。
    if (FMath::IsNaN(NewValue))
    {
        NewValue = 0.f;
        return;
    }

    // 取属性指针做比较，不用 `Attribute == GetHealthAttribute()`：
    // 后者每次调用都要 `FindFieldChecked` 查一次反射表，而本函数在每次属性写入前都会被调。
    const FProperty* const Property = Attribute.GetUProperty();
    if (Property == nullptr)
    {
        return;
    }

    if (Property == GetHealthAttribute().GetUProperty())
    {
        // 当前生命：0 ≤ Health ≤ MaxHealth（当前值，含修饰器）。
        const float MaxHealthValue = MaxHealth.GetCurrentValue();
        NewValue = (MaxHealthValue > 0.f) ? FMath::Clamp(NewValue, 0.f, MaxHealthValue) : 0.f;
    }
    else if (Property == GetFocusAttribute().GetUProperty())
    {
        // 当前专注：0 ≤ Focus ≤ FocusMax。
        const float FocusMaxValue = FocusMax.GetCurrentValue();
        NewValue = (FocusMaxValue > 0.f) ? FMath::Clamp(NewValue, 0.f, FocusMaxValue) : 0.f;
    }
    else if (Property == GetMentalAttribute().GetUProperty())
    {
        // 精神：0 ≤ Mental ≤ MentalMax。注意它只是 FMentalSystem 的镜像（见头文件）。
        const float MentalMaxValue = MentalMax.GetCurrentValue();
        NewValue = (MentalMaxValue > 0.f) ? FMath::Clamp(NewValue, 0.f, MentalMaxValue) : 0.f;
    }
    else if (Property == GetShieldAttribute().GetUProperty())
    {
        // 护盾：只有下界（无上限，GAS 无内置护盾，量由 FCombatResolver 决定）。
        NewValue = FMath::Max(0.f, NewValue);
    }
    else
    {
        // 其余（含 18 个真实基础属性）：属性值不为负。
        // 与阶段 1 的 `FBattleStatPipeline::Recompute` 的 `FMath::Max(0.f, Value)`
        //（该函数已随修饰器数组一起删除，口径写在 GAS 迁移文档里）同一口径，
        // 保证阶段 2 把读权威反转过去之后数值不会因"负值处理不同"而变。
        NewValue = FMath::Max(0.f, NewValue);
    }
}

void UACBattleAttributeSet::PostGameplayEffectExecute(const struct FGameplayEffectModCallbackData& Data)
{
    Super::PostGameplayEffectExecute(Data);

    // -----------------------------------------------------------------------
    // 管线守卫（§4.6 第 3 条 / §8.2 第 3 项不变量）
    //
    // 契约：任何 `Health` 变化都必须经过 `UACDamageExecution` → `FCombatResolver`。
    //
    // ⚠️ 为什么这里**不是** `ensure`（阶段 2 的实现裁决，见头文件的长说明）：
    //   本阶段的伤害路径（`FCombatResolver` → `AACBattleUnitBase::SetHealthFromResolver`）
    //   是**直写 `FGameplayAttributeData`**，不走 GE Modifier，因此**根本不会触发本函数**——
    //   正常伤害连这里都进不来。用一个"永远不会被正常伤害触发"的钩子去做"
    //   非法伤害要立刻炸"的断言，等于断言了一个不存在的场景：
    //     ① 它抓不到真实伤害（真实伤害走直写）；
    //     ② 阶段 3/4 内容开始用 GE 之后，治疗 / 吸血 / 处决 / 周期结算这些**合法**的
    //        `Health` GE 修改会全部命中这里，`ensure` 会当场打断整场战斗。
    //   而现在还没有任何"合法 GE 应当带什么标签"的判据可供区分 —— 判据不存在时写断言是错的。
    //   因此：**记 Warning**，把"唯一允许的路径"写进日志，等阶段 3 的实际调用点出来后再复核是否升级。
    // -----------------------------------------------------------------------
    if (Data.EvaluatedData.Attribute == GetHealthAttribute())
    {
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] UACBattleAttributeSet: `Health` 被 GE 直接修改（新值 %.3f）。"
                    "按 §4.6 契约，任何生命变化都必须经过 UACDamageExecution → FCombatResolver"
                    "（FCombatResolver 直写属性集，不经 GE Modifier）。请检查这次 GE 的来源。"),
               Data.EvaluatedData.Magnitude);
    }
}
