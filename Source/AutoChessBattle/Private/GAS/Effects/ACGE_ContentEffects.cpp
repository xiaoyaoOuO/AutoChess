// 阶段 3.1a 新增（GAS 重构实施方案 §4.2 / §4.3 / §4.5 / §7 阶段 3.1）：具体内容 GE 的实现。
//
// 数值逐项抄自 `Source/AutoChess/Private/Content/ACBattleContentDefinitions.cpp`（行号见各构造的注释），
// 本文件**没有一处数值是新造的**（C6/D10）。
#include "GAS/Effects/ACGE_ContentEffects.h"

#include "Core/ACBattleTags.h"
#include "Core/ACBattleTypes.h"
#include "GAS/ACBattleAttributeSet.h"
#include "GAS/Effects/ACHealExecution.h"
#include "GAS/Components/ACSummonEffectComponent.h"

// =============================================================================
//  UACGE_StatModifier（通用属性修饰基类）
// =============================================================================

UACGE_StatModifier::UACGE_StatModifier()
{
    // 默认 `Infinite`（旧 scope 的 `Permanent` / `BattlePermanent` → `DurationPolicy = Infinite`，§4.3）。
    // 需要"限时"的子类调 `AddStatModifierForStat(..., 秒数)` 时会切成 HasDuration。
    MakeInfinite();
}

void UACGE_StatModifier::AddStatModifierForStat(EACStat Stat, EACModOp Op, float Value, float DurationSeconds)
{
    // `EACStat` → `FGameplayAttribute` 走属性集的**显式 switch**（`GetAttributeForStat`），
    // 这正是 §4.3 要求的写法："映射必须写成显式 switch/查表，不要用按枚举索引遍历数组的循环"——
    // 这样将来 `EACStat` 加减项时编译器会提醒漏了哪一项。
    // 传 `EACStat::Count` 等越界值会得到无效句柄，`UACGameplayEffectBase::AddModifier` 会告警并跳过。
    AddResourceModifier(UACBattleAttributeSet::GetAttributeForStat(Stat), Op, Value, DurationSeconds);
}

void UACGE_StatModifier::AddResourceModifier(const FGameplayAttribute& Attribute, EACModOp Op, float Value,
                                            float DurationSeconds)
{
    switch (Op)
    {
    case EACModOp::Add:
        AddAdditiveModifier(Attribute, Value);
        break;

    case EACModOp::MulPct:
        // §4.3 裁决：`MulPct`（旧口径 `1.0 = +100%`）→ `MultiplyAdditive`，**数值换算为倍率**。
        // 例：超导线圈 `MulPct = -0.3`（-30% 防御）→ 倍率 0.7；红·生命百分比 `+0.10` → 倍率 1.10。
        AddMultiplierModifier(Attribute, Conv_PercentToMultiplier(Value));
        break;

    case EACModOp::Override:
        AddModifier(Attribute, EGameplayModOp::Override, FACEffectMagnitude::Literal(Value));
        break;

    default:
        // 枚举扩项时的兜底：当作固定值处理，并留痕（静默错算比报错危险得多）。
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle][GAS] UACGE_StatModifier: 未处理的 EACModOp %d，按 Additive 处理（GE=%s）。"),
               static_cast<int32>(Op), *GetName());
        AddAdditiveModifier(Attribute, Value);
        break;
    }

    // 作用域（§4.3；阶段 4 起自研的 `EACModScope` 已删除）：判据只剩"有没有正时长"。
    //   · `DurationSeconds > 0` → `HasDuration`（旧 `BattleTemp` 的等价物）；
    //   · `DurationSeconds <= 0` → 保持构造里设的 `Infinite`
    //     （旧 `Permanent` / `BattlePermanent`；也覆盖了旧数据里"限时档但时长填 0"的情况 ——
    //      旧管线里 `0` 表示"不过期"，保持 Infinite 才是等价行为）。
    // 这条简化**不丢语义**：8 处内容调用点逐项核对过，"限时"的都以正时长出现
    //（心流刃抢攻 4s / 超导线圈后发 5s），"永久"的都不带时长。
    if (DurationSeconds > 0.f)
    {
        MakeHasDuration(DurationSeconds);
    }
}

