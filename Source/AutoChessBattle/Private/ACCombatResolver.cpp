#include "Combat/ACCombatResolver.h"
#include "Battle/ACBattleTime.h"
#include "Battle/ACBattleUnitBase.h"
#include "Battle/ACBattleWorld.h"
// 阶段 4（D3）：`#include "Battle/ACBattleRng.h"` 已删除 —— 暴击判定改 `FMath::FRand()`（C3）。
#include "Events/ACBattleEventBus.h"
#include "GAS/ACBattleAttributeSet.h"
#include "Stats/ACBattleStats.h"
#include "Diagnostics/ACBattleDiagnostics.h"
#include "Core/ACBattleTags.h"

// ---------------------------------------------------------------------------
// 结构化日志埋点（阶段 0.1b）
//
// 为什么都在函数*返回前*埋：这三条路径中途都会派发钩子（Hook.BeforeDealDamage 等）。
// 而钩子可以取消结算或改值。写在返回前，记下的才是"最终生效"，不是会被推翻的中间值。
//
// Category / EventTag 用FName 字面量常量而不用FString::Printf。
//   FName 有全局名字表，重复构造同名字面量只做一次查表，热路径（每次伤害一次）不产生字符串拼接。
// ---------------------------------------------------------------------------
namespace
{
    /** 日志分类：伤害/ 治疗 / 护盾都在这里，比对脚本按 category 分组即可。*/
    const FName LogCategory_Combat(TEXT("Combat"));
    const FName LogEvent_Damage(TEXT("Damage"));
    const FName LogEvent_Heal(TEXT("Heal"));
    const FName LogEvent_Shield(TEXT("Shield"));

    /** 记一条战斗事件：时间取当前世界时间（阶段 0.5：由"固定步 tick"改成秒）。*/
    void LogCombatEvent(const UBattleWorld& World, FName EventTag, FUnitId Source, FUnitId Target,
                        float ValueA, float ValueB = 0.f, int32 IntValue = 0)
    {
        FBattleLogRecord Record;
        Record.Time = FACBattleTime::ElapsedSeconds(World);
        Record.Category = LogCategory_Combat;
        Record.EventTag = EventTag;
        Record.Source = Source;
        Record.Target = Target;
        Record.ValueA = ValueA;
        Record.ValueB = ValueB;
        Record.IntValue = IntValue;
        //TODO: 熟悉了项目之后在这里补上Log
    }
}

// ---------------------------------------------------------------------------
// FShieldPool
// ---------------------------------------------------------------------------

void FShieldPool::Initialize(UACBattleAttributeSet* InAttributeSet)
{
    AttributeSet = InAttributeSet;
    // 不在这里写 `Shield = 0`：注入发生在 `AACBattleUnitBase` 的构造期，
    // 那时属性集本身的初值还没写（属性集全部初值由 `InitializeFromStatBlock` /
    // `SetResourceAttribute` 在延迟构造期显式写入）。这里只记录落点。
}

void FShieldPool::Add(FShieldInstance Instance)
{
    Instance.InstanceId = NextInstanceId++;
    Instances.Add(Instance);
    SyncTotalToAttributeSet();
}

float FShieldPool::GetTotal() const
{
    // 阶段 2：总护盾量以属性集 `Shield` 为准（§4.3）。
    // 这里**不再**自己求和 —— 自己求和就等于留了第二份真相，
    // 而 `SyncTotalToAttributeSet` 已经把数组之和写进了属性集。
    // 属性集缺失时（只可能是代码被改坏）退回"数组求和"，保证不会报 0 反而更危险：
    // 报 0 会让调用方以为"没有护盾"，直接扣血。
    if (AttributeSet != nullptr)
    {
        const FGameplayAttributeData* const Data =
            UACBattleAttributeSet::GetShieldAttribute().GetGameplayAttributeData(AttributeSet);
        if (Data != nullptr)
        {
            return Data->GetCurrentValue();
        }
    }

    float Total = 0.f;
    for (const FShieldInstance& Instance : Instances)
    {
        Total += Instance.Amount;
    }
    return Total;
}

