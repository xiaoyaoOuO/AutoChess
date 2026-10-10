#include "Run/ACRunSubsystem.h"
#include "Run/ACRunBattleAssembler.h"
#include "Run/ACRunConfig.h"
#include "Run/ACRunMap.h"
#include "Content/ACBattleContentLibrary.h"
#include "Content/ACOperatorLibrary.h"
#include "Flow/ACBattleSubsystem.h"
#include "Battle/ACBattleTime.h"

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void UACRunSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    RunConfig = NewObject<UACRunConfig>(this, TEXT("DefaultRunConfig"));

    // 内容要走在最前面：干员/敌人/效果块必须先注册进 UBattleDataSubsystem，
    // 否则 StartBattle 拿到的 FACBattleDataContext 是空的（单位会退化成兜底属性）。
    if (UACOperatorLibrary* OperatorLibrary = GetOperatorLibraryView())
    {
        OperatorLibrary->EnsureContentReady();
    }

    const UACBattleContentLibrary* Content = GetContentLibrary();

    // 初始化顺序与头文件里的成员声明顺序保持一致（RunConfig → Squad → Economy），
    Squad.Initialize(RunConfig, GetOperatorLibraryView());
    Economy.Initialize(RunConfig, Content, GetOperatorLibraryView());

    if (UBattleSubsystem* Battle = GetBattleSubsystem())
    {
        // 战斗结果走委托回调，而不是每帧轮询：与 M01 的"唯一结果出口"约定保持一致。
        Battle->OnBattleFinished.AddDynamic(this, &UACRunSubsystem::OnBattleFinished);
    }
}

void UACRunSubsystem::Deinitialize()
{
    if (UBattleSubsystem* Battle = GetBattleSubsystem())
    {
        Battle->OnBattleFinished.RemoveDynamic(this, &UACRunSubsystem::OnBattleFinished);
    }
    Super::Deinitialize();
}

UBattleSubsystem* UACRunSubsystem::GetBattleSubsystem() const
{
    UGameInstance* GameInstance = GetGameInstance();
    return GameInstance != nullptr ? GameInstance->GetSubsystem<UBattleSubsystem>() : nullptr;
}

UACBattleContentLibrary* UACRunSubsystem::GetContentLibrary() const
{
    UGameInstance* GameInstance = GetGameInstance();
    return GameInstance != nullptr ? GameInstance->GetSubsystem<UACBattleContentLibrary>() : nullptr;
}

UACOperatorLibrary* UACRunSubsystem::GetOperatorLibraryView() const
{
    UGameInstance* GameInstance = GetGameInstance();
    return GameInstance != nullptr ? GameInstance->GetSubsystem<UACOperatorLibrary>() : nullptr;
}

// ---------------------------------------------------------------------------
// 开新局
// ---------------------------------------------------------------------------

void UACRunSubsystem::StartRun(int32 Seed)
{
    // ---- 种子与派生随机流 ----
    // 三条流分开的用意：经济掷骰（招募/集市）、奖励掷骰（掉落）、地图生成互不干扰，
    RunSeed = Seed != 0 ? Seed : static_cast<int32>(FDateTime::UtcNow().GetTicks() & 0x7FFFFFFF);
    EconomyStream.Initialize(static_cast<int32>(HashCombine(static_cast<uint32>(RunSeed), 0x1u)));
    RewardStream.Initialize(static_cast<int32>(HashCombine(static_cast<uint32>(RunSeed), 0x2u)));

    //TODO:地图先不做，目前先把战斗跑通


    // ---- 经济 / 编队 / 库存 ----
    Economy.Reset(RunConfig->StartingSoulCrystal);
    Squad.Initialize(RunConfig, GetOperatorLibraryView());
    EquipmentInventory.Reset();
    PendingBattleOperatorIds.Reset();
    ResolvedNodeIds.Reset();

    RowsCleared = 0;
    LastBattleOutcome = FACRunBattleOutcome();
    Settlement = FACRunSettlement();
    EventLog.Reset();

    // ---- 起始编队：开局给 1 名示例干员，其余靠招募（模拟"局外初始招募"的最小版本）----
    if (UACOperatorLibrary* OperatorLibrary = GetOperatorLibraryView())
    {
        TArray<FName> Starters;
        OperatorLibrary->PickRandomOperators(EconomyStream, 1, Starters);
        for (const FName& StarterId : Starters)
        {
            Squad.RecruitOperator(StarterId);
        }
    }
    
    // 直接进入部署阶段，部署完毕开始战斗
    EnterPhase(EACRunPhase::Deploy);
}

