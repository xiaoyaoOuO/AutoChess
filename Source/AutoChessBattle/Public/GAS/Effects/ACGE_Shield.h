// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 的 `ApplyShield` 行 / §7 阶段 3.1）：
// 护盾 GE + 护盾 Execution + 护盾到期移除 GE。
//
// 为什么护盾不能写成 `Modifier`：理由有两条
//   ① `UACBattleAttributeSet::Shield` 的总量权威在 **`FShieldPool`**
//      （实例数组负责 FIFO 与"持续护盾"，ACCombatResolver.cpp:149-153 每次改动都把合计同步进属性）；
//   ② 只改属性而不动实例数组，会让 `SyncTotalToAttributeSet` 在下一次吸收时把属性**覆盖回去**
//      —— 表现为"护盾加了但一打就没"。
//   因此护盾与伤害/治疗一样走 Execution → `FCombatResolver::ApplyShield`（ACCombatResolver.cpp:396-429）。
//
// 时长口径：`FShieldRequest::DurationSeconds`（ACCombatResolver.cpp:413-415）
//   `> 0` = 限时护盾；`< 0` = 持续护盾（`ExpireTime = -1`，永不过期）。
//   对应到 GE：限时用 `HasDuration` + `Data.DurationSeconds`（SetByCaller）；
//   持续用 `Infinite`（`ConfigureAsPersistentShield()`），
//   且量仍按 `FShieldRequest` 的 `< 0` 判据交给结算器，两条判据一致。
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayEffectTypes.h"
#include "GAS/ACBattleAttributeSet.h"
#include "GAS/Effects/ACGameplayEffectBase.h"
#include "ACGE_Shield.generated.h"

/**
 * 护盾 Execution：把 SetByCaller `Data.Shield` 交给 `FCombatResolver::ApplyShield`。
 *
 * 等价关系：旧 `EACActionType::ApplyShield`（ACEffectSystem.cpp:630-644）：
 *   `Amount = Action.Value.Get(Tier)`；
 *   `DurationSeconds = Action.DurationSeconds > 0 ? Action.DurationSeconds : RuleConfig->DefaultShieldDuration`。
 * 那条"缺省时长取规则配置"的兜底**不在这里做**：它需要 `UBattleRuleConfig`，而配置只有战斗世界拿得到。
 * 本类把 `DurationSeconds <= 0` 原样传给结算器（`< 0` = 持续护盾）；
 * 施加方（阶段 3.1b 的能力）负责按规则配置填 `Data.DurationSeconds`。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACShieldExecution : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()

public:
    UACShieldExecution();

    /**
     * 执行入口。
     *
     * ⚠️ 这是 `UFUNCTION(BlueprintNativeEvent)` 生成的 `_Implementation`
     * （引擎声明见 GameplayEffectExecutionCalculation.h:304-314：
     * 「Native subclasses should override the auto-generated Execute_Implementation function and NOT this one」），
     * 签名必须与生成版本**逐字一致，包括末尾的 `const`**。
     */
    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                        FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;
};

