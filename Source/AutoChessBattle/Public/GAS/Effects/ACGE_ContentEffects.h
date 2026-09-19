// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 / §4.3 / §4.5 / §7 阶段 3.1）：
// **具体内容 GE**——装备 / 干员强化 / 通用词条 / 敌人特殊机制。
//
// 每个类都是"只填数值"的子类：所有行为都在基类里，因此数值改动只动一个类、不会分叉。
// 每个类的注释里都写明来源（`ACBattleContentDefinitions.cpp` 的行号 + BlockId），逐项可核对（C6/D10）。
//
// 与旧内容定义的对应关系（14 个 BlockId 全覆盖，逐个见下）：
//   - `Skill_OP01_GreatAxe`        → 技能伤害（等价的通道 GE 是 `UACGE_InstantDamage`，内容量由能力填）
//   - `Passive_OP01_BloodThirst`   → 技能后治疗 + 清空专注（触发是能力，见 3.1b）→ 本文件的 `UACGE_HealFromMaxHP`
//   - `Upgrade_OP01_C1`            → `UACGE_Upgrade_OP01_C1`
//   - `Upgrade_OP01_S1`            → `UACGE_Upgrade_OP01_S1`
//   - `Trait_DeadlyStrike`         → `UACGE_Trait_DeadlyStrike`
//   - `Trait_FocusSurge`           → `UACGE_Trait_FocusSurge`
//   - `Equip_White_Greatsword`     → `UACGE_Equip_White_Greatsword`
//   - `Equip_Gold_FlowBlade`       → `UACGE_Equip_Gold_FlowBlade_Preemptive`
//   - `Equip_Gold_FlowBlade_OnAttack` → 常驻段走 `UACPassive_Equip_FlowBlade_OnAttack` + `UACGE_FocusGain_2`
//     （阶段 3.1a 那个"常驻 `FocusPerAttack += 2`"的 GE 类已被 3.1b 裁决废弃，阶段 3.3 删除）
//   - `Equip_Red_ConduitCoil`      → `UACGE_Equip_Red_ConduitCoil`
//   - `Enemy_Worm_DeathToxin`      → 施加 `UACGE_State_Poison`（3 层；由能力在 Hook.Death 上施加）
//   - `Enemy_MotherNest_Spawn`     → `UACGE_Enemy_MotherNest_Spawn`（挂 `UACSummonEffectComponent`）
//   - `Enemy_Leech_Bite`           → 施加 `UACGE_State_Bleed_Leech`（2 层；由能力在 Hook.Hit 上施加）
//   - `Skill_Ironwall_Shield` / `Trait_HeavyArmor`（非 Blocks 常量，但是在同一函数里定义的两个块）
//                                  → `UACGE_Ironwall_Guard_Shield` / `UACGE_Trait_HeavyArmor`
//   - `Equip_Gold_FlowBlade_OnAttack` 的**触发**（Hook.Attack.After）→ 3.1b 的
//     `UACPassive_Equip_FlowBlade_OnAttack`；它施加的量走 `UACGE_FocusGain_2`（本文件，3.1b 新增）
//   - `GrantFocus` 一类"加专注"动作     → `UACGE_FocusGain`（本文件，3.1b 新增；SetByCaller `Data.FocusGain`）
#pragma once

#include "CoreMinimal.h"
#include "GAS/Effects/ACGameplayEffectBase.h"
#include "GAS/Effects/ACGE_Shield.h"
#include "ACGE_ContentEffects.generated.h"

// =============================================================================
//  通用属性修饰基类
// =============================================================================

