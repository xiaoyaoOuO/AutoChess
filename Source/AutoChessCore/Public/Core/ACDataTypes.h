// 战斗数据定义（M16）：全局规则、单位定义、普攻模式、召唤定义。
// 全部为只读数据；运行时逻辑不得写入。
//
// 阶段 3.2a（GAS 重构实施方案 §3.1 表 / §7 阶段 3.2）本文件删掉的类型：
//   `FACAbnormalStateDef` / `UAbnormalStateDefinition`  → 7 个状态 GE（`GAS/Effects/ACGE_States.*`）
//   `FACEffectAction` / `FACCondition` / `FACEffectBlock` → `UGameplayEffect` + `UGameplayAbility`
//   `UBattleEffectLibrary`                                → `UACAbilitySet`（能力 / GE 清单）
// 阶段 3.2b（技能执行线）本文件又删掉两项：
//   `FACSkillDef`                       → 技能内容写在能力类上（`UACSkillAbilityBase` 的
//                                         `SkillId` / `CostGameplayEffectClass` / 目标选择器）；
//                                         埋点的 EventTag 直接用能力上的 `SkillId`。
//   `UUnitDefinitionBase::Skill`        → 技能由 `UACAbilitySet::GrantedAbilities` 授予，
//                                         定义资产上不再挂一份只读副本（第二份真相）。
//   `UUnitDefinitionBase::PassiveEffectBlockIds` → 被动能力的唯一入口是 `UACAbilitySet`
//                                         （`GrantedAbilities`），该字段 3.2a 起就没人读了。
// 保留：`UBattleRuleConfig`、`FACAttackSegment` / `FACAttackPatternDef`（§4.1 明文保留，
//      与行动条攻速推进耦合）、`UUnitDefinitionBase` 及其派生、`FACSummonSpec` / `USummonDefinition`。
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Templates/SubclassOf.h"
#include "Core/ACBattleTypes.h"
#include "ACDataTypes.generated.h"

// 阶段 3.3：本文件里两处 `TArray<FName> xxxEffectBlockIds`（普攻段级附加效果、召唤物
// OnSpawn 效果）已同样改成 `TArray<TSubclassOf<UGameplayEffect>>`。
// 它们是同一类契约字段（"用 BlockId 指一个旧效果块"），漏改任何一处都会让
// `FACUnitSpawnRequest::Effects` / `UACBasicAttackAbility::OnHitEffects` 与内容侧对不上。
// 这两个类属 GameplayAbilities 模块（引擎），见 `AutoChessCore.Build.cs` 的依赖说明。
class UGameplayEffect;

// ---------------------------------------------------------------------------
// 全局规则配置
// ---------------------------------------------------------------------------

