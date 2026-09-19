// 战斗系统共享类型（AutoChessCore）。
// 只允许放跨层共享的 POD / 枚举 / 轻量结构；战斗逻辑不在此模块。
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "ACBattleTypes.generated.h"

/** 单位句柄：战斗内唯一、单调递增、不复用。0 = InvalidUnitId。 */
using FUnitId = int32;
static constexpr FUnitId InvalidUnitId = 0;

/** 阵营（单机：我方 / 敌方 / 中立）。 */
UENUM(BlueprintType)
enum class EACTeam : uint8
{
    Player  UMETA(DisplayName = "我方"),
    Enemy   UMETA(DisplayName = "敌方"),
    Neutral UMETA(DisplayName = "中立")
};

/** 单位类别（对应设计文档「战斗单位分类」）。 */
UENUM(BlueprintType)
enum class EACUnitKind : uint8
{
    Operator UMETA(DisplayName = "干员"),
    Enemy    UMETA(DisplayName = "敌人"),
    Summon   UMETA(DisplayName = "召唤物"),
    Clone    UMETA(DisplayName = "分身")
};

/** 战斗阶段（对应 TechDocs/M01 状态机）。 */
UENUM(BlueprintType)
enum class EACPhase : uint8
{
    None,
    Loading,
    Prepare,
    BattleStart,
    Combat,
    CheckEnd,
    Result,
    Teardown
};

/** 战斗结果。 */
UENUM(BlueprintType)
enum class EACOutcome : uint8
{
    None,
    Victory,
    Defeat,
    Abandoned,
    Timeout,
    Error
};

/** 伤害类型（物理 / 技术 / 真实 / 精神）。 */
UENUM(BlueprintType)
enum class EACDamageType : uint8
{
    Physical    UMETA(DisplayName = "物理"),
    Technical   UMETA(DisplayName = "技术"),
    // 注意：UHT 禁止枚举元素名为 true/false（不分大小写），故真实伤害用 TrueDamage。
    TrueDamage  UMETA(DisplayName = "真实"),
    Mental      UMETA(DisplayName = "精神")
};

/** 伤害来源。 */
UENUM(BlueprintType)
enum class EACDamageReason : uint8
{
    BasicAttack,
    Skill,
    Dot,
    Reflect,
    Environment,
    Execute
};

/** 判定点（B4 的显式契约，见 TechDocs/00A §3）。 */
UENUM(BlueprintType)
enum class EACJudgmentPoint : uint8
{
    Requested,
    AfterMitigation,
    AfterShield,
    AppliedHpLoss,
    Overkill
};

/**
 * 单位行动状态。
 *
 * 阶段 4（§7 阶段 4 的枚举清理）：`Channeling` / `Stunned` 两个枚举项**已删除**。
 *   · `Stunned`：旧写入口是 `FAbilityExecutor::ApplyStun`（随技能执行线一起删除）。
 *     "被眩晕"现在由 ASC 上 `UACGE_State_Stun` 授予的 `State.Stun` 标签表达，
 *     门控方一律查标签（`FActionScheduler::EvaluateGate` 返回 `EACSchedulerGate::Stunned`，
 *     `FBattleAISystem::Decide` 返回 `Skip`，普攻/技能能力的 `ActivationBlockedTags` 挡下）
 *     —— 全仓**没有任何写入点**能产生这个取值。
 *   · `Channeling`：引导线整体不存在（无内容使用引导施放），`ACActionScheduler` 里
 *     唯一读它的那个分支因此**恒为假**，本阶段随枚举项一起删除。
 *
 * **整数值口径变化（对 UI 的影响已核实为零）**：删掉中间两项后
 * `Dead` 由 6 变成 4。唯一把它当整数往外写的地方是 `FACUnitViewModel::ActionState`
 * （`static_cast<int32>`），而 `FACUnitViewModel` 本身在本阶段随表现桥一起删除，
 * 因此没有任何外部消费者按整数解释过它；日志与两份 JSON 产物都不写这个字段
 * （已 grep 核实：`GetActionState()` 的读取点只剩 `FActionScheduler::EvaluateGate`）。
 */
UENUM(BlueprintType)
enum class EACUnitActionState : uint8
{
    Idle,
    Moving,
    Attacking,
    Casting,
    Dead
};

/** 属性枚举（对应 TechDocs/M07；CurrentHP/护盾/专注/精神为资源，不在此表）。 */
UENUM(BlueprintType)
enum class EACStat : uint8
{
    MaxHP = 0,
    ATK,
    TECH,
    DEF,
    RES,
    CritValue,
    CritDamageBonus,
    Lifesteal,
    ASPD,
    DEFPen,
    RESPen,
    HpRegen,
    FocusMax,
    FocusInit,
    FocusRegen,
    FocusPerAttack,
    MentalMax,
    Range,
    Count UMETA(Hidden)
};

