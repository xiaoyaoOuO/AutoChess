// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 的 `SummonUnit` 行 / §7 阶段 3.1）：
// 召唤 EffectComponent 实现。
#include "GAS/Components/ACSummonEffectComponent.h"

#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "GAS/ACGameplayEffectContext.h"

void UACSummonEffectComponent::OnGameplayEffectApplied(FActiveGameplayEffectsContainer& ActiveGEContainer,
                                                       FGameplayEffectSpec& GESpec,
                                                       FPredictionKey& PredictionKey) const
{
    // `ActiveGEContainer` / `PredictionKey` 本组件不用：单机项目没有预测，
    // 而"谁被打上了这个 GE"由 spec 的 context / 目标 ASC 给出（下面取）。
    // 显式 `(void)` 是为了让"确实不用"这件事在代码里可见，而不是被读成漏用。
    (void)ActiveGEContainer;
    (void)PredictionKey;

    if (SummonSpecId.IsNone())
    {
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] UACSummonEffectComponent: SummonSpecId 为空，跳过召唤（GE=%s）。"),
               *GetNameSafe(GetOwner()));
        return;
    }

    // -----------------------------------------------------------------------
    // 取"施加者"（= 召唤物的 owner）与所属战斗
    // -----------------------------------------------------------------------
    // 旧口径：`SpawnSummonFromSpec(Action.SubId, Context.Source)` —— owner 是**效果上下文的 Source**
    // （ACEffectSystem.cpp:749-754），也就是"施加这个效果的单位"。
    // 效果的 instigator 就存在 context 里：`FGameplayEffectContext::AddInstigator` 会缓存
    // `InstigatorAbilitySystemComponent`（GameplayEffectTypes.cpp:177-188），
    // 读取入口是 `GetInstigatorAbilitySystemComponent()`（GameplayEffectTypes.h:302-305 / 602-606）；
    // instigator Actor 本身走 `GetInstigator()`（GameplayEffectTypes.h:284-287 / 562-569）。
    //
    // 取 Owner 的三级兜底：
    //   ① 本项目的 `FACGameplayEffectContext::SourceUnitId`（§5.1 的战斗内瞬时句柄，最准）；
    //   ② context 的 instigator ASC 的 OwnerActor 的 UnitId；
    //   ③ instigator Actor 本身（它就是单位 Actor，`Outer` 即战斗世界）。
    const FACGameplayEffectContext* const AcContext = ACGameplayEffectContext::FromHandleConst(GESpec.GetContext());
    const AActor* const InstigatorActor = GESpec.GetContext().GetInstigator();

    FUnitId OwnerUnitId = (AcContext != nullptr) ? AcContext->SourceUnitId : InvalidUnitId;

    if (OwnerUnitId == InvalidUnitId)
    {
        if (const UAbilitySystemComponent* const InstigatorASC = GESpec.GetContext().GetInstigatorAbilitySystemComponent())
        {
            if (const AACBattleUnitBase* const InstigatorUnit = Cast<AACBattleUnitBase>(InstigatorASC->GetOwnerActor()))
            {
                OwnerUnitId = InstigatorUnit->GetUnitId();
            }
        }
    }

    // ⚠️ **不能用 `Cast<UBattleWorld>(Actor->GetOuter())`**：`UWorld::SpawnActor` 把单位 Actor 的
    // Outer 设成 `ULevel`，那个 Cast 恒为 nullptr（会让召唤静默不生成）。
    // 统一走 `UBattleWorld::FindFromActor`（Actor → UWorld → GameInstance → Subsystem → Session → World）。
    // ① 优先从 instigator Actor 直接取（单位在 `BeginPlay` 里 `InitAbilityActorInfo(this, this)`，
    //    因此 instigator 通常就是施法单位本人）；
    // ② 退化时从它的 ASC 的 OwnerActor 取。
    UBattleWorld* World = UBattleWorld::FindFromActor(InstigatorActor);
    if (World == nullptr)
    {
        if (const UAbilitySystemComponent* const InstigatorASC = GESpec.GetContext().GetInstigatorAbilitySystemComponent())
        {
            World = UBattleWorld::FindFromActor(InstigatorASC->GetOwnerActor());
        }
    }

    if (World == nullptr)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] UACSummonEffectComponent: 找不到所属的 UBattleWorld，跳过召唤 %s。"),
               *SummonSpecId.ToString());
        return;
    }

    // -----------------------------------------------------------------------
    // 召唤（旧的 `SummonUnit` 动作 → 本组件的唯一落点）
    // -----------------------------------------------------------------------
    // 为什么逐个调用而不是给 World 加一个"批量召唤"入口：`SpawnSummonFromSpec` 内部已经处理了
    // 出生位置、`MaxAlivePerOwner` 上限、召唤物时长等既有规则（M06），
    // 批量入口会把这些规则再实现一遍 —— 那就是第二份真相。
    const int32 Count = FMath::Max(1, SummonCount);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        World->SpawnSummonFromSpec(SummonSpecId, OwnerUnitId);
    }

    UE_LOG(LogTemp, Verbose,
           TEXT("[Battle][GAS] UACSummonEffectComponent: Owner=%d 召唤 %s × %d。"),
           OwnerUnitId, *SummonSpecId.ToString(), Count);
}
