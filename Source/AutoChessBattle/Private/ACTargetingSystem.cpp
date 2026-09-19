#include "Targeting/ACTargetingSystem.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "Diagnostics/ACBattleDiagnostics.h"
#include "Grid/ACBattleGrid.h"
#include "Stats/ACBattleStats.h"
#include "Core/ACBattleTags.h"
// 阶段 3.2a：嘲讽来源改从"状态 GE 的效果上下文 instigator"读（见下面的匿名命名空间），
// 需要 ASC 的 `GetActiveEffects` 与 `FActiveGameplayEffect::Spec`。
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"

namespace
{
    /**
     * 取"嘲讽来源单位"（阶段 3.2a）。
     *
     * 旧实现读的是 `Self.GetStates().Find(State.Taunt)->Source`（`FAbnormalStateInstance::Source`），
     * 而状态实例数组随 `FAbnormalStateContainer` 一起删除了。新链路里"谁施加的这个状态"
     * 落在状态 GE 的效果上下文 instigator 上（`UACBattleAbility::ApplyEffectToUnit` 把
     * context 的 instigator 设成来源单位），因此：
     *
     *   ① 先按 `GrantedTags`/owning tags 找到该单位身上匹配 `State.Taunt` 的活动 GE
     *      （`GetActiveEffectsWithAllTags` 只返回句柄，`FGameplayEffectQuery` 才能按
     *       owning tags 匹配 —— `GameplayEffect.h:1505`）；
     *   ② 从 `FActiveGameplayEffect::Spec.GetEffectContext()`（`:1101`）取 instigator Actor
     *      （`GameplayEffectTypes.h:284` 的 `GetInstigator`），再 `Cast<AACBattleUnitBase>`。
     *
     * 拿不到任何一项就返回 `InvalidUnitId` = "没有嘲讽来源"，调用方按"无嘲讽"处理 ——
     * 与旧实现"找不到实例就不进嘲讽分支"的行为一致。
     *
     * ⚠️ 为什么不在 `AACBattleUnitBase` 上做成通用接口：这个查询需要 GAS 的完整类型
     *（`FActiveGameplayEffect` 与 `FGameplayEffectQuery`），而 `ACBattleUnitBase.h` 刻意只
     * 暴露"两个标签计数查询"这两条不依赖 GAS 细节的口子（见该头文件的说明）。
     * 只有嘲讽需要"来源"，因此把它留在唯一的使用方这里。
     */
    FUnitId ResolveTauntSourceUnitId(const AACBattleUnitBase& Self)
    {
        const UAbilitySystemComponent* const ASC = Self.GetAbilitySystemComponent();
        if (ASC == nullptr)
        {
            return InvalidUnitId;
        }

        FGameplayTagContainer TauntTags;
        TauntTags.AddTag(BattleTags::State_Taunt);

        const TArray<FActiveGameplayEffectHandle> Handles =
            ASC->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(TauntTags));
        for (const FActiveGameplayEffectHandle& Handle : Handles)
        {
            const FActiveGameplayEffect* const ActiveEffect = ASC->GetActiveGameplayEffect(Handle);
            if (ActiveEffect == nullptr || ActiveEffect->Spec.Def == nullptr)
            {
                continue;
            }
            if (const AActor* const Instigator =
                    ActiveEffect->Spec.GetEffectContext().GetInstigator())
            {
                if (const AACBattleUnitBase* const SourceUnit = Cast<AACBattleUnitBase>(Instigator))
                {
                    return SourceUnit->GetUnitId();
                }
            }
        }
        return InvalidUnitId;
    }
}

void FTargetingSystem::Initialize(UBattleWorld* InWorld)
{
    World = InWorld;
}

int32 FTargetingSystem::GetDistance(const AACBattleUnitBase& A, const AACBattleUnitBase& B) const
{
    return UACHexGridStatics::Distance(A.GetCell(), B.GetCell());
}

