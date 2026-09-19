// 阶段 1 新增骨架，**阶段 3.1b 补完**（GAS 重构实施方案 §0 C9 / §2.3 / §4.1 / §5.1 / §6.3 / §7 阶段 3.1）：
// **技能 / 普攻 / 被动的共同基类**。
//
// 三种能力在本方案里同源：`FACSkillDef` → 技能能力、`FACAttackPatternDef` → 普攻能力、
// 被动能力（事件触发）—— 它们都要"取战斗世界、取施法者、取目标、取时间"。
// 把这几件事收口到一个基类，能力子类就只写自己的时序与效果，不再各自 Cast 一遍。
//
// 裁决依据（§0 C9）：**技能目标解析不用 `UGameplayAbilityTargetActor`**。
// 六边形棋盘的目标选择规则（`EACSelectorType` 15 种）已经完整实现于 `FTargetingSystem`，
// TargetActor 体系解决的是"玩家在 3D 场景里用鼠标/摇杆指哪打哪"，
// 而自走棋的攻击目标由 AI 与选择器决定、玩家只下达"对谁用"的命令 ——
// 接 TargetActor 只会引入一条复制/预测链路和一套 Confirmation 生命周期，换不到任何东西。
// 因此 `ResolveTargets` 直接调 `UBattleWorld::Targeting()`（§6.3 同款裁决）。
//
// ---------------------------------------------------------------------------
// 阶段 3.1b 相对阶段 1 骨架补上的五件事
// ---------------------------------------------------------------------------
//   ① `ResolveTargets` 落地：占位（返回 false）→ 真调 `FTargetingSystem::ResolveSelector`；
//   ② `IsInRange` / `MoveOneStepTowards` 从 `FAbilityExecutor` **逐字搬来**
//      （§3.2 会删 `Ability/ACAbilityExecutor.{h,cpp}`，本阶段**只复制、不删原件**）；
//   ③ 专注消耗"包一层 Cost GE"：`CheckCost` 覆写 + 提交后把 `Focus` 夹回 `[0, FocusMax]`
//      （为什么必须包，见 .cpp 里那段带引擎行号的注释 —— 不包就不是"清空专注"而是"永远付不起"）；
//   ④ 钩子**双发**入口（EventBus + GameplayEvent，见下）；
//   ⑤ 技能施放埋点：口径与旧 `ACAbilityExecutor.cpp` 的 `Ability | SkillId` 逐字一致。
//
// ---------------------------------------------------------------------------
// 为什么钩子要"双发"（§0 C10 的落地，本基类只有这一处约定）
// ---------------------------------------------------------------------------
// `FBattleEventBus` 与 GAS 的 GameplayEvent 分工**不是二选一**，而是两类语义：
//   - **`EventBus.Dispatch`（保留）**：服务"可取消 / 可改值"的内核内部钩子。
//     `Dispatch` 返回 `FBattleHookResult`，订阅者能改 `FBattleHookContext` 的
//     `FloatValue` / `bCancelled`，也能用 `bCancel` / `bInterrupt` 否决整次动作 ——
//     静电紊乱（`UBattleWorld::HandleActionTriggered` 订阅 `Hook.BeforeAttack` /
//     `Hook.Unit.Moved` / `Hook.SkillCast`）、坦克受击回专注（订阅 `Hook.Damage.Take`）
//     依赖的正是这个能力。而 `FGameplayEventData` **没有**"回写 + 否决"的通路。
//   - **GameplayEvent（新增）**：服务"只负责触发"的玩法被动。
//     被动能力靠 `AbilityTriggers = { GameplayEvent, Hook.Xxx }` 被唤起 —— 这是 GAS 的原生机制，
//     替掉旧的 `FEffectSystem::HandleHook`（遍历订阅表 + `MaxTriggers` 计数）。
// 两条通道**同 Tag、同上下文、同一时刻**发出，因此 3.3 接线后内核订阅者与被动能力都能照旧收到；
// 分工边界写在 `DispatchHook()` 一处，不靠调用方自觉。
//
// ⚠️ **没有任何成员**叫 `ASC::SendGameplayEvent`（已核实 5.6 全库）：
//   - `UGameplayAbility::SendGameplayEvent(Tag, Payload)`（GameplayAbility.h:561）只发给**自己**的 ASC；
//   - `UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(Actor, Tag, Payload)`
//     （AbilitySystemBlueprintLibrary.h:83）要先拿到 Actor；
//   - 底层入口是 `UAbilitySystemComponent::HandleGameplayEvent(Tag, const FGameplayEventData*)`
//     （声明 AbilitySystemComponent.h:756，实现 AbilitySystemComponent_Abilities.cpp:2536）。
//   本基类走**底层入口**，因为"发给谁"必须由调用点决定（自己 / 主目标 / 全场广播）。
//
// ⚠️ **载荷能带什么、不能带什么**（写在这里，免得子类以为能带任意字段）：
//   `FGameplayEventData`（GameplayAbilityTypes.h:230-283）的可用字段是
//   `Instigator` / `Target` / `OptionalObject` / `OptionalObject2` / `ContextHandle` /
//   `InstigatorTags` / `TargetTags` / `EventMagnitude` / `TargetData`。
//   本基类的映射：`Context.Source → Instigator`、`Context.Target → Target`、
//   `Context.FloatValue → EventMagnitude`、`Context.Tags → InstigatorTags`。
//   **`IntValue` / `bBoolValue` 装不进去**（没有整型/布尔载荷位）。
//   当前内容里这两个字段的用处只有"静电紊乱的层数"与"是否有源伤"这类**内核**语义
//   （内核读的是 EventBus 那份上下文），本阶段产出的 7 个被动**一个都不用**它们；
//   将来若真有被动要用，再按 §4.2 的路子加一个"挂 `FBattleHookContext` 的 UObject"当 `OptionalObject`。
#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "Core/ACBattleTypes.h"
#include "Events/ACBattleEventBus.h"
#include "ACBattleAbility.generated.h"

