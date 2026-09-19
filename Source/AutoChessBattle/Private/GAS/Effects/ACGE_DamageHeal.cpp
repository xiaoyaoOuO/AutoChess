// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 / §7 阶段 3.1）：伤害与治疗通道 GE 的实现。
#include "GAS/Effects/ACGE_DamageHeal.h"

#include "GAS/ACDamageExecution.h"
#include "GAS/Effects/ACHealExecution.h"

UACGE_InstantDamage::UACGE_InstantDamage()
{
    // 时长策略 = Instant（基类默认值，`UACGameplayEffectBase` 构造函数里显式设了一次）。
    // 这里不改它：伤害是"一次结算完就走"，没有任何东西需要留在 ASC 里。
    // ⚠️ 刻意**不加** `GrantedTags`：旧的 `ApplyDamage` 动作不授予任何标签，
    //    加了会多出"目标身上挂着一个瞬时标签"的副作用（瞬时 GE 不进容器，标签也无从移除）。

    // 伤害通道：`UACDamageExecution`（阶段 2 已就绪）内部把量交给 `FCombatResolver::ApplyDamage`。
    // 它读的 SetByCaller 键名是 **`Data.Damage`**（ACDamageExecution.cpp:166）。
    AddExecution(UACDamageExecution::StaticClass());
}

UACGE_InstantHeal::UACGE_InstantHeal()
{
    // 时长策略 = Instant（基类默认值）。
    // 治疗通道：`UACHealExecution` 内部把量交给 `FCombatResolver::ApplyHeal`。
    AddExecution(UACHealExecution::StaticClass());
}
