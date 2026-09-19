// 阶段 1 新增，阶段 2 接通（GAS 重构实施方案 §4.6 / §7 阶段 1–2）：伤害 Execution 实现。
//
// 阶段 2 起本类是**唯一被允许触发结算管线**的入口：它把 GE 执行上下文解析成 `FDamageRequest`
// 并交给 `UBattleWorld::Combat().ApplyDamage(...)`（§4.6 新契约）。
// 契约与边界见头文件。
#include "GAS/ACDamageExecution.h"

#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
// `AACBattleUnitBase`：取 `SourceASC` / `TargetASC` 的 OwnerActor 的 `UnitId`（§5.1 的整型句柄口径）。
// 这里只用到 `GetUnitId()`，不依赖单位的任何战斗逻辑。
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "GAS/ACGameplayEffectContext.h"

// ---------------------------------------------------------------------------
// 属性捕获定义
// ---------------------------------------------------------------------------

// 宏展开（GameplayEffectExecutionCalculation.h:323-331）：
//   DECLARE_ATTRIBUTE_CAPTUREDEF(X) → `FProperty* X##Property; FGameplayEffectAttributeCaptureDefinition X##Def;`
//   DEFINE_ATTRIBUTE_CAPTUREDEF(S,P,T,B) → 用 `FindFieldChecked` 找到属性、按 Source/Target + 快照开关构造定义。
// 这里用引擎的宏而不是手写：手写要重复四遍"找属性 + 构造定义"，而且 `FindFieldChecked` 用的是
// `GET_MEMBER_NAME_CHECKED(S, P)` —— 属性改名时编译器会立刻发现。
UACDamageExecution::FACDamageStatics::FACDamageStatics()
{
    DEFINE_ATTRIBUTE_CAPTUREDEF(UACBattleAttributeSet, MaxHealth, Target, false);
    DEFINE_ATTRIBUTE_CAPTUREDEF(UACBattleAttributeSet, Defense, Target, false);
    DEFINE_ATTRIBUTE_CAPTUREDEF(UACBattleAttributeSet, Resistance, Target, false);
    DEFINE_ATTRIBUTE_CAPTUREDEF(UACBattleAttributeSet, Attack, Source, false);
}

const UACDamageExecution::FACDamageStatics& UACDamageExecution::DamageStatics()
{
    // 函数内静态：只在第一次执行时构造一次。
    // 捕获定义内部存的是 `FGameplayAttribute`（TFieldPath），是纯反射句柄、不依赖任何 UObject 实例，
    // 因此做成静态是安全的，也避免了每次 Execute 都重新 `FindFieldChecked` 一遍。
    static const FACDamageStatics Statics;
    return Statics;
}

UACDamageExecution::UACDamageExecution()
{
    // 把捕获定义注册进 `RelevantAttributesToCapture`（`UGameplayEffectCalculation` 的 protected 成员，
    // GameplayEffectCalculation.h:27-28）。**必须在构造函数里做**：
    // GAS 在 `UGameplayEffect::PostInitProperties` / 资产加载时就会读这份列表，
    // 跑到 Execute 里再加就晚了（那时聚合器已经建好）。
    const FACDamageStatics& Statics = DamageStatics();

    RelevantAttributesToCapture.Add(Statics.MaxHealthDef);
    RelevantAttributesToCapture.Add(Statics.DefenseDef);
    RelevantAttributesToCapture.Add(Statics.ResistanceDef);
    RelevantAttributesToCapture.Add(Statics.AttackDef);
}

// ---------------------------------------------------------------------------
// 执行入口（阶段 2：组请求 + 交给 FCombatResolver）
// ---------------------------------------------------------------------------

void UACDamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                                FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
    // ⚠️ `OutExecutionOutput` 全程**一个字都不写**（本阶段不引入任何中间属性）。
    //
    // 取舍说明（任务 5 第 5 条）：伤害不走 GE Modifier。
    //   ① 契约要求"只有 `FCombatResolver` 能改 `Health`"，而 `AddOutputModifier` 会直接把修饰结果
    //      落到属性上（引擎在 Execution 返回后立刻应用），那条路会**绕开**结算管线，
    //      日志里就会出现"血量变了但没有对应伤害记录"；
    //   ② §4.2 建议的中间属性（`IncomingDamage` 之类）是"先记量、再由管线消费"的写法，
    //      属于阶段 3/4 的选择；本阶段的目标是"数值等价 + 读权威迁移"，引入中间属性会多出
    //      一条与 `FCombatResolver` 并行的口径，反而更难证明等价。
    //   因此：**原始量从 GE 的 SetByCaller 直接取，伤害由 `FCombatResolver` 直写 `Health`**。
    //   等阶段 3/4 需要"多段计算 + 中间属性"时再改，届时这套接口（ExecutionParams → Request）不用动。
    UAbilitySystemComponent* const SourceASC = ExecutionParams.GetSourceAbilitySystemComponent();
    UAbilitySystemComponent* const TargetASC = ExecutionParams.GetTargetAbilitySystemComponent();

    // `GetSet<T>()` 在 ASC 没有该属性集时返回 nullptr（AbilitySystemComponent.h:127-131），
    // 不 check 崩溃：编辑器里手工试放 GE、或将来某个单位没有属性集，都是"这次执行无事可做"，
    // 不是程序错误。返回 nullptr 的判断在这里就必须挡住，不能留到后面解引用。
    //
    // 阶段 2 只判**目标**有没有属性集：伤害公式仍完全在 `FCombatResolver` 里（C6/D10 数值不动），
    // 本阶段不从源属性集读任何东西（连"源有没有属性集"也不影响结算 —— 无来源伤害是合法场景）。
    // 阶段 3/4 的公式（攻击力参与原始量等）会重新需要源侧的属性集，届时再加回来。
    const UACBattleAttributeSet* const TargetAttributes =
        (TargetASC != nullptr) ? TargetASC->GetSet<UACBattleAttributeSet>() : nullptr;
    if (TargetAttributes == nullptr)
    {
        return;
    }

    const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();

    // 聚合标签评估参数：GAS 标准写法（`AggregatedSourceTags` / `AggregatedTargetTags` 的声明见
    // GameplayEffect.h:1279 / 1282，`FAggregatorEvaluateParameters` 见 GameplayEffectAggregator.h:15-38）。
    FAggregatorEvaluateParameters EvaluationParameters;

    // 快照一遍被捕获的属性（阶段 1 就落地，阶段 2 保留）。
    // 为什么留着：① 让捕获定义的"存在"变成可验证的（否则只是构造里加进数组）；
    // ② 阶段 3/4 的真实公式（减免 / 处决阈值 / 攻击力参与原始量）要用的就是这几个数；
    // ③ `AttemptCalculateCapturedAttributeMagnitude` 在"这个 GE 没有捕获该属性"时返回 false，
    // 在这里把失败显式记下来，比将来在一堆计算里发现拿到 0 要好查得多。
    // ⚠️ 本阶段它们**不参与**伤害公式：伤害数值仍完全由 `FCombatResolver` 决定（C6/D10 数值不动）。
    float CapturedMaxHealth = 0.f;
    float CapturedDefense = 0.f;
    float CapturedResistance = 0.f;
    float CapturedAttack = 0.f;
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().MaxHealthDef, EvaluationParameters, CapturedMaxHealth);
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().DefenseDef, EvaluationParameters, CapturedDefense);
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ResistanceDef, EvaluationParameters, CapturedResistance);
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().AttackDef, EvaluationParameters, CapturedAttack);

    // 自定义 context：伤害类型 / 伤害来源 / 来源效果块 / 源与目标单位句柄（§5.1：战斗内瞬时数据用整型 FUnitId）。
    // 走 `ACGameplayEffectContext::FromHandleConst` 而不是直接 `static_cast`：
    // 这个 GE 的 context 可能是普通 `FGameplayEffectContext`（例如阶段 3 的某个状态 GE 顺手挂了这个 Execution），
    // 类型不匹配时返回 nullptr，下面的默认值就是"没给口径"的降级结果。
    const FACGameplayEffectContext* const DamageContext = ACGameplayEffectContext::FromHandleConst(Spec.GetContext());

    FDamageRequest Request;
    Request.Source = (DamageContext != nullptr) ? DamageContext->SourceUnitId : InvalidUnitId;
    Request.Target = (DamageContext != nullptr) ? DamageContext->TargetUnitId : InvalidUnitId;

    // §5.1 裁决：`FDamageRequest::Source/Target` 是**战斗内瞬时结构**，用整型 `FUnitId`。
    // 但 context 可能没带（`InvalidUnitId`），这时退回 ASC 的 Owner Actor ——
    // 单位 Actor 在 `BeginPlay` 里做的正是 `InitAbilityActorInfo(this, this)`（§2.2），
    // 因此 OwnerActor 就是单位本身。凡是内容侧忘了填 context 的场景，这里都能自愈，
    // 而不是把伤害"送给 InvalidUnitId"从而被结算器静默丢弃。
    if (Request.Source == InvalidUnitId && SourceASC != nullptr)
    {
        if (const AACBattleUnitBase* const SourceUnit = Cast<AACBattleUnitBase>(SourceASC->GetOwnerActor()))
        {
            Request.Source = SourceUnit->GetUnitId();
        }
    }
    if (Request.Target == InvalidUnitId && TargetASC != nullptr)
    {
        if (const AACBattleUnitBase* const TargetUnit = Cast<AACBattleUnitBase>(TargetASC->GetOwnerActor()))
        {
            Request.Target = TargetUnit->GetUnitId();
        }
    }

    Request.DamageType = (DamageContext != nullptr) ? DamageContext->DamageType : EACDamageType::Physical;
    Request.Reason = (DamageContext != nullptr) ? DamageContext->DamageReason : EACDamageReason::Skill;
    Request.SourceEffectBlockId = (DamageContext != nullptr)
        ? DamageContext->SourceEffectBlockId
        : NAME_None;

    // -----------------------------------------------------------------------
    // 原始量：SetByCaller
    // -----------------------------------------------------------------------
    // **键名约定（阶段 3/4 的内容资产必须按它配）**：
    //   伤害 GE 的 `FGameplayModifierInfo` / `SetByCaller` 使用名字 `Data.Damage`
    //（与 §4.2 的 `SetByCaller` 幅度写法一致；用 FName 而不是 GameplayTag，
    //  因为引擎的 `GetSetByCallerMagnitude(FName, ...)` 重载不需要先注册标签）。
    //   约定：`Data.Damage` = **原始伤害量（未暴击、未减免）**，即旧代码里
    //   `FDamageRequest::RawAmount` 的那个值（`FAbilityExecutor` 里 `ATK * Segment.Multiplier` 的结果）。
    //   其余口径（是否可暴击、是否无视护盾、判定点）本阶段取默认值，与旧普攻路径一致：
    //   旧代码在 `ACAbilityExecutor.cpp` 里组请求时也只填了 Source/Target/RawAmount/Reason/Type。
    //
    // `WarnIfNotFound = false`：没配 SetByCaller 的 GE 是**内容配置问题**，
    // 但 GAS 的默认告警会在每次执行时刷屏；这里用 `DefaultIfNotFound = 0` 静默取 0，
    // 然后由下面的"0 伤害直接返回"来体现（既不结算也不报错）。
    const FName DamageSetByCallerName(TEXT("Data.Damage"));
    Request.RawAmount = Spec.GetSetByCallerMagnitude(DamageSetByCallerName, /*WarnIfNotFound=*/false, /*DefaultIfNotFound=*/0.f);

    // 取不到量（没配 SetByCaller、或配成了 0）时不进结算管线：
    //   - `ApplyDamage` 拿 0 也会跑完整条管线并记一条 AppliedHpLoss=0 的日志，
    //     那会污染基线比对（多出无意义的 Damage 事件）；
    //   - 这里直接返回与"GE 只做修饰、不造成伤害"的意图一致。
    // 注意：负数也在此挡掉（`GetSetByCallerMagnitude` 不校验符号，负的原始量没有物理意义；
    // 旧代码靠 `FMath::Max(0.f, Context.FloatValue)` 兜底，这里提前挡，口径一致）。
    if (Request.RawAmount <= 0.f)
    {
        UE_LOG(LogTemp, Verbose,
               TEXT("[Battle][GAS] UACDamageExecution: SetByCaller `Data.Damage` 为 %.3f（<= 0），跳过结算。"),
               Request.RawAmount);
        return;
    }

    Request.bCanCrit = true;
    Request.bIsBasicAttack = (Request.Reason == EACDamageReason::BasicAttack);
    Request.bIgnoreShield = false;
    Request.DeclaredJudgment = EACJudgmentPoint::AppliedHpLoss;

    // -----------------------------------------------------------------------
    // 交给结算管线（§4.6：只有本类能调 `FCombatResolver`）
    // -----------------------------------------------------------------------
    // 取 World：**唯一正确口径是 `UBattleWorld::FindFromActor`**。
    // ⚠️ 曾经这里写的是 `Cast<UBattleWorld>(TargetOwnerActor->GetOuter())`，那个口径是**错的**：
    //   `UBattleWorld::SpawnUnitActor` 用 `UWorld::SpawnActor` 生成单位，而 `SpawnActor` 内部是
    //   `NewObject<AActor>(LevelToSpawnIn, ...)`（Engine/Private/LevelActor.cpp），因此单位 Actor 的
    //   Outer 是 `ULevel` 而不是 `UBattleWorld` —— 那个 Cast **恒为 nullptr**，
    //   表现为"技能放了但伤害一点不落地，且没有任何报错"。
    // 正确路径：Actor → UWorld → UGameInstance → UBattleSubsystem → UBattleSession → UBattleWorld。
    // 承伤单位一定在战斗注册表里，所以从 **Target** 的 ASC 取最稳（无来源伤害也成立）。
    UBattleWorld* World = nullptr;
    if (TargetASC != nullptr)
    {
        World = UBattleWorld::FindFromActor(TargetASC->GetOwnerActor());
    }
    if (World == nullptr)
    {
        // 目标不在任何战斗里（编辑器里手工试放 GE、或单位已被销毁）：无事可做。
        // 不告警刷屏（这条路径在正常战斗里不会出现），留 Verbose 痕便于排查。
        UE_LOG(LogTemp, Verbose,
               TEXT("[Battle][GAS] UACDamageExecution: 找不到目标单位所属的 UBattleWorld，跳过结算。"));
        return;
    }

    const FDamageResult Result = World->Combat().ApplyDamage(Request);

    // 检查点留痕（§4.6 第 2 条要求把 7 个检查点写进日志缓冲）。
    // ⚠️ 阶段 2 **不新增日志记录**：`FCombatResolver::ApplyDamage` 内部已经按既有口径
    // 写了一条 `Combat/Damage` 结构化日志（ValueA = 实际扣血、ValueB = 护盾吸收、IntValue = 伤害类型），
    // 三份产物（schema `acbattle.log.v4`）的字段集合与事件数量**必须保持不变**（§8.2 日志对照）。
    // 因此检查点先走引擎日志（VeryVerbose，正式构建零开销）留痕；
    // 是否把它们提升为结构化产物字段，属于阶段 8/收尾的日志格式裁决（本阶段不擅自扩 schema）。
    UE_LOG(LogTemp, VeryVerbose,
           TEXT("[Battle][GAS] UACDamageExecution 检查点: Requested=%.3f AfterMitigation=%.3f "
                "AfterIncreaseReduction=%.3f RedirectedOut=%.3f ShieldAbsorbed=%.3f AppliedHpLoss=%.3f Overkill=%.3f "
                "（Crit=%d, Multiplier=%.3f, 取消=%d, 捕获 ATK=%.3f / MaxHP=%.3f DEF=%.3f RES=%.3f）"),
           Result.Requested, Result.AfterMitigation, Result.AfterIncreaseReduction, Result.RedirectedOut,
           Result.ShieldAbsorbed, Result.AppliedHpLoss, Result.Overkill,
           Result.bWasCrit ? 1 : 0, Result.CritMultiplier, Result.bCancelled ? 1 : 0,
           CapturedAttack, CapturedMaxHealth, CapturedDefense, CapturedResistance);

    // `ResolvedTarget` 与 `Source` 的实际落地值也留痕：context 没带单位句柄时上面做了兜底，
    // 出问题时需要区分"内容没配 context"与"兜底也没拿到"。
    UE_LOG(LogTemp, VeryVerbose,
           TEXT("[Battle][GAS] UACDamageExecution 请求落地: Source=%d Target=%d（FinalTarget=%d）RawAmount=%.3f"),
           Request.Source, Request.Target, Result.FinalTarget, Request.RawAmount);
}
