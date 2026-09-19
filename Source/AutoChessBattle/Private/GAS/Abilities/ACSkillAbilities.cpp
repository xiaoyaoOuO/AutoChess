// 阶段 3.1b 新增（GAS 重构实施方案 §4.1 / §7 阶段 3.1）：2 个主动技能能力实现。
// 对照表见头文件；数值来源逐条标了 `ACBattleContentDefinitions.cpp` 的行号。
#include "GAS/Abilities/ACSkillAbilities.h"

#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "GAS/Effects/ACGE_ContentEffects.h"
#include "GAS/Effects/ACGE_Costs.h"
#include "GAS/Effects/ACGE_DamageHeal.h"
#include "GAS/Effects/ACGE_Shield.h"

// =============================================================================
//  巨斧裂体（SK_001）
// =============================================================================

UACSkill_Solivar_GreatAxe::UACSkill_Solivar_GreatAxe()
{
    // 来源：`MakeOperator_Solivar()`（ACBattleContentDefinitions.cpp:580-591）。
    SkillId = FName(TEXT("SK_001"));

    // `CostMode = EACFocusCostMode::ClearAll`（:585）→ Cost GE（§4.1 的映射表）。
    // `UACGE_Cost_ClearAll` 的幅度是 -999（"减到负数再由夹取归零"），
    // 它的"付得起"判定由基类 `UACBattleAbility::CheckCost` 覆写接管（引擎的负值拒绝会恒为 false）。
    CostGameplayEffectClass = UACGE_Cost_ClearAll::StaticClass();

    PresentationCueId = FName(TEXT("Cue_OP01_Skill"));   // :591（阶段 4 接到 GameplayCue）

    // 目标/射程全部用基类默认值：`PrimaryTarget` / 半径 1 / `RangeOverride = -1`（施法者射程）/
    // 要求目标在射程内 —— 与内容定义 :586-589 逐项一致，因此这里不写任何覆写。
    //
    // 沉默 / 眩晕的 `ActivationBlockedTags` 在 `UACSkillAbilityBase` 的构造里统一加（两个技能都要）。
}

void UACSkill_Solivar_GreatAxe::ApplySkillEffects(const TArray<FUnitId>& Targets)
{
    // 效果来源：块 `Skill_OP01_GreatAxe`（ACBattleContentDefinitions.cpp:229-237）：
    //   `MakeDamageAction(Tier(2.f), EACDamageType::Physical, bCanCrit = true)`，
    //   `TargetSelector = PrimaryTarget`。
    // 旧路径：`EACActionType::ApplyDamage`（ACEffectSystem.cpp:601-616）→
    //   `RawAmount = Value.Get(Tier) × 施法者 ATK`、`Reason = Skill`、`bCanCrit = true`、
    //   `SourceEffectBlockId = 块 ID` → `World->Combat().ApplyDamage(...)`。
    // 新路径（§4.6 的新契约）：`UACGE_InstantDamage` → `UACDamageExecution` → 同一个结算器。
    if (Targets.Num() == 0)
    {
        // 无目标不施放：`CanActivateAbility` 已经挡过一次，但目标可能在同一步内被移除。
        return;
    }

    UBattleWorld* const World = GetBattleWorld();
    AACBattleUnitBase* const Caster = GetCasterUnit();
    if (World == nullptr || Caster == nullptr)
    {
        return;
    }
    AACBattleUnitBase* const Target = World->FindUnit(Targets[0]);
    if (Target == nullptr)
    {
        return;
    }

    // `RawAmount` 的口径与旧实现逐字一致：**施法者当前攻击力 × 段倍率**（不含暴击、不含减免）。
    // 属性读的是属性集的 CurrentValue（`AACBattleUnitBase::GetStat`），因此装备/强化的 +ATK 会如实计入。
    const float RawDamage = Caster->GetStat(EACStat::ATK) * DamageMultiplier;

    // SetByCaller 键名走 `UACGE_InstantDamage::GetDamageDataName()`（= `Data.Damage`），
    // 与 `UACDamageExecution` 读取的键名是同一个常量（ACDamageExecution.cpp:166-167）。
    ApplyEffectToUnit(*Caster, *Target, UACGE_InstantDamage::StaticClass(),
                      MakeSetByCaller(UACGE_InstantDamage::GetDamageDataName(), RawDamage));

    // ⚠️ 这里**不填**自定义 `FACGameplayEffectContext`（伤害类型 / 来源块 / 双方句柄）：
    //   引擎分配 context 的类型由 `UAbilitySystemGlobals::AllocGameplayEffectContext()` 决定
    //   （AbilitySystemComponent.cpp:470-472），而本工程**没有** `UAbilitySystemGlobals` 子类、
    //   `Config/DefaultGame.ini` 也没有配 `AbilitySystemGlobalsClassName`，
    //   所以 `MakeEffectContext()` 给出的是基类 `FGameplayEffectContext`，
    //   `ACGameplayEffectContext::FromHandleConst` 会返回 nullptr。
    //   此时 `UACDamageExecution` 的降级路径（ACDamageExecution.cpp:130-146）会从 ASC 的
    //   OwnerActor 反推 Source/Target，并把 `Reason` 取默认值 `Skill`、`DamageType` 取默认值
    //   `Physical` —— 正好与本技能的口径一致，因此**行为等价**，只有 `SourceEffectBlockId`
    //   会记为空（那是日志字段，旧值 `Skill_OP01_GreatAxe`）。
    //   要补上它需要阶段 4 加 `UACAbilitySystemGlobals`（一行 `AllocGameplayEffectContext` 覆写
    //   + 一行 ini），属阶段 4 的范围，已写进 3.1b 报告。
}

