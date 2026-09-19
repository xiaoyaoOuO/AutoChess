// 阶段 3.1a 新增（GAS 重构实施方案 §4.1 的 Cost 映射表 / §7 阶段 3.1）：**Cost GE（4 个）**。
//
// §4.1 映射表（逐行）：
//   | 旧 `EACFocusCostMode`     | GAS                                            |
//   | ------------------------ | ---------------------------------------------- |
//   | `ClearAll`               | Cost GE：`Instant` + `AttributeModifier`（`Add` 负值） |
//   | `Fixed`                  | Cost GE：`Instant` + `AttributeModifier`（`Add` 负值） |
//   | `DrainPerSecond`         | Cost GE：`HasDuration` + `Period`               |
//   | `None`                   | **无 Cost GE**（因此本文件只有 3 个类，不是 4 个）    |
//
// 为什么 Cost 是 GE 而不是能力里直接 `SetNumericAttributeBase`：
//   §4.1 的裁决就是"CostMode → Cost GE"，而且能力侧只需要
//   `UGameplayAbility::CommitAbility`（它自己按 `CostGameplayEffectClass` 施加），
//   不需要为三种消耗模式各写一段分支。这让"消耗方式"成为**内容**而不是代码。
//
// ⚠️ 三个 GE 都把 `Focus` 写成**负幅度**（`Additive`, 负值），而不是把 `Focus` 设成 0 /
//    设成某个目标值。理由：旧口径里"专注"是**当前值**（资源属性），
//    `ClearAll` 的实际语义是 `DrainFocus(999)` → 夹到 0（ACBattleContentDefinitions.cpp:252-256 的注释）；
//    而 `AttributeSet::PreAttributeChange` 会对 `Focus <= FocusMax` 做夹取
//    （ACBattleAttributeSet.h:279-291），因此"减 999"落地后必然夹成 `[0, FocusMax]` 内的值，
//    与旧 `DrainFocus` "Clamp 到 0，不会扣成负数"逐字一致。
#pragma once

#include "CoreMinimal.h"
#include "GAS/Effects/ACGameplayEffectBase.h"
#include "ACGE_Costs.generated.h"

/**
 * 清空全部专注（旧 `EACFocusCostMode::ClearAll`）。
 *
 * 来源：`FACSkillDef::CostMode = EACFocusCostMode::ClearAll`（ACBattleContentDefinitions.cpp:585 索利瓦尔、
 * :651 铁壁）+ 被动块里显式的 `DrainFocus(Tier(999.f))`（ACBattleContentDefinitions.cpp:252-256）：
 *   "专注清空：DrainFocus 走 FAbilityExecutor::DrainFocus（Clamp 到 0，不会扣成负数）"。
 * 因此本 GE 用 **-999**（远大于任何 `FocusMax`，等效于"清空"），而不是 `Override 0`：
 *   `Override 0` 会让"同一帧内其它专注来源"的顺序变得敏感（谁先谁后决定最终值），
 *   而"减一个足够大的数"与旧实现是同一种语义（夹取由属性集的 `PreAttributeChange` 兜住）。
 *
 * 幅度用 **SetByCaller 兜底 + 字面量**的取舍：这里用字面量 999（内容写死），
 * 因为"清空"的语义与具体数值无关，不需要施加方传参。需要精确扣固定值的场景用 `UACGE_Cost_Fixed`。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Cost_ClearAll : public UACGameplayEffectBase
{
    GENERATED_BODY()
public:
    UACGE_Cost_ClearAll();

    /** "清空"用的扣减量（>= 任何 FocusMax；与旧 `DrainFocus(999)` 同值）。 */
    static constexpr float ClearAllDrainAmount = 999.f;
};

/**
 * 固定消耗（旧 `EACFocusCostMode::Fixed`）。
 *
 * 来源：`FACSkillDef::FixedCost`（Core/ACDataTypes.h:153-155）—— 由技能定义给出固定值，
 * 因此**量必须由施加方传**（`Data.FocusCost`），不能在 GE 上写死：
 * 每个技能的 `FixedCost` 不同，而 GE 的 CDO 是共享的。
 *
 * 形态：`Instant` + `Additive` 作用于 `Focus`，幅度 = `Data.FocusCost`（施加方填**正数**，
 * GE 内部取负）。为什么让施加方填正数：旧数据 `FixedCost` 是正的"消耗量"，
 * 保持一致才不会出现"某个技能配了负的消费量"这种看不懂的数据。
 * 取负这件事在 GE 里做：`FACEffectMagnitude` 目前只支持字面量与 SetByCaller，
 * 没有"取负"包装，因此这里用一个极小的**派生 GE**思路行不通 ——
 * 改为在能力侧填负数更简单，但那样"约定"就藏在了能力里。
 * **本类的裁决**：填正数、GE 侧不取负，由 `UGameplayAbility::CommitAbility` 的调用方
 * （阶段 3.1b 的技能能力）确认后再定；当前实现按"施加方填**带符号**的量"设计
 * （即：想扣就填负数），并把这条约定写在这里 + 报告里。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Cost_Fixed : public UACGameplayEffectBase
{
    GENERATED_BODY()
public:
    UACGE_Cost_Fixed();

    /** SetByCaller 键名：固定消耗量（**带符号**，扣减填负数）。 */
    static FName GetCostDataName() { return FName(TEXT("Data.FocusCost")); }
};

/**
 * 每秒消耗（旧 `EACFocusCostMode::DrainPerSecond`）。
 *
 * 来源：`FACSkillDef::DrainPerSecond`（Core/ACDataTypes.h:156-158）。
 * 形态（§4.1）：`HasDuration` + `Period = 1`（每秒扣 `DrainPerSecond`）。
 *
 * 两个量都由施加方传：
 *   - `Data.FocusDrainPerSecond`：每秒扣减量（带符号，扣减填负数）；
 *   - `Data.DurationSeconds`：总时长（= 旧的 `ChannelDuration`；引导技能结束就该停扣）。
 *     为什么时长必须由施加方给：`FACSkillDef::ChannelDuration` 是每个技能各自的数
 *     （Core/ACDataTypes.h:176-177），GE 上写死会让所有引导技能共用一个时长。
 *
 * ⚠️ 与旧实现的**已知差异**（写进报告）：
 *   旧的 `DrainFocus` 是"能力在引导期间每帧/每秒调用一次"，
 *   而这里是"GE 的周期执行"（`Period = 1`，由 ASC 的 `TickComponent` 驱动）。
 *   两者的**总量**一致（都是"每秒扣 N"），但**时间点**可能差一帧的相位
 *   （GE 的周期起点是施加时刻）。这不改变数值口径（D10），但会影响日志里的时间戳对齐。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Cost_DrainPerSecond : public UACGameplayEffectBase
{
    GENERATED_BODY()
public:
    UACGE_Cost_DrainPerSecond();

    /** SetByCaller 键名：每秒扣减量（**带符号**，扣减填负数）。 */
    static FName GetDrainPerSecondDataName() { return FName(TEXT("Data.FocusDrainPerSecond")); }

    /** SetByCaller 键名：时长（= 旧的 `ChannelDuration`）。 */
    static FName GetDurationDataName() { return FName(TEXT("Data.DurationSeconds")); }
};