/**
 * 护盾到期移除 Execution：清空目标的整个护盾池。
 *
 * 为什么需要它：GAS 5.6 **没有内置护盾**（实施方案附录 C 已核实），
 * `Shield` 只是属性集里的一个普通属性 + `FShieldPool` 的实例数组。
 * 因此"护盾持续 6 秒后消失"这件事没人替我们做 —— 旧代码靠
 * `FCombatResolver::RemoveExpiredShields()`（ACCombatResolver.cpp:158-169）扫实例实现。
 *
 * 本类做的是同一件事的 GE 侧表达：配在 `UAdditionalEffectsGameplayEffectComponent::OnCompleteAlways`
 * （AdditionalEffectsGameplayEffectComponent.h:56）里，宿主护盾 GE 一结束就
 * `FShieldPool::Clear()`（ACCombatResolver.cpp:171-175，它同时把属性集 `Shield` 归零），
 * 因此不会出现"属性上还留着护盾量、实例数组已经空了"的分叉。
 *
 * ⚠️ 已知粒度差异（**已写进报告，请主 agent 裁决**）：
 *   旧实现是**逐实例**按各自的 `ExpireTime` 到期（FIFO 池里可以同时有多层不同剩余时长的护盾），
 *   而"宿主 GE 结束 → 清空整池"在"同一次战斗里叠了两层不同时长的护盾"时会提前清掉未到期的那层。
 *   要做到逐实例到期，需要"每个 `FShieldInstance` 一个 GE"，那是 3.3 接线时的设计选择；
 *   本阶段只把这个可用的近似形态产出来，并把差异写在注释里。
 *   另一条已知差异：`< 0`（持续护盾）在旧代码里由 `RemoveExpiredShields` 的 `IsExpiredAt(-1)` 天然跳过，
 *   本 GE 用 `Infinite` 同样不会到期 —— 这一条**无差异**，故持续护盾不需要挂本清理。
 *
 * ⚠️ **阶段 3.2a 补充（接线时核实引擎行为得出的一条硬约束）**：
 *   `Infinite` 且**没有 `Period`** 的 GE **不会执行 `Executions`** ——
 *   `FActiveGameplayEffectsContainer::InternalExecutePeriodicGameplayEffect` 的早退判据
 *   （GameplayEffect.cpp:2936-2943）是"有持续时间且没有周期 → 直接 return"，
 *   而唯一的执行入口是周期定时器（`ApplyGameplayEffectSpec` :4241-4252 按 `GetPeriod()` 建）。
 *   因此 **`Infinite` 不能用来承载"施加瞬间跑一次 Execution"的护盾/伤害类效果**
 *   （对纯属性修饰的 GE 没有这个问题：修饰是走聚合器，与 Execution 是两条路）。
 *   需要"持续到战斗结束的护盾"时，用**超长 `HasDuration`**（见 `UACGE_Trait_HeavyArmor`
 *   在 `ACGE_ContentEffects.cpp` 里改成 `HasDuration(600)` 的理由与取舍）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACShieldExpireExecution : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()

public:
    UACShieldExpireExecution();

    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                        FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;
};

/**
 * 护盾 GE。
 *
 * 用法（两个分支只能选一个，由 `ConfigureAsDurationShield` / `ConfigureAsPersistentShield` 设定）：
 *   - 限时：`HasDuration`，时长 = SetByCaller `Data.DurationSeconds`（引擎显式支持 SetByCaller 时长）；
 *   - 持续：`Infinite`（旧约定 `DurationSeconds < 0`）。
 *   两种形态的量都是 SetByCaller `Data.Shield`（与 `FShieldRequest::Amount` 同口径）。
 *
 * 为什么量走 SetByCaller 而不是构造里的字面量：旧内容里护盾量来自效果动作的 `Value`
 * （铁壁守望 200、重装词条 160，ACBattleContentDefinitions.cpp:468 / 484），
 * 而"随属性动态计算护盾量"在旧代码里本来就做不到（那段注释明说了）。
 * 走 SetByCaller 后，阶段 3.1b 的能力可以自由地"先算 `DEF * 2` 再填量"，不改本类。
 * 需要固定量的场景由子类（`ACGE_ContentEffects.h`）用 `SetFixedShieldMagnitude` 覆盖。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Shield : public UACGameplayEffectBase
{
    GENERATED_BODY()

public:
    UACGE_Shield();

protected:
    /**
     * 供子类使用的"空构造"：子类（`ACGE_ContentEffects.h` 里的两个固定量护盾）自己决定
     * 时长策略与量，不需要父构造先做一遍。
     *
     * 为什么需要它：C++ 里父类构造总会在子类构造之前跑一次，若父构造已经
     * `ConfigureAsDurationShield()` 并加了 Execution，子类再改就是"覆盖一遍"——
     * 能work但语义混乱（读者要跳两层才知道最终状态）。给一个什么都不做的受保护构造更清楚。
     */
    struct FContentShieldTag {};
    explicit UACGE_Shield(FContentShieldTag);

public:
    /** 配成"限时护盾"：`HasDuration`，时长由 `Data.DurationSeconds` 决定。 */
    void ConfigureAsDurationShield();

    /** 配成"持续护盾"：`Infinite`（旧约定 `DurationSeconds < 0`）。 */
    void ConfigureAsPersistentShield();

    /** 覆盖量：改用字面量而不是 SetByCaller（子类构造里用）。 */
    void SetFixedShieldMagnitude(float Amount);

    /** SetByCaller 键名（唯一常量处，供施加方与 Execution 共用，避免字符串写两遍）。 */
    static FName GetShieldDataName() { return FName(TEXT("Data.Shield")); }
    static FName GetDurationDataName() { return FName(TEXT("Data.DurationSeconds")); }
};

/**
 * "护盾到期清理"GE：自身不产生任何效果，只负责在宿主护盾 GE 结束时清空护盾池。
 *
 * 为什么要单独一个 GE 类而不是在 `UACGE_Shield` 上挂一个"清理用的 Modifier"：
 * `OnCompleteAlways` 要的是 **GE 类**（`TArray<TSubclassOf<UGameplayEffect>>`），
 * 引擎会在宿主结束时为每一项 `MakeOutgoingSpec` 再施加一次；
 * 而"清空护盾池"是一个 `Execution`，只能挂在某个 GE 上 —— 于是就有了这个只有 Execution 的空壳 GE。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_ShieldExpire : public UACGameplayEffectBase
{
    GENERATED_BODY()

public:
    UACGE_ShieldExpire();
};
