// 阶段 3.1a 新增（GAS 重构实施方案 §4.4 / §7 阶段 3.1）：
// 周期伤害 Execution。**7 种异常状态的周期结算全走它**。
//
// ---------------------------------------------------------------- 量的口径（必须先读）
// 旧口径（ACAbnormalStates.cpp:293-362 的 `ApplyTickEffects`，伤害段在 320-344）：
//     PerStack  = bPercentOfMaxHP ? (MaxHP * DamagePerStack / 100) : DamagePerStack
//     RawAmount = PerStack * 当前层数
//     伤害类型   = Definition->TickDamageType
//     Reason    = Dot
//     bCanCrit  = false
//     SourceEffectBlockId = "AbnormalTick"
//     结算后若 bConsumeStackOnTick 则层数 -1
//
// 本类的等价实现（**选定的方案，写在这里备查**）：
//   - **每层伤害 / 是否按 MaxHP 百分比 / 伤害类型 / 是否扣层** 四项，
//     读自 **GE 类本身**（`Spec.Def` → `UACGameplayEffectBase` 的 UPROPERTY，
//     由 `ConfigurePeriodicDamage` 在内容定义里一次写定，逐个对应旧 `FACAbnormalStateDef` 的字段）。
//     为什么不用 SetByCaller 传这四项：它们是**内容常量**而不是"每次施放才定"的瞬时数据
//     （旧 `FACAbnormalStateDef::DamagePerStack` 本来就是状态定义的一部分）。
//     放进 SetByCaller 会让每个施加点都必须记得填，漏填就是静默的 0 伤害。
//     本项目的约定：**SetByCaller 只用于真正的瞬时量**（`Data.Damage` / `Data.Heal` / `Data.Shield`）。
//   - **层数** 读 `FGameplayEffectSpec::GetStackCount()`（引擎维护的权威层数 = 旧 `Instance.Stacks`）；
//   - 扣层通过 `GetOwningSpecForPreExecuteMod()` 改 spec 的层数（`SetStackCount`）。
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayEffectTypes.h"
#include "GAS/ACBattleAttributeSet.h"
#include "ACPeriodicDamageExecution.generated.h"

/**
 * 周期伤害 Execution。
 *
 * 装配方式：给状态 GE 的 `Executions` 加一项本类（`UACGE_StateBase` 已经这么做了）。
 * 周期参数由 GE 类上的五个属性提供：
 *   `bEnablePeriodicDamage` / `PeriodicDamagePerStack` / `bPeriodicPercentOfMaxHP` /
 *   `PeriodicDamageType` / `bPeriodicConsumeStackOnTick`
 * （`UACGameplayEffectBase::ConfigurePeriodicDamage` 一次写好五项）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPeriodicDamageExecution : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()

public:
    UACPeriodicDamageExecution();

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

    /**
     * 旧埋点里的来源块名（ACAbnormalStates.cpp:333）。
     * 保留它是为了日志对照（§8.2）：迁移后周期伤害的 `SourceEffectBlockId` 仍是同一个字符串，
     * 基线比对时不会因为"来源名变了"而多出一批差异。
     */
    static FName GetPeriodicSourceBlockId() { return FName(TEXT("AbnormalTick")); }

private:
    /** 捕获定义：周期伤害按"目标最大生命"算百分比时要读 MaxHealth。 */
    struct FACPeriodicStatics
    {
        DECLARE_ATTRIBUTE_CAPTUREDEF(MaxHealth);

        FACPeriodicStatics();
    };

    static const FACPeriodicStatics& PeriodicStatics();
};
