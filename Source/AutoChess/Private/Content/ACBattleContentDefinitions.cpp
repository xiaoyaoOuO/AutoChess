// =============================================================================
//  内容数据定义（示例内容）——新增干员 / 敌人只需要改这一个文件。
// =============================================================================
//
// 这里的内容是**示例**，目的是让整条链路在没有 .uasset 的情况下也能跑通，同时作为
// "怎么用数据拼出一个干员"的活文档。数值取自：
//   - doc/tables/Operators.csv      （干员五档属性 / 技能 / 强化文案）
//   - doc/tables/AbnormalStates.csv （异常状态）
//   - doc/design/自走棋系统结构说明.md §5.3（蚀隧敌人）
//   - doc/design/经济系统详细设计.md §7 （装备品级）
//
// 三条改动约定。
//   1. **能力优先用类引用表达**（阶段 3.2a 起）：技能 / 被动 / 常驻效果都是
//      `UGameplayAbility` / `UGameplayEffect` 的**子类**，不在 C++ 里用数据拼效果。
//      旧约定（"每一个效果都是一个 `FACEffectBlock`，BlockId 用常量表"）随效果块系统一起废除 ——
//      拼错 BlockId 这类配置事故因此从"运行期静默失效"变成"编译期找不到类"。
//   2. **数值一律照抄**（C6/D10）：本文件只负责"哪个单位拿到哪些类"，
//      类的数值写在 `GAS/Abilities/*` 与 `GAS/Effects/*` 里，每处都标注了它抄自哪一段旧内容。
//   3. **五档属性一律用 `Stats(...)` 逐档写全**：不要用倍率生成——策划表就是逐档给的。
//      代码里做换算会让"表里的数"与"跑起来的数"对不上。
//
// 如何新增一名干员：
//   ① 写一个 `MakeOperator_XX()`，用 `MakeOperatorDefinition` 填属性 + 普攻 + 技能 Id。
//   ② 在 `BuildAbilitySets` 里登记它的能力 / GE 清单。
//   ③ 在 `BuildOperators` 里登记（附带招募品级与职业标签）。
//   ④ 无需改战斗内核、无需改 Run 层。
//
// 阶段 3.2a（GAS 重构实施方案 §3.1 表 / §7 阶段 3.2）本文件删掉的内容：
//   `Blocks` 命名空间（14 个 BlockId 常量）、`BuildEffectBlocks`（约 300 行）、
//   `BuildAbnormalStates`（7 种状态的定义表）、以及三个只服务于效果块的工厂
//   （`MakeSelfStatAction` / `MakeSelfBlock` / `MakeDamageAction` / `MakeStatAction`）。
// =============================================================================

#include "Content/ACBattleContentLibrary.h"
#include "Core/ACBattleTags.h"
#include "Run/ACRunTypes.h"
// 阶段 3.2a：能力类与 GE 类（阶段 3.1 的产出）。本文件是"内容 → 类引用"的唯一翻译处，
// 因此必须能看到这些类的完整定义。
// 阶段 3.3：这一段**就是**内容侧的效果块字段 —— 原来还要经过 `BuildEffectBlockGEs`
// 过渡映射表把 `FName` 翻成类，现在类引用直接写在下面，翻译层已删除。
#include "GAS/ACAbilitySet.h"
#include "GAS/Abilities/ACBasicAttackAbility.h"
#include "GAS/Abilities/ACPassiveAbilities.h"
#include "GAS/Abilities/ACSkillAbilities.h"
#include "GAS/Effects/ACGE_ContentEffects.h"
#include "GAS/Effects/ACGE_States.h"
#include "UObject/Package.h"     // GetTransientPackage()

// =============================================================================
//  阶段 3.3：`RunBlockIds` 命名空间（过渡映射表的键）**已整段删除**
// =============================================================================
//
// 阶段 3.2a 在这里留了一张 `FName` 效果块 Id 常量表（`RunBlockIds::Upgrade_OP01_C1` 等 9 个），
// 它们的**唯一用途**是给 `BuildEffectBlockGEs` 当映射表的键 —— 因为当时
// `FACPlayerUnitSpec` / `FACRunEquipment` / `FACRunOperator` 的效果块字段还是 `TArray<FName>`。
//
// 本阶段把这些字段改成 `TArray<TSubclassOf<UGameplayEffect>>` +
// `TArray<TSubclassOf<UGameplayAbility>>`（§3.2 表），于是：
//   · 常量表与映射表一起删除（**没有留下任何"名字 → 类"的环节**）；
//   · 内容侧现在直接引用 `UACGE_*` / `UACPassive_*` 的类，
//     "拼错 BlockId → 运行期静默失效"这类事故从此变成**编译错误**。
// 与存档兼容性的说明：这些名字从来没有写进存档（`FACRunOperator` 的字段是内存态，
// 装备池每次由本文件重建），因此删除它们不会让任何旧数据失效。

// =============================================================================
//  小工具
// =============================================================================

namespace ACBattleContent
{
    FACStatBlock MakeStatBlock(float MaxHP, float ATK, float TECH, float DEF, float RES,
                               float CritValue, float ASPD, float Range,
                               float FocusMax, float FocusInit, float FocusRegen, float FocusPerAttack)
    {
        FACStatBlock Block;
        Block.InitDefaults();
        Block.Set(EACStat::MaxHP, MaxHP);
        Block.Set(EACStat::ATK, ATK);
        Block.Set(EACStat::TECH, TECH);
        Block.Set(EACStat::DEF, DEF);
        Block.Set(EACStat::RES, RES);
        Block.Set(EACStat::CritValue, CritValue);
        Block.Set(EACStat::CritDamageBonus, 0.f);
        Block.Set(EACStat::Lifesteal, 0.f);
        Block.Set(EACStat::ASPD, ASPD);
        Block.Set(EACStat::DEFPen, 0.f);
        Block.Set(EACStat::RESPen, 0.f);
        Block.Set(EACStat::HpRegen, 0.f);
        Block.Set(EACStat::FocusMax, FocusMax);
        Block.Set(EACStat::FocusInit, FocusInit);
        Block.Set(EACStat::FocusRegen, FocusRegen);
        Block.Set(EACStat::FocusPerAttack, FocusPerAttack);
        Block.Set(EACStat::MentalMax, 100.f);
        Block.Set(EACStat::Range, Range);
        return Block;
    }

    FACTieredValue Tier(float Value)
    {
        FACTieredValue Result;
        Result.C = Value;
        Result.B = Value;
        Result.A = Value;
        Result.S = Value;
        return Result;
    }

    FACTieredValue Tiers(float S, float A, float B, float C)
    {
        FACTieredValue Result;
        Result.C = C;
        Result.B = B;
        Result.A = A;
        Result.S = S;
        return Result;
    }

    FACRunStatModifier MakeStatModifier(EACStat Stat, float Value, FName SourceId)
    {
        FACRunStatModifier Modifier;
        Modifier.Stat = Stat;
        Modifier.Op = EACModOp::Add;
        Modifier.Value = Value;
        Modifier.SourceId = SourceId;
        return Modifier;
    }

    // ⚠️ 阶段 3.2a：`MakeSelfStatAction(...)` **已删除** —— 它造的是 `FACEffectAction`
    //（随效果块系统删除的类型）。战斗内的属性修饰改由 `UACGE_StatModifier` 的子类表达
    //（`GAS/Effects/ACGE_ContentEffects.h`：`UACGE_Upgrade_OP01_C1` / `UACGE_Equip_White_Greatsword` 等）。
    // Run 层的属性修饰一直走 `FACRunStatModifier`（`MakeStatModifier`），与本函数无关。

