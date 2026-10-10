// Run 层（局内层）数据契约。
//
// 定位（对应 doc/TechDocs/00_战斗系统架构总览.md §1.2 的 L3 编排与接入层）：
//   局外层 Meta → 本层（Run）→ 战斗内核（AutoChessBattle）。
//
// 本层只做三件事：①持有跨战斗的局内状态；②把局内状态**翻译**为 FACBattleSetup 启动战斗；
// ③消耗 FACBattleResult 做战后流转。**本层不含任何战斗规则**（伤害/索敌/状态全部在战斗内核里）。
//
// 本文件只有数据，没有逻辑：逻辑分别在 ACRunMap / ACRunSquad / ACRunEconomy /
// ACRunBattleAssembler / ACRunPostBattle / ACRunSubsystem 中，便于单独替换与单测。

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Templates/SubclassOf.h"
#include "Core/ACBattleTypes.h"
#include "Core/ACBattleSetup.h"
#include "Core/ACDataTypes.h"
#include "ACRunTypes.generated.h"

// 阶段 3.3（GAS 重构实施方案 §3.2 表「Run/ACRunTypes.h」一行）：
// 本文件里所有 `TArray<FName> xxxEffectBlockIds` **已改成类引用**
//（`TSubclassOf<UGameplayEffect>` = 战斗内 GE；`TSubclassOf<UGameplayAbility>` = 被动能力）。
// 本模块（AutoChess）的 Public 依赖里有 AutoChessBattle，而后者 Public 依赖 GameplayAbilities，
// 因此这里可以直接用这两个引擎类型（依赖方向 Core ⇐ Battle ⇐ AutoChess 没有被破坏）。
class UGameplayEffect;
class UGameplayAbility;

// ---------------------------------------------------------------------------
// 枚举
// ---------------------------------------------------------------------------

/** Run 阶段机。与战斗阶段机（EACPhase）是两套：Run ⟷ 地图 ⟷ 战斗 ⟷ 之间来回切换。 */
UENUM(BlueprintType)
enum class EACRunPhase : uint8
{
    Idle            UMETA(DisplayName = "未开局"),
    Map             UMETA(DisplayName = "地图选路"),
    Deploy          UMETA(DisplayName = "部署干员"),
    NodeResolving   UMETA(DisplayName = "节点结算"),
    Shop            UMETA(DisplayName = "战后商店"),
    Battle          UMETA(DisplayName = "战斗中"),
    Settled         UMETA(DisplayName = "已结算")
};

/** 节点类型。 */
UENUM(BlueprintType)
enum class EACRunNodeType : uint8
{
    Entrance    UMETA(DisplayName = "起点"),
    Combat      UMETA(DisplayName = "遇敌"),
    Elite       UMETA(DisplayName = "精英"),
    Challenge   UMETA(DisplayName = "挑战"),
    Merchant    UMETA(DisplayName = "商人"),
    Campfire    UMETA(DisplayName = "篝火"),
    Relic       UMETA(DisplayName = "遗物"),
    Waypoint    UMETA(DisplayName = "中转站"),
    Boss        UMETA(DisplayName = "首领")
};

/** 招募栏位状态。*/
UENUM(BlueprintType)
enum class EACShopSlotState : uint8
{
    Available   UMETA(DisplayName = "可购买"),
    Purchased   UMETA(DisplayName = "已购买"),
    Sold        UMETA(DisplayName = "已售出"),
    Locked      UMETA(DisplayName = "已锁定")
};

/** 干员成长档位 D→S（与 FACPlayerUnitSpec::Level 同一口径）。*/
UENUM(BlueprintType)
enum class EACOperatorLevel : uint8
{
    D = 0       UMETA(DisplayName = "D"),
    C = 1       UMETA(DisplayName = "C"),
    B = 2       UMETA(DisplayName = "B"),
    A = 3       UMETA(DisplayName = "A"),
    S = 4       UMETA(DisplayName = "S")
};

// ---------------------------------------------------------------------------
// 永久修饰 / 装备 / 干员
// ---------------------------------------------------------------------------

