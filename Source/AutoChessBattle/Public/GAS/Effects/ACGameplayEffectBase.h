// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 / §4.4 / §7 阶段 3.1）：**全部原生 GE 的公共基类**。
//
// 为什么要有它（而不是每个 GE 各自设一遍）：
//   ① 单机约定只有一处（`bExecutePeriodicEffectOnApplication` / 周期抑制策略 / 默认时长策略），
//      散在 20 个 GE 类里迟早会有一处写错，而写错的表现是"某个状态偶尔不结算"这种极难复现的问题；
//      （注：复制模式不在 GE 上设 —— `UGameplayEffect` **没有** `bReplicate` 字段，见下方类的文档注释。）
//   ② 属性句柄（`FGameplayAttribute`）、SetByCaller 键名、`EGameplayEffectModifierMagnitude`
//      的构造写法是本阶段最容易写错的三个点（`FGameplayModifierInfo` 用 `TEnumAsByte<EGameplayModOp::Type>`，
//      幅度必须经 `FGameplayEffectModifierMagnitude` 包装），收口成受保护辅助函数后，
//      子类构造函数里只出现"数值 + 属性"这两件事。
//
// 与旧 `FACEffectBlock` / `FEffectSystem` 的关系（本阶段**只产出、不接线**，§7 阶段 3.1）：
//   旧的 `BuildEffectBlocks`（ACBattleContentDefinitions.cpp:220-519）与 `FEffectSystem` 一字未动、
//   继续可用；本目录下的新 GE 暂时**没有任何调用者**。删除与接线在 3.2 / 3.3 做。
//
// 数值口径（C6 / D10：**数值一律不动**）：
//   每一个具体 GE 的数值都逐项抄自 `BuildEffectBlocks` / `BuildEquipmentPools` / `BuildAbnormalStates`，
//   来源写在各自的类注释里（格式："来源：ACBattleContentDefinitions.cpp 第 NNN-NNN 行"）。
//
// ⚠️ 关于"百分比 → 倍率"（§4.3 裁决）：
//   `EACModOp::MulPct` 的旧口径是 **1.0 = +100%**（ACRunStatUtils.cpp:50 `Current * (1.f + Modifier.Value)`），
//   而 GAS 的 `MultiplyAdditive` 直接吃倍率，因此换算恒为 `倍率 = 1 + 百分比`。
//   本文件提供 `Conv_PercentToMultiplier()` 一处做这件事，子类不要各自写 `1 + x`。
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayTagContainer.h"
#include "Core/ACBattleTypes.h"
#include "ACGameplayEffectBase.generated.h"

/**
 * 单个属性修饰的取值来源。
 *
 * 为什么需要这个小结构：`FGameplayEffectModifierMagnitude` 有两个常用构造
 * （`FScalableFloat` 字面量 / `FSetByCallerFloat`），但 `FScalableFloat` 与 `FSetByCallerFloat`
 * 都是普通 USTRUCT、彼此没有重载区分，直接重载 `AddModifier` 会让"传一个 float 常量"
 * 与"传一个 SetByCaller 名"在调用点看不出差别。包一层后调用点写的是
 * `FACEffectMagnitude::Literal(20.f)` / `FACEffectMagnitude::SetByCaller(TEXT("Data.Damage"))`，
 * 一眼能看出这次修饰的数值是**内容写死的**还是**施加方传进来的**。
 */
struct AUTOCHESSBATTLE_API FACEffectMagnitude
{
    /** 字面量（内容写死的固定值 / 由 GE 等级缩放的倍率）。 */
    static FACEffectMagnitude Literal(float Value)
    {
        FACEffectMagnitude Result;
        Result.bSetByCaller = false;
        Result.LiteralValue = Value;
        return Result;
    }

    /** SetByCaller（施加方在 spec 上设值；键名用 FName，不需要先注册 GameplayTag）。 */
    static FACEffectMagnitude SetByCaller(FName InDataName)
    {
        FACEffectMagnitude Result;
        Result.bSetByCaller = true;
        Result.DataName = InDataName;
        return Result;
    }

