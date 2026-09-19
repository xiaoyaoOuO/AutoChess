// 阶段 3.1b 新增（GAS 重构实施方案 §4.1 / §7 阶段 3.1）：**技能能力基类**。
//
// 承载物：`FAbilityExecutor::ExecuteSkill`（`Private/ACAbilityExecutor.cpp:344-424`）。
// 两个具体技能（巨斧裂体 / 守望）的差别**只有效果与目标选择器**，而时序完全同构，
// 因此把时序收在本类、把效果留给子类的 `ApplySkillEffects()` ——
// 与 3.1a 把 GE 行为收进 `UACGameplayEffectBase` 是同一个理由（分叉的代价迟早会以
// "某个技能偶尔不派发 SkillCast 钩子"这种形式收账）。
//
// ---------------------------------------------------------------------------
// 逐项对照旧 `ExecuteSkill`（`ACAbilityExecutor.cpp:344-424`）
// ---------------------------------------------------------------------------
//   | 旧行号 | 旧行为                                        | 本类                          |
//   | ------ | --------------------------------------------- | ----------------------------- |
//   | :346-350 | `FSkillCastContext`（Caster/SkillId/Targets）| 不用（新世界没有它）           |
//   | :353-356 | `FBattleHookContext`（Source/Target/Time）    | `SkillHookContext`             |
//   | :359-364 | 敌方技能 `Hook.EnemySkillCast` 可打断 → Idle + `Hook.SkillInterrupted` + 不施放 | 同（`IsEnemySkillCastInterruptible`） |
//   | :365-369 | `Hook.BeforeSkillCast` 的 `bCancel`/`bCancelled` → Idle + 不施放 | 同            |
//   | :371-373 | `SetActionState(Casting)`（Channel → Channeling）| 同（Instant → Casting）      |
//   | :376-387 | 专注消耗（ClearAll / Fixed / DrainPerSecond）  | `CommitBattleCost()`（Cost GE）|
//   | :389-391 | `Hook.SkillCast` + `Hook.AllySkillCast`        | `DispatchHook`（双发）         |
//   | :397-403 | 埋点：Category=Ability、EventTag=SkillId        | `LogAbilityEvent(SkillId, …)`  |
//   | :405    | `Effects().ExecuteBlocks(Skill.EffectBlockIds)`| `ApplySkillEffects()`（GE）    |
//   | :407-423 | 引导入队 / 设 Idle                             | 设 Idle（无引导内容，见下）     |
//
// ---------------------------------------------------------------------------
// 本阶段**不产出引导（Channel）能力**，理由如下（已核实，不是遗漏）
// ---------------------------------------------------------------------------
//   ① 内容侧**没有任何 Channel 技能**：唯一的 `EACCastType` 写入点是内容定义里的三个干员，
//      三个都是 `EACCastType::Instant`（`ACBattleContentDefinitions.cpp:583` 索利瓦尔 /
//      :650 铁壁；蕾拉的 `SkillId` 为空、根本不施放）。全仓没有一处写 `EACCastType::Channel`。
//   ② 引导的承载形式（§4.1）是 "`InstancingPolicy = InstancedPerActor` + 能力内自持
//      `UAbilityTask`（延迟 / 重复）+ Cost GE（`HasDuration` + `Period`）"，
//      与本类的"同步施放后 `EndAbility`"是两套生命周期。
//   ③ 因此本阶段只产出 Instant 技能；等真出现引导内容时，按 §4.1 加一个
//      `UACChannelSkillAbility`（`UAbilityTask_WaitDelay` + `UAbilityTask_Repeat`），
//      本类作为它的父类不需要改。
//   同理，前摇（`FACSkillDef::WindUpSeconds` → `UAbilityTask_WaitDelay` 后再 `CommitAbility`）
//   也没有内容在用：唯一显式写入是 `Skill.WindUpSeconds = 0.f`
//   （`ACBattleContentDefinitions.cpp:584`，A10"默认无前摇"），因此本类不引入延迟任务。
#pragma once

#include "CoreMinimal.h"
#include "GAS/ACBattleAbility.h"
#include "ACSkillAbilityBase.generated.h"

/**
 * Instant 技能能力的公共基类。
 *
 * 子类只需要做三件事：
 *   ① 构造函数里填 `SkillId`（埋点用）与 `CostGameplayEffectClass`（§4.1 的 CostMode → Cost GE）；
 *   ② 需要时覆写 `GetTargetSelector` / `GetEffectiveRangeOverride`（默认 = 内容里最常见的
 *      `PrimaryTarget` + 施法者射程）；
 *   ③ 实现 `ApplySkillEffects()`。
 */
UCLASS(Abstract)
class AUTOCHESSBATTLE_API UACSkillAbilityBase : public UACBattleAbility
{
    GENERATED_BODY()

public:
    UACSkillAbilityBase();

    /**
     * 技能 ID。**同时是施放埋点的 EventTag**（旧口径 `Ability | <SkillId>`）。
     *
     * 内容侧的对应值：旧 `FACSkillDef::SkillId`（阶段 3.2b 已随技能线删除），取值 `SK_001`（索利瓦尔）/
     * `SK_Ironwall_Guard`（铁壁）。阶段 3.3 的内容翻译必须把同一个字符串填进本字段 ——
     * 日志按它分组做基线比对，两边不一致会让"技能埋点对不上"变成需要推理的问题。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Skill")
    FName SkillId;

    /**
     * 表现 Cue（旧 `FACSkillDef::PresentationCueId`）。
     *
     * 本阶段**只留字段、不派发**：§4.7 的表现层迁移（`GameplayCue.Skill.*` + `UGameplayCueNotify`）
     * 属阶段 4。留字段的价值是"内容翻译时不用再回来改数据资产"。
     * 类型选 `FName` 而不是 `FGameplayTag`：旧数据是 `Cue_OP01_Skill` 这样的名字，
     * 而 `GameplayCue.*` 标签体系要到阶段 4 才落地，现在定类型等于替阶段 4 拍板。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|Skill")
    FName PresentationCueId;

    /**
     * **A15：专注满才放技能**（旧 `FAbilityExecutor::CanCastSkill` 的判据）。
     *
     * 这是内核侧"该不该尝试放技能"的判定（§2.3 第 5 步），见基类 `IsReadyToActivate` 的长注释：
     * 内核对 CDO 调它 → 通过才 `TryActivateAbility` → **失败则 fallback 普攻**。
     *
     * ⚠️ 只读形参单位（属性集读数），因此在本函数被 CDO 调用时同样安全。
     */
    virtual bool IsReadyToActivate(const AACBattleUnitBase& Caster) const override;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;

    /**
     * 子类实现：把技能效果施加到解析出的目标上（旧 `World->Effects().ExecuteBlocks(Skill.EffectBlockIds, …)`）。
     *
     * @param Targets 已解析的目标（`Targets[0]` 是主目标；可能为空 = 无目标技能）。
     *                默认实现什么都不做，并打一条 Warning —— "技能没有效果"通常是子类忘了实现，
     *                静默通过会让"技能放了但没伤害"变成一场没有线索的排查。
     */
    virtual void ApplySkillEffects(const TArray<FUnitId>& Targets);

    /** 本次施放的主目标（`Targets[0]`；无目标时为 `InvalidUnitId`）。 */
    FUnitId GetPrimaryTargetId() const { return PrimaryTargetId; }

private:
    /** 本次激活解析出的主目标，供 `ApplySkillEffects` 的实现读取（每次激活开头重设）。 */
    FUnitId PrimaryTargetId = InvalidUnitId;
};
