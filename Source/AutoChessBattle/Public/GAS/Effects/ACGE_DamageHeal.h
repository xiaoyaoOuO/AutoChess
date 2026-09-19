// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 的 `ApplyDamage` / `ApplyHeal` 两行 / §7 阶段 3.1）：
// 伤害与治疗的**瞬时通道 GE**。
//
// 这两个类都是"内容无关的通道"：真正的内容（技能 200% 攻击力、嗜血回 8% 生命……）由
// 施加方在 spec 上把量写进 SetByCaller，或者由子类在构造里给出固定值（见 ACGE_ContentEffects.h）。
//
// 为什么用 `Executions` 而不是 `Modifiers`（§4.6 新契约）：
//   **只有 `UACDamageExecution` 能调 `FCombatResolver`；只有 `FCombatResolver` 能改 `Health`。**
//   `Modifier` 的结果由引擎聚合器直接落到属性上，会绕开结算管线，七个检查点
//   （Requested → … → Overkill）就丢了，日志里会出现"血量变了但没有对应伤害记录"。
//   因此：
//     伤害 → `UACDamageExecution`（阶段 2 已就绪，读 SetByCaller 键名 **`Data.Damage`**）；
//     治疗 → `UACHealExecution`（本阶段新增，读键名 `Data.Heal`）；
//   两者的实现都走 `FCombatResolver`，与旧 `FEffectSystem` 的 `ApplyDamage` / `ApplyHeal` 分支等价
//   （ACEffectSystem.cpp:601-628）。
#pragma once

#include "CoreMinimal.h"
#include "GAS/Effects/ACGameplayEffectBase.h"
#include "ACGE_DamageHeal.generated.h"

/**
 * 瞬时伤害 GE。
 *
 * 来源与等价关系：
 *   - `EACActionType::ApplyDamage`（ACEffectSystem.cpp:601-616）：
 *     `RawAmount = Action.Value.Get(Tier) * 施法者 ATK`，`Reason = Skill`，
 *     `bCanCrit = Action.bCanCrit`，`DeclaredJudgment = Action.JudgmentPoint`；
 *   - 阶段 2 起 `UACDamageExecution` 已按同一口径组 `FDamageRequest`
 *     （ACDamageExecution.cpp:121-186，`Reason` 默认 `Skill`、`bCanCrit = true`、
 *      `DeclaredJudgment = AppliedHpLoss`）。
 *
 * **量的口径**：SetByCaller 键名 `Data.Damage` = **原始伤害量**（未暴击、未减免），
 * 即旧代码里 `FDamageRequest::RawAmount` 的那个值。与 `UACDamageExecution.cpp:154-167` 的约定逐字一致。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_InstantDamage : public UACGameplayEffectBase
{
    GENERATED_BODY()

public:
    UACGE_InstantDamage();

    /** SetByCaller 键名（唯一常量处，避免字符串散落）。 */
    static FName GetDamageDataName() { return FName(TEXT("Data.Damage")); }
};

/**
 * 瞬时治疗 GE。
 *
 * 来源与等价关系：`EACActionType::ApplyHeal`（ACEffectSystem.cpp:618-628）：
 * `RawAmount = Action.Value.Get(Tier)`，其余走 `FCombatResolver::ApplyHeal` 的既有口径
 * （含 `bAllowOverhealConversion = true` 的重载转化，ACCombatResolver.cpp:353-380）。
 *
 * **量的口径**：SetByCaller 键名 `Data.Heal` = 治疗量（未截断，过量部分由结算器算）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_InstantHeal : public UACGameplayEffectBase
{
    GENERATED_BODY()

public:
    UACGE_InstantHeal();

    /** SetByCaller 键名。 */
    static FName GetHealDataName() { return FName(TEXT("Data.Heal")); }
};
