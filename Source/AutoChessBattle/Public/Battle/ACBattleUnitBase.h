#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "Core/ACBattleTypes.h"
#include "Core/ACBattleSetup.h"
#include "Core/ACDataTypes.h"
#include "Stats/ACBattleStats.h"
#include "Mental/ACMentalSystem.h"
#include "Combat/ACCombatResolver.h"
#include "GAS/ACBattleAttributeSet.h"
#include "Battle/Components/ACUnitGridComponent.h"
#include "Battle/ACAbilitySetComponent.h"
#include "GameFramework/NavMovementComponent.h"
#include "ACBattleUnitBase.generated.h"


UCLASS()
class AUTOCHESSBATTLE_API AACBattleUnitBase : public AActor, public IAbilitySystemInterface
{
    GENERATED_BODY()

public:
    AACBattleUnitBase();

    /** 销毁时反注册到 UBattleWorld（当前只占位，见实现）。 */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /**
     * 阶段 1：初始化 GAS 的 ActorInfo
     * 单位由 `SpawnActor(..., bDeferConstruction = true)` 创建（ACBattleWorld::SpawnUnitActor），
     * 出生后不跑构造脚本与 BeginPlay，等 `UBattleWorld` 调完 `InitializeFromXxx` 把数据填好，再调 `FinishSpawning` 才触发 BeginPlay。
     * 因此 BeginPlay 一定发生在"单位数据就绪"之后 —— 这里 `InitAbilityActorInfo` 时，
     * 属性集已经被 `InitializeFromStatBlock` 写好了初值，不会出现"先 Init 再填属性"的空窗。
     */
    virtual void BeginPlay() override;

    /**
     * `IAbilitySystemInterface` 实现（§2.1：单位类实现该接口）。
     * 返回的 ASC 由本 Actor 持有（`CreateDefaultSubobject` 创建），生命周期与单位一致。
     */
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;


    // 生成后由 UBattleWorld 依次调用填数据（不再需要 UObject 时代的 Outer 参数）。
    void InitializeFromPlayerSpec(const FACPlayerUnitSpec& Spec, const UOperatorDefinition* InDefinition);
    void InitializeFromEnemySpec(const FACEnemyUnitSpec& Spec, const UEnemyDefinition* InDefinition);
    void InitializeFromSpawnRequest(const FACUnitSpawnRequest& Request, const UUnitDefinitionBase* InDefinition,
                                    const TArray<float>* InheritedStats);

    // ---- 身份 ----
    FORCEINLINE FUnitId GetUnitId() const { return UnitId; }
    FORCEINLINE void SetUnitId(FUnitId InId) { UnitId = InId; }

    FORCEINLINE EACTeam GetTeam() const { return Team; }
    FORCEINLINE EACUnitKind GetKind() const { return Kind; }
    FORCEINLINE FName GetDefinitionId() const { return DefinitionId; }
    FORCEINLINE int32 GetTierIndex() const { return TierIndex; }

    const FGameplayTagContainer& GetTags() const { return Tags; }
    FGameplayTagContainer& GetMutableTags() { return Tags; }
    bool HasTag(FGameplayTag Tag) const { return Tags.HasTag(Tag); }
    
    /** 仅用于日志与调试绘制，不参与任何逻辑判定、不进结果结构。 */
    FORCEINLINE int32 GetDebugId() const { return DebugId; }
    void SetDebugId(int32 InId) { DebugId = InId; }

    // ---- 生命周期 ----
    FORCEINLINE bool IsAlive() const { return !bIsDead && !bPendingDeath; }
    FORCEINLINE bool IsDead() const { return bIsDead; }
    FORCEINLINE bool IsPendingDeath() const { return bPendingDeath; }
    void MarkPendingDeath(FUnitId KillerId);
    void ClearPendingDeath();
    void MarkDead();
    void Revive();

    /** 可被锁定（普攻/单体技能）：隐身/不可选中/未上场/死亡均不可。 */
    bool CanBeTargeted() const;

    // ---- 属性与生命 ----
    FBattleStatSheet& GetStatsSheet() { return Stats; }
    const FBattleStatSheet& GetStatsSheet() const { return Stats; }