bool FTargetingSystem::MatchesFilter(const AACBattleUnitBase& Self, const AACBattleUnitBase& Candidate, EACTargetFilter Filter) const
{
    if (Candidate.GetUnitId() == Self.GetUnitId())
    {
        return false;
    }

    switch (Filter)
    {
    case EACTargetFilter::Ally:    return Candidate.GetTeam() == Self.GetTeam();
    case EACTargetFilter::Enemy:   return Candidate.GetTeam() != Self.GetTeam() && Candidate.GetTeam() != EACTeam::Neutral;
    case EACTargetFilter::Alive:   return Candidate.IsAlive();
    case EACTargetFilter::Selectable:
        return Candidate.GetTeam() != Self.GetTeam() && Candidate.CanBeTargeted();
    case EACTargetFilter::IncludeUntargetable:
        return Candidate.GetTeam() != Self.GetTeam() && Candidate.IsAlive() && Candidate.bOnBoard;
    case EACTargetFilter::Any:
    default:                       return true;
    }
}

void FTargetingSystem::CollectCandidates(const AACBattleUnitBase& Self, EACTargetFilter Filter, TArray<FUnitId>& OutCandidates) const
{
    OutCandidates.Reset();
    if (World == nullptr)
    {
        return;
    }

    // 注册表顺序（UnitId 升序）保证确定性。
    World->ForEachAlive([this, &Self, Filter, &OutCandidates](AACBattleUnitBase& Candidate)
    {
        if (MatchesFilter(Self, Candidate, Filter))
        {
            OutCandidates.Add(Candidate.GetUnitId());
        }
    });
}

FACScoreWeights FTargetingSystem::GetWeightsFor(const AACBattleUnitBase& Self) const
{
    FACScoreWeights Weights;

    // A17：刺客"最脆弱"以当前生命值为主键；距离仅作次级平局裁决，不与生命值同权。
    if (Self.HasTag(BattleTags::Unit_Class_Assassin))
    {
        Weights.CurrentHpWeight = -1.f;
        Weights.DistanceWeight = 0.f;
        return Weights;
    }

    // 其他单位：优先就近。
    Weights.DistanceWeight = -1.f;
    return Weights;
}

float FTargetingSystem::ScoreCandidate(const AACBattleUnitBase& Self, const AACBattleUnitBase& Candidate, const FACScoreWeights& Weights) const
{
    float Score = 0.f;

    if (!FMath::IsNearlyZero(Weights.CurrentHpWeight))
    {
        Score += Weights.CurrentHpWeight * Candidate.GetCurrentHP();
    }
    if (!FMath::IsNearlyZero(Weights.DistanceWeight))
    {
        Score += Weights.DistanceWeight * static_cast<float>(GetDistance(Self, Candidate));
    }
    // 坦克被优先锁定。
    if (Candidate.HasTag(BattleTags::Unit_Class_Tank))
    {
        Score += 0.5f;
        Score += Weights.ThreatBias;
    }

    return Score;
}

void FTargetingSystem::SortCandidates(TArray<FUnitId>& Candidates, const AACBattleUnitBase& Self) const
{
    if (World == nullptr)
    {
        return;
    }

    const FACScoreWeights Weights = GetWeightsFor(Self);

    // 先算分再排序（decorate-sort-undecorate）：比较器只用整数与精确比较。
    // 满足 strict weak ordering 要求；同时避免每对比较都重复查表/算分。
    struct FScoredCandidate
    {
        FUnitId UnitId = InvalidUnitId;
        int32 ScoreMilli = 0;
        int32 Distance = 0;
    };

    TArray<FScoredCandidate> Scored;
    Scored.Reserve(Candidates.Num());
    for (const FUnitId CandidateId : Candidates)
    {
        const AACBattleUnitBase* Candidate = World->FindUnit(CandidateId);
        if (Candidate == nullptr)
        {
            continue;
        }
        FScoredCandidate Entry;
        Entry.UnitId = CandidateId;
        // 量化到千分位，消除浮点近等带来的非传递比较。
        Entry.ScoreMilli = FMath::RoundToInt(ScoreCandidate(Self, *Candidate, Weights) * 1000.f);
        Entry.Distance = GetDistance(Self, *Candidate);
        Scored.Add(Entry);
    }

    // 平局裁决：A17 -> 生命百分比升序 -> 距离升序 -> UnitId 升序（全序，稳定）。
    Scored.StableSort([this, &Self](const FScoredCandidate& A, const FScoredCandidate& B)
    {
        if (A.ScoreMilli != B.ScoreMilli)
        {
            return A.ScoreMilli > B.ScoreMilli;
        }
        const AACBattleUnitBase* UnitA = World->FindUnit(A.UnitId);
        const AACBattleUnitBase* UnitB = World->FindUnit(B.UnitId);
        const float RatioA = UnitA != nullptr ? UnitA->GetHealthRatio() : 1.f;
        const float RatioB = UnitB != nullptr ? UnitB->GetHealthRatio() : 1.f;
        if (!FMath::IsNearlyEqual(RatioA, RatioB))
        {
            return RatioA < RatioB;
        }
        if (A.Distance != B.Distance)
        {
            return A.Distance < B.Distance;
        }
        return A.UnitId < B.UnitId;
    });

    Candidates.Reset();
    Candidates.Reserve(Scored.Num());
    for (const FScoredCandidate& Entry : Scored)
    {
        Candidates.Add(Entry.UnitId);
    }
}