    /**
     * 造一件装备（阶段 3.3：三个"内容"形参都从 `FName` 列表换成了类引用列表）。
     *
     * 三个数组的分工（内容作者只需要记住这一条）：
     *   · `Effects`           —— 战斗内**常驻** GE，"装上就生效"的属性类内容放这里；
     *   · `GrantedAbilities`  —— 要**授予**的被动能力，"装上之后每次 X 就 Y"的触发类内容放这里；
     *   · `TimelineGrants`    —— 抢攻 / 后发（开局或第 N 秒施加一个限时 GE）。
     * 一件装备可以只填其中一个。判据是"这条内容需不需要一个触发者"：
     * 需要（每次普攻 / 每次击杀）→ 能力；不需要（开局 +20 攻击力）→ GE。
     */
    FACRunEquipment MakeEquipment(FName EquipmentId, const FString& DisplayName, int32 Tier,
                                  const TArray<FACRunStatModifier>& StatModifiers,
                                  const TArray<TSubclassOf<UGameplayEffect>>& Effects,
                                  const TArray<TSubclassOf<UGameplayAbility>>& GrantedAbilities,
                                  const TArray<FACRunTimelineGrant>& TimelineGrants)
    {
        FACRunEquipment Equipment;
        Equipment.EquipmentId = EquipmentId;
        Equipment.DisplayName = FText::FromString(DisplayName);
        Equipment.Tier = Tier;
        Equipment.StatModifiers = StatModifiers;
        Equipment.Effects = Effects;
        Equipment.GrantedAbilities = GrantedAbilities;
        Equipment.TimelineGrants = TimelineGrants;
        Equipment.EquippedToOperatorId = NAME_None;
        return Equipment;
    }

    FACRunFragment MakeFragment(FName FragmentId, const FString& DisplayName, int32 Tier,
                                const TArray<FACRunStatModifier>& StatModifiers)
    {
        FACRunFragment Fragment;
        Fragment.FragmentId = FragmentId;
        Fragment.DisplayName = FText::FromString(DisplayName);
        Fragment.Tier = Tier;
        Fragment.StatModifiers = StatModifiers;
        return Fragment;
    }
}

namespace
{
    using namespace ACBattleContent;

    /**
     * ⚠️ **阶段 3.2a 已删除的一段**（原文在 `ACBattleContentDefinitions.cpp:184-508`，约 325 行）。
     *
     * 那里是"效果块库"：`MakeSelfBlock` / `MakeDamageAction` / `MakeStatAction` 三个工厂 +
     * `BuildEffectBlocks` 全表（14 个内容块：技能伤害 / 被动治疗清专注 / 两个强化 / 两个词条 /
     * 四件装备 / 三个敌人机制 / 铁壁与重装的护盾）。
     *
     * 删除依据（§3.1 表 + §7 阶段 3.2）：表中每一项的替代物在阶段 3.1 已经产出：
     *   | 旧块                                     | 新承载                                              |
     *   | ---------------------------------------- | --------------------------------------------------- |
     *   | `Skill_OP01_GreatAxe`（200% ATK 物理）    | `UACSkill_Solivar_GreatAxe`                         |
     *   | `Passive_OP01_BloodThirst`（治疗 8 + 清专注）| `UACPassive_Solivar_BloodThirst` + `UACGE_Passive_OP01_BloodThirst_Heal` |
     *   | `Upgrade_OP01_C1`（专注上限 -20 / ATK +10）| `UACGE_Upgrade_OP01_C1`                              |
     *   | `Upgrade_OP01_S1`（击杀 20 → 暴击 +50 / ATK +80）| `UACPassive_Upgrade_OP01_S1` + `UACGE_Upgrade_OP01_S1` |
     *   | `Trait_DeadlyStrike`（物理增伤 20%）       | `UACGE_Trait_DeadlyStrike`（形态差异见该类注释）      |
     *   | `Trait_FocusSurge`（击杀回 15 专注）       | `UACPassive_Trait_FocusSurge` + `UACGE_Trait_FocusSurge` |
     *   | `Equip_White_Greatsword`（ATK +20）        | `UACGE_Equip_White_Greatsword`                       |
     *   | `Equip_Gold_FlowBlade`（ASPD +25 / 4s）    | `UACGE_Equip_Gold_FlowBlade_Preemptive`              |
     *   | `Equip_Gold_FlowBlade_OnAttack`（普攻回 2）| `UACPassive_Equip_FlowBlade_OnAttack` + `UACGE_FocusGain_2` |
     *   | `Equip_Red_ConduitCoil`（DEF × 0.7 / 5s）  | `UACGE_Equip_Red_ConduitCoil`                        |
     *   | `Enemy_Worm_DeathToxin`（死时 3 层中毒）   | `UACPassive_Enemy_Worm_DeathToxin` + `UACGE_State_Poison` |
     *   | `Enemy_MotherNest_Spawn`（开局召唤 4 只）  | `UACPassive_Enemy_MotherNest_Spawn` + `UACGE_Enemy_MotherNest_Spawn` |
     *   | `Enemy_Leech_Bite`（命中 2 层流血 / 5s）   | `UACPassive_Enemy_Leech_Bite` + `UACGE_State_Bleed_Leech` |
     *   | `Skill_Ironwall_Shield`（护盾 200 / 6s）   | `UACSkill_Ironwall_Guard` + `UACGE_Ironwall_Guard_Shield` |
     *   | `Trait_HeavyArmor`（开局护盾 160 持续）    | `UACGE_Trait_HeavyArmor`                              |
     *
     * 数值**一字未改**（C6/D10）：每个新类都在自己的头文件里标注了它抄自本文件哪一段旧内容。
     * 下面这张表就是替代品：`BuildAbilitySets`（谁拿到什么）——
     * 阶段 3.3 起"Run 层按局内进度追加的内容"（强化 / 装备 / 词条）也直接写成类引用，
     * 写在 `BuildOperators`（强化候选）与 `BuildEquipmentPools`（装备）里，不再需要映射表。
     */

    /**
     * 造一个能力 / GE 授予清单（阶段 3.2a）。
     *
     * 为什么用 `NewObject(GetTransientPackage())` 而不是资产：与本文件其余内容一致 ——
     * 代码侧内容的目的是"没有 .uasset 也能跑通整条链路"（见 `ACBattleContentLibrary.h` 的说明），
     * 因此对象是瞬态的，由 `UACBattleContentLibrary` 与 `UBattleDataSubsystem` 双侧强引用保命。
     */
    UACAbilitySet* MakeAbilitySet(FName Description,
                                  const TArray<TSubclassOf<UGameplayAbility>>& Abilities,
                                  const TArray<TSubclassOf<UGameplayEffect>>& Effects)
    {
        UACAbilitySet* Set = NewObject<UACAbilitySet>(GetTransientPackage(),
                                                      *FString::Printf(TEXT("AbilitySet_%s"), *Description.ToString()));
        Set->GrantedAbilities = Abilities;
        Set->GrantedEffects = Effects;
        // `GrantedAbilityLevels` 留空：`UACAbilitySet::GiveTo` 对越界项按 1 处理，
        // 而本项目的能力数值都是内容常量（没有按能力等级缩放的幅度），等级 1 就是原值。
        // 显式填空数组只会让"这里本来该有个等级表"变成一个假象。
        return Set;
    }
}

// =============================================================================
//  能力 / GE 授予清单（阶段 3.2a：替代旧的"效果块 + 被动块 Id 数组"）
// =============================================================================

