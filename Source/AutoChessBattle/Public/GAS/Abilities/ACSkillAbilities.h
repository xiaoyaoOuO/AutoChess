// 阶段 3.1b 新增（GAS 重构实施方案 §4.1 / §7 阶段 3.1）：**2 个主动技能能力**。
//
// 内容来源（数值逐项抄自 `Source/AutoChess/Private/Content/ACBattleContentDefinitions.cpp`，C6/D10 数值不动）：
//   ① 巨斧裂体（`SK_001`，索利瓦尔）：:224-237 的效果块 + :580-591 的技能定义；
//   ② 守望（`SK_Ironwall_Guard`，铁壁）：:459-474 的效果块 + :647-653 的技能定义。
//
// 两个技能都**只覆写"目标选择器"与"效果"两件事**，时序全部继承 `UACSkillAbilityBase`
//（那份时序是逐行对着旧 `FAbilityExecutor::ExecuteSkill` 写的，见该文件头部的对照表）。
#pragma once

#include "CoreMinimal.h"
#include "GAS/Abilities/ACSkillAbilityBase.h"
#include "ACSkillAbilities.generated.h"

/**
 * 巨斧裂体（`SK_001`）：对**当前锁定的目标**造成 **200% 攻击力物理伤害**，清空全部专注。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACSkill_Solivar_GreatAxe : public UACSkillAbilityBase
{
    GENERATED_BODY()

public:
    UACSkill_Solivar_GreatAxe();

    /**
     * 伤害倍率：`Tier(2.f)` 的四个档位都是 2.0（`ACBattleContentDefinitions.cpp:99-107`
     * 的 `Tier()` 把 C/B/A/S 全设成同一个值），因此这是一个与档位无关的常量 200%。
     */
    static constexpr float DamageMultiplier = 2.f;

protected:
    virtual void ApplySkillEffects(const TArray<FUnitId>& Targets) override;
};

/**
 * 守望（`SK_Ironwall_Guard`）：为**自身**叠加一个 200 点、持续 6 秒的护盾，清空全部专注。
 *
 * 逐项对照：
 *   | 项            | 内容值                                     | 本类                                    |
 *   | ------------- | ------------------------------------------ | --------------------------------------- |
 *   | `SkillId`     | `SK_Ironwall_Guard`（:648）                 | `SkillId`                               |
 *   | `CastType`    | `Instant`（:650）                           | 继承基类                                 |
 *   | `CostMode`    | `ClearAll`（:651）                          | `CostGameplayEffectClass = UACGE_Cost_ClearAll` |
 *   | 目标          | `Self`（:652）                              | `GetTargetSelector` → `Self`             |
 *   | 效果          | `ApplyShield(200, 6s)`（:465-468）           | `UACGE_Ironwall_Guard_Shield` + `Data.DurationSeconds = 6` |
 *   | `PresentationCueId` | 未配（:647-653 没有这一行）           | 留空                                     |
 */
UCLASS()
class AUTOCHESSBATTLE_API UACSkill_Ironwall_Guard : public UACSkillAbilityBase
{
    GENERATED_BODY()

public:
    UACSkill_Ironwall_Guard();

    /** 内容里写死的护盾时长（秒）；必须**同时**填进 spec 的 `Data.DurationSeconds`，理由见 .cpp。 */
    static constexpr float ShieldDurationSeconds = 6.f;

    virtual EACSelectorType GetTargetSelector(const AACBattleUnitBase& Caster) const override;

protected:
    virtual void ApplySkillEffects(const TArray<FUnitId>& Targets) override;
};
