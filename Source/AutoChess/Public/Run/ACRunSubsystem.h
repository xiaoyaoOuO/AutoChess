// Run 层主循环（局内层，对应 doc/TechDocs/00_战斗系统架构总览.md §2.4.4 的单场生命周期）。
//
// 它是 Run 层**唯一**的对外入口，也是"把各模块串起来"的那根线：
//
//   ┌─ 地图选路 ────────────────────────────────────────────────┐
//   │  FACRunMap::GetSelectableNodes / MoveTo                   │
//   ▼                                                            │
//   ┌─ 节点结算 ────────────────────────────────────────────────┤
//   │  战斗节点：FACRunBattleAssembler::BuildBattleSetup         │
//   │            → UBattleSubsystem::StartBattle                 │
//   │            → Tick（本类驱动）→ TryConsumeResult            │
//   │            → FACRunPostBattle::ApplyResult                 │
//   │  非战斗节点：直接产出魂晶 / 装备 / 治疗                     │
//   ▼                                                            │
//   ┌─ 战后商店 ────────────────────────────────────────────────┤
//   │  FACRunEconomy：招募（FACRunSquad::RecruitOperator）        │
//   │                集市（装备 → FACRunSquad::EquipItem）        │
//   │                锻体（碎片 → FACRunSquad::ApplyFragment）    │
//   │                复活（FACRunSquad::ReviveOperator）          │
//   └───────────────────────────────────────────────────────────►┘
//
// 三个"谁负责什么"的边界（读代码时最容易混的地方）：
//   - 本类只**编排**：不写数值、不判胜负、不选目标；
//   - 战斗内核（AutoChessBattle）只**模拟一场**：不知道地图、金币、招募；
//   - 内容库（AutoChess/Content）只**提供数据**：干员/敌人/装备/碎片/效果块。
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/ACBattleSetup.h"
#include "Core/ACBattleTypes.h"
#include "Run/ACRunEconomy.h"
#include "Run/ACRunMap.h"
#include "Run/ACRunPostBattle.h"
#include "Run/ACRunSquad.h"
#include "Run/ACRunTypes.h"
#include "ACRunSubsystem.generated.h"

class UACBattleContentLibrary;
class UACOperatorLibrary;
class UACRunConfig;
class UBattleSubsystem;
class FACRunSquad;      // 已在 ACRunSquad.h 中完整定义；此处保留前向声明以便阅读依赖方向

/** 调试时间轴：记录最近若干步，便于出问题时回看"发生了什么"。 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACRunEventLogEntry
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Log")
    float TimeSeconds = 0.f;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Log")
    FString Message;
};

UCLASS()
class AUTOCHESS_API UACRunSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // ---- 生命周期 ----
    /** 开一局（Seed = 0 时按当前时间派生）。 */
    UFUNCTION(BlueprintCallable, Category = "Run")
    void StartRun(int32 Seed = 0);

    /** 结束本局并结算（法托折算、秘藏带出等；局外持久化不在本层职责内）。 */
    UFUNCTION(BlueprintCallable, Category = "Run")
    FACRunSettlement SettleRun(bool bCleared);

    UFUNCTION(BlueprintPure, Category = "Run")
    EACRunPhase GetPhase() const { return Phase; }

    UFUNCTION(BlueprintPure, Category = "Run")
    int32 GetRunSeed() const { return RunSeed; }

    // ---- 核心推进（每一步都是显式 API，UI 或控制台命令可以直接调用）----

    /** 选择下一跳节点（必须在 GetSelectableNodes 的结果里）。 */
    UFUNCTION(BlueprintCallable, Category = "Run")
    bool SelectNode(int32 NodeId);

    /** 结算当前节点：战斗节点会**启动**一场战斗（随后由 Tick 推进）。 */
    UFUNCTION(BlueprintCallable, Category = "Run")
    bool ResolveCurrentNode();

    /** 放弃当前战斗（结果记为 Abandoned，本节点无奖励）。 */
    UFUNCTION(BlueprintCallable, Category = "Run")
    void AbandonBattle();

    /** 离开战后商店，回到地图选路。 */
    UFUNCTION(BlueprintCallable, Category = "Run")
    void LeaveShop();

    /** 只在 Map 阶段有意义：自动选中"第一个可选节点"，让控制台驱动更省事。 */
    UFUNCTION(BlueprintCallable, Category = "Run")
    bool SelectFirstAvailableNode();

    UFUNCTION(BlueprintCallable, Category = "Run|Shop")
    bool RefreshRecruit();

    UFUNCTION(BlueprintCallable, Category = "Run|Shop")
    bool RefreshMarket();

    /** 复活一名阵亡干员（扣魂晶 + 永久属性惩罚）。 */
    UFUNCTION(BlueprintCallable, Category = "Run|Shop")
    bool ReviveOperator(FName OperatorId);

    /** 由 GameMode 每帧调用：驱动战斗 + 自动演示节拍。 */
    void Tick(float RealDeltaSeconds);

    // ---- 只读访问（UI / HUD / 调试）----
    UFUNCTION(BlueprintPure, Category = "Run")
    UACRunConfig* GetRunConfig() const { return RunConfig; }

    const FACRunMap& GetMap() const { return Map; }
    const FACRunSquad& GetSquad() const { return Squad; }
    const FACRunEconomy& GetEconomy() const { return Economy; }
    const FACRunBattleOutcome& GetLastBattleOutcome() const { return LastBattleOutcome; }
    const FACRunSettlement& GetSettlement() const { return Settlement; }
    const TArray<FACRunEventLogEntry>& GetEventLog() const { return EventLog; }

private:
    // ---- 内部推进 ----
    void EnterPhase(EACRunPhase NextPhase);
    void OnPhaseChanged(EACRunPhase NewPhase);
    UFUNCTION()
    void OnBattleFinished(const FACBattleResult& Result);
    void EnterShopPhase(EACRunNodeType NodeType);
    void FinishEncounter(bool bVictory);
    void StartBattleForCurrentNode();

    /** 把当前节点标记为已结算。 */
    void MarkCurrentNodeResolved();

    UBattleSubsystem* GetBattleSubsystem() const;
    UACBattleContentLibrary* GetContentLibrary() const;
    UACOperatorLibrary* GetOperatorLibraryView() const;

    /** RunConfig：目前用 C++ 默认值构造；资产接入后从 UACBattleContentLibrary / DataAsset 取。 */
    UPROPERTY() TObjectPtr<UACRunConfig> RunConfig = nullptr;

    // ---- 局内状态 ----
    FACRunMap Map;
    FACRunSquad Squad;
    FACRunEconomy Economy;

    /** 本局参战顺序 → 干员 ID（与最近一次 FACBattleSetup::PlayerUnits 同序，用于结果回流）。 */
    TArray<FName> PendingBattleOperatorIds;

    EACRunPhase Phase = EACRunPhase::Idle;
    int32 RunSeed = 0;
    int32 RowsCleared = 0;
    FACRunBattleOutcome LastBattleOutcome;
    FACRunSettlement Settlement;

    /** 当前节点的装备库存（已购买/掉落的装备；装备到干员后仍保留在库存里）。 */
    TArray<FACRunEquipment> EquipmentInventory;

    /** 各构造性随机的独立随机流：互不干扰，保证"改一处不影响另一处的复现"。 */
    FRandomStream EconomyStream;
    FRandomStream RewardStream;

    TArray<FACRunEventLogEntry> EventLog;

    /** 已结算过的节点（防止"同一节点被结算两次"导致重复开战/重复发奖）。 */
    TArray<int32> ResolvedNodeIds;
};