class UBattleWorld;
class AACBattleUnitBase;
class UGameplayEffect;

UCLASS(Abstract)
class AUTOCHESSBATTLE_API UACBattleAbility : public UGameplayAbility
{
    GENERATED_BODY()

public:
    /** 单机项目：不参与网络复制与客户端预测（实施方案 §7 阶段 1）。 */
    UACBattleAbility();

    // ---------------------------------------------------------------------
    // 世界 / 施法者
    // ---------------------------------------------------------------------

    /**
     * 取战斗世界。
     *
     * ⚠️ **阶段 3.1b 修正了阶段 1 的取法**（这是一处会静默失效的坑，已核实）：
     *   阶段 1 的注释写的是"单位 Actor 的 Outer 链最终落在 `UBattleWorld`"，
     *   但 `UBattleWorld::SpawnUnitActor` 并没有设 `SpawnParameters.OverrideLevel`，
     *   而 `UWorld::SpawnActor` 是 **`NewObject<AActor>(LevelToSpawnIn, ...)`**
     *   （`Engine/Private/LevelActor.cpp:671`，`FActorSpawnParameters` 里根本没有 `Outer` 字段）
     *   —— 单位的 Outer 是 **`ULevel`**，不是 `UBattleWorld`。
     *   因此本函数按"两条路径"取世界：
     *     ① 快路径：`OwnerActor->GetOuter()` 能 Cast 成 `UBattleWorld` 时直接用
     *        （保留阶段 1 的语义，将来若有人把单位 Outer 指过去，这里立即生效）；
     *     ② 实际生效路径：单位 Actor → `GetWorld()` → `GetGameInstance()`
     *        → `UBattleSubsystem::GetSession()` → `UBattleSession::GetBattleWorld()`。
     *        `UBattleSubsystem` 是 `UGameInstanceSubsystem`（`Flow/ACBattleSubsystem.h:15`），
     *        单机单场战斗下这条链是唯一且确定的。
     *   **为什么不用 `GetAbilitySystemComponentFromActorInfo()` 找世界**：`CanActivateAbility`
     *   是在 **CDO** 上被调用的（GameplayAbility.cpp:426 的注释明写 "Don't set the actor info"），
     *   那时 `CurrentActorInfo` 是空的，经 ASC 取世界会拿到 nullptr。
     *
     * 拿不到世界返回 nullptr（**不 check 崩溃**）：能力只应在战斗世界里被激活，
     * 但在编辑器里手工试放 / 蓝图预览时拿不到世界是正常情况，不该把编辑器打崩。
     * 调用方**必须**判空。
     */
    UBattleWorld* GetBattleWorld() const;