/**
 * 属性修饰 GE 的**可复用基类**（§4.3：`EACModOp` → `EGameplayModOp` 的落地点）。
 *
 * 为什么不给每个强化/装备/词条各写一个独立的 GE 类：它们的差别**只有数值与属性**
 * （+20 攻击力 / +25 攻速 / -20 专注上限……），行为完全同构（"永久或限时地改一条属性"）。
 * 把行为收在这里，子类构造里只出现"哪个属性、什么运算、多少、多久"四件事。
 *
 * 运算映射（§4.3 映射表，逐条）：
 *   `EACModOp::Add`         → `EGameplayModOp::Additive`
 *   `EACModOp::MulPct`      → `EGameplayModOp::MultiplyAdditive`，**数值换算为倍率**
 *                             （旧口径 `1.0 = +100%` → 倍率 `1 + 百分比`，见 `Conv_PercentToMultiplier`）
 *   `EACModOp::Override`    → `EGameplayModOp::Override`
 *
 * 作用域映射（§4.3 / §4.5；**阶段 4 起自研的 `EACModScope` 已删除**）：
 *   三档 scope 收敛为 GE 的 `DurationPolicy`，判据只剩"有没有正时长"这一个：
 *   `DurationSeconds <= 0` → `Infinite`（旧 `Permanent` / `BattlePermanent`）；
 *   `DurationSeconds > 0`  → `HasDuration(DurationSeconds)`（旧 `BattleTemp`）。
 */
UCLASS(Abstract)
class AUTOCHESSBATTLE_API UACGE_StatModifier : public UACGameplayEffectBase
{
    GENERATED_BODY()

public:
    UACGE_StatModifier();

    /**
     * 加一条作用于 `EACStat` 的修饰（旧 `EACStat` → 属性集属性由
     * `UACBattleAttributeSet::GetAttributeForStat` 显式 switch 映射，§4.3 要求"不许按下标猜"）。
     *
     * @param Stat           旧属性枚举
     * @param Op             旧运算枚举（本函数内部翻译成 `EGameplayModOp`，含 MulPct 的倍率换算）
     * @param Value          `Add` / `Override` 时是数值本身；`MulPct` 时是**百分比**（`-0.3` = -30%）
     * @param DurationSeconds `> 0` = 限时（→ `HasDuration`）；`<= 0` = 永久（→ `Infinite`）。
     *                        阶段 4 起这一个形参取代了旧的 `EACModScope Scope` 参数
     *                        （"BattleTemp 才看时长"那条规则被并进"有没有正时长"）。
     */
    void AddStatModifierForStat(EACStat Stat, EACModOp Op, float Value, float DurationSeconds = 0.f);

    /** 资源属性版（不在 `EACStat` 里、`FACStatBlock` 也不镜像的那些，例如 `Focus`）。 */
    void AddResourceModifier(const FGameplayAttribute& Attribute, EACModOp Op, float Value,
                             float DurationSeconds = 0.f);
};

// =============================================================================
//  装备（4 件，来源：BuildEffectBlocks 337-394 + BuildEquipmentPools 934-1023）
// =============================================================================

