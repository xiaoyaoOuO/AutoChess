// 战斗输入 / 输出契约（Run <-> Battle）。
// 对应 TechDocs/00_战斗系统架构总览.md §2.4 与 M01/M02。
#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Templates/SubclassOf.h"
#include "Core/ACBattleTypes.h"
#include "ACBattleSetup.generated.h"

// 阶段 3.3（GAS 重构实施方案 §3.2 表「Core/ACBattleSetup.h」一行）：
// 本文件里原来 6 处 `TArray<FName> xxxEffectBlockIds`（旧自研效果系统的 BlockId）
// **已全部改成类引用**。两类替代物：
//   · `TSubclassOf<UGameplayEffect>`   —— 装备 / 强化 / 词条 / 敌人额外效果 / 时间轴条目；
//   · `TSubclassOf<UGameplayAbility>`  —— 装备 / 强化 / 词条带来的**被动能力**
//     （"击杀 20 才生效""每次击杀回专注"这类内容必须先把能力授予 ASC，才能在事件上触发，
//      见 `FACPlayerUnitSpec::GrantedAbilities` 的说明）。
// 收益：拼错 BlockId 从"运行期静默失效"变成"编译期找不到类"，过渡映射表整张删除。
// 注意：这两个类属于 **GameplayAbilities 模块**（引擎模块），不是 AutoChessBattle 的项目类型 ——
// 依赖方向 Core ⇐ Battle 没有被破坏（Core 仍然不认识 `AACBattleUnitBase`，
// 这正是 `FACUnitSpawnRequest` 不加 `UnitClass` 的原因，见该结构体的说明）。
class UGameplayEffect;
class UGameplayAbility;

