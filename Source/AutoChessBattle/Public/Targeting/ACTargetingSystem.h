// M14 索敌与战斗AI：目标获取、位置关系解析、行动决策。
// 规则：强制目标（嘲讽）覆盖一切；隐身/不可选中只影响"被锁定"，范围选择器可命中。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleTypes.h"

class UBattleWorld;
// 阶段 0b（D8）：单位参数一律显式写成 Actor 类型。本头只把 `AACBattleUnitBase&` 当函数参数用，
// 前向声明足够；**不能** include Battle/ACBattleUnitBase.h —— 它反过来 include 本文件，会成环。
class AACBattleUnitBase;

/** 目标选择上下文。 */
struct AUTOCHESSBATTLE_API FACTargetContext
{
    FUnitId Self = InvalidUnitId;
    EACTargetFilter Filter = EACTargetFilter::Enemy;
    int32 Radius = 1;
    bool bIncludeOrigin = false;
};

/** 索敌评分权重（默认值；后续可数据化到 TargetScoreRule）。 */
struct AUTOCHESSBATTLE_API FACScoreWeights
{
    float CurrentHpWeight = 0.f;    // 负值 = 越低越优（刺客）
    float DistanceWeight = -1.f;    // 负值 = 越近越优
    float ThreatBias = 0.f;         // 坦克被锁定权重
};

class AUTOCHESSBATTLE_API FTargetingSystem
{
public:
    void Initialize(UBattleWorld* InWorld);

    /** 索敌主入口：返回目标 UnitId；无合法目标返回 InvalidUnitId。 */
    FUnitId AcquireTarget(AACBattleUnitBase& Self);

    /** 锁定是否仍然有效。 */
    bool IsTargetValid(const AACBattleUnitBase& Self, FUnitId TargetId) const;

    /** 解析选择器（技能 / 效果块）。 */
    void ResolveSelector(EACSelectorType Selector, const AACBattleUnitBase& Self, int32 Radius, TArray<FUnitId>& OutTargets) const;

    /** 以指定原点解析位置关系型选择器。 */
    void ResolveSelectorOnOrigin(EACSelectorType Selector, const AACBattleUnitBase& Self, FUnitId OriginId, int32 Radius,
                                 TArray<FUnitId>& OutTargets) const;

    /** 距离（六边形最短步数）。 */
    int32 GetDistance(const AACBattleUnitBase& A, const AACBattleUnitBase& B) const;

private:
    void CollectCandidates(const AACBattleUnitBase& Self, EACTargetFilter Filter, TArray<FUnitId>& OutCandidates) const;
    float ScoreCandidate(const AACBattleUnitBase& Self, const AACBattleUnitBase& Candidate, const FACScoreWeights& Weights) const;
    void SortCandidates(TArray<FUnitId>& Candidates, const AACBattleUnitBase& Self) const;
    FACScoreWeights GetWeightsFor(const AACBattleUnitBase& Self) const;
    bool MatchesFilter(const AACBattleUnitBase& Self, const AACBattleUnitBase& Candidate, EACTargetFilter Filter) const;

    UBattleWorld* World = nullptr;
};

/** 简单的行动决策器（详细行为标记在后续迭代扩展）。 */
class AUTOCHESSBATTLE_API FBattleAISystem
{
public:
    void Initialize(UBattleWorld* InWorld);
    EACActionIntent Decide(const AACBattleUnitBase& Unit) const;

private:
    UBattleWorld* World = nullptr;
};