// ---------------------------------------------------------------------------
// 推进
// ---------------------------------------------------------------------------

bool UACRunSubsystem::SelectNode(int32 NodeId)
{
    if (Phase != EACRunPhase::Map)
    {
        return false;
    }
    if (!Map.MoveTo(NodeId))
    {
        return false;
    }

    Map.RefreshVisibility(*RunConfig);
    EnterPhase(EACRunPhase::NodeResolving);

    const FACRunMapNode* Node = Map.GetCurrentNode();

    return true;
}

bool UACRunSubsystem::SelectFirstAvailableNode()
{
    TArray<int32> Candidates;
    Map.GetSelectableNodes(Candidates);
    if (Candidates.Num() == 0)
    {
        return false;
    }
    return SelectNode(Candidates[0]);
}

bool UACRunSubsystem::ResolveCurrentNode()
{
    if (Phase != EACRunPhase::NodeResolving)
    {
        return false;
    }

    const FACRunMapNode* Node = Map.GetCurrentNode();
    if (Node == nullptr)
    {
        return false;
    }

    // 幂等保护：当前节点已经结算过就不再重复（否则会重复开战/重复发奖）。
    if (ResolvedNodeIds.Contains(Node->NodeId))
    {
        EnterPhase(EACRunPhase::Map);
        return false;
    }
    MarkCurrentNodeResolved();

    switch (Node->Type)
    {
    // ---------------- 战斗类节点 ----------------
    case EACRunNodeType::Combat:
    case EACRunNodeType::Elite:
    case EACRunNodeType::Challenge:
    case EACRunNodeType::Boss:
        StartBattleForCurrentNode();
        return true;

    // ---------------- 起点：不做事，直接回到选路 ----------------
    case EACRunNodeType::Entrance:
        EnterPhase(EACRunPhase::Map);
        return true;

    // ---------------- 商人：只开集市（招募仍只在战后出现）----------------
    case EACRunNodeType::Merchant:
    {
        Economy.RollMarketPool(Node->Row, EconomyStream);
        Economy.RollFragmentOffer(Node->Row, EconomyStream);
        EnterShopPhase(EACRunNodeType::Merchant);
        return true;
    }

    // ---------------- 篝火：小队恢复 ----------------
    case EACRunNodeType::Campfire:
    {
        Economy.AddSoulCrystal(RunConfig->CampfireSoulCrystal);
        int32 Healed = 0;
        for (const FACRunOperator& Operator : Squad.GetAll())
        {
            if (!Operator.bDead && Squad.HealOperator(Operator.OperatorId, RunConfig->CampfireHealPercent))
            {
                ++Healed;
            }
        }

        EnterShopPhase(EACRunNodeType::Campfire);
        return true;
    }

    // ---------------- 遗物：白拿一件装备 ----------------
    case EACRunNodeType::Relic:
    {
        FACRunEquipment Loot;
        const int32 Tier = FMath::Max(0, RunConfig->RelicEquipmentMaxTier);
        if (Economy.RollEquipment(Tier, NAME_None, RewardStream, Loot))
        {
            EquipmentInventory.Add(Loot);
        }

        EnterShopPhase(EACRunNodeType::Relic);
        return true;
    }

    // ---------------- 中转站：收束点，相当于一次免费休整 ----------------
    case EACRunNodeType::Waypoint:
    {
        Economy.AddSoulCrystal(RunConfig->CampfireSoulCrystal);
        EnterShopPhase(EACRunNodeType::Waypoint);
        return true;
    }

    default:
        EnterPhase(EACRunPhase::Map);
        return true;
    }
}

