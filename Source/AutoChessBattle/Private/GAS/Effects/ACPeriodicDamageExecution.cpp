// 阶段 3.1a 新增（GAS 重构实施方案 §4.4 / §7 阶段 3.1）：周期伤害 Execution 实现。
//
// 数值口径与旧 `FAbnormalStateContainer::ApplyTickEffects`（ACAbnormalStates.cpp:293-362）
// 的逐行对应关系写在下面的注释里；**没有一处数值是本文件新造的**。
//
// ---------------------------------------------------------------- 参数从哪来（关键约定）
// 参数（每层伤害 / 是否按 MaxHP 百分比 / 伤害类型 / 是否扣层）**读自 GE 类本身**：
//   `Spec.Def` 就是 GE 的类对象（CDO），`UACGameplayEffectBase::ConfigurePeriodicDamage`
//   把五项写在了它的 UPROPERTY 上（基类头文件里有"为什么不用 SetByCaller"的说明）。
// 层数读自 `FGameplayEffectSpec::GetStackCount()`（引擎维护的权威层数，对应旧 `Instance.Stacks`）。
#include "GAS/Effects/ACPeriodicDamageExecution.h"

#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "GAS/ACGameplayEffectContext.h"
// 读周期参数（`Spec.Def` → `UACGameplayEffectBase`）。依赖方向是单向的：
// 执行体 → 基类头；状态 GE 头 → 基类头。执行体**不**依赖状态 GE 头，因此没有头文件环。
#include "GAS/Effects/ACGameplayEffectBase.h"

// ---------------------------------------------------------------------------
// 属性捕获定义
// ---------------------------------------------------------------------------

UACPeriodicDamageExecution::FACPeriodicStatics::FACPeriodicStatics()
{
    // `bSnapshot = false`：沿用"结算时读当前值"的口径（与 UACDamageExecution / UACHealExecution 一致）。
    // 流血要按"当前最大生命"算 0.5%/层（ACAbnormalStates.cpp:322-324 读的就是当时的 MaxHP）。
    DEFINE_ATTRIBUTE_CAPTUREDEF(UACBattleAttributeSet, MaxHealth, Target, false);
}

const UACPeriodicDamageExecution::FACPeriodicStatics& UACPeriodicDamageExecution::PeriodicStatics()
{
    static const FACPeriodicStatics Statics;
    return Statics;
}

UACPeriodicDamageExecution::UACPeriodicDamageExecution()
{
    // **必须在构造函数里**注册捕获定义（同 UACDamageExecution.cpp:43-55 的理由：
    // 引擎在 GE 加载 / PostInitProperties 时就会读这份列表）。
    RelevantAttributesToCapture.Add(PeriodicStatics().MaxHealthDef);
}

// ---------------------------------------------------------------------------
// 执行入口
// ---------------------------------------------------------------------------

void UACPeriodicDamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                                        FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
    // 一个字都不写 `OutExecutionOutput`：伤害由 `FCombatResolver::ApplyDamage` 直写 `Health`
    // （§4.6 契约：只有它能改 `Health`）。理由与 `UACDamageExecution.cpp:64-74` 相同。
    UAbilitySystemComponent* const TargetASC = ExecutionParams.GetTargetAbilitySystemComponent();
    const UACBattleAttributeSet* const TargetAttributes =
        (TargetASC != nullptr) ? TargetASC->GetSet<UACBattleAttributeSet>() : nullptr;
    if (TargetAttributes == nullptr)
    {
        return;
    }

    // ⚠️ `GetOwningSpec()` 返回 **const 引用**，但周期结算需要把层数改小（旧口径 `bConsumeStackOnTick`）。
    // 引擎为此提供了 `GetOwningSpecForPreExecuteMod()`（GameplayEffectExecutionCalculation.h:36）：
    // 「Non const access. Be careful with this, especially when modifying a spec after attribute capture.」
    // 本类只**读**捕获的 `MaxHealth`（用来算伤害），不在捕获后改任何被捕获的量，因此这里改层数是安全的。
    //
    // 为什么改 spec 上的层数就等价于改活动 GE 的层数：周期结算拿到的是**活动实例自己的 spec**
    // （GameplayEffect.cpp:4486 `ExecuteActiveEffectsFrom(ActiveEffect.Spec)` → 3076
    //  `FGameplayEffectSpec& SpecToUse = Spec;`，是一条引用链，不是拷贝）。
    FGameplayEffectSpec* const MutableSpec = ExecutionParams.GetOwningSpecForPreExecuteMod();
    if (MutableSpec == nullptr || MutableSpec->Def == nullptr)
    {
        return;
    }

    // 周期参数（每层伤害 / 百分比开关 / 伤害类型 / 是否扣层）读自 GE 类本身。
    const UACGameplayEffectBase* const EffectCDO = Cast<UACGameplayEffectBase>(MutableSpec->Def);
    if (EffectCDO == nullptr)
    {
        // 挂了这个 Execution 的 GE 却不是 `UACGameplayEffectBase` 派生（内容配错）。
        // 不崩溃、留痕：这类配置错误在编辑器里手工试放 GE 时最容易出现。
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] UACPeriodicDamageExecution: GE %s 不是 UACGameplayEffectBase 派生，"
                    "读不到周期参数，跳过。"),
               *GetNameSafe(MutableSpec->Def));
        return;
    }

    const int32 StacksAtTick = MutableSpec->GetStackCount();
    if (StacksAtTick <= 0)
    {
        // 层数已经空了：旧容器在 `Stacks <= 0` 时把实例整个摘掉（ACAbnormalStates.cpp:369-376），
        // 因此这里"什么都不结算"是等价行为。
        // ⚠️ 引擎**不会**在层数归零时自动移除活动 GE（已核实：全库没有 `GetStackCount() <= 0` 的清理点），
        //    因此"层数到 0 → 移除 GE"这一步留给阶段 3.3 的接线（本阶段只产出、不接线）。
        return;
    }

    // ---------------------------------------------------------------
    // 目标 / 来源句柄（伤害与"层数消耗"都要用，故在伤害之前先算出来）
    // ---------------------------------------------------------------
    const FACGameplayEffectContext* const Context = ACGameplayEffectContext::FromHandleConst(MutableSpec->GetContext());

    FDamageRequest Request;
    Request.Source = (Context != nullptr) ? Context->SourceUnitId : InvalidUnitId;
    Request.Target = (Context != nullptr) ? Context->TargetUnitId : InvalidUnitId;

    // context 没带句柄时退回 ASC 的 OwnerActor（口径与 ACDamageExecution.cpp:130-143 一致）。
    // 周期伤害"源"的语义：旧容器记的是 `Instance.Source`（施加状态的那个单位）——
    // 走 context 时它就是施加方填进去的值；拿不到时退回 ASC（SourceASC 常为空），
    // 与旧代码"状态源已死 → FindUnit 拿不到 → 记 InvalidUnitId"的降级同向。
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

    // ---------------------------------------------------------------
    // 周期伤害（`bEnablePeriodicDamage == false` 的状态跳过这一段，但仍然要执行下面的扣层）
    // ---------------------------------------------------------------
    // ⚠️ 这里**不能 return**：没有周期伤害的状态（伤口）仍然可能"每秒 -1 层"
    // （旧 `bConsumeStackOnTick = true`，见 ACBattleContentDefinitions.cpp:886-891 的注释）。
    // 旧代码里伤害段被 `if (Definition->bPeriodicDamage && StacksAtTick > 0)` 包住，
    // 而扣层段在它内部 —— 因此旧实现里"无伤害状态不会掉层"是个副产品；
    // 本实现按 `bConsumeStackOnTick` 独立判定（更符合内容定义的意图），差异见报告。
    if (EffectCDO->bEnablePeriodicDamage)
    {
        FAggregatorEvaluateParameters EvaluationParameters;
        const FGameplayTagContainer& PassedInTags = ExecutionParams.GetPassedInTags();
        EvaluationParameters.SourceTags = &PassedInTags;
        EvaluationParameters.TargetTags = &PassedInTags;

        float CapturedMaxHealth = 0.f;
        ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(PeriodicStatics().MaxHealthDef, EvaluationParameters, CapturedMaxHealth);

        // 逐行对应 ACAbnormalStates.cpp:322-324：
        //   bPercentOfMaxHP ? (MaxHP * DamagePerStack / 100) : DamagePerStack
        const float PerStack = EffectCDO->bPeriodicPercentOfMaxHP
            ? (CapturedMaxHealth * EffectCDO->PeriodicDamagePerStack / 100.f)
            : EffectCDO->PeriodicDamagePerStack;

        Request.DamageType = EffectCDO->PeriodicDamageType;
        Request.Reason = EACDamageReason::Dot;                            // 旧口径：ACAbnormalStates.cpp:330
        Request.RawAmount = PerStack * static_cast<float>(StacksAtTick);  // 旧口径：ACAbnormalStates.cpp:331
        Request.bCanCrit = false;                                         // 旧口径：ACAbnormalStates.cpp:332
        Request.bIsBasicAttack = false;
        Request.bIgnoreShield = false;
        Request.DeclaredJudgment = EACJudgmentPoint::AppliedHpLoss;
        Request.SourceEffectBlockId = GetPeriodicSourceBlockId();         // 旧口径：ACAbnormalStates.cpp:333

        UBattleWorld* World = nullptr;
        if (const AActor* const TargetOwnerActor = TargetASC->GetOwnerActor())
        {
            World = UBattleWorld::FindFromActor(TargetOwnerActor);
        }
        if (World == nullptr)
        {
            UE_LOG(LogTemp, Verbose,
                   TEXT("[Battle][GAS] UACPeriodicDamageExecution: 找不到目标单位所属的 UBattleWorld，跳过周期伤害。"));
        }
        else
        {
            const FDamageResult Result = World->Combat().ApplyDamage(Request);

            UE_LOG(LogTemp, VeryVerbose,
                   TEXT("[Battle][GAS] UACPeriodicDamageExecution: Source=%d Target=%d 层数=%d 每层=%.4f（%%MaxHP=%d）"
                        " 原始=%.4f 实际扣血=%.4f 护盾吸收=%.4f"),
                   Request.Source, Request.Target, StacksAtTick, PerStack,
                   EffectCDO->bPeriodicPercentOfMaxHP ? 1 : 0,
                   Request.RawAmount, Result.AppliedHpLoss, Result.ShieldAbsorbed);
        }
    }

    // ---------------------------------------------------------------
    // 层数消耗（旧 `bConsumeStackOnTick`）
    // ---------------------------------------------------------------
    if (EffectCDO->bPeriodicConsumeStackOnTick)
    {
        // 旧口径：结算后 -1 层（ACAbnormalStates.cpp:336-343）。
        // `MarkStackCountHandledManually()` 告诉引擎"层数我已自己处理"；它本来只影响
        // "输出修饰是否再乘层数"（GameplayEffect.cpp:3008 / 3145），本类不输出修饰，
        // 写上是为了语义明确、避免将来有人加输出修饰时被静默再乘一遍。
        OutExecutionOutput.MarkStackCountHandledManually();
        MutableSpec->SetStackCount(StacksAtTick - 1);
    }

    // ---------------------------------------------------------------
    // 层数归零 → **主动移除 GE**（§4.6 裁决③）
    // ---------------------------------------------------------------
    // ⚠️ 引擎**不会**在 `StackCount` 归零时自动移除活动 GE（已核实：全库没有
    //    `GetStackCount() <= 0` 的清理点，GE 的 `StackingExpirationPolicy` 只在"叠加导致的
    //    层数变化"上生效，管不到本类手动 `SetStackCount` 这条路径）。不处理的话，层数已经 0 的
    //    状态 GE 会一直留在 ASC 里，它授予的 `State.Xxx` 标签就永远挂着 ——
    //    表现为"流血层数显示 0，但单位仍被视为流血中"，而 `HasStateTag` / `GetTagCount`
    //    的查询（技能门控、索敌、条件判定）全部跟着错。
    //    旧容器的等价行为：`Stacks <= 0` 时把实例整个摘掉（ACAbnormalStates.cpp:369-376）。
    //
    // 怎么拿到活动 GE 的句柄：`FGameplayEffectSpec` **没有** `GetHandle()`
    //   （已核实 `GameplayEffect.h` 全文无此成员），Execution 也拿不到 `FActiveGameplayEffect`。
    //   因此在目标 ASC 上按"GE 类 == 正在执行的这个 Def"反查 —— 这也把"执行的是哪一个
    //   活动 GE"收敛到唯一匹配（同一 GE 类对同一目标只有一条活动记录，`AggregateByTarget`）。
    if (MutableSpec->GetStackCount() <= 0)
    {
        if (UAbilitySystemComponent* const MutableTargetASC = ExecutionParams.GetTargetAbilitySystemComponent())
        {
            const UGameplayEffect* const EffectDef = MutableSpec->Def;
            // 空查询 = 匹配全部活动 GE（`FGameplayEffectQuery` 默认全通）。
            for (const FActiveGameplayEffectHandle& Handle : MutableTargetASC->GetActiveEffects(FGameplayEffectQuery()))
            {
                const FActiveGameplayEffect* const ActiveEffect = MutableTargetASC->GetActiveGameplayEffect(Handle);
                if (ActiveEffect != nullptr && ActiveEffect->Spec.Def == EffectDef)
                {
                    MutableTargetASC->RemoveActiveGameplayEffect(Handle);
                    break;
                }
            }
        }
    }
}