/**
 * 白·巨剑：开局 `ATK + 20`（永久）。
 * 来源：`Blocks::Equip_White_Greatsword`（ACBattleContentDefinitions.cpp:337-342）：
 *   `AddStatModifier(ATK, Add, 20, BattlePermanent)`，`TriggerType = Immediate` → 开局常驻。
 * 另有 Run 层的 `FACRunStatModifier{ATK, Add, 20}`（ACBattleContentDefinitions.cpp:941-942），
 * 那部分在 Run 层结算基础属性，与本 GE 无关（本 GE 只表达战斗内那一条）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Equip_White_Greatsword : public UACGE_StatModifier
{
    GENERATED_BODY()
public:
    UACGE_Equip_White_Greatsword();
};

/**
 * 金·心流刃（抢攻段）：`ASPD + 25`，持续 4 秒。
 * 来源：`Blocks::Equip_Gold_FlowBlade`（ACBattleContentDefinitions.cpp:350-358）：
 *   `AddStatModifier(ASPD, Add, 25, BattleTemp, DurationSeconds = 4)`。
 * 时间轴侧：`FlowBladePreemptive{ EffectBlockId, bPreemptive = true, DurationSeconds = 4 }`
 * （ACBattleContentDefinitions.cpp:978-981）——"抢攻 4 秒"的时长写在两处且一致。
 * §4.5：抢攻 = GE（`HasDuration` + `GrantedTags = Effect.Preemptive` 由接线阶段补）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Equip_Gold_FlowBlade_Preemptive : public UACGE_StatModifier
{
    GENERATED_BODY()
public:
    UACGE_Equip_Gold_FlowBlade_Preemptive();
};

// 阶段 3.3 删除的类：`UACGE_Equip_Gold_FlowBlade_OnAttack`。
// 它表达的是"常驻 `FocusPerAttack += 2`"，被 3.1b 裁决废弃（会与"射手普攻按 `FocusPerAttack`
// 回专注"重复计数，2 点变 4 点，见下面 `UACGE_FocusGain` 的注释），
// 且**从来没有被任何内容引用**（3.2a 的过渡映射表刻意不给它映射）。
// 阶段 3.3 把"金·心流刃常驻段"正式接到 `UACPassive_Equip_FlowBlade_OnAttack` +
// `UACGE_FocusGain_2` 之后，它就成了唯一的"零引用 + 明确废弃"的类 —— 留着它只是给
// "把它重新映射回去"留了一个陷阱，因此删除（依据：3.1b / 3.2a 两份报告的遗留项）。

/**
 * 红·超导线圈（后发段）：后发（20 秒）触发时，目标 `DEF × (1 - 0.3)`，持续 5 秒。
 * 来源：`Blocks::Equip_Red_ConduitCoil`（ACBattleContentDefinitions.cpp:378-393）：
 *   `AddStatModifier(DEF, MulPct, -0.3, BattleTemp, DurationSeconds = 5)`，
 *   `TargetSelector = PrimaryTarget`，`MaxTriggers = 1`；
 *   时间轴侧 `ConduitPostEffect{ bPreemptive = false, TriggerSeconds = 20 }`
 *   （ACBattleContentDefinitions.cpp:1005-1008）。
 *
 * **百分比 ↔ 倍率换算（§4.3 裁决，本类是最容易写错的一处）**：
 *   旧 `MulPct = -0.3` 的作用形式是 `DEF * (1 + (-0.3)) = DEF * 0.7`（ACRunStatUtils.cpp:50）；
 *   GAS 的 `MultiplyAdditive` 直接吃倍率，因此 GE 上写的是 **0.7**（`1 + (-0.3)`），
 *   即"目标 DEF 乘 0.7" = "-30% 防御"。写成 `-0.3` 会得到"DEF × -0.3"（负防御），完全不是原意。
 *   换算由 `Conv_PercentToMultiplier` 一处完成，子类不自己写 `1 + x`。
 *
 * 常驻段（`DEFPen + 15`、`ATK + 30`，ACBattleContentDefinitions.cpp:1010-1013）在
 * `FACRunEquipment::StatModifiers` 里，属 **Run 层**（改基础属性），**没有**战斗内块 ——
 * 与源文件注释"属性部分在 StatModifiers 里"一致，因此本阶段不产出对应的 GE。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Equip_Red_ConduitCoil : public UACGE_StatModifier
{
    GENERATED_BODY()
public:
    UACGE_Equip_Red_ConduitCoil();
};

// =============================================================================
//  干员强化与通用词条（来源：BuildEffectBlocks 268-332）
// =============================================================================

/**
 * 索利瓦尔 C 级强化：`FocusMax - 20` + `ATK + 10`（永久）。
 * 来源：`Blocks::Upgrade_OP01_C1`（ACBattleContentDefinitions.cpp:268-278）：
 *   `AddStatModifier(FocusMax, Add, -20, BattlePermanent)` + `AddStatModifier(ATK, Add, 10, BattlePermanent)`，
 *   `TriggerType = Immediate` → 开局立即执行一次。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Upgrade_OP01_C1 : public UACGE_StatModifier
{
    GENERATED_BODY()
public:
    UACGE_Upgrade_OP01_C1();
};

/**
 * 索利瓦尔 S 级强化"处刑"：累计击杀 20 → 永久 `CritValue + 50` + `ATK + 80`。
 * 来源：`Blocks::Upgrade_OP01_S1`（ACBattleContentDefinitions.cpp:280-297）：
 *   `AddStatModifier(CritValue, Add, 50, BattlePermanent)` + `AddStatModifier(ATK, Add, 80, BattlePermanent)`，
 *   `TriggerType = Hook`、`TriggerTag = Hook.Kill`、`MaxTriggers = 1`、
 *   条件 `KillCount >= 20`（`EACConditionType::KillCount`，ValueA = 20）。
 *
 * ⚠️ **触发条件不在这里**：`Hook.Kill` 订阅 + "击杀数 ≥ 20"是**能力**（3.1b 的被动能力 +
 * `UGameplayEffectCustomApplicationRequirement` 或能力内判定，§4.2 映射表）；
 * 本 GE 只表达"被施加时永久 +50 暴击 / +80 攻击力"这一部分。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Upgrade_OP01_S1 : public UACGE_StatModifier
{
    GENERATED_BODY()
public:
    UACGE_Upgrade_OP01_S1();
};

/**
 * 通用 A 级词条"处刑人"：对生命值低于 30% 的敌人额外造成 20% 伤害。
 * 来源：`Blocks::Trait_DeadlyStrike`（ACBattleContentDefinitions.cpp:299-318）：
 *   `AddDamageModifier`，`ModOp = MulPct`，`Value = 0.2`，`DamageType = Physical`，
 *   `Stacks = 1`（> 0 = 增伤），`DurationSeconds = 0`（战斗内永久）。
 *
 * **本类的形态与其局限（已被"禁止新增字段"约束逼出来的近似，见报告遗留项）**：
 *   旧系统的增伤走 `FCombatResolver` 的 `DamageModifiers` 表（`FACDamageModifier`），
 *   而**那张表与两个注册函数已在阶段 2 删除**（§3.1 表 / §7 阶段 2）。因此"只对物理伤害生效的 +20%"
 *   在当前内核里**没有任何承载体**。本类的做法是把它表达成
 *   **一条作用于 `Attack` 的 `MultiplyAdditive = 1.2` 修饰**（= 攻击力 +20%），
 *   再由 3.1b 的能力把"目标生命 < 30%"作为**施加条件**（旧代码里那条 30% 阈值也没实现，
 *   见 ACBattleContentDefinitions.cpp:300 的注释 —— 它写的是"对生命值低于 30% 的敌人额外造成 20% 伤害"，
 *   但块里既没有 `HPPercentBelow` 条件、也没有目标选择器，`TargetSelector = Self`）。
 *
 *   两条**已知差异**，必须写进对照表：
 *     ① 作用面：旧 = "本次物理伤害 × 1.2"；新 = "攻击力 × 1.2"（对技术伤害也会生效、且不改变伤害类型筛选）；
 *     ② 触发：旧 = 开局 Immediate 无条件生效；新 = 同样开局生效，但是否加"目标生命 < 30%"的动态条件
 *        取决于 3.1b 的能力实现。
 *   数值 1.2 = `1 + 0.2`，与旧 `MulPct = 0.2`（`× (1 + 0.2)`）在"作用面一致时"逐字等价。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Trait_DeadlyStrike : public UACGE_StatModifier
{
    GENERATED_BODY()
public:
    UACGE_Trait_DeadlyStrike();
};

/**
 * 通用 B 级词条"心流"：每次击杀回复 15 点专注。
 * 来源：`Blocks::Trait_FocusSurge`（ACBattleContentDefinitions.cpp:320-332）：
 *   `GrantFocus(15)`，`TriggerType = Hook`、`TriggerTag = Hook.Kill`。
 *
 * ⚠️ 与"心流刃常驻"同构的取舍：本 GE 是"击杀后施加一次 `Instant` 的专注回复"用的**载体 GE**
 *   （`Focus += 15`）。触发（`Hook.Kill`）在 3.1b 的能力里。
 *   为什么不做成"常驻 GE + 事件监听"：GE 本身没有"监听事件"的能力（那是 `UGameplayAbility::AbilityTriggers`
 *   的职责），所以"击杀回专注"必然拆成"能力（触发）+ GE（改值）"两半，这正是 §4.2 的映射方向。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Trait_FocusSurge : public UACGE_StatModifier
{
    GENERATED_BODY()
public:
    UACGE_Trait_FocusSurge();
};

// =============================================================================
//  纯数值瞬时 GE（能力施加，量在 GE 上写死）
// =============================================================================

/**
 * "按最大生命百分比回复"的瞬时治疗 GE（供 `Passive_OP01_BloodThirst` 用）。
 * 来源：`Blocks::Passive_OP01_BloodThirst`（ACBattleContentDefinitions.cpp:239-266）：
 *   `ApplyHeal(Value = 8)`，注释写"固定治疗量（示例：约等于 6~10% 最大生命）"。
 *
 * ⚠️ **口径差异（必须在报告里写明）**：旧块的治疗是 `ApplyHeal(8)` =
 *   `FCombatResolver::ApplyHeal(RawAmount = 8)` —— 一个**绝对值 8**，不是百分比。
 *   注释里的"8%"是内容作者的意图，代码里没实现成百分比（`ApplyHeal` 的 `Value` 直接当 `RawAmount`）。
 *   本阶段**严格遵守 D10（数值不动）**，因此本 GE 用**绝对值 8**：
 *   `SetByCaller` 键名 `Data.Heal`，值由施加方填，`UACGE_HealFixed` 的子类把 8 写成 `Modifiers[0]`。
 *   若策划确认要"8% 最大生命"，那是**数值变更**，需要单独裁决（见报告遗留项）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_HealFixed : public UACGameplayEffectBase
{
    GENERATED_BODY()

public:
    UACGE_HealFixed();

protected:
    /** 受保护构造：供子类指定固定治疗量（写进 `Modifiers[0]`，供 `UACHealExecution` 兜底读取）。 */
    struct FContentHealTag {};
    explicit UACGE_HealFixed(FContentHealTag);