/** 参战干员快照（由 Run 层在战斗开始前组装）。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACPlayerUnitSpec
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    FName DefinitionId;

    /** 0=C, 1=B, 2=A, 3=S（对应设计文档成长档位）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    int32 TierIndex = 0;

    /** 当前干员等级：0=D, 1=C, 2=B, 3=A, 4=S（装备栏与强化选择使用）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    int32 Level = 0;

    /** 基础属性（无定义资产时可直接使用；否则由定义资产覆盖）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    FACStatBlock BaseStats;

    /** 上一场战斗结束时的基础血量；< 0 表示满血。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    float CurrentBaseHP = -1.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    FACHexCoord SpawnCell;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    EACFacing Facing = EACFacing::Up;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    FGameplayTagContainer ExtraTags;

    /**
     * 个性强化（C/S）带来的战斗内常驻 GE。
     *
     * ⚠️ 只有"被施加即生效"的那一条能放这里。带触发条件（例如 S 级"处刑"的
     * "累计击杀 ≥ 20 后一次性生效"）的强化必须走下面的 `GrantedAbilities`：
     * GE 不会自己监听事件，条件与一次性语义由被动能力的 `AbilityTriggers` 承担。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<TSubclassOf<UGameplayEffect>> UpgradeEffects;

    /** 装备提供的战斗内**常驻** GE（由 `FACRunEquipment::Effects` 汇总而来）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<TSubclassOf<UGameplayEffect>> EquipmentEffects;

    /** 通用词条（B/A）提供的战斗内常驻 GE。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<TSubclassOf<UGameplayEffect>> TraitEffects;

    /**
     * 个性强化 / 装备 / 词条带来的**被动能力**（阶段 3.3 新增的授予通道）。
     *
     * 为什么必须与上面三个 GE 数组并列存在：
     *   旧效果块（`FACEffectBlock`）把"触发条件"和"产生什么效果"写在同一个块里，
     *   而 GAS 把这两件事拆成两个类型 —— `UGameplayAbility::AbilityTriggers` 管"什么时候触发"，
     *   `UGameplayEffect` 管"改什么"。于是"选了处刑强化"这件事必须表达成
     *   **授予能力**（`UACPassive_Upgrade_OP01_S1`）而不是"开局立刻 +50 暴击"，
     *   否则 20 击杀的门槛与一次性语义会整体丢失（阶段 3.2a 的已知退化）。
     *
     * 与 `UACAbilitySet` 的分工（"天生就有" vs "局内进度带来的"）：
     *   · `UACAbilitySet`（按 `DefinitionId` 注册在 `FACBattleDataContext::AbilitySets`）
     *     = 技能 / 普攻 / 天生被动，单位**出生时**由 `UACAbilitySetComponent::GrantToOwner` 授予；
     *   · 本数组 = Run 层按局内进度追加的被动，由 `UBattleWorld` 在生成该单位时
     *     经 `UACAbilitySetComponent::SetExtraAbilities` 注入，**与上面那份清单在同一次
     *     `GrantToOwner` 里一起授予** —— 因此"授予点"仍然只有一个。
     * 两侧内容没有重叠（`Passive_OP01_BloodThirst` 只在清单里，`Upgrade_OP01_S1` 只在这里）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<TSubclassOf<UGameplayAbility>> GrantedAbilities;
};

/** 敌方单位快照（由关卡 / 遭遇配置生成）。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACEnemyUnitSpec
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    FName DefinitionId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    int32 TierIndex = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    FACStatBlock BaseStats;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    FACHexCoord SpawnCell;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    FGameplayTagContainer ExtraTags;

    /** 额外战斗内常驻 GE（例如"本场敌人开局有 5 层静电紊乱"）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<TSubclassOf<UGameplayEffect>> Effects;

    /** 编队/分组 ID：用于"一起生成""多波次"等扩展。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    int32 GroupId = 0;
};

/** 战前一次性注入（学派强化 / 秘术预置 / 委托装备等）。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACExternalModifierSpec
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    FName SourceId;

    /** 作用范围：-1 = 全队，>=0 = 指定单位（按参战顺序索引）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    int32 TargetUnitIndex = -1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<TSubclassOf<UGameplayEffect>> Effects;

    /** 直接属性修饰（无效果块时的快捷通道，例如学派数值强化）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<FName> ReservedUnused;
};

/**
 * 开局时间轴条目（抢攻 / 后发）。
 *
 * "第 X 秒触发一个**能力**"这件事不在这里 —— 它已经由内核派发的
 * `Hook.PostEffectTrigger` GameplayEvent 承担（`UBattleWorld::TickPendingTimelineFires` 广播），
 * 被动能力只要在 `UACAbilitySet` 里声明 `AbilityTriggers = { GameplayEvent, Hook.PostEffectTrigger }`
 * 就能在那一刻被唤醒（§4.5 映射表那一行）。因此本结构只需要"施加哪个 GE"这一个字段。
 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACStartingTimelineSpec
{
    GENERATED_BODY()

    /**
     * 抢攻：开局施加的 GE（时长写在 GE 类上，见下）。
     * 后发：`TriggerSeconds` 到点时施加的 GE。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TSubclassOf<UGameplayEffect> Effect;

    /** true = 抢攻（BattleStart 生效）；false = 后发（第 TriggerSeconds 秒触发）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    bool bPreemptive = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    float TriggerSeconds = 0.f;

    // 阶段 3.3：`float DurationSeconds` **已删除**（原字段名 `DurationSeconds`，3.2b 报告确认无消费者）。
    // 删除依据：`UBattleWorld::ApplyPreBattleConfiguration` 施加时间轴 GE 时只读 `Effect` 与
    // `bPreemptive` / `TriggerSeconds` / `TargetUnitIndex`，**从不读时长** ——
    // 时长写在 GE 类上（心流刃抢攻 4s / 超导线圈后发 5s，与内容里的旧 `DurationSeconds` 逐值一致，C6/D10）。
    // 留一个"看起来能配时长、实际被忽略"的字段比删掉它危险得多。
    // Run 层侧的对应字段 `FACRunTimelineGrant::DurationSeconds` 也一并删除（同一个理由）。

    /** -1 = 全队（我方），否则为参战顺序索引。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    int32 TargetUnitIndex = -1;
};

/** 战斗启动选项。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACBattleLaunchOptions
{
    GENERATED_BODY()

    // 阶段 0.5（D2 / §5.2）：`MaxCatchUpSteps` 已删除 —— `UBattleSession::Tick` 直接
    // `World->Step(DeltaSeconds)`，不再有固定步累积器，也就没有"最多追赶几步"这件事。

    /** <= 0 表示不限制（仅调试）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    float MaxBattleSeconds = 300.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    bool bAllowMysticCards = true;

    // 阶段 4（D7 / §5.4）：`bool bHeadless` **已删除**。
    // 删除依据：C4「不使用无头模式，所有战斗都带表现」。它的两个读取点（会话 Tick 的
    // "是否跳过表现"分支与 `ACRunBattleAssembler::BuildLaunchOptions` 的写入点）都不存在了，
    // 留一个恒为 false 的开关只会让"战斗到底带不带表现"重新变成一个需要推理的问题。

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    int32 LogLevel = 1;
};

/** 战斗输入快照。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACBattleSetup
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    FGuid BattleId;

    // 阶段 4（D3 / §5.3）：`int32 RngSeed` **已删除** —— 战斗内核不再有随机流，
    // 随机直接用 `FMath::FRand` / `FMath::RandRange`（C3），种子与状态哈希一起退场。
    // ⚠️ **Run 层仍然有种子**，那是另一件事：`FACRunMapNode::EncounterSeed` 决定"同一局同一节点
    // 生成同一批敌人"（遭遇生成属 Run 层，§5.3 明文保留），它与战斗内随机无关。

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<FACPlayerUnitSpec> PlayerUnits;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<FACEnemyUnitSpec> EnemyUnits;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<FACExternalModifierSpec> ExternalModifiers;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Setup")
    TArray<FACStartingTimelineSpec> StartingTimeline;
};

/** 战斗内命令类型。 */
UENUM(BlueprintType)
enum class EACCommandType : uint8
{
    None,
    UseMysticCard,
    AbandonBattle,
    Debug
};

