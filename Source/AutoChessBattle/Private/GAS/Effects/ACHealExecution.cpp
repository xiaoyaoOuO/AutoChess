// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 / §4.6 / §7 阶段 3.1）：治疗 Execution 实现。
#include "GAS/Effects/ACHealExecution.h"

#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
// 取 `SourceASC` / `TargetASC` 的 OwnerActor 的 `UnitId`（§5.1 的整型句柄口径）与所属战斗。
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "GAS/ACGameplayEffectContext.h"
// SetByCaller 键名 `Data.Heal` 的唯一定义处（`UACGE_InstantHeal::GetHealDataName()`）。
// 为什么向 GE 头文件取名字而不是在这里写一遍字符串：键名写两遍必然有一天只改一处，
// 而症状是"治疗静默变成 0"（`WarnIfNotFound=false` 连告警都没有）。
#include "GAS/Effects/ACGE_DamageHeal.h"
// 读 `FixedHealAmount`（`Spec.Def` → `UACGameplayEffectBase`）。依赖方向单向：执行体 → 基类头。
#include "GAS/Effects/ACGameplayEffectBase.h"

// ---------------------------------------------------------------------------
// 属性捕获定义
// ---------------------------------------------------------------------------
// 宏展开见 GameplayEffectExecutionCalculation.h:323-331：
//   DECLARE_ATTRIBUTE_CAPTUREDEF(X) → `FProperty* X##Property; FGameplayEffectAttributeCaptureDefinition X##Def;`
//   DEFINE_ATTRIBUTE_CAPTUREDEF(S,P,T,B) → `FindFieldChecked` 找属性 + 按 Source/Target + 快照开关构造定义。
// 用引擎宏而不是手写：`FindFieldChecked` 用的是 `GET_MEMBER_NAME_CHECKED(S, P)`，属性改名时编译器立刻报错。
UACHealExecution::FACHealStatics::FACHealStatics()
{
    DEFINE_ATTRIBUTE_CAPTUREDEF(UACBattleAttributeSet, MaxHealth, Target, false);
}

const UACHealExecution::FACHealStatics& UACHealExecution::HealStatics()
{
    // 函数内静态：捕获定义内部存的是 `FGameplayAttribute`（TFieldPath，纯反射句柄、不依赖实例），
    // 因此做成静态是安全的，也避免每次 Execute 都重新 `FindFieldChecked`。
    static const FACHealStatics Statics;
    return Statics;
}

UACHealExecution::UACHealExecution()
{
    // **必须在构造函数里**注册捕获定义：GAS 在 `UGameplayEffect::PostInitProperties` / 资产加载时
    // 就会读这份列表（`FGameplayEffectExecutionDefinition::GetAttributeCaptureDefinitions`），
    // 跑到 Execute 里再加就晚了（那时聚合器已经建好）。
    RelevantAttributesToCapture.Add(HealStatics().MaxHealthDef);
}

// ---------------------------------------------------------------------------
// 执行入口
// ---------------------------------------------------------------------------

void UACHealExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                              FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
    // `OutExecutionOutput` 全程一个字都不写：治疗由 `FCombatResolver::ApplyHeal` 直写 `Health`
    // （§4.6 契约），不走 `AddOutputModifier`（那会绕开管线，理由与 `UACDamageExecution.cpp:64-74` 相同）。
    UAbilitySystemComponent* const TargetASC = ExecutionParams.GetTargetAbilitySystemComponent();
    const UACBattleAttributeSet* const TargetAttributes =
        (TargetASC != nullptr) ? TargetASC->GetSet<UACBattleAttributeSet>() : nullptr;
    if (TargetAttributes == nullptr)
    {
        // 没有属性集就没有"承伤/承疗单位"，这次执行无事可做（与伤害 Execution 的早退口径一致）。
        return;
    }

    const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();

    // `GetPassedInTags()` 是执行期中**可用**的统一 tag 容器；它是 GAS 的真实入口，
    // while `FGameplayEffectSpec` 本身没有 `AggregatedSourceTags / AggregatedTargetTags` 成员
    //（5.6 里这两个字段只在 `FGameplayCueParameters` / `FGameplayEffectSpecForRPC` 上）。
    FAggregatorEvaluateParameters EvaluationParameters;
    const FGameplayTagContainer& PassedInTags = ExecutionParams.GetPassedInTags();
    EvaluationParameters.SourceTags = &PassedInTags;
    EvaluationParameters.TargetTags = &PassedInTags;

    float CapturedMaxHealth = 0.f;
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(HealStatics().MaxHealthDef, EvaluationParameters, CapturedMaxHealth);

    // 自定义 context：伤害类型/来源与源/目标单位句柄（§5.1：战斗内瞬时数据用整型 FUnitId）。
    const FACGameplayEffectContext* const Context = ACGameplayEffectContext::FromHandleConst(Spec.GetContext());

    FHealRequest Request;
    Request.Source = (Context != nullptr) ? Context->SourceUnitId : InvalidUnitId;
    Request.Target = (Context != nullptr) ? Context->TargetUnitId : InvalidUnitId;

    // context 没带句柄时退回 ASC 的 OwnerActor（单位在 BeginPlay 里 `InitAbilityActorInfo(this, this)`，
    // 因此 OwnerActor 就是单位本身）。口径与 `UACDamageExecution.cpp:130-143` 逐字一致 ——
    // "所有治疗/伤害都必须能自愈地找到施受双方"，否则内容侧漏填 context 就会静默丢失效果。
    if (Request.Source == InvalidUnitId)
    {
        if (const UAbilitySystemComponent* const SourceASC = ExecutionParams.GetSourceAbilitySystemComponent())
        {
            if (const AACBattleUnitBase* const SourceUnit = Cast<AACBattleUnitBase>(SourceASC->GetOwnerActor()))
            {
                Request.Source = SourceUnit->GetUnitId();
            }
        }
    }
    if (Request.Target == InvalidUnitId)
    {
        if (const AACBattleUnitBase* const TargetUnit = Cast<AACBattleUnitBase>(TargetASC->GetOwnerActor()))
        {
            Request.Target = TargetUnit->GetUnitId();
        }
    }

    Request.SourceEffectBlockId = (Context != nullptr) ? Context->SourceEffectBlockId : NAME_None;

    // `WarnIfNotFound = false` + `DefaultIfNotFound = 0`：没配 SetByCaller 是内容配置问题，
    // 但引擎默认告警会每次执行刷屏；这里静默取 0，再由下面的"固定量兜底 / 0 治疗直接返回"体现。
    Request.RawAmount = Spec.GetSetByCallerMagnitude(UACGE_InstantHeal::GetHealDataName(),
                                                     /*WarnIfNotFound=*/false, /*DefaultIfNotFound=*/0.f);

    // 固定量兜底：内容把治疗量写死在 GE 上时（`UACGE_HealFixed` 的子类，例如嗜血的 8 点），
    // 施加方不需要每次填 SetByCaller。读取走 **GE 类上的 `FixedHealAmount`**：
    // 执行体拿不到 GE 实例，但 `Spec.Def` 就是 GE 的类对象（CDO）。
    // ⚠️ 为什么不用 `Modifiers[0]`/`Modifier`：对 Instant GE 引擎会把每条 `Modifier` 立即落到属性上
    //    （GameplayEffect.cpp:3933 `ApplyModToAttribute`），而治疗已经由 `FCombatResolver::ApplyHeal`
    //    落库（`AddCurrentHP`）—— 两边都写就是**双倍治疗**。详见基类头文件的说明。
    if (Request.RawAmount <= 0.f)
    {
        if (const UACGameplayEffectBase* const EffectCDO = Cast<UACGameplayEffectBase>(Spec.Def))
        {
            Request.RawAmount = EffectCDO->FixedHealAmount;
        }
    }

    // 取不到量（或配成 0/负数）时不进管线：`ApplyHeal` 拿 0 也会跑完整条流程并记一条
    // Applied=0 的日志，那会污染基线比对（§8.2 日志对照要求事件数量不变）。
    if (Request.RawAmount <= 0.f)
    {
        UE_LOG(LogTemp, Verbose,
               TEXT("[Battle][GAS] UACHealExecution: SetByCaller `Data.Heal` 为 %.3f（<= 0），跳过治疗。"),
               Request.RawAmount);
        return;
    }

    // 取 World：**不能用 `Cast<UBattleWorld>(Actor->GetOuter())`** ——
    // `UWorld::SpawnActor` 把单位 Actor 的 Outer 设成 `ULevel`，那个 Cast 恒为 nullptr
    // （会让治疗静默不落地）。统一走 `UBattleWorld::FindFromActor`。
    // 从 Target 取最稳（无来源治疗也成立）。
    UBattleWorld* World = UBattleWorld::FindFromActor(TargetASC->GetOwnerActor());
    if (World == nullptr)
    {
        UE_LOG(LogTemp, Verbose,
               TEXT("[Battle][GAS] UACHealExecution: 找不到目标单位所属的 UBattleWorld，跳过治疗。"));
        return;
    }

    const FHealResult Result = World->Combat().ApplyHeal(Request);

    UE_LOG(LogTemp, VeryVerbose,
           TEXT("[Battle][GAS] UACHealExecution: Source=%d Target=%d 申请=%.3f 实际=%.3f 溢出=%.3f（捕获 MaxHP=%.3f）"),
           Request.Source, Request.Target, Request.RawAmount, Result.Applied, Result.Overheal, CapturedMaxHealth);
}
