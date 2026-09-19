#include "Run/ACRunSquad.h"
#include "Run/ACRunConfig.h"
#include "Run/ACRunStatUtils.h"
#include "Content/ACOperatorLibrary.h"
// 阶段 3.3：`FACContentUpgradeOption`（个性强化候选：GE 数组 + 被动能力数组）定义在这里，
// 按值返回需要完整类型。
#include "Content/ACBattleContentLibrary.h"
#include "Core/ACDataTypes.h"
#include "UObject/NameTypes.h"   // FNameLexicalLess

void FACRunSquad::Initialize(const UACRunConfig* InConfig, const UACOperatorLibrary* InLibrary)
{
    Config = InConfig;
    Library = InLibrary;
    Operators.Reset();
    ActiveOperatorIds.Reset();
    BenchOperatorIds.Reset();
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

const FACRunOperator* FACRunSquad::Find(const FName OperatorId) const
{
    return Operators.FindByPredicate([OperatorId](const FACRunOperator& Candidate)
    {
        return Candidate.OperatorId == OperatorId;
    });
}

FACRunOperator* FACRunSquad::FindMutable(const FName OperatorId)
{
    return Operators.FindByPredicate([OperatorId](const FACRunOperator& Candidate)
    {
        return Candidate.OperatorId == OperatorId;
    });
}

bool FACRunSquad::HasAnyAlive() const
{
    for (const FACRunOperator& Operator : Operators)
    {
        if (!Operator.bDead)
        {
            return true;
        }
    }
    return false;
}

void FACRunSquad::GetDeadOperatorIds(TArray<FName>& OutIds) const
{
    OutIds.Reset();
    for (const FACRunOperator& Operator : Operators)
    {
        if (Operator.bDead)
        {
            OutIds.Add(Operator.OperatorId);
        }
    }
}

bool FACRunSquad::HasRoomForNewOperator() const
{
    const int32 ActiveCapacity = Config != nullptr ? Config->GetActiveCapacity() : 3;
    const int32 BenchCapacity = Config != nullptr ? Config->GetBenchCapacity() : 3;
    return ActiveOperatorIds.Num() < ActiveCapacity || BenchOperatorIds.Num() < BenchCapacity;
}

// ---------------------------------------------------------------------------
// 编成
// ---------------------------------------------------------------------------

FACRunRecruitOutcome FACRunSquad::RecruitOperator(FName OperatorId)
{
    FACRunRecruitOutcome Outcome;
    if (OperatorId.IsNone())
    {
        return Outcome;
    }

    // ① 重复招募 = 升级（《系统结构说明》§5.11）。
    if (FACRunOperator* Existing = FindMutable(OperatorId))
    {
        if (Existing->Level >= 4)
        {
            // S 级为终态：重复招募不再有效（正式版本应转为"回收价/其他资源"）。
            Outcome.bSuccess = false;
            Outcome.NewLevel = Existing->Level;
            return Outcome;
        }
        ApplyLevelUp(*Existing);
        Outcome.bSuccess = true;
        Outcome.bLeveledUp = true;
        Outcome.NewLevel = Existing->Level;
        return Outcome;
    }

    // ② 新干员：需要空位。
    if (!HasRoomForNewOperator())
    {
        Outcome.bNoRoom = true;
        return Outcome;
    }

    const UOperatorDefinition* Definition = Library != nullptr ? Library->FindOperatorDefinition(OperatorId) : nullptr;
    const int32 ActiveCapacity = Config != nullptr ? Config->GetActiveCapacity() : 3;

    FACRunOperator NewOperator;
    NewOperator.OperatorId = OperatorId;
    NewOperator.DisplayName = Definition != nullptr
        ? Definition->DisplayName
        : FText::FromString(OperatorId.ToString());
    NewOperator.Level = 0;               // D 级
    NewOperator.TierIndex = 0;
    NewOperator.CurrentBaseHP = -1.f;    // 满血开局
    NewOperator.bDead = false;

    Operators.Add(NewOperator);

    // 优先上场，满员后才进备战席。
    if (ActiveOperatorIds.Num() < ActiveCapacity)
    {
        ActiveOperatorIds.Add(OperatorId);
        if (FACRunOperator* Added = FindMutable(OperatorId))
        {
            Added->BenchIndex = -1;
            Added->FormationCell = GetFormationCell(ActiveOperatorIds.Num() - 1);
        }
    }
    else
    {
        BenchOperatorIds.Add(OperatorId);
        if (FACRunOperator* Added = FindMutable(OperatorId))
        {
            Added->BenchIndex = BenchOperatorIds.Num() - 1;
        }
    }

    SortOperators();
    Outcome.bSuccess = true;
    Outcome.bLeveledUp = false;
    Outcome.NewLevel = 0;
    return Outcome;
}

void FACRunSquad::ApplyLevelUp(FACRunOperator& Operator)
{
    Operator.Level = FMath::Clamp(Operator.Level + 1, 0, 4);

    // 个性强化的数值档位随等级自动成长（C 级选定的强化覆盖 C→B→A→S 四档）。
    // 这里用"等级 → 档位"的直接映射：Level 0 = D（无强化），Level 1..4 = 档位 0..3。
    Operator.TierIndex = FMath::Clamp(Operator.Level - 1, 0, 3);

    // 升级到 C 级（Level 1）时选定个性强化；升到 S 级（Level 4）时替换为 S 级强化。
    // TODO(内容侧)：正式版本必须改成"三选一 UI"。当前取内容库的候选首项，
    // 目的是让自动演示无需人工输入即可跑通；接口 ResolveUpgradeChoice 已按档位参数化。
    //
    // 阶段 3.3：候选从 `FName` 效果块 Id 换成 `FACContentUpgradeOption`
    //（`Effects` = 施加的 GE，`GrantedAbilities` = 要授予的被动能力）。
    // 两者都写进 `FACRunOperator`，再由 `FACRunBattleAssembler` 搬进 `FACPlayerUnitSpec`。
    if (Operator.Level == 1 || Operator.Level == 4)
    {
        const FACContentUpgradeOption Choice = Library != nullptr
            ? Library->ResolveUpgradeChoice(Operator.OperatorId, Operator.TierIndex)
            : FACContentUpgradeOption();

        if (Operator.Level == 1)
        {
            // C 级：清空后写入（避免重复招募同一干员时叠加）。
            // ⚠️ 只清"个性强化"带来的那两份 —— `TraitEffects` / 词条能力由词条获取路径负责，
            //    它们的写入点不在本函数里，因此这里不动它们。
            Operator.UpgradeEffects.Reset();
            Operator.UpgradeGrantedAbilities.Reset();
        }
        for (const TSubclassOf<UGameplayEffect>& EffectClass : Choice.Effects)
        {
            if (EffectClass.Get() != nullptr)
            {
                // AddUnique：同一次升级被重复调用（例如重放自动演示）不会把同一条内容写两遍。
                Operator.UpgradeEffects.AddUnique(EffectClass);
            }
        }
        for (const TSubclassOf<UGameplayAbility>& AbilityClass : Choice.GrantedAbilities)
        {
            if (AbilityClass.Get() != nullptr)
            {
                Operator.UpgradeGrantedAbilities.AddUnique(AbilityClass);
            }
        }
    }

    // 升级回满血：等级提升会拉高最大生命值，沿用旧的基础血量会让干员"越练越残"。
    Operator.CurrentBaseHP = -1.f;
}

bool FACRunSquad::RemoveOperator(FName OperatorId)
{
    const int32 Removed = Operators.RemoveAll([OperatorId](const FACRunOperator& Candidate)
    {
        return Candidate.OperatorId == OperatorId;
    });
    if (Removed == 0)
    {
        return false;
    }
    ActiveOperatorIds.Remove(OperatorId);
    BenchOperatorIds.Remove(OperatorId);
    RebuildRosterIndex();
    return true;
}

bool FACRunSquad::SwapActive(FName ActiveId, FName BenchId)
{
    const int32 ActiveIndex = ActiveOperatorIds.IndexOfByKey(ActiveId);
    const int32 BenchIndex = BenchOperatorIds.IndexOfByKey(BenchId);
    if (ActiveIndex == INDEX_NONE || BenchIndex == INDEX_NONE)
    {
        return false;
    }

    ActiveOperatorIds[ActiveIndex] = BenchId;
    BenchOperatorIds[BenchIndex] = ActiveId;
    RebuildRosterIndex();
    return true;
}

bool FACRunSquad::PromoteToActive(FName OperatorId)
{
    const int32 Cap = Config != nullptr ? Config->GetActiveCapacity() : 3;
    const int32 BenchIndex = BenchOperatorIds.IndexOfByKey(OperatorId);
    if (BenchIndex == INDEX_NONE || ActiveOperatorIds.Num() >= Cap)
    {
        return false;
    }
    BenchOperatorIds.RemoveAt(BenchIndex);
    ActiveOperatorIds.Add(OperatorId);
    RebuildRosterIndex();
    return true;
}

bool FACRunSquad::DemoteToBench(FName OperatorId)
{
    const int32 Cap = Config != nullptr ? Config->GetBenchCapacity() : 3;
    const int32 ActiveIndex = ActiveOperatorIds.IndexOfByKey(OperatorId);
    if (ActiveIndex == INDEX_NONE || BenchOperatorIds.Num() >= Cap || BenchOperatorIds.Contains(OperatorId))
    {
        return false;
    }
    ActiveOperatorIds.RemoveAt(ActiveIndex);
    BenchOperatorIds.Add(OperatorId);
    RebuildRosterIndex();
    return true;
}

void FACRunSquad::PrepareForBattle()
{
    const int32 ActiveCapacity = Config != nullptr ? Config->GetActiveCapacity() : 3;

    // ① 阵亡者让出上场名额（保留在编队里，等待战后复活）。
    for (int32 Index = ActiveOperatorIds.Num() - 1; Index >= 0; --Index)
    {
        const FACRunOperator* Operator = Find(ActiveOperatorIds[Index]);
        if (Operator == nullptr || Operator->bDead)
        {
            if (!BenchOperatorIds.Contains(ActiveOperatorIds[Index]))
            {
                BenchOperatorIds.Add(ActiveOperatorIds[Index]);
            }
            ActiveOperatorIds.RemoveAt(Index);
        }
    }

    // ② 按备战席顺序补位（BenchOperatorIds 的顺序即玩家的编排顺序，确定性）。
    while (ActiveOperatorIds.Num() < ActiveCapacity)
    {
        int32 PromotedIndex = INDEX_NONE;
        for (int32 Index = 0; Index < BenchOperatorIds.Num(); ++Index)
        {
            const FACRunOperator* Candidate = Find(BenchOperatorIds[Index]);
            if (Candidate != nullptr && !Candidate->bDead)
            {
                PromotedIndex = Index;
                break;
            }
        }
        if (PromotedIndex == INDEX_NONE)
        {
            break;      // 没有可补的存活干员
        }
        ActiveOperatorIds.Add(BenchOperatorIds[PromotedIndex]);
        BenchOperatorIds.RemoveAt(PromotedIndex);
    }

    RebuildRosterIndex();
}

void FACRunSquad::RebuildRosterIndex()
{
    for (FACRunOperator& Operator : Operators)
    {
        const int32 ActiveIndex = ActiveOperatorIds.IndexOfByKey(Operator.OperatorId);
        if (ActiveIndex != INDEX_NONE)
        {
            Operator.BenchIndex = -1;
            Operator.FormationCell = GetFormationCell(ActiveIndex);
            Operator.Facing = EACFacing::Up;      // 我方默认朝上（行号减小方向）
            continue;
        }
        Operator.BenchIndex = BenchOperatorIds.IndexOfByKey(Operator.OperatorId);
        Operator.FormationCell = FACHexCoord();
    }
}

void FACRunSquad::SortOperators()
{
    Operators.Sort([](const FACRunOperator& A, const FACRunOperator& B)
    {
        return A.OperatorId.LexicalLess(B.OperatorId);
    });
}

// ---------------------------------------------------------------------------
// 养成
// ---------------------------------------------------------------------------

bool FACRunSquad::EquipItem(FName OperatorId, const FACRunEquipment& Equipment)
{
    FACRunOperator* Operator = FindMutable(OperatorId);
    if (Operator == nullptr || Equipment.EquipmentId.IsNone())
    {
        return false;
    }
    if (Operator->EquippedIds.Contains(Equipment.EquipmentId))
    {
        return false;
    }
    if (Operator->EquippedIds.Num() >= GetEquipSlotMax(OperatorId))
    {
        return false;
    }
    Operator->EquippedIds.Add(Equipment.EquipmentId);

    // 装备的属性加成直接折进永久修饰（战斗内核只看到"最终基础属性"）。
    for (const FACRunStatModifier& Modifier : Equipment.StatModifiers)
    {
        FACRunStatModifier Copy = Modifier;
        if (Copy.SourceId.IsNone())
        {
            Copy.SourceId = Equipment.EquipmentId;
        }
        Operator->PermanentModifiers.Add(Copy);
    }
    return true;
}

bool FACRunSquad::UnequipItem(FName OperatorId, FName EquipmentId)
{
    FACRunOperator* Operator = FindMutable(OperatorId);
    if (Operator == nullptr)
    {
        return false;
    }
    if (Operator->EquippedIds.Remove(EquipmentId) == 0)
    {
        return false;
    }
    Operator->PermanentModifiers.RemoveAll([EquipmentId](const FACRunStatModifier& Modifier)
    {
        return Modifier.SourceId == EquipmentId;
    });
    return true;
}

bool FACRunSquad::ApplyPermanentModifier(FName OperatorId, const FACRunStatModifier& Modifier)
{
    FACRunOperator* Operator = FindMutable(OperatorId);
    if (Operator == nullptr)
    {
        return false;
    }
    Operator->PermanentModifiers.Add(Modifier);
    return true;
}

bool FACRunSquad::ApplyFragment(FName OperatorId, const FACRunFragment& Fragment)
{
    FACRunOperator* Operator = FindMutable(OperatorId);
    if (Operator == nullptr)
    {
        return false;
    }
    for (const FACRunStatModifier& Modifier : Fragment.StatModifiers)
    {
        FACRunStatModifier Copy = Modifier;
        if (Copy.SourceId.IsNone())
        {
            Copy.SourceId = Fragment.FragmentId;
        }
        Operator->PermanentModifiers.Add(Copy);
    }
    return true;
}

bool FACRunSquad::ReviveOperator(FName OperatorId, float PenaltyPercent)
{
    FACRunOperator* Operator = FindMutable(OperatorId);
    if (Operator == nullptr || !Operator->bDead)
    {
        return false;
    }

    Operator->bDead = false;
    Operator->CurrentBaseHP = -1.f;                       // 复活即满血
    FACRunStatUtils::ApplyResurrectPenalty(Operator->PermanentModifiers, PenaltyPercent);
    return true;
}

bool FACRunSquad::HealOperator(FName OperatorId, float PercentOfMaxHP)
{
    FACRunOperator* Operator = FindMutable(OperatorId);
    if (Operator == nullptr || Operator->bDead || PercentOfMaxHP <= 0.f)
    {
        return false;
    }

    const float MaxHP = FMath::Max(1.f, GetDerivedMaxHP(OperatorId));
    const float Current = Operator->CurrentBaseHP < 0.f ? MaxHP : Operator->CurrentBaseHP;
    Operator->CurrentBaseHP = FMath::Clamp(Current + MaxHP * PercentOfMaxHP / 100.f, 1.f, MaxHP);
    return true;
}

void FACRunSquad::RecordBattleResult(FName OperatorId, bool bDead, float RemainingBaseHP)
{
    FACRunOperator* Operator = FindMutable(OperatorId);
    if (Operator == nullptr)
    {
        return;
    }
    Operator->bDead = bDead;
    Operator->CurrentBaseHP = bDead ? 0.f : FMath::Max(0.f, RemainingBaseHP);
}

// ---------------------------------------------------------------------------
// 派生属性
// ---------------------------------------------------------------------------

void FACRunSquad::ComputeBaseStats(const FName OperatorId, FACStatBlock& OutBlock) const
{
    OutBlock.InitDefaults();

    const FACRunOperator* Operator = Find(OperatorId);
    if (Operator == nullptr)
    {
        if (Library != nullptr)
        {
            OutBlock = Library->GetDefaultStatBlock();
        }
        return;
    }

    // 等级基础属性：优先取定义资产的 StatsByLevel，无资产时取库里的兜底属性。
    FACStatBlock LevelBlock;
    if (Library != nullptr)
    {
        Library->GetStatsByLevel(OperatorId, Operator->Level, LevelBlock);
    }
    else
    {
        LevelBlock.InitDefaults();
    }

    FACRunStatUtils::ComposeBaseStats(Library != nullptr ? Library->FindOperatorDefinition(OperatorId) : nullptr,
                                      Operator->Level, Operator->PermanentModifiers, LevelBlock, OutBlock);
}

float FACRunSquad::GetDerivedMaxHP(const FName OperatorId) const
{
    FACStatBlock Block;
    ComputeBaseStats(OperatorId, Block);
    return Block.Get(EACStat::MaxHP);
}

FACHexCoord FACRunSquad::GetFormationCell(int32 ActiveIndex) const
{
    // 落位表在配置里，越界时回退到最后一个有效位，避免"第 4 名干员没地方站"。
    static const int32 DefaultRows[] = { 8, 8, 7 };
    static const int32 DefaultCols[] = { 2, 5, 3 };

    auto Sample = [ActiveIndex](const TArray<int32>& Values, const int32* Fallback, int32 FallbackCount) -> int32
    {
        if (Values.Num() > 0)
        {
            return Values[FMath::Clamp(ActiveIndex, 0, Values.Num() - 1)];
        }
        return Fallback[FMath::Clamp(ActiveIndex, 0, FallbackCount - 1)];
    };

    const int32 Row = Config != nullptr
        ? Sample(Config->PlayerDeployRows, DefaultRows, UE_ARRAY_COUNT(DefaultRows))
        : DefaultRows[FMath::Clamp(ActiveIndex, 0, 2)];
    const int32 Col = Config != nullptr
        ? Sample(Config->PlayerDeployCols, DefaultCols, UE_ARRAY_COUNT(DefaultCols))
        : DefaultCols[FMath::Clamp(ActiveIndex, 0, 2)];

    return FACHexCoord(Row, Col);
}

int32 FACRunSquad::GetEquipSlotMax(const FName OperatorId) const
{
    const FACRunOperator* Operator = Find(OperatorId);
    const int32 Level = Operator != nullptr ? Operator->Level : 0;
    const UOperatorDefinition* Definition = Library != nullptr ? Library->FindOperatorDefinition(OperatorId) : nullptr;
    return FACRunStatUtils::GetEquipSlotMax(Definition, Level);
}

void FACRunSquad::CollectUpgradeEffects(const FName OperatorId, TArray<TSubclassOf<UGameplayEffect>>& OutEffects) const
{
    OutEffects.Reset();
    if (const FACRunOperator* Operator = Find(OperatorId))
    {
        OutEffects.Append(Operator->UpgradeEffects);
        OutEffects.Append(Operator->TraitEffects);
    }
}

void FACRunSquad::CollectGrantedAbilities(const FName OperatorId, TArray<TSubclassOf<UGameplayAbility>>& OutAbilities) const
{
    OutAbilities.Reset();
    if (const FACRunOperator* Operator = Find(OperatorId))
    {
        OutAbilities.Append(Operator->UpgradeGrantedAbilities);
        OutAbilities.Append(Operator->TraitGrantedAbilities);
    }
}

FString FACRunSquad::ToDebugString() const
{
    FString Text = FString::Printf(TEXT("编队 上场%d/备战%d"), ActiveOperatorIds.Num(), BenchOperatorIds.Num());
    for (const FName& OperatorId : ActiveOperatorIds)
    {
        const FACRunOperator* Operator = Find(OperatorId);
        if (Operator == nullptr)
        {
            continue;
        }
        Text += FString::Printf(TEXT("  [%s Lv%d%s]"),
                                *Operator->DisplayName.ToString(),
                                Operator->Level,
                                Operator->bDead ? TEXT(" 阵亡") : TEXT(""));
    }
    return Text;
}