    /**
     * 从任意 Actor（通常是单位）反查战斗世界；上一条注释里的两条路径都在这里。
     *
     * 为什么做成 public static：**这是全项目唯一正确的"从单位取世界"口径**，
     * 而同样的错误写法（`Cast<UBattleWorld>(Actor->GetOuter())`）目前散落在
     * `ACDamageExecution.cpp:204`、`ACHealExecution.cpp:140`、`ACPeriodicDamageExecution.cpp:167`、
     * `ACGE_Shield.cpp:67`、`ACSummonEffectComponent.cpp:67/75`、`ACKillReviveEffectComponent.cpp:49/55`
     * 共 8 处 —— 它们**全部会取到 nullptr**（伤害/治疗/护盾会被静默丢弃）。
     * 阶段 3.1b 不动那些文件（它们不属于本阶段），但把正确实现放在这里，
     * 3.3 接线时把那 8 处换成 `UACBattleAbility::FindBattleWorldFromActor(...)` 即可。
     */
    static UBattleWorld* FindBattleWorldFromActor(const AActor* Actor);

    /**
     * 取施法者单位：`GetAvatarActorFromActorInfo()` → `AACBattleUnitBase`；失败返回 nullptr。
     *
     * ⚠️ 只能在**能力已激活**时用（依赖 `CurrentActorInfo`）。目标解析请用
     * `ResolveTargets(const AACBattleUnitBase&, ...)`，因为 `CanActivateAbility` 跑在 CDO 上。
     */
    AACBattleUnitBase* GetCasterUnit() const;

    // ---------------------------------------------------------------------
    // 目标解析（C9 / §6.3：不用 UGameplayAbilityTargetActor）
    // ---------------------------------------------------------------------

    /**
     * 解析本次施放的目标（核心实现）。
     *
     * @param Caster     施法者，**必须显式传入**：`CanActivateAbility` 在 CDO 上被调用，
     *                   此时 `this` 上没有可用的单位。
     * @param OutTargets 解析结果（§5.1：目标列表用整型 `FUnitId`，热路径零解引用）。
     * @return true  = 解析动作本身成功完成（`OutTargets` 可能为空，那是"本来就没目标"）；
     *         false = 解析根本没做（没有世界 / 施法者不在战斗注册表里）。
     *         调用方必须区分这两者：把 false 当成"没目标"会把装配错误伪装成合法结果。
     *
     * 选择器与半径来自 `GetTargetSelector()` / `GetSelectorRadius()`（子类覆写）。
     */
    bool ResolveTargets(const AACBattleUnitBase& Caster, TArray<FUnitId>& OutTargets) const;

    /** 便捷重载：用当前 Avatar 当施法者（只能在能力已激活、ActorInfo 就绪时用）。 */
    bool ResolveTargets(TArray<FUnitId>& OutTargets) const;

    /**
     * 目标选择器（默认 `PrimaryTarget`：复用 AI 已锁定的目标）。子类按内容覆写。
     *
     * ⚠️ 形参收施法者而**不是**读 `GetCasterUnit()`：本函数会在 `CanActivateAbility` 里被调用，
     * 而那个语境跑在 **CDO** 上（没有实例态）。普攻要靠施法者的
     * `FACAttackPatternDef::bHealAttack` 才能决定"打敌人"还是"奶队友"，因此必须按形参取。
     */
    virtual EACSelectorType GetTargetSelector(const AACBattleUnitBase& Caster) const { return EACSelectorType::PrimaryTarget; }

    /** 选择器半径（`NeighborsN` 一类形状查询用；默认 1，与 `FACSkillDef::SelectorRadius` 默认一致）。 */
    virtual int32 GetSelectorRadius(const AACBattleUnitBase& Caster) const { return 1; }

    /**
     * 本次施放使用的射程覆盖值（喂给 `IsInRange` 的 `RangeOverride` 形参）。
     * `<= 0` = 用施法者基础射程（对应旧 `FACSkillDef::RangeOverride = -1`）；
     * 普攻覆写成"按 `FACAttackPatternDef::PreferredRange`"。
     */
    virtual float GetEffectiveRangeOverride(const AACBattleUnitBase& Caster) const { return -1.f; }

    /**
     * 是否要求"启动时能解析出目标、且主目标在射程内"（旧 `FACSkillDef::bRequireTargetInRange`）。
     *
     * 为什么要有这个开关：技能/普攻的目标由**内容**给定（必须能解析出来才算能施放），
     * 而事件触发的**被动**的目标是"触发时刻才决定"的（例如"死者周围 1 格"），
     * 启动时拿不到也不该被挡下。被动子类统一覆写为 false。
     */
    virtual bool RequiresTargetInRange() const { return true; }