    /** 转成引擎的幅度类型（唯一构造点，见头文件说明）。 */
    FGameplayEffectModifierMagnitude ToMagnitude() const
    {
        if (bSetByCaller)
        {
            FSetByCallerFloat ByCaller;
            ByCaller.DataName = DataName;
            return FGameplayEffectModifierMagnitude(ByCaller);
        }
        return FGameplayEffectModifierMagnitude(FScalableFloat(LiteralValue));
    }

    bool bSetByCaller = false;
    float LiteralValue = 0.f;
    FName DataName;
};

/**
 * 全部原生战斗 GE 的基类。
 *
 * 单机约定：
 *   - `bExecutePeriodicEffectOnApplication = false`：周期 GE 不在施加瞬间先结算一次。
 *     引擎默认是 true（GameplayEffect.h:2239），必须显式关掉
 */
UCLASS(Abstract)
class AUTOCHESSBATTLE_API UACGameplayEffectBase : public UGameplayEffect
{
    GENERATED_BODY()

public:
    UACGameplayEffectBase();

    // ---------------------------------------------------------------------
    // 受保护辅助（子类构造函数专用）
    // ---------------------------------------------------------------------

    /**
     * 百分比 ↔ 倍率换算（§4.3 裁决的唯一入口）。
     *
     * 旧口径（`EACModOp::MulPct`）：`1.0 = +100%`，作用形式是 `值 * (1 + Pct)`
     * （ACRunStatUtils.cpp:50）。GAS 的 `MultiplyAdditive` 直接吃倍率，
     * 因此换算就是 `1 + Pct`：`MulPct = -0.3`（超导线圈 -30% 防御）→ 倍率 `0.7`。
     */
    static float Conv_PercentToMultiplier(float Percent)
    {
        return 1.f + Percent;
    }

    /**
     * 加一条属性修饰。
     *
     * @param Attribute 目标属性（用属性集的 `GetXxxAttribute()` 静态函数取，别手写 `FGameplayAttribute`）。
     * @param ModOp     引擎运算（注意：`EGameplayModOp::Type` 是**非作用域枚举**，且是 `TEnumAsByte` 包装）。
     * @param Magnitude 取值来源（字面量或 SetByCaller）。
     */
    void AddModifier(const FGameplayAttribute& Attribute,
                     TEnumAsByte<EGameplayModOp::Type> ModOp,
                     const FACEffectMagnitude& Magnitude);

    /** 加一条"固定值"属性修饰（最常用的一条：`Additive` + 字面量）。 */
    void AddAdditiveModifier(const FGameplayAttribute& Attribute, float Value);

    /** 加一条"倍率"属性修饰（`MultiplyAdditive` + 字面量倍率）。 */
    void AddMultiplierModifier(const FGameplayAttribute& Attribute, float Multiplier);

    /** 加一条 SetByCaller 属性修饰（值由施加方在 spec 上设）。 */
    void AddSetByCallerModifier(const FGameplayAttribute& Attribute,
                                TEnumAsByte<EGameplayModOp::Type> ModOp,
                                FName DataName);

    /**
     * 加一条 Execution 计算类（§4.6：伤害 / 治疗 / 护盾 / 周期伤害的唯一通道）。
     *
     * 为什么伤害不写成 `Modifier`：§4.6 的新契约是"**只有 `UACDamageExecution` 能调用 `FCombatResolver`**、
     * 只有 `FCombatResolver` 能改 `Health`"。而 `Modifier` 的结果由引擎聚合器直接落到属性上，
     * 会绕开结算管线（七个检查点就丢了，日志里会出现"血量变了但没有伤害记录"）。
     * 因此本项目的伤害/治疗/护盾/周期伤害一律走 `Executions`。
     */
    void AddExecution(TSubclassOf<UGameplayEffectExecutionCalculation> CalculationClass);

