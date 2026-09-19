// 阶段 2 改造（GAS 重构实施方案 §3.2 / §4.3 / §7 阶段 2）：属性计算从"自研管线"搬到 GAS 属性集。
//
// 阶段 1 之前：本文件是 `M07 属性与修饰器` 的**唯一计算入口**（`FBattleStatSheet` 持有
//   Base 数组 + Current 数组 + 修饰器数组 + 转换规则 + 脏标记，`FBattleStatPipeline` 负责重算）。
// 阶段 2 之后：**属性的唯一权威是 `UACBattleAttributeSet`**（`AACBattleUnitBase::GetStat` 直接读它）。
//   本文件只剩两件事：
//     ① `FBattleStatSheet`：`FACStatBlock` 的落地形态（**只有 Base 数组**），
//        既是 Run↔Battle 的属性口径快照，也是"战斗结果持久化口径"（`GetBaseMaxHP`）与
//        "召唤继承快照"（`SnapshotValues`）的数据源；
//     ② `FACStatConversionRule`：王恩 / 蕾拉 / 科雷 / 那摩的攻速特例（本阶段**保留结构**，
//        但改由调用方在 `InitializeFromStatBlock` **之前**作用于 `FACStatBlock`，见 `ApplyToBlock`）。
//
// **已删除**（阶段 2，§3.2）：`Current` 数组、`Modifiers` 数组、`Version`、`bDirty`，
// 以及 `FBattleStatPipeline` 的 `Recompute` / `AddModifier` / `RemoveBySource` / `RemoveExpired` /
// `ClearBattleTemp` / `GetStat` / `GetVersion` / `ApplyConversionRules`。
// 这些能力全部由 GAS 承担：修饰器 → `FGameplayModifierInfo` + 聚合器（C8：不写 MMC），
// 到期 → GE 时长，重算 → 聚合器自动（§4.3 映射表）。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleTypes.h"

class UACBattleAttributeSet;

/**
 * 攻速特例（王恩 / 蕾拉 / 科雷 / 那摩），数据驱动。
 *
 * 阶段 2 的取舍（实施方案 §4.3 末行给了"GE Modifier 或 MMC"两条路，本阶段走第三条）：
 *   **保留结构，改为作用于 `FACStatBlock`（初始化前）**，理由是
 *     ① 本阶段的目标是"把读权威搬到属性集"而**不改数值**；在属性集初始化**之前**改输入块，
 *        属性集与 `FBattleStatSheet` 看到的是同一份已转换的值，数值等价性最容易证明；
 *     ② C8 禁止为属性修饰写 `UGameplayModMagnitudeCalculation`。转换规则严格说不是"修饰器"，
 *        但把它做成 MMC 就会在阶段 2 引入一条只有它用的新机制，收益为负；
 *     ③ 现阶段**没有任何调用方往规则数组 / 输入块里填过规则**（全仓无写入点），
 *        因此这是一段"保留接口、行为为空操作"的代码，不承担数值风险。
 *   阶段 3/4 若真要用它，再按 §4.3 裁决走 GE Modifier / MMC，本结构会被替换。
 */
struct AUTOCHESSBATTLE_API FACStatConversionRule
{
    EACStat Source = EACStat::ASPD;
    bool bFixedValue = false;
    float FixedValue = 0.f;
    float Cap = 0.f;                    // > 0 时启用
    EACStat OverflowTarget = EACStat::ATK;
    float OverflowRatio = 1.f;
    float GainScale = 1.f;              // 科雷 0.2
    float OscillationMin = 0.f;         // 那摩 80%
    float OscillationMax = 0.f;         // 那摩 120%
    float OscillationPeriod = 1.f;