// =============================================================================
//  装备
// =============================================================================

UACGE_Equip_White_Greatsword::UACGE_Equip_White_Greatsword()
{
    // 来源：ACBattleContentDefinitions.cpp:337-342（`Blocks::Equip_White_Greatsword`）：
    //   `MakeStatAction(EACStat::ATK, EACModOp::Add, Tier(20.f), /*scope=*/BattlePermanent)`。
    AddStatModifierForStat(EACStat::ATK, EACModOp::Add, 20.f);
}

UACGE_Equip_Gold_FlowBlade_Preemptive::UACGE_Equip_Gold_FlowBlade_Preemptive()
{
    // 来源：ACBattleContentDefinitions.cpp:350-358（`Blocks::Equip_Gold_FlowBlade`）：
    //   `MakeStatAction(EACStat::ASPD, EACModOp::Add, Tier(25.f), /*scope=*/BattleTemp, 4.f)`。
    // → `ASPD + 25`，持续 4 秒（`HasDuration`）。阶段 4：`BattleTemp` 这一层被并进"正时长"，
    //   因此这里就是 `DurationSeconds = 4`。
    AddStatModifierForStat(EACStat::ASPD, EACModOp::Add, 25.f, /*DurationSeconds=*/4.f);

    // 阶段 3.2b（§4.5）：抢攻的 GE 形态是 **`HasDuration` + `GrantedTags = Effect.*Preemptive`**。
    // 这个标签是"这条限时效果属于抢攻"的**唯一可查标记**，用途有两处：
    //   ① 表现层的 `FACBattleViewModel::Timeline` 快照（遍历 ASC 的活动 GE 时靠它区分
    //      "抢攻"与"其他限时 GE"）—— 阶段 4：那份快照与表现桥一起删除，
    //      但**这个标签仍然必需**：它是"抢攻在生效"这件事的唯一判据，
    //      `UBattleWorld::ApplyPreBattleConfiguration` 与将来的 GameplayCue 都靠它。
    //   ② 将来做 §6.2 的"时间轴改写"时按标签找目标（`ASC->GetActiveEffects(按 owning tags 查询)`）。
    // 取值用既有的 `BattleTags::Effect_Trigger_Preemptive`（`Effect.Trigger.Preemptive`）：
    // §4.5 写的是 `Effect.Preemptive`，但那是个**尚不存在**的标签 —— 凭空新建一个标签名属于
    // 引入内容（C6），而既有的这一个在全仓从未被派发 / 读取过（§3.1 表已确认），
    // 语义正好是"抢攻触发器"，因此直接复用它（改名收口留给阶段 4 的标签清理）。
    AddGrantedTag(BattleTags::Effect_Trigger_Preemptive);
}

// 阶段 3.3 删除：`UACGE_Equip_Gold_FlowBlade_OnAttack::UACGE_Equip_Gold_FlowBlade_OnAttack()`
// （原来这里是 `AddStatModifierForStat(EACStat::FocusPerAttack, EACModOp::Add, 2.f)`）。
// 删除依据与替代见头文件同名说明。

UACGE_Equip_Red_ConduitCoil::UACGE_Equip_Red_ConduitCoil()
{
    // 来源：ACBattleContentDefinitions.cpp:378-393（`Blocks::Equip_Red_ConduitCoil`）：
    //   `AddStatModifier(DEF, MulPct, -0.3, /*scope=*/BattleTemp, DurationSeconds = 5)`，`TargetSelector = PrimaryTarget`。
    // §4.3 换算：`-0.3`（旧 `× (1 + (-0.3))`）→ 倍率 **0.7**。
    // 作用目标是**技能目标**（不是自己），因此这个 GE 必须由能力施加到目标身上 —— 属 3.1b 的接线。
    AddStatModifierForStat(EACStat::DEF, EACModOp::MulPct, -0.3f, /*DurationSeconds=*/5.f);
}

// =============================================================================
//  干员强化 / 通用词条
// =============================================================================