/**
 * 跨战斗保留的固定属性加成。
 * 与战斗内属性修饰的区别：本结构由 **Run 层**持有并叠加到基础属性上
 *（阶段 2 起战斗内核的自研修饰器 `FBattleStatModifier` 已删除，战斗内修饰改由 GAS GE 承担，
 *  §4.3；因此二者不再是"同一种东西的两层"，本结构的语义是纯粹的 Run 层合成）。
 * 战斗内核不知道它的存在（它只看到"这一场的基础属性"）。这就是"锻体/复活惩罚"的落地方式。
 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunStatModifier
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Stat")
    EACStat Stat = EACStat::ATK;

    /** Add = 固定值；MulPct = 百分比（按当前累计值乘算）。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Stat")
    EACModOp Op = EACModOp::Add;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Stat")
    float Value = 0.f;

    /** 来源标识，仅用于展示与去重（如 "Resurrect" 或 "Fragment_S_ATK"）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Stat")
    FName SourceId;
};

/**
 * 装备提供的时间轴条目（抢攻 / 后发）。
 * 与 `FACStartingTimelineSpec` 一一对应，由 `FACRunBattleAssembler` 在装配战斗时翻译过去。
 * 单独抽出来是为了让"装备数据"（Run 层）与"战斗输入契约"（Core 层）保持解耦。
 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunTimelineGrant
{
    GENERATED_BODY()

    /** 要施加的 GE 类（抢攻 = 自带时长的 GE；后发 = 到点时施加的 GE）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    TSubclassOf<UGameplayEffect> Effect;

    /** true = 抢攻（BattleStart 生效）；false = 后发（第 TriggerSeconds 秒触发）。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    bool bPreemptive = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    float TriggerSeconds = 0.f;
};

/** 持有中的装备。属性加成在 Run 层结算（改基础属性），战斗内效果走 GE / 被动能力。*/
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunEquipment
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    FName EquipmentId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    FText DisplayName;

    /** 0=白 1=绿 2=蓝 3=红（FACRunConfig::EquipmentTierPrices 的下标）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    int32 Tier = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    TArray<FACRunStatModifier> StatModifiers;

    /** 战斗内生效的**常驻** GE（由 FACPlayerUnitSpec::EquipmentEffects 传给内核）。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    TArray<TSubclassOf<UGameplayEffect>> Effects;

    /**
     * 装备带来的**被动能力**（阶段 3.3 新增；由 `FACPlayerUnitSpec::GrantedAbilities` 传给内核）。
     *
     * 为什么装备也需要这条通道：金·心流刃的常驻段是"**每次普攻后**回 2 点专注"——
     * 它是一个 `Hook.AfterAttack` 触发的被动（`UACPassive_Equip_FlowBlade_OnAttack`），
     * 不是一个"装上就立即生效"的属性修饰。若只给 `Effects`（开局施加一次 GE），
     * 这条内容会退化成"开局 +2 专注一次"（阶段 3.2a 的已知退化，本阶段恢复）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    TArray<TSubclassOf<UGameplayAbility>> GrantedAbilities;

    /** 抢攻 / 后发条目（由 FACBattleSetup::StartingTimeline 传给内核）。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    TArray<FACRunTimelineGrant> TimelineGrants;

    /** 已装备到的干员（NAME_None = 在库存中）。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Equipment")
    FName EquippedToOperatorId;
};

/** 锻体碎片（永久属性强化）。*/
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunFragment
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Fragment")
    FName FragmentId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Fragment")
    FText DisplayName;

    /** 0=白 1=绿 2=蓝 3=彩（对应《经济系统详细设计》§7.3 的概率表）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Fragment")
    int32 Tier = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Fragment")
    TArray<FACRunStatModifier> StatModifiers;
};

/**
 * 干员局内状态（Run 层持有的"棋子"）。
 * 注意：这不是战斗单位。战斗单位是 AACBattleUnitBase，每场战斗临时创建；本结构跨战斗存活。
 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunOperator
{
    GENERATED_BODY()

    /** 与 UUnitDefinitionBase::DefinitionId 对应；同一干员可多次招募（重复招募 = 升级）。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    FName OperatorId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    FText DisplayName;

    /** 0=D ~ 4=S（同时是 StatsByLevel 的下标）。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    int32 Level = 0;

    /** 个性强化的数值档位：0=C 1=B 2=A 3=S（成长后由 Level 推导，见 ACRunSquad::GetTierIndex）。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    int32 TierIndex = 0;

    /** C/S 级已选个性强化带来的战斗内**常驻** GE。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    TArray<TSubclassOf<UGameplayEffect>> UpgradeEffects;

    /**
     * C/S 级已选个性强化带来的**被动能力**（阶段 3.3 新增）。
     *
     * 为什么要与 `UpgradeEffects` 分开：带触发条件的内容（S 级"处刑"的
     * "累计击杀 ≥ 20 后一次性 +50 暴击 / +80 攻击力"）只有作为**被动能力**授予 ASC
     * 才能保住门槛与一次性语义；只施加 GE 会退化成"选了就立刻生效"（阶段 3.2a 的已知退化）。
     * 与 `TraitGrantedAbilities` 分开则是为了让"这条内容来自哪一类获取途径"在数据上可见
     *（两者最终都会汇总进 `FACPlayerUnitSpec::GrantedAbilities`，见 `FACRunSquad::CollectGrantedAbilities`）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    TArray<TSubclassOf<UGameplayAbility>> UpgradeGrantedAbilities;

    /** 通用词条（B/A 级）带来的战斗内常驻 GE。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    TArray<TSubclassOf<UGameplayEffect>> TraitEffects;

    /** 通用词条（B/A 级）带来的被动能力（例如 B 级"心流"的"每次击杀回 15 专注"）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    TArray<TSubclassOf<UGameplayAbility>> TraitGrantedAbilities;

    /** 已装备的装备 ID（长度不得超过装备槽上限）。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    TArray<FName> EquippedIds;

    /** 锻体/复活惩罚等永久修饰。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    TArray<FACRunStatModifier> PermanentModifiers;

    /** 上一场结束时的基础血量；< 0 = 满血开局（对应 FACPlayerUnitSpec::CurrentBaseHP 的口径）。*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    float CurrentBaseHP = -1.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    bool bDead = false;

    /** 备战席（不参与战斗；名字取自《自走棋系统结构说明》§6.4）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    int32 BenchIndex = -1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    FACHexCoord FormationCell;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Run|Operator")
    EACFacing Facing = EACFacing::Up;
};

// 注意：**编队本身不是 USTRUCT**。
// 编队是带逻辑的对象（招募/升级/装备/复活/派生属性），实现在 `Run/ACRunSquad.h` 的
// `FACRunSquad` 类里；本文件只定义"干员"这一条不可变数据记录（FACRunOperator）。
// 早期版本这里曾放一个同名的 USTRUCT，导致与 ACRunSquad.h 的 class 前向声明冲突
// （MSVC C2011 / C4099 / C2027 连锁），已移除——一个名字只能有一个类型。

// ---------------------------------------------------------------------------
// 地图与节点
// ---------------------------------------------------------------------------

/** 地图节点。Row 的"向下推进"方向递增（0 = 起点所在行）。 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunMapNode
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Map")
    int32 NodeId = INDEX_NONE;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Map")
    int32 Row = 0;

    /** 行内序号，仅用于确定性排序与日志。*/
    UPROPERTY(BlueprintReadOnly, Category = "Run|Map")
    int32 SlotInRow = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Map")
    EACRunNodeType Type = EACRunNodeType::Combat;

    /** 该节点的后继节点（玩家在其中选一个）。*/
    UPROPERTY(BlueprintReadOnly, Category = "Run|Map")
    TArray<int32> NextNodeIds;

    /** 该节点的前驱节点。*/
    UPROPERTY(BlueprintReadOnly, Category = "Run|Map")
    TArray<int32> PrevNodeIds;

    /** 是否在视野内（《自走棋系统结构说明》§6.1：仅当前行与接下来 2 行可见）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Map")
    bool bVisible = false;

    /** 遭遇/事件的确定性种子（由 RunSeed + NodeId 派生，保证同种子同遭遇）。*/
    UPROPERTY(BlueprintReadOnly, Category = "Run|Map")
    int32 EncounterSeed = 0;
};

