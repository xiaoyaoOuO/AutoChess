// 阶段 0a 新增（GAS 重构实施方案 §2.1 / §2.2），阶段 1 挂上 GAS 地基：
// 本文件是 Battle/ACBattleUnit.{h,cpp} 的 **Actor 化替代**，逻辑一字未改，只换承载形式：
//   ① C1：一个单位一个 Actor，Actor 直接承载逻辑（AActor 派生，不再是无头 UObject）；
//   ② §2.2：单位 Actor 关闭 PrimaryActorTick，逻辑仍由 UBattleWorld::Step 统一驱动；
//             NoCollision、位置由 UACUnitGridComponent 控制、不使用导航网格；
//             生成方式 SpawnActorDeferred → 填数据 → FinishSpawning（见 ACBattleWorld::Initialize）；
//   ③ **阶段 1（GAS 地基）**：挂 ASC + `UACBattleAttributeSet` + `UACAbilitySetComponent`，
//      实现 `IAbilitySystemInterface`，在 BeginPlay 里 `InitAbilityActorInfo(this, this)`。
//      阶段 1 里 **GAS 只被挂载、不被使用**：属性集当时只做 `FBattleStatSheet` 的镜像，
//      ASC 不被 Tick、不授予能力、不施加 GE —— 行为与阶段 0.5 完全一致（§7 阶段 1 验收）。
//   ④ **阶段 2（属性迁移，§4.3 / §7 阶段 2）**：读权威**反转**到 `UACBattleAttributeSet`：
//      - `GetStat` / `GetMaxHP` / `GetCurrentHP` / `GetHealthRatio` 全部改读属性集；
//      - 当前生命从成员变量 `CurrentHP` 改为属性集 `Health`（**单一真相**）；
//      - 三个派生量（暴击率 / 暴击倍率 / 攻速 Hz）改由 `FBattleStatPipeline` 的静态工具读属性集；
//      - 护盾总量以属性集 `Shield` 为准（`FShieldPool` 仍保留实例数组做 FIFO 与持续护盾）；
//      - **数值一字未改**（C6/D10）：属性集 Base 值的来源仍是 `FACStatBlock`，
//        那正是 Run 层（等级档位 / 锻体 / 装备都折在 `Spec.BaseStats`）交给战斗侧的那一份数。
//   ⑤ 本阶段**不建 Mesh**：表现留给阶段 0c 的 `UACUnitPresentationComponent`；
//   ⑥ 阶段 0b 已删掉旧的单位类型别名，本类一律以 `AACBattleUnitBase` 显式出现；
//      D8（对外结构改弱指针）同样在阶段 0b 落地，见 Core/ACBattleSetup.h 与 UBattleWorld::Resolve。
//
// 与旧的逻辑单位类型（无头 UObject 版）的差异仅限"承载形式 + 生成方式"，属性/生命/资源/位置/行动/目标/定义/召唤/
// 击杀归属/标记的取值语义与调用方约定全部保持不变。
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
// 阶段 3.2b：`Ability/ACAbilityExecutor.h` 的依赖**已删除** —— 那条线（`FAbilityExecutor` /
// `FFocusState` / `FStunEntry` / `FACSkillDef` 成员）整体退场：
//   · 专注 → `UACBattleAttributeSet` 的 `Focus` / `FocusMax`（下面的三个访问器）；
//   · 技能 → 单位 ASC 上由 `UACAbilitySet` 授予的 `UGameplayAbility` 实例；
//   · 眩晕 → ASC 上由状态 GE 授予的 `State.Stun` 标签（不再有成员 bool）。
#include "Combat/ACCombatResolver.h"
#include "GAS/ACBattleAttributeSet.h"
#include "Battle/Components/ACUnitGridComponent.h"
#include "Battle/Components/ACUnitPresentationComponent.h"
#include "Battle/ACAbilitySetComponent.h"
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
     * 阶段 1：初始化 GAS 的 ActorInfo（§2.2：Owner 与 Avatar 同为该 Actor）。
     *
     * ⚠️ 为什么放在 BeginPlay 是**正确**的时序：
     * 单位由 `SpawnActor(..., bDeferConstruction = true)` 创建（ACBattleWorld::SpawnUnitActor），
     * 出生后**不跑构造脚本与 BeginPlay**，等 `UBattleWorld` 调完 `InitializeFromXxx` 把数据填好、
     * 再调 `FinishSpawning` 才触发 BeginPlay。
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

    // ---- 调试标识（§2.2：保留只读 DebugId，否则 D8 之后日志无法排障）----
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

    /**
     * 读基础属性（`EACStat`）。
     *
     * **阶段 2：读权威是属性集**，不再是 `FBattleStatPipeline::GetStat(Sheet, Stat)`。
     * 实现见 .cpp（转发到 `UACBattleAttributeSet` 的 CurrentValue），
     * 那里写明了"为什么直接读属性集而不走 `ASC->GetNumericAttribute`"。
     * 调用点无需改动：全仓读属性一律走本函数（`FBattleStatPipeline::GetStat` 已删除）。
     */
    float GetStat(EACStat Stat) const;
    float GetMaxHP() const;
    /** 当前生命：属性集 `Health` 的当前值（阶段 2 起**不再是成员变量**）。 */
    float GetCurrentHP() const;

    /**
     * 受控的当前生命写入入口（§4.6 新契约：**只有 `FCombatResolver` 能改 `Health`**）。
     *
     * 名字里带 `FromResolver` 就是为了让人一眼看出"这是给结算管线用的"，不是通用 setter：
     *   - 通用 setter（旧的 `SetCurrentHP`）已删除，避免出现第二条改生命的路径；
     *   - 内部写属性集 `Health` 的 **BaseValue + CurrentValue**（同 `SetAttributeDataValue` 口径），
     *     并在写入前按 `[0, MaxHealth]` 夹取 —— 与属性集 `PreAttributeChange` 的 Clamp 同口径，
     *     而"直写 FGameplayAttributeData"这条路引擎不会回调 `PreAttributeChange`，故必须在这里夹。
     */
    void SetHealthFromResolver(float NewHealth);
    void AddCurrentHP(float Delta);
    float GetHealthRatio() const;
    /** 战斗结果持久化口径（`FACUnitBattleResult::RemainingBaseHP`），见 `FBattleStatPipeline::GetBaseMaxHP`。 */
    float GetBaseMaxHP() const { return FBattleStatPipeline::GetBaseMaxHP(Stats); }

    // ---- 资源 ----
    // 阶段 3.2b：`FFocusState& GetFocus()` **已删除**。专注的唯一真相是属性集的两个属性
    //（`Focus` = 当前值、`FocusMax` = 上限，§4.3 的"额外资源属性"）。三个只读访问器足够
    // 覆盖全仓的读法；写专注一律走 `UACBattleAbility::GrantFocusToUnit`（改值走 GE + 钩子 + 埋点）。
    //
    //   `Unit.GetFocus().Current` → `Unit.GetFocusCurrent()`
    //   `Unit.GetFocus().Max`     → `Unit.GetFocusMax()`
    //   `Focus.Current >= Focus.Max`（A15 施放前提）→ `Unit.IsFocusFull()`
    //
    // `Focus.Init` / `Focus.RegenPerSecond` / `Focus.PerAttackGain` 三个派生字段同样删除：
    // 它们的权威是属性集的 `FocusInit` / `FocusRegen` / `FocusPerAttack`（`EACStat` 的第 13/15/16 项），
    // 各消费点直接读属性（`GetStat(EACStat::FocusRegen)` 等），不需要第三份缓存。
    /** 当前专注（属性集 `Focus` 的当前值）。 */
    float GetFocusCurrent() const;
    /** 专注上限（属性集 `FocusMax` 的当前值；`<= 0` = 这个单位没有专注条）。 */
    float GetFocusMax() const;
    /**
     * 专注是否已满（A15 的施放前提；`FocusMax <= 0` 时恒为 false）。
     *
     * **这是 A15 在全仓的唯一定义**：`UACSkillAbilityBase::IsReadyToActivate`（内核侧
     * "该不该去试技能"的判定）直接读它，两处不再各写一遍 `Max > 0 && Current >= Max`。
     */
    bool IsFocusFull() const;

    FORCEINLINE FMentalState& GetMental() { return Mental; }
    FORCEINLINE const FMentalState& GetMental() const { return Mental; }
    // ---- 阶段 3.2a：异常状态查询接口的迁移（旧 `GetStates()` 已删除）----
    // 旧的 `FAbnormalStateContainer& GetStates()` 提供的是"自己拿容器去查 / 去改"的接口
    // （`Has` / `GetStacks` / `Find` / `Apply` / `Remove` / `Tick`）。异常状态的唯一真相
    // 现在是 **ASC 上由状态 GE 授予的标签与层数**，因此查询必须回到 ASC：
    //
    //   `Unit.GetStates().Has(Tag)`      → `Unit.HasStateTag(Tag)`（内部 `HasMatchingGameplayTag`）
    //   `Unit.GetStates().GetStacks(Tag)`→ `Unit.GetStateStacks(Tag)`（内部 `GetTagCount`）
    //
    // 为什么在本类上包一层而不是让调用点自己写 `GetAbilitySystemComponent()->GetTagCount(...)`：
    //   ① 调用点（`ACActionScheduler` / `ACTargetingSystem` / `UBattleWorld` / 各能力）
    //      拿到的都是 `const AACBattleUnitBase&`，逐个判空 ASC 会把同一段样板抄很多遍；
    //   ② "层数就是标签计数"这条口径需要一个唯一落点，否则将来改成"读 GE 的 stack count"
    //      就要全仓改一遍；
    //   ③ 与旧的 `HasTag()`（以及阶段 3.2b 删除的 `IsStunned()`）一样是 `const` 只读查询，
    //      不提供任何写入口 —— **改层数只能通过施加 / 移除状态 GE**（§4.4），
    //      这从接口形状上就堵住了。
    //
    // ⚠️ ASC 缺失（或尚未 `InitAbilityActorInfo`）时一律返回 0 / false：
    //    那等价于"身上没有任何状态"，是安全降级，不会让 gate 判定误判成"被控制住了"。

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
    // 阶段 3.2b：`bStunned` 成员与 `IsStunned()` / `SetStunned()` **已删除**。
    // 眩晕的唯一真相是 ASC 上由 `UACGE_State_Stun`（`HasDuration` + `GrantedTags = State.Stun`）
    // 授予的标签 —— GE 到期标签自动消失，因此内核不再需要任何"眩晕计时器 / 到期解除"逻辑。
    // 查询一律用上面已有的 `HasStateTag(BattleTags::State_Stun)`：
    //   · 调度器门控 → `FActionScheduler::EvaluateGate`；
    //   · 索敌/AI   → `FBattleAISystem::Decide`；
    //   · 能力门控   → `ActivationBlockedTags`（普攻与技能能力都含 `State.Stun`）。
    // 阶段 4：`EACUnitActionState::Stunned` / `Channeling` 两个枚举项**也已删除**
    //（见 `Core/ACBattleTypes.h` 的说明），"行动状态"这一列不再需要表达这两种态。

    // ---- 目标 ----
    FORCEINLINE FUnitId GetCurrentTargetId() const { return CurrentTargetId; }
    FORCEINLINE void SetCurrentTargetId(FUnitId InTargetId) { CurrentTargetId = InTargetId; }

    // ---- 定义 / 普攻 ----
    const UUnitDefinitionBase* GetDefinition() const { return Definition; }
    // 阶段 3.2b：成员 `FACSkillDef SkillDef` 与 `GetSkillDef()` **已删除** ——
    // 技能由"单位 ASC 上被授予的 `UACSkillAbilityBase` 子类"表达（`UACAbilitySet::GrantedAbilities`），
    // 技能 ID / 消耗 / 目标选择器都在能力类上，单位侧不再需要一份只读副本（那是第二份真相）。
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

    // ---- 组件（§2.1：单位基类持有网格与表现两个组件）----
    FORCEINLINE UACUnitGridComponent* GetGridComponent() const { return GridComponent; }
    FORCEINLINE UACUnitPresentationComponent* GetPresentationComponent() const { return PresentationComponent; }

    // ---- GAS（阶段 1 新增，§2.1 / §2.2；阶段 2 起属性集成为读权威；阶段 3.2a 起能力集真正授予）----
    // `GetAbilitySystemComponent()` 是 `IAbilitySystemInterface` 的重写（见上方声明），不在这里再写一遍。
    // 三个 GAS 对象在这几个阶段里的角色变化：
    //   - ASC：**引擎不会自动 Tick 它**。ASC 是 ActorComponent，`TickComponent` 由所在 Actor 的 tick 驱动，
    //     而本类的 `PrimaryActorTick.bCanEverTick = false`（§2.2），所以它拿不到 tick。
    //     这是**刻意**的：GE 时长的推进由 `UBattleWorld::TickFrame` 手动调 `TickComponent(DeltaTime)`
    //     负责（§2.3 第 2 步，阶段 3.2a 已接上）。
    //     ⚠️ 但**周期结算与时长到期不依赖这次手动调用** —— 引擎是在施加 / 更新活动 GE 时
    //     用 `Owner->GetWorld()->GetTimerManager()` 起定时器（GameplayEffect.cpp:4228-4252 / 5119-5160）。
    //     手动 Tick 的意义是"与 §2.3 的目标顺序一致 + 驱动可 Tick 的属性集"，两条路都成立。
    //   - 属性集：**阶段 2 起它是属性的唯一读权威**（`GetStat` 转发到它的 CurrentValue）。
    //     GE 开始施加之后 Current 会自动变成"Base + 聚合修饰器"的结果，读路径一行不用改。
    //   - 能力集组件：**阶段 3.2a 起真正授予**能力与常驻 GE（`GrantToOwner`，在 `BeginPlay` 里调）。

    /**
     * 单位属性集（只读）。**阶段 2 起它是属性的唯一权威**：
     * 基础属性（18 项）由 `InitializeFromStatBlock` 在延迟构造期写入（**不经过 ASC**，
     * 因为那时 `InitAbilityActorInfo` 还没跑），资源属性（`Health` / `Shield` / `Focus` / `Mental`）
     * 由 `SetResourceAttribute`（内部）、`SetHealthFromResolver`、`FShieldPool::SyncTotalToAttributeSet` 维护。
     *
     * 为什么返回 const 指针：读路径必须是"看得见但不该改" ——
     * 仓库里读属性的地方有几十处，写属性的地方必须屈指可数，类型上就分开才不会靠自觉。
     */
    FORCEINLINE const UACBattleAttributeSet* GetAttributeSet() const { return AttributeSet; }

    /**
     * 可写属性集指针。
     *
     * ⚠️ 唯一的合法写入方是 `AACBattleUnitBase` 自己的受控入口（`SetHealthFromResolver` /
     * `SetResourceAttribute`）与 `FShieldPool::SyncTotalToAttributeSet`（护盾总量同步）。
     * **不要在别处拿它改属性**：§4.6 的契约是"只有 `FCombatResolver` 能改 `Health`"，
     * 绕过去会让日志出现"血量变了但没有对应伤害记录"的静默不一致。
     */
    FORCEINLINE UACBattleAttributeSet* GetMutableAttributeSet() { return AttributeSet; }

    /** 能力 / GE 授予清单组件（§2.1 的第三个组件）；阶段 1 只挂载，不授予任何东西。 */
    FORCEINLINE UACAbilitySetComponent* GetAbilitySetComponent() const { return AbilitySetComponent; }

    /** 阶段 0a 桥接：把 Cell / bOnBoard 镜像给网格组件（阶段 0b 由组件接管为唯一出口）。 */
    void SyncGridComponent();

    // ---- 对象池预留（§2.2 / §6.3 D11：本阶段不做池，只留接口，将来加池不改调用方）----
    UPROPERTY()
    bool bReusable = false;

    /** 池化复用时重置单位状态；阶段 0a 是空实现（C7：暂不做对象池）。 */
    virtual void ResetForReuse() {}

    // ---- 标记 ----
    UPROPERTY() bool bOnBoard = false;
    UPROPERTY() bool bStealthed = false;
    UPROPERTY() bool bUntargetable = false;
    UPROPERTY() bool bOccupyCell = true;
    UPROPERTY() bool bSelectable = true;
    UPROPERTY() bool bCountsAsKill = true;