/** 战斗内命令（外部命令 + 内部延迟请求共用；内部请求走 Internal 通道）。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACBattleCommand
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Command")
    EACCommandType Type = EACCommandType::None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Command")
    FName CardId;

    /**
     * 秘术卡的**牌面效果**（阶段 3.3 新增）。
     *
     * 为什么命令里要带 GE 类：旧实现在内核侧拿 `CardId` 去效果块库里查块
     * （`EffectSystem.ExecuteBlock(Command.CardId)`），而效果块库已随 3.2 删除。
     * 命令是**跨帧存活的对外结构**，它要么带上"要施加什么"的类引用，要么这条通道就没了 ——
     * 保留 `CardId` 只是为了让日志 / UI / 去重有一个稳定名字（它不是内容索引）。
     *
     * 当前 Run 层还没有任何秘术卡内容（从来没有人往 `CardId` 里填过东西），
     * 因此这个数组实际恒为空、施加段是空操作 —— 与改造前"库里查不到块就什么都不做"等价。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Command")
    TArray<TSubclassOf<UGameplayEffect>> Effects;

    /**
     * 目标单位的句柄 = 该单位 Actor 的 `UnitId`（AACBattleUnitBase::GetUnitId()），
     * InvalidUnitId 表示"没有指定目标"。
     *
     * 为什么这里是 int32 而不是 §5.1 逐处口径表写的 `TWeakObjectPtr<AACBattleUnitBase>`：
     * **模块依赖方向**。本结构定义在 AutoChessCore，而 AACBattleUnitBase 属于 AutoChessBattle；
     * Core 不能依赖 Battle（依赖是单向的 Core <- Battle <- AutoChess），Core 里既拿不到该类型，
     * 也不该为了一个字段把 Battle 的类型拖进对外契约。这是本阶段对 §5.1 的一处**有意偏离**：
     * "命令队列跨帧存活、目标可能已销毁"这件事改由消费端处理 ——
     * `UBattleWorld::ConsumeCommands` 用 `UBattleWorld::ResolveOrWarn(FUnitId)` 解引用，
     * 目标已销毁时告警并丢弃该条命令（§5.1「必须处理的坑」第 1 条）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Command")
    int32 TargetUnitId = InvalidUnitId;

    /**
     * 命令提交时刻的**绝对时间**（秒，`FACBattleTime::Now`）。
     * 阶段 0.5：由 `int64 IssuedAtTick` 改成秒（§5.2）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Command")
    float IssuedAtTime = 0.f;
};

/**
 * 单位级战斗结果（战后血量持久化与统计）。
 *
 * D8（§5.1）落地口径：本结构是**对外契约结构**，指向单位的字段按裁决本应是弱指针，
 * 但本结构定义在 AutoChessCore，而 `AACBattleUnitBase` 属于 AutoChessBattle（依赖方向不可反转），
 * 因此这里**不引入任何弱指针字段**：`UnitId` 已经能唯一定位一个单位
 * （整数句柄单调递增、不复用，见 §5.1 的实现裁决），并能跨场对齐日志与结果。
 * 需要 Actor 时由调用方经 `UBattleWorld::FindUnit(UnitId)` / `Resolve(...)` 解引用。
 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACUnitBattleResult
{
    GENERATED_BODY()

    /** 单位句柄 = 该单位 Actor 的 UnitId；不复用，可跨场对齐（D8 之后仍是日志/比对的锚点）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    int32 UnitId = InvalidUnitId;

    /** 定义 ID：与 UnitId 一起构成稳定标识对（Run 层按它回写干员状态）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    FName DefinitionId;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    bool bDead = false;

    /** 剩余基础血量（口径见 TechDocs/99 A13）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    float RemainingBaseHP = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    float DamageDealt = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    float DamageTaken = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    float HealingDone = 0.f;
};

/** 战斗统计快照（委托埋点 + 批测）。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACBattleStatsSnapshot
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Stats")
    TMap<FName, int32> KillCountByTag;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Stats")
    TMap<int32, int32> KillsBySourceUnit;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Stats")
    int32 EnemyMentalBreakCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Stats")
    TMap<FName, int32> AbnormalStacksApplied;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Stats")
    TMap<int32, float> DamageDealtByUnit;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Stats")
    TMap<int32, float> DamageTakenByUnit;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Stats")
    TMap<int32, float> HealingDoneByUnit;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Stats")
    int32 SoulCrystalDelta = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Stats")
    int32 SearchableSecretCount = 0;

    /** 战斗内相对时长（秒）。阶段 0.5：由 `int64 BattleTicks` 改成秒（§5.2）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Stats")
    float BattleSeconds = 0.f;

    // 阶段 4（D3 / §5.3）：`int64 FinalStateHash` **已删除** —— 状态哈希是"确定性复现"的产物，
    // 随 `UBattleWorld::ComputeStateHash` 一起退场。回归与排障改由三份日志产物承担（§8.1）。

    /**
     * 逐单位对外结果（D8 §5.1：由数组承载，不再只靠 6 张统计表反推）。
     *
     * 6 张 `TMap<int32, …>` 保持整型键不变（采集是热路径，每次伤害都写），
     * 本字段是"逐单位结果"的唯一权威副本：
     *   - **填充点**：`UBattleWorld::CollectUnitResults`（只收 Player + Operator，按 UnitId 升序）；
     *   - **消费点**：`FACBattleSession::BuildResult` 把它同时写进 `FACBattleResult::Stats` 与
     *     `FACBattleResult::Units`；`FACBattleLogWriter` 的 `_units.json` 也从这里取，
     *     因此"结果结构"与"磁盘产物"不会分叉。
     */
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    TArray<FACUnitBattleResult> UnitResults;
};

