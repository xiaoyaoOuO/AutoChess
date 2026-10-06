#include "Battle/ACBattleUnitBase.h"
#include "Core/ACBattleTags.h"
#include "Stats/ACBattleStats.h"
#include "GAS/ACBattleAttributeSet.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"

AACBattleUnitBase::AACBattleUnitBase()
{
    // 单位 Actor 关闭 PrimaryActorTick —— 所有逻辑仍由 UBattleWorld::Step 统一驱动。
    // 由 `UBattleWorld::Step` 手动调 `ASC->TickComponent(DeltaTime)`，
    // 这样"GE 的时长推进"与"UBattleWorld 的固定顺序"是同一件事，
    PrimaryActorTick.bCanEverTick = false;

    // NoCollision；位置由 UACUnitGridComponent 控制，不使用导航网格。
    SetActorEnableCollision(false);
    
    USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(SceneRoot);

    // 单位基类持有网格与表现两个组件
    GridComponent = CreateDefaultSubobject<UACUnitGridComponent>(TEXT("GridComponent"));
    SkeletalMeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("SkeletalMeshComponent"));
    AnimInstance = CreateDefaultSubobject<UAnimInstance>(TEXT("AnimInstance"));
    NavMovementComponent = CreateDefaultSubobject<UNavMovementComponent>(TEXT("NavMovementComponent"));
    
    // 由 ASC 在 `InitAbilityActorInfo` 时自动把属性集注册进来（`GetSet<UACBattleAttributeSet>()` 可查到）。
    // 注意创建顺序：属性集必须先于/同时于 ASC 存在，但 GAS 并不要求两者有构造顺序 
    // 注册发生在 BeginPlay 的 `InitAbilityActorInfo`，那时两者都已创建完毕。
    AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
    AttributeSet = CreateDefaultSubobject<UACBattleAttributeSet>(TEXT("AttributeSet"));

    //：能力 / GE 授予清单。
    AbilitySetComponent = CreateDefaultSubobject<UACAbilitySetComponent>(TEXT("AbilitySetComponent"));

    // 护盾池要把"总护盾量"同步进属性集 Shield。
    // `FShieldPool` 是纯数据容器（不持有 World、不做取时），
    // 它只需要一个"把总量写哪儿"的落点，而属性集是构造函数里就存在的默认子对象。
    // 时序上只要不早于 `CreateDefaultSubobject<UACBattleAttributeSet>`（上一行）即可。
    Shields.Initialize(AttributeSet);
}

void AACBattleUnitBase::BeginPlay()
{
    Super::BeginPlay();
    
    // 时序依赖（这是正确性的一部分，不是巧合）：
    //   单位由 `UBattleWorld::SpawnUnitActor` 用 `SpawnActor(..., bDeferConstruction = true)` 生成，
    //   出生后**不跑 BeginPlay**；`UBattleWorld` 随后依次调用
    //   `SetUnitId` → `InitializeFromXxx`（内部走 `InitCommon`，把属性写进属性集）
    //   → `SetAbilitySet`→ `FinishSpawning(FTransform::Identity)`。
    if (AbilitySystemComponent != nullptr)
    {
        AbilitySystemComponent->InitAbilityActorInfo(this, this);
        
        // **必须在 `InitAbilityActorInfo` 之后**调 `GrantToOwner`：
        //   - `GiveAbility` 走 `OnGiveAbility` 时会把 `GameplayEventTriggeredAbilities` 填好
        //     （AbilitySystemComponent_Abilities.cpp:557-572），被动能力的 `AbilityTriggers`
        //     靠它生效；而 `CanActivateAbility` 还要用 ActorInfo 里的 Avatar 去找世界与目标；
        //   - 常驻 GE 的时长 / 周期定时器要用 `Owner->GetWorld()->GetTimerManager()`
        //     （GameplayEffect.cpp:4228-4252）。`BeginPlay` 时单位已经在世界里，
        //     而延迟构造期（`InitializeFromXxx` 那一刻）**还没有** —— 这就是清单不能更早授予的原因。
        if (AbilitySetComponent != nullptr)
        {
            AbilitySetComponent->GrantToOwner();
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] AACBattleUnitBase::BeginPlay: AbilitySystemComponent 缺失，跳过 InitAbilityActorInfo。"));
    }
}