    /** 给 GE 授予标签（会**传给目标**：`GrantedTags`，`CachedGrantedTags` 由组件写入）。 */
    void AddGrantedTag(const FGameplayTag& Tag);
    void AddGrantedTags(const FGameplayTagContainer& Tags);

    /** 给 GE 自身打标签（**不传给目标**：`AssetTags`，驱散/筛选按它查）。 */
    void AddAssetTag(const FGameplayTag& Tag);

    /** 设为"持续时间型"，时长 = 字面量秒。 */
    void MakeHasDuration(float DurationSeconds);

    /** 设为"持续时间型"，时长 = SetByCaller（键名默认 `Data.DurationSeconds`）。 */
    void MakeHasDurationByCaller(FName DataName = FName(TEXT("Data.DurationSeconds")));

    /** 设为"永久"（旧 `EACModScope::Permanent` / `BattlePermanent`，§4.3 映射表）。 */
    void MakeInfinite();

    /** 设周期（秒）。`Period` 是 `FScalableFloat`，会按 GE 等级缩放（等级 1 = 原值）。 */
    void SetPeriodSeconds(float Seconds);

    /**
     * 设堆叠策略（§4.4 的 `EACStackPolicy` 映射表的唯一落地处）。
     *
     * @param InStackLimitCount 层数上限；`0` = 无上限（对应旧 `MaxStacks = 0`，
     *                          引擎在 `StackLimitCount <= 0` 时不做上限裁剪，见 GameplayEffect.cpp:4054）。
     */
    void ConfigureStacking(int32 InStackLimitCount,
                           EGameplayEffectStackingDurationPolicy InDurationPolicy,
                           EGameplayEffectStackingPeriodPolicy InPeriodPolicy);

    // ---------------------------------------------------------------------
    // 周期结算参数（供 `UACPeriodicDamageExecution` 从 GE 类上读取）
    // ---------------------------------------------------------------------
    //
    // ⚠️ 为什么是"GE 上的成员"而不是 SetByCaller：
    //   GE 对象（含 CDO）是**共享**的，而这两个参数（每层伤害、是否按 MaxHP 百分比、是否扣层、伤害类型）
    //   都是**内容常量**，不是"每次施加才定"的瞬时数据 —— 它们本来就该写在内容定义里。
    //   放进 SetByCaller 反而要求每个施加点都记得填，漏填就是静默的 0 伤害。
    //   所以：**参数声明为成员**，由执行体从 `Spec.Def` 上读（执行体拿不到 GE 实例，
    //   但 `Spec.Def` 就是 GE 的 CDO/类对象，读到的正是内容里写下的值）。
    //
    // 为什么放在基类而不是 `UACGE_StateBase`：`UACPeriodicDamageExecution` 要读它们，
    // 而执行体不能 `#include` 状态 GE 头（那会形成"GE 头 → 基类头 → 执行体头 → GE 头"的环）。
    // 放在基类头里，执行体只依赖基类头，依赖方向单向。

    /** 是否启用周期伤害（旧 `bPeriodicDamage`）。 */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Periodic")
    bool bEnablePeriodicDamage = false;

    /** 每层每 tick 的伤害；`bPercentOfMaxHP` 时是百分比数值（0.5 = 0.5% 最大生命）。 */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Periodic")
    float PeriodicDamagePerStack = 0.f;

    /** `true` = 按 `MaxHP * PeriodicDamagePerStack / 100` 折算（旧 `bPercentOfMaxHP`）。 */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Periodic")
    bool bPeriodicPercentOfMaxHP = false;

    /** 周期伤害类型（旧 `TickDamageType`）。 */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Periodic")
    EACDamageType PeriodicDamageType = EACDamageType::Technical;