    /**
     * **内核侧门控**：这次行动该不该"尝试"激活本能力（§2.3 第 5 步的"技能可放"判定）。
     *
     * 为什么不把它塞进 `CanActivateAbility`：那条路 `TryActivateAbility` 一定会走 ——
     * 引擎判不过就返回 false。而内核需要的**不是"失败了"这个结果，而是"该不该去尝试"**：
     * §2.3 要求"技能放不出就 fallback 普攻"，只有当内核能先问出"技能现在不满足条件"，
     * 才能正确地跳过技能去放普攻（否则每次都先试技能、失败再试普攻，语义上等价但多一次
     * 无意义的 `CanActivateAbility`，而且无法区分"技能条件不满足"与"技能被门控挡下"）。
     * 把 A15（专注满才放）放在这里，`CanActivateAbility` 就能保持"纯粹的 GAS 门控"
     * （阻塞标签 / 冷却 / Cost / 目标射程），两者职责不重叠。
     *
     * ⚠️ 本函数会在 **CDO** 上被调用（内核拿到的 `FGameplayAbilitySpec::Ability` 就是 CDO，
     *   `GameplayAbilitySpec.h:196-198` 明写 "Always the CDO"），因此**只能读形参**，
     *   不得读任何实例状态（`CurrentActorInfo` / `CurrentSpecHandle` / 成员变量）——
     *   这也正是它收 `Caster` 而不是用 `GetCasterUnit()` 的原因（后者在 CDO 上恒为 nullptr）。
     *
     * 默认 `true`（普攻与被动没有额外前提）；`UACSkillAbilityBase` 覆写成 A15 的"专注满"。
     */
    virtual bool IsReadyToActivate(const AACBattleUnitBase& Caster) const { return true; }