// ---------------------------------------------------------------------------
// 商店（招募 / 集市）
// ---------------------------------------------------------------------------

/** 招募栏位。*/
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRecruitSlot
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    FName OperatorId;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    FText DisplayName;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    int32 Tier = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    int32 Price = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    EACShopSlotState State = EACShopSlotState::Available;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    bool bOwned = false;
};

/** 集市栏位（装备）。*/
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACMarketSlot
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    FName EquipmentId;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    FText DisplayName;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    int32 Tier = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    int32 Price = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    EACShopSlotState State = EACShopSlotState::Available;
};

/** 锻体（属性碎片）三选一。*/
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACFragmentOffer
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    TArray<FACRunFragment> Options;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    int32 Price = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    bool bPurchased = false;
};

/** 战后商店的完整状态。*/
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunShopState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    TArray<FACRecruitSlot> RecruitSlots;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    TArray<FACMarketSlot> MarketSlots;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    FACFragmentOffer FragmentOffer;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    int32 RecruitRefreshCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    int32 MarketRefreshCount = 0;

    /** 被锁定的招募栏位下标（-1 = 未锁定；只能有 1 个）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Shop")
    int32 LockedRecruitSlot = -1;
};

// ---------------------------------------------------------------------------
// 结算
// ---------------------------------------------------------------------------

/** 单局结算结果（Run → 局外 Meta）。 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunSettlement
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Settle")
    bool bCleared = false;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Settle")
    int32 RowsCleared = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Settle")
    int32 SoulCrystalRemaining = 0;

    /** 剩余魂晶按上限与转化率折算出的法托（局外货币）。*/
    UPROPERTY(BlueprintReadOnly, Category = "Run|Settle")
    int32 FatoGained = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Settle")
    int32 BattlesWon = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Settle")
    int32 BattlesLost = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Settle")
    TArray<FName> DeadOperatorIds;
};

/** 战斗遭遇（节点 → 敌人编队）。 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunEncounter
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Encounter")
    FName EncounterId;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Encounter")
    FText DisplayName;

    /** 敌人定义 ID（同一份定义可放多个，占位由 ACRunBattleAssembler 决定）。*/
    UPROPERTY(BlueprintReadOnly, Category = "Run|Encounter")
    TArray<FName> EnemyDefinitionIds;

    /** 额外战斗内 GE（例如"本场敌人开局有 5 层静电紊乱"）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Run|Encounter")
    TArray<TSubclassOf<UGameplayEffect>> EnemyEffects;
};

/** 干员等级（1..4）的中文显示名：D/C/B/A/S。*/
namespace FACRunLevelName
{
    AUTOCHESS_API FText ToDisplayText(int32 Level);
}