    /** 读基础属性（`EACStat`）。 */
    float GetStat(EACStat Stat) const;
    float GetMaxHP() const;
    /** 当前生命：属性集 `Health` 的当前值（阶段 2 起**不再是成员变量**）。 */
    float GetCurrentHP() const;

    /**
     * 受控的当前生命写入入口。
     * 在写入前按 `[0, MaxHealth]` 夹取 —— 与属性集 `PreAttributeChange` 的 Clamp 同口径，
     * 而"直写 FGameplayAttributeData"这条路引擎不会回调 `PreAttributeChange`，故必须在这里夹。
     */
    void SetHealth(float NewHealth);
    void AddCurrentHP(float Delta);
    float GetHealthRatio() const;
    /** 战斗结果持久化口径（`FACUnitBattleResult::RemainingBaseHP`），见 `FBattleStatPipeline::GetBaseMaxHP`。 */
    float GetBaseMaxHP() const { return FBattleStatPipeline::GetBaseMaxHP(Stats); }

    // ---- 资源 ----
    /** 当前专注（属性集 `Focus` 的当前值）。 */
    float GetFocusCurrent() const;
    /** 专注上限（属性集 `FocusMax` 的当前值；`<= 0` = 这个单位没有专注条）。 */
    float GetFocusMax() const;
    /** 专注是否已满（FocusMax <= 0 时恒为 false）。*/
    bool IsFocusFull() const;

    FORCEINLINE FMentalState& GetMental() { return Mental; }
    FORCEINLINE const FMentalState& GetMental() const { return Mental; }

    /** 是否带有该状态（等价旧 `GetStates().Has(Tag)`）。 */
    bool HasStateTag(FGameplayTag StateTag) const;

    /** 该状态的层数（等价旧 `GetStates().GetStacks(Tag)`）；无 ASC 或无该标签时为 0。 */
    int32 GetStateStacks(FGameplayTag StateTag) const;

    FORCEINLINE FShieldPool& GetShields() { return Shields; }
    FORCEINLINE const FShieldPool& GetShields() const { return Shields; }

    // ---- 位置 ----
    FORCEINLINE FACHexCoord GetCell() const { return Cell; }
    /** 记录格子，并把格子镜像给 UACUnitGridComponent（阶段 0a 桥接，见实现）。 */
    void SetCell(FACHexCoord InCell);
    FORCEINLINE EACFacing GetFacing() const { return Facing; }
    /** 记录朝向，并把朝向镜像给 UACUnitPresentationComponent（阶段 0a 桥接，见实现）。 */
    void SetFacing(EACFacing InFacing);

    // ---- 行动 ----
    FORCEINLINE EACUnitActionState GetActionState() const { return ActionState; }
    FORCEINLINE void SetActionState(EACUnitActionState InState) { ActionState = InState; }

    // ---- 目标 ----
    FORCEINLINE FUnitId GetCurrentTargetId() const { return CurrentTargetId; }
    FORCEINLINE void SetCurrentTargetId(FUnitId InTargetId) { CurrentTargetId = InTargetId; }

    // ---- 定义 / 普攻 ----
    const UUnitDefinitionBase* GetDefinition() const { return Definition.Get(); }
    // 技能由"单位 ASC 上被授予的 `UACSkillAbilityBase` 子类"表达（`UACAbilitySet::GrantedAbilities`），
    // 技能 ID / 消耗 / 目标选择器都在能力类上，单位侧不再需要一份只读副本。
    FORCEINLINE FACAttackPatternDef& GetAttackPattern() { return AttackPattern; }
    FORCEINLINE const FACAttackPatternDef& GetAttackPattern() const { return AttackPattern; }

    FORCEINLINE bool IsRanged() const { return bIsRanged; }
    FORCEINLINE float GetBaseRange() const { return BaseRange; }
    void CacheCombatIdentity();       // 战斗开始时按基础射程结算近/远程（不中途改变）

