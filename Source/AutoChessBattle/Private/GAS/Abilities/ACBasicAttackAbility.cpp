// 阶段 3.1b 新增（GAS 重构实施方案 §4.1 / §7 阶段 3.1）：普攻能力实现。
// 逐项对照表与三处刻意差异见头文件；本文件里的每一段都在注释里标了旧实现的出处行号。
#include "GAS/Abilities/ACBasicAttackAbility.h"

#include "GameplayEffect.h"

#include "Battle/ACBattleTime.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
#include "Combat/ACCombatResolver.h"
#include "Core/ACBattleTags.h"
#include "Core/ACDataTypes.h"
#include "Targeting/ACTargetingSystem.h"

UACBasicAttackAbility::UACBasicAttackAbility()
{
    // ⚠️ 本能力**只挡眩晕，不挡沉默** —— 这是对旧门控的忠实翻译（§4.1 的
    // "`CanCastSkill` / `CanAct` 的沉默眩晕门控 → `ActivationBlockedTags`"）：
    //   - 旧 `FAbilityExecutor::CanAct`（`ACAbilityExecutor.cpp:66-79`）门控**所有**行动
    //     （技能与普攻都走它），条件是"未死 / 未待死 / `IsStunned` / 无 `State.Stun`"；
    //   - 旧 `FAbilityExecutor::IsSilenced` 只出现在 `CanCastSkill`（:84）里 ——
    //     也就是说**沉默只挡技能，不挡普攻**。
    // 把 `State.Silence` 也加进来会凭空创造一条"沉默后不能普攻"的新规则（行为不等价）。
    ActivationBlockedTags.AddTag(BattleTags::State_Stun);

    // 旧的 `CanAct` 还判"未死 / 未待死 / 行动状态属于 Idle|Moving|Attacking"——
    // 那几条属于**行动资格**（调度器只在就绪且存活的单位上推进行动条），
    // 是内核的职责，不在能力里重复判：能力重复判会让"到底谁挡住了这次攻击"变得难以定位。
}

// ---------------------------------------------------------------------------
// 目标 / 射程（§4.1：来自 `FACAttackPatternDef`）
// ---------------------------------------------------------------------------

EACSelectorType UACBasicAttackAbility::GetTargetSelector(const AACBattleUnitBase& Caster) const
{
    // 治疗型普攻（玛恩娜伯爵·蓝血）：永远不选敌方，改选友方最低血
    //（M11 §5.1 / M14 §1.3；旧 `FAbilityExecutor::RequestAction` 的 :145-166）。
    // 注意 `EACTeam` 没有 "Ally" 枚举项（Player/Enemy/Neutral），"友方" = 与自身同阵营，
    // 因此判据是 `Pattern.TargetTeam == Caster.GetTeam()`（与旧代码一字不差）。
    const FACAttackPatternDef& Pattern = Caster.GetAttackPattern();
    if (Pattern.bHealAttack && Pattern.TargetTeam == Caster.GetTeam())
    {
        return EACSelectorType::LowestHpPercentAlly;
    }
    return EACSelectorType::PrimaryTarget;
}

float UACBasicAttackAbility::GetEffectiveRangeOverride(const AACBattleUnitBase& Caster) const
{
    const FACAttackPatternDef& Pattern = Caster.GetAttackPattern();
    if (Pattern.bHealAttack && Pattern.TargetTeam == Caster.GetTeam())
    {
        // 治疗型普攻的射程用**基础射程**：旧实现是
        // `IsInRange(Unit, *AllyTarget, Unit.GetBaseRange())`（:159），不看 PreferredRange。
        return Caster.GetBaseRange();
    }
    // 普通普攻：旧实现的 `DesiredRange`（:193）= `PreferredRange > 0 ? PreferredRange : BaseRange`。
    return Pattern.PreferredRange > 0.f ? Pattern.PreferredRange : Caster.GetBaseRange();
}

const TArray<FACAttackSegment>& UACBasicAttackAbility::GetSegments(const FACAttackPatternDef& Pattern)
{
    // 兜底段（旧 `ExecuteBasicAttack` 的 `static TArray<FACAttackSegment> DefaultSegments`，:273-278）：
    // 无普攻段配置时按 100% 攻击力物理伤害兜底。
    // 这里用"函数内静态 + 初始化即构造"而不是旧的 `if (Num() == 0) Add(...)`：
    // 后者的判据依赖一次可变的懒初始化，语义上等价（默认构造的 `FACAttackSegment` 就是
    // Multiplier = 1 / Physical / HitCount = 1 / bCanCrit = true，见 Core/ACDataTypes.h:189-207），
    // 但不需要每次调用都判一次数组长度。
    static const TArray<FACAttackSegment> DefaultSegments = []()
    {
        TArray<FACAttackSegment> Segments;
        Segments.Add(FACAttackSegment());
        return Segments;
    }();

    return (Pattern.Segments.Num() > 0) ? Pattern.Segments : DefaultSegments;
}