    /**
     * 能力门控。
     *
     * 覆写点只有一处：在 `Super::CanActivateAbility`（它已经处理了
     * `ActivationBlockedTags`（沉默/眩晕）、冷却、Cost、输入阻断）之后，
     * 追加"目标解析 + 射程"检查 —— §4.1 要求射程判定放这里，距离走 `UACHexGridStatics::Distance`。
     *
     * ⚠️ 本函数可能在 **CDO** 上被调用（GameplayAbility.cpp:426），因此只能读：
     * 形参里的 `ActorInfo` / `Handle`，以及传入的施法者 Actor；
     * **不得**读 `CurrentActorInfo` / `CurrentSpecHandle` / 任何实例状态（那会是上一次激活的残留值）。
     */
    virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                    const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags,
                                    FGameplayTagContainer* OptionalRelevantTags) const override;

    // ---------------------------------------------------------------------
    // 位置（`IsInRange` / `MoveOneStepTowards`：逐字搬自 `FAbilityExecutor`）
    // ---------------------------------------------------------------------

    /**
     * 射程判定：`Distance <= Range`，`RangeOverride <= 0` 时用施法者射程。
     * 六边形距离走 `UACHexGridStatics::Distance`（六边形几何，GAS 无对应实现，§11 B）。
     *
     * 来源：`ACAbilityExecutor.cpp:106-111`（`FAbilityExecutor::IsInRange`），只把
     * `const UBattleUnit&` 换成 `const AACBattleUnitBase&` —— 阶段 0b 已经换过类型，
     * 因此这里是**逐字复制**。
     */
    bool IsInRange(const AACBattleUnitBase& Self, const AACBattleUnitBase& Target, float RangeOverride = -1.f) const;

    /**
     * 朝目标走一格：BFS 最短路（走到"距目标 <= 期望射程"的格子）→ 占位移动 → 派发移动钩子。
     *
     * 来源：`ACAbilityExecutor.cpp:203-252`（`FAbilityExecutor::MoveOneStepTowards`），逐字复制。
     * 派发的两个通道是**静电紊乱的触发点**，一个都不能漏：
     *   ① `Events().Broadcast(Hook.Unit.Moved)` + `IntValue = EACMoveReason::Pathing`（给 `Listen` 的观察者）；
     *   ② `DispatchHook(Hook.Unit.Moved, …, &Unit)`（给钩子订阅者 —— `UBattleWorld::HandleActionTriggered`
     *      在这里扣静电紊乱伤害 —— 同时把 GameplayEvent 发给移动者的 ASC）。
     *
     * ⚠️ **阶段 3.2b 把本函数改成 `static` 并显式收 `UBattleWorld&`**（3.1b 产出时它是非静态成员，
     *   实现里靠 `GetBattleWorld()` 取世界）。原因是**内核要调它**：§2.3 第 5 步在
     *   "普攻也放不出（超距）"时让单位朝目标走一格，而内核手里只有 CDO ——
     *   `GetBattleWorld()` 走的是 `GetAvatarActorFromActorInfo()`，在 CDO 上恒为 nullptr，
     *   非静态版本在内核手里**必然静默失败**。把世界显式传进来是唯一正确的姿势。
     *   3.1b 产出本函数时全仓**没有任何调用者**（已 grep 核实），因此这次改签名不影响任何调用点。
     *
     * @return true = 真的走了一格（调用方据此判断"本帧是移动而不是攻击"）。
     *         已在期望射程内 / 无路可走 / 目标不存在时返回 false（不移动）。
     */
    static bool MoveOneStepTowards(UBattleWorld& World, AACBattleUnitBase& Unit, FUnitId TargetId);

    // ---------------------------------------------------------------------
    // 钩子双发（见文件头"为什么钩子要双发"）
    // ---------------------------------------------------------------------

    /**
     * 钩子派发入口：EventBus（可取消/可改值）+ GameplayEvent（玩法被动触发）。
     *
     * @param EventRecipient 事件要发给哪个单位的 ASC；`nullptr` = 只走 EventBus。
     *                       技能/普攻一律传**施法者自己**（自己的被动由自己的钩子触发）；
     *                       "无 owner 过滤"的钩子（`Hook.Kill` / `Hook.Death` / `Hook.BattleStart`）
     *                       由内核用 `UBattleWorld::BroadcastHookGameplayEvent` 广播给全场。
     * @return EventBus 的返回值（`bCancel` / `bInterrupt`），调用方据此决定是否中止本次动作。
     */
    FBattleHookResult DispatchHook(FGameplayTag HookTag, FBattleHookContext& Context,
                                   AACBattleUnitBase* EventRecipient) const;

    /**
     * **static 版双发入口**（阶段 3.2b 新增）：把世界显式传进来，供"没有能力实例"的调用点使用
     * —— 目前是 `MoveOneStepTowards`（它已经被改成 static，因为内核要调它）。
     *
     * 为什么不让 static 版自己去 `GetBattleWorld()`：那条路走 `GetAvatarActorFromActorInfo()`，
     * 在 CDO 上恒为 nullptr。**双发规则只有这一份实现**（成员版转发到这里），
     * 因此"EventBus 先发、被否决就不再触发被动"这条顺序不会出现两份互相矛盾的版本。
     */
    static FBattleHookResult DispatchHook(UBattleWorld& World, FGameplayTag HookTag, FBattleHookContext& Context,
                                          AACBattleUnitBase* EventRecipient);

    /** 只走 GameplayEvent 通道（发给一个单位）；返回被触发的能力条数（引擎 `HandleGameplayEvent` 的返回值）。 */
    int32 SendHookToUnit(AACBattleUnitBase* Recipient, FGameplayTag HookTag, const FBattleHookContext& Context) const;

    /** static 版：与成员版同一份实现（世界显式传入）。 */
    static int32 SendHookToUnit(UBattleWorld& World, AACBattleUnitBase* Recipient, FGameplayTag HookTag,
                                const FBattleHookContext& Context);

    /**
     * 广播 GameplayEvent 给全场存活单位（旧 `FEffectSystem::HandleHook` 的"无 owner 过滤"语义）。
     *
     * ⚠️ 阶段 3.2b：**实现已上移到 `UBattleWorld::BroadcastHookGameplayEvent`**（那个"全场"概念
     *   本来就属于内核，且内核必须能广播 —— `Hook.BattleStart` / `Hook.Kill` / `Hook.Death`
     *   三个钩子由内核派发，而内核手里只有 CDO）。本函数保留为转发，供能力在激活期间使用，
     *   避免"同一件事两份实现"。
     */
    int32 BroadcastHookGameplayEvent(FGameplayTag HookTag, const FBattleHookContext& Context) const;

    /**
     * `FBattleHookContext` → `FGameplayEventData` 的**唯一**映射处（字段对应见文件头）。
     * 需要世界才能把整型 `FUnitId` 还原成 Actor（`Instigator` / `Target` 收的是 `AActor*`）。
     *
     * ⚠️ 阶段 3.2b 新增了这个收 `World` 的 **static 重载**：内核要广播
     *   `Hook.BattleStart` / `Hook.Kill` / `Hook.Death`，而它手里只有 CDO ——
     *   成员版靠 `GetBattleWorld()` 取世界，在 CDO 上恒为 nullptr，那条路在内核里静默失效。
     *   字段映射只有一份实现（成员版转发到本重载），因此不存在"两处映射表分叉"的风险。
     */
    static FGameplayEventData MakeEventPayload(const UBattleWorld& World, const FBattleHookContext& Context);

    /** 成员版：世界取自身（只能在能力已激活时用）。取不到世界时返回默认载荷（不崩）。 */
    FGameplayEventData MakeEventPayload(const FBattleHookContext& Context) const;

    /**
     * 反向：被动能力在 `ActivateAbility` 里把 `TriggerEventData` 还原成钩子上下文。
     * 传 nullptr 时返回"Source/Target 都是 InvalidUnitId"的上下文（不崩，调用方判 Invalid 即可）。
     */
    static FBattleHookContext MakeHookContext(const FGameplayEventData* TriggerEventData);

    // ---------------------------------------------------------------------
    // GameplayEffect 施加（能力内的统一出口）
    // ---------------------------------------------------------------------

    /**
     * 把一个 GE 施加到目标单位（走 `MakeOutgoingSpec` + `ApplyGameplayEffectSpecToTarget`）。
     *
     * 为什么收 `SourceUnit` 而不是用 `this` 的 ActorInfo：**这个函数是 static**，
     * 既要能在能力里用，也要能被内核（3.3 的专注回复等）复用；
     * 而且 context 的 instigator 必须指向**来源单位** —— `AggregateBySource` 的状态 GE
     * 靠它判定"同源才叠加"（`FindStackableActiveGameplayEffect`，GameplayEffect.cpp:3518-3522）。
     *
     * @param SetByCallerValues 键名 → 值；空表示这个 GE 不需要 SetByCaller。
     *                          键名常量在各自的 GE 类上（例如 `UACGE_Shield::GetDurationDataName()`），
     *                          不要在调用点写字符串字面量。
     * @param Stacks            层数（`FGameplayEffectSpec::SetStackCount`，GameplayEffect.h:1059）；
     *                          旧 `ApplyAbnormalState(Stacks = 3)` 就是这条路径。
     * @return true = spec 已成功交给目标 ASC（不代表目标真的接受了层数，那由引擎的叠加策略决定）。
     */
    static bool ApplyEffectToUnit(AACBattleUnitBase& SourceUnit, AACBattleUnitBase& TargetUnit,
                                  TSubclassOf<UGameplayEffect> EffectClass,
                                  const TMap<FName, float>& SetByCallerValues, int32 Stacks = 1);

    /** 便捷重载：不需要 SetByCaller 的 GE（绝大多数内容 GE 都属于这一类）。 */
    static bool ApplyEffectToUnit(AACBattleUnitBase& SourceUnit, AACBattleUnitBase& TargetUnit,
                                  TSubclassOf<UGameplayEffect> EffectClass, int32 Stacks = 1);

    /**
     * 组一条 SetByCaller 的便捷工厂：`ApplyEffectToUnit(..., MakeSetByCaller(Name, Value))`。
     * 当前内容里每条 GE 最多只需要一条 SetByCaller，因此不需要更复杂的容器接口。
     */
    static TMap<FName, float> MakeSetByCaller(FName DataName, float Value);

    // ---------------------------------------------------------------------
    // 专注（旧 `FAbilityExecutor::GrantFocus` 的等价物）
    // ---------------------------------------------------------------------

    /**
     * 给单位加专注（可为负）。**改值走 GE，钩子与埋点在原处**，逐项等价于旧 `GrantFocus`
     * （`ACAbilityExecutor.cpp:592-630`）：
     *   - 改值：施加 `UACGE_FocusGain`（Instant，`Focus += Amount`，SetByCaller `Data.FocusGain`）
     *     —— §4.1 把 `GrantFocus` 映射到"GE Modifier（Focus）"，所以不再手写属性；
     *   - `Hook.FocusFull`：仅当 `旧 < Max && 新 >= Max` 时派发（旧口径）；
     *   - `Hook.FocusChanged`：仅当值真的变了才派发，`FloatValue` = 变化后的值（旧口径）；
     *   - 埋点 `Focus | FocusChanged`：`Source = InvalidUnitId`、`Target = 单位`、
     *     `ValueA = 变化后的值`、`ValueB = **实际**变化量`（不是形参 Amount，理由见旧实现注释）。
     *
     * 被夹取掉的部分（例如满专注时继续回专注）同样不会出现在日志里 —— 靠"前后读一次属性"实现，
     * 与旧的 `FMath::Clamp` 口径一致（属性集侧另有 `PreAttributeChange` 的 `Focus <= FocusMax` 夹取）。
     *
     * @return 实际变化量（新值 - 旧值）。
     */
    static float GrantFocusToUnit(AACBattleUnitBase& Unit, float Amount);

    // ---------------------------------------------------------------------
    // 埋点（口径与旧 `ACAbilityExecutor.cpp` 的 `Ability | SkillId` 逐字一致）
    // ---------------------------------------------------------------------

    /**
     * 写一条 `Category = Ability` 的结构化日志。
     *
     * 旧口径（`ACAbilityExecutor.cpp:33-45` + :397-403）：
     *   Category=`Ability`、EventTag=**SkillId**、Source=施法者、Target=主目标（可能是
     *   InvalidUnitId，例如无目标的自身增益技能）、`Time = FACBattleTime::ElapsedSeconds`。
     *   EventTag 直接用 SkillId 而不是再造一个常量表：脚本按 `category=Ability` 取行、
     *   按 `event` 分组就能逐技能比对。
     * 位置：在 BeforeSkillCast / EnemySkillCast 的取消与打断判定**之后**、在效果**之前** ——
     *   被取消的技能不产生这条记录。
     */
    void LogAbilityEvent(FName EventTag, FUnitId Source, FUnitId Target,
                         float ValueA = 0.f, float ValueB = 0.f, int32 IntValue = 0) const;

