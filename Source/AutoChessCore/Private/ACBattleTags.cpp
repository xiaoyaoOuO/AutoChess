#include "Core/ACBattleTags.h"

// 原生 Tag 定义：名称固定，不允许复用到其他语义。
namespace BattleTags
{
    UE_DEFINE_GAMEPLAY_TAG(Hook_BattleStart,       "Hook.Battle.Start");
    UE_DEFINE_GAMEPLAY_TAG(Hook_BattleEnd,         "Hook.Battle.End");
    UE_DEFINE_GAMEPLAY_TAG(Hook_Spawn,             "Hook.Spawn");
    UE_DEFINE_GAMEPLAY_TAG(Hook_Death,             "Hook.Death");
    UE_DEFINE_GAMEPLAY_TAG(Hook_Revive,            "Hook.Revive");
    UE_DEFINE_GAMEPLAY_TAG(Hook_BeforeAttack,      "Hook.Attack.Before");
    UE_DEFINE_GAMEPLAY_TAG(Hook_AfterAttack,       "Hook.Attack.After");
    UE_DEFINE_GAMEPLAY_TAG(Hook_Hit,               "Hook.Attack.Hit");
    UE_DEFINE_GAMEPLAY_TAG(Hook_Crit,              "Hook.Attack.Crit");
    UE_DEFINE_GAMEPLAY_TAG(Hook_Kill,              "Hook.Kill");
    UE_DEFINE_GAMEPLAY_TAG(Hook_AssistKill,        "Hook.AssistKill");
    UE_DEFINE_GAMEPLAY_TAG(Hook_BeforeSkillCast,   "Hook.Skill.Before");
    UE_DEFINE_GAMEPLAY_TAG(Hook_SkillCast,         "Hook.Skill.Cast");
    UE_DEFINE_GAMEPLAY_TAG(Hook_SkillInterrupted,  "Hook.Skill.Interrupted");
    UE_DEFINE_GAMEPLAY_TAG(Hook_AllyAttack,        "Hook.Ally.Attack");
    UE_DEFINE_GAMEPLAY_TAG(Hook_AllySkillCast,     "Hook.Ally.SkillCast");
    UE_DEFINE_GAMEPLAY_TAG(Hook_EnemySkillCast,    "Hook.Enemy.SkillCast");
    UE_DEFINE_GAMEPLAY_TAG(Hook_BeforeTakeDamage,  "Hook.Damage.BeforeTake");
    UE_DEFINE_GAMEPLAY_TAG(Hook_BeforeDealDamage,  "Hook.Damage.BeforeDeal");
    UE_DEFINE_GAMEPLAY_TAG(Hook_TakeDamage,        "Hook.Damage.Take");
    UE_DEFINE_GAMEPLAY_TAG(Hook_DamageDealt,       "Hook.Damage.Dealt");
    UE_DEFINE_GAMEPLAY_TAG(Hook_ShieldBroken,      "Hook.Shield.Broken");
    UE_DEFINE_GAMEPLAY_TAG(Hook_ShieldGain,        "Hook.Shield.Gain");
    UE_DEFINE_GAMEPLAY_TAG(Hook_Heal,              "Hook.Heal");
    UE_DEFINE_GAMEPLAY_TAG(Hook_Overheal,          "Hook.Heal.Overheal");
    UE_DEFINE_GAMEPLAY_TAG(Hook_StateAdded,        "Hook.State.Added");
    UE_DEFINE_GAMEPLAY_TAG(Hook_StateRemoved,      "Hook.State.Removed");
    UE_DEFINE_GAMEPLAY_TAG(Hook_BeforeStateApply,  "Hook.State.BeforeApply");
    UE_DEFINE_GAMEPLAY_TAG(Hook_StateTick,         "Hook.State.Tick");
    UE_DEFINE_GAMEPLAY_TAG(Hook_FocusFull,         "Hook.Focus.Full");
    UE_DEFINE_GAMEPLAY_TAG(Hook_FocusChanged,      "Hook.Focus.Changed");
    UE_DEFINE_GAMEPLAY_TAG(Hook_ChargeChanged,     "Hook.Charge.Changed");
    UE_DEFINE_GAMEPLAY_TAG(Hook_MentalChanged,     "Hook.Mental.Changed");
    UE_DEFINE_GAMEPLAY_TAG(Hook_MentalBreak,       "Hook.Mental.Break");
    UE_DEFINE_GAMEPLAY_TAG(Hook_PreemptiveTrigger, "Hook.Timeline.Preemptive");
    UE_DEFINE_GAMEPLAY_TAG(Hook_PostEffectTrigger, "Hook.Timeline.PostEffect");
    // 阶段 4：`Hook_TimelineModified` 已删除（无派发者，见头文件说明）。
    UE_DEFINE_GAMEPLAY_TAG(Hook_UnitMoved,         "Hook.Unit.Moved");
    UE_DEFINE_GAMEPLAY_TAG(Hook_TargetChanged,     "Hook.Target.Changed");

