// M08 战斗结算管线：一切伤害/治疗/护盾/吸血的唯一入口（R4）。
// 规则（阶段 2 更新，§4.6 新契约）：只有本类可以修改属性集 `Health` 与 `Shield`；
// 所有中间值写入 FDamageResult（日志可复现）。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleTypes.h"
#include "Core/ACDataTypes.h"
#include "ACCombatResolver.generated.h"

class UBattleWorld;
class AACBattleUnitBase;
class UACBattleAttributeSet;

// 阶段 0b（D8）：单位参数一律显式写成 Actor 类型；本头只把 `AACBattleUnitBase&` / `AACBattleUnitBase*`
// 当函数参数用，故前向声明足够。**不能** include Battle/ACBattleUnitBase.h ——
// 它反过来 include 本文件（FShieldPool 是按值成员），会形成环。

/** 单层护盾实例。 */
struct AUTOCHESSBATTLE_API FShieldInstance
{
    uint64 InstanceId = 0;
    float Amount = 0.f;
    /** 到期时刻（绝对时间，秒）；**负值 = 持续护盾**。 */
    float ExpireTime = -1.f;
    FUnitId Source = InvalidUnitId;
    FName SourceEffectBlockId;
    /** 创建时刻（绝对时间，秒）。 */
    float CreatedTime = 0.f;
};

/**
 * 护盾池：FIFO 吸收，支持持续护盾。
 *
 * 阶段 2（§4.3：护盾迁到 `UACBattleAttributeSet::Shield`）之后的分工：
 *   - **总护盾量以属性集 `Shield` 为准**（`GetTotal()` 读它，任何改动都经 `SyncTotalToAttributeSet` 写回）；
 *   - `Instances` 数组**保留**：GAS 5.6 没有内置护盾（附录 C），而"每层护盾各有各的到期时刻"
 *     这件事属性集单值表达不了（FIFO 顺序、`ExpireTime`、来源单位都要留）。
 *   换句话说：数组是"结构"，属性集是"总量"，两者由 `SyncTotalToAttributeSet` 保持同一。
 */
class AUTOCHESSBATTLE_API FShieldPool
{
public:
    void Add(FShieldInstance Instance);
    /** 当前总护盾量：读属性集 `Shield`（阶段 2 起它是唯一权威）。 */
    float GetTotal() const;
    /** 吸收伤害；返回实际吸收量；bOutBroken 表示本次被打光。 */
    float Absorb(float Damage, bool& bOutBroken);
    /**
     * 清理到期护盾。
     * @param NowSeconds 当前**绝对时间**（秒）。护盾池是纯数据容器（不持有 World），
     *                   时间由 `FCombatResolver` 统一传入，容器内不做任何取时。
     */
    void RemoveExpired(float NowSeconds);
    /**
     * 按**来源效果块**移除护盾实例（§4.6 裁决②）。
     *
     * 用途：护盾 GE 到期时的清理。每次施加护盾 = 一个独立 GE 实例、各自持有时长，
     * 因此到期只该移除**它自己那一层**，而不是 `Clear()` 整池 ——
     * 同一次战斗叠了两层不同时长的护盾时，整池清空会把未到期的那层一起清掉。
     *
     * `SourceEffectBlockId` 为 `NAME_None` 时**不做任何事**：按空来源删会把所有
     * "没有来源"的护盾一起删掉（早期实现就是这么写的），属于误伤。
     *
     * 为什么用 `SourceEffectBlockId` 而不是 `InstanceId`：GE 的施加与到期是两个独立的
     * Execution 调用，中间隔着引擎的容器管理，"我上次创建了哪个实例"没有可传递的载体；
     * 而来源标识从 `FACGameplayEffectContext` 一路传到实例，两侧都能复现。
     */
    void RemoveBySourceEffectBlockId(FName SourceEffectBlockId);
    void Clear();
    bool IsEmpty() const { return Instances.Num() == 0; }
    const TArray<FShieldInstance>& GetAll() const { return Instances; }

private:
    /**
     * 注入"总护盾量写哪儿"的落点。由 `AACBattleUnitBase` 的构造函数调用一次
     * （属性集是那时的默认子对象），其余地方不要调 —— 护盾池是单位的按值成员，
     * 和属性集同生共死，不存在换绑的场景。
     */
    void Initialize(UACBattleAttributeSet* InAttributeSet);

    /**
     * 把 `Instances` 的合计写回属性集 `Shield`（Base 与 Current 一起）。
     *
     * ⚠️ **每一个**改动实例数组的地方都必须调用它（Add / Absorb / RemoveExpired / Clear）：
     * FIFO 吸收会分多次扣减实例，任何一处漏同步都会让 `GetTotal()`
     *（= 属性集的值）与数组实际之和静默不一致。
     */
    void SyncTotalToAttributeSet();

    TArray<FShieldInstance> Instances;
    uint64 NextInstanceId = 1;

    /** `Shield` 属性的宿主（阶段 2）。单位 Actor 的默认子对象，生命周期覆盖本容器。 */
    UACBattleAttributeSet* AttributeSet = nullptr;

    /** 只有单位 Actor 能注入属性集（`Initialize` 是私有的）。 */
    friend class AACBattleUnitBase;
};

