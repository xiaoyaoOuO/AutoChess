#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "Core/ACBattleTypes.h"
#include "Events/ACBattleEventBus.h"
#include "ACBattleAbility.generated.h"

class UBattleWorld;
class AACBattleUnitBase;
class UGameplayEffect;

UCLASS(Abstract)
class AUTOCHESSBATTLE_API UACBattleAbility : public UGameplayAbility
{
    GENERATED_BODY()

public:
    UACBattleAbility();

    /**取战斗世界。*/
    UBattleWorld* GetBattleWorld() const;
    
    static UBattleWorld* FindBattleWorldFromActor(const AActor* Actor);

    /**
     * 取施法者单位：`GetAvatarActorFromActorInfo()` → `AACBattleUnitBase`；
     */
    AACBattleUnitBase* GetCasterUnit() const;

    /**
     * 解析本次施放的目标（核心实现）。
     *
     * @param Caster     施法者
     * @param OutTargets 解析结果
     * @return true  = 解析动作本身成功完成（`OutTargets` 可能为空，那是"本来就没目标"）；
     *         false = 解析根本没做（没有世界 / 施法者不在战斗注册表里）。
     *         调用方必须区分这两者：把 false 当成"没目标"会把装配错误伪装成合法结果。
     *
     * 选择器与半径来自 `GetTargetSelector()` / `GetSelectorRadius()`（子类覆写）。
     */
    bool ResolveTargets(const AACBattleUnitBase& Caster, TArray<FUnitId>& OutTargets) const;

    /** 便捷重载：用当前 Avatar 当施法者（只能在能力已激活、ActorInfo 就绪时用）。 */
    bool ResolveTargets(TArray<FUnitId>& OutTargets) const;

    /**
     * 目标选择器（默认 `PrimaryTarget`：复用 AI 已锁定的目标）。子类按内容覆写。
     */
    virtual EACSelectorType GetTargetSelector(const AACBattleUnitBase& Caster) const { return EACSelectorType::PrimaryTarget; }

    /** 选择器半径（`NeighborsN` 一类形状查询用；默认 1，与 `FACSkillDef::SelectorRadius` 默认一致）。 */
    virtual int32 GetSelectorRadius(const AACBattleUnitBase& Caster) const { return 1; }

    /**
     * 本次施放使用的射程覆盖值（喂给 `IsInRange` 的 `RangeOverride` 形参）。
     * `<= 0` = 用施法者基础射程（对应旧 `FACSkillDef::RangeOverride = -1`）；
     * 普攻覆写成"按 `FACAttackPatternDef::PreferredRange`"。
     */
    virtual float GetEffectiveRangeOverride(const AACBattleUnitBase& Caster) const { return -1.f; }

    /**
     * 是否要求"启动时能解析出目标、且主目标在射程内"
     */
    virtual bool RequiresTargetInRange() const { return true; }