FUnitId FTargetingSystem::AcquireTarget(AACBattleUnitBase& Self)
{
    if (World == nullptr)
    {
        return InvalidUnitId;
    }

    // 1) 嘲讽：强制锁定嘲讽来源。
    //    阶段 3.2a：`Self.GetStates().Find(...)->Source` → `ResolveTauntSourceUnitId(Self)`
    //    （从活动状态 GE 的效果上下文 instigator 取，见文件头匿名命名空间的说明）。
    const FUnitId TauntSourceId = ResolveTauntSourceUnitId(Self);
    if (TauntSourceId != InvalidUnitId)
    {
        AACBattleUnitBase* TauntSource = World->FindUnit(TauntSourceId);
        if (TauntSource != nullptr && TauntSource->IsAlive() && TauntSource->GetTeam() != Self.GetTeam())
        {
            return TauntSource->GetUnitId();
        }
    }

    // 2) 候选过滤 + 评分。
    TArray<FUnitId> Candidates;
    CollectCandidates(Self, EACTargetFilter::Selectable, Candidates);
    if (Candidates.Num() == 0)
    {
        return InvalidUnitId;
    }

    SortCandidates(Candidates, Self);
    return Candidates[0];
}

bool FTargetingSystem::IsTargetValid(const AACBattleUnitBase& Self, FUnitId TargetId) const
{
    if (World == nullptr || TargetId == InvalidUnitId)
    {
        return false;
    }
    const AACBattleUnitBase* Target = World->FindUnit(TargetId);
    if (Target == nullptr || !Target->IsAlive() || Target->GetTeam() == Self.GetTeam())
    {
        return false;
    }
    if (!Target->CanBeTargeted())
    {
        return false;
    }

    // 嘲讽期间目标必须为嘲讽来源。
    // 阶段 3.2a：同上，改从状态 GE 的 instigator 取来源单位。
    const FUnitId TauntSourceId = ResolveTauntSourceUnitId(Self);
    if (TauntSourceId != InvalidUnitId)
    {
        return TauntSourceId == TargetId;
    }
    return true;
}

void FTargetingSystem::ResolveSelector(EACSelectorType Selector, const AACBattleUnitBase& Self, int32 Radius, TArray<FUnitId>& OutTargets) const
{
    ResolveSelectorOnOrigin(Selector, Self, Self.GetUnitId(), Radius, OutTargets);
}