void UACRunSubsystem::StartBattleForCurrentNode()
{
    UBattleSubsystem* Battle = GetBattleSubsystem();
    UACOperatorLibrary* OperatorLibrary = GetOperatorLibraryView();
    if (Battle == nullptr || OperatorLibrary == nullptr || RunConfig == nullptr)
    {
        return;
    }

    const FACRunMapNode* Node = Map.GetCurrentNode();
    if (Node == nullptr)
    {
        return;
    }

    // 开战前整理编成：阵亡者让位、备战席补位。
    Squad.PrepareForBattle();
    if (Squad.GetActiveIds().Num() == 0)
    {
        return;
    }

    // 节点 → 遭遇 → 战斗输入快照。
    const FACRunEncounter Encounter = FACRunBattleAssembler::GenerateEncounter(*Node);

    FACBattleSetup Setup;
    FACBattleLaunchOptions Options;
    // 阶段 4（D3）：`RunSeed` 形参已删除 —— 它只用于派生战斗内核的随机种子，
    // 而内核的随机流与 `FACBattleSetup::RngSeed` 字段都随 D3 退场（C3）。
    FACRunBattleAssembler::BuildBattleSetup(Encounter, *Node, Squad, *RunConfig, *OperatorLibrary,
                                            EquipmentInventory, Setup, Options);

    // 记录"参战顺序 → 干员 ID"：战后结果回流靠它按下标对齐（见 ACRunPostBattle）。
    PendingBattleOperatorIds.Reset();
    for (const FACPlayerUnitSpec& Spec : Setup.PlayerUnits)
    {
        PendingBattleOperatorIds.Add(Spec.DefinitionId);
    }

    if (!Battle->StartBattle(Setup, Options))
    {
        EnterPhase(EACRunPhase::Map);
        return;
    }

    // 注意顺序：StartBattle 在 Setup 非法时会立即产出 Error 结果并同步广播 OnBattleFinished，
    // 那时本函数还没返回、阶段还停在 NodeResolving（回调内部已把流程推进到商店/结算）。
    // 因此这里必须先判断"回调是否已经处理完这场战斗"，再决定要不要进入 Battle 阶段。
    if (Phase != EACRunPhase::NodeResolving)
    {
        return;
    }

    EnterPhase(EACRunPhase::Battle);
}

void UACRunSubsystem::AbandonBattle()
{
    if (Phase != EACRunPhase::Battle)
    {
        return;
    }
    if (UBattleSubsystem* Battle = GetBattleSubsystem())
    {
        Battle->AbandonBattle();
    }
}

void UACRunSubsystem::LeaveShop()
{
    if (Phase != EACRunPhase::Shop)
    {
        return;
    }
    EnterPhase(EACRunPhase::Map);
}

void UACRunSubsystem::OnBattleFinished(const FACBattleResult& Result)
{
    UBattleSubsystem* Battle = GetBattleSubsystem();
    if (Battle == nullptr)
    {
        return;
    }

    // 只处理"本层正在等待"的那一场：手动调用控制台命令、或外部系统自行开战都不会被误吃。
    if (Phase != EACRunPhase::Battle && Phase != EACRunPhase::NodeResolving)
    {
        return;
    }

    // M01 契约：结果一次性消费。取走结果会同时销毁 Session，下一场才能重新 StartBattle。
    FACBattleResult Consumed;
    if (!Battle->TryConsumeResult(Consumed))
    {
        return;
    }

    const FACRunMapNode* Node = Map.GetCurrentNode();
    if (Node == nullptr)
    {
        return;
    }

    UACBattleContentLibrary* Content = GetContentLibrary();
    if (Content == nullptr)
    {
        return;
    }

    LastBattleOutcome = FACRunPostBattle::ApplyResult(Consumed, *Node, Squad, Economy, *RunConfig,
                                                      *Content, RewardStream, PendingBattleOperatorIds);
    
    for (const FACRunEquipment& Loot : LastBattleOutcome.Loot)
    {
        EquipmentInventory.Add(Loot);
    }
    FinishEncounter(LastBattleOutcome.bVictory);
}

void UACRunSubsystem::FinishEncounter(bool bVictory)
{
    // ---- 失败：全队阵亡 → 本局结束 ----
    if (LastBattleOutcome.bSquadWiped)
    {
        SettleRun(/*bCleared=*/false);
        return;
    }

    // ---- 胜利且清的是 BOSS：通关 ----
    const FACRunMapNode* Node = Map.GetCurrentNode();
    if (bVictory && Node != nullptr && Node->Type == EACRunNodeType::Boss)
    {
        RowsCleared = Node->Row;
        SettleRun(/*bCleared=*/true);
        return;
    }

    if (bVictory && Node != nullptr)
    {
        RowsCleared = FMath::Max(RowsCleared, Node->Row);
    }

    EnterShopPhase(Node != nullptr ? Node->Type : EACRunNodeType::Combat);
}