private:
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

    /**
     * 攻速转换规则（王恩 / 蕾拉 / 科雷 / 那摩）。
     *
     * 阶段 2 从 `FBattleStatSheet::Conversions` 挪到这里：`FBattleStatSheet` 现在只是
     * `FACStatBlock` 的落地形态（一个纯 Base 数组），不该再挂"会改数值的规则"。
     * 规则由 `ApplyStatConversionRules` 在**属性集初始化之前**作用于输入块。
     * 现阶段全仓没有任何写入点（内容侧尚未填规则），因此当前是空容器、行为等价于空操作。
     */
    TArray<FACStatConversionRule> Conversions;

    // 阶段 3.2b：成员 `FFocusState Focus` **已删除**（专注的唯一真相是属性集 `Focus` / `FocusMax`，
    // 见上面三个访问器）。留一份本地副本就等于留第二份真相：GE 改属性时它不会跟着动。
    FMentalState Mental;
    // 阶段 3.2a：`FAbnormalStateContainer States` 成员**已删除**。
    // 异常状态的唯一真相是 ASC 上由状态 GE 授予的标签（层数 = `GetTagCount`），
    // 留一份本地数组就等于留第二份真相 —— 那正是本次重构要消除的东西。
    FShieldPool Shields;

    // 阶段 2：成员 `float CurrentHP` **已删除**。"当前生命"的唯一真相是属性集的 `Health`
    //（`GetCurrentHP` / `AddCurrentHP` / `SetHealthFromResolver` 全部读写它）。
    // 留一个成员变量就等于留第二份真相：任何一处忘记同步都会变成静默的数值分叉。

    EACUnitActionState ActionState = EACUnitActionState::Idle;
    // 阶段 3.2b：`bool bStunned` **已删除**（眩晕改查 ASC 上的 `State.Stun` 标签，
    // 理由见上面"行动"那一节的注释）。
    bool bPendingDeath = false;
    bool bIsDead = false;
    // 阶段 0.5：原来的 `float StunEndTick` 是**从未被读写的死字段**（眩晕到期时间当时由
    // `FAbilityExecutor::FStunEntry::EndTime` 权威持有），随 tick→秒 一起删除；
    // 阶段 3.2b 连那条线本身也删了 —— 现在"眩晕什么时候到期"由引擎的 GE 定时器回答。

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
    TObjectPtr<const UUnitDefinitionBase> Definition = nullptr;
    // 阶段 3.2b：成员 `FACSkillDef SkillDef` **已删除**（技能内容现在住在能力类上，见上面说明）。
    FACAttackPatternDef AttackPattern;

    /** 网格位置组件：单位"格坐标 ↔ 世界坐标"的唯一出口（§2.1）。 */
    UPROPERTY(VisibleAnywhere, Category="Battle|Unit")
    TObjectPtr<UACUnitGridComponent> GridComponent = nullptr;

    /** 表现组件：Mesh / 朝向 / 血条挂点；逻辑不得读写它的状态（§2.2）。 */
    UPROPERTY(VisibleAnywhere, Category="Battle|Unit")
    TObjectPtr<UACUnitPresentationComponent> PresentationComponent = nullptr;

    /**
     * 技能系统组件（§2.1）。Owner 与 Avatar 都是本 Actor（`InitAbilityActorInfo(this, this)`，在 BeginPlay）。
     * 阶段 1 只挂载：不 Tick、不授予能力、不施加 GE。
     */
    UPROPERTY(VisibleAnywhere, Category="Battle|GAS")
    TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent = nullptr;

    /**
     * 属性集（§2.1）。**阶段 2 起它是属性的唯一读权威**（`GetStat` 就是它的 CurrentValue）；
     * 阶段 1 时它只是 `FBattleStatSheet` 的镜像。可写指针见上面 `GetMutableAttributeSet()`。
     */
    UPROPERTY(VisibleAnywhere, Category="Battle|GAS")
    TObjectPtr<UACBattleAttributeSet> AttributeSet = nullptr;

    /** 能力 / GE 授予清单组件（§2.1 的第三个组件）；阶段 1 只挂载，不授予任何东西。 */
    UPROPERTY(VisibleAnywhere, Category="Battle|GAS")
    TObjectPtr<UACAbilitySetComponent> AbilitySetComponent = nullptr;
};