void FShieldPool::SyncTotalToAttributeSet()
{
    if (AttributeSet == nullptr)
    {
        return;
    }

    float Total = 0.f;
    for (const FShieldInstance& Instance : Instances)
    {
        Total += Instance.Amount;
    }

    // Base 与 Current 一起写：阶段 2 没有任何 GE 修饰器作用于 `Shield`，二者相等。
    // 走属性集的静态写入工具（直写 `FGameplayAttributeData`）而不是 `SetShield(...)`：
    // 后者内部是 `ASC->SetNumericAttributeBase`，而护盾池只是一个数据容器、拿不到 ASC，
    // 也不该为了写一个数去反查单位（那会把容器重新绑到 Actor 上）。
    // ⚠️ 代价是这次写入**不会**回调 `PreAttributeChange` 的 `Shield >= 0` 夹取，
    // 但下面的 `FMath::Max(0.f, ...)` 与那条 Clamp 是同一口径（实例的 Amount 只减不增、不会为负）。
    UACBattleAttributeSet::SetAttributeDataValue(AttributeSet, UACBattleAttributeSet::GetShieldAttribute(),
                                                FMath::Max(0.f, Total));
}

float FShieldPool::Absorb(float Damage, bool& bOutBroken)
{
    bOutBroken = false;
    if (Damage <= 0.f)
    {
        return 0.f;
    }

    float Remaining = Damage;
    // FIFO：按创建顺序吸收。
    for (int32 Index = 0; Index < Instances.Num() && Remaining > 0.f; ++Index)
    {
        FShieldInstance& Instance = Instances[Index];
        if (Instance.Amount <= 0.f)
        {
            continue;
        }
        const float Absorbed = FMath::Min(Instance.Amount, Remaining);
        Instance.Amount -= Absorbed;
        Remaining -= Absorbed;
    }

    const float TotalAbsorbed = Damage - Remaining;
    if (TotalAbsorbed > 0.f)
    {
        const bool bHadShield = Instances.Num() > 0;
        Instances.RemoveAll([](const FShieldInstance& Instance)
        {
            return Instance.Amount <= 0.f;
        });
        bOutBroken = bHadShield && Instances.Num() == 0;
    }

    // 阶段 2：同步**必须在 return 之前无条件执行**（哪怕本次一点都没吸收到）。
    // FIFO 是一次调用里多次扣减实现的，属性集的量必须跟着数组走 ——
    // 漏掉这一句就会让 `GetTotal()`（属性集）与数组之和静默分叉，
    // 而分叉的表现是"护盾看起来还在、伤害却已经扣到血上"，极难回溯。
    SyncTotalToAttributeSet();

    return TotalAbsorbed;
}

void FShieldPool::RemoveExpired(float NowSeconds)
{
    // 到期判定统一走 IsExpiredAt（负值 = 持续护盾，永不过期）。
    const int32 Removed = Instances.RemoveAll([NowSeconds](const FShieldInstance& Instance)
    {
        return FACBattleTime::IsExpiredAt(NowSeconds, Instance.ExpireTime);
    });
    if (Removed > 0)
    {
        SyncTotalToAttributeSet();
    }
}

void FShieldPool::RemoveBySourceEffectBlockId(FName SourceEffectBlockId)
{
    // 按**来源**移除单层护盾（§4.6 裁决②：每次施加护盾 = 一个独立的 GE 实例，各自持有时长）。
    //
    // 为什么不能像早期实现那样"宿主 GE 结束就把整池清空"：同一次战斗里完全可能叠两层不同时长的
    // 护盾（例如铁壁的守望 6 秒 + 重装的持续护盾），整池清空会把还没到期的那层一起清掉。
    //
    // 为什么按 `SourceEffectBlockId` 而不是 `InstanceId`：GE 的 Execution 拿不到"我上一次创建了
    // 哪个实例"（施加与到期是两个独立的 Execution 调用，中间隔着引擎的容器管理）。
    // 而 `SourceEffectBlockId` 从 `FACGameplayEffectContext` 一路传到 `FShieldRequest` 再落进实例，
    // 是同一个 GE 类在施加与到期时都能复现的稳定标识 —— 它天然就是"这一层护盾的来源"。
    // 因此同一个 GE 类施加的护盾被视为同一层（重复施加会先被移除再新增，语义与"刷新"一致）。
    if (SourceEffectBlockId.IsNone())
    {
        // 来源为空时**不敢按来源删**：会把所有来源为空的护盾一起删掉。交给调用方决定。
        return;
    }

    const int32 Removed = Instances.RemoveAll([SourceEffectBlockId](const FShieldInstance& Instance)
    {
        return Instance.SourceEffectBlockId == SourceEffectBlockId;
    });
    if (Removed > 0)
    {
        SyncTotalToAttributeSet();
    }
}