namespace ACBattleContent
{
void BuildAbilitySets(TMap<FName, TObjectPtr<UACAbilitySet>>& OutAbilitySets,
                      TArray<TObjectPtr<UACAbilitySet>>& OutOwned)
{
    OutAbilitySets.Reset();
    OutOwned.Reset();

    auto AddSet = [&OutAbilitySets, &OutOwned](FName DefinitionId, UACAbilitySet* Set)
    {
        if (Set == nullptr || DefinitionId.IsNone())
        {
            return;
        }
        OutAbilitySets.Add(DefinitionId, Set);
        OutOwned.Add(Set);
    };

    // 普攻能力对所有单位都成立（§4.1：普攻的"段 / 射程 / 目标"来自 `FACAttackPatternDef`，
    // 因此同一个能力类配不同攻击模式即可；这里不存在"每个单位一个普攻类"的必要）。
    const TArray<TSubclassOf<UGameplayAbility>> WithBasicAttack =
    {
        UACBasicAttackAbility::StaticClass()
    };

    // -------------------------------------------------------------------------
    // 干员
    // -------------------------------------------------------------------------

    // 索利瓦尔（OP_01）：巨斧裂体 + 嗜血被动。开局无常驻 GE。
    //   · 巨斧裂体：`Skill_OP01_GreatAxe` 块（:224-237）+ 技能定义（:580-591）
    //   · 嗜血：`Passive_OP01_BloodThirst` 块（:239-266）
    AddSet(FName(TEXT("OP_01")), MakeAbilitySet(TEXT("OP_01"),
        { UACBasicAttackAbility::StaticClass(),
          UACSkill_Solivar_GreatAxe::StaticClass(),
          UACPassive_Solivar_BloodThirst::StaticClass() },
        { }));

    // 蕾拉中尉（OP_17）：**没有主动技能**（原文：攻速固定 50 转攻击力），只有普攻。
    //   旧内容里她的 `Skill.SkillId = NAME_None`（:617-618），`CanCastSkill` 因空 SkillId 直接返回 false；
    //   新链路里"没有技能"就是"清单里没有技能能力"，语义更直白。
    AddSet(FName(TEXT("OP_17")), MakeAbilitySet(TEXT("OP_17"),
        { UACBasicAttackAbility::StaticClass() },
        { }));

    // 铁壁（OP_Tank_Ironwall）：守望（自身 200 点护盾 / 6 秒）+ 重装（开局 160 点持续护盾）。
    //   · 守望：`Skill_Ironwall_Shield` 块（:459-474）+ 技能定义（:647-653）
    //   · 重装：`Trait_HeavyArmor` 块（:476-490，`DurationSeconds = -1` = 持续护盾）
    //   重装是 `TriggerType = Hook + Hook.BattleStart + MaxTriggers = 1` 的**开局常驻**效果，
    //   因此它的承载形式就是 `GrantedEffects` 里的一个 GE —— 不需要能力：
    //   `UACGE_Trait_HeavyArmor` 的 `UACShieldExecution` 在**施加瞬间**就把 160 点护盾落进护盾池。
    //   ⚠️ 该 GE 在阶段 3.2a 从 `Infinite` 改成了"超长 `HasDuration`"：引擎**不会**为
    //      `Infinite` 且无 `Period` 的 GE 跑 `Executions`，那会让这 160 点护盾静默消失。
    //      依据与取舍写在 `ACGE_ContentEffects.cpp` 的 `UACGE_Trait_HeavyArmor` 构造函数注释里。
    AddSet(FName(TEXT("OP_Tank_Ironwall")), MakeAbilitySet(TEXT("OP_Tank_Ironwall"),
        { UACBasicAttackAbility::StaticClass(),
          UACSkill_Ironwall_Guard::StaticClass() },
        { UACGE_Trait_HeavyArmor::StaticClass() }));

    // -------------------------------------------------------------------------
    // 敌人（蚀隧）
    // -------------------------------------------------------------------------

    // 普通蠕虫：只有普攻（旧 `MakeEnemyDefinition(..., /*EffectBlockIds=*/{ })`，:753-755）。
    AddSet(FName(TEXT("EN_Worm")), MakeAbilitySet(TEXT("EN_Worm"), WithBasicAttack, { }));

    // 腐殖蛆：普攻 + 死亡毒爆（`Enemy_Worm_DeathToxin` 块，:396-416）。
    AddSet(FName(TEXT("EN_HuskGrub")), MakeAbilitySet(TEXT("EN_HuskGrub"),
        { UACBasicAttackAbility::StaticClass(),
          UACPassive_Enemy_Worm_DeathToxin::StaticClass() },
        { }));

    // 巨蛭：普攻 + 撕咬流血（`Enemy_Leech_Bite` 块，:440-457）。
    AddSet(FName(TEXT("EN_GreatLeech")), MakeAbilitySet(TEXT("EN_GreatLeech"),
        { UACBasicAttackAbility::StaticClass(),
          UACPassive_Enemy_Leech_Bite::StaticClass() },
        { }));

    // 硬化甲虫 / 巨噬蠕虫：只有普攻（旧内容里它们的 Passive 数组为空，:767-775）。
    AddSet(FName(TEXT("EN_HardShellBeetle")), MakeAbilitySet(TEXT("EN_HardShellBeetle"), WithBasicAttack, { }));
    AddSet(FName(TEXT("EN_DevourerWorm")), MakeAbilitySet(TEXT("EN_DevourerWorm"), WithBasicAttack, { }));

    // 蠕虫母巢（BOSS）：普攻 + 开局召唤 4 只 `SUM_Wormling`（`Enemy_MotherNest_Spawn` 块，:418-438）。
    //   `MaxTriggers = 1` 由能力内部的 `ConsumeTrigger` 保证（见 `UACPassiveAbilities.h` 的说明）；
    //   召唤本身由 `UACGE_Enemy_MotherNest_Spawn` 的 `UACSummonEffectComponent` 执行。
    AddSet(FName(TEXT("EN_MotherNest")), MakeAbilitySet(TEXT("EN_MotherNest"),
        { UACBasicAttackAbility::StaticClass(),
          UACPassive_Enemy_MotherNest_Spawn::StaticClass() },
        { }));

    // 召唤物（`SUM_Wormling`）：**只给普攻**，不给任何技能。
    //   ⚠️ **阶段 3.2b 修正了 3.2a 的一个决定**：3.2a 里这里刻意**不建清单**，理由是
    //   "建清单会与'召唤物要不要能放技能'这个还没裁决的问题绑在一起"。
    //   但阶段 3.2b 把行动执行从 `FAbilityExecutor::RequestAction` 换成了**能力驱动**
    //   （`UBattleWorld::ExecuteActionForUnit`：技能能力 → 普攻能力 → 移动），
    //   于是"没有清单"的后果从"不能放技能"变成了"**连普攻都不会**"——
    //   4 只蠕虫幼体会站在原地一动不动（旧实现在内核里给所有单位执行普攻，没有这个问题）。
    //   因此必须建清单：**给普攻、不给技能**，这正好也是对那个未裁决问题的回答
    //  （召唤物不放技能，但会普攻）。
    AddSet(FName(TEXT("SUM_Wormling")), MakeAbilitySet(TEXT("SUM_Wormling"), WithBasicAttack, { }));

    // ⚠️ 内容侧契约（阶段 3.2b 起是硬约束）：**每一个会被生成的单位定义都必须在上面登记**，
    //    否则它在战斗里"一个能力都没有"，表现为完全不行动。
    //    `UBattleWorld::RegisterUnit` 对"查不到清单"会打一条 Warning，便于发现漏配。
}

// 阶段 3.3：这里原来的 `BuildEffectBlockGEs(TMap<FName, TSubclassOf<UGameplayEffect>>&)`
// （约 80 行过渡映射表）**已整体删除** —— `Run` 层与 `FACBattleSetup` 的效果块字段现在是
// GE / 能力**类引用**，内容侧直接给出类，不再需要"名字 → 类"的翻译。
}
// =============================================================================
//  干员
// =============================================================================