void FTargetingSystem::ResolveSelectorOnOrigin(EACSelectorType Selector, const AACBattleUnitBase& Self, FUnitId OriginId,
                                               int32 Radius, TArray<FUnitId>& OutTargets) const
{
    OutTargets.Reset();
    if (World == nullptr)
    {
        return;
    }

    const AACBattleUnitBase* Origin = World->FindUnit(OriginId);

    switch (Selector)
    {
    case EACSelectorType::Self:
        OutTargets.Add(Self.GetUnitId());
        break;

    case EACSelectorType::PrimaryTarget:
        if (IsTargetValid(Self, Self.GetCurrentTargetId()))
        {
            OutTargets.Add(Self.GetCurrentTargetId());
        }
        break;

    case EACSelectorType::FrontAdjacent:
    case EACSelectorType::BackAdjacent:
    {
        if (Origin == nullptr) { break; }
        const FACHexCoord Cell = (Selector == EACSelectorType::FrontAdjacent)
            ? UACHexGridStatics::GetFrontCell(Origin->GetCell(), Origin->GetFacing())
            : UACHexGridStatics::GetBackCell(Origin->GetCell(), Origin->GetFacing());
        const FUnitId Occupant = World->Grid().GetOccupant(Cell);
        if (Occupant != InvalidUnitId)
        {
            OutTargets.Add(Occupant);
        }
        break;
    }

    case EACSelectorType::Neighbors1:
    case EACSelectorType::Neighbors2:
    case EACSelectorType::SameRow:
    case EACSelectorType::FrontCone:
    {
        if (Origin == nullptr) { break; }
        FACGridQuery Query;
        Query.Origin = Origin->GetCell();
        Query.Facing = Origin->GetFacing();
        Query.Radius = Radius;
        Query.bIncludeOrigin = false;
        switch (Selector)
        {
        case EACSelectorType::Neighbors1: Query.Type = EACGridQueryType::Neighbors1; break;
        case EACSelectorType::Neighbors2: Query.Type = EACGridQueryType::Neighbors2; break;
        case EACSelectorType::SameRow:    Query.Type = EACGridQueryType::SameRow; break;
        default:                          Query.Type = EACGridQueryType::FrontCone; break;
        }

        TArray<FACHexCoord> Cells;
        World->Grid().QueryCells(Query, Cells);
        for (const FACHexCoord& Cell : Cells)
        {
            const FUnitId Occupant = World->Grid().GetOccupant(Cell);
            if (Occupant == InvalidUnitId)
            {
                continue;
            }
            const AACBattleUnitBase* Unit = World->FindUnit(Occupant);
            if (Unit != nullptr && Unit->IsAlive())
            {
                OutTargets.AddUnique(Occupant);
            }
        }
        OutTargets.Sort();
        break;
    }

    case EACSelectorType::MostDenseCluster:
    {
        // 在场地内寻找半径 Radius 内敌方数量最多的格子（并列取 (Row,Col) 最小）。
        TArray<FUnitId> Cells;
        FACHexCoord BestCell = Origin != nullptr ? Origin->GetCell() : FACHexCoord(0, 0);
        int32 BestCount = -1;

        for (int32 Row = 0; Row < UACHexGridStatics::Rows; ++Row)
        {
            for (int32 Col = 0; Col < UACHexGridStatics::Cols; ++Col)
            {
                const FACHexCoord Cell(Row, Col);
                if (!World->Grid().IsPlayableCell(Cell))
                {
                    continue;
                }
                int32 Count = 0;
                const TArray<FACHexCoord> Area = UACHexGridStatics::GetCellsInRadius(Cell, FMath::Max(1, Radius));
                for (const FACHexCoord& AreaCell : Area)
                {
                    const FUnitId Occupant = World->Grid().GetOccupant(AreaCell);
                    const AACBattleUnitBase* OccupantUnit = World->FindUnit(Occupant);
                    if (OccupantUnit != nullptr && OccupantUnit->IsAlive() && OccupantUnit->GetTeam() != Self.GetTeam())
                    {
                        ++Count;
                    }
                }
                if (Count > BestCount)
                {
                    BestCount = Count;
                    BestCell = Cell;
                }
            }
        }
        // 返回该区域的敌人。
        const TArray<FACHexCoord> Area = UACHexGridStatics::GetCellsInRadius(BestCell, FMath::Max(1, Radius));
        for (const FACHexCoord& AreaCell : Area)
        {
            const FUnitId Occupant = World->Grid().GetOccupant(AreaCell);
            const AACBattleUnitBase* OccupantUnit = World->FindUnit(Occupant);
            if (OccupantUnit != nullptr && OccupantUnit->IsAlive() && OccupantUnit->GetTeam() != Self.GetTeam())
            {
                OutTargets.AddUnique(Occupant);
            }
        }
        OutTargets.Sort();
        break;
    }

    case EACSelectorType::LowestHpPercentAlly:
    case EACSelectorType::HighestDamageDealtAlly:
    {
        TArray<FUnitId> Candidates;
        CollectCandidates(Self, EACTargetFilter::Ally, Candidates);
        Candidates.Add(Self.GetUnitId());

        FUnitId BestId = InvalidUnitId;
        float BestValue = (Selector == EACSelectorType::LowestHpPercentAlly) ? MAX_flt : -1.f;
        for (const FUnitId CandidateId : Candidates)
        {
            const AACBattleUnitBase* Candidate = World->FindUnit(CandidateId);
            if (Candidate == nullptr || !Candidate->IsAlive())
            {
                continue;
            }
            const float Value = (Selector == EACSelectorType::LowestHpPercentAlly)
                ? Candidate->GetHealthRatio()
                : World->Stats().GetDamageDealt(CandidateId);

            const bool bBetter = (Selector == EACSelectorType::LowestHpPercentAlly) ? (Value < BestValue) : (Value > BestValue);
            if (bBetter || BestId == InvalidUnitId)
            {
                BestValue = Value;
                BestId = CandidateId;
            }
        }
        if (BestId != InvalidUnitId)
        {
            OutTargets.Add(BestId);
        }
        break;
    }

    case EACSelectorType::MarkedTarget:
    {
        // 原生 Tag 常量，避免每次解析都做字符串查表（TechDocs/00A §5）。
        const FGameplayTag MarkedTag = BattleTags::Unit_Tag_Marked;
        TArray<FUnitId> Candidates;
        CollectCandidates(Self, EACTargetFilter::Selectable, Candidates);
        for (const FUnitId CandidateId : Candidates)
        {
            const AACBattleUnitBase* Candidate = World->FindUnit(CandidateId);
            if (Candidate != nullptr && Candidate->HasTag(MarkedTag))
            {
                OutTargets.Add(CandidateId);
            }
        }
        break;
    }

    case EACSelectorType::AllEnemiesInRange:
    case EACSelectorType::AllAlliesInRange:
    {
        if (Origin == nullptr) { break; }
        const EACTargetFilter Filter = (Selector == EACSelectorType::AllEnemiesInRange)
            ? EACTargetFilter::IncludeUntargetable
            : EACTargetFilter::Ally;
        CollectCandidates(Self, Filter, OutTargets);
        OutTargets.RemoveAll([this, Origin, Radius](const FUnitId CandidateId)
        {
            const AACBattleUnitBase* Candidate = World->FindUnit(CandidateId);
            return Candidate == nullptr
                || UACHexGridStatics::Distance(Origin->GetCell(), Candidate->GetCell()) > FMath::Max(1, Radius);
        });
        OutTargets.Sort();
        break;
    }

    case EACSelectorType::OwnerOf:
    {
        // 只返回仍然存在且存活的所有者，避免调用方拿到已离场单位的 UnitId。
        if (Origin != nullptr && Origin->GetOwnerUnitId() != InvalidUnitId)
        {
            const AACBattleUnitBase* OwnerUnit = World->FindUnit(Origin->GetOwnerUnitId());
            if (OwnerUnit != nullptr && OwnerUnit->IsAlive())
            {
                OutTargets.Add(Origin->GetOwnerUnitId());
            }
        }
        break;
    }

    default:
        break;
    }
}