// ---------------------------------------------------------------------------
// 阶段 3.2a：异常状态查询接口（旧 `FAbnormalStateContainer` 的只读替代）
// ---------------------------------------------------------------------------

bool AACBattleUnitBase::HasStateTag(FGameplayTag StateTag) const
{
    return AbilitySystemComponent && AbilitySystemComponent->HasMatchingGameplayTag(StateTag);
}

int32 AACBattleUnitBase::GetStateStacks(FGameplayTag StateTag) const
{
    const UAbilitySystemComponent* const ASC = AbilitySystemComponent;
    return ASC != nullptr ? ASC->GetTagCount(StateTag) : 0;
}

UAbilitySystemComponent* AACBattleUnitBase::GetAbilitySystemComponent() const
{
    return AbilitySystemComponent;
}

void AACBattleUnitBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);

    /** 阶段 0b 会在这里接入 UBattleWorld 的反注册。 */
    // 本阶段刻意不反注册：World 侧注册表此时仍以 FUnitId 为键（D8 在阶段 0b 才做），
    // 反注册逻辑要与 UnitLookup 的指针改造一起写才不会做两遍；
    // Shutdown() 已经统一 Destroy 所有单位并清空注册表，因此阶段 0a 不存在泄漏。
}

void AACBattleUnitBase::SetCell(FACHexCoord InCell)
{
    Cell = InCell;

    // 网格组件是"格坐标 ↔ 世界坐标"的唯一出口，单位自身改格时同步镜像，
    // 这样注入了棋盘 Actor 的单位位置会跟着动（没注入棋盘时组件只记录格子）。
    // 阶段 0b 会反过来：格子只存在组件里、单位不再自持 Cell。
    if (GridComponent != nullptr)
    {
        GridComponent->SetCell(InCell);
    }
}

void AACBattleUnitBase::SetFacing(EACFacing InFacing)
{
    Facing = InFacing;
}

void AACBattleUnitBase::SyncGridComponent()
{
    // bOnBoard 目前仍是单位的公开字段（多处直接写字段），只能由调用点在改完后主动调本函数镜像。
    if (GridComponent != nullptr)
    {
        GridComponent->SetCell(Cell);
        GridComponent->SetOnBoard(bOnBoard);
    }
}

void AACBattleUnitBase::InitCommon(const FACStatBlock& BaseStats, int32 InTierIndex, EACUnitKind InKind, EACTeam InTeam, FName InDefinitionId)
{
    TierIndex = InTierIndex;
    Kind = InKind;
    Team = InTeam;
    DefinitionId = InDefinitionId;

    Stats.InitDefaults();


    const FACStatBlock ConvertedStats = ApplyStatConversionRules(BaseStats);

    // 把 `FACStatBlock` 写进属性集 + 留一份 `Stats.Base` 快照，
    // **属性集从此刻起是属性的读权威**（`GetStat` 就是它的 CurrentValue）。
    //
    // 为什么放在这里而不是 `BeginPlay`：属性集 Base 值的输入就是上面那一行准备好的块；
    // 而单位是延迟构造的（`SpawnActor(bDeferConstruction)` → 填数据 → `FinishSpawning`），
    // 本函数执行时 Actor 还没有跑过 BeginPlay，所以这里写属性集**不经过 ASC**
    //（`InitializeFromStatBlock` / `SetResourceAttribute` 直接写 `FGameplayAttributeData`）——
    // 这正是阶段 1 已确认的约束：延迟构造期走 `SetXxx`（内部 `SetNumericAttributeBase`）会 ensure 失败。
    SyncAttributeSetFromStatBlock(ConvertedStats);
    
    // 属性集是权威，先落它再由它推导 Focus / Mental 的派生字段，保证两边来自同一次读取、不会分叉。
    SetHealth(GetMaxHP());
    Mental.Current = 100.f;
    Mental.MaxBase = GetStat(EACStat::MentalMax);
    if (Mental.MaxBase <= 0.f)
    {
        Mental.MaxBase = 100.f;
    }
    Mental.MaxCurrent = Mental.MaxBase;
    Mental.Current = Mental.MaxCurrent;

    // 阶段 3.2b：这里原来的四行 `Focus.Max / Current / RegenPerSecond / PerAttackGain = GetStat(...)`
    // **已删除** —— `FFocusState` 不存在了。四项的权威就是属性集自己的四个属性
    //（`FocusMax` / `FocusInit` / `FocusRegen` / `FocusPerAttack`），读法见 `GetFocusMax()` 等访问器，
    // 因此不需要"启动时抄一份到成员变量"这种会与 GE 修饰脱节的镜像。

    ActionState = EACUnitActionState::Idle;
    bIsDead = false;
    bPendingDeath = false;
    bOnBoard = true;

    // 资源属性在 `Mental` 就绪之后再写一次（`Health` 已在上面写过）。
    SyncResourceAttributes();

    SyncGridComponent();
}