namespace
{
    using namespace ACBattleContent;

    /**
     * 干员定义的通用装配：普攻 + 装备槽。
     *
     * **重要契约：代码侧干员定义不填 `StatsByLevel`**（属性值请看各 `MakeOperator_XX` 顶部的数据表）。
     * 原因：`AACBattleUnitBase::InitializeFromPlayerSpec` 的取值优先级：
     *     `StatsByLevel[Spec.Level]`  **优先于**  `Spec.BaseStats`
     * 只有 `Spec.BaseStats` 才是 Run 层把"等级基础值 + 锻体 + 装备 + 复活惩罚"折算后的**唯一**口径
     * （见 ACRunStatUtils::ComposeBaseStats）。若这里再填一个 StatsByLevel，就会把 Run 层辛苦算出来
     * 的成长整份丢掉——表现为"锻体/装备加了属性但打起来没变化"。
     *
     * 于是本工程的分工是：
     *   - 干员属性口径 → `ACRunStatUtils` + `ACRunConfig`（Run 层，见下方各 MakeOperator_XX 的注释表）。
     *   - 敌人属性口径 → `UEnemyDefinition::StatsByLevel`（单档，见 MakeEnemyDefinition）。
     * 若将来要做"纯资产驱动"的干员表，请先改 `AACBattleUnitBase` 的优先级或让 Run 层只读资产，再填 StatsByLevel。
     */
    /**
     * 造一个干员定义（**阶段 3.2b 起不再收 `FACSkillDef`**）。
     *
     * 技能内容的落点已经从"定义资产上的 `Skill` 字段"搬到**能力类**：
     *   · `SkillId` 在 `UACSkill_Solivar_GreatAxe` / `UACSkill_Ironwall_Guard` 的构造函数里
     *     （`SK_001` / `SK_Ironwall_Guard`，同时是施放埋点的 EventTag）；
     *   · 消耗 / 目标选择器 / 射程 / 效果全部在能力类上；
     *   · "这个干员会放什么技能"由下面 `BuildAbilitySets` 的 `GrantedAbilities` 决定。
     * 因此本函数不再有技能形参 —— 传一个只有 `SkillId` 有意义的结构体进来，
     * 只会让"技能到底按哪一份配置跑"变成需要推理的问题（两份配置随时可能分叉）。
     */
    UOperatorDefinition* MakeOperatorDefinition(FName DefinitionId, const FString& DisplayName,
                                                const FGameplayTagContainer& IdentityTags,
                                                const FACAttackPatternDef& AttackPattern,
                                                const TArray<int32>& EquipSlotMaxByLevel)
    {
        UOperatorDefinition* Definition = NewObject<UOperatorDefinition>(GetTransientPackage());
        Definition->DefinitionId = DefinitionId;
        Definition->DisplayName = FText::FromString(DisplayName);
        Definition->IdentityTags = IdentityTags;
        // 阶段 3.2b：`Definition->Skill = Skill;` 已删除（字段本身也删了，见 Core/ACDataTypes.h）。
        Definition->AttackPattern = AttackPattern;
        Definition->EquipSlotMaxByLevel = EquipSlotMaxByLevel;
        // 不填 StatsByLevel：见上方契约说明。
        return Definition;
    }

    /** 近战普攻：100% 攻击力物理伤害，射程内自动攻击。*/
    FACAttackPatternDef MakeMeleeAttackPattern(float PreferredRange = -1.f)
    {
        FACAttackSegment Segment;
        Segment.Multiplier = 1.f;
        Segment.DamageType = EACDamageType::Physical;
        Segment.HitCount = 1;
        Segment.bCanCrit = true;

        FACAttackPatternDef Pattern;
        Pattern.Segments.Add(Segment);
        Pattern.bUseAttackSpeed = true;
        Pattern.AttackSpeedGainScale = 1.f;
        Pattern.TargetTeam = EACTeam::Enemy;
        Pattern.PreferredRange = PreferredRange;
        Pattern.bCanCrit = true;
        return Pattern;
    }

    /* =========================================================================
     * 索利瓦尔（巨斧）· 战士 · 近战 · 示例干员
     * -------------------------------------------------------------------------
     * 数据来源：doc/tables/Operators.csv → OP_01。
     *
     *   档位   HP   DEF  RES  ATK  TECH  ASPD  专注回复  初始专注  专注上限
     *   D     160   10   10   30    0    80      5         0        100
     *   C     260   40   15   45    0    80      5         0        100
     *   B     380   70   20   60    0    80      5         0        100
     *   A     580  120   20   80    0    80      5         0        100
     *   S     700  150   25  110    0    80      5         0        100
     *   射程 1（近战）；FocusPerAttack 0；装备槽 D=1, C=2, B=3, A=3, S=4
     *
     * 技能：巨斧裂体 —— 对当前目标造成 200% 攻击力物理伤害
     *       → `UACSkill_Solivar_GreatAxe`（数值/时序逐项对照见该类的头文件）。
     * 被动：嗜血 —— 释放技能后回复自身生命并清空专注
     *       → `UACPassive_Solivar_BloodThirst`（本阶段已不再登记效果块 Id）。
     * C 强化：专注上限 -20、攻击力 +10 → `UACGE_Upgrade_OP01_C1`（纯属性，只需要 GE）。
     * S 强化：累计击杀 20 人后永久 +50 暴击 / +80 攻击力
     *       → **能力** `UACPassive_Upgrade_OP01_S1`（承担"击杀数 ≥ 20 + 只触发一次"）
     *         + **GE** `UACGE_Upgrade_OP01_S1`（承担那两条属性修饰）。
     *       阶段 3.3 起两者都由 `BuildOperators` 的强化候选同时给出（见那里的说明）。
     * ========================================================================= */
    UOperatorDefinition* MakeOperator_Solivar()
    {
        FGameplayTagContainer Tags;
        Tags.AddTag(BattleTags::Unit_Class_Warrior);
        Tags.AddTag(BattleTags::Unit_Kind_Operator);

        // ⚠️ 阶段 3.2b：这里原来的 `FACSkillDef Skill`（`SkillId = SK_001` / `CastType = Instant` /
        // `CostMode = ClearAll`）**已整段删除** —— 三项的等价物都在
        // `UACSkill_Solivar_GreatAxe` 上（`SkillId` + 继承 `UACSkillAbilityBase` 的同步施放与
        // `CostGameplayEffectClass = UACGE_Cost_ClearAll`），且由 `BuildAbilitySets` 授予。
        // 属性口径见上方数据表（`ACRunStatUtils` + `ACRunConfig`，定义里不填 StatsByLevel）。

        const TArray<int32> EquipSlots = { 1, 2, 3, 3, 4 };
        return MakeOperatorDefinition(FName(TEXT("OP_01")), TEXT("索利瓦尔"), Tags,
                                      MakeMeleeAttackPattern(), EquipSlots);
    }