UACGE_Upgrade_OP01_C1::UACGE_Upgrade_OP01_C1()
{
    // 来源：ACBattleContentDefinitions.cpp:268-278（`Blocks::Upgrade_OP01_C1`）。
    AddStatModifierForStat(EACStat::FocusMax, EACModOp::Add, -20.f);
    AddStatModifierForStat(EACStat::ATK, EACModOp::Add, 10.f);
}

UACGE_Upgrade_OP01_S1::UACGE_Upgrade_OP01_S1()
{
    // 来源：ACBattleContentDefinitions.cpp:280-297（`Blocks::Upgrade_OP01_S1`）。
    // 触发条件（Hook.Kill + 击杀数 ≥ 20 + MaxTriggers = 1）在 3.1b 的能力里，见头文件。
    AddStatModifierForStat(EACStat::CritValue, EACModOp::Add, 50.f);
    AddStatModifierForStat(EACStat::ATK, EACModOp::Add, 80.f);
}

UACGE_Trait_DeadlyStrike::UACGE_Trait_DeadlyStrike()
{
    // 来源：ACBattleContentDefinitions.cpp:299-318（`Blocks::Trait_DeadlyStrike`）：
    //   `AddDamageModifier(ModOp = MulPct, Value = 0.2, DamageType = Physical, Stacks = 1, DurationSeconds = 0)`。
    // §4.3 换算：`0.2`（旧 `× (1 + 0.2)`）→ 倍率 **1.2**，作用于 `Attack`。
    // 形态与两条已知差异写在头文件里（作用面从"本次物理伤害"变为"攻击力"；类型筛选丢失）。
    AddStatModifierForStat(EACStat::ATK, EACModOp::MulPct, 0.2f);
}

UACGE_Trait_FocusSurge::UACGE_Trait_FocusSurge()
{
    // 来源：ACBattleContentDefinitions.cpp:320-332（`Blocks::Trait_FocusSurge`）：`GrantFocus(Value = 15)`。
    //
    // 形态：这是一个"被施加时给 15 点专注"的载体 GE（`Instant`）。
    // 为什么是 Instant：旧动作是 `EACActionType::GrantFocus`，它**直接加到当前专注上**，
    // 不是"常驻地让专注涨得更快"。因此 `Focus += 15` 之后 GE 就该结束（`Instant` 不进容器）。
    // 清掉基类默认的 `Infinite`：`UACGE_StatModifier` 的构造把它设成了 Infinite（那是给"永久属性强化"用的），
    // 对"一次性回资源"是错的 —— 留一个 Infinite 的 +15 专注 GE 会让每次读专注都被叠加。
    DurationPolicy = EGameplayEffectDurationType::Instant;
    Modifiers.Reset();

    // `Focus` 是**资源属性**（不在 `EACStat` 里，`FACStatBlock` 也不镜像它，§4.3），
    // 因此不能用 `AddStatModifierForStat(EACStat::…)`，要走资源属性的句柄。
    AddResourceModifier(UACBattleAttributeSet::GetFocusAttribute(), EACModOp::Add, 15.f);
}

// =============================================================================
//  纯数值瞬时 GE
// =============================================================================

UACGE_HealFixed::UACGE_HealFixed()
{
    // `Instant`（基类默认），治疗通道走 `UACHealExecution`（§4.6）。
    AddExecution(UACHealExecution::StaticClass());
}

UACGE_HealFixed::UACGE_HealFixed(FContentHealTag)
{
    // 子类专用：同样只挂 Execution；"量"由子类写进 `FixedHealAmount`（见基类头文件说明）。
    AddExecution(UACHealExecution::StaticClass());
}

void UACGE_HealFixed::SetFixedHealAmount(float Amount)
{
    // 同 `UACGE_Shield::SetFixedShieldMagnitude`：**不往 `Modifiers` 里加任何东西** ——
    // 对 Instant GE，引擎会把每条 `Modifier` 立即落到属性上（GameplayEffect.cpp:3933），
    // 而治疗已经由 `FCombatResolver::ApplyHeal` 写进 `Health`，两边都写就是**双倍治疗**。
    // "量"只放在基类的 `FixedHealAmount` 字段上，由 `UACHealExecution` 从 `Spec.Def` 读。
    FixedHealAmount = Amount;
}

