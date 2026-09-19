// 击杀 / 复活 / 结算产出组件实现。
#include "GAS/Components/ACKillReviveEffectComponent.h"

#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "Diagnostics/ACBattleDiagnostics.h"
#include "GAS/ACGameplayEffectContext.h"

void UACKillReviveEffectComponent::OnGameplayEffectApplied(FActiveGameplayEffectsContainer& ActiveGEContainer,
                                                           FGameplayEffectSpec& GESpec,
                                                           FPredictionKey& PredictionKey) const
{
    // 容器与预测键本组件不用（单机无预测）。显式 `(void)` 让"确实不用"可见。
    (void)ActiveGEContainer;
    (void)PredictionKey;

    // -----------------------------------------------------------------------
    // 取效果上下文：源/目标单位句柄（§5.1：战斗内瞬时数据用整型 FUnitId）
    // -----------------------------------------------------------------------
    const FACGameplayEffectContext* const AcContext = ACGameplayEffectContext::FromHandleConst(GESpec.GetContext());

    FUnitId SourceUnitId = (AcContext != nullptr) ? AcContext->SourceUnitId : InvalidUnitId;
    FUnitId TargetUnitId = (AcContext != nullptr) ? AcContext->TargetUnitId : InvalidUnitId;

    // context 没带句柄时退回 context 的 instigator ASC（口径与其它 Execution 的兜底一致）：
    // `FGameplayEffectContext::AddInstigator` 会缓存 ASC（GameplayEffectTypes.cpp:177-188）。
    const AActor* const InstigatorActor = GESpec.GetContext().GetInstigator();
    const UAbilitySystemComponent* const InstigatorASC = GESpec.GetContext().GetInstigatorAbilitySystemComponent();

    if (SourceUnitId == InvalidUnitId && InstigatorASC != nullptr)
    {
        if (const AACBattleUnitBase* const InstigatorUnit = Cast<AACBattleUnitBase>(InstigatorASC->GetOwnerActor()))
        {
            SourceUnitId = InstigatorUnit->GetUnitId();
        }
    }

    // -----------------------------------------------------------------------
    // 取 World
    // -----------------------------------------------------------------------
    // ⚠️ **不能用 `Cast<UBattleWorld>(Actor->GetOuter())`**：`UWorld::SpawnActor` 把单位 Actor 的
    // Outer 设成 `ULevel`，那个 Cast 恒为 nullptr（会让击杀/复活静默失效）。
    // 统一走 `UBattleWorld::FindFromActor`（Actor → UWorld → GameInstance → Subsystem → Session → World）。
    // 从 instigator 侧取（施法者一定在战斗里）；退化时从它的 OwnerActor 取。
    UBattleWorld* World = UBattleWorld::FindFromActor(InstigatorActor);
    if (World == nullptr && InstigatorASC != nullptr)
    {
        World = UBattleWorld::FindFromActor(InstigatorASC->GetOwnerActor());
    }

    if (World == nullptr)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] UACKillReviveEffectComponent: 找不到所属的 UBattleWorld，跳过 %d 操作。"),
               static_cast<int32>(Operation));
        return;
    }

    // -----------------------------------------------------------------------
    // 执行
    // -----------------------------------------------------------------------
    switch (Operation)
    {
    case EACKillReviveOperation::KillUnit:
    {
        // 旧口径（ACEffectSystem.cpp:756-758）：`KillUnit(PrimaryTarget, EACDamageReason::Execute)`。
        const FUnitId VictimId = bAffectTarget ? TargetUnitId : SourceUnitId;
        if (VictimId == InvalidUnitId)
        {
            UE_LOG(LogTemp, Warning,
                   TEXT("[Battle][GAS] UACKillReviveEffectComponent::KillUnit: 目标句柄为 InvalidUnitId，跳过。"));
            break;
        }
        World->KillUnit(VictimId, EACDamageReason::Execute);
        break;
    }

    case EACKillReviveOperation::ReviveUnit:
    {
        // 旧口径（ACEffectSystem.cpp:760-762）：`ReviveUnit(PrimaryTarget)`。
        const FUnitId ReviveId = bAffectTarget ? TargetUnitId : SourceUnitId;
        if (ReviveId == InvalidUnitId)
        {
            UE_LOG(LogTemp, Warning,
                   TEXT("[Battle][GAS] UACKillReviveEffectComponent::ReviveUnit: 目标句柄为 InvalidUnitId，跳过。"));
            break;
        }
        World->ReviveUnit(ReviveId);
        break;
    }

    case EACKillReviveOperation::GrantSoulCrystal:
        // 旧口径（ACEffectSystem.cpp:764-766）：`Stats().AddSoulCrystal(RoundToInt(Value))`。
        // 它是**统计产出**（不进战斗结果结构以外的任何状态），因此不依赖目标句柄。
        World->Stats().AddSoulCrystal(Amount);
        break;

    case EACKillReviveOperation::AddSearchableSecret:
        // 旧口径（ACEffectSystem.cpp:768-770）：`Stats().AddSearchableSecret(RoundToInt(Value))`。
        World->Stats().AddSearchableSecret(Amount);
        break;

    default:
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] UACKillReviveEffectComponent: 未处理的操作 %d（GE=%s）。"),
               static_cast<int32>(Operation), *GetNameSafe(GetOwner()));
        break;
    }
}