// ---------------------------------------------------------------------------
// 主流程（逐项对照旧 ExecuteBasicAttack，ACAbilityExecutor.cpp:254-342）
// ---------------------------------------------------------------------------

void UACBasicAttackAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                            const FGameplayAbilityActivationInfo ActivationInfo,
                                            const FGameplayEventData* TriggerEventData)
{
    // 普攻不走 cost / cooldown：`CommitAbility` 在这里**不调用**。
    // 旧实现的普攻也没有消耗（专注是技能的资源），调用它只会多一条"没有 Cost GE 所以恒真"的空转。
    // （攻击频率由 `FActionScheduler` 的攻速推进控制，不是能力的冷却。）

    UBattleWorld* const World = GetBattleWorld();
    AACBattleUnitBase* const Caster = GetCasterUnit();
    if (World == nullptr || Caster == nullptr)
    {
        // 没有世界 / 施法者不是战斗单位：不该发生（该能力只授予单位），但也不崩。
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }

    // 目标：由基类按 `GetTargetSelector` 解析（普通普攻 = PrimaryTarget / 治疗型 = LowestHpPercentAlly）。
    // `CanActivateAbility` 已经解析过一次（并做了射程判定），这里再解析一次拿**当前**目标：
    // 两次之间可能插入过钩子（BeforeAttack 的订阅者可以改容器），重复解析比缓存句柄安全。
    TArray<FUnitId> Targets;
    if (!ResolveTargets(*Caster, Targets) || Targets.Num() == 0)
    {
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }
    AACBattleUnitBase* const Target = World->FindUnit(Targets[0]);
    if (Target == nullptr)
    {
        // 目标可能在同一步内被移除（旧实现专门判过这种情况）。
        FinishAbility(/*bWasCancelled=*/true);
        return;
    }

    // 旧 :256
    Caster->SetActionState(EACUnitActionState::Attacking);

    // 旧 :257-262：攻击时朝向目标。`IsTargetValid` 仍然要判 —— 治疗型普攻的目标是**友方**，
    // 而 `IsTargetValid` 对同阵营返回 false（ACTargetingSystem.cpp:198），
    // 因此治疗型普攻不改朝向。这正是旧代码的行为（旧实现同样先过 IsTargetValid）。
    if (World->Targeting().IsTargetValid(*Caster, Target->GetUnitId()))
    {
        const int32 RowDelta = static_cast<int32>(Target->GetCell().Row) - static_cast<int32>(Caster->GetCell().Row);
        Caster->SetFacing(RowDelta >= 0 ? EACFacing::Down : EACFacing::Up);
    }

    // 旧 :264-270：攻击前钩子。**静电紊乱的触发点之一**（`Hook.BeforeAttack`），
    // 另有 `Hook.AllyAttack`（订阅者按 Source 阵营过滤，实现 OnAllyAttack）。
    // 两者都走基类的"双发"入口：EventBus 给内核订阅者，GameplayEvent 给自己身上的被动。
    FBattleHookContext AttackContext;
    AttackContext.Source = Caster->GetUnitId();
    AttackContext.Target = Target->GetUnitId();
    AttackContext.Time = FACBattleTime::ElapsedSeconds(*World);
    DispatchHook(BattleTags::Hook_BeforeAttack, AttackContext, Caster);
    DispatchHook(BattleTags::Hook_AllyAttack, AttackContext, Caster);

    const FACAttackPatternDef& Pattern = Caster->GetAttackPattern();

    if (Pattern.bHealAttack)
    {
        // 旧 :280-288：治疗型普攻回 `ATK × HealRatio`（`HealRatio` 默认 0.5，见 Core/ACDataTypes.h:233）。
        // 走 `FCombatResolver::ApplyHeal`（含过量治疗转化），不产生伤害、不派发 Hit/Crit 钩子 ——
        // 旧实现在这个分支里也**没有**段循环，因此同样没有那两条钩子。
        FHealRequest HealRequest;
        HealRequest.Source = Caster->GetUnitId();
        HealRequest.Target = Target->GetUnitId();
        HealRequest.RawAmount = Caster->GetStat(EACStat::ATK) * Pattern.HealRatio;
        HealRequest.SourceEffectBlockId = FName(TEXT("BasicAttackHeal"));
        World->Combat().ApplyHeal(HealRequest);
    }
    else
    {
        // 旧 :291-334：多段 × 每段 HitCount 次。
        const TArray<FACAttackSegment>& Segments = GetSegments(Pattern);
        for (const FACAttackSegment& Segment : Segments)
        {
            const int32 Hits = FMath::Max(1, Segment.HitCount);
            for (int32 HitIndex = 0; HitIndex < Hits; ++HitIndex)
            {
                // 旧 :296-304：六项逐字对应。
                FDamageRequest DamageRequest;
                DamageRequest.Source = Caster->GetUnitId();
                DamageRequest.Target = Target->GetUnitId();
                DamageRequest.DamageType = Segment.DamageType;
                DamageRequest.Reason = EACDamageReason::BasicAttack;
                DamageRequest.RawAmount = Caster->GetStat(EACStat::ATK) * Segment.Multiplier;
                DamageRequest.bCanCrit = Segment.bCanCrit && Pattern.bCanCrit;
                DamageRequest.bIsBasicAttack = true;
                DamageRequest.SourceEffectBlockId = FName(TEXT("BasicAttack"));

                // ⚠️ 普攻伤害**直接走结算器**，不经过 `UACGE_InstantDamage`：
                // §4.6 的新契约是"只有 `FCombatResolver` 能改 `Health`"，
                // 而伤害 GE 的目的地也是同一个结算器 —— 普攻这条路本来就持有组请求所需的全部
                // 上下文（攻击者/目标/段倍率/暴击开关），套一层 GE 只会多一次 spec 分配与一次
                // 上下文还原（`UACDamageExecution` 还得从 ASC 反推 Source/Target）。
                const FDamageResult DamageResult = World->Combat().ApplyDamage(DamageRequest);

                // 旧 :309-315：段级附加效果（旧为效果块，新为 GE，见头文件的粒度差异说明）。
                ApplySegmentOnHitEffects(*Caster, *Target);

                if (DamageResult.bWasCrit)
                {
                    // 旧 :317-325：`FloatValue = 实际扣血`（不是原始量）。
                    FBattleHookContext CritContext;
                    CritContext.Source = Caster->GetUnitId();
                    CritContext.Target = Target->GetUnitId();
                    CritContext.Time = FACBattleTime::ElapsedSeconds(*World);
                    CritContext.FloatValue = DamageResult.AppliedHpLoss;
                    DispatchHook(BattleTags::Hook_Crit, CritContext, Caster);
                }

                // 旧 :327-332：`Hook.Hit` **每次命中都发**（多段多击就是多条），
                // 巨蛭的"每次普攻附加 2 层流血"挂在这里。
                FBattleHookContext HitContext;
                HitContext.Source = Caster->GetUnitId();
                HitContext.Target = Target->GetUnitId();
                HitContext.Time = FACBattleTime::ElapsedSeconds(*World);
                HitContext.FloatValue = DamageResult.AppliedHpLoss;
                DispatchHook(BattleTags::Hook_Hit, HitContext, Caster);
            }
        }
    }

    // 旧 :337-339：计数 + 行动后钩子 + 命中后回专注。
    Caster->IncrementAttackPatternHits();
    // ⚠️ 复用**同一个** `AttackContext`（旧实现也是同一个对象，`FloatValue` 保持 0）：
    // 订阅者在 BeforeAttack 里改过的字段会延续到 AfterAttack —— 这是旧行为，不改。
    DispatchHook(BattleTags::Hook_AfterAttack, AttackContext, Caster);
    OnBasicAttackHit(*Caster);

    // 旧 :341
    Caster->SetActionState(EACUnitActionState::Idle);
    FinishAbility(/*bWasCancelled=*/false);
}

