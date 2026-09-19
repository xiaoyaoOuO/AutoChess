// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 `ApplyShield` / §4.6 / §7 阶段 3.1）：护盾实现。
#include "GAS/Effects/ACGE_Shield.h"

#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "GAS/ACGameplayEffectContext.h"

// ---------------------------------------------------------------------------
// 共用的"取 World + 取施受句柄"辅助
// ---------------------------------------------------------------------------
// 为什么在两个 Execution 里各写一遍而不是抽公共基类：这段逻辑只有十余行，
// 而它必须与 `UACDamageExecution.cpp:119-206` 的既有口径**逐字一致**（那里是本项目的范本）。
// 抽基类会把"改一处口径"变成一次跨文件重构，反而更容易分叉。
namespace
{
    /**
     * 解析出本次执行涉及的战斗世界与源/目标单位句柄。
     *
     * 取 World 的路径与 `UACDamageExecution.cpp:199-206` 完全一致：单位 Actor 的 Outer 链落在
     * `UBattleWorld`，因此从 Target 的 ASC 的 OwnerActor 向上 cast 就是"这一场战斗"。
     */
    UBattleWorld* ResolveBattleWorldAndUnits(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                             FUnitId& OutSource, FUnitId& OutTarget, FName& OutSourceBlockId)
    {
        OutSource = InvalidUnitId;
        OutTarget = InvalidUnitId;
        OutSourceBlockId = NAME_None;

        UAbilitySystemComponent* const TargetASC = ExecutionParams.GetTargetAbilitySystemComponent();
        if (TargetASC == nullptr)
        {
            return nullptr;
        }

        const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
        const FACGameplayEffectContext* const Context = ACGameplayEffectContext::FromHandleConst(Spec.GetContext());

        OutSource = (Context != nullptr) ? Context->SourceUnitId : InvalidUnitId;
        OutTarget = (Context != nullptr) ? Context->TargetUnitId : InvalidUnitId;
        OutSourceBlockId = (Context != nullptr) ? Context->SourceEffectBlockId : NAME_None;

        // context 没带句柄时退回 ASC 的 OwnerActor（单位在 BeginPlay 里 `InitAbilityActorInfo(this, this)`，
        // OwnerActor 就是单位本身）。口径与 `UACDamageExecution.cpp:130-143` 一致：
        // 内容侧漏填 context 时这里能自愈，而不是把效果"送给 InvalidUnitId"从而被结算器静默丢弃。
        if (OutSource == InvalidUnitId)
        {
            if (const UAbilitySystemComponent* const SourceASC = ExecutionParams.GetSourceAbilitySystemComponent())
            {
                if (const AACBattleUnitBase* const SourceUnit = Cast<AACBattleUnitBase>(SourceASC->GetOwnerActor()))
                {
                    OutSource = SourceUnit->GetUnitId();
                }
            }
        }
        if (OutTarget == InvalidUnitId)
        {
            if (const AACBattleUnitBase* const TargetUnit = Cast<AACBattleUnitBase>(TargetASC->GetOwnerActor()))
            {
                OutTarget = TargetUnit->GetUnitId();
            }
        }

        // 单位 Actor 的 Outer 是 ULevel，不是 UBattleWorld —— 必须走 UBattleWorld::FindFromActor
        // （Cast<UBattleWorld>(Actor->GetOuter()) 恒为 nullptr，会让护盾静默不落地）。
        return UBattleWorld::FindFromActor(TargetASC->GetOwnerActor());
    }
}

// ---------------------------------------------------------------------------
// UACShieldExecution
// ---------------------------------------------------------------------------

UACShieldExecution::UACShieldExecution()
{
    // 本 Execution 不需要捕获任何属性：护盾量完全由 SetByCaller 给（见头文件的取舍说明）。
    // 因此**刻意**不往 `RelevantAttributesToCapture` 里加东西（留空是合法配置）。
}

void UACShieldExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                                FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
    // 不写 `OutExecutionOutput`：护盾由 `FCombatResolver::ApplyShield` 落库（实例数组 + 属性同步）。
    FUnitId SourceUnit = InvalidUnitId;
    FUnitId TargetUnit = InvalidUnitId;
    FName SourceBlockId;
    UBattleWorld* const World = ResolveBattleWorldAndUnits(ExecutionParams, SourceUnit, TargetUnit, SourceBlockId);
    if (World == nullptr)
    {
        UE_LOG(LogTemp, Verbose,
               TEXT("[Battle][GAS] UACShieldExecution: 找不到目标单位所属的 UBattleWorld，跳过护盾。"));
        return;
    }

    const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();

    FShieldRequest Request;
    Request.Source = SourceUnit;
    Request.Target = TargetUnit;
    Request.SourceEffectBlockId = SourceBlockId;

    // 量：优先 SetByCaller `Data.Shield`；取不到时回落到 **GE 类上的 `FixedShieldAmount`**
    // （子类用 `SetFixedShieldMagnitude` 写进去）。执行体拿不到 GE 实例，但 `Spec.Def` 就是
    // GE 的类对象（CDO），读到的正是内容里写下的值。
    Request.Amount = Spec.GetSetByCallerMagnitude(UACGE_Shield::GetShieldDataName(),
                                                  /*WarnIfNotFound=*/false, /*DefaultIfNotFound=*/0.f);
    if (Request.Amount <= 0.f)
    {
        if (const UACGameplayEffectBase* const EffectCDO = Cast<UACGameplayEffectBase>(Spec.Def))
        {
            Request.Amount = EffectCDO->FixedShieldAmount;
        }
    }

    if (Request.Amount <= 0.f)
    {
        UE_LOG(LogTemp, Verbose,
               TEXT("[Battle][GAS] UACShieldExecution: SetByCaller `Data.Shield` 与静态幅度都 <= 0（当前 %.3f），跳过护盾。"),
               Request.Amount);
        return;
    }

    // 时长：`< 0` = 持续护盾（结算器侧约定，ACCombatResolver.cpp:415）。
    // 默认给 -1 而不是 0：0 会让 `ExpireTime = Now + 0`（下一帧就被 `RemoveExpiredShields` 摘掉），
    // 那是"施加了一个立刻消失的护盾"，不是任何内容的意图。
    Request.DurationSeconds = Spec.GetSetByCallerMagnitude(UACGE_Shield::GetDurationDataName(),
                                                          /*WarnIfNotFound=*/false, /*DefaultIfNotFound=*/-1.f);

    World->Combat().ApplyShield(Request);

    UE_LOG(LogTemp, VeryVerbose,
           TEXT("[Battle][GAS] UACShieldExecution: Source=%d Target=%d 护盾量=%.3f 时长=%.3f"),
           Request.Source, Request.Target, Request.Amount, Request.DurationSeconds);
}

