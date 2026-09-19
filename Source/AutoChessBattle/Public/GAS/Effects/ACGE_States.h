// 阶段 3.1a 新增（GAS 重构实施方案 §4.4 / §7 阶段 3.1）：**7 种异常状态的 GE**。
//
// §4.4 映射表的落地方式（逐行对应，数值取自 `BuildAbnormalStates`：
// ACBattleContentDefinitions.cpp:849-925 与 doc/tables/AbnormalStates.csv）：
//
//   | 旧（FACAbnormalStateDef）        | 新（GE）                                            |
//   | ------------------------------- | --------------------------------------------------- |
//   | `StateTag`                      | `GrantedTags`（授予目标，驱散/判定按它查）              |
//   | `MaxStacks`                     | `StackLimitCount`（`0` = 无上限）                     |
//   | `StackPolicy = StackOnly`       | `NeverRefresh`（层数累加、时长不刷新）                  |
//   | `StackPolicy = RefreshOnly`     | `StackLimitCount = 1` + `RefreshOnSuccessfulApplication` |
//   | `StackPolicy = StackAndRefresh` | `StackLimitCount = N` + `RefreshOnSuccessfulApplication` |
//   | `StackPolicy = IndependentInstance` | `StackingType = AggregateBySource`（见下方备注）      |
//   | `StackPolicy = IgnoreIfPresent` | `bDenyOverflowApplication = true` + `StackLimitCount = 1` |
//   | `DurationSeconds`（`0` = 无限）  | `HasDuration` + 字面量时长 / `Infinite`                  |
//   | `TickInterval`                  | `Period`                                             |
//   | `DamagePerStack` + `bPercentOfMaxHP` | `UACPeriodicDamageExecution`（按 `StackCount` 算） |
//   | `bConsumeStackOnTick`           | SetByCaller `Data.PeriodicConsumeStackOnTick`（由 Execution 消费） |
//   | `bRemovable`                    | `AssetTags` 打标 `Effect.Removable`                   |
//   | `bIsDebuff`                     | `AssetTags` 打标 `Effect.Debuff`                      |
//   | `SourcePowerSnapshot`           | 走 `FGameplayEffectSpec::SetByCaller`（本阶段不产出字段，见报告遗留项） |
//   | `TickEffectBlockIds`            | `UAdditionalEffectsGameplayEffectComponent`（7 种状态当前都为空，故未挂） |
//
// 备注（`IndependentInstance`）：§4.4 原文就是映射到 `AggregateBySource`；
//   本基类把它与其他策略一样落到 `AggregateBySource`（理由见 `UACGameplayEffectBase::ConfigureStacking`
//   的注释：`None` 会变成"每次施加都是独立实例"，反而丢掉"同状态一个实例、层数累加"的旧语义）。
//   7 种状态里**没有任何一种**用 `IndependentInstance`（见各自的策略），因此这条映射当前无实际内容。
//
// 每 tick 一次的固定顺序（旧容器 `FAbnormalStateContainer::Tick`，ACAbnormalStates.cpp:243-283）：
//   "到点 → 先算伤害 → 再 -1 层"。本实现把这两步都放在 `UACPeriodicDamageExecution` 里
//   （伤害在前、扣层在后），顺序一致。
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "NativeGameplayTags.h"
#include "GAS/Effects/ACGameplayEffectBase.h"
#include "ACGE_States.generated.h"

/**
 * 效果分类标签（`Effect.*`）。
 *
 * 为什么新建而不是复用 `BattleTags`：`Core/ACBattleTags.h` 里现有的是 `Effect.Trigger.*`
 * 三个**触发器**标签，而 §4.4 / §4.2 要的是"效果性质"标签（`Effect.Debuff` / `Effect.Removable`）。
 * 本阶段的范围约束是"只改 `Source/` 下的代码"，改 `ACBattleTags.h` 属于动既有文件（虽在同一目录树），
 * 因此按 GAS 原生标签的惯例，在新增文件里独立声明一组（原生标签是幂等注册，不会与既有标签冲突）。
 *
 * 声明写法逐字跟随 `Core/ACBattleTags.h:11-12`：`AUTOCHESSCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(X)`
 * —— 本模块里对应 `AUTOCHESSBATTLE_API`。
 * 该宏展开为 `AUTOCHESSBATTLE_API extern FNativeGameplayTag X;`（NativeGameplayTags.h:31），
 * 即"本模块导出的全局标签对象"，与本模块既有的 `BattleTags` 声明形态一致。
 */