void FShieldPool::Clear()
{
    Instances.Reset();
    SyncTotalToAttributeSet();
}

// ---------------------------------------------------------------------------
// FCombatResolver
// ---------------------------------------------------------------------------

void FCombatResolver::Initialize(UBattleWorld* InWorld)
{
    World = InWorld;
    // 阶段 3.2a：`DamageModifiers.Reset()` 已删除 —— 那张表（`FACDamageModifier`）随
    // 旧 `FEffectSystem` 的 `AddDamageModifier` 动作一起消失（见头文件的结构体注释）。
}

FDamageResult FCombatResolver::ApplyDamage(const FDamageRequest& Request)
{
    FDamageResult Result;
    if (World == nullptr)
    {
        Result.bCancelled = true;
        return Result;
    }

    AACBattleUnitBase* Source = World->FindUnit(Request.Source);
    AACBattleUnitBase* Target = World->FindUnit(Request.Target);
    if (Target == nullptr || Target->IsDead())
    {
        Result.bCancelled = true;
        return Result;
    }

    Result.Requested = Request.RawAmount;
    Result.FinalTarget = Request.Target;

    // 0) 前置钩子：可取消、可改值。
    FBattleHookContext Context;
    Context.Source = Request.Source;
    Context.Target = Request.Target;
    Context.Time = FACBattleTime::ElapsedSeconds(*World);
    Context.FloatValue = Request.RawAmount;
    if (Request.DamageType == EACDamageType::Physical)
    {
        Context.Tags.AddTag(BattleTags::Damage_Type_Physical);
    }
    else if (Request.DamageType == EACDamageType::Technical)
    {
        Context.Tags.AddTag(BattleTags::Damage_Type_Technical);
    }
    else if (Request.DamageType == EACDamageType::TrueDamage)
    {
        Context.Tags.AddTag(BattleTags::Damage_Type_True);
    }
    else
    {
        Context.Tags.AddTag(BattleTags::Damage_Type_Mental);
    }
    Context.Tags.AppendTags(Request.Tags);

    if (World->Events().Dispatch(BattleTags::Hook_BeforeDealDamage, Context).bCancel || Context.bCancelled)
    {
        Result.bCancelled = true;
        return Result;
    }
    if (World->Events().Dispatch(BattleTags::Hook_BeforeTakeDamage, Context).bCancel || Context.bCancelled)
    {
        Result.bCancelled = true;
        return Result;
    }

    float Damage = FMath::Max(0.f, Context.FloatValue);

    // 1) 精神伤害：转交M10，不扣生命。
    if (Request.DamageType == EACDamageType::Mental)
    {
        World->Mental().ApplyMentalDamage(Request.Target, Damage, Request.Source);
        Result.AfterMitigation = Damage;
        Result.AfterIncreaseReduction = Damage;
        return Result;
    }

    // 2) 暴击。
    // 阶段 2：派生量改读属性集（公式一字未改，见 FBattleStatPipeline::GetCritChance 的注释）。
    // `Source->GetAttributeSet()` 可能为 nullptr（只可能是代码被改坏）——那时按"不暴击"处理，
    // 与旧代码在 `Source == nullptr` 时跳过暴击的行为一致。
    //
    // 阶段 4（D3 / §5.3）：判据从 `World->GetRng().RollChance(Source, EACRngChannel::Crit, CritChance)`
    // 改成 `FMath::FRand() < CritChance` —— C3「不追求确定性」：`FBattleRng` 与
    // `EACRngChannel` 已随 `Battle/ACBattleRng.h` 整文件删除，随机直接用引擎的 `FMath::FRand`。
    // 边界语义逐项等价（旧 `RollChance` 的两个短路分支照抄，只是不再需要"谁抽的、抽第几次"）：
    //   · `Chance <= 0` → 不暴击；`Chance >= 1` → 必暴击；两者都不消耗随机数；
    //   · 其余 → `[0,1)` 均匀分布与 `Chance` 比较。
    if (Request.bCanCrit && Source != nullptr && Source->GetAttributeSet() != nullptr)
    {
        const UACBattleAttributeSet& SourceAttributes = *Source->GetAttributeSet();
        const float CritChance = FBattleStatPipeline::GetCritChance(SourceAttributes);
        const bool bCrit = CritChance >= 1.f
            || (CritChance > 0.f && FMath::FRand() < CritChance);
        if (bCrit)
        {
            Result.bWasCrit = true;
            Result.CritMultiplier = FBattleStatPipeline::GetCritMultiplier(SourceAttributes);
            Damage *= Result.CritMultiplier;
        }
    }

    // 3) 类型减免。
    Damage = ComputeMitigation(Request, *Target, Damage);
    Result.AfterMitigation = Damage;

    // 4) 增伤 / 减伤。
    Damage = ApplyIncreaseReduction(Request, Damage);
    Result.AfterIncreaseReduction = FMath::Max(0.f, Damage);

    // 5) 护盾吸收（冰冻满层+20% 等特殊由 M09 查询，框架先保留入口）。
    if (!Request.bIgnoreShield)
    {
        bool bBroken = false;
        Result.ShieldAbsorbed = Target->GetShields().Absorb(Result.AfterIncreaseReduction, bBroken);
        if (Result.ShieldAbsorbed > 0.f)
        {
            FBattleHookContext ShieldContext;
            ShieldContext.Source = Request.Source;
            ShieldContext.Target = Request.Target;
            ShieldContext.Time = FACBattleTime::ElapsedSeconds(*World);
            ShieldContext.FloatValue = Result.ShieldAbsorbed;
            World->Events().Dispatch(BattleTags::Hook_ShieldGain, ShieldContext);
        }
        if (bBroken)
        {
            FBattleHookContext BrokenContext;
            BrokenContext.Source = Request.Source;
            BrokenContext.Target = Request.Target;
            BrokenContext.Time = FACBattleTime::ElapsedSeconds(*World);
            World->Events().Dispatch(BattleTags::Hook_ShieldBroken, BrokenContext);
        }
    }

    // 6) 扣血。
    // 阶段 2（§4.6 新契约）：**只有本类能改 `Health`**，且唯一写入口是
    // `AACBattleUnitBase::SetHealthFromResolver`（名字里带 FromResolver 就是为了让"受控入口"一眼可见）。
    // 它内部写属性集 `Health` 的 Base/Current 并按 [0, MaxHealth] 夹取。
    //
    // ⚠️ 关键事实（决定了任务 6 那个守卫怎么写）：这条路是**直写 `FGameplayAttributeData`**，
    // 不是通过 GE Modifier，因此**不会**触发 `PostGameplayEffectExecute`，也不会触发
    // `PreAttributeChange` —— 夹取由 `SetHealthFromResolver` 自己做（口径与那边逐字一致）。
    Result.AppliedHpLoss = FMath::Max(0.f, Result.AfterIncreaseReduction - Result.ShieldAbsorbed);
    const float HpBefore = Target->GetCurrentHP();
    Target->SetHealth(HpBefore - Result.AppliedHpLoss);
    Result.Overkill = FMath::Max(0.f, Result.AppliedHpLoss - HpBefore);

    if (Target->GetCurrentHP() <= 0.f)
    {
        // 兜底归零：`SetHealthFromResolver` 已经夹到 [0, MaxHealth]，
        // 因此这里只在"MaxHealth 本身为 0 且血量算出为负"的退化场景才真正改写值。
        // 保留这一行是为了让"血量到 0 就再显式写一次 0"这个不变量留在管线上，不要删。
        Target->SetHealth(0.f);
        Target->MarkPendingDeath(Request.Source);
    }

    // 7) 后置：吸血（默认按实际扣血）。
    if (Source != nullptr && Source != Target)
    {
        ApplyLifesteal(Request.Source, Result, Request.DeclaredJudgment);
    }

    // 8) 事件与统计。
    FBattleHookContext DamageContext;
    DamageContext.Source = Request.Source;
    DamageContext.Target = Request.Target;
    DamageContext.Time = FACBattleTime::ElapsedSeconds(*World);
    DamageContext.FloatValue = Result.AppliedHpLoss;
    DamageContext.IntValue = Result.bWasCrit ? 1 : 0;
    DamageContext.bBoolValue = Result.bWasCrit;
    DamageContext.Tags = Context.Tags;
    World->Events().Dispatch(BattleTags::Hook_TakeDamage, DamageContext);
    World->Events().Dispatch(BattleTags::Hook_DamageDealt, DamageContext);

    RecordDamageStats(Request.Source, Request.Target, Result.AppliedHpLoss, Result.AppliedHpLoss);

    // 埋点：ValueA = 实际扣血（阶.2 "单层增益场景的伤害数据比对口径），
    //       ValueB = 被护盾吸收的部分（伤害数值一致但护盾量不同，是另一类回归）。
    //       IntValue = 伤害类型（EACDamageType）。
    // 精神伤害走上面的早退分支（不扣血、由 M10 结算），按此口径它的 AppliedHpLoss / ShieldAbsorbed 都是 0
    // ——这是刻意的：日志记 0 点生命伤害正是"这次攻击走的是精神通道"的可比对信号。
    LogCombatEvent(*World, LogEvent_Damage, Request.Source, Request.Target,
                   Result.AppliedHpLoss, Result.ShieldAbsorbed, static_cast<int32>(Request.DamageType));
    return Result;
}