void FBattleAISystem::Initialize(UBattleWorld* InWorld)
{
    World = InWorld;
}

EACActionIntent FBattleAISystem::Decide(const AACBattleUnitBase& Unit) const
{
    // 阶段 3.2b：`Unit.IsStunned()`（`bStunned` 成员）已删除，改查 ASC 标签
    //（`UACGE_State_Stun` 授予的 `State.Stun`）—— 与调度器门控、能力门控同一条判据。
    if (World == nullptr || Unit.IsDead() || Unit.HasStateTag(BattleTags::State_Stun))
    {
        return EACActionIntent::Skip;
    }

    // 决策顺序：可放技能 -> 目标在射程内普攻 -> 移动 -> 等待。
    // 具体执行由 `UBattleWorld::ExecuteActionForUnit` 完成（阶段 3.2b：旧 `FAbilityExecutor::RequestAction`
    // 已删除，改为"技能能力 → 普攻能力 → 移动"三段，含技能失败 fallback 普攻）；
    // 此处仅返回意图供调试与扩展行为标记。
    const AACBattleUnitBase* Target = World->FindUnit(Unit.GetCurrentTargetId());
    if (Target == nullptr || !Target->IsAlive())
    {
        return EACActionIntent::Wait;
    }
    const int32 Distance = UACHexGridStatics::Distance(Unit.GetCell(), Target->GetCell());
    if (static_cast<float>(Distance) <= Unit.GetBaseRange())
    {
        return EACActionIntent::BasicAttack;
    }
    return EACActionIntent::Move;
}