FACStatBlock AACBattleUnitBase::ApplyStatConversionRules(const FACStatBlock& Block) const
{
    // 现阶段 `Conversions` 是空的（全仓没有任何写入点），本函数等价于"原样返回"。
    FACStatBlock Result = Block;
    for (const FACStatConversionRule& Rule : Conversions)
    {
        // 与阶段 1 的 `ApplyConversionRules(Sheet, -1.f)`（Recompute 里那条内置调用）行为一致。
        Rule.ApplyToBlock(Result, -1.f);
    }
    return Result;
}

void AACBattleUnitBase::SyncAttributeSetFromStatBlock(const FACStatBlock& Block)
{
    if (AttributeSet == nullptr)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("[Battle] AACBattleUnitBase: 属性集缺失，单位属性未初始化（GetStat 将全为 0）。"));
        return;
    }

    // 属性集是读权威，`Stats.Base` 是同一次输入留下的快照（召唤继承 / 结果持久化口径 / 自查用）。
    FBattleStatPipeline::SetBaseFromBlock(Stats, Block);
    AttributeSet->InitializeFromStatBlock(Block);
}

void AACBattleUnitBase::SetResourceAttribute(const FGameplayAttribute& Attribute, float Value) const
{
    if (AttributeSet == nullptr)
    {
        return;
    }

    // 资源属性不在 `EACStat` / `FACStatBlock` 里，只能按属性句柄单独写。
    // 走属性集的静态写入工具（与 `InitializeFromStatBlock` 同一姿势）：直写 `FGameplayAttributeData`，
    // **不经过 ASC** —— 初始化发生在 `InitAbilityActorInfo` 之前，走 ASC 会 ensure 失败。
    UACBattleAttributeSet::SetAttributeDataValue(AttributeSet, Attribute, Value);
}

void AACBattleUnitBase::SyncResourceAttributes()
{
    if (AttributeSet == nullptr)
    {
        return;
    }

    // 资源属性不在 `EACStat` / `FACStatBlock` 里，逐个显式写。
    // 取值口径全部跟随 `InitCommon` 里已经算好的字段，不重新推导一遍（避免两套算法分叉）：
    //   Health = 属性集 `Health` 的当前值（`InitCommon` 里 = GetMaxHP()；
    //            随后 `InitializeFromPlayerSpec` 可能按 `Spec.CurrentBaseHP` 覆盖，
    //            那一处会再写一次）
    //   Shield = 0       （护盾是 `FShieldPool` 持有的多层实例，初始化时为空池；
    //                     阶段 2 起"总护盾量"由 `FShieldPool::SyncTotalToAttributeSet` 同步）
    //   Focus  = `FocusInit` 属性值（阶段 3.2b：原来是读已删除的 `Focus.Current` 成员，
    //            现在直接读属性 —— 两者在 `InitCommon` 里本来就取自同一处，口径不变）
    //   Mental = Mental.Current（= Mental.MaxCurrent = MaxBase = MentalMax 属性值）；
    //            ⚠️ 它只是 `FMentalSystem` 的镜像，权威值仍在 `FMentalSystem`。
    //
    // ⚠️ `Health` 的写法阶段 2 有变化：从"读成员 `CurrentHP`"改成"读属性集自己" ——
    // 属性集已经是当前生命的唯一真相，再从它自己读一次并写回是无操作，
    // 但保留了"调用 `SyncResourceAttributes` 就会把四个资源属性都对齐"的语义
    //（否则函数名会名不副实，且将来漏写某一项时不容易发现）。
    SetResourceAttribute(UACBattleAttributeSet::GetHealthAttribute(), GetCurrentHP());
    SetResourceAttribute(UACBattleAttributeSet::GetShieldAttribute(), 0.f);
    SetResourceAttribute(UACBattleAttributeSet::GetFocusAttribute(), GetStat(EACStat::FocusInit));
    SetResourceAttribute(UACBattleAttributeSet::GetMentalAttribute(), Mental.Current);
}

