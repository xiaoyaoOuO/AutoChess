// 阶段 3.1b 新增（GAS 重构实施方案 §4.1 / §7 阶段 3.1）：**普攻能力**。
//
// 承载物：`FAbilityExecutor::ExecuteBasicAttack`（`Private/ACAbilityExecutor.cpp:254-342`）。
// 本类逐项等价复刻它，一行数值/顺序都没改（C6/D10）：
//
//   | 旧行为（ACAbilityExecutor.cpp）                        | 本类                        |
//   | ----------------------------------------------------- | --------------------------- |
//   | `SetActionState(Attacking)`（:256）                     | `ActivateAbility` 开头       |
//   | 攻击时朝向目标（:257-262，RowDelta >= 0 ? Down : Up）    | 同（`SetFacing`）            |
//   | `Dispatch(Hook.BeforeAttack)`（:268）                   | `DispatchHook`（双发）        |
//   | `Dispatch(Hook.AllyAttack)`（:270）                     | `DispatchHook`（双发）        |
//   | `DefaultSegments` 兜底（:273-278）                      | `MakeDefaultSegment()`       |
//   | 治疗型普攻 `ApplyHeal(ATK × HealRatio)`（:280-288）      | 同                           |
//   | 多段 × 每段 `HitCount` 次（:291-334）                    | 同                           |
//   | `FDamageRequest` 六项（:296-304）                       | 同                           |
//   | 段级 `OnHitEffectBlockIds` 执行效果块（:309-315）         | **改为施加 GE**（`OnHitEffects`）|
//   | `Dispatch(Hook.Crit)`（:317-325）                       | `DispatchHook`（双发）        |
//   | `Dispatch(Hook.Hit)`（:327-332）                        | `DispatchHook`（双发）        |
//   | `IncrementAttackPatternHits()`（:337）                  | 同                           |
//   | `Dispatch(Hook.AfterAttack)`（:338）                    | `DispatchHook`（双发）        |
//   | `OnBasicAttackHit`（:339 → :637-651）                   | 同（读属性集 `FocusPerAttack`）|
//   | `SetActionState(Idle)`（:341）                          | 同                           |
//
// ---------------------------------------------------------------------------
// 与旧实现的**三处刻意差异**（都属于"承载形式变了"，不是数值变了）
// ---------------------------------------------------------------------------
//   ① 段级附加效果：旧的 `FACAttackSegment::OnHitEffectBlockIds`（`TArray<FName>` 效果块 ID）
//      在本阶段**不读**（那属于 3.3 的内容翻译）。替代字段是本类的
//      `OnHitEffects`（`TArray<TSubclassOf<UGameplayEffect>>`）。
//      阶段 3.3：契约侧的同名字段已改成 `FACAttackSegment::OnHitEffects`
//      （类型一致，内容侧可以直接填类）。当前内容里该数组仍然为空 ——
//      4 个普攻模式都没有段级附加效果（旧实现里 `OnHitEffectBlockIds` 也从来是空的），
//      因此 `ApplySegmentOnHitEffects` 是空操作，与改造前"数组为空就什么都不做"逐字等价。
//      ⚠️ **已知粒度差异**：旧的附加效果是**逐段**配置的，而 `OnHitEffects` 挂在能力上、
//      对**每一段**都生效。当前 4 个普攻模式都只有 1 段，因此现阶段行为等价；
//      将来真出现"多段各自附加不同效果"的内容，把 `OnHitEffects` 换成
//      "与 `Segments` 等长的数组"即可（本阶段不做，避免造一个没人填的字段）。
//   ② 治疗型普攻的目标：旧实现在 `FAbilityExecutor::RequestAction` 里选（`LowestHpPercentAlly`），
//      本类把"选目标"收到 `GetTargetSelector()` 里（同样是 `LowestHpPercentAlly` + 半径 1）。
//   ③ 移动：旧 `RequestAction` 在"目标超出射程"时调 `MoveOneStepTowards` 并**不走普攻**。
//      本类**不含移动**：射程不满足时 `CanActivateAbility` 直接返回 false（不激活），
//      由内核的决策层（§2.3 第 5 步，3.3 迁移 `RequestAction` 的剩余部分）决定改走移动 ——
//      `UACBattleAbility::MoveOneStepTowards` 已经就位，内核一行调用即可。
//      这样切分的原因：**能力不该替调度器决定"这帧是攻击还是移动"**，
//      那件事依赖行动条与 AI 意图，是内核的职责。
#pragma once