// =============================================================================
//  守望（SK_Ironwall_Guard）
// =============================================================================

UACSkill_Ironwall_Guard::UACSkill_Ironwall_Guard()
{
    // 来源：`MakeOperator_Ironwall()`（ACBattleContentDefinitions.cpp:647-653）。
    SkillId = FName(TEXT("SK_Ironwall_Guard"));

    // `CostMode = EACFocusCostMode::ClearAll`（:651）→ 与巨斧裂体同一个 Cost GE。
    CostGameplayEffectClass = UACGE_Cost_ClearAll::StaticClass();

    // 内容定义里**没有** `PresentationCueId`（:647-653），因此 `SkillId` 之外什么也不填。
    // 沉默 / 眩晕门控继承基类。
}

EACSelectorType UACSkill_Ironwall_Guard::GetTargetSelector(const AACBattleUnitBase& Caster) const
{
    // 内容：`Skill.TargetSelector = EACSelectorType::Self`（ACBattleContentDefinitions.cpp:652）。
    // 因此这个技能不需要目标就能施放 —— 射程检查在 `CanActivateAbility` 里
    // 用"自己到自己"的距离 0 通过，与旧实现（`bRequireTargetInRange` 默认 true + `RangeOverride = -1`）
    // 的判定结果一致。
    return EACSelectorType::Self;
}

void UACSkill_Ironwall_Guard::ApplySkillEffects(const TArray<FUnitId>& Targets)
{
    // 效果来源：块 `"Skill_Ironwall_Shield"`（ACBattleContentDefinitions.cpp:464-474）：
    //   `ApplyShield(Value = 200, DurationSeconds = 6)`，`TargetSelector = Self`。
    // 旧路径：`EACActionType::ApplyShield`（ACEffectSystem.cpp:630-644）→
    //   `FShieldRequest{ Source, Target, Amount = 200, DurationSeconds = 6 }` → `World->Combat().ApplyShield(...)`。
    // 新路径（§4.6）：`UACGE_Ironwall_Guard_Shield` → `UACShieldExecution` → 同一个结算器。
    if (Targets.Num() == 0)
    {
        return;
    }

    UBattleWorld* const World = GetBattleWorld();
    AACBattleUnitBase* const Caster = GetCasterUnit();
    if (World == nullptr || Caster == nullptr)
    {
        return;
    }
    // `Self` 选择器解析出来的就是施法者自己（`ResolveSelectorOnOrigin` 的 `Self` 分支，
    // ACTargetingSystem.cpp:233-235），因此这里直接取 Targets[0]（护盾落在自己身上）。
    AACBattleUnitBase* const Target = World->FindUnit(Targets[0]);
    if (Target == nullptr)
    {
        return;
    }

    // ⚠️ **必须同时填 `Data.DurationSeconds = 6`**（这是阶段 3.1b 补上的一处必要参数）：
    //   `UACGE_Ironwall_Guard_Shield` 的 `MakeHasDuration(6.f)` 只管 **GE 自身**的时长；
    //   而护盾池实例的到期时刻读的是 `FShieldRequest::DurationSeconds`
    //   （`UACShieldExecution` 从 SetByCaller `Data.DurationSeconds` 取，缺省回落到 -1，
    //    见 ACGE_Shield.cpp:127-128）—— 缺了这一项，"200 点护盾"会变成**永不过期**，
    //   而 GE 侧 6 秒后照样消失，两边分叉。
    //   旧实现是把 `DurationSeconds = 6` 直接写进 `FShieldRequest` 的，因此这里是等价复刻。
    ApplyEffectToUnit(*Caster, *Target, UACGE_Ironwall_Guard_Shield::StaticClass(),
                      MakeSetByCaller(UACGE_Shield::GetDurationDataName(), ShieldDurationSeconds));
}