void UACRunSubsystem::EnterShopPhase(EACRunNodeType NodeType)
{
    // 每次进入商店都重新生成货架（招募池保留锁定项；集市与锻体全刷新）。
    const FACRunMapNode* Node = Map.GetCurrentNode();
    const int32 Row = Node != nullptr ? Node->Row : 0;
    Economy.RollRecruitPool(Row, EconomyStream);
    Economy.RollMarketPool(Row, EconomyStream);
    Economy.RollFragmentOffer(Row, EconomyStream);

    EnterPhase(EACRunPhase::Shop);
}

// ---------------------------------------------------------------------------
// 商店操作
// ---------------------------------------------------------------------------

bool UACRunSubsystem::RefreshRecruit()
{
    const FACRunMapNode* Node = Map.GetCurrentNode();
    const int32 Row = Node != nullptr ? Node->Row : 0;

    FACPurchaseResult Result;
    if (!Economy.RefreshRecruit(Row, EconomyStream, Result))
    {
        return false;
    }
    return true;
}

bool UACRunSubsystem::RefreshMarket()
{
    const FACRunMapNode* Node = Map.GetCurrentNode();
    const int32 Row = Node != nullptr ? Node->Row : 0;

    FACPurchaseResult Result;
    if (!Economy.RefreshMarket(Row, EconomyStream, Result))
    {
        return false;
    }
    return true;
}

bool UACRunSubsystem::ReviveOperator(FName OperatorId)
{
    const FACRunOperator* Operator = Squad.Find(OperatorId);
    if (Operator == nullptr || !Operator->bDead)
    {
        return false;
    }

    const int32 Cost = Economy.GetReviveCost(Map.GetCurrentRow());
    if (!Economy.TrySpend(Cost))
    {
        return false;
    }

    if (!Squad.ReviveOperator(OperatorId, RunConfig->ReviveStatPenaltyPercent))
    {
        Economy.AddSoulCrystal(Cost);
        return false;
    }
    
    return true;
}

// ---------------------------------------------------------------------------
// 每帧驱动
// ---------------------------------------------------------------------------

void UACRunSubsystem::Tick(float RealDeltaSeconds)
{
    const float Delta = FMath::Max(0.f, RealDeltaSeconds);

    // 1) 驱动战斗：Run 层是战斗内核的驱动方。
    if (UBattleSubsystem* Battle = GetBattleSubsystem())
    {
        Battle->Tick(Delta);
    }
}

void UACRunSubsystem::MarkCurrentNodeResolved()
{
    if (const FACRunMapNode* Node = Map.GetCurrentNode())
    {
        ResolvedNodeIds.AddUnique(Node->NodeId);
    }
}

// ---------------------------------------------------------------------------
// 结算
// ---------------------------------------------------------------------------

FACRunSettlement UACRunSubsystem::SettleRun(bool bCleared)
{
    Settlement = FACRunSettlement();
    Settlement.bCleared = bCleared;
    Settlement.RowsCleared = RowsCleared;
    Settlement.SoulCrystalRemaining = Economy.GetSoulCrystal();

    // 剩余魂晶 → 法托（有上限）。【待定 E3】转化率与上限都是占位值。
    const int32 Convertible = FMath::Min(Settlement.SoulCrystalRemaining, RunConfig->SoulCrystalToFatoCap);
    Settlement.FatoGained = FMath::RoundToInt(Convertible * RunConfig->SoulCrystalToFatoRate);
    Economy.Reset(0);

    Squad.GetDeadOperatorIds(Settlement.DeadOperatorIds);

    EnterPhase(EACRunPhase::Settled);
    return Settlement;
}

void UACRunSubsystem::EnterPhase(EACRunPhase NextPhase)
{
    Phase = NextPhase;
    OnPhaseChanged(NextPhase);
}

void UACRunSubsystem::OnPhaseChanged(EACRunPhase NewPhase)
{
    //当Phase切换时，比如Deploy就显示六边形地图，此时玩家可以放置干员
}