constexpr int32 ACStatCount = static_cast<int32>(EACStat::Count);

/** 修饰器运算。 */
UENUM(BlueprintType)
enum class EACModOp : uint8
{
    Add,
    MulPct,
    Override
};

// ---------------------------------------------------------------------------
// 作用域 / 堆叠策略（阶段 4 已删除）
// ---------------------------------------------------------------------------
//
// `EACModScope`（Permanent / BattlePermanent / BattleTemp）**已删除**（D6 / §4.3）：
// 三档 scope 收敛为 GE 的 `DurationPolicy`，判据只剩"有没有正时长"这一个：
//   · `Permanent` / `BattlePermanent` → `Infinite`（`DurationSeconds <= 0`）；
//   · `BattleTemp`                    → `HasDuration(DurationSeconds)`。
// 落点是 `UACGE_StatModifier::AddStatModifierForStat(EACStat, EACModOp, Value, DurationSeconds = 0.f)`
// —— 形参从"三档枚举 + 时长"变成"时长"（`0` = 无限），8 处内容调用点逐项等价改写。
//
// `EACStackPolicy`（StackOnly / RefreshOnly / StackAndRefresh / IndependentInstance / IgnoreIfPresent）
// **已删除**（A7 / §4.4）：`UACGE_StateBase::ConfigureState` 的第三个形参直接收引擎的
// `EGameplayEffectStackingDurationPolicy` + `EGameplayEffectStackingPeriodPolicy`，
// 即"§4.4 映射表的三件套"本身。7 种状态实际只用到两种组合（6 处 StackAndRefresh、1 处 StackOnly），
// 中间那层自研枚举只是把引擎取值换了个名字，删掉它让状态 GE 的构造函数一眼能看出引擎语义。

// ---------------------------------------------------------------------------
// 技能类型 / 专注消耗模式 / 打断原因（阶段 3.2b 已整段删除）
// ---------------------------------------------------------------------------
//
// 原本是 `EACCastType`(2) / `EACFocusCostMode`(4) / `EACInterruptCause`(5) 三个枚举，
// 它们只被旧技能执行线（`FAbilityExecutor` + `FACSkillDef`）使用，随那条线一起删除。
//
// 替代（§4.1 映射表）：
//   `EACCastType`       → 引导（`Channel`）在全仓**没有任何内容**（唯一的写入点全是 `Instant`），
//                         因此不产出等价物；将来真出现时按 §4.1 用
//                         `InstancingPolicy = InstancedPerActor` + 能力内自持 `UAbilityTask`。
//   `EACFocusCostMode`  → `CostGameplayEffectClass`（`GAS/Effects/ACGE_Costs.h` 的 4 个 Cost GE）。
//   `EACInterruptCause` → `UGameplayAbility::CancelAbility` + 标签变化监听；而"打断引导"
//                         这件事本身随引导线一起消失（旧 `InterruptChannel` 无调用价值）。
// ⚠️ 已核实全仓再无引用：`EACInterruptCause` 只在 `ACInterruptCause::None` 的默认成员初始化里出现过
//    （`FSkillCastContext` / `FActiveChannel`，两个结构体同批删除）。

/** 移动原因（用于事件与日志）。 */
UENUM(BlueprintType)
enum class EACMoveReason : uint8
{
    Pathing,
    Forced,
    Teleport,
    Push
};

/** 目标过滤器。注意：IncludeUntargetable 仅用于范围效果语义。 */
UENUM(BlueprintType)
enum class EACTargetFilter : uint8
{
    Any,
    Ally,
    Enemy,
    Alive,
    Selectable,
    IncludeUntargetable
};

/** 目标选择器类型（对应 TechDocs/M14 §5.3）。 */
UENUM(BlueprintType)
enum class EACSelectorType : uint8
{
    Self,
    PrimaryTarget,
    FrontAdjacent,
    BackAdjacent,
    Neighbors1,
    Neighbors2,
    SameRow,
    FrontCone,
    MostDenseCluster,
    LowestHpPercentAlly,
    HighestDamageDealtAlly,
    MarkedTarget,
    AllEnemiesInRange,
    AllAlliesInRange,
    OwnerOf
};

/** AI 行动意图。 */
UENUM(BlueprintType)
enum class EACActionIntent : uint8
{
    CastSkill,
    BasicAttack,
    Move,
    Wait,
    Skip
};