    /**
     * 结算后是否 -1 层（旧 `bConsumeStackOnTick`）。
     * `true` 时由执行体在结算后把 `Spec` 的层数减一。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Periodic")
    bool bPeriodicConsumeStackOnTick = false;

    /**
     * 配周期结算参数（旧 `bPeriodicDamage` / `DamagePerStack` / `bPercentOfMaxHP` / `TickDamageType` /
     * `bConsumeStackOnTick` 五项一次灌进来）。
     *
     * @param PerStack        每层每 tick 的伤害；`0` 表示"这个状态没有周期伤害"
     * @param bPercentOfMaxHP `true` 时按 `MaxHP * PerStack / 100` 折算（与旧代码逐字一致）
     */
    void ConfigurePeriodicDamage(float PerStack, bool bPercentOfMaxHP, EACDamageType DamageType,
                                 bool bConsumeStackOnTick);

    // ---------------------------------------------------------------------
    // "固定量"参数（供 Execution 兜底读取）
    // ---------------------------------------------------------------------
    //
    // ⚠️ **为什么固定量不能写成 `Modifier`**（这是一个真会出错的点，已核实引擎行为）：
    //   对 **Instant GE**，引擎会**立即把每一条 `Modifier` 落到属性上**
    //   （`FActiveGameplayEffectsContainer::InternalExecuteMod`，GameplayEffect.cpp:3907-3963 →
    //    `ApplyModToAttribute`，:3933）。
    //   而本项目的护盾/治疗走的是 Execution → `FCombatResolver`，它自己已经把量落库了
    //   （护盾进 `FShieldPool` 并同步 `Shield` 属性；治疗走 `AddCurrentHP`）。
    //   若同时挂一条 `Modifier`，量就会被加**两遍**（护盾 200 变 400）。
    //
    //   因此：**"量"一律放在下面这两个普通字段上**，由 Execution 从 `Spec.Def` 读
    //   （`Spec.Def` 就是 GE 的类对象/CDO，读到的正是内容里写下的值），
    //   **不往 `Modifiers` 里放任何"只为存数"的条目**。
    //   `Modifiers` 只用于**真正要改属性**的修饰（强化 / 装备 / 状态）。

    /** 固定护盾量（旧 `ApplyShield` 的 `Value`）；`0` = 未设，由 SetByCaller `Data.Shield` 提供。 */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Fixed")
    float FixedShieldAmount = 0.f;

    /** 固定治疗量（旧 `ApplyHeal` 的 `Value`）；`0` = 未设，由 SetByCaller `Data.Heal` 提供。 */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Fixed")
    float FixedHealAmount = 0.f;

private:
    /**
     * `GrantedTags` 组件（`CachedGrantedTags` 的唯一写入方）。
     *
     * ⚠️ 为什么必须走组件、不能直接写 `InheritableOwnedTagsContainer`：
     *   `InheritableOwnedTagsContainer` 在 5.3 起被 `UE_DEPRECATED`（GameplayEffect.h:2316-2318），
     *   而真正被运行时读的是 `CachedGrantedTags`（`GetGrantedTags()`，GameplayEffect.h:2147），
     *   它**只由 `OnGameplayEffectChanged()` 从 `GEComponents` 重新聚合**（GameplayEffect.cpp:339-356）。
     *   原生 GE 的 CDO 不走 `PostLoad`，因此唯一可靠的路径是：
     *   `CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>` +
     *   `SetAndApplyTargetTagChanges()`（TargetTagsGameplayEffectComponent.cpp:58-74，
     *   它内部就是 `ApplyTo(Owner->CachedGrantedTags)`）。
     */
    UPROPERTY(Transient)
    TObjectPtr<class UTargetTagsGameplayEffectComponent> GrantedTagsComponent = nullptr;

    /** `AssetTags` 组件（同上，引擎文件：AssetTagsGameplayEffectComponent.cpp:SetAndApplyAssetTagChanges）。 */
    UPROPERTY(Transient)
    TObjectPtr<class UAssetTagsGameplayEffectComponent> AssetTagsComponent = nullptr;
};