FHealResult FCombatResolver::ApplyHeal(const FHealRequest& Request)
{
    FHealResult Result;
    if (World == nullptr)
    {
        return Result;
    }

    AACBattleUnitBase* Target = World->FindUnit(Request.Target);
    if (Target == nullptr || Target->IsDead())
    {
        return Result;
    }

    const float Missing = FMath::Max(0.f, Target->GetMaxHP() - Target->GetCurrentHP());
    Result.Applied = FMath::Min(Request.RawAmount, Missing);
    Result.Overheal = FMath::Max(0.f, Request.RawAmount - Result.Applied);
    Target->AddCurrentHP(Result.Applied);

    FBattleHookContext Context;
    Context.Source = Request.Source;
    Context.Target = Request.Target;
    Context.Time = FACBattleTime::ElapsedSeconds(*World);
    Context.FloatValue = Result.Applied;
    World->Events().Dispatch(BattleTags::Hook_Heal, Context);

    if (Result.Overheal > 0.f && Request.bAllowOverhealConversion)
    {
        FBattleHookContext OverhealContext;
        OverhealContext.Source = Request.Source;
        OverhealContext.Target = Request.Target;
        OverhealContext.Time = FACBattleTime::ElapsedSeconds(*World);
        OverhealContext.FloatValue = Result.Overheal;
        World->Events().Dispatch(BattleTags::Hook_Overheal, OverhealContext);   // 溢出转化由效果块订阅
    }

    World->Stats().RecordHealing(Request.Source, Result.Applied);

    // 埋点：ValueA = 实际治疗量（过量部分不计），ValueB = 溢出量（Overheal 转化类效果块的判据）。
    LogCombatEvent(*World, LogEvent_Heal, Request.Source, Request.Target, Result.Applied, Result.Overheal);
    return Result;
}