    // ---- 召唤 / 领域 ----
    FORCEINLINE FUnitId GetOwnerUnitId() const { return OwnerUnitId; }
    FORCEINLINE void SetOwnerUnitId(FUnitId InOwnerId) { OwnerUnitId = InOwnerId; }
    /** 出生时刻（绝对时间，秒）。 */
    FORCEINLINE float GetSpawnTime() const { return SpawnTime; }
    FORCEINLINE void SetSpawnTime(float InTime) { SpawnTime = InTime; }
    /** 到期时刻（绝对时间，秒）；**负值 = 永不过期**（判定统一走 `FACBattleTime::IsExpiredAt`）。 */
    FORCEINLINE float GetExpireTime() const { return ExpireTime; }
    FORCEINLINE void SetExpireTime(float InTime) { ExpireTime = InTime; }

    // ---- 击杀归属 ----
    FORCEINLINE FUnitId GetLastDamageSourceId() const { return LastDamageSourceId; }
    FORCEINLINE void SetLastDamageSourceId(FUnitId InSourceId) { LastDamageSourceId = InSourceId; }
    FORCEINLINE EACDamageReason GetLastDeathCause() const { return LastDeathCause; }
    FORCEINLINE void SetLastDeathCause(EACDamageReason InCause) { LastDeathCause = InCause; }

    FORCEINLINE int32 GetAttackPatternHitsResolved() const { return AttackPatternHitsResolved; }
    FORCEINLINE void IncrementAttackPatternHits() { ++AttackPatternHitsResolved; }
    FORCEINLINE void ResetAttackPatternHits() { AttackPatternHitsResolved = 0; }

    // ---- 组件（单位基类持有网格与表现两个组件）----
    FORCEINLINE UACUnitGridComponent* GetGridComponent() const { return GridComponent; }



    /**
     * 单位属性集（只读）
     * 基础属性（18 项）由 `InitializeFromStatBlock` 在延迟构造期写入（不经过 ASC，
     * 因为那时 `InitAbilityActorInfo` 还没跑），资源属性（`Health` / `Shield` / `Focus` / `Mental`）
     * 由 `SetResourceAttribute`（内部）、`SetHealthFromResolver`、`FShieldPool::SyncTotalToAttributeSet` 维护。
     */
    FORCEINLINE const UACBattleAttributeSet* GetAttributeSet() const { return AttributeSet; }

    /**
     * 可写属性集指针。
     * ⚠️ 唯一的合法写入方是 `AACBattleUnitBase` 自己的受控入口（SetHealthFromResolver /
     * SetResourceAttribute）与 FShieldPool::SyncTotalToAttributeSet（护盾总量同步）。
     */
    FORCEINLINE UACBattleAttributeSet* GetMutableAttributeSet() { return AttributeSet; }

    /** 能力 / GE 授予清单组件（§2.1 的第三个组件）；阶段 1 只挂载，不授予任何东西。 */
    FORCEINLINE UACAbilitySetComponent* GetAbilitySetComponent() const { return AbilitySetComponent; }

    /** 把 Cell / bOnBoard 镜像给网格组件。 */
    void SyncGridComponent();

    // ---- 对象池预留（本阶段不做池，只留接口）----
    UPROPERTY()
    bool bReusable = false;

    /** 池化复用时重置单位状态 */
    virtual void ResetForReuse() {}

    // ---- 标记 ----
    UPROPERTY() bool bOnBoard = false;
    UPROPERTY() bool bStealthed = false;
    UPROPERTY() bool bUntargetable = false;
    UPROPERTY() bool bOccupyCell = true;
    UPROPERTY() bool bSelectable = true;
    UPROPERTY() bool bCountsAsKill = true;

protected:
    void InitCommon(const FACStatBlock& BaseStats, int32 InTierIndex, EACUnitKind InKind, EACTeam InTeam, FName InDefinitionId);

    /** 按属性句柄读属性集的当前值；属性集缺失或句柄无效时返回 0（不崩溃）。 */
    float ReadAttributeCurrentValue(const FGameplayAttribute& Attribute) const;