// ---------------------------------------------------------------------------
// 效果系统（M15）：触发器类型 / 触发策略 / 动作类型 / 条件类型
// ---------------------------------------------------------------------------
//
// **阶段 3.2a 已整段删除**（原本是 `EACEffectTriggerType`(4) / `EACTriggerPolicy`(4) /
// `EACActionType`(34) / `EACConditionType`(22) 四个枚举）。
//
// 删除依据（§3.1 表 + §7 阶段 3.2）：这四个枚举是自研效果系统的"指令集"，
// 而系统本身（`Effects/ACEffectSystem.*`）、它的数据类型（`FACEffectBlock` / `FACEffectAction`
// / `FACCondition`，原在 `Core/ACDataTypes.h`）与全部内容（`BuildEffectBlocks`）本阶段一起删除。
// 全仓已无任何引用点（含 `ACAbilityExecutor` / `ACActionScheduler`：
// 它们只引用了 `FACSkillDef` 与"状态标签"，前者属 3.2b、后者已改走 `ASC->GetTagCount`）。
//
// 替代映射见 §4.2：
//   `TriggerType`      → `UGameplayAbility::AbilityTriggers` / GE 的 `Period` / 直接施加
//   `TriggerPolicy`    → `StackLimitCount` + `bDenyOverflowApplication` / `CooldownGameplayEffectClass`
//   `EACActionType`    → `UACGameplayEffectBase` 的 Modifier / `UGameplayEffectExecutionCalculation`
//                        / `UACSummonEffectComponent` / `UACKillReviveEffectComponent`
//   `EACConditionType` → `FGameplayTagQuery` + `UGameplayEffectCustomApplicationRequirement`
//
// 保留在本文件里的近邻：`EACStat` / `EACModOp`（属性口径）。`EACModScope` / `EACStackPolicy`
// 已在阶段 4 删除，理由见本文件"作用域 / 堆叠策略"一节。
//
// **阶段 3.2b 复核**：技能执行线删除后（`FAbilityExecutor` / `FACSkillDef` 一并退场），
// 上面四个枚举依旧没有任何引用点 —— 唯一引用过它们（连同 `FACSkillDef`）的文件已经不存在了。

/** 六边形坐标：偏移坐标（even-r，偶数行右偏），Row 0 为敌方最上排。
 *  注：UHT 不支持 int16 暴露给蓝图，故用 int32；棋盘范围内数值不受影响。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACHexCoord
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Hex")
    int32 Row = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Hex")
    int32 Col = 0;

    FACHexCoord() = default;
    FACHexCoord(int32 InRow, int32 InCol) : Row(InRow), Col(InCol) {}

    FORCEINLINE bool operator==(const FACHexCoord& Other) const
    {
        return Row == Other.Row && Col == Other.Col;
    }

    FORCEINLINE bool operator!=(const FACHexCoord& Other) const
    {
        return !(*this == Other);
    }

    friend FORCEINLINE uint32 GetTypeHash(const FACHexCoord& Coord)
    {
        return HashCombine(::GetTypeHash(static_cast<int32>(Coord.Row)), ::GetTypeHash(static_cast<int32>(Coord.Col)));
    }
};

/** 朝向：我方默认 Up（Row 减小方向），敌方默认 Down。 */
UENUM(BlueprintType)
enum class EACFacing : uint8
{
    Up,
    Down
};

/** 四档数值（C/B/A/S），对应设计文档的成长档位。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACTieredValue
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Tier")
    float C = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Tier")
    float B = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Tier")
    float A = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Tier")
    float S = 0.f;

    /** TierIndex: 0=C, 1=B, 2=A, 3=S；越界时取最近有效档。 */
    FORCEINLINE float Get(int32 TierIndex) const
    {
        switch (FMath::Clamp(TierIndex, 0, 3))
        {
        case 0: return C;
        case 1: return B;
        case 2: return A;
        default: return S;
        }
    }
};

/** 基础属性块：按 EACStat 索引，长度固定 ACStatCount。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACStatBlock
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Stats")
    TArray<float> Values;

    void InitDefaults()
    {
        Values.SetNumZeroed(ACStatCount);
    }

    FORCEINLINE float Get(EACStat Stat) const
    {
        const int32 Index = static_cast<int32>(Stat);
        return Values.IsValidIndex(Index) ? Values[Index] : 0.f;
    }

    FORCEINLINE void Set(EACStat Stat, float Value)
    {
        const int32 Index = static_cast<int32>(Stat);
        if (Values.Num() < ACStatCount)
        {
            Values.SetNumZeroed(ACStatCount);
        }
        if (!Values.IsValidIndex(Index))
        {
            return;
        }
        Values[Index] = Value;
    }
};