void FCombatResolver::ApplyShield(const FShieldRequest& Request)
{
    if (World == nullptr)
    {
        return;
    }

    AACBattleUnitBase* Target = World->FindUnit(Request.Target);
    if (Target == nullptr || Target->IsDead())
    {
        return;
    }

    FShieldInstance Instance;
    Instance.Amount = Request.Amount;
    Instance.Source = Request.Source;
    Instance.SourceEffectBlockId = Request.SourceEffectBlockId;
    // 阶段 0.5：护盾的时间口径改秒。DurationSeconds < 0 = 持续护盾（ExpireTime 写 -1，永不过期）。
    Instance.CreatedTime = FACBattleTime::Now(*World);
    Instance.ExpireTime = Request.DurationSeconds < 0.f ? -1.f : Instance.CreatedTime + Request.DurationSeconds;
    Target->GetShields().Add(Instance);

    FBattleHookContext Context;
    Context.Source = Request.Source;
    Context.Target = Request.Target;
    Context.Time = FACBattleTime::ElapsedSeconds(*World);
    Context.FloatValue = Request.Amount;
    World->Events().Dispatch(BattleTags::Hook_ShieldGain, Context);

    // 埋点：ValueA = 本次获得的护盾量（按申请量记，未截断 ——护盾池没有上限概念）。
    // 记在末尾：上面的 Hook.ShieldGain 订阅方可能立刻消耗护盾，因此顺序应获得"先于"被吸收。
    // 日志里的 Shield 事件永远排在同一步内后续的Damage 事件之前，时序可读。
    LogCombatEvent(*World, LogEvent_Shield, Request.Source, Request.Target, Request.Amount);
}