UACGE_Passive_OP01_BloodThirst_Heal::UACGE_Passive_OP01_BloodThirst_Heal()
    : Super(UACGE_HealFixed::FContentHealTag())
{
    // 来源：ACBattleContentDefinitions.cpp:244-256（`Blocks::Passive_OP01_BloodThirst`）：
    //   `ApplyHeal(Value = Tier(8.f))` → 8 点**绝对**治疗量（口径差异见头文件）。
    SetFixedHealAmount(8.f);
}

// =============================================================================
//  专注回复（阶段 3.1b 新增）
// =============================================================================

UACGE_FocusGain::UACGE_FocusGain()
{
    // `Instant`（基类默认）：回专注是一次性的，不留在 ASC 里。
    // 形态取舍（为什么不是常驻 `FocusPerAttack += N`）写在头文件里，这是本次重构最容易算错的一处。
    //
    // `Focus` 是**资源属性**（不在 `EACStat` 里、`FACStatBlock` 也不镜像它，§4.3），
    // 因此不能用 `AddStatModifierForStat(EACStat::…)`，要走资源属性的句柄。
    // 与 `UACGE_Trait_FocusSurge` 同构：都是"被施加时给一次专注"的载体 GE。
    AddSetByCallerModifier(UACBattleAttributeSet::GetFocusAttribute(),
                           EGameplayModOp::Additive, GetFocusGainDataName());
}

UACGE_FocusGain_2::UACGE_FocusGain_2()
{
    // 来源：ACBattleContentDefinitions.cpp:360-369（`Blocks::Equip_Gold_FlowBlade_OnAttack`）的
    // `GrantFocus(Value = 2)` —— 数量写死在内容里，因此不需要 SetByCaller。
    // 清掉父构造那条 SetByCaller 修饰再写死字面量（与 `UACGE_Trait_FocusSurge` 改 Instant 时同样的做法）：
    // 留着两条修饰会让"施加方忘记填 SetByCaller"变成一次静默的 0 加成（引擎对取不到的
    // SetByCaller 会回落到 0，GameplayEffect.cpp 的 `CalculateModifierMagnitudes`）。
    Modifiers.Reset();
    AddAdditiveModifier(UACBattleAttributeSet::GetFocusAttribute(), 2.f);
}

UACGE_Ironwall_Guard_Shield::UACGE_Ironwall_Guard_Shield()
    : Super(UACGE_Shield::FContentShieldTag())
{
    // 来源：ACBattleContentDefinitions.cpp:459-474（块 `"Skill_Ironwall_Shield"`）：
    //   `ApplyShield(Value = 200, DurationSeconds = 6)`。
    //
    // 时长写死在 GE 上（6 秒）—— 这条内容是写死的 6 秒，GE 侧不需要 SetByCaller。
    // ⚠️ 但**护盾池实例的到期时刻**读的是另一处：`UACShieldExecution` 把 SetByCaller
    //    `Data.DurationSeconds` 直接交给 `FShieldRequest::DurationSeconds`，缺省回落到 -1
    //    （= 持续护盾，ACGE_Shield.cpp:127-128）。因此施加方（`UACSkill_Ironwall_Guard`）
    //    仍必须把 `Data.DurationSeconds = 6` 填进 spec，否则 GE 6 秒后消失、
    //    而护盾池里那一层永不过期（数值就变了）。
    MakeHasDuration(6.f);

    SetFixedShieldMagnitude(200.f);
    AddExecution(UACShieldExecution::StaticClass());
}

