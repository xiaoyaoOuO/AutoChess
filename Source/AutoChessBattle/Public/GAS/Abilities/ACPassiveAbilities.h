#pragma once

#include "CoreMinimal.h"
#include "GAS/ACBattleAbility.h"
#include "ACPassiveAbilities.generated.h"

/**
 * 被动能力的公共基类（阶段 3.1b）：唯一的职责是关掉启动时必须有目标且在射程内这道门。
 */
UCLASS(Abstract)
class AUTOCHESSBATTLE_API UACPassiveAbilityBase : public UACBattleAbility
{
    GENERATED_BODY()

public:
    UACPassiveAbilityBase();

    /** 见类注释：被动的目标在触发时刻才决定，不在激活前门控。 */
    virtual bool RequiresTargetInRange() const override { return false; }
};

/**
 * 索利瓦尔被动·嗜血：技能施放后回复自身 8 点生命，并清空自身专注。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Solivar_BloodThirst : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Solivar_BloodThirst();

    /** 治疗量由 GE 承载（`UACGE_Passive_OP01_BloodThirst_Heal` 的 `FixedHealAmount = 8`）。 */
protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};

/**
 * 索利瓦尔 S 级强化·处刑：**累计击杀满 20 后一次性**获得永久 `CritValue + 50` / `ATK + 80`。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Upgrade_OP01_S1 : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Upgrade_OP01_S1();

    /** 触发门槛（内容值 20，`ACBattleContentDefinitions.cpp:294` 的 `KillCount.ValueA`）。 */
    static constexpr int32 RequiredKills = 20;

    /** 最多触发次数（内容值 1，块定义 `MaxTriggers = 1`）。 */
    static constexpr int32 MaxTriggers = 1;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};

/**
 * 通用 B 级词条·心流：每次击杀回复 15 点专注。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Trait_FocusSurge : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Trait_FocusSurge();

    /** 每次击杀回复的专注量（内容值 15，`ACBattleContentDefinitions.cpp:326` 的 `GrantFocus.Value`）。 */
    static constexpr float FocusSurgeAmount = 15.f;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};

/**
 * 金·心流刃（常驻段）：**自己普攻之后**回 2 点专注。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Equip_FlowBlade_OnAttack : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Equip_FlowBlade_OnAttack();

    /** 每次普攻回复的专注量（内容值 2，`ACBattleContentDefinitions.cpp:363` 的 `GainFocus.Value`）。 */
    static constexpr float FlowBladeFocusGain = 2.f;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};

/**
 * 敌人·腐殖蛆：**自己死亡时**对周围 1 格内的单位施加 3 层中毒（一次）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Enemy_Worm_DeathToxin : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Enemy_Worm_DeathToxin();

    /** 中毒层数（内容值 3，`ACBattleContentDefinitions.cpp:405` 的 `Poison.Stacks`）。 */
    static constexpr int32 PoisonStacks = 3;

    /** 最多触发次数（内容值 1，块定义 `MaxTriggers = 1`，`ACBattleContentDefinitions.cpp:413`）。 */
    static constexpr int32 MaxTriggers = 1;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;

    /** `Neighbors1`（含半径 1），与内容块逐字一致。 */
    virtual EACSelectorType GetTargetSelector(const AACBattleUnitBase& Caster) const override;
    virtual int32 GetSelectorRadius(const AACBattleUnitBase& Caster) const override;
};

/**
 * 敌人·蠕虫母巢：战斗开始时召唤 4 只 `SUM_Wormling`（一次）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Enemy_MotherNest_Spawn : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Enemy_MotherNest_Spawn();

    /** 最多触发次数（内容值 1，块定义 `MaxTriggers = 1`）。 */
    static constexpr int32 MaxTriggers = 1;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};

/**
 * 敌人·巨蛭：**自己命中后**给主目标挂 2 层、5 秒的流血。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACPassive_Enemy_Leech_Bite : public UACPassiveAbilityBase
{
    GENERATED_BODY()

public:
    UACPassive_Enemy_Leech_Bite();

    /** 流血层数（内容值 2，`ACBattleContentDefinitions.cpp:447` 的 `Bleed.Stacks`）。 */
    static constexpr int32 BleedStacks = 2;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};