void FCombatResolver::ApplyLifesteal(FUnitId SourceId, FDamageResult& Result, EACJudgmentPoint JudgmentPoint)
{
    if (World == nullptr)
    {
        return;
    }

    AACBattleUnitBase* Source = World->FindUnit(SourceId);
    if (Source == nullptr || Source->IsDead())
    {
        return;
    }

    const float Lifesteal = Source->GetStat(EACStat::Lifesteal);
    if (Lifesteal <= 0.f)
    {
        return;
    }

    float BaseDamage = Result.AppliedHpLoss;
    switch (JudgmentPoint)
    {
    case EACJudgmentPoint::Requested:       BaseDamage = Result.Requested; break;
    case EACJudgmentPoint::AfterMitigation: BaseDamage = Result.AfterMitigation; break;
    // "护盾后、扣血前 = 减免增减伤后再扣掉护盾吸收量的部分。
    case EACJudgmentPoint::AfterShield:     BaseDamage = FMath::Max(0.f, Result.AfterIncreaseReduction - Result.ShieldAbsorbed); break;
    case EACJudgmentPoint::Overkill:        BaseDamage = Result.Overkill; break;
    case EACJudgmentPoint::AppliedHpLoss:
    default:                                BaseDamage = Result.AppliedHpLoss; break;
    }

    FHealRequest HealRequest;
    HealRequest.Source = SourceId;
    HealRequest.Target = SourceId;
    HealRequest.RawAmount = BaseDamage * Lifesteal / 100.f;
    HealRequest.bAllowOverhealConversion = false;
    HealRequest.SourceEffectBlockId = FName(TEXT("Lifesteal"));

    // ApplyHeal 内部已写入治疗统计，这里只把结果回填到判定点记录。
    const FHealResult HealResult = ApplyHeal(HealRequest);
    Result.LifestealHealed = HealResult.Applied;
}

void FCombatResolver::RemoveExpiredShields()
{
    if (World == nullptr)
    {
        return;
    }

    // 护盾与修饰器的过期要覆盖所有仍在注册表中的单位：死亡（可能被复活）的单位也保留着状态。
    // 时间只取一次：同一帧内所有单位的护盾按同一时刻判定。
    const float NowSeconds = FACBattleTime::Now(*World);
    World->ForEachUnit([NowSeconds](AACBattleUnitBase& Unit)
    {
        Unit.GetShields().RemoveExpired(NowSeconds);
    });
}