    /** 把攻速转换规则作用到输入块上（阶段 2：在属性集初始化**之前**调用）。 */
    FACStatBlock ApplyStatConversionRules(const FACStatBlock& Block) const;

    /** 把 `FACStatBlock` 逐项写进属性集（阶段 2：这是属性集 Base 值的唯一写入点）。 */
    void SyncAttributeSetFromStatBlock(const FACStatBlock& Block);

    /** 把一个资源属性的 Base/Current 一起写进属性集（直写 `FGameplayAttributeData`，不走 ASC）。 */
    void SetResourceAttribute(const FGameplayAttribute& Attribute, float Value) const;

    /**
     * 把资源属性的初值写进属性集：`Health` = 当前生命、`Shield` = 0、`Focus` = 当前专注、
     * `Mental` = 当前精神（`Mental` 的权威值在 `FMentalSystem`，这里只是镜像）。
     */
    void SyncResourceAttributes();

    FUnitId UnitId = InvalidUnitId;
    EACTeam Team = EACTeam::Neutral;
    EACUnitKind Kind = EACUnitKind::Operator;
    FName DefinitionId;
    int32 TierIndex = 0;
    int32 DebugId = 0;

    FGameplayTagContainer Tags;
    FACHexCoord Cell;
    EACFacing Facing = EACFacing::Up;

    FBattleStatSheet Stats;

    //攻速转换规则（特例：王恩 / 蕾拉 / 科雷 / 那摩）
    TArray<FACStatConversionRule> Conversions;

    //精神值
    FMentalState Mental;

    FShieldPool Shields;

    EACUnitActionState ActionState = EACUnitActionState::Idle;
    
    bool bPendingDeath = false;
    bool bIsDead = false;

    FUnitId CurrentTargetId = InvalidUnitId;
    FUnitId LastDamageSourceId = InvalidUnitId;
    FUnitId OwnerUnitId = InvalidUnitId;
    EACDamageReason LastDeathCause = EACDamageReason::BasicAttack;

    /** 出生 / 到期时刻（绝对时间，秒）。 */
    float SpawnTime = 0.f;
    float ExpireTime = -1.f;

    bool bIsRanged = false;
    float BaseRange = 1.f;

    int32 AttackPatternHitsResolved = 0;

    /** 定义资产由 UBattleDataSubsystem 通过 UPROPERTY 持有（GC 安全），此处只做弱引用指针。 */
    TWeakObjectPtr<const UUnitDefinitionBase> Definition = nullptr;
    FACAttackPatternDef AttackPattern;

    /** 网格位置组件：单位"格坐标 ↔ 世界坐标"的唯一出口（§2.1）。 */
    UPROPERTY(VisibleAnywhere, Category="Battle|Unit")
    TObjectPtr<UACUnitGridComponent> GridComponent = nullptr;

    /**
     * 技能系统组件（§2.1）。Owner 与 Avatar 都是本 Actor（`InitAbilityActorInfo(this, this)`，在 BeginPlay）。
     */
    UPROPERTY(VisibleAnywhere, Category="Battle|GAS")
    TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent = nullptr;

    /**属性的唯一读权威**（`GetStat` 就是它的 CurrentValue）；*/
    UPROPERTY(VisibleAnywhere, Category="Battle|GAS")
    TObjectPtr<UACBattleAttributeSet> AttributeSet = nullptr;

    /** 能力 / GE 授予清单组件 */
    UPROPERTY(VisibleAnywhere, Category="Battle|GAS")
    TObjectPtr<UACAbilitySetComponent> AbilitySetComponent = nullptr;

    /** 骨骼网格组件 , ASC想要播放Montage，需要通过MeshComponent和AnimInstance进行播放 ，所以这是必要的 */
    UPROPERTY(EditDefaultsOnly)
    TObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent = nullptr;

    /** 动画实例组件 */
    UPROPERTY(EditDefaultsOnly)
    TObjectPtr<UAnimInstance> AnimInstance = nullptr;

    /** 作为人物移动的组件，主要用于移动和旋转 */
    UPROPERTY()
    TObjectPtr<UNavMovementComponent> NavMovementComponent = nullptr;
};