namespace BattleEffectTags
{
    /** 负面状态（对应旧 `bIsDebuff`）。 */
    AUTOCHESSBATTLE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Debuff);
    /** 可被驱散（对应旧 `bRemovable`）。 */
    AUTOCHESSBATTLE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Removable);
    /** 正面增益（本阶段没有内容用，先留给阶段 3.1b 的词条/强化打标）。 */
    AUTOCHESSBATTLE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Buff);
}

/**
 * 异常状态 GE 的公共基类。
 *
 * 为什么要有基类：7 种状态的差别**只有数值**（层数上限 / 策略 / 时长 / 周期 / 每层伤害），
 * 行为完全同构。把行为写一遍、把数值留给子类，才不会出现"流血扣层、中毒忘了扣层"这种分叉。
 */
UCLASS(Abstract)
class AUTOCHESSBATTLE_API UACGE_StateBase : public UACGameplayEffectBase
{
    GENERATED_BODY()

public:
    UACGE_StateBase();

    /**
     * 一次性把状态定义灌进来（子类构造函数里调用，且**只调用一次**）。
     *
     * 阶段 4：第三个形参从自研的 `EACStackPolicy` 换成**引擎的叠加策略三件套里的两件**
     *（`EACStackPolicy` 已随阶段 4 的枚举清理删除，见 `Core/ACBattleTypes.h`）。
     * 7 种状态实际只用两种组合，直接写引擎取值比经过一层"自研名字"更难看错：
     *   · 叠层且刷新时长 → `RefreshOnSuccessfulApplication` + `ResetOnSuccessfulApplication`
     *     （旧 `StackPolicy::StackAndRefresh`，Bleed / Wound / Poison / Burn / Corrosion / Freeze / Bleed_Leech）；
     *   · 只叠层不刷时长 → `NeverRefresh` + `NeverReset`（旧 `StackPolicy::StackOnly`，StaticDisorder）。
     * 旧映射表里另外三种策略（`RefreshOnly` / `IgnoreIfPresent` / `IndependentInstance`）
     * **没有任何状态使用**，随枚举一起删除；将来真需要时直接写引擎取值即可
     *（`RefreshOnly` = `StackLimitCount 1` + Refresh；`IgnoreIfPresent` = `bDenyOverflowApplication = true`；
     *  `IndependentInstance` = `EGameplayEffectStackingType::AggregateBySource`，本基类默认就是它）。
     *
     * @param StateTag       状态标签（授予目标）
     * @param MaxStacks      层数上限，`0` = 无上限（旧 `MaxStacks = 0`）
     * @param StackDurationPolicy 层数刷新策略（引擎取值）
     * @param StackPeriodPolicy   周期重置策略（引擎取值）
     * @param DurationSeconds 时长；`0` = 无时限（→ `Infinite`）
     * @param TickInterval   周期（秒）
     */
    void ConfigureState(const FGameplayTag& StateTag, int32 MaxStacks,
                        EGameplayEffectStackingDurationPolicy StackDurationPolicy,
                        EGameplayEffectStackingPeriodPolicy StackPeriodPolicy,
                        float DurationSeconds, float TickInterval);

    /**
     * 配"每层一个属性修饰"（用于冰冻的"每层 -10 攻速"）。
     *
     * 引擎侧语义已核实：`FGameplayEffectSpec::GetModifierMagnitude` 在
     * `bFactorInStackCount` 为真时会把幅度乘上层数（GameplayEffect.cpp:1918-1929），
     * 而 `UGameplayEffect` 的 `bFactorInStackCount` 默认 true（GameplayEffect.cpp:149 `bFactorInStackCount = true;`），
     * 换算公式是 `GameplayEffectUtilities::ComputeStackedModifierMagnitude`
     * （GameplayEffectTypes.cpp:113-131）：非 Override 运算按 `(幅度 - Bias) * 层数 + Bias`，
     * `Additive` 的 Bias = 0，因此"每层 -10 攻速"只需写一条 `Additive(-10)` 修饰。
     */
    void ConfigurePerStackModifier(const FGameplayAttribute& Attribute, float PerStackValue);

    /** 按旧 `bIsDebuff` / `bRemovable` 打 `AssetTags`。 */
    void ConfigureClassificationTags(bool bIsDebuff, bool bRemovable);
};