float FCombatResolver::ComputeMitigation(const FDamageRequest& Request, const AACBattleUnitBase& Target, float Damage) const
{
    const AACBattleUnitBase* Source = World != nullptr ? World->FindUnit(Request.Source) : nullptr;

    switch (Request.DamageType)
    {
    case EACDamageType::Physical:
    {
        const float Defense = Target.GetStat(EACStat::DEF);
        const float DefPen = Source != nullptr ? Source->GetStat(EACStat::DEFPen) : 0.f;
        const float EffectiveDefense = FMath::Max(0.f, Defense - DefPen);
        const float MinDamage = (World != nullptr && World->GetRuleConfig() != nullptr)
            ? World->GetRuleConfig()->MinPhysicalDamage
            : 1.f;
        return FMath::Max(MinDamage, Damage - EffectiveDefense);
    }

    case EACDamageType::Technical:
    {
        const float Resist = Target.GetStat(EACStat::RES);
        const float ResistPen = Source != nullptr ? Source->GetStat(EACStat::RESPen) : 0.f;
        const float ResistPct = FMath::Clamp((Resist - ResistPen) / 100.f, 0.f,
            (World != nullptr && World->GetRuleConfig() != nullptr) ? World->GetRuleConfig()->TechResistCap : 0.99f);
        return Damage * (1.f - ResistPct);
    }

    case EACDamageType::TrueDamage:
    case EACDamageType::Mental:
    default:
        return Damage;
    }
}

float FCombatResolver::ApplyIncreaseReduction(const FDamageRequest& Request, float Damage) const
{
    // -----------------------------------------------------------------------
    // 阶段 3.2a：本函数从"遍历增伤/减伤表"退化为**恒等变换**，但**保留在管线里**。
    // -----------------------------------------------------------------------
    // 为什么保留：§4.6 明文要求**保留 7 个检查点**
    //（`Requested → AfterMitigation → AfterIncreaseReduction → RedirectedOut →
    //  ShieldAbsorbed → AppliedHpLoss → Overkill`）。检查点 ③ 正是由本函数的返回值写入
    //（调用点 `Result.AfterIncreaseReduction = FMath::Max(0.f, Damage)`），
    // 把函数与调用一起删掉会让日志与 `_damage.jsonl` 少一列、破坏与基线的列对齐。
    //
    // 为什么表不存在了：它唯一的注册者是旧 `FEffectSystem` 的 `AddDamageModifier` 动作
    //（§4.2 映射：`AddDamageModifier` → "GE Modifier + SetByCaller 幅度"），
    // 而 `FEffectSystem` + `FACEffectBlock` 本阶段已整条删除。
    //
    // 数值等价性：改造前全仓没有任何内容注册过增伤 / 减伤修饰器
    //（唯一一处相关内容是 `Trait_DeadlyStrike` 的 `AddDamageModifier`，
    //  而 `BuildOperators` 的 `UpgradeChoiceIds` 里**从未**引用它，因此它从来没被注册过），
    // 表恒为空 ⇒ 本函数恒等返回，与旧实现的 `(Damage + 0) * (1 + 0)` 逐字等价。
    // 上一条注释提到的 `Trait_DeadlyStrike` 现在由 3.1a 的 `UACGE_Trait_DeadlyStrike`
    // 以"攻击力 × 1.2"表达（已知语义差异，写在 `ACGE_ContentEffects.h` 的类注释里）。
    //
    // 设计决定（不是待办）：若将来需要"伤害侧增伤/减伤"，按 §4.2 走 GE Modifier 或专用 Execution，
    // **不要**把增伤/减伤表在这条管线里重新造回来 —— 那会让 `Health` 出现第二条改动路径，
    // 违反 §4.6 的新契约（只有 `UACDamageExecution` 能调用本类；只有本类能改 `Health`）。
    (void)Request;
    return Damage;
}

void FCombatResolver::RecordDamageStats(FUnitId Source, FUnitId Target, float Dealt, float Taken)
{
    if (World == nullptr)
    {
        return;
    }
    World->Stats().RecordDamageDealt(Source, Dealt);
    World->Stats().RecordDamageTaken(Target, Taken);
}
