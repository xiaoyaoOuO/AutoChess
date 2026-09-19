#include "Content/ACBattleContentLibrary.h"
#include "Data/ACBattleDataSubsystem.h"
#include "Core/ACBattleTags.h"
// 注册能力清单（`RegisterAbilitySet`）时需要 `UACAbilitySet` 的完整类型。
// 阶段 3.3：过渡映射表（`TMap<FName, TSubclassOf<UGameplayEffect>>` + `SetEffectBlockGEs`）
// 已删除，因此这里不再需要 `GameplayEffect.h`。
#include "GAS/ACAbilitySet.h"

void UACBattleContentLibrary::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    EnsureInitialized();
}

void UACBattleContentLibrary::EnsureInitialized()
{
    if (bBuilt)
    {
        return;
    }
    BuildAndRegister();
    bBuilt = true;
}

void UACBattleContentLibrary::BuildAndRegister()
{
    UGameInstance* GameInstance = GetGameInstance();
    UBattleDataSubsystem* DataSubsystem = GameInstance != nullptr
        ? GameInstance->GetSubsystem<UBattleDataSubsystem>()
        : nullptr;

    // ---- 1) 规则配置：留空表示沿用引擎侧缺省规则（300s 时限 / A9–A20 默认值）----
    // 需要覆盖时在这里 NewObject<UBattleRuleConfig> 并改字段，再 SetRuleConfig。

    // ---- 2) 干员 ----
    ACBattleContent::BuildOperators(Operators);

    // ---- 3) 敌人 ----
    // 注意：BuildEnemies 需要 TArray<TObjectPtr<UEnemyDefinition>>（强类型），
    // 不能直接传 OwnedDefinitions（TArray<TObjectPtr<UUnitDefinitionBase>>，模板不兼容）。
    ACBattleContent::BuildEnemies(Enemies, OwnedEnemies);
    for (const TObjectPtr<UEnemyDefinition>& Enemy : OwnedEnemies)
    {
        OwnedDefinitions.Add(Enemy);
    }

    // ---- 4) 召唤物 ----
    TMap<FName, TObjectPtr<USummonDefinition>> Summons;
    ACBattleContent::BuildSummons(Summons, OwnedSummons);

    // ---- 5) 能力 / 常驻 GE 清单（阶段 3.2a：替代旧的"效果块库 + 异常状态表"）----
    TArray<TObjectPtr<UACAbilitySet>> OwnedAbilitySets;
    ACBattleContent::BuildAbilitySets(AbilitySets, OwnedAbilitySets);

    // 阶段 3.3：这里原来的第 6 步"过渡映射表：Run 层仍是 FName 的效果块 Id → GE 类"
    // （`BuildEffectBlockGEs` + `SetEffectBlockGEs`）**已删除** —— 契约字段改成类引用之后，
    // 翻译层没有存在意义了。

    // ---- 6) 装备池与锻体碎片池（Run 层集市 / 锻体的内容来源）----
    ACBattleContent::BuildEquipmentPools(EquipmentPools);
    ACBattleContent::BuildFragmentPools(FragmentPools);

    // ---- 7) 注册给战斗数据子系统（战斗内核的唯一读取入口）----
    if (DataSubsystem == nullptr)
    {
        UE_LOG(LogTemp, Error, TEXT("[Content] UBattleDataSubsystem 不存在，代码侧内容未注册。"));
        return;
    }

    if (CodeRuleConfig != nullptr)
    {
        DataSubsystem->SetRuleConfig(CodeRuleConfig);
    }

    for (const TPair<FName, TObjectPtr<UACAbilitySet>>& Pair : AbilitySets)
    {
        if (Pair.Value != nullptr)
        {
            DataSubsystem->RegisterAbilitySet(Pair.Key, Pair.Value);
        }
    }

    // 阶段 3.3：这里原来还有一句 `DataSubsystem->SetEffectBlockGEs(EffectBlockGEs);`
    // （3.2a 的过渡映射表）—— **已删除**，内容侧现在直接给 GE / 能力类引用。

    for (const FACContentOperatorEntry& Entry : Operators)
    {
        if (Entry.Definition != nullptr)
        {
            DataSubsystem->RegisterUnitDefinition(Entry.Definition);
            OwnedDefinitions.Add(Entry.Definition);
        }
    }

    for (const TPair<FName, TObjectPtr<UEnemyDefinition>>& Pair : Enemies)
    {
        if (Pair.Value != nullptr)
        {
            DataSubsystem->RegisterUnitDefinition(Pair.Value);
        }
    }

    for (const TPair<FName, TObjectPtr<USummonDefinition>>& Pair : Summons)
    {
        if (Pair.Value != nullptr)
        {
            DataSubsystem->RegisterSummonDefinition(Pair.Value);
        }
    }

    UE_LOG(LogTemp, Log,
           TEXT("[Content] 代码侧内容已注册：干员 %d、敌人 %d、召唤物 %d、能力清单 %d。"),
           Operators.Num(), Enemies.Num(), Summons.Num(), AbilitySets.Num());
}

UACAbilitySet* UACBattleContentLibrary::FindAbilitySet(FName DefinitionId) const
{
    if (DefinitionId.IsNone())
    {
        return nullptr;
    }
    const TObjectPtr<UACAbilitySet>* Found = AbilitySets.Find(DefinitionId);
    return Found != nullptr ? Found->Get() : nullptr;
}

const FACContentOperatorEntry* UACBattleContentLibrary::FindOperator(FName OperatorId) const
{
    for (const FACContentOperatorEntry& Entry : Operators)
    {
        if (Entry.Definition != nullptr && Entry.Definition->DefinitionId == OperatorId)
        {
            return &Entry;
        }
    }
    return nullptr;
}

const UEnemyDefinition* UACBattleContentLibrary::FindEnemy(FName EnemyDefinitionId) const
{
    const TObjectPtr<UEnemyDefinition>* Found = Enemies.Find(EnemyDefinitionId);
    return Found != nullptr ? Found->Get() : nullptr;
}

FACContentUpgradeOption UACBattleContentLibrary::ResolveUpgradeChoice(FName OperatorId, int32 TierIndex) const
{
    const FACContentOperatorEntry* Entry = FindOperator(OperatorId);
    if (Entry == nullptr || Entry->UpgradeChoices.Num() == 0)
    {
        return FACContentUpgradeOption();
    }
    const int32 Index = FMath::Clamp(TierIndex, 0, Entry->UpgradeChoices.Num() - 1);
    return Entry->UpgradeChoices[Index];
}

void UACBattleContentLibrary::GetEquipmentByTier(int32 Tier, TArray<FACRunEquipment>& OutEquipment) const
{
    OutEquipment.Reset();
    if (const FACContentEquipmentPool* Pool = EquipmentPools.Find(Tier))
    {
        OutEquipment = Pool->Items;
    }
}

bool UACBattleContentLibrary::GetFragmentsByTier(int32 Tier, TArray<FACRunFragment>& OutFragments) const
{
    OutFragments.Reset();
    const FACContentFragmentPool* Pool = FragmentPools.Find(Tier);
    if (Pool == nullptr || Pool->Items.Num() == 0)
    {
        return false;
    }
    OutFragments = Pool->Items;
    return true;
}
