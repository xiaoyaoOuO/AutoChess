#include "Run/ACRunSubsystem.h"
#include "Run/ACRunBattleAssembler.h"
#include "Run/ACRunConfig.h"
#include "Run/ACRunMap.h"
#include "Content/ACBattleContentLibrary.h"
#include "Content/ACOperatorLibrary.h"
#include "Flow/ACBattleSubsystem.h"
// 阶段 0.5：状态面板 / 结算日志要用"战斗内相对时间（秒）"。
// AutoChess 模块依赖 AutoChessBattle，因此可以直接 include 内核的唯一时间源头（§2.4）。
#include "Battle/ACBattleTime.h"

namespace
{
    constexpr int32 MaxLoggedEvents = 128;
}

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

    // 初始化顺序与头文件里的成员声明顺序保持一致（RunConfig → Squad → Economy → bAutoDemo），
    // 避免 MSVC 的 C5038（初始化顺序不一致）告警。bAutoDemo 依赖 RunConfig，因此放在最后。
    Squad.Initialize(RunConfig, GetOperatorLibraryView());
    Economy.Initialize(RunConfig, Content, GetOperatorLibraryView());

    bAutoDemo = RunConfig->bAutoDemoByDefault;

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
    // 因此"改一处内容"不会让另一处的随机结果整体偏移（调试与批测时非常关键）。
    RunSeed = Seed != 0 ? Seed : static_cast<int32>(FDateTime::UtcNow().GetTicks() & 0x7FFFFFFF);
    EconomyStream.Initialize(static_cast<int32>(HashCombine(static_cast<uint32>(RunSeed), 0x1u)));
    RewardStream.Initialize(static_cast<int32>(HashCombine(static_cast<uint32>(RunSeed), 0x2u)));

    // ---- 地图 ----
    Map.Generate(RunSeed, *RunConfig);

    // ---- 经济 / 编队 / 库存 ----
    Economy.Reset(RunConfig->StartingSoulCrystal);
    Squad.Initialize(RunConfig, GetOperatorLibraryView());
    EquipmentInventory.Reset();
    PendingBattleOperatorIds.Reset();
    ResolvedNodeIds.Reset();

    RowsCleared = 0;
    ElapsedRunSeconds = 0.f;
    AutoDemoTimer = 0.f;
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
            Log(FString::Printf(TEXT("初始招募：%s"), *StarterId.ToString()));
        }
    }

    // ---- 第一跳：地图选路 ----
    Map.RefreshVisibility(*RunConfig);
    EnterPhase(EACRunPhase::Map);

    Log(FString::Printf(TEXT("开局：Seed=%d，魂晶=%d，%s"),
                        RunSeed, Economy.GetSoulCrystal(), *Map.ToDebugString()));
}

// ---------------------------------------------------------------------------
// 推进
// ---------------------------------------------------------------------------

bool UACRunSubsystem::SelectNode(int32 NodeId)
{
    if (Phase != EACRunPhase::Map)
    {
        Log(TEXT("当前不在选路阶段，忽略选点。"));
        return false;
    }
    if (!Map.MoveTo(NodeId))
    {
        Log(FString::Printf(TEXT("节点 #%d 不可达，忽略。"), NodeId));
        return false;
    }

    Map.RefreshVisibility(*RunConfig);
    EnterPhase(EACRunPhase::NodeResolving);

    const FACRunMapNode* Node = Map.GetCurrentNode();
    Log(FString::Printf(TEXT("进入节点 #%d（第 %d 行 · %s）"),
                        NodeId, Node != nullptr ? Node->Row : -1,
                        Node != nullptr ? *ACRunNodeType::ToDisplayText(Node->Type).ToString() : TEXT("?")));
    return true;
}