// ---------------------------------------------------------------------------
// 资源：专注（阶段 3.2b：`FFocusState` 的只读替代）
// ---------------------------------------------------------------------------

float AACBattleUnitBase::GetFocusCurrent() const
{
    return ReadAttributeCurrentValue(UACBattleAttributeSet::GetFocusAttribute());
}

float AACBattleUnitBase::GetFocusMax() const
{
    // `FocusMax` 是 `EACStat` 里的基础属性（第 12 项），因此走 `GetStat` 即可。
    return GetStat(EACStat::FocusMax);
}

bool AACBattleUnitBase::IsFocusFull() const
{
    // 施放前提：`FocusMax > 0`（有专注条）且 `Focus >= FocusMax`（满则释放）。
    // 唯一的变化是读数来源从 `FFocusState` 换成了属性集。
    const float Max = GetFocusMax();
    return Max > 0.f && GetFocusCurrent() >= Max;
}

void AACBattleUnitBase::InitializeFromPlayerSpec(const FACPlayerUnitSpec& Spec, const UOperatorDefinition* InDefinition)
{
    const FACStatBlock* StatsBlock = &Spec.BaseStats;
    if (InDefinition != nullptr && InDefinition->StatsByLevel.IsValidIndex(Spec.Level))
    {
        StatsBlock = &InDefinition->StatsByLevel[Spec.Level];
    }

    InitCommon(*StatsBlock, Spec.TierIndex, EACUnitKind::Operator, EACTeam::Player, Spec.DefinitionId);
    Cell = Spec.SpawnCell;
    Facing = Spec.Facing;
    Tags = Spec.ExtraTags;

    if (InDefinition != nullptr)
    {
        Tags.AppendTags(InDefinition->IdentityTags);
        // 阶段 3.2b：`SkillDef = InDefinition->Skill;` **已删除**（技能内容不再由定义资产承载，
        // 而是由 `UACAbilitySet::GrantedAbilities` 授予的能力类承载）。
        AttackPattern = InDefinition->AttackPattern;
        this->Definition = InDefinition;
    }

    CacheCombatIdentity();

    if (Spec.CurrentBaseHP >= 0.f)
    {
        // 阶段 2：当前生命只在属性集里，"带血量入场"就是往属性集写一次受控值。
        // 夹取口径与改造前逐字一致（旧代码：`Clamp(Spec.CurrentBaseHP, 1.f, GetMaxHP())`）。
        SetHealth(FMath::Clamp(Spec.CurrentBaseHP, 1.f, GetMaxHP()));
    }
}

void AACBattleUnitBase::InitializeFromEnemySpec(const FACEnemyUnitSpec& Spec, const UEnemyDefinition* InDefinition)
{
    const FACStatBlock* StatsBlock = &Spec.BaseStats;
    if (InDefinition != nullptr && InDefinition->StatsByLevel.IsValidIndex(Spec.TierIndex))
    {
        StatsBlock = &InDefinition->StatsByLevel[Spec.TierIndex];
    }

    InitCommon(*StatsBlock, Spec.TierIndex, EACUnitKind::Enemy, EACTeam::Enemy, Spec.DefinitionId);
    Cell = Spec.SpawnCell;
    Facing = EACFacing::Down;
    Tags = Spec.ExtraTags;

    if (InDefinition != nullptr)
    {
        Tags.AppendTags(InDefinition->IdentityTags);
        if (InDefinition->bIsElite)
        {
            Tags.AddTag(BattleTags::Unit_Tag_Elite);
        }
        if (InDefinition->bIsBoss)
        {
            Tags.AddTag(BattleTags::Unit_Tag_Boss);
        }
        // 阶段 3.2b：`SkillDef = InDefinition->Skill;` **已删除**（技能内容不再由定义资产承载，
        // 而是由 `UACAbilitySet::GrantedAbilities` 授予的能力类承载）。
        AttackPattern = InDefinition->AttackPattern;
        this->Definition = InDefinition;
    }

    CacheCombatIdentity();
}