/** 伤害请求。 */
USTRUCT(BlueprintType)
struct AUTOCHESSBATTLE_API FDamageRequest
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    int32 Source = InvalidUnitId;

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    int32 Target = InvalidUnitId;

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    EACDamageType DamageType = EACDamageType::Physical;

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    EACDamageReason Reason = EACDamageReason::Skill;

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    float RawAmount = 0.f;

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    bool bCanCrit = true;

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    bool bIsBasicAttack = false;

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    bool bIgnoreShield = false;

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    EACJudgmentPoint DeclaredJudgment = EACJudgmentPoint::AppliedHpLoss;

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    FName SourceEffectBlockId;

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat")
    FGameplayTagContainer Tags;
};

/** 伤害结果（每个检查点都记录，便于日志与平衡分析）。 */
USTRUCT(BlueprintType)
struct AUTOCHESSBATTLE_API FDamageResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") bool  bCancelled = false;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") bool  bWasCrit = false;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float Requested = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float CritMultiplier = 1.f;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float AfterMitigation = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float AfterIncreaseReduction = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float RedirectedOut = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float ShieldAbsorbed = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float AppliedHpLoss = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float Overkill = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float LifestealHealed = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") int32 FinalTarget = InvalidUnitId;
};

USTRUCT(BlueprintType)
struct AUTOCHESSBATTLE_API FHealRequest
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat") int32 Source = InvalidUnitId;
    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat") int32 Target = InvalidUnitId;
    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat") float RawAmount = 0.f;
    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat") bool bAllowOverhealConversion = true;
    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat") FName SourceEffectBlockId;
};

USTRUCT(BlueprintType)
struct AUTOCHESSBATTLE_API FHealResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float Applied = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Combat") float Overheal = 0.f;
};

USTRUCT(BlueprintType)
struct AUTOCHESSBATTLE_API FShieldRequest
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat") int32 Source = InvalidUnitId;
    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat") int32 Target = InvalidUnitId;
    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat") float Amount = 0.f;
    /** < 0 = 持续护盾。 */
    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat") float DurationSeconds = 6.f;
    UPROPERTY(BlueprintReadWrite, Category = "Battle|Combat") FName SourceEffectBlockId;
};

/**
 * 阶段 3.2a（GAS 重构实施方案 §3.1 表 / §7 阶段 3.2）：`FACDamageModifier` 与它的两个注册函数
 *（`AddDamageModifier` / `RemoveDamageModifiersBySource`）以及清理函数
 *（`RemoveExpiredDamageModifiers`）**全部已删除**。
 *
 * 删它的依据：本结构唯一的注册者是旧 `FEffectSystem` 的 `EACActionType::AddDamageModifier` 分支
 *（`ACEffectSystem.cpp:681`），而 `FEffectSystem` + `FACEffectBlock` + `EACActionType`
 * 在本阶段整条删除 —— 留着它就是一个"永远为空、没有任何代码能往里写"的死表。
 * 全仓没有任何内容真正注册过它（唯一相关的 `Trait_DeadlyStrike` 从未被任何强化选项引用），
 * 因此删除它**不改任何数值**（C6/D10）。
 *
 * 替代通道（§4.2 映射表）：`AddDamageModifier` → "GE Modifier + SetByCaller 幅度"。
 * 现阶段的 `UACGE_Trait_DeadlyStrike` 用"`Attack` × 1.2"表达处刑人（已知语义差异写在
 * `ACGE_ContentEffects.h` 的类注释里）；若将来真需要"只对某类伤害生效的增伤"，
 * 按 §4.6 的新契约走 `UGameplayEffectExecutionCalculation`，**不要重建这张表** ——
 * 那会让 `Health` 出现第二条改动路径。
 *
 * 7 个检查点仍然保留：`ApplyIncreaseReduction` 保留在管线里（现在恒等返回），
 * 于是 `FDamageResult::AfterIncreaseReduction` 这一列照旧有值，与基线日志列对齐。
 */

/**
 * 战斗结算器。
 * 承伤顺序：钩子 -> 暴击 -> 类型减免 -> 增减伤 -> 分摊(framework 预留) -> 护盾 -> 扣血 -> 后置(吸血) -> 事件/统计。
 */
class AUTOCHESSBATTLE_API FCombatResolver
{
public:
    void Initialize(UBattleWorld* InWorld);

    FDamageResult ApplyDamage(const FDamageRequest& Request);
    FHealResult   ApplyHeal(const FHealRequest& Request);
    void          ApplyShield(const FShieldRequest& Request);
    void          ApplyLifesteal(FUnitId Source, FDamageResult& Result, EACJudgmentPoint JudgmentPoint);

    void RemoveExpiredShields();

private:
    float ComputeMitigation(const FDamageRequest& Request, const AACBattleUnitBase& Target, float Damage) const;
    /**
     * 检查点 ③ 的写入点（§4.6 的 7 个检查点之一）。
     *
     * 阶段 3.2a：增伤 / 减伤表（`FACDamageModifier`）已删除，本函数因此**恒等返回** ——
     * 但**保留在管线里**，否则 `FDamageResult::AfterIncreaseReduction` 就没有生产点，
     * 日志与 `_damage.jsonl` 会少一列。
     */
    float ApplyIncreaseReduction(const FDamageRequest& Request, float Damage) const;
    void  RecordDamageStats(FUnitId Source, FUnitId Target, float Dealt, float Taken);

    UBattleWorld* World = nullptr;
};