    /* =========================================================================
     * 蕾拉中尉（炮姐）· 射手 · 远程 · 对照示例（用来验证"射手普攻回专注 5"等职业规则）
     * 数据来源：Operators.csv OP_17（此处只取关键值，用作示例）。
     * 射程 6（远程）；FocusPerAttack 0（走 A11 默认 5）；专注上限 100
     * ========================================================================= */
    UOperatorDefinition* MakeOperator_Leila()
    {
        FGameplayTagContainer Tags;
        Tags.AddTag(BattleTags::Unit_Class_Shooter);
        Tags.AddTag(BattleTags::Unit_Kind_Operator);

        // 属性口径由 Run 层持有；此处只登记本示例的 D 档参考值（便于对照）：
        //   HP 140 / ATK 35 / DEF 8 / RES 10 / ASPD 70 / 射程 6 / 专注上限 100
        // 正式数值请写进 ACRunConfig / ACRunStatUtils 所依赖的干员数值表。
        // 蕾拉没有主动技能（原文：攻速固定 50 转攻击力）—— 新链路上"没有技能"就是
        // `BuildAbilitySets` 里她的清单**不含技能能力**（旧路径靠 `SkillId` 为空来判，
        // 那个字段随 `FACSkillDef` 一起删除了）。
        FACAttackSegment Segment;
        Segment.Multiplier = 1.f;
        Segment.DamageType = EACDamageType::Physical;
        FACAttackPatternDef Pattern;
        Pattern.Segments.Add(Segment);
        Pattern.bUseAttackSpeed = true;
        Pattern.TargetTeam = EACTeam::Enemy;

        const TArray<int32> EquipSlots = { 1, 2, 3, 3, 4 };
        return MakeOperatorDefinition(FName(TEXT("OP_17")), TEXT("蕾拉中尉"), Tags,
                                      Pattern, EquipSlots);
    }

    /* =========================================================================
     * 铁壁 · 坦克 · 近战 · 对照示例（用来验证"坦克受击 +1 专注"）。
     * 数据来源：Operators.csv（坦克系，此处取示例值）
     * 技能：守望 —— 为自身叠加 200 点 / 6 秒护盾 → `UACSkill_Ironwall_Guard`。
     * 被动（词条"重装"）：开局获得 160 点**持续**护盾 → `UACGE_Trait_HeavyArmor`（常驻 GE）。
     * ========================================================================= */
    UOperatorDefinition* MakeOperator_Ironwall()
    {
        FGameplayTagContainer Tags;
        Tags.AddTag(BattleTags::Unit_Class_Tank);
        Tags.AddTag(BattleTags::Unit_Kind_Operator);

        // 属性口径由 Run 层持有；此处只登记本示例的 D 档参考值：
        //   HP 240 / ATK 20 / DEF 40 / RES 15 / ASPD 60 / 射程 1 / 专注上限 100

        // 阶段 3.2b：这里原来的 `FACSkillDef Skill`（`SK_Ironwall_Guard` / `Instant` / `ClearAll`）
        // **已删除** —— 等价物在 `UACSkill_Ironwall_Guard` 上（`SkillId` + 同步施放 +
        // `CostGameplayEffectClass = UACGE_Cost_ClearAll` + `GetTargetSelector() = Self`）。

        const TArray<int32> EquipSlots = { 1, 2, 3, 3, 4 };
        return MakeOperatorDefinition(FName(TEXT("OP_Tank_Ironwall")), TEXT("铁壁"), Tags,
                                      MakeMeleeAttackPattern(), EquipSlots);
    }
}

namespace ACBattleContent
{
void BuildOperators(TArray<FACContentOperatorEntry>& OutOperators)
{
    OutOperators.Reset();

    // ---- 索利瓦尔：主力示例干员，C 级招募品级 ----
    {
        FACContentOperatorEntry Entry;
        Entry.Definition = MakeOperator_Solivar();
        Entry.RecruitTier = 1;                                   // C
        Entry.ClassTag = BattleTags::Unit_Class_Warrior;

        // 每个等级档位（0=C,1=B,2=A,3=S）的个性强化候选项。
        // 示例只给每个档位一个选项；正式内容应为 3 选 1（见 ACRunSquad::ApplyLevelUp 的 TODO）。
        //
        // 阶段 3.3（**内容语义保真的关键一处**）：候选从 `FName` 效果块 Id 换成了
        // `FACContentUpgradeOption{ Effects, GrantedAbilities }`。
        //   · C/B/A 档："专注置换"只有一个纯属性效果 → 只填 `Effects`
        //     （旧块 `Upgrade_OP01_C1`：`FocusMax -20` + `ATK +10`，两条都是 `BattlePermanent`）。
        //   · S 档："处刑"**两个都要填**：
        //       `GrantedAbilities` → `UACPassive_Upgrade_OP01_S1`
        //           （监听 `Hook.Kill`、判 `KillsBySource(自己) >= 20`、`MaxTriggers = 1`）；
        //       `Effects`          → `UACGE_Upgrade_OP01_S1`
        //           （`CritValue + 50` + `ATK + 80`，由上面的能力在达标那一刻施加）。
        //     ⚠️ 二者缺一不可：只给 `Effects` 会退化成"选了就立刻 +50/+80"（丢掉 20 击杀门槛
        //        与一次性语义，那正是阶段 3.2a 的已知退化）；只给能力则永远等不到那条 GE。
        //     "能力先授予、GE 由能力施加"这条链路的授予点见
        //     `UACAbilitySetComponent::SetExtraAbilities` 与 `UBattleWorld::Initialize`。
        FACContentUpgradeOption FocusTrade;                                  // C/B/A：纯属性
        FocusTrade.Effects = { UACGE_Upgrade_OP01_C1::StaticClass() };

        FACContentUpgradeOption Executioner;                                 // S：门槛 + 一次性
        Executioner.Effects = { UACGE_Upgrade_OP01_S1::StaticClass() };
        Executioner.GrantedAbilities = { UACPassive_Upgrade_OP01_S1::StaticClass() };

        Entry.UpgradeChoices = {
            FocusTrade,                                               // C
            FocusTrade,                                               // B（示例复用）
            FocusTrade,                                               // A（示例复用）
            Executioner                                               // S（处刑）
        };
        OutOperators.Add(Entry);
    }

    // ---- 蕾拉中尉：D 级招募，验证射手规则 ----
    {
        FACContentOperatorEntry Entry;
        Entry.Definition = MakeOperator_Leila();
        Entry.RecruitTier = 0;                                   // D
        Entry.ClassTag = BattleTags::Unit_Class_Shooter;
        // 四个档位都是空候选（两个数组都空 = 没有强化）。用显式长度 4 的数组表示
        // "这个干员有四个档位、每档都没内容"，与"忘了填"在数据上可区分。
        Entry.UpgradeChoices.SetNum(4);
        OutOperators.Add(Entry);
    }

    // ---- 铁壁：C 级招募，验证坦克规则 ----
    {
        FACContentOperatorEntry Entry;
        Entry.Definition = MakeOperator_Ironwall();
        Entry.RecruitTier = 1;                                   // C
        Entry.ClassTag = BattleTags::Unit_Class_Tank;
        Entry.UpgradeChoices.SetNum(4);
        OutOperators.Add(Entry);
    }
}
}

// =============================================================================
//  敌人（蚀隧）· 见 doc/design/自走棋系统结构说明.md §5.3。
// =============================================================================

