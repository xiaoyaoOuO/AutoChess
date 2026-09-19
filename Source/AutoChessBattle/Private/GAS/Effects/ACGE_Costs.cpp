// 阶段 3.1a 新增（GAS 重构实施方案 §4.1 的 Cost 映射表 / §7 阶段 3.1）：Cost GE 实现。
#include "GAS/Effects/ACGE_Costs.h"

#include "GAS/ACBattleAttributeSet.h"

UACGE_Cost_ClearAll::UACGE_Cost_ClearAll()
{
    // `Instant`（基类默认）：消耗是一次性的，不需要留在 ASC 里。
    // 幅度：`Focus += (-999)`，夹取由 `UACBattleAttributeSet::PreAttributeChange` 的 `Focus <= FocusMax`
    // 与"资源属性不小于 0"的分支完成（ACBattleAttributeSet.h:279-291）。
    AddAdditiveModifier(UACBattleAttributeSet::GetFocusAttribute(), -ClearAllDrainAmount);
}

UACGE_Cost_Fixed::UACGE_Cost_Fixed()
{
    // `Instant` + `Additive` 作用于 `Focus`，量由施加方按 `Data.FocusCost` 传入（**带符号**）。
    AddSetByCallerModifier(UACBattleAttributeSet::GetFocusAttribute(),
                           EGameplayModOp::Additive, GetCostDataName());
}

UACGE_Cost_DrainPerSecond::UACGE_Cost_DrainPerSecond()
{
    // §4.1：`HasDuration` + `Period = 1`（每秒扣 DrainPerSecond）。
    MakeHasDurationByCaller(GetDurationDataName());
    SetPeriodSeconds(1.f);

    // 每秒执行一次：GE 的周期执行由 ASC 的 `TickComponent(DeltaTime)` 推进
    // （§2.3 第 2 步：`UBattleWorld::Step` 手动调），而**不是**引擎 Actor tick
    // （单位 Actor 的 `PrimaryActorTick.bCanEverTick = false`，见 ACBattleUnitBase.cpp:19）。
    // 因此"每秒"是"累积到一个周期"的语义，与旧内核的秒边界一致。
    AddSetByCallerModifier(UACBattleAttributeSet::GetFocusAttribute(),
                           EGameplayModOp::Additive, GetDrainPerSecondDataName());
}