public:
    /** 治疗 8 点（`Passive_OP01_BloodThirst` 的 `ApplyHeal(8)`）。 */
    void SetFixedHealAmount(float Amount);
};

/**
 * 索利瓦尔被动"嗜血"的治疗段：回复 8 点生命。
 * 见 `UACGE_HealFixed` 的口径说明；"技能后触发"与"清空专注"分别由 3.1b 的能力与 `UACGE_Cost_ClearAll` 承担。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Passive_OP01_BloodThirst_Heal : public UACGE_HealFixed
{
    GENERATED_BODY()
public:
    UACGE_Passive_OP01_BloodThirst_Heal();
};

/**
 * 专注回复的**瞬时通道 GE**（阶段 3.1b 新增）：`Focus += Data.FocusGain`。
 *
 * 来源（旧的"加专注"到底有几条路，这里一次说清）：
 *   - `EACActionType::GrantFocus`（`ACEffectSystem.cpp:706-708`）→ 直接调
 *     `FAbilityExecutor::GrantFocus(UnitId, Value.Get(Tier))`；
 *   - `FAbilityExecutor::OnBasicAttackHit`（`ACAbilityExecutor.cpp:637-651`）：射手普攻回 `FocusPerAttack`；
 *   - `FAbilityExecutor::OnTakeHit`（同文件 :653-663）：坦克受击回 `TankFocusOnHit`。
 *
 * ⚠️ **为什么形态是 Instant，而不是常驻 `FocusPerAttack`**（阶段 3.1b 的裁决）：
 *   "每次普攻后 `Focus += 2`"（金·心流刃）与"射手普攻按 `FocusPerAttack` 回专注"是
 *   **两条互相独立**的加专注路径。若把前者做成常驻 `FocusPerAttack += 2`，
 *   普攻能力读属性集 `FocusPerAttack` 时会把装备那 2 点一起读进来，
 *   于是同一次普攻既走了"装备块"又走了"职业规则" —— **回 4 点**（旧版本是 2 点）。
 *   Instant 形态（施加一次、加一次）与旧 `GrantFocus(2)` 逐字等价，且不会污染属性口径。
 *   ⇒ 阶段 3.1a 的 `UACGE_Equip_Gold_FlowBlade_OnAttack`（常驻 `FocusPerAttack += 2`）
 *     在阶段 3.3 **已删除**（本类 + `UACPassive_Equip_FlowBlade_OnAttack` 是这条内容的唯一承载）。
 *
 * 量与符号：由施加方在 spec 上填（可正可负）。回收专注用本类，扣专注请用 `ACGE_Costs.h` 的 Cost GE。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_FocusGain : public UACGameplayEffectBase
{
    GENERATED_BODY()

public:
    UACGE_FocusGain();

    /** SetByCaller 键名（唯一常量处，避免字符串散落）。 */
    static FName GetFocusGainDataName() { return FName(TEXT("Data.FocusGain")); }
};