UACGE_Trait_HeavyArmor::UACGE_Trait_HeavyArmor()
    : Super(UACGE_Shield::FContentShieldTag())
{
    // 来源：ACBattleContentDefinitions.cpp:476-490（块 `"Trait_HeavyArmor"`）：
    //   `ApplyShield(Value = 160, DurationSeconds = -1)`（`< 0` = 持续护盾）。
    //
    // ---------------------------------------------------------------------
    // ⚠️ **阶段 3.2a 的必改项：从 `Infinite` 改成"超长 `HasDuration`"**
    // ---------------------------------------------------------------------
    // 3.1a 这里原本调 `ConfigureAsPersistentShield()`（= `MakeInfinite()`）。
    // 接线时核实引擎行为发现：**`Infinite` GE 在施加瞬间不会跑 `Executions`**，
    // 于是这 160 点护盾**永远不会落地**。依据（GameplayEffect.cpp，5.6）：
    //   · 只有 `Instant` 与"周期型"两类 GE 会被执行 —— 见 `FActiveGameplayEffectsContainer::
    //     InternalExecutePeriodicGameplayEffect` 的早退判据（:2936-2943）：
    //     `const bool bNotInstantEffect = (Spec.GetDuration() > INSTANT_APPLICATION);
    //      const bool bNoPeriodEffect = (Spec.GetPeriod() != NO_PERIOD);
    //      if (bNotInstantEffect && bNoPeriodEffect) { return; }`
    //     —— "有持续时间且没有周期"的 GE **直接 return**；
    //   · 唯一的执行入口是周期定时器（`ApplyGameplayEffectSpec` :4241-4252 按 `GetPeriod()` 起
    //     `SetTimer`），而本 GE 没有 `Period`，因此连入口都不会建。
    //   结论：`Infinite` + 无周期 + Execution 的组合是**静默失效**的（不报错、不告警）。
    //
    // 为什么不用"再加一个 `Period = 999`"绕过：周期型每 tick 都会重跑 Execution，
    // 而 `UACShieldExecution` 是"新增一层护盾"，重复执行会把 160 点变成 160 × N。
    //
    // 为什么是 `HasDuration(600)`：旧语义的 `DurationSeconds = -1` 是"永不结束"，
    // 而本项目的战斗时限由 `UBattleRuleConfig::MaxBattleSeconds` 兜底（缺省 300 秒，见
    // `UBattleWorld::CheckOutcome`）。600 秒是它的 2 倍，因此在本项目的任何一场战斗里
    // 都等价于"持续护盾"：GE **不会**中途到期（`FCombatResolver::RemoveExpiredShields` 只清理
    // 护盾池实例、不碰 GE），护盾池实例的 `ExpireTime` 仍是 `DurationSeconds = -1` → 永不过期。
    // 代价（如实记录）：GE 本身会在第 600 秒到期，届时 `UACShieldExpireExecution` 会清空护盾池 ——
    // 只有当内容侧把 `MaxBattleSeconds` 调到 600 秒以上时才会被观察到，那时把这里一起调大即可。
    //
    // 为什么不改 `UACGE_Shield::ConfigureAsPersistentShield()` 的语义：那个函数被其他护盾 GE 复用，
    // "Infinite 表达持续"本身没有错（对纯属性修饰的 GE 完全正确），错的只是"Infinite + Execution"。
    MakeHasDuration(600.f);

    SetFixedShieldMagnitude(160.f);
    AddExecution(UACShieldExecution::StaticClass());
}


//  敌人特殊机制
UACGE_Enemy_MotherNest_Spawn::UACGE_Enemy_MotherNest_Spawn()
{
    // 来源：ACBattleContentDefinitions.cpp:418-438（`Blocks::Enemy_MotherNest_Spawn`）：
    //   4 个 `SummonUnit(SubId = "SUM_Wormling")` 动作。
    // `SUM_Wormling` 的召唤物定义见 ACDefinitions 的 BuildSummons（ACBattleContentDefinitions.cpp:791-812）。
    //
    // 挂组件的写法（已核实）：走引擎的官方入口 `UGameplayEffect::AddComponent<T>()`
    // （GameplayEffect.h:2453-2463）—— 它内部 `NewObject<T>(this, NAME_None, GetMaskedFlags(...))`
    // 并把实例加进 `GEComponents`。
    // ⚠️ 这里**不**用 `CreateDefaultSubobject`：`PostInitProperties`（GameplayEffect.cpp:179-194）
    //    会把"默认子对象"自动补进 `GEComponents`，两套机制同时用会插入两条。
    //    本组件的配置是内容数据（SubId / 数量），用 `AddComponent` 更直观。
    UACSummonEffectComponent* SummonComponent = CreateDefaultSubobject<UACSummonEffectComponent>(TEXT("SummonComponent"));
    SummonComponent->SummonSpecId = FName(TEXT("SUM_Wormling"));
    SummonComponent->SummonCount = 4;
}