void AACBattleUnitBase::InitializeFromSpawnRequest(const FACUnitSpawnRequest& Request, const UUnitDefinitionBase* InDefinition,
                                                   const TArray<float>* InheritedStats)
{
    FACStatBlock StatsBlock = Request.BaseStats;
    if (InheritedStats != nullptr)
    {
        StatsBlock.InitDefaults();
        for (int32 Index = 0; Index < ACStatCount && InheritedStats->IsValidIndex(Index); ++Index)
        {
            StatsBlock.Values[Index] = (*InheritedStats)[Index] * Request.InheritRatio;
        }
    }
    else if (InDefinition != nullptr && InDefinition->StatsByLevel.IsValidIndex(Request.TierIndex))
    {
        StatsBlock = InDefinition->StatsByLevel[Request.TierIndex];
    }

    InitCommon(StatsBlock, Request.TierIndex, Request.Kind, Request.Team, Request.DefinitionId);
    Cell = Request.PreferredCell;
    Facing = Request.Facing;
    Tags = Request.ExtraTags;
    OwnerUnitId = Request.OwnerUnitId;
    bOccupyCell = Request.bOccupyCell;
    bSelectable = Request.bSelectable;
    bCountsAsKill = Request.bCountsAsKill;

    if (InDefinition != nullptr)
    {
        Tags.AppendTags(InDefinition->IdentityTags);
        // 阶段 3.2b：`SkillDef = InDefinition->Skill;` **已删除**（技能内容不再由定义资产承载，
        // 而是由 `UACAbilitySet::GrantedAbilities` 授予的能力类承载）。
        AttackPattern = InDefinition->AttackPattern;
        this->Definition = InDefinition;
    }

    CacheCombatIdentity();
}

void AACBattleUnitBase::MarkPendingDeath(FUnitId KillerId)
{
    if (!bPendingDeath && !bIsDead)
    {
        bPendingDeath = true;
        // 只有有效来源才覆盖归属，避免 KillUnit/到期路径清掉真实击杀者。
        if (KillerId != InvalidUnitId)
        {
            LastDamageSourceId = KillerId;
        }
        ActionState = EACUnitActionState::Dead;
    }
}

void AACBattleUnitBase::ClearPendingDeath()
{
    bPendingDeath = false;
}

void AACBattleUnitBase::MarkDead()
{
    bPendingDeath = false;
    bIsDead = true;
    bOnBoard = false;
    ActionState = EACUnitActionState::Dead;

    SyncGridComponent();
}

void AACBattleUnitBase::Revive()
{
    bPendingDeath = false;
    bIsDead = false;
    bOnBoard = true;
    ActionState = EACUnitActionState::Idle;
    // M02 §5.3：复活恢复 Max(1, 当前血量)。
    // 阶段 2：当前生命在属性集里，因此读它、且**必须**再写回去 —— 这正是任务 5 那个陷阱：
    // 若照搬旧代码"读进来算完就丢"，单位会带着 0 血复活（旧代码是成员变量赋值，天然落库）。
    SetHealth(FMath::Max(1.f, GetCurrentHP()));

    SyncGridComponent();
}

bool AACBattleUnitBase::CanBeTargeted() const
{
    return IsAlive() && bOnBoard && bSelectable && !bStealthed && !bUntargetable;
}

// ---------------------------------------------------------------------------
// 属性读路径（阶段 2：唯一权威是 UACBattleAttributeSet）
// ---------------------------------------------------------------------------

float AACBattleUnitBase::GetStat(EACStat Stat) const
{
    if (Stat == EACStat::Count)
    {
        // 哨兵：`ACStatCount = 19` 是**数组长度**，不是第 19 个属性
        return 0.f;
    }
    return ReadAttributeCurrentValue(UACBattleAttributeSet::GetAttributeForStat(Stat));
}