/**
 * 金·心流刃（常驻段）：普攻命中时回 **2** 点专注（`Focus += 2`，Instant）。
 * 来源：`Blocks::Equip_Gold_FlowBlade_OnAttack`（ACBattleContentDefinitions.cpp:360-369）：
 *   `GrantFocus(Value = 2)`，`TriggerType = Hook`、`TriggerTag = Hook.Attack.After`。
 * 触发在 `UACPassive_Equip_FlowBlade_OnAttack`（`Hook.Attack.After`），本类只负责"加 2 点"。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_FocusGain_2 : public UACGE_FocusGain
{
    GENERATED_BODY()
public:
    UACGE_FocusGain_2();
};

/**
 * 铁壁技能"守望"的护盾：固定 200 点，持续 6 秒。
 * 来源：`Blocks::"Skill_Ironwall_Shield"`（ACBattleContentDefinitions.cpp:459-474）：
 *   `ApplyShield(Value = 200, DurationSeconds = 6)`。
 *
 * 时长走 `MakeHasDuration(6.f)`（GE 自身的时长），但**施加方还必须把
 * `Data.DurationSeconds = 6` 填进 spec**：护盾池实例的 `ExpireTime` 读的是
 * `FShieldRequest::DurationSeconds`，而 `UACShieldExecution` 在 SetByCaller 缺省时
 * 回落到 `-1`（= 持续护盾，ACGE_Shield.cpp:127-128）——
 * 只写 GE 侧时长会让"200 点护盾"变成永不过期（阶段 3.1b 已从能力侧填上这一项，见报告）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Ironwall_Guard_Shield : public UACGE_Shield
{
    GENERATED_BODY()
public:
    UACGE_Ironwall_Guard_Shield();
};

/**
 * 铁壁被动"重装"（词条）：开局获得 160 点**持续护盾**（无时限）。
 * 来源：`Blocks::"Trait_HeavyArmor"`（ACBattleContentDefinitions.cpp:476-490）：
 *   `ApplyShield(Value = 160, DurationSeconds = -1)`，注释"< 0 = 持续护盾（CombatResolver 约定）"，
 *   `TriggerType = Hook` + `Hook.BattleStart`、`MaxTriggers = 1`（开局派发一次）。
 * 时长：`DurationSeconds = -1` → GE 用 `Infinite`，同时把 `-1` 透传给结算器（两条判据一致）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Trait_HeavyArmor : public UACGE_Shield
{
    GENERATED_BODY()
public:
    UACGE_Trait_HeavyArmor();
};

// =============================================================================
//  敌人特殊机制（来源：BuildEffectBlocks 396-457）
// =============================================================================

/**
 * 敌人·蠕虫母巢：战斗开始时召唤 4 只 `SUM_Wormling`。
 * 来源：`Blocks::Enemy_MotherNest_Spawn`（ACBattleContentDefinitions.cpp:418-438）：
 *   4 个 `SummonUnit(SubId = "SUM_Wormling")` 动作，`TriggerType = Hook`、`Hook.BattleStart`、`MaxTriggers = 1`。
 *
 * 实现：`Instant` GE + `UACSummonEffectComponent`（§4.2：`SummonUnit` → `UACSummonEffectComponent`）。
 * "4 只"由组件的 `SummonCount = 4` 表达（旧代码也是"同一个 SubId 执行 4 次"）。
 * 触发（`Hook.BattleStart`）由 3.1b 的能力/内核接线，本 GE 只管"被施加时召 4 只"。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_Enemy_MotherNest_Spawn : public UACGameplayEffectBase
{
    GENERATED_BODY()
public:
    UACGE_Enemy_MotherNest_Spawn();
};
