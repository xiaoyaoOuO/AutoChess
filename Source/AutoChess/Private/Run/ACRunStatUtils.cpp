#include "Run/ACRunStatUtils.h"
#include "Run/ACRunTypes.h"
#include "Core/ACDataTypes.h"

void FACRunStatUtils::MakeZeroBlock(FACStatBlock& OutBlock)
{
    OutBlock.InitDefaults();
}

void FACRunStatUtils::ComposeBaseStats(const UUnitDefinitionBase* Definition, int32 Level,
                                       const TArray<FACRunStatModifier>& StatModifiers,
                                       const FACStatBlock& Fallback, FACStatBlock& OutBlock)
{
    // 1) 等级基础值：StatsByLevel 的下标即 Level（0=D … 4=S）。
    if (Definition != nullptr && Definition->StatsByLevel.IsValidIndex(Level)
        && Definition->StatsByLevel[Level].Values.Num() > 0)
    {
        OutBlock = Definition->StatsByLevel[Level];
    }
    else
    {
        OutBlock = Fallback;
    }

    // 兜底：长度不足时补齐，避免后续 Get/Set 越界（FACStatBlock::Set 也会自愈，这里显式做一次更直观）。
    if (OutBlock.Values.Num() < ACStatCount)
    {
        OutBlock.Values.SetNumZeroed(ACStatCount);
    }

    // 2) 固定加成（Add）先于百分比，保证"百分比放大的是最终基础值"。
    for (const FACRunStatModifier& Modifier : StatModifiers)
    {
        if (Modifier.Op != EACModOp::Add)
        {
            continue;
        }
        const float Current = OutBlock.Get(Modifier.Stat);
        OutBlock.Set(Modifier.Stat, Current + Modifier.Value);
    }

    // 3) 百分比（MulPct，乘算）。
    for (const FACRunStatModifier& Modifier : StatModifiers)
    {
        if (Modifier.Op != EACModOp::MulPct)
        {
            continue;
        }
        const float Current = OutBlock.Get(Modifier.Stat);
        OutBlock.Set(Modifier.Stat, Current * (1.f + Modifier.Value));
    }
}

void FACRunStatUtils::ApplyResurrectPenalty(TArray<FACRunStatModifier>& InOutModifiers, float PenaltyPercent)
{
    const float Scale = FMath::Clamp(1.f - PenaltyPercent / 100.f, 0.f, 1.f);
    for (FACRunStatModifier& Modifier : InOutModifiers)
    {
        switch (Modifier.Op)
        {
        case EACModOp::Add:
            Modifier.Value *= Scale;
            break;
        case EACModOp::MulPct:
            // 惩罚作用在"增益幅度"上：+10% → +9.9%，而不是把乘数本身缩放。
            Modifier.Value *= Scale;
            break;
        case EACModOp::Override:
        default:
            // 覆盖值不参与惩罚（它本来就表示一个确定值），保持语义可读。
            break;
        }
    }
}

int32 FACRunStatUtils::GetEquipSlotMax(const UOperatorDefinition* Definition, int32 Level)
{
    if (Definition == nullptr)
    {
        return Level >= 4 ? 4 : 1;    // 无资产时的近似：D 级 1 格、S 级 4 格
    }
    return Definition->GetEquipSlotMax(Level);
}

int32 FACRunStatUtils::GetTierIndexForLevel(int32 Level)
{
    // C 级强化从 C 级开始生效，覆盖 C→B→A→S 共 4 档（《系统结构说明》§5.11）。
    return FMath::Clamp(Level - 1, 0, 3);
}