// ---------------------------------------------------------------------------
// UACShieldExpireExecution
// ---------------------------------------------------------------------------

UACShieldExpireExecution::UACShieldExpireExecution()
{
    // 同样不需要捕获属性。
}

void UACShieldExpireExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                                      FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
    FUnitId SourceUnit = InvalidUnitId;
    FUnitId TargetUnit = InvalidUnitId;
    FName SourceBlockId;
    UBattleWorld* const World = ResolveBattleWorldAndUnits(ExecutionParams, SourceUnit, TargetUnit, SourceBlockId);
    if (World == nullptr)
    {
        return;
    }

    // 按**来源**移除自己那一层护盾（§4.6 裁决②：每次施加 = 一个独立 GE 实例，各自持有时长）。
    //
    // ⚠️ 这里**不能**用 `Clear()`：那会把整池清空，在"同场战斗叠了不同时长护盾"的场景下
    //    把还没到期的那层一起清掉（早期实现就是这样，属已知缺陷）。`RemoveBySourceEffectBlockId`
    //    用从 `FACGameplayEffectContext` 传来的 `SourceEffectBlockId` 精确定位到本 GE 施加的那一层。
    // `GetShields()` 的非 const 重载见 ACBattleUnitBase.h。
    AACBattleUnitBase* const Target = World->FindUnit(TargetUnit);
    if (Target == nullptr)
    {
        return;
    }

    if (SourceBlockId.IsNone())
    {
        // 没有来源标识就无从定位是哪一层。**不猜**（按空来源删会误伤其它无来源护盾），
        // 也不整池清空（那是本次要修的缺陷）。留痕，让内容侧把 `SourceEffectBlockId` 填上。
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] UACShieldExpireExecution: Unit=%d 的护盾 GE 没有 SourceEffectBlockId，"
                    "无法定位要移除哪一层，已跳过（护盾会等到 RemoveExpired 按各自到期时刻清理）。"),
               TargetUnit);
        return;
    }

    Target->GetShields().RemoveBySourceEffectBlockId(SourceBlockId);

    UE_LOG(LogTemp, VeryVerbose,
           TEXT("[Battle][GAS] UACShieldExpireExecution: 移除 Unit=%d 来自 %s 的那一层护盾。"),
           TargetUnit, *SourceBlockId.ToString());
}

// ---------------------------------------------------------------------------
// UACGE_Shield
// ---------------------------------------------------------------------------

UACGE_Shield::UACGE_Shield()
{
    // 默认按"限时护盾"配（旧内容里绝大多数护盾都是限时的；持续护盾由
    // `ConfigureAsPersistentShield()` 显式切换）。
    ConfigureAsDurationShield();

    AddExecution(UACShieldExecution::StaticClass());
}

UACGE_Shield::UACGE_Shield(FContentShieldTag)
{
    // 子类专用：**什么都不做**。时长策略、量与 Execution 都由子类自己定
    // （见头文件说明：避免"父构造配一遍、子构造覆盖一遍"的混乱语义）。
}

void UACGE_Shield::ConfigureAsDurationShield()
{
    MakeHasDurationByCaller(GetDurationDataName());
}

void UACGE_Shield::ConfigureAsPersistentShield()
{
    MakeInfinite();
}

void UACGE_Shield::SetFixedShieldMagnitude(float Amount)
{
    // ⚠️ 这里**刻意不往 `Modifiers` 里加任何东西**（早期草稿曾加了一条 `Shield` 的 `Additive`，
    //    那是错的）：对 Instant GE，引擎会立即把每条 `Modifier` 落到属性上
    //    （GameplayEffect.cpp:3907-3963 的 `InternalExecuteMod` → `ApplyModToAttribute`），
    //    而 `UACShieldExecution` 已经通过 `FCombatResolver::ApplyShield` 把量写进
    //    `FShieldPool` 并同步了 `Shield` 属性 —— 两边都写就是**双倍护盾**。
    //    因此"量"只放在基类的 `FixedShieldAmount` 字段上，由 Execution 从 `Spec.Def` 读。
    FixedShieldAmount = Amount;
}

// ---------------------------------------------------------------------------
// UACGE_ShieldExpire
// ---------------------------------------------------------------------------

UACGE_ShieldExpire::UACGE_ShieldExpire()
{
    // 纯"清理"GE：Instant（基类默认），执行一次 `FShieldPool::Clear()`。
    AddExecution(UACShieldExpireExecution::StaticClass());

    // 不带任何数值、不授予任何标签：它的唯一作用是那一次 Execution。
    // `DurationPolicy = Instant` 保证它不会留在 ASC 里（瞬时 GE 不进容器）。
}