namespace
{
    /**
     * 敌人通用装配（阶段 3.2b：**去掉了 `EffectBlockIds` 形参**）。
     *
     * 为什么去掉：唯一的用途是写 `Definition->PassiveEffectBlockIds`，而那个字段本身已删除
     *（被动内容的唯一入口是 `BuildAbilitySets` 的 `GrantedAbilities`）。
     * ⚠️ 顺带修掉一处**已存在的编译错误**：三个调用点传的
     *    `RunBlockIds::Enemy_Worm_DeathToxin` / `Enemy_Leech_Bite` / `Enemy_MotherNest_Spawn`
     *    这三个常量在 `RunBlockIds` 命名空间里**根本没有定义**（阶段 3.2a 删常量表时漏删了调用点）。
     *    它们的语义现在由 `UACPassive_Enemy_Worm_DeathToxin` / `_Leech_Bite` / `_MotherNest_Spawn`
     *    三个能力类承担，因此删掉形参就是正确的收口（不是"为了让编译通过而删"）。
     */
    UEnemyDefinition* MakeEnemyDefinition(FName DefinitionId, const FString& DisplayName,
                                          const FACStatBlock& Stats, float Range,
                                          bool bIsElite = false, bool bIsBoss = false)
    {
        UEnemyDefinition* Definition = NewObject<UEnemyDefinition>(GetTransientPackage());
        Definition->DefinitionId = DefinitionId;
        Definition->DisplayName = FText::FromString(DisplayName);
        Definition->IdentityTags.AddTag(BattleTags::Unit_Kind_Enemy);
        Definition->StatsByLevel.Add(Stats);
        Definition->AttackPattern = MakeMeleeAttackPattern();
        Definition->AttackPattern.PreferredRange = Range;
        Definition->bIsElite = bIsElite;
        Definition->bIsBoss = bIsBoss;
        return Definition;
    }
}

namespace ACBattleContent
{
void BuildEnemies(TMap<FName, TObjectPtr<UEnemyDefinition>>& OutEnemies, TArray<TObjectPtr<UEnemyDefinition>>& OutOwned)
{
    OutEnemies.Reset();
    OutOwned.Reset();

    auto AddEnemy = [&OutEnemies, &OutOwned](UEnemyDefinition* Definition)
    {
        if (Definition == nullptr)
        {
            return;
        }
        OutEnemies.Add(Definition->DefinitionId, Definition);
        OutOwned.Add(Definition);
    };

    // 普通蠕虫：HP 85 / DEF 5 / RES 0 / ATK 15 / ASPD 100（射程 1）。
    AddEnemy(MakeEnemyDefinition(FName(TEXT("EN_Worm")), TEXT("普通蠕虫"),
        MakeStatBlock(85.f, 15.f, 0.f, 5.f, 0.f, 0.f, 100.f, 1.f, 0.f, 0.f, 0.f, 0.f), 1.f));

    // 腐殖蛆：HP 50 / ATK 10 / ASPD 70；每次攻击 +1 层中毒；死亡时对周围 1 格施加 3 层中毒。
    //   死亡毒爆的承载是 `UACPassive_Enemy_Worm_DeathToxin`（监听 `Hook.Death`，由
    //   `BuildAbilitySets` 授予）—— 阶段 3.2b 起不再往定义上挂效果块 Id
    //（那三个 `RunBlockIds::Enemy_*` 常量本来就已经不存在了，见 `MakeEnemyDefinition` 的注释）。
    AddEnemy(MakeEnemyDefinition(FName(TEXT("EN_HuskGrub")), TEXT("腐殖蛆"),
        MakeStatBlock(50.f, 10.f, 0.f, 0.f, 5.f, 0.f, 70.f, 1.f, 0.f, 0.f, 0.f, 0.f), 1.f));

    // 巨蛭：HP 200 / DEF 10 / ATK 25 / ASPD 80（精英）。撕咬流血的承载是
    // `UACPassive_Enemy_Leech_Bite`（监听 `Hook.Hit`）。
    AddEnemy(MakeEnemyDefinition(FName(TEXT("EN_GreatLeech")), TEXT("巨蛭"),
        MakeStatBlock(200.f, 25.f, 0.f, 10.f, 0.f, 0.f, 80.f, 1.f, 70.f, 0.f, 5.f, 0.f), 1.f,
        /*bIsElite=*/true));

    // 硬化甲虫：HP 150 / DEF 20 / RES 10 / ATK 25 / ASPD 55（精英）
    AddEnemy(MakeEnemyDefinition(FName(TEXT("EN_HardShellBeetle")), TEXT("硬化甲虫"),
        MakeStatBlock(150.f, 25.f, 0.f, 20.f, 10.f, 0.f, 55.f, 1.f, 100.f, 0.f, 5.f, 0.f), 1.f,
        /*bIsElite=*/true));

    // 巨噬蠕虫：HP 350 / DEF 10 / RES 10 / ATK 45 / ASPD 55（精英）
    AddEnemy(MakeEnemyDefinition(FName(TEXT("EN_DevourerWorm")), TEXT("巨噬蠕虫"),
        MakeStatBlock(350.f, 45.f, 0.f, 10.f, 10.f, 0.f, 55.f, 1.f, 100.f, 0.f, 5.f, 0.f), 1.f,
        /*bIsElite=*/true));

    // 蠕虫母巢（BOSS）：HP 1200 / DEF 20 / RES 20 / ATK 0（不自攻击，靠召唤）
    // ATK 设为 1：内核的伤害公式下 0 攻击会打出最低 1 点，不影响"靠召唤"的设计意图。
    //   开局召唤的承载是 `UACPassive_Enemy_MotherNest_Spawn`（监听 `Hook.BattleStart`，
    //   靠 `UBattleWorld::Initialize` 的**全场广播**唤醒 —— 见该处注释）。
    AddEnemy(MakeEnemyDefinition(FName(TEXT("EN_MotherNest")), TEXT("蠕虫母巢"),
        MakeStatBlock(1200.f, 1.f, 0.f, 20.f, 20.f, 0.f, 50.f, 1.f, 100.f, 0.f, 5.f, 0.f), 1.f,
        /*bIsElite=*/false, /*bIsBoss=*/true));
}
}

// =============================================================================
//  召唤物（M06）
// =============================================================================

namespace ACBattleContent
{
void BuildSummons(TMap<FName, TObjectPtr<USummonDefinition>>& OutSummons, TArray<TObjectPtr<USummonDefinition>>& OutOwned)
{
    OutSummons.Reset();
    OutOwned.Reset();

    // 蠕虫幼体：继承 owner 60% 属性、存活 60 秒、占用格子、可被选中、计入击杀。
    USummonDefinition* Wormling = NewObject<USummonDefinition>(GetTransientPackage());
    Wormling->Spec.DefinitionId = FName(TEXT("SUM_Wormling"));
    Wormling->Spec.InheritMode = EACSummonInheritMode::Snapshot;    // A5 默认：召唤瞬间快照
    Wormling->Spec.InheritRatio = 0.6f;
    Wormling->Spec.DurationSeconds = 60.f;
    Wormling->Spec.MaxAlivePerOwner = 4;
    Wormling->Spec.bOccupyCell = true;
    Wormling->Spec.bSelectable = true;
    Wormling->Spec.bCountsAsKill = true;
    Wormling->Spec.bFollowOwnerTarget = false;
    Wormling->Spec.OwnerDeathPolicy = EACOwnerDeathPolicy::Persist;
    Wormling->BaseStats = MakeStatBlock(60.f, 10.f, 0.f, 5.f, 0.f, 0.f, 100.f, 1.f, 0.f, 0.f, 0.f, 0.f);

    OutSummons.Add(Wormling->Spec.DefinitionId, Wormling);
    OutOwned.Add(Wormling);
}
}

