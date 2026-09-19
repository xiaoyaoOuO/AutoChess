// 阶段 1 新增（GAS 重构实施方案 §2.1 / §3.3 / §7 阶段 1）：能力集挂载组件。
//
// 职责：把 `UACAbilitySet` 数据资产挂到单位 Actor 上，并在合适的时机把清单授予 ASC。
// 为什么是一个组件而不是单位基类上的一个字段（§0 C5 / §1 D12）：
// **基类只放共有项**。技能/被动是"干员特有内容"的入口，将来还会长出装备槽、强化等同类需求，
// 它们都应该往组件上挂，而不是往 `AACBattleUnitBase` 里继续加字段。
// 阶段 0a 已经用同一理由把网格与表现拆成了组件（`UACUnitGridComponent` / `UACUnitPresentationComponent`），
// 本组件是第三个，位置并列。
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Templates/SubclassOf.h"
#include "ACAbilitySetComponent.generated.h"

class UACAbilitySet;
class UGameplayAbility;

UCLASS(ClassGroup=(AutoChess), meta=(BlueprintSpawnableComponent))
class AUTOCHESSBATTLE_API UACAbilitySetComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UACAbilitySetComponent();

    /** 本单位的授予清单（可空 = 什么都不授予）。 */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|GAS")
    TObjectPtr<UACAbilitySet> AbilitySet = nullptr;

    /**
     * **追加授予**的能力（阶段 3.3 新增）：装备 / 强化 / 词条带来的被动能力。
     *
     * 为什么需要第二条通道（这是本阶段"内容语义保真"的关键一处）：
     *   旧 `FACEffectBlock` 把"触发条件"与"产生什么效果"写在同一个块里，因此
     *   "S 级强化·处刑"（累计击杀 ≥ 20 后**一次性** +50 暴击 / +80 攻击力）是一个块；
     *   而 GAS 把两件事拆开了 —— `UGameplayAbility::AbilityTriggers` 管触发、`UGameplayEffect` 管改值。
     *   于是"这个干员选了这个强化 / 装了这件装备"这件事必须表达成
     *   **把对应的被动能力授予它的 ASC**，否则只能退回"开局无条件施加那条 GE"，
     *   20 击杀的门槛与一次性语义会整体丢失（阶段 3.2a 的已知退化）。
     *
     * 与 `AbilitySet` 的分工（两侧内容**没有重叠**）：
     *   · `AbilitySet`（由 `DefinitionId` 在 `FACBattleDataContext::AbilitySets` 里查到）
     *     = 技能 / 普攻 / **天生**被动，例如索利瓦尔的嗜血、蠕虫母巢的开局召唤；
     *   · `ExtraAbilities`（由 `UBattleWorld` 从 `FACPlayerUnitSpec::GrantedAbilities` 注入）
     *     = Run 层**按局内进度**追加的被动，例如处刑强化、心流词条、心流刃常驻段。
     *   判据是"要不要先被授予才能在事件上触发"，不是"哪一层写的"。
     *
     * 注入时机：`UBattleWorld::Initialize` 生成单位时（`FinishSpawning` 之前），
     * 授予时机：`RegisterUnit` → `GrantToOwner`（与 `AbilitySet` 在**同一次**里授完）。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|GAS")
    TArray<TSubclassOf<UGameplayAbility>> ExtraAbilities;

    /**
     * 由内核在**单位数据填好之后、`FinishSpawning` 之前**注入本次要授予的清单
     * （`UBattleWorld` 按 `Definition->DefinitionId` 从 `FACBattleDataContext::AbilitySets` 里取）。
     *
     * 为什么需要这个 setter，而不是让内容侧直接写在定义资产上：
     *   定义类型 `UUnitDefinitionBase` 在 `AutoChessCore`，而 `UACAbilitySet` 在 `AutoChessBattle`
     *   （依赖方向 Core ⇐ Battle）。Core 里放不下 `TObjectPtr<UACAbilitySet>` 字段
     *   （跨模块 UHT 反射需要完整类型），`TSoftObjectPtr` 那条路又不可靠 ——
     *   代码侧内容用 `NewObject(GetTransientPackage())` 造对象，软引用在重载 / GC 后解析不回来。
     *   因此把"定义 → 清单"这张表放在 `FACBattleDataContext`（Battle 侧），由内容侧填充、
     *   由内核注入组件。详见阶段 3.2a 报告"依赖方向方案与依据"。
     */
    void SetAbilitySet(UACAbilitySet* InAbilitySet) { AbilitySet = InAbilitySet; }

    /**
     * 注入追加授予的能力（`FACPlayerUnitSpec::GrantedAbilities`）。
     *
     * 与 `SetAbilitySet` 一样在 `FinishSpawning` 之前调用，授予仍然只发生在 `GrantToOwner`。
     * 传空数组是常态（大多数单位没有"选了才有"的被动），此时 `ExtraAbilities` 为空、
     * `GrantToOwner` 里的追加段直接跳过。
     */
    void SetExtraAbilities(const TArray<TSubclassOf<UGameplayAbility>>& InAbilities) { ExtraAbilities = InAbilities; }

    /**
     * 把 `AbilitySet` 与 `ExtraAbilities` 一起授予 Owner 的 ASC。
     *
     * **阶段 3.2a 已落地，阶段 3.3 扩了第二条通道**（实施见 .cpp）：
     *   两份都为空 → 静默返回；
     *   `GetOwner()` → `Cast<IAbilitySystemInterface>` → `GetAbilitySystemComponent()` →
     *   `AbilitySet->GiveTo(*ASC)` + `UACAbilitySet::GiveAbilitiesTo(*ASC, ExtraAbilities, …)`。
     *
     * 调用链（`BeginPlay` 与 `UBattleWorld::RegisterUnit` 两处，后者才是真正生效的那一次）：
     *   `UWorld::SpawnActor(bDeferConstruction=true)`
     *     → `UBattleWorld` 填数据（`InitializeFromXxx` → `InitCommon` 写属性集；
     *        `SetAbilitySet` + `SetExtraAbilities`）
     *     → `FinishSpawning()` → **`AACBattleUnitBase::BeginPlay`**
     *         → `ASC->InitAbilityActorInfo(this, this)`   ← 必须先做，GiveAbility 才有 ActorInfo 可绑
     *         → `AbilitySetComponent->GrantToOwner()`     ← 本函数（此刻清单还没注入，是空转）
     *     → … `PlaceUnitOnGrid` → `UBattleWorld::RegisterUnit` → `GrantToOwner()` ← **真正授予**
     * 即"单位被注册进 UBattleWorld 之前就已经拿到能力与常驻 GE"，
     * 因此 `Hook.BattleStart` 派发时被动能力已经在 ASC 里登记好了。
     */
    void GrantToOwner() const;
};
