// 阶段 1 新增、阶段 2 接通（GAS 重构实施方案 §4.6 / §7 阶段 1–2）：伤害 Execution。
//
// ============================ 新契约（§4.6，阶段 2 起生效） ============================
//   **只有本类能调用 `FCombatResolver`；只有 `FCombatResolver` 能改 `Health` 属性。**
//
// 为什么收这么紧：结算管线（`FCombatResolver`）留下的 7 个检查点
// （Requested → AfterMitigation → AfterIncreaseReduction → RedirectedOut → ShieldAbsorbed →
//  AppliedHpLoss → Overkill）是日志与平衡分析的**唯一数据源**（写入 `FDamageResult`）。
// 只要有一条路径绕过它直接改 `Health`，日志就会出现"血量变了但没有对应的伤害记录"，
// 之后任何数值问题都无法回溯。因此 `UACBattleAttributeSet::PostGameplayEffectExecute`
// 里对"绕过管线的 GE 直改"加了守卫（§8.2 第 3 项不变量；阶段 2 的形式是 `UE_LOG(Warning)`
// 而不是 `ensure`，理由见属性集头文件），而本类是唯一被允许触发管线的入口 —— 守卫与入口成对才有意义。
//
// ⚠️ 一个必须记住的事实（阶段 2 落实）：本类调的 `FCombatResolver` 是**直写属性集**
//（`AACBattleUnitBase::SetHealthFromResolver` 直写 `FGameplayAttributeData`），
// 不是通过 GE Modifier。因此正常伤害**不会**触发 `PostGameplayEffectExecute`，
// 也不会触发 `PreAttributeChange` —— 那两条钩子只覆盖"经 ASC 改属性"的路径（阶段 3/4 的 GE）。
// 这也是本类**不**使用 `OutExecutionOutput.AddOutputModifier` 的原因（见实现里的取舍说明）。
//
// 为什么用 `UGameplayEffectExecutionCalculation` 而不是自研动作原语：
// §4.2 明确删除 `CustomAction` + `IBattleCustomAction` 字符串白名单，改用 Execution 子类 ——
// 类型安全（不用在运行时按字符串找注册表）、能声明属性捕获（`RelevantAttributesToCapture`）、
// 能在编辑器里被 GE 直接引用。
//
// ============================ 阶段 2 的能力边界 ============================
// 已接通：
//   - 从 `ExecutionParams` 取 Source/Target 的 `UACBattleAttributeSet` 与 `UBattleWorld`；
//   - 从 `FGameplayEffectSpec` 取 `SetByCaller` 的原始量（**键名 `Data.Damage`**，见实现的约定说明）；
//   - 组 `FDamageRequest`（Source/Target 用整型 `FUnitId`，§5.1 裁决）；
//   - 调 `UBattleWorld::Combat().ApplyDamage(Request)`；
//   - 把 7 个检查点写进引擎日志（VeryVerbose），**不新增结构化产物记录**（见实现的说明）。
// 尚未做（阶段 3/4）：
//   - 用捕获到的属性值参与伤害公式（当前公式仍完全在 `FCombatResolver` 里，C6/D10 数值不动）；
//   - 中间属性（`IncomingDamage` 之类）与多段结算。
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayEffectTypes.h"
#include "GAS/ACBattleAttributeSet.h"
#include "Combat/ACCombatResolver.h"
#include "ACDamageExecution.generated.h"

/**
 * 伤害 Execution。
 *
 * 装配方式（阶段 3/4）：给伤害 GE 的 `Executions` 数组加一项本类，
 * 并用 `SetByCaller`（键名 `Data.Damage`）给出原始量（§4.2：`ApplyDamage` → GE Modifier + 本类调 `FCombatResolver`）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACDamageExecution : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()

public:
    UACDamageExecution();

    /**
     * 执行入口。
     *
     * ⚠️ 这是 `UFUNCTION(BlueprintNativeEvent)` 生成的 `_Implementation`
     * （引擎声明见 GameplayEffectExecutionCalculation.h:308-314：
     * 「Native subclasses should override the auto-generated Execute_Implementation function and NOT this one」）。
     * 签名必须与生成版本**逐字一致，包括末尾的 `const`** ——
     * UHT 会把原函数的 cv 限定符带进 `_Implementation` 声明
     * （引擎实例：`AnimNotify.generated.h:34` 的 `virtual float GetDefaultTriggerWeightThreshold_Implementation() const`）。
     */
    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                        FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;

private:
    /**
     * 属性捕获定义。
     *
     * 用 GAS 的常见写法：一个 helper struct 持有一组 `FGameplayEffectAttributeCaptureDefinition`，
     * 在构造函数里填进 `RelevantAttributesToCapture`（本类基类 `UGameplayEffectCalculation` 的 protected 成员，
     * GameplayEffectCalculation.h:27-28）。
     * 为什么不直接在构造函数里逐个 `FindFieldChecked`：定义要么集中在一处（本结构），
     * 要么就散在二十行里，散着写没有人能一眼看出"这次执行到底捕获了哪几个属性"。
     *
     * 捕获这几个的理由（阶段 2 的真实计算公式会用到）：
     *   `MaxHealth`  —— 百分比类减伤 / 处决阈值要按最大生命算；
     *   `Defense`    —— 物理减免；
     *   `Resistance` —— 技术减免；
     *   `Attack`     —— 攻击力参与原始量（阶段 2 裁决是"GE Modifier 给量、还是这里乘 Attack"）。
     * 快照与否一律 `false`（`bSnapshot = false`）：沿用"结算时读当前值"的口径，
     * 与现有 `FCombatResolver` 即时读属性一致；快照会让"同一次结算里属性的后续变化"不可见，
     * 那是阶段 2 的数值裁决，本阶段不预先决定。
     */
    struct FACDamageStatics
    {
        DECLARE_ATTRIBUTE_CAPTUREDEF(MaxHealth);
        DECLARE_ATTRIBUTE_CAPTUREDEF(Defense);
        DECLARE_ATTRIBUTE_CAPTUREDEF(Resistance);
        DECLARE_ATTRIBUTE_CAPTUREDEF(Attack);

        FACDamageStatics();
    };

    /** 捕获定义的访问点（构造函数里初始化一次）。 */
    static const FACDamageStatics& DamageStatics();
};