bool UACRunSubsystem::SelectFirstAvailableNode()
{
    TArray<int32> Candidates;
    Map.GetSelectableNodes(Candidates);
    if (Candidates.Num() == 0)
    {
        Log(TEXT("没有可选节点（可能已到终点）。"));
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
        Log(TEXT("当前没有节点可结算。"));
        return false;
    }

    // 幂等保护：当前节点已经结算过就不再重复（否则会重复开战/重复发奖）。
    if (ResolvedNodeIds.Contains(Node->NodeId))
    {
        Log(FString::Printf(TEXT("节点 #%d 已结算过，直接回到选路。"), Node->NodeId));
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
        Log(TEXT("起点：无事件，继续上路。"));
        EnterPhase(EACRunPhase::Map);
        return true;

    // ---------------- 商人：只开集市（招募仍只在战后出现）----------------
    case EACRunNodeType::Merchant:
    {
        Economy.RollMarketPool(Node->Row, EconomyStream);
        Economy.RollFragmentOffer(Node->Row, EconomyStream);
        Log(TEXT("商人：可以买卖装备与锻体。"));
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
        Log(FString::Printf(TEXT("篝火：全队回复 %g%% 生命（%d 人），魂晶 +%d。"),
                            RunConfig->CampfireHealPercent, Healed, RunConfig->CampfireSoulCrystal));
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
            Log(FString::Printf(TEXT("遗物：获得 %s。"), *Loot.DisplayName.ToString()));
        }
        else
        {
            Log(TEXT("遗物：内容库没有可掉落的装备（请检查 BuildEquipmentPools）。"));
        }
        EnterShopPhase(EACRunNodeType::Relic);
        return true;
    }

    // ---------------- 中转站：收束点，相当于一次免费休整 ----------------
    case EACRunNodeType::Waypoint:
    {
        Economy.AddSoulCrystal(RunConfig->CampfireSoulCrystal);
        Log(TEXT("中转站：短暂休整。"));
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
        Log(TEXT("无法开战：战斗子系统或内容库不可用。"));
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
        Log(TEXT("无法开战：没有可参战干员。"));
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
        Log(TEXT("开战失败：战斗子系统拒绝了本次启动。"));
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
    // 阶段 4（D3）：日志里不再打印 `Setup.RngSeed` —— 战斗内核没有随机流了（C3）。
    // 保留"节点 / 双方人数"这两项，它们才是这一条日志里可对照的信息。
    Log(FString::Printf(TEXT("开战：%s（我方 %d vs 敌方 %d）"),
                        *Encounter.DisplayName.ToString(),
                        Setup.PlayerUnits.Num(), Setup.EnemyUnits.Num()));
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

    // 阶段 4（D3）：日志里不再打印 `Consumed.Log.FinalStateHash` —— 状态哈希已随
    // "放弃确定性"退场（C3 / §5.3），回归与排障改由三份日志产物承担。
    Log(FString::Printf(TEXT("战斗结束：%s（%.1f 秒）"),
                        Consumed.Outcome == EACOutcome::Victory ? TEXT("胜利") :
                        Consumed.Outcome == EACOutcome::Defeat ? TEXT("失败") :
                        Consumed.Outcome == EACOutcome::Abandoned ? TEXT("已放弃") :
                        Consumed.Outcome == EACOutcome::Timeout ? TEXT("超时") : TEXT("异常"),
                        Consumed.Log.BattleSeconds));

    if (LastBattleOutcome.SoulCrystalGained != 0)
    {
        Log(FString::Printf(TEXT("魂晶 +%d（战斗内 %d + 节点 %d）"),
                            LastBattleOutcome.SoulCrystalGained,
                            LastBattleOutcome.SoulCrystalFromBattle,
                            LastBattleOutcome.SoulCrystalFromNode));
    }
    for (const FACRunEquipment& Loot : LastBattleOutcome.Loot)
    {
        EquipmentInventory.Add(Loot);
        Log(FString::Printf(TEXT("掉落：%s"), *Loot.DisplayName.ToString()));
    }
    if (LastBattleOutcome.DeadOperatorIds.Num() > 0)
    {
        Log(FString::Printf(TEXT("阵亡：%d 人（可在商店复活）"), LastBattleOutcome.DeadOperatorIds.Num()));
    }

    FinishEncounter(LastBattleOutcome.bVictory);
}

void UACRunSubsystem::FinishEncounter(bool bVictory)
{
    // ---- 失败：全队阵亡 → 本局结束 ----
    if (LastBattleOutcome.bSquadWiped)
    {
        Log(TEXT("全队阵亡，本局失败。"));
        SettleRun(/*bCleared=*/false);
        return;
    }

    // ---- 胜利且清的是 BOSS：通关 ----
    const FACRunMapNode* Node = Map.GetCurrentNode();
    if (bVictory && Node != nullptr && Node->Type == EACRunNodeType::Boss)
    {
        RowsCleared = Node->Row;
        Log(TEXT("击败首领，本局通关。"));
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
    AutoDemoTimer = 0.f;

    EnterPhase(EACRunPhase::Shop);
    Log(FString::Printf(TEXT("进入战后商店（来源节点：%s）。"), *ACRunNodeType::ToDisplayText(NodeType).ToString()));
}

// ---------------------------------------------------------------------------
// 商店操作
// ---------------------------------------------------------------------------

bool UACRunSubsystem::BuyRecruitSlot(int32 SlotIndex)
{
    FName OperatorId = NAME_None;
    int32 Tier = 0;
    FACPurchaseResult Result;
    if (!Economy.PurchaseRecruitSlot(SlotIndex, OperatorId, Tier, Result))
    {
        Log(FString::Printf(TEXT("招募失败：%s"), *Result.FailReason.ToString()));
        return false;
    }

    const FACRunRecruitOutcome RecruitOutcome = Squad.RecruitOperator(OperatorId);
    if (!RecruitOutcome.bSuccess)
    {
        // 编队满：把钱退回去（避免"付了钱没拿到人"）。
        Economy.AddSoulCrystal(Result.Cost);
        Log(TEXT("招募失败：编队已满。"));
        return false;
    }

    Log(FString::Printf(TEXT("招募 %s（Lv%d%s），花费 %d 魂晶。"),
                        *OperatorId.ToString(), RecruitOutcome.NewLevel,
                        RecruitOutcome.bLeveledUp ? TEXT("，升级") : TEXT("，新入队"),
                        Result.Cost));
    return true;
}

bool UACRunSubsystem::BuyMarketSlot(int32 SlotIndex)
{
    FACRunEquipment Equipment;
    FACPurchaseResult Result;
    if (!Economy.PurchaseMarketSlot(SlotIndex, Equipment, Result))
    {
        Log(FString::Printf(TEXT("购买失败：%s"), *Result.FailReason.ToString()));
        return false;
    }

    EquipmentInventory.Add(Equipment);
    Log(FString::Printf(TEXT("购买 %s，花费 %d 魂晶。"), *Equipment.DisplayName.ToString(), Result.Cost));

    // 自动装备：优先给空槽位最多（= 装备最少）的存活上场干员。
    FName BestOperator = NAME_None;
    int32 BestUsed = MAX_int32;
    for (const FName& OperatorId : Squad.GetActiveIds())
    {
        const FACRunOperator* Operator = Squad.Find(OperatorId);
        if (Operator == nullptr || Operator->bDead)
        {
            continue;
        }
        const int32 Used = Operator->EquippedIds.Num();
        if (Used < BestUsed && Used < Squad.GetEquipSlotMax(OperatorId))
        {
            BestUsed = Used;
            BestOperator = OperatorId;
        }
    }

    if (!BestOperator.IsNone())
    {
        const int32 InventoryIndex = EquipmentInventory.Num() - 1;
        if (Squad.EquipItem(BestOperator, Equipment))
        {
            EquipmentInventory[InventoryIndex].EquippedToOperatorId = BestOperator;
            Log(FString::Printf(TEXT("  → 已装备给 %s。"), *BestOperator.ToString()));
        }
    }
    return true;
}

bool UACRunSubsystem::RefreshRecruit()
{
    const FACRunMapNode* Node = Map.GetCurrentNode();
    const int32 Row = Node != nullptr ? Node->Row : 0;

    FACPurchaseResult Result;
    if (!Economy.RefreshRecruit(Row, EconomyStream, Result))
    {
        Log(FString::Printf(TEXT("刷新招募失败：%s"), *Result.FailReason.ToString()));
        return false;
    }
    Log(FString::Printf(TEXT("刷新招募，花费 %d 魂晶。"), Result.Cost));
    return true;
}

bool UACRunSubsystem::RefreshMarket()
{
    const FACRunMapNode* Node = Map.GetCurrentNode();
    const int32 Row = Node != nullptr ? Node->Row : 0;

    FACPurchaseResult Result;
    if (!Economy.RefreshMarket(Row, EconomyStream, Result))
    {
        Log(FString::Printf(TEXT("刷新集市失败：%s"), *Result.FailReason.ToString()));
        return false;
    }
    Log(FString::Printf(TEXT("刷新集市，花费 %d 魂晶。"), Result.Cost));
    return true;
}

bool UACRunSubsystem::BuyFragmentAndApply(int32 OptionIndex, int32 SquadIndex)
{
    const FACRunShopState& Shop = Economy.GetShop();
    if (!Shop.FragmentOffer.Options.IsValidIndex(OptionIndex))
    {
        Log(TEXT("锻体失败：碎片序号无效。"));
        return false;
    }

    FACPurchaseResult Result;
    if (!Economy.PurchaseFragmentOffer(Result))
    {
        Log(FString::Printf(TEXT("锻体失败：%s"), *Result.FailReason.ToString()));
        return false;
    }

    // 施加目标：按"存活上场干员"的顺序取第 SquadIndex 个。
    TArray<FName> Targets;
    for (const FName& OperatorId : Squad.GetActiveIds())
    {
        const FACRunOperator* Operator = Squad.Find(OperatorId);
        if (Operator != nullptr && !Operator->bDead)
        {
            Targets.Add(OperatorId);
        }
    }
    if (Targets.Num() == 0)
    {
        Economy.AddSoulCrystal(Result.Cost);      // 没有可施加对象，退款
        Log(TEXT("锻体失败：没有可强化的干员。"));
        return false;
    }

    const FName TargetId = Targets[FMath::Clamp(SquadIndex, 0, Targets.Num() - 1)];
    const FACRunFragment& Fragment = Shop.FragmentOffer.Options[OptionIndex];
    Squad.ApplyFragment(TargetId, Fragment);

    Log(FString::Printf(TEXT("锻体：%s 获得 %s，花费 %d 魂晶。"),
                        *TargetId.ToString(), *Fragment.DisplayName.ToString(), Result.Cost));
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
        Log(FString::Printf(TEXT("复活失败：需要 %d 魂晶。"), Cost));
        return false;
    }

    if (!Squad.ReviveOperator(OperatorId, RunConfig->ReviveStatPenaltyPercent))
    {
        Economy.AddSoulCrystal(Cost);
        return false;
    }

    Log(FString::Printf(TEXT("复活 %s，花费 %d 魂晶（永久属性 -%g%%）。"),
                        *OperatorId.ToString(), Cost, RunConfig->ReviveStatPenaltyPercent));
    return true;
}

int32 UACRunSubsystem::ReviveAllAffordable()
{
    int32 Revived = 0;
    const int32 Cost = Economy.GetReviveCost(Map.GetCurrentRow());

    TArray<FName> DeadIds;
    Squad.GetDeadOperatorIds(DeadIds);
    for (const FName& OperatorId : DeadIds)
    {
        if (Economy.GetSoulCrystal() < Cost)
        {
            break;      // 魂晶不够就停下（不做部分扣款）
        }
        if (ReviveOperator(OperatorId))
        {
            ++Revived;
        }
    }
    return Revived;
}

// ---------------------------------------------------------------------------
// 每帧驱动
// ---------------------------------------------------------------------------

void UACRunSubsystem::Tick(float RealDeltaSeconds)
{
    // 为什么不夹取 DeltaTime（原实现在这里做 `Clamp(RealDeltaSeconds, 0, MaxFrameDelta)`）：
    // 战斗内核的时间源是引擎的 `UWorld::GetTimeSeconds()`（GAS 重构实施方案 §5.2 的"实现裁决"），
    // 而 GAS 的 GE 时长也读同一个值。若在这里把帧长截短，逻辑推进的秒数就会小于世界时钟走过的秒数 ——
    // "中毒还剩 2 秒"会变成永远走不完（因为判定用的是世界时钟，而它跑得比逻辑快），
    // 这正是 D2 要消除的两个时钟漂移问题。
    // 因此这里改为原样透传；卡顿时确实会让一次逻辑步变长，但"时间真的过去了"本就是 World tick 的语义。
    const float Delta = FMath::Max(0.f, RealDeltaSeconds);
    ElapsedRunSeconds += Delta;

    // 1) 驱动战斗：Run 层是战斗内核的驱动方（M01 约定由调用方 Tick）。
    if (UBattleSubsystem* Battle = GetBattleSubsystem())
    {
        Battle->Tick(Delta);
    }

    // 2) 自动演示节拍。
    if (bAutoDemo)
    {
        TickAutoDemo(Delta);
    }
}

void UACRunSubsystem::TickAutoDemo(float RealDeltaSeconds)
{
    AutoDemoTimer += RealDeltaSeconds;
    const float Interval = RunConfig != nullptr
        ? FMath::Max(0.05f, RunConfig->AutoDemoStepIntervalSeconds)
        : 0.6f;
    if (AutoDemoTimer < Interval)
    {
        return;
    }
    AutoDemoTimer = 0.f;

    switch (Phase)
    {
    case EACRunPhase::Battle:
        // 战斗中：什么都不做（由 Tick 里的 Battle->Tick 推进）。**不要**返回失败计数，
        // 否则长战斗会被误判为"卡住"。
        return;

    case EACRunPhase::Map:
    case EACRunPhase::NodeResolving:
    case EACRunPhase::Shop:
        if (!AdvanceOneStep())
        {
            // 防死循环兜底：连续两拍都推不动，说明流程卡住了（例如地图异常）。
            // 直接结算，宁可结束这一局也不要让 Tick 永远空转刷日志。
            UE_LOG(LogTemp, Warning, TEXT("[Run] 自动演示连续无法推进，强制结算本局。"));
            Log(TEXT("自动演示无法继续推进，结算本局。"));
            SettleRun(/*bCleared=*/false);
        }
        return;

    case EACRunPhase::Settled:
        // 已结算：自动演示重新开一局，便于长时间挂着观察稳定性。
        StartRun(0);
        return;

    case EACRunPhase::Idle:
    default:
        StartRun(0);
        return;
    }
}

bool UACRunSubsystem::AdvanceOneStep()
{
    switch (Phase)
    {
    case EACRunPhase::Idle:
    case EACRunPhase::Settled:
        StartRun(0);
        return true;

    case EACRunPhase::Map:
        return SelectFirstAvailableNode();

    case EACRunPhase::NodeResolving:
        return ResolveCurrentNode();

    case EACRunPhase::Shop:
        AutoDemoShopStep();
        return true;

    case EACRunPhase::Battle:
    default:
        return false;
    }
}

void UACRunSubsystem::MarkCurrentNodeResolved()
{
    if (const FACRunMapNode* Node = Map.GetCurrentNode())
    {
        ResolvedNodeIds.AddUnique(Node->NodeId);
    }
}

void UACRunSubsystem::AutoDemoShopStep()
{
    // 这是"最小 AI 玩家"：规则很笨，但覆盖了每一个商店入口，用来证明系统之间是通的。
    // 正式版本的战后 UI 会把这些决定交给玩家。
    const FACRunShopState& Shop = Economy.GetShop();

    // ① 有富余就买招募（新干员 / 升级）。
    for (int32 SlotIndex = 0; SlotIndex < Shop.RecruitSlots.Num(); ++SlotIndex)
    {
        const FACRecruitSlot& Slot = Shop.RecruitSlots[SlotIndex];
        if (Slot.State != EACShopSlotState::Available && Slot.State != EACShopSlotState::Locked)
        {
            continue;
        }
        if (Economy.GetSoulCrystal() >= Slot.Price && Squad.HasRoomForNewOperator())
        {
            BuyRecruitSlot(SlotIndex);
            return;
        }
    }

    // ② 复活：只要有人阵亡且付得起，优先复活（避免下一场没人可上）。
    if (LastBattleOutcome.DeadOperatorIds.Num() > 0 && ReviveAllAffordable() > 0)
    {
        return;
    }

    // ③ 装备：魂晶富余（≥ 招募价的一半）时买一件。
    for (int32 SlotIndex = 0; SlotIndex < Shop.MarketSlots.Num(); ++SlotIndex)
    {
        const FACMarketSlot& Slot = Shop.MarketSlots[SlotIndex];
        if (Slot.State != EACShopSlotState::Available)
        {
            continue;
        }
        if (Economy.GetSoulCrystal() >= Slot.Price + 10)
        {
            BuyMarketSlot(SlotIndex);
            return;
        }
    }

    // ④ 锻体：还有余钱就强化。
    if (Shop.FragmentOffer.Options.Num() > 0
        && !Shop.FragmentOffer.bPurchased
        && Economy.GetSoulCrystal() >= Shop.FragmentOffer.Price + 10)
    {
        BuyFragmentAndApply(0, 0);
        return;
    }

    // ⑤ 无事可做 → 上路。
    LeaveShop();
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

    Log(FString::Printf(TEXT("本局结算：%s，通过 %d 行，剩余魂晶 %d → 法托 %d，阵亡 %d 人。"),
                        bCleared ? TEXT("通关") : TEXT("失败"),
                        Settlement.RowsCleared,
                        Settlement.SoulCrystalRemaining,
                        Settlement.FatoGained,
                        Settlement.DeadOperatorIds.Num()));

    EnterPhase(EACRunPhase::Settled);
    return Settlement;
}

// ---------------------------------------------------------------------------
// 日志与文本
// ---------------------------------------------------------------------------

void UACRunSubsystem::EnterPhase(EACRunPhase NextPhase)
{
    Phase = NextPhase;
}

void UACRunSubsystem::Log(const FString& Message)
{
    FACRunEventLogEntry Entry;
    Entry.TimeSeconds = ElapsedRunSeconds;
    Entry.Message = Message;
    EventLog.Add(Entry);

    if (EventLog.Num() > MaxLoggedEvents)
    {
        EventLog.RemoveAt(0, EventLog.Num() - MaxLoggedEvents);
    }

    UE_LOG(LogTemp, Log, TEXT("[Run] %s"), *Message);
}

namespace
{
    const TCHAR* PhaseToText(EACRunPhase Phase)
    {
        switch (Phase)
        {
        case EACRunPhase::Idle:          return TEXT("未开局");
        case EACRunPhase::Map:           return TEXT("地图选路");
        case EACRunPhase::NodeResolving: return TEXT("节点结算");
        case EACRunPhase::Shop:          return TEXT("战后商店");
        case EACRunPhase::Battle:        return TEXT("战斗中");
        case EACRunPhase::Settled:       return TEXT("已结算");
        default:                         return TEXT("?");
        }
    }
}

FString UACRunSubsystem::BuildStatusText() const
{
    TArray<FString> Lines;

    Lines.Add(FString::Printf(TEXT("=== Run 层状态 · %s%s ==="),
                              PhaseToText(Phase),
                              bAutoDemo ? TEXT("（自动演示）") : TEXT("")));
    Lines.Add(FString::Printf(TEXT("Seed %d · 已通过 %d 行 · %s"), RunSeed, RowsCleared, *Economy.ToDebugString()));
    Lines.Add(Map.ToDebugString());
    Lines.Add(Squad.ToDebugString());

    // 干员明细：等级 / 血量 / 装备 / 强化数量 —— 这些正是 Run 层持有、战斗内核看不到的东西。
    for (const FName& OperatorId : Squad.GetActiveIds())
    {
        const FACRunOperator* Operator = Squad.Find(OperatorId);
        if (Operator == nullptr)
        {
            continue;
        }
        const float MaxHP = Squad.GetDerivedMaxHP(OperatorId);
        const float CurrentHP = Operator->CurrentBaseHP < 0.f ? MaxHP : Operator->CurrentBaseHP;
        Lines.Add(FString::Printf(TEXT("  · %s  Lv%d(%s)  HP %.0f/%.0f  装备%d  强化%d%s"),
                                  *Operator->DisplayName.ToString(),
                                  Operator->Level,
                                  *FACRunLevelName::ToDisplayText(Operator->Level).ToString(),
                                  CurrentHP, MaxHP,
                                  Operator->EquippedIds.Num(),
                                  Operator->UpgradeEffects.Num() + Operator->TraitEffects.Num(),
                                  Operator->bDead ? TEXT("  [阵亡]") : TEXT("")));
    }

    // 战斗中的额外信息：让状态面板能直接看出"战斗内核在跑"。
    if (Phase == EACRunPhase::Battle)
    {
        if (UBattleSubsystem* Battle = GetBattleSubsystem())
        {
            if (const UBattleSession* Session = Battle->GetSession())
            {
                if (const UBattleWorld* World = Session->GetBattleWorld())
                {
                    // 阶段 0.5（D2）：时间口径改秒 —— 不再有 tick 计数可显示，
                    // 只显示战斗内相对时长（FACBattleTime::ElapsedSeconds）。
                    Lines.Add(FString::Printf(TEXT("  战斗：%.1fs · 我方存活 %d · 敌方存活 %d"),
                                              FACBattleTime::ElapsedSeconds(*World),
                                              World->GetAliveCount(EACTeam::Player),
                                              World->GetAliveCount(EACTeam::Enemy)));
                }
            }
        }
    }

    if (Phase == EACRunPhase::Shop)
    {
        const FACRunShopState& Shop = Economy.GetShop();
        Lines.Add(TEXT("--- 招募（3 选 1，可锁定 1 个）---"));
        for (int32 Index = 0; Index < Shop.RecruitSlots.Num(); ++Index)
        {
            const FACRecruitSlot& Slot = Shop.RecruitSlots[Index];
            Lines.Add(FString::Printf(TEXT("  [%d] %s  %s  品级%d  价格%d%s"),
                                      Index,
                                      *Slot.DisplayName.ToString(),
                                      *Slot.OperatorId.ToString(),
                                      Slot.Tier, Slot.Price,
                                      Slot.State == EACShopSlotState::Locked ? TEXT("（锁定）")
                                          : Slot.State == EACShopSlotState::Available ? TEXT("") : TEXT("（已售）")));
        }
        Lines.Add(FString::Printf(TEXT("  刷新费 %d（已刷新 %d 次）"), Economy.GetNextRecruitRefreshCost(), Shop.RecruitRefreshCount));

        Lines.Add(TEXT("--- 集市 ---"));
        for (int32 Index = 0; Index < Shop.MarketSlots.Num(); ++Index)
        {
            const FACMarketSlot& Slot = Shop.MarketSlots[Index];
            Lines.Add(FString::Printf(TEXT("  [%d] %s  品级%d  价格%d%s"),
                                      Index, *Slot.DisplayName.ToString(), Slot.Tier, Slot.Price,
                                      Slot.State == EACShopSlotState::Available ? TEXT("") : TEXT("（已售）")));
        }
        Lines.Add(FString::Printf(TEXT("  刷新费 %d（已刷新 %d 次）"), Economy.GetNextMarketRefreshCost(), Shop.MarketRefreshCount));

        Lines.Add(FString::Printf(TEXT("--- 锻体（%d 魂晶，三选一）---"), Shop.FragmentOffer.Price));
        for (int32 Index = 0; Index < Shop.FragmentOffer.Options.Num(); ++Index)
        {
            Lines.Add(FString::Printf(TEXT("  [%d] %s"), Index, *Shop.FragmentOffer.Options[Index].DisplayName.ToString()));
        }
    }

    if (Phase == EACRunPhase::Settled)
    {
        Lines.Add(FString::Printf(TEXT("--- 结算：%s · 通过 %d 行 · 法托 +%d · 阵亡 %d 人 ---"),
                                  Settlement.bCleared ? TEXT("通关") : TEXT("失败"),
                                  Settlement.RowsCleared, Settlement.FatoGained,
                                  Settlement.DeadOperatorIds.Num()));
    }

    return FString::Join(Lines, TEXT("\n"));
}

FString UACRunSubsystem::BuildLogText(int32 MaxLines) const
{
    const int32 Count = FMath::Min(FMath::Max(0, MaxLines), EventLog.Num());
    TArray<FString> Lines;
    Lines.Reserve(Count);
    for (int32 Index = EventLog.Num() - Count; Index < EventLog.Num(); ++Index)
    {
        if (EventLog.IsValidIndex(Index))
        {
            Lines.Add(FString::Printf(TEXT("[%.1fs] %s"), EventLog[Index].TimeSeconds, *EventLog[Index].Message));
        }
    }
    return FString::Join(Lines, TEXT("\n"));
}
