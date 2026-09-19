// 阶段 3.2a 改造（GAS 重构实施方案 §3.1「D 行」/ §7 阶段 3.2）：
// 旧的"效果块库"通道（`SetEffectLibrary` / `SetCodeEffectLibrary` / `BuildMergedEffectLibrary`）
// 与异常状态注册（`RegisterAbnormalState`）全部删除 —— 它们承载的两套数据类型
// （`FACEffectBlock` / `UAbnormalStateDefinition`）在 `Core/ACDataTypes.h` 里也不存在了。
// 现在这个子系统的职责只剩三件：规则配置、内容定义注册（含能力清单）、组装数据上下文。
#include "Data/ACBattleDataSubsystem.h"

#include "GAS/ACAbilitySet.h"

void UBattleDataSubsystem::SetRuleConfig(UBattleRuleConfig* InRuleConfig)
{
    RuleConfig = InRuleConfig;
}

void UBattleDataSubsystem::RegisterAbilitySet(FName DefinitionId, UACAbilitySet* InAbilitySet)
{
    if (DefinitionId.IsNone() || InAbilitySet == nullptr)
    {
        // 空 Id 或空清单：内容侧漏填。静默跳过（注册阶段不告警，理由见 .h 的说明），
        // 但**不覆盖已有的有效项** —— `Add` 的覆盖语义会让"后注册的空壳"悄悄顶掉真清单。
        return;
    }
    AbilitySets.Add(DefinitionId, InAbilitySet);
}

void UBattleDataSubsystem::RegisterUnitDefinition(UUnitDefinitionBase* InDefinition)
{
    if (InDefinition != nullptr && !InDefinition->DefinitionId.IsNone())
    {
        UnitDefinitions.RemoveAll([InDefinition](const TObjectPtr<UUnitDefinitionBase>& Existing)
        {
            return Existing == InDefinition;
        });
        UnitDefinitions.Add(InDefinition);
    }
}

void UBattleDataSubsystem::RegisterSummonDefinition(USummonDefinition* InDefinition)
{
    if (InDefinition != nullptr && !InDefinition->Spec.DefinitionId.IsNone())
    {
        SummonDefinitions.Add(InDefinition);
    }
}

const UBattleRuleConfig* UBattleDataSubsystem::GetRuleConfig() const
{
    if (RuleConfig != nullptr)
    {
        return RuleConfig;
    }

    if (DefaultRuleConfig == nullptr)
    {
        UBattleDataSubsystem* MutableThis = const_cast<UBattleDataSubsystem*>(this);
        MutableThis->DefaultRuleConfig = NewObject<UBattleRuleConfig>(MutableThis, TEXT("DefaultBattleRuleConfig"));
    }
    return DefaultRuleConfig;
}

FACBattleDataContext UBattleDataSubsystem::BuildContext() const
{
    FACBattleDataContext Context;
    Context.RuleConfig = GetRuleConfig();

    // 能力 / GE 授予清单：按定义 Id 拷进上下文（值拷贝 `TObjectPtr` 只是多一份引用，
    // 对象的生命周期仍由本子系统保证 —— 它是 GameInstance 级的，长于任何一场战斗）。
    Context.AbilitySets = AbilitySets;

    // 阶段 3.3：这里原来的 `Context.EffectBlockGEs = EffectBlockGEs;`（过渡映射表）
    // 已随字段类型改造删除 —— 契约里的效果字段现在直接是 GE 类，不需要翻译层。

    for (const TObjectPtr<UUnitDefinitionBase>& Definition : UnitDefinitions)
    {
        if (Definition != nullptr && !Definition->DefinitionId.IsNone())
        {
            Context.UnitDefinitions.Add(Definition->DefinitionId, Definition);
        }
    }

    for (const TObjectPtr<USummonDefinition>& Definition : SummonDefinitions)
    {
        if (Definition != nullptr && !Definition->Spec.DefinitionId.IsNone())
        {
            Context.SummonSpecs.Add(Definition->Spec.DefinitionId, Definition->Spec);
            Context.SummonBaseStats.Add(Definition->Spec.DefinitionId, Definition->BaseStats);
        }
    }

    return Context;
}