/**
 * 流血（State.Bleed）。
 * 来源：ACBattleContentDefinitions.cpp:865-870 + AbnormalStates.csv ST_01
 *   MaxStacks 20 / StackAndRefresh / 时长 0（无限）/ 周期 1s /
 *   DamagePerStack 0.5 且按 MaxHP 百分比 / 技术伤害 / bConsumeStackOnTick = true / 默认 Debuff+Removable。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_State_Bleed : public UACGE_StateBase
{
    GENERATED_BODY()
public:
    UACGE_State_Bleed();
};

/**
 * 伤口（State.Wound）。
 * 来源：ACBattleContentDefinitions.cpp:886-891 + AbnormalStates.csv ST_02
 *   MaxStacks 0（无限）/ StackAndRefresh / 时长 0（无限）/ 周期 1s /
 *   无周期伤害（DamagePerStack 0）/ bConsumeStackOnTick = true。
 *   "受物理伤害时追加 1×层数 的额外物理伤害"由 Hook 效果块表达（属 3.1b 的能力/钩子），本 GE 不含它。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_State_Wound : public UACGE_StateBase
{
    GENERATED_BODY()
public:
    UACGE_State_Wound();
};

/**
 * 中毒（State.Poison）。
 * 来源：ACBattleContentDefinitions.cpp:872-877 + AbnormalStates.csv ST_03
 *   MaxStacks 0（无限）/ StackAndRefresh / 时长 0（无限）/ 周期 1s /
 *   DamagePerStack 1（绝对值）/ 技术伤害 / bConsumeStackOnTick = true。
 *   "层数达 100 时失去 10% 最大生命上限"是 CSV 里的待补项，旧代码同样未实现（注释写明），本 GE 不含它。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_State_Poison : public UACGE_StateBase
{
    GENERATED_BODY()
public:
    UACGE_State_Poison();
};

/**
 * 灼烧（State.Burn）。
 * 来源：ACBattleContentDefinitions.cpp:879-884 + AbnormalStates.csv ST_04
 *   MaxStacks 0（无限）/ StackAndRefresh / 时长 5s / 周期 1s /
 *   DamagePerStack 1（绝对值）/ 技术伤害 / bConsumeStackOnTick = **false**（"结束时清空层数"）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_State_Burn : public UACGE_StateBase
{
    GENERATED_BODY()
public:
    UACGE_State_Burn();
};

/**
 * 腐蚀（State.Corrosion）。
 * 来源：ACBattleContentDefinitions.cpp:901-906 + AbnormalStates.csv ST_05
 *   MaxStacks 0（无限）/ StackAndRefresh / 时长 5s / 周期 1s / 无周期伤害 / 不消耗层数。
 *   "抗性减少 1×层数"：
 *     ⚠️ 旧代码**没有**实现它（内容定义里明写"抗性减少由效果块（AddStatModifier + RES）表达"），
 *     因此本 GE **也不加**那条修饰 —— 加了就等于凭空引入一个旧版本没有的数值（直接违反 C6/D10）。
 *     `ConfigurePerStackModifier` 因此在本类里被注释掉，用法见 `UACGE_State_Freeze`（那一处 CSV 与代码一致）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_State_Corrosion : public UACGE_StateBase
{
    GENERATED_BODY()
public:
    UACGE_State_Corrosion();
};

/**
 * 冰冻（State.Freeze）。
 * 来源：ACBattleContentDefinitions.cpp:917-919 + AbnormalStates.csv ST_06
 *   MaxStacks 3 / StackAndRefresh / 时长 5s / 周期 1s / 无周期伤害 / 不消耗层数。
 *   "每层减少 10 点攻速"→ 一条 `Additive(-10)` 修饰（引擎按 `bFactorInStackCount` 自动乘层数）。
 *   ⚠️ "满层时护盾受额外 20% 伤害"在旧代码里**没有实现**（内容定义只登记时长与叠加策略，注释明说），
 *      因此本 GE 不含它；这条留给阶段 3.1b/4 的护盾增伤通道决议。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_State_Freeze : public UACGE_StateBase
{
    GENERATED_BODY()
public:
    UACGE_State_Freeze();
};

/**
 * 静电紊乱（State.StaticDisorder）。
 * 来源：ACBattleContentDefinitions.cpp:893-899 + AbnormalStates.csv ST_08
 *   MaxStacks 0（无限）/ **StackOnly** / 时长 10s / 周期 1s / 无周期伤害 / 不消耗层数。
 *   "每次动作受 1×层数 技术伤害"在旧代码里由 `UBattleWorld::HandleActionTriggered`
 *   （框架内置的系统级钩子）统一处理，**不是**本状态的周期结算 —— 那是 3.1b/3.3 的钩子接线，
 *   本 GE 只表达"存在 / 层数 / 10 秒后消失"。
 *   "每 10 层电磁爆发（施法者 30% 技术强度 + 150% 层数技术伤害）"是 CSV 待补项，旧代码未实现，本 GE 不含它。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_State_StaticDisorder : public UACGE_StateBase
{
    GENERATED_BODY()
public:
    UACGE_State_StaticDisorder();
};

/**
 * "蠕虫撕咬"专用流血：**同一个状态标签、不同时长**。
 *
 * 来源：`Blocks::Enemy_Leech_Bite`（`ACBattleContentDefinitions.cpp:443-457`，原文已随 3.2a 删除）：
 *   `ApplyAbnormalState(State.Bleed, Stacks = 2, DurationSeconds = 5.f)`。
 * 为什么必须是独立的 GE 类：冒号里的 `DurationSeconds = 5` 是**施加时覆盖**，
 *   而默认 `UACGE_State_Bleed` 的时长是"无限"（`DurationSeconds = 0.f`）。两个时长不可能同时写在
 *   一个 GE 类上，而引擎重算时长只看 `Def->DurationMagnitude`（GameplayEffect.cpp:4200-4203），
 *   所以"施加时改时长"的唯一可靠做法是**换一个 GE 类**。
 * 其余数值（0.5%/层技术伤害、无上限、StackAndRefresh、每秒 -1 层）与 `UACGE_State_Bleed` **完全一致**。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_State_Bleed_Leech : public UACGE_StateBase
{
    GENERATED_BODY()
public:
    UACGE_State_Bleed_Leech();
};

/**
 * 眩晕（`State.Stun`）—— **阶段 3.2b 唯一新增的 GE**（§3.2 表："`FStunEntry` / `ApplyStun` → GE"）。
 *
 * 为什么必须产出它：旧 `FAbilityExecutor::ApplyStun` 是**内核 API**（被
 * `FMentalSystem::HandleBreak` 调用：精神崩溃眩晕 2 秒），随技能执行线一起删除后，
 * 若没有 GE 承接，"精神崩溃"这条规则就断了 —— 这是删除动作**迫使**新增的一个类，
 * 不是凭空加内容（数值 2 秒取自旧调用的实参 `ACMentalSystem.cpp:222` 的 `2.f`）。
 *
 * 与 `UACGE_StateBase` 的关系：**刻意不继承它**。基类会挂 `UACPeriodicDamageExecution`
 * 并强制一个"周期 + 扣层"的模型，而眩晕是**纯粹的限时标签**（无周期结算、无层数消耗、
 * 重复施加只刷新时长），套那套机制只会引入一段每秒空跑的执行体。
 *
 * 逐项映射（§4.1 的 `FStunEntry` / `ApplyStun` 行）：
 *   | 旧（ACAbilityExecutor.cpp:590-624）                    | 本类                                        |
 *   | ----------------------------------------------------- | ------------------------------------------- |
 *   | `FStunEntry::EndTime = Now + Seconds`                  | `HasDuration` + SetByCaller `Data.DurationSeconds` |
 *   | 叠加取最晚到期时刻（A18）                                | `ConfigureStacking(1, RefreshOnSuccessfulApplication, …)` |
 *   | `Unit->SetStunned(true)` + `State.Stun` 状态            | `GrantedTags = State.Stun`                  |
 *   | `Unit->SetActionState(Stunned)`                        | 不再需要（门控一律查标签，见 `EACUnitActionState` 的注释）|
 *   | `InterruptChannel(Target, Stun)`                       | 不再需要（引导线已删除，全仓无引导内容）        |
 *   | `Source`（`FName("MentalBreak")`）                      | 无对应物：旧字段只写不读，删除后没有信息丢失     |
 *
 * ⚠️ 时长**必须由施加方给**（`MakeHasDurationByCaller`）：它是"每次施加各自决定"的瞬时数据
 *（旧 `ApplyStun(Target, Seconds, Source)` 的第二个形参），写死在类上就无法表达别的时长。
 * 键名用 `GetDurationDataName()` 取，不要在调用点写字符串字面量。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACGE_State_Stun : public UACGameplayEffectBase
{
    GENERATED_BODY()
public:
    UACGE_State_Stun();

    /**
     * 时长 SetByCaller 的键名（默认 `Data.DurationSeconds`，与基类 `MakeHasDurationByCaller` 一致）。
     * 施加方写法：`MakeSetByCaller(UACGE_State_Stun::GetDurationDataName(), 2.f)`。
     */
    static FName GetDurationDataName();
};