    /**
     * 内核侧门控：这次行动该不该"尝试"激活本能力
     * 默认 `true`（普攻与被动没有额外前提）；`UACSkillAbilityBase` 覆写成 A15 的"专注满"。
     */
    virtual bool IsReadyToActivate(const AACBattleUnitBase& Caster) const { return true; }

    
    virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                    const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags,
                                    FGameplayTagContainer* OptionalRelevantTags) const override;

    /**
     * 射程判定：`Distance <= Range`，`RangeOverride <= 0` 时用施法者射程。
     * 六边形距离走 `UACHexGridStatics::Distance`。
     */
    bool IsInRange(const AACBattleUnitBase& Self, const AACBattleUnitBase& Target, float RangeOverride = -1.f) const;

    /**
     * 朝目标走一格：BFS 最短路（走到"距目标 <= 期望射程"的格子）→ 占位移动 → 派发移动钩子。
     * @return true = 真的走了一格（调用方据此判断"本帧是移动而不是攻击"）。
     *         已在期望射程内 / 无路可走 / 目标不存在时返回 false（不移动）。
     */
    static bool MoveOneStepTowards(UBattleWorld& World, AACBattleUnitBase& Unit, FUnitId TargetId);

    /**
     * 钩子派发入口：EventBus（可取消/可改值）+ GameplayEvent（玩法被动触发）。
     *
     * @param EventRecipient 事件要发给哪个单位的 ASC；nullptr = 只走 EventBus。
     *                       技能/普攻一律传施法者自己（自己的被动由自己的钩子触发）；
     *                       "无 owner 过滤"的钩子（`Hook.Kill` / `Hook.Death` / `Hook.BattleStart`）
     *                       由内核用 `UBattleWorld::BroadcastHookGameplayEvent` 广播给全场。
     * @return EventBus 的返回值（`bCancel` / `bInterrupt`），调用方据此决定是否中止本次动作。
     */
    FBattleHookResult DispatchHook(FGameplayTag HookTag, FBattleHookContext& Context,
                                   AACBattleUnitBase* EventRecipient) const;
    
    static FBattleHookResult DispatchHook(UBattleWorld& World, FGameplayTag HookTag, FBattleHookContext& Context,
                                          AACBattleUnitBase* EventRecipient);

    /** 只走 GameplayEvent 通道（发给一个单位）；返回被触发的能力条数（引擎 `HandleGameplayEvent` 的返回值）。 */
    int32 SendHookToUnit(AACBattleUnitBase* Recipient, FGameplayTag HookTag, const FBattleHookContext& Context) const;

    /** static 版：与成员版同一份实现（世界显式传入）。 */
    static int32 SendHookToUnit(UBattleWorld& World, AACBattleUnitBase* Recipient, FGameplayTag HookTag,
                                const FBattleHookContext& Context);

    /**
     * 广播 GameplayEvent 给全场存活单位。
     */
    int32 BroadcastHookGameplayEvent(FGameplayTag HookTag, const FBattleHookContext& Context) const;

    /**
     * `FBattleHookContext` → `FGameplayEventData` 的唯一映射处。
     * 需要世界才能把整型 `FUnitId` 还原成 Actor（`Instigator` / `Target` 收的是 `AActor*`）。
     */
    static FGameplayEventData MakeEventPayload(const UBattleWorld& World, const FBattleHookContext& Context);

    /** 成员版：世界取自身（只能在能力已激活时用）。取不到世界时返回默认载荷（不崩）。 */
    FGameplayEventData MakeEventPayload(const FBattleHookContext& Context) const;

    /**
     * 反向：被动能力在 `ActivateAbility` 里把 `TriggerEventData` 还原成钩子上下文。
     */
    static FBattleHookContext MakeHookContext(const FGameplayEventData* TriggerEventData);

    // ---------------------------------------------------------------------
    // GameplayEffect 施加（能力内的统一出口）
    // ---------------------------------------------------------------------

    /**
     * 把一个 GE 施加到目标单位（走 `MakeOutgoingSpec` + `ApplyGameplayEffectSpecToTarget`）。
     *
     * @param SetByCallerValues 键名 → 值；空表示这个 GE 不需要 SetByCaller。
     *                          键名常量在各自的 GE 类上（例如 `UACGE_Shield::GetDurationDataName()`），
     *                          不要在调用点写字符串字面量。
     * @param Stacks            层数（`FGameplayEffectSpec::SetStackCount`，GameplayEffect.h:1059）；
     *                          旧 `ApplyAbnormalState(Stacks = 3)` 就是这条路径。
     * @return true = spec 已成功交给目标 ASC（不代表目标真的接受了层数，那由引擎的叠加策略决定）。
     */
    static bool ApplyEffectToUnit(AACBattleUnitBase& SourceUnit, AACBattleUnitBase& TargetUnit,
                                  TSubclassOf<UGameplayEffect> EffectClass,
                                  const TMap<FName, float>& SetByCallerValues, int32 Stacks = 1);

    /** 便捷重载：不需要 SetByCaller 的 GE（绝大多数内容 GE 都属于这一类）。 */
    static bool ApplyEffectToUnit(AACBattleUnitBase& SourceUnit, AACBattleUnitBase& TargetUnit,
                                  TSubclassOf<UGameplayEffect> EffectClass, int32 Stacks = 1);

    /**
     * 组一条 SetByCaller 的便捷工厂：`ApplyEffectToUnit(..., MakeSetByCaller(Name, Value))`。
     * 当前内容里每条 GE 最多只需要一条 SetByCaller，因此不需要更复杂的容器接口。
     */
    static TMap<FName, float> MakeSetByCaller(FName DataName, float Value);

    /**
     * 给单位加专注（可为负）。改值走 GE，钩子与埋点在原处，逐项等价于旧 `GrantFocus`
     *
     * @return 实际变化量（新值 - 旧值）。
     */
    static float GrantFocusToUnit(AACBattleUnitBase& Unit, float Amount);

    /**
     * 写一条 `Category = Ability` 的结构化日志。
     *
     * 位置：在 BeforeSkillCast / EnemySkillCast 的取消与打断判定之后、在效果之前 ——
     *   被取消的技能不产生这条记录。
     */
    void LogAbilityEvent(FName EventTag, FUnitId Source, FUnitId Target,
                         float ValueA = 0.f, float ValueB = 0.f, int32 IntValue = 0) const;

protected:
    /**
     * 登记一条"GameplayEvent 触发"
     * "能力必须已被授予"是事件能触发它的前提。
     */
    void AddEventTrigger(FGameplayTag HookTag);

    /**
     * 触发次数计数。
     * @param MaxTriggers `<= 0` = 不限次数；否则达到上限后返回 false。
     */
    bool ConsumeTrigger(int32 MaxTriggers);

    /** 已触发次数（调试/日志用）。 */
    int32 GetTriggerCount() const { return TriggerCount; }

    // ---------------------------------------------------------------------
    // 敌方技能可打断（旧 `ExecuteSkill` 的 `Hook.EnemySkillCast` 分支）
    // ---------------------------------------------------------------------

    /**
     * 本次施放是否要先过 Hook.EnemySkillCast（可打断）。
     */
    virtual bool IsEnemySkillCastInterruptible(const AACBattleUnitBase& Caster) const;

    /**
     * 结束能力的统一出口：`EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, bWasCancelled)`。
     */
    void FinishAbility(bool bWasCancelled);

private:
    int32 TriggerCount = 0;
};