    UE_DEFINE_GAMEPLAY_TAG(Unit_Class_Warrior,     "Unit.Class.Warrior");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Class_Tank,        "Unit.Class.Tank");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Class_Assassin,    "Unit.Class.Assassin");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Class_Shooter,     "Unit.Class.Shooter");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Class_Technician,  "Unit.Class.Technician");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Class_Support,     "Unit.Class.Support");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Camp_Mechanic,     "Unit.Camp.Mechanic");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Camp_Jianghu,      "Unit.Camp.Jianghu");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Kind_Operator,     "Unit.Kind.Operator");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Kind_Enemy,        "Unit.Kind.Enemy");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Kind_Summon,       "Unit.Kind.Summon");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Tag_Ranged,        "Unit.Tag.Ranged");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Tag_Melee,         "Unit.Tag.Melee");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Tag_Elite,         "Unit.Tag.Elite");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Tag_Boss,          "Unit.Tag.Boss");
    UE_DEFINE_GAMEPLAY_TAG(Unit_Tag_Marked,        "Unit.Tag.Marked");

    UE_DEFINE_GAMEPLAY_TAG(State_Bleed,            "State.Bleed");
    UE_DEFINE_GAMEPLAY_TAG(State_Wound,            "State.Wound");
    UE_DEFINE_GAMEPLAY_TAG(State_Poison,           "State.Poison");
    UE_DEFINE_GAMEPLAY_TAG(State_Burn,             "State.Burn");
    UE_DEFINE_GAMEPLAY_TAG(State_Corrosion,        "State.Corrosion");
    UE_DEFINE_GAMEPLAY_TAG(State_Freeze,           "State.Freeze");
    UE_DEFINE_GAMEPLAY_TAG(State_Shield,           "State.Shield");
    UE_DEFINE_GAMEPLAY_TAG(State_PersistentShield, "State.PersistentShield");
    UE_DEFINE_GAMEPLAY_TAG(State_StaticDisorder,   "State.StaticDisorder");
    UE_DEFINE_GAMEPLAY_TAG(State_Silence,          "State.Silence");
    UE_DEFINE_GAMEPLAY_TAG(State_Taunt,            "State.Taunt");
    UE_DEFINE_GAMEPLAY_TAG(State_Charge,           "State.Charge");
    UE_DEFINE_GAMEPLAY_TAG(State_Stun,             "State.Stun");

    UE_DEFINE_GAMEPLAY_TAG(Damage_Type_Physical,   "Damage.Type.Physical");
    UE_DEFINE_GAMEPLAY_TAG(Damage_Type_Technical,  "Damage.Type.Technical");
    UE_DEFINE_GAMEPLAY_TAG(Damage_Type_True,       "Damage.Type.True");
    UE_DEFINE_GAMEPLAY_TAG(Damage_Type_Mental,     "Damage.Type.Mental");

    // 阶段 4：抢攻判据（被 `UACGE_Equip_Gold_FlowBlade_Preemptive` 授予，见头文件说明）。
    UE_DEFINE_GAMEPLAY_TAG(Effect_Trigger_Preemptive, "Effect.Trigger.Preemptive");
}
