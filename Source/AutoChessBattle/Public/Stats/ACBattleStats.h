// 阶段 2 之后：**属性的唯一权威是 `UACBattleAttributeSet`**（`AACBattleUnitBase::GetStat` 直接读它）。
//   本文件只剩两件事：
//     ① `FBattleStatSheet`：`FACStatBlock` 的落地形态（**只有 Base 数组**），
//        既是 Run↔Battle 的属性口径快照，也是"战斗结果持久化口径"（`GetBaseMaxHP`）与
//        "召唤继承快照"（`SnapshotValues`）的数据源；
//     ② `FACStatConversionRule`：王恩 / 蕾拉 / 科雷 / 那摩的攻速特例（本阶段**保留结构**，
//        但改由调用方在 `InitializeFromStatBlock` **之前**作用于 `FACStatBlock`，见 `ApplyToBlock`）。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleTypes.h"

class UACBattleAttributeSet;

/**
 * 攻速特例（王恩 / 蕾拉 / 科雷 / 那摩），数据驱动。
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
