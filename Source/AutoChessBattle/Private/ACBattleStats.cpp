// 阶段 2 改造（GAS 重构实施方案 §3.2 / §4.3 / §7 阶段 2）：属性管线实现。
//
// 本文件**不再包含任何"属性计算"**：修饰器聚合 / 重算 / 到期已全部交给 GAS
// （`UACBattleAttributeSet` + 聚合器 + GE 时长）。剩下的六个静态工具见头文件说明。
#include "Stats/ACBattleStats.h"
#include "GAS/ACBattleAttributeSet.h"

void FBattleStatPipeline::SetBaseFromBlock(FBattleStatSheet& Sheet, const FACStatBlock& Block)
{
    if (Sheet.Base.Num() != ACStatCount)
    {
        Sheet.Base.SetNumZeroed(ACStatCount);
    }
    for (int32 Index = 0; Index < ACStatCount; ++Index)
    {
        Sheet.Base[Index] = Block.Values.IsValidIndex(Index) ? Block.Values[Index] : 0.f;
    }
    // 阶段 2：原来的 `Sheet.bDirty = true` 已删除 —— 脏标记与重算随修饰器一起交给 GAS 聚合器。
}

void FACStatConversionRule::ApplyToBlock(FACStatBlock& Block, float NowSeconds) const
{
    const int32 SourceIndex = static_cast<int32>(Source);
    if (!Block.Values.IsValidIndex(SourceIndex))
    {
        return;
    }

    float SourceValue = Block.Values[SourceIndex];
    if (bFixedValue)
    {
        SourceValue = FixedValue;
    }
    else if (OscillationMax > OscillationMin)
    {
        // 那摩：80% -> 120% 循环。
        // 阶段 0.5：相位直接用**秒**算（原来是 `CurrentTick / 30.f`，即把 tick 折成秒）。
        // 现在时间源本来就是秒，因此去掉 /30.f 这一步，公式其余部分一字未改。
        const float Period = FMath::Max(0.01f, OscillationPeriod);
        const float Phase01 = (NowSeconds >= 0.f)
            ? FMath::Fmod(NowSeconds / Period, 1.f)
            : 0.5f;
        SourceValue *= FMath::Lerp(OscillationMin, OscillationMax, Phase01);
    }
    else
    {
        SourceValue *= GainScale;
    }

    float Overflow = 0.f;
    if (Cap > 0.f && SourceValue > Cap)
    {
        Overflow = SourceValue - Cap;
        SourceValue = Cap;
    }

    Block.Values[SourceIndex] = SourceValue;

    const int32 TargetIndex = static_cast<int32>(OverflowTarget);
    if (Overflow > 0.f && Block.Values.IsValidIndex(TargetIndex) && OverflowRatio > 0.f)
    {
        Block.Values[TargetIndex] += Overflow * OverflowRatio;
    }
}

float FBattleStatPipeline::GetBaseMaxHP(const FBattleStatSheet& Sheet)
{
    // 阶段 2 简化（口径变化，已在阶段 2 报告里确认）：
    // 旧实现 = Base[MaxHP] 叠加 **Permanent + BattlePermanent** 两个 scope 的修饰器，再 `Max(1, ...)`；
    // 现在修饰器数组已删除，因此退化为"只读 Base 里的 MaxHP"，公式外壳（`Max(1, ...)`）保持不变。
    //
    // 为什么这个差异可接受：Run 层的永久修饰**已经折进 `Spec.BaseStats`**
    //（`FACRunStatUtils::ComposeBaseStats` 在组装 `FACPlayerUnitSpec::BaseStats` 时就叠好了，
    // 见 Private/Run/ACRunStatUtils.cpp）—— 也就是说战斗内核里从来不会出现"只作用于 MaxHP 的
    // Permanent 修饰器"，旧实现的那两轮循环在本项目的数据流下拿到的就是 Base 本身。
    // 剩余差异为零；真正会用到"战斗内永久修饰"的场景属于阶段 3/4 的 GE 化范围。
    const float Value = Sheet.Base.IsValidIndex(static_cast<int32>(EACStat::MaxHP))
        ? Sheet.Base[static_cast<int32>(EACStat::MaxHP)]
        : 0.f;
    return FMath::Max(1.f, Value);
}

void FBattleStatPipeline::SnapshotValues(const FBattleStatSheet& Sheet, TArray<float>& OutValues)
{
    // 阶段 2：快照源从 `Sheet.Current` 换成 `Sheet.Base`。
    // 二者在语义上等价 —— `Current` 是"Base 叠加修饰器后的结果"，随修饰器删除而不复存在；
    // 阶段 2 起属性的**最终值**由属性集持有，但"召唤继承"的口径历来是
    // "继承 Owner 的属性快照 × InheritRatio"（`InitializeFromSpawnRequest`），
    // 拿 Base（= 属性集在初始化时写入的那一份数）就足够了。
    OutValues = Sheet.Base;
}

// ---------------------------------------------------------------------------
// 三个派生量：数据来源改读属性集（公式一字未改）
// ---------------------------------------------------------------------------

namespace
{
    /**
     * 按 `EACStat` 从属性集取**当前值**。
     *
     * 与 `AACBattleUnitBase::GetStat` 同一姿势（详见那边的注释）：
     * `GetAttributeForStat` 拿到 `FGameplayAttribute`，再经 `FStructProperty::ContainerPtrToValuePtr`
     * 落到 `FGameplayAttributeData` 上取 CurrentValue。
     * 刻意**不走** `ASC->GetNumericAttribute`：属性集的 CurrentValue 就是权威值，
     * 无需第二次查询，也不引入"ASC 是否已 InitAbilityActorInfo"这一层时序依赖。
     *
     * 用 `FGameplayAttribute::GetGameplayAttributeData`（AttributeSet.h:120 的 const 重载）
     * 而不是自己 `CastField<FStructProperty>`：该函数内部已做 `IsGameplayAttributeDataProperty` 校验，
     * 与引擎 `UAttributeSet::InitFromMetaDataTable` 的实现路径一致。
     */
    float GetAttributeCurrentValue(const UACBattleAttributeSet& Attributes, EACStat Stat)
    {
        const FGameplayAttribute Attribute = UACBattleAttributeSet::GetAttributeForStat(Stat);
        if (!Attribute.IsValid())
        {
            // 只有 `EACStat::Count`（哨兵）会走到这里。属性集不存在"当前生命/护盾"以外的越界属性。
            return 0.f;
        }
        const FGameplayAttributeData* const Data = Attribute.GetGameplayAttributeData(&Attributes);
        return (Data != nullptr) ? Data->GetCurrentValue() : 0.f;
    }
}

float FBattleStatPipeline::GetCritChance(const UACBattleAttributeSet& Attributes)
{
    return FMath::Clamp(GetAttributeCurrentValue(Attributes, EACStat::CritValue), 0.f, 100.f) / 100.f;
}

float FBattleStatPipeline::GetCritMultiplier(const UACBattleAttributeSet& Attributes)
{
    const float CritValue = GetAttributeCurrentValue(Attributes, EACStat::CritValue);
    const float ExtraBonus = GetAttributeCurrentValue(Attributes, EACStat::CritDamageBonus) / 100.f;
    return 1.2f + FMath::Max(0.f, CritValue - 100.f) / 100.f + ExtraBonus;
}

float FBattleStatPipeline::GetAttackSpeedHz(const UACBattleAttributeSet& Attributes)
{
    // 每点攻速 = 每秒 0.01 次。
    return FMath::Max(0.f, GetAttributeCurrentValue(Attributes, EACStat::ASPD) * 0.01f);
}