    /**
     * 把本规则作用到一个 `FACStatBlock` 上（原地修改）。
     *
     * @param NowSeconds 当前**绝对时间**（秒），用于"那摩 80%→120% 循环"的相位；
     *                   负值 = 调用方拿不到时钟（取相位中值 0.5）。
     *
     * 公式与阶段 1 的 `FBattleStatPipeline::ApplyConversionRules`（Private/ACBattleStats.cpp:128-174）
     * **一字未改**，只换作用对象：`Sheet.Current[]` → `Block.Values[]`。
     * `Override 最后生效`那条旧行为（同属性取最后一个 Override）**没有搬过来**：
     * 转换规则里本来就没有 Override 语义（它只做"固定值 / 增益缩放 / 振荡 / 超上限溢出"），
     * 旧实现里那句是对 `FBattleStatModifier` 的处理，随修饰器一起删除。
     */
    void ApplyToBlock(FACStatBlock& Block, float NowSeconds) const;
};

/**
 * 单位属性表（阶段 2：只剩 Base）。
 *
 * 为什么还留着：它是 `FACStatBlock`（Run→Battle 的属性口径快照）在战斗侧的落地形态，
 * 有三处**必须**继续存在且口径不变：
 *   ① `GetBaseMaxHP` 的"战斗结果持久化口径"（`FACUnitBattleResult::RemainingBaseHP`）；
 *   ② `SnapshotValues` 的"召唤继承快照"；
 *   ③ `SetBaseFromBlock` 的 Run 层组装入口。
 */
struct AUTOCHESSBATTLE_API FBattleStatSheet
{
    /** 长度 `ACStatCount`。**阶段 2 起它就是属性集的 Base 值来源**（属性集是读权威，本数组是写入源）。 */
    TArray<float> Base;

    void InitDefaults()
    {
        Base.SetNumZeroed(ACStatCount);
    }
};

/** 属性工具（静态，无状态）。阶段 2 起只剩下这六个工具函数（§3.2）。 */
class AUTOCHESSBATTLE_API FBattleStatPipeline
{
public:
    /** Run 层组装用：把 `FACStatBlock` 逐项写进 `Sheet.Base`（缺项补 0）。 */
    static void SetBaseFromBlock(FBattleStatSheet& Sheet, const FACStatBlock& Block);

    /** 战斗结果持久化口径（见实现的简化说明）。 */
    static float GetBaseMaxHP(const FBattleStatSheet& Sheet);

    /** 快照全部基础值（召唤继承使用）。 */
    static void SnapshotValues(const FBattleStatSheet& Sheet, TArray<float>& OutValues);

    // -----------------------------------------------------------------------
    // 三个派生量（阶段 2：数据来源从 `FBattleStatSheet` 换成属性集）
    // -----------------------------------------------------------------------
    // 公式**一字未改**（阶段 2 的验收要求"数值等价"），只把取值换成属性集的 CurrentValue：
    // 属性集的 CurrentValue 就是"Base + 已聚合修饰器"的最终值，
    // 与旧实现里 `Sheet.Current[]`（Recompute 的产物）语义等价 —— 修饰器不存在时二者都等于 Base。
    //
    // 为什么直接读属性集、**不**走 `ASC->GetNumericAttribute(...)`（任务 1 的推荐姿势，同样适用这里）：
    //   ① 属性集的 CurrentValue 本身就是权威值，ASC 的聚合器查询只是"同一个数"的另一条路径；
    //   ② 聚合器查询要求 ASC 已经 `InitAbilityActorInfo`；而派生量在单位**出生后**才被读，
    //      看起来安全，但阶段 2 的初始化路径（延迟构造期写属性集）刻意**不经过 ASC**，
    //      两条路径混用会让"到底谁在读谁"重新变得需要推理 —— 统一走属性集最省心。
    /** 暴击概率：Clamp(暴击值, 0, 100) / 100。 */
    static float GetCritChance(const UACBattleAttributeSet& Attributes);

    /** 暴击倍率：1.2 + max(0, 暴击值 - 100) / 100 + 额外暴击伤害%。 */
    static float GetCritMultiplier(const UACBattleAttributeSet& Attributes);

    /** 攻速（Hz）：每点攻速 = 每秒 0.01 次。 */
    static float GetAttackSpeedHz(const UACBattleAttributeSet& Attributes);
};