#include "CoreMinimal.h"
#include "GAS/ACBattleAbility.h"
#include "GameplayEffect.h"
// `FACAttackPatternDef` / `FACAttackSegment`：本类的私有辅助（`GetSegments`）在**声明**里就用到它们，
// 因此不能只靠 .cpp 包含（前向声明也不够：`TArray<FACAttackSegment>` 需要完整类型）。
#include "Core/ACDataTypes.h"
#include "ACBasicAttackAbility.generated.h"

class UGameplayEffect;

/**
 * 普攻能力：一个单位"行动条到点且目标在射程内"时执行的那一次攻击。
 *
 * 数据来源是**单位的定义**（`AACBattleUnitBase::GetAttackPattern()` → `FACAttackPatternDef`），
 * 而不是能力自己的字段：§4.1 明确 `FACAttackPatternDef` 保留在项目侧
 *（与 `FActionScheduler` 的攻速推进耦合），`FACAttackSegment` 用能力内的循环表达。
 * 因此本能力类**没有**对应 `UACAbilitySet` 里的内容资产，普攻模式仍然由干员/敌人定义给出。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACBasicAttackAbility : public UACBattleAbility
{
    GENERATED_BODY()

public:
    UACBasicAttackAbility();

    /**
     * 段级附加效果（新承载：GE 类数组）。
     *
     * - 旧的 `FACAttackSegment::OnHitEffectBlockIds` 是 `TArray<FName>`（效果块 ID），
     *   而 GE 是**类**，两者不是同一层次的标识 —— 这也正是 §4.2 "效果块 → GE" 的全部含义。
     * - **阶段 3.3**：契约侧的 `FACAttackSegment::OnHitEffects` 已经是同类型的 `TSubclassOf<UGameplayEffect>` 数组，
     *   内容侧可以直接填类；当前内容里它为空（旧实现也从来是空的），
     *   因此 `ApplySegmentOnHitEffects` 是空操作，行为与旧实现
     *   "`OnHitEffectBlockIds.Num() == 0` 时什么都不做"（`ACAbilityExecutor.cpp:309`）等价。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Battle|BasicAttack")
    TArray<TSubclassOf<UGameplayEffect>> OnHitEffects;

    // ---- §4.1：普攻的"目标 / 射程"来自攻击模式，见 .cpp ----

    virtual EACSelectorType GetTargetSelector(const AACBattleUnitBase& Caster) const override;
    virtual float GetEffectiveRangeOverride(const AACBattleUnitBase& Caster) const override;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;

private:
    /**
     * 取本次攻击用的段列表：`Pattern.Segments` 为空时用"100% 攻击力物理伤害"的兜底段
     *（旧 `ExecuteBasicAttack` 的 `static TArray<FACAttackSegment> DefaultSegments`，:273-278）。
     */
    static const TArray<FACAttackSegment>& GetSegments(const FACAttackPatternDef& Pattern);

    /**
     * 段级附加效果：把 `OnHitEffects` 逐个施加到**目标**身上。
     * 旧的对应实现是 `World->Effects().ExecuteBlocks(Segment.OnHitEffectBlockIds, EffectContext)`
     *（`ACAbilityExecutor.cpp:311-314`），其中 `EffectContext.Source = 攻击者`、
     * `PrimaryTarget = 目标` —— 本函数用同样的来源/目标组 spec。
     */
    void ApplySegmentOnHitEffects(AACBattleUnitBase& Caster, AACBattleUnitBase& Target);

    /**
     * 普攻命中后的专注回复（旧 `FAbilityExecutor::OnBasicAttackHit`，`ACAbilityExecutor.cpp:637-651`）。
     *
     * A11：射手普攻回专注。**读属性集**（阶段 2 起专注的权威在 `UACBattleAttributeSet`）：
     *   ① 先读 `EACStat::FocusPerAttack`（= 属性集 `FocusPerAttack` 的当前值，
     *      `AACBattleUnitBase::GetStat` 就是读它）；
     *   ② 为 0 且**远程**时回退到规则配置 `UBattleRuleConfig::ShooterFocusPerAttack`
     *      （旧实现回退 5.f，配置缺失时这里也回退 5.f，三级口径一致）。
     * 旧实现读的是 `FFocusState::PerAttackGain`（成员变量），那个字段已随专注迁属性集而废；
     * "为 0 才回退"这条判据本身逐字保留。
     */
    static void OnBasicAttackHit(AACBattleUnitBase& Caster);
};