/** 日志元信息（路径 / 版本 / 哈希）。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACBattleLogMeta
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Log")
    FString LogPath;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Log")
    int32 DataVersion = 0;

    /** 战斗内相对时长（秒）。阶段 0.5：由 `int64 Ticks` 改成秒（§5.2）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Battle|Log")
    float BattleSeconds = 0.f;

    // 阶段 4（D3 / §5.3）：`FinalStateHash` / `RngSeed` / `RngDrawCount` 三个字段**已删除**。
    // 它们是"确定性复现 + 批测比对"的产物（M02 §9），C3 放弃确定性之后没有任何生产点，
    // 也没有任何消费点。三份日志产物（`.jsonl` / `_stats.json` / `_units.json`）原样保留 ——
    // 它们是 D3 之后**唯一**的回归手段（§5.3「必须补做」）。

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Log")
    int32 RecordCount = 0;
};

/** 战斗输出结果（唯一出口）。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACBattleResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    FGuid BattleId;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    EACOutcome Outcome = EACOutcome::None;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    TArray<FACUnitBattleResult> Units;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    int32 SoulCrystalDelta = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    int32 SearchableSecretCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    FACBattleStatsSnapshot Stats;

    UPROPERTY(BlueprintReadOnly, Category = "Battle|Result")
    FACBattleLogMeta Log;
};

/** 单位生成请求（召唤 / 复活 / 战斗准备共用；由 M02 消费）。 */
USTRUCT(BlueprintType)
struct AUTOCHESSCORE_API FACUnitSpawnRequest
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    EACUnitKind Kind = EACUnitKind::Summon;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    EACTeam Team = EACTeam::Player;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    FName DefinitionId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    int32 OwnerUnitId = InvalidUnitId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    int32 TierIndex = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    FACHexCoord PreferredCell;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    EACFacing Facing = EACFacing::Up;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    FACStatBlock BaseStats;

    /** true = 以 owner 当前属性按 InheritRatio 生成快照（A5）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    bool bInheritFromOwner = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    float InheritRatio = 1.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    float DurationSeconds = -1.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    bool bOccupyCell = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    bool bSelectable = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    bool bCountsAsKill = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    FGameplayTagContainer ExtraTags;

    /** 生成后立刻施加的战斗内 GE（召唤物的 `FACSummonSpec::OnSpawnEffects` 落到这里）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
    TArray<TSubclassOf<UGameplayEffect>> Effects;

    // 阶段 3.3：§2.2 要求本结构增加 `TSubclassOf<AACBattleUnitBase> UnitClass`，**本轮未加**。
    //
    // 为什么：`AACBattleUnitBase` 属于 `AutoChessBattle`，而本文件在 `AutoChessCore`
    // （依赖方向 Core ⇐ Battle ⇐ AutoChess，不可反转）。加这个字段需要先让 Core 依赖 Battle，
    // 那会同时违反 §5.1 的实现裁决与"Core 不引用 AutoChessBattle 类型"这条自检项。
    //
    // 替代方案（已在用，功能等价）：**`DefinitionId → Actor 类` 的映射表路线** ——
    // `UBattleWorld::ResolveUnitClass(EACTeam, EACUnitKind)` 按队伍与类别选
    // `AACOperatorActor` / `AACEnemyActor` / `AACSummonActor`。当前内容每个 (Team, Kind)
    // 组合只有一个类，因此这条 3 路 switch 与"逐 DefinitionId 配类"信息量相同。
    //
    // 将来真需要"每个 DefinitionId 一个 Actor 类"时的落点（已评估，属于 AutoChess 主模块侧）：
    //   ① `UBattleDataSubsystem` 增加 `RegisterUnitClass(FName DefinitionId, TSubclassOf<AACBattleUnitBase>)`
    //      （与既有的 `RegisterAbilitySet` 同构，同住 Battle 模块，可以合法引用单位基类）；
    //   ② `UACBattleContentLibrary::BuildAndRegister` 从 `FACContentOperatorEntry`
    //      （AutoChess 模块侧结构，那里可以合法引用 `AACBattleUnitBase`）读出类并注册；
    //   ③ `UBattleWorld::ResolveUnitClass` 改成"先按 `DefinitionId` 查表，查不到回落 team/kind switch"。
    //   本轮不实现：当前没有任何内容需要它，加一条没人填的通道只会多出第二份真相。
};