protected:
    /**
     * 登记一条"GameplayEvent 触发"（被动能力的唤醒方式，§4.2 的
     * `TriggerType = Hook` → `AbilityTriggers = { GameplayEvent, Hook.Xxx }`）。
     *
     * 字段名已核实（GameplayAbility.h:89-106）：
     *   `FAbilityTriggerData::TriggerTag`（:101，`FGameplayTag`）
     *   `FAbilityTriggerData::TriggerSource`（:105，`TEnumAsByte<EGameplayAbilityTriggerSource::Type>`）；
     *   枚举值 `EGameplayAbilityTriggerSource::GameplayEvent` 声明在 GameplayAbilityTypes.h:110-123。
     * 登记动作发生在 `GiveAbility` 时（引擎在 `OnGiveAbility` 里把它填进
     * `GameplayEventTriggeredAbilities`，AbilitySystemComponent_Abilities.cpp:557-572），
     * 因此"能力必须已被授予"是事件能触发它的前提（见报告的可行性说明）。
     */
    void AddEventTrigger(FGameplayTag HookTag);

    // ---------------------------------------------------------------------
    // Cost（§4.1：CostMode → Cost GE）
    // ---------------------------------------------------------------------

    /**
     * 成本检查（包一层 GAS 的 `CheckCost`）。
     *
     * ⚠️ 为什么必须覆写：`UACGE_Cost_ClearAll` 的幅度是 `Additive -999`（"减到负数再夹回 0"），
     * 而引擎的 `CheckCost` → `CanApplyAttributeModifiers` 对 `Additive` 有一条
     * `当前值 + 幅度 < 0 → 付不起` 的判定（GameplayEffect.cpp:5191-5205），
     * `Focus` 上限只有 100 —— 于是"清空专注"这类 Cost **永远被判为付不起**，
     * 技能一次都放不出来。旧语义里"清空专注"根本不是施放前提（施放前提是"专注满"，在门控之外），
     * 因此对"扣专注型 Cost GE"跳过引擎的负值拒绝，其余 Cost GE 仍走 `Super`。
     */
    virtual bool CheckCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                          FGameplayTagContainer* OptionalRelevantTags) const override;

    /**
     * 提交能力（消耗 + 冷却）并把专注夹回合法区间。
     *
     * 专注消耗**走 Cost GE**（`CommitAbility` 内部按 `CostGameplayEffectClass` 施加），
     * 本函数只补一件引擎不会替我们做的事：**把 `Focus` 夹回 `[0, FocusMax]`**。
     *
     * ⚠️ 为什么需要这一步（第二个引擎行为，已核实）：GE 的幅度落在属性的 **BaseValue** 上
     *   （`ApplyModToAttribute` → `SetAttributeBaseValue`，GameplayEffect.cpp:3803-3831），
     *   而本项目属性集的夹取写在 `PreAttributeChange` —— 引擎只在
     *   `FGameplayAttribute::SetNumericValueChecked`（AttributeSet.cpp:100）里回调它，
     *   也就是说 Base 会被写成 **-999**，只有 Current 被夹回 0。
     *   后果不是"数值难看"而是**功能坏掉**：之后每次回专注都在给 -999 还债
     *   （`Focus += 5` 得到 -994 → 再夹回 0），表现为"清空之后再也攒不满专注"。
     *   因此这里按旧 `FAbilityExecutor::DrainFocus` 的口径（`FMath::Clamp(Current + Amount, 0, Max)`）
     *   把 Base 与 Current 一起写回合法值。
     *
     * @return false = 提交失败（`CommitAbility` 返回 false），调用方必须放弃本次施放。
     */
    bool CommitBattleCost();

    /** Cost GE 是否是"扣专注"型（`Additive` 作用于资源属性 `Focus`）。 */
    bool HasFocusDrainCost() const;

    // ---------------------------------------------------------------------
    // 事件触发（被动）用：载荷还原 + MaxTriggers 计数
    // ---------------------------------------------------------------------

    /**
     * 触发次数计数（旧 `EACTriggerPolicy::Once` / `MaxTriggers` 的等价物）。
     *
     * 为什么用"实例计数器"而不是 `bRetriggerInstancedAbility`：
     *   - GAS 的 `AbilityTriggers` **没有**"最多触发 N 次"这个概念（只有 Tag + 触发源）；
     *   - `bRetriggerInstancedAbility`（GameplayAbility.h:723）解决的是"InstancedPerActor 的能力
     *     还在激活中又被触发时，先 EndAbility 再重触发"（AbilitySystemComponent_Abilities.cpp:1811-1831），
     *     而本阶段的能力全是**同步的**（`ActivateAbility` 里做完就 `EndAbility`），
     *     不存在"还激活着"的窗口，用它等于什么都没做；
     *   - `InstancingPolicy = InstancedPerActor` 保证同一个 ASC 上只有一个 primary instance
     *     （`Spec.GetPrimaryInstance()`，AbilitySystemComponent_Abilities.cpp:2476），
     *     因此成员计数器**跨多次激活存活**，与旧 `FEffectInstance::TriggerCount` 同寿命。
     *
     * @param MaxTriggers `<= 0` = 不限次数；否则达到上限后返回 false（调用方直接结束，不做任何事）。
     */
    bool ConsumeTrigger(int32 MaxTriggers);

    /** 已触发次数（调试/日志用）。 */
    int32 GetTriggerCount() const { return TriggerCount; }

    // ---------------------------------------------------------------------
    // 敌方技能可打断（旧 `ExecuteSkill` 的 `Hook.EnemySkillCast` 分支）
    // ---------------------------------------------------------------------

    /**
     * 本次施放是否要先过 `Hook.EnemySkillCast`（可打断）这道门。
     *
     * 旧行为（`ACAbilityExecutor.cpp:359-364`）：
     *   `if (Unit.GetTeam() == EACTeam::Enemy && Dispatch(Hook_EnemySkillCast).bInterrupt)` →
     *   设为 Idle + `Dispatch(Hook_SkillInterrupted)` + **不施放**。
     *   即"敌方技能可被监视类效果打断，我方技能不受此门约束"。
     * 默认实现照旧：`Caster.GetTeam() == EACTeam::Enemy`。子类可覆写成"对任何人都可打断"
     * （例如将来的 BOSS 特殊技），或返回 false 表示"这个技能不可打断"。
     */
    virtual bool IsEnemySkillCastInterruptible(const AACBattleUnitBase& Caster) const;

    // ---------------------------------------------------------------------
    // 收尾
    // ---------------------------------------------------------------------

    /**
     * 结束能力的统一出口：`EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, bWasCancelled)`。
     * 五个形参的语义见 GameplayAbility.h:628（`bReplicateEndAbility` / `bWasCancelled`）。
     */
    void FinishAbility(bool bWasCancelled);

private:
    /**
     * 触发计数器（`InstancedPerActor` 的 primary instance 上存活，见 `ConsumeTrigger`）。
     * 技能/普攻不会碰它（它们的 `MaxTriggers` 语义是"每帧由调度器决定"，不在能力里计）。
     */
    int32 TriggerCount = 0;
};