void UACBasicAttackAbility::ApplySegmentOnHitEffects(AACBattleUnitBase& Caster, AACBattleUnitBase& Target)
{
    // 旧的 `EffectContext` 是 `{ Source = 攻击者, PrimaryTarget = 目标 }`
    //（`ACAbilityExecutor.cpp:311-314`），本函数把同一对来源/目标交给 GE spec。
    for (const TSubclassOf<UGameplayEffect>& EffectClass : OnHitEffects)
    {
        if (EffectClass.Get() == nullptr)
        {
            continue;
        }
        ApplyEffectToUnit(Caster, Target, EffectClass);
    }
}

void UACBasicAttackAbility::OnBasicAttackHit(AACBattleUnitBase& Caster)
{
    // A11：射手普攻回专注。优先取属性集的 `FocusPerAttack`（`EACStat::FocusPerAttack`），
    // 为 0 且远程时回退 `RuleConfig::ShooterFocusPerAttack`（旧实现回退 5.f，三级口径一致）。
    UBattleWorld* const World = FindBattleWorldFromActor(&Caster);
    float Gain = Caster.GetStat(EACStat::FocusPerAttack);
    if (Gain <= 0.f && Caster.IsRanged())
    {
        Gain = (World != nullptr && World->GetRuleConfig() != nullptr)
            ? World->GetRuleConfig()->ShooterFocusPerAttack
            : 5.f;
    }
    if (Gain > 0.f)
    {
        // 回专注的**改值走 GE、钩子与埋点在 `GrantFocusToUnit` 里**（旧 `FAbilityExecutor::OnBasicAttackHit`
        // 也是调 `GrantFocus`，那一处同时负责 Hook.FocusFull / Hook.FocusChanged / Focus 埋点）。
        GrantFocusToUnit(Caster, Gain);
    }
}