/** 全局战斗规则配置：时限、专注/护盾默认值、效果递归与频率上限等。 */
UCLASS(BlueprintType)
class AUTOCHESSCORE_API UBattleRuleConfig : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    // 阶段 0.5（D2 / §5.2）：`FixedDt` 与 `MaxCatchUpSteps` 已删除 ——
    // 时间来自引擎世界时间（`FACBattleTime`），既没有"逻辑固定步长"，也就没有"追赶步数上限"。

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    float MaxBattleSeconds = 300.f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    float ShooterFocusPerAttack = 5.f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    float TankFocusOnHit = 1.f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    float DefaultShieldDuration = 6.f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    float MinPhysicalDamage = 1.f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    float TechResistCap = 0.99f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    bool bCritOnShield = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    int32 MaxDamageRedirectDepth = 2;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    int32 EffectMaxDepth = 4;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    int32 EffectMaxTriggersPerTick = 256;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    int32 EffectMaxTriggersPerInstancePerTick = 8;

    /** 数据版本：随内容变更递增，写入战斗日志与结果。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Rules")
    int32 DataVersion = 1;
};

// ---------------------------------------------------------------------------
// 异常状态定义（对应 doc/tables/AbnormalStates.csv 的结构化版本）
// ---------------------------------------------------------------------------
//
// **阶段 3.2a 已整段删除**（原本是 `FACAbnormalStateDef` + `UAbnormalStateDefinition`）。
// 替代见 §4.4 映射表：一个状态 = 一个 `UGameplayEffect`
//（`StackLimitCount` / `StackDurationRefreshPolicy` / `Period` / `GrantedTags` /
//  `UACPeriodicDamageExecution`），7 种状态对应
// `Source/AutoChessBattle/Public/GAS/Effects/ACGE_States.h` 里的 7 个 GE 类。
// 数值来源不变（`doc/tables/AbnormalStates.csv`），只是承载形式从"数据表 + 自研容器"
// 变成"GE 类 + 引擎的叠加 / 周期机制"。

// ---------------------------------------------------------------------------
// 技能定义
// ---------------------------------------------------------------------------
//
// **阶段 3.2b 已整段删除**（原本是 `FACSkillDef`：`SkillId` / `DisplayName` / `CastType` /
// `WindUpSeconds` / `CostMode` / `FixedCost` / `DrainPerSecond` / `TargetSelector` / `SelectorRadius` /
// `RangeOverride` / `bRequireTargetInRange` / `EffectBlockIds` / 引导三项 / `PresentationCueId`）。
//
// 删除依据（§3.2 表 + §4.1 映射表 + §7 阶段 3.2「3 名干员的 `FACSkillDef` 段」）：
// 技能的全部内容现在由**能力类**承载（`Source/AutoChessBattle/{Public,Private}/GAS/Abilities/`）：
//   · `SkillId`              → `UACSkillAbilityBase::SkillId`（同时是施放埋点的 EventTag）；
//   · `CostMode` / 数值       → `CostGameplayEffectClass`（4 个 Cost GE，§4.1 的映射表）；
//   · `TargetSelector` / 半径 → `UACBattleAbility::GetTargetSelector` / `GetSelectorRadius` 覆写；
//   · `RangeOverride` / `bRequireTargetInRange` → `GetEffectiveRangeOverride` / `RequiresTargetInRange`；
//   · `EffectBlockIds`       → `ApplySkillEffects()` 里的 GE 施加；
//   · `CastType` / 引导三项 / `WindUpSeconds` → 引导与前摇在全仓**没有任何内容使用**
//     （已核实：`EACCastType` 的唯一写入点全部是 `Instant`），因此不产出等价物；
//     将来真出现时按 §4.1 加 `UAbilityTask_WaitDelay` / `UAbilityTask_Repeat`；
//   · `PresentationCueId`    → `UACSkillAbilityBase::PresentationCueId`（阶段 4 接 GameplayCue）。
//
// 同一批删除的还有 `UUnitDefinitionBase::Skill`（定义资产上那份只读副本）与
// `UUnitDefinitionBase::PassiveEffectBlockIds`（3.2a 起已无人读取）——
// 技能的授予链是 `UACAbilitySet` → `UACAbilitySetComponent::GrantToOwner()`，
// 定义资产只保留"五档属性 + 身份标签 + 普攻模式"。

// ---------------------------------------------------------------------------
// 普攻模式
// ---------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACAttackSegment
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    float Multiplier = 1.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    EACDamageType DamageType = EACDamageType::Physical;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    int32 HitCount = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    bool bCanCrit = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    TArray<TSubclassOf<UGameplayEffect>> OnHitEffects;
};

USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACAttackPatternDef
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    TArray<FACAttackSegment> Segments;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    bool bUseAttackSpeed = true;

    /** 不随攻速时的固定攻击间隔（秒）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    float FixedIntervalSeconds = 1.f;

    /** 攻速收益系数（科雷 0.2）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    float AttackSpeedGainScale = 1.f;

    /** 治疗型普攻（玛恩娜伯爵·蓝血）：目标为友方，回复攻击力比例。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    bool bHealAttack = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    float HealRatio = 0.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    EACTeam TargetTeam = EACTeam::Enemy;

    /** 那摩：主动靠近至指定射程。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    float PreferredRange = -1.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Attack")
    bool bCanCrit = true;
};

// ---------------------------------------------------------------------------
// 单位定义
// ---------------------------------------------------------------------------

UCLASS(BlueprintType, Abstract)
class AUTOCHESSCORE_API UUnitDefinitionBase : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Unit")
    FName DefinitionId;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Unit")
    FText DisplayName;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Unit")
    FGameplayTagContainer IdentityTags;

    /** 五档基础属性（D/C/B/A/S），每档长度 = ACStatCount。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Unit")
    TArray<FACStatBlock> StatsByLevel;

    // 阶段 3.2b 删除的两个字段（技能执行线收口）：
    //   `FACSkillDef Skill`                   → 技能内容写在能力类上（见本文件"技能定义"一节）；
    //   `TArray<FName> PassiveEffectBlockIds` → 被动内容的唯一入口是 `UACAbilitySet::GrantedAbilities`
    //     （该字段自 3.2a 起就没有任何读取点：内核改走 `GrantedAbilities` + `GrantedEffects`）。
    // 两者都不是"暂时保留"：留着它们会让"这个单位到底有哪些技能 / 被动"有两份互相矛盾的答案。

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Unit")
    FACAttackPatternDef AttackPattern;
};

/** 干员定义：在单位定义基础上增加按等级的装备槽数。 */
UCLASS(BlueprintType)
class AUTOCHESSCORE_API UOperatorDefinition : public UUnitDefinitionBase
{
    GENERATED_BODY()

public:
    /** 装备槽数量（D=1 … S=4），由 Run 层读取。UHT 不支持静态数组暴露给蓝图，故用 TArray。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Operator")
    TArray<int32> EquipSlotMaxByLevel = { 1, 2, 3, 3, 4 };

    /** 按等级取装备槽数（越界取最近有效档）。 */
    int32 GetEquipSlotMax(int32 Level) const
    {
        if (EquipSlotMaxByLevel.Num() == 0)
        {
            return 1;
        }
        return EquipSlotMaxByLevel[FMath::Clamp(Level, 0, EquipSlotMaxByLevel.Num() - 1)];
    }
};

UCLASS(BlueprintType)
class AUTOCHESSCORE_API UEnemyDefinition : public UUnitDefinitionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Enemy")
    bool bIsElite = false;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Enemy")
    bool bIsBoss = false;
};

/** 召唤物继承模式（A5 默认 Snapshot）。 */
UENUM(BlueprintType)
enum class EACSummonInheritMode : uint8
{
    Snapshot,
    Dynamic,
    FixedOverride
};

UENUM(BlueprintType)
enum class EACOwnerDeathPolicy : uint8
{
    Persist,
    Destroy,
    Transfer
};

USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACSummonSpec
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    FName DefinitionId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    EACSummonInheritMode InheritMode = EACSummonInheritMode::Snapshot;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    float InheritRatio = 1.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    float DurationSeconds = -1.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    int32 MaxAlivePerOwner = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    bool bOccupyCell = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    bool bSelectable = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    bool bCountsAsKill = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    bool bFollowOwnerTarget = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    EACOwnerDeathPolicy OwnerDeathPolicy = EACOwnerDeathPolicy::Persist;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Summon")
    TArray<TSubclassOf<UGameplayEffect>> OnSpawnEffects;
};

UCLASS(BlueprintType)
class AUTOCHESSCORE_API USummonDefinition : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Summon")
    FACSummonSpec Spec;

    /** 召唤物基础属性（FixedOverride 或作为比例基底）。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Battle|Summon")
    FACStatBlock BaseStats;
};
