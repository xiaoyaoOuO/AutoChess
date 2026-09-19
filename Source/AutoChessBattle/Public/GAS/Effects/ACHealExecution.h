// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 / §4.6 / §7 阶段 3.1）：治疗 Execution。
//
// 为什么需要它：§4.6 的契约是"只有 `FCombatResolver` 能改 `Health`"，
// 而治疗效果**也是改 `Health`**（`FCombatResolver::ApplyHeal` → `AACBattleUnitBase::AddCurrentHP`，
// ACCombatResolver.cpp:353-380）。因此治疗不能写成 `Modifier`（那会绕开管线），
// 必须和伤害一样走 `UGameplayEffectExecutionCalculation`。
//
// 与 `UACDamageExecution` 的分工：
//   本类**不复制**它的伤害逻辑，只做"取量 → 组 `FHealRequest` → 交给 `FCombatResolver::ApplyHeal`"。
//   取 World 的写法与它逐字一致（见 .cpp 的注释），所以两处的口径不会分叉。
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayEffectTypes.h"
#include "GAS/ACBattleAttributeSet.h"
#include "ACHealExecution.generated.h"

/**
 * 治疗 Execution。
 *
 * 装配方式：给治疗 GE 的 `Executions` 加一项本类，并用 SetByCaller（键名 `Data.Heal`）给出治疗量
 * （`UACGE_InstantHeal` 已经这么做了）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACHealExecution : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()

public:
    UACHealExecution();

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

private:
    /**
     * 属性捕获定义（沿用 `UACDamageExecution` 的形态：一个 helper struct 持有一组捕获定义，
     * 在构造函数里填进 `RelevantAttributesToCapture`，引擎文件：GameplayEffectCalculation.h 的 protected 成员）。
     *
     * 治疗为什么要捕获 `MaxHealth`：`FCombatResolver::ApplyHeal` 需要按"缺失生命"截断
     * （ACCombatResolver.cpp:367-369），而它自己会去读目标属性集；这里捕获一份只是为了让
     * "本次治疗的目标最大生命"可留痕（与伤害 Execution 捕获 MaxHealth 的理由相同）。
     * `bSnapshot = false`：沿用"结算时读当前值"的口径。
     */
    struct FACHealStatics
    {
        DECLARE_ATTRIBUTE_CAPTUREDEF(MaxHealth);

        FACHealStatics();
    };

    static const FACHealStatics& HealStatics();
};