float AACBattleUnitBase::GetMaxHP() const
{
    return GetStat(EACStat::MaxHP);
}

float AACBattleUnitBase::GetCurrentHP() const
{
    return ReadAttributeCurrentValue(UACBattleAttributeSet::GetHealthAttribute());
}

float AACBattleUnitBase::ReadAttributeCurrentValue(const FGameplayAttribute& Attribute) const
{
    if (!Attribute.IsValid())
    {
        return 0.f;
    }
    
    const FGameplayAttributeData* const Data = Attribute.GetGameplayAttributeData(AttributeSet);
    return (Data != nullptr) ? Data->GetCurrentValue() : 0.f;
}

void AACBattleUnitBase::SetHealth(float NewHealth)
{
    if (AttributeSet == nullptr)
    {
        return;
    }

    // NaN 先挡掉：NaN 与任何数比较都是 false，会让下面的 Clamp 全部失效，
    // 之后 Health = NaN 会污染整场战斗且无法从日志回溯（日志里它是 "nan"）。
    // 这与 `UACBattleAttributeSet::PreAttributeChange` 的第一道防线同口径 ——
    // 那条路走不到（见下），所以在这里补一份。
    if (FMath::IsNaN(NewHealth))
    {
        NewHealth = 0.f;
    }

    // 夹取口径与属性集 `PreAttributeChange` 的 `Health` 分支逐字一致：0 ≤ Health ≤ MaxHealth。
    //
    // ⚠️ 这里是**替换** `PreAttributeChange` 的 Clamp，不是"多此一举"：
    // `PreAttributeChange` 只在**通过 ASC 改属性**时被引擎回调（`SetNumericAttributeBase` /
    // GE Modifier 求值路径），而本函数（以及 `FCombatResolver` 经它走的路径）是
    // **直写 `FGameplayAttributeData`** 的 —— 引擎完全不知道这次写入，不会回调任何钩子。
    // 换句话说：这张保护网必须由写入方自己拉起来。
    const float MaxHealthValue = GetMaxHP();
    const float Clamped = (MaxHealthValue > 0.f) ? FMath::Clamp(NewHealth, 0.f, MaxHealthValue) : 0.f;

    // Base 与 Current 一起写：本阶段没有任何 GE 修饰器，二者必然相等；
    // 只写 Base 会让"写完后立刻读 Current"（`GetCurrentHP`）读到旧值 —— 那是本阶段最容易踩的坑。
    // 等阶段 3/4 的 GE 开始给 `MaxHealth` 加修饰器时，Current 会由聚合器接管，
    // 那时这里的写入口径要重新裁决（本阶段不预先设计）。
    // 直写 `FGameplayAttributeData`（= `SetResourceAttribute` 的实现），**不经过 ASC**：
    // 一次扣血在热路径上，这里没有必要也不应该去碰聚合器。
    SetResourceAttribute(UACBattleAttributeSet::GetHealthAttribute(), Clamped);
}

void AACBattleUnitBase::AddCurrentHP(float Delta)
{
    SetHealth(FMath::Clamp(GetCurrentHP() + Delta, 0.f, GetMaxHP()));
}

float AACBattleUnitBase::GetHealthRatio() const
{
    const float MaxHP = GetMaxHP();
    return MaxHP > 0.f ? FMath::Clamp(GetCurrentHP() / MaxHP, 0.f, 1.f) : 0.f;
}

void AACBattleUnitBase::CacheCombatIdentity()
{
    const float Range = GetStat(EACStat::Range);
    BaseRange = FMath::Max(1.f, Range);
    // 近战 / 远程身份：射程 >= 3 远程，<= 2 近战；战斗开始时结算后不再改变。
    bIsRanged = BaseRange >= 3.f;
    Tags.RemoveTag(BattleTags::Unit_Tag_Ranged);
    Tags.RemoveTag(BattleTags::Unit_Tag_Melee);
    Tags.AddTag(bIsRanged ? BattleTags::Unit_Tag_Ranged : BattleTags::Unit_Tag_Melee);
}