// =============================================================================
//  异常状态（M09 数据面）——**阶段 3.2a 已整段删除**
// =============================================================================
//
// 原来这里是 `MakeStateDefinition` + `BuildAbnormalStates`（约 100 行）：
// 把 10 种状态（流血 / 中毒 / 灼烧 / 伤口 / 静电紊乱 / 腐蚀 / 沉默 / 嘲讽 / 冰冻 / 充能）
// 登记成 `UAbnormalStateDefinition`，再由 `FAbnormalStateContainer` 按 `TickInterval` 结算。
//
// 删除依据（§4.4 映射表 + §7 阶段 3.2）：一个状态 = 一个 `UGameplayEffect`，
// 数值逐项搬到了 `Source/AutoChessBattle/Public/GAS/Effects/ACGE_States.h` 的 7 个 GE 类里
//（`MaxStacks → StackLimitCount`、`StackPolicy → StackDurationRefreshPolicy`、
//  `DurationSeconds → DurationPolicy`、`TickInterval → Period`、
//  `DamagePerStack + bPercentOfMaxHP → UACPeriodicDamageExecution`、
//  `bIsDebuff / bRemovable → AssetTags`）。
//
// ⚠️ **两处已知的内容缺口**（旧表里有、新 GE 里刻意没有，属"旧代码本来就没实现"的一类）：
//   · `State_Silence` / `State_Taunt` / `State_Charge` 三个状态**没有任何内容施加过**
//     （旧表只登记了时长与叠加策略，全仓没有一处 `ApplyAbnormalState` 写这三个标签），
//     因此本阶段也**不产出**对应的 GE —— 凭空造一个没人施加的 GE 等于凭空引入内容。
//   · `State_StaticDisorder`（静电紊乱）有 GE（`UACGE_State_StaticDisorder`），
//     但同样**没有任何施加方**：它的效果（每次动作受伤害）由内核的
//     `UBattleWorld::HandleActionTriggered` 实现，而"谁给它挂上这个状态"在旧代码里就是空的。
//     因此这条机制在本阶段仍是"查得到、挂不上"的状态（与改造前一致，不是回归）。
//
// 中间那一节 `BuildAbnormalStates` 的调用点（`UACBattleContentLibrary::BuildAndRegister`
// 与 `UBattleDataSubsystem::RegisterAbnormalState`）也一并删除。


// =============================================================================
//  装备池与锻体碎片池（集市 / 锻体的内容来源）
//   数值取自《经济系统详细设计》§7.2（品级）与 §7.3（碎片）。
//   **阶段 3.3**：装备的**战斗内**效果直接写成类引用（`Effects` = 常驻 GE、
//   `GrantedAbilities` = 被动能力、`TimelineGrants` = 抢攻 / 后发），
//   过渡映射表已删除；**属性**部分仍然是 `FACRunStatModifier`
//   （由 Run 层在组 `Spec.BaseStats` 时折算，这一层从头到尾没有变过）。
// =============================================================================

namespace ACBattleContent
{
void BuildEquipmentPools(TMap<int32, FACContentEquipmentPool>& OutPools)
{
    OutPools.Reset();

    // ---- 白色（Tier 0）：纯属性件 ----
    {
        FACContentEquipmentPool Pool;

        // 巨剑：属性 `ATK + 20`（Run 层折算）+ 战斗内常驻 GE `ATK + 20`（`UACGE_Equip_White_Greatsword`）。
        // 两条都保留是**旧行为**：旧块 `Equip_White_Greatsword` 是 `Immediate` 的
        // `AddStatModifier(ATK, Add, 20, BattlePermanent)`，而装备表里另有一条同值的
        // `FACRunStatModifier`（`ACRunStatUtils` 在 Run 层折算），因此旧版本本来就是 +40 总合。
        // C6/D10：数值一律不动，两条都照抄。
        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_W_Greatsword")), TEXT("巨剑"), 0,
            { MakeStatModifier(EACStat::ATK, 20.f) },
            { UACGE_Equip_White_Greatsword::StaticClass() }));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_W_Plate")), TEXT("重甲"), 0,
            { MakeStatModifier(EACStat::MaxHP, 120.f), MakeStatModifier(EACStat::DEF, 8.f) }, {}));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_W_Battery")), TEXT("电池"), 0,
            { MakeStatModifier(EACStat::ASPD, 15.f) }, {}));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_W_Bandage")), TEXT("绷带"), 0,
            { MakeStatModifier(EACStat::MaxHP, 80.f), MakeStatModifier(EACStat::HpRegen, 2.f) }, {}));

        OutPools.Add(0, Pool);
    }

    // ---- 蓝色（Tier 1）：属性件 ----
    {
        FACContentEquipmentPool Pool;
        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_B_SingleSword")), TEXT("单手剑"), 1,
            { MakeStatModifier(EACStat::ATK, 35.f), MakeStatModifier(EACStat::ASPD, 20.f) }, {}));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_B_CrystalCharm")), TEXT("晶体护符"), 1,
            { MakeStatModifier(EACStat::TECH, 40.f), MakeStatModifier(EACStat::FocusRegen, 2.f) }, {}));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_B_SoftArmor")), TEXT("软甲"), 1,
            { MakeStatModifier(EACStat::DEF, 15.f), MakeStatModifier(EACStat::RES, 8.f) }, {}));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_B_Barbell")), TEXT("杠铃"), 1,
            { MakeStatModifier(EACStat::MaxHP, 200.f) }, {}));

        OutPools.Add(1, Pool);
    }

    // ---- 金色（Tier 2）：强机制件 ----
    {
        FACContentEquipmentPool Pool;

        // 心流刃（`EQ_G_FlowBlade`）：**两段内容，各用各的通道**（阶段 3.3 恢复点之一）。
        //
        //   ① 抢攻段（旧块 `Equip_Gold_FlowBlade`）：`ASPD + 25` 持续 4 秒。
        //      → `TimelineGrants` 里一条 `bPreemptive = true` 的条目，
        //        `Effect = UACGE_Equip_Gold_FlowBlade_Preemptive`
        //        （该 GE 是 `HasDuration(4s)` + `GrantedTags = Effect.Trigger.Preemptive`）。
        //      ⚠️ 时长写在 **GE 类**上，不在这里 —— `FACRunTimelineGrant::DurationSeconds` 已删除
        //        （它从来没有被内核读过，见 `Run/ACRunTypes.h` 的说明）。
        //   ② 常驻段（旧块 `Equip_Gold_FlowBlade_OnAttack`）：`Hook.AfterAttack` 的 `GrantFocus(2)`。
        //      → `GrantedAbilities` 里一条 `UACPassive_Equip_FlowBlade_OnAttack`
        //        （它监听 `Hook.AfterAttack`，命中时施加 `UACGE_FocusGain_2`，即 `Focus += 2`）。
        //      ⚠️ **不能**用 `UACGE_Equip_Gold_FlowBlade_OnAttack`（常驻 `FocusPerAttack += 2`）：
        //        那是 3.1b 已裁决废弃的形态 —— 它会与"射手普攻按 `FocusPerAttack` 回专注"
        //        重复计数（2 点变 4 点）。该废弃类已在本阶段删除（见报告"额外删除项"）。
        FACRunTimelineGrant FlowBladePreemptive;
        FlowBladePreemptive.Effect = UACGE_Equip_Gold_FlowBlade_Preemptive::StaticClass();
        FlowBladePreemptive.bPreemptive = true;
        // TriggerSeconds 保持 0：抢攻是 BattleStart 生效，不走"第 N 秒"那条路。

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_G_FlowBlade")), TEXT("心流刃"), 2,
            { MakeStatModifier(EACStat::ATK, 45.f) },
            /*Effects=*/{},
            /*GrantedAbilities=*/{ UACPassive_Equip_FlowBlade_OnAttack::StaticClass() },
            { FlowBladePreemptive }));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_G_BloodSucker")), TEXT("汲血者短剑"), 2,
            { MakeStatModifier(EACStat::ATK, 40.f), MakeStatModifier(EACStat::Lifesteal, 8.f) }, {}));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_G_MicroAccel")), TEXT("微型加速器"), 2,
            { MakeStatModifier(EACStat::ASPD, 40.f) }, {}));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_G_EmergencyMed")), TEXT("紧急医疗设备"), 2,
            { MakeStatModifier(EACStat::MaxHP, 320.f), MakeStatModifier(EACStat::HpRegen, 5.f) }, {}));

        OutPools.Add(2, Pool);
    }

    // ---- 红色（Tier 3）：规则改写件（真实伤害 / 无视防御 / 后发等） ----
    {
        FACContentEquipmentPool Pool;

        // 超导线圈：后发（**第 20 秒**）触发一次"目标防御 × 0.7 持续 5 秒"；
        // 常驻 +15 无视防御 / +30 攻击力（属性部分在 `StatModifiers` 里，Run 层折算）。
        //
        // 阶段 3.3（恢复点之一）：后发的**时机**由 `TriggerSeconds = 20` 表达，
        // 由 `UBattleWorld::PendingTimelineFires` 在"战斗起始世界时刻 + 20 秒"那一刻施加
        // `UACGE_Equip_Red_ConduitCoil`（`DEF × 0.7` 持续 5 秒，时长写在 GE 类上）。
        // 旧行为里这条内容的 `TargetSelector = PrimaryTarget`（施加给技能主目标），
        // 而时间轴条目的作用域是"装备持有者自己" —— 这一点与旧实现**不等价**，
        // 属阶段 3.2b 已记录的时间轴作用域简化（见报告"遗留问题"）。
        FACRunTimelineGrant ConduitPostEffect;
        ConduitPostEffect.Effect = UACGE_Equip_Red_ConduitCoil::StaticClass();
        ConduitPostEffect.bPreemptive = false;
        ConduitPostEffect.TriggerSeconds = 20.f;

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_R_ConduitCoil")), TEXT("超导线圈"), 3,
            { MakeStatModifier(EACStat::DEFPen, 15.f), MakeStatModifier(EACStat::ATK, 30.f) },
            /*Effects=*/{},                                // 不常驻注册：只在后发时刻执行一次。
            /*GrantedAbilities=*/{},
            { ConduitPostEffect }));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_R_PhantomBlade")), TEXT("幻影刃"), 3,
            { MakeStatModifier(EACStat::CritValue, 25.f), MakeStatModifier(EACStat::CritDamageBonus, 30.f) }, {}));

        Pool.Items.Add(MakeEquipment(FName(TEXT("EQ_R_EternalHeart")), TEXT("永动之心"), 3,
            { MakeStatModifier(EACStat::FocusMax, 50.f), MakeStatModifier(EACStat::FocusRegen, 8.f) }, {}));

        OutPools.Add(3, Pool);
    }
}

void BuildFragmentPools(TMap<int32, FACContentFragmentPool>& OutPools)
{
    OutPools.Reset();

    // ---- 银色（Tier 0）----
    {
        TArray<FACRunFragment>& Pool = OutPools.Add(0).Items;
        Pool.Add(MakeFragment(FName(TEXT("FR_S_ATK")), TEXT("银·攻击力+6"), 0, { MakeStatModifier(EACStat::ATK, 6.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_S_ASPD")), TEXT("银·攻击速度+10"), 0, { MakeStatModifier(EACStat::ASPD, 10.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_S_DEF")), TEXT("银·防御+8"), 0, { MakeStatModifier(EACStat::DEF, 8.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_S_RES")), TEXT("银·抗性+3"), 0, { MakeStatModifier(EACStat::RES, 3.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_S_LIFESTEAL")), TEXT("银·吸血+2"), 0, { MakeStatModifier(EACStat::Lifesteal, 2.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_S_CRIT")), TEXT("银·暴击+3"), 0, { MakeStatModifier(EACStat::CritValue, 3.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_S_FREG")), TEXT("银·专注回复+2"), 0, { MakeStatModifier(EACStat::FocusRegen, 2.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_S_FINIT")), TEXT("银·初始专注+10"), 0, { MakeStatModifier(EACStat::FocusInit, 10.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_S_DEFPEN")), TEXT("银·无视防御+2"), 0, { MakeStatModifier(EACStat::DEFPen, 2.f) }));
    }

    // ---- 金色（Tier 1）----
    {
        TArray<FACRunFragment>& Pool = OutPools.Add(1).Items;
        Pool.Add(MakeFragment(FName(TEXT("FR_G_HP")), TEXT("金·生命值+120"), 1, { MakeStatModifier(EACStat::MaxHP, 120.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_G_ATK")), TEXT("金·攻击力+15"), 1, { MakeStatModifier(EACStat::ATK, 15.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_G_ASPD")), TEXT("金·攻击速度+25"), 1, { MakeStatModifier(EACStat::ASPD, 25.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_G_DEF")), TEXT("金·防御+20"), 1, { MakeStatModifier(EACStat::DEF, 20.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_G_RES")), TEXT("金·抗性+10"), 1, { MakeStatModifier(EACStat::RES, 10.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_G_CRIT")), TEXT("金·暴击+10"), 1, { MakeStatModifier(EACStat::CritValue, 10.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_G_LIFESTEAL")), TEXT("金·吸血+5"), 1, { MakeStatModifier(EACStat::Lifesteal, 5.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_G_FREG")), TEXT("金·专注回复+5"), 1, { MakeStatModifier(EACStat::FocusRegen, 5.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_G_DEFPEN")), TEXT("金·无视防御+5"), 1, { MakeStatModifier(EACStat::DEFPen, 5.f) }));
    }

    // ---- 红色（Tier 2）----
    {
        TArray<FACRunFragment>& Pool = OutPools.Add(2).Items;
        // 百分比：MulPct 口径：1.0 = +100%。这里 0.10 = +10%。
        {
            FACRunStatModifier Modifier = MakeStatModifier(EACStat::MaxHP, 0.f);
            Modifier.Op = EACModOp::MulPct;
            Modifier.Value = 0.10f;
            Pool.Add(MakeFragment(FName(TEXT("FR_R_HP_PCT")), TEXT("红·生命值+10%"), 2, { Modifier }));
        }
        {
            FACRunStatModifier Modifier = MakeStatModifier(EACStat::ATK, 0.f);
            Modifier.Op = EACModOp::MulPct;
            Modifier.Value = 0.10f;
            Pool.Add(MakeFragment(FName(TEXT("FR_R_ATK_PCT")), TEXT("红·攻击力+10%"), 2, { Modifier }));
        }
        Pool.Add(MakeFragment(FName(TEXT("FR_R_CRITDMG")), TEXT("红·暴击伤害+25%"), 2, { MakeStatModifier(EACStat::CritDamageBonus, 25.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_R_HREGEN")), TEXT("红·生命回复+8"), 2, { MakeStatModifier(EACStat::HpRegen, 8.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_R_LIFESTEAL")), TEXT("红·吸血+10"), 2, { MakeStatModifier(EACStat::Lifesteal, 10.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_R_FOCUSONHIT")), TEXT("红·每次攻击回复专注+3"), 2, { MakeStatModifier(EACStat::FocusPerAttack, 3.f) }));
        Pool.Add(MakeFragment(FName(TEXT("FR_R_DEFPEN")), TEXT("红·无视防御+15%"), 2, { MakeStatModifier(EACStat::DEFPen, 15.f) }));
    }
}
}
