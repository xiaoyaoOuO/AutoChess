#include "Run/ACRunEconomy.h"
#include "Run/ACRunConfig.h"
#include "Content/ACBattleContentLibrary.h"
#include "Content/ACOperatorLibrary.h"

namespace
{
    /** 按权重抽取下标；返回 INDEX_NONE 表示数组为空。 */
    int32 PickWeightedIndex(FRandomStream& Stream, const TArray<float>& Weights)
    {
        float Total = 0.f;
        for (const float Weight : Weights)
        {
            Total += FMath::Max(0.f, Weight);
        }
        if (Weights.Num() == 0 || Total <= 0.f)
        {
            return INDEX_NONE;
        }

        const float Roll = Stream.FRandRange(0.f, Total);
        float Accumulated = 0.f;
        for (int32 Index = 0; Index < Weights.Num(); ++Index)
        {
            Accumulated += FMath::Max(0.f, Weights[Index]);
            if (Roll <= Accumulated)
            {
                return Index;
            }
        }
        return Weights.Num() - 1;
    }

    bool IsSlotAvailable(EACShopSlotState State)
    {
        return State == EACShopSlotState::Available;
    }
}

void FACRunEconomy::Initialize(const UACRunConfig* InConfig, const UACBattleContentLibrary* InContent,
                               const UACOperatorLibrary* InLibrary)
{
    Config = InConfig;
    Content = InContent;
    Library = InLibrary;
    SoulCrystal = 0;
    Shop = FACRunShopState();
    if (Config != nullptr)
    {
        Shop.FragmentOffer.Price = Config->GetFragmentPrice();
    }
}

void FACRunEconomy::Reset(int32 StartingSoulCrystal)
{
    SoulCrystal = FMath::Max(0, StartingSoulCrystal);
    Shop = FACRunShopState();
    if (Config != nullptr)
    {
        Shop.FragmentOffer.Price = Config->GetFragmentPrice();
    }
}

// ---------------------------------------------------------------------------
// 魂晶收支
// ---------------------------------------------------------------------------

void FACRunEconomy::AddSoulCrystal(int32 Delta)
{
    SoulCrystal = FMath::Max(0, SoulCrystal + Delta);
}

bool FACRunEconomy::TrySpend(int32 Cost)
{
    if (Cost <= 0)
    {
        return true;
    }
    if (SoulCrystal < Cost)
    {
        return false;
    }
    SoulCrystal -= Cost;
    return true;
}

// ---------------------------------------------------------------------------
// 价格（越界回退由 UACRunConfig 负责）
// ---------------------------------------------------------------------------

int32 FACRunEconomy::GetRecruitPrice(int32 Tier) const
{
    return Config != nullptr ? Config->GetOperatorRecruitPrice(Tier) : 15;
}

int32 FACRunEconomy::GetRecruitRecyclePrice(int32 Tier) const
{
    return Config != nullptr ? Config->GetOperatorRecyclePrice(Tier) : 9;
}

int32 FACRunEconomy::GetEquipmentPrice(int32 Tier) const
{
    return Config != nullptr ? Config->GetEquipmentPrice(Tier) : 5;
}

int32 FACRunEconomy::GetEquipmentRecyclePrice(int32 Tier) const
{
    return Config != nullptr ? Config->GetEquipmentRecyclePrice(Tier) : 3;
}

int32 FACRunEconomy::GetNextRecruitRefreshCost() const
{
    return Config != nullptr ? Config->GetRecruitRefreshCost(Shop.RecruitRefreshCount) : 3;
}

int32 FACRunEconomy::GetNextMarketRefreshCost() const
{
    return Config != nullptr ? Config->GetMarketRefreshCost(Shop.MarketRefreshCount) : 5;
}

int32 FACRunEconomy::GetFragmentPrice() const
{
    return Config != nullptr ? Config->GetFragmentPrice() : 12;
}

int32 FACRunEconomy::GetReviveCost(int32 Row) const
{
    return Config != nullptr ? Config->GetReviveCost(Row) : 3;
}

// ---------------------------------------------------------------------------
// 品级掷骰
// ---------------------------------------------------------------------------

int32 FACRunEconomy::RollTierIndex(const TArray<float>& Weights, int32 Row, FRandomStream& Stream) const
{
    if (Weights.Num() == 0)
    {
        return 0;
    }

    // "随行数推进整体上移"：每 LevelUpRowStep 行把权重数组左移一格（丢弃最低品级）。
    const int32 Step = Config != nullptr ? FMath::Max(1, Config->LevelUpRowStep) : 3;
    const int32 Shift = FMath::Clamp(Row / Step, 0, Weights.Num() - 1);

    TArray<float> Adjusted;
    Adjusted.SetNumZeroed(Weights.Num());
    for (int32 Index = Shift; Index < Weights.Num(); ++Index)
    {
        Adjusted[Index - Shift] = Weights[Index];
    }

    const int32 Picked = PickWeightedIndex(Stream, Adjusted);
    return Picked != INDEX_NONE ? Picked : 0;
}

// ---------------------------------------------------------------------------
// 招募
// ---------------------------------------------------------------------------

FName FACRunEconomy::PickOperatorId(int32 Row, FRandomStream& Stream) const
{
    if (Library == nullptr || Library->GetOperatorIds().Num() == 0)
    {
        return NAME_None;
    }

    const TArray<float>& Weights = Config != nullptr
        ? Config->OperatorTierWeights
        : TArray<float>();

    const int32 DesiredTier = RollTierIndex(Weights, Row, Stream);

    // 从期望品级开始向下回落，找到第一个有内容的品级（内容少时不至于抽空）。
    TArray<FName> Candidates;
    for (int32 Tier = DesiredTier; Tier >= 0; --Tier)
    {
        Library->GetOperatorIdsByTier(Tier, Candidates);
        if (Candidates.Num() > 0)
        {
            break;
        }
    }
    if (Candidates.Num() == 0)
    {
        // 全部品级都没内容（或品级都不匹配）：从全库里抽。
        Candidates = Library->GetOperatorIds();
    }
    if (Candidates.Num() == 0)
    {
        return NAME_None;
    }

    return Candidates[Stream.RandRange(0, Candidates.Num() - 1)];
}

void FACRunEconomy::RollRecruitPool(int32 Row, FRandomStream& Stream)
{
    const int32 SlotCount = Config != nullptr ? FMath::Max(1, Config->RecruitSlotCount) : 3;

    // 保留被锁定的栏位（只锁 1 个）。
    FACRecruitSlot LockedSlot;
    bool bHasLocked = false;
    if (Shop.RecruitSlots.IsValidIndex(Shop.LockedRecruitSlot)
        && Shop.RecruitSlots[Shop.LockedRecruitSlot].State == EACShopSlotState::Locked)
    {
        LockedSlot = Shop.RecruitSlots[Shop.LockedRecruitSlot];
        bHasLocked = true;
    }

    Shop.RecruitSlots.Reset();
    Shop.RecruitSlots.SetNum(SlotCount);          // 先占位，稍后回填锁定项

    TArray<int32> UsedIndices;
    if (bHasLocked)
    {
        const int32 RestoredIndex = FMath::Clamp(Shop.LockedRecruitSlot, 0, SlotCount - 1);
        if (Shop.RecruitSlots.IsValidIndex(RestoredIndex))
        {
            Shop.RecruitSlots[RestoredIndex] = LockedSlot;
            UsedIndices.Add(RestoredIndex);
        }
    }

    for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
    {
        if (UsedIndices.Contains(SlotIndex))
        {
            continue;
        }

        FACRecruitSlot Slot;
        Slot.OperatorId = PickOperatorId(Row, Stream);

        const FACOperatorTemplate* Template = Library != nullptr ? Library->FindOperatorTemplate(Slot.OperatorId) : nullptr;
        Slot.Tier = Template != nullptr ? Template->RecruitTier : 0;
        Slot.DisplayName = Template != nullptr && Template->Definition != nullptr
            ? Template->Definition->DisplayName
            : FText::FromString(Slot.OperatorId.ToString());
        Slot.Price = GetRecruitPrice(Slot.Tier);
        Slot.State = EACShopSlotState::Available;
        Slot.bOwned = false;

        Shop.RecruitSlots[SlotIndex] = Slot;
    }
}

bool FACRunEconomy::CanRefreshRecruit() const
{
    return Config != nullptr && Config->CanRefreshRecruit(Shop.RecruitRefreshCount);
}

bool FACRunEconomy::RefreshRecruit(int32 Row, FRandomStream& Stream, FACPurchaseResult& OutResult)
{
    OutResult = FACPurchaseResult();

    if (!CanRefreshRecruit())
    {
        OutResult.FailReason = FText::FromString(TEXT("刷新次数已用尽"));
        return false;
    }

    const int32 Cost = GetNextRecruitRefreshCost();
    if (!TrySpend(Cost))
    {
        OutResult.FailReason = FText::FromString(TEXT("魂晶不足"));
        return false;
    }

    ++Shop.RecruitRefreshCount;
    RollRecruitPool(Row, Stream);

    OutResult.bSuccess = true;
    OutResult.Cost = Cost;
    return true;
}

bool FACRunEconomy::PurchaseRecruitSlot(int32 SlotIndex, FName& OutOperatorId, int32& OutTier,
                                        FACPurchaseResult& OutResult)
{
    OutResult = FACPurchaseResult();
    OutOperatorId = NAME_None;
    OutTier = 0;

    if (!Shop.RecruitSlots.IsValidIndex(SlotIndex))
    {
        OutResult.FailReason = FText::FromString(TEXT("栏位不存在"));
        return false;
    }

    FACRecruitSlot& Slot = Shop.RecruitSlots[SlotIndex];
    if (Slot.State != EACShopSlotState::Available && Slot.State != EACShopSlotState::Locked)
    {
        OutResult.FailReason = FText::FromString(TEXT("该栏位已售出"));
        return false;
    }

    if (!TrySpend(Slot.Price))
    {
        OutResult.FailReason = FText::FromString(TEXT("魂晶不足"));
        return false;
    }

    OutOperatorId = Slot.OperatorId;
    OutTier = Slot.Tier;
    Slot.State = EACShopSlotState::Purchased;

    OutResult.bSuccess = true;
    OutResult.Cost = Slot.Price;
    return true;
}

void FACRunEconomy::MarkRecruitSlotSold(int32 SlotIndex)
{
    if (Shop.RecruitSlots.IsValidIndex(SlotIndex))
    {
        Shop.RecruitSlots[SlotIndex].State = EACShopSlotState::Sold;
    }
}

bool FACRunEconomy::ToggleRecruitLock(int32 SlotIndex)
{
    if (!Shop.RecruitSlots.IsValidIndex(SlotIndex))
    {
        return false;
    }

    if (Shop.LockedRecruitSlot == SlotIndex)
    {
        Shop.RecruitSlots[SlotIndex].State = EACShopSlotState::Available;
        Shop.LockedRecruitSlot = -1;
        return true;
    }

    // 只能锁 1 个：先解锁旧的。
    if (Shop.RecruitSlots.IsValidIndex(Shop.LockedRecruitSlot)
        && Shop.RecruitSlots[Shop.LockedRecruitSlot].State == EACShopSlotState::Locked)
    {
        Shop.RecruitSlots[Shop.LockedRecruitSlot].State = EACShopSlotState::Available;
    }

    Shop.RecruitSlots[SlotIndex].State = EACShopSlotState::Locked;
    Shop.LockedRecruitSlot = SlotIndex;
    return true;
}

// ---------------------------------------------------------------------------
// 集市
// ---------------------------------------------------------------------------

void FACRunEconomy::RollMarketPool(int32 Row, FRandomStream& Stream)
{
    Shop.MarketSlots.Reset();
    if (Content == nullptr || Config == nullptr)
    {
        return;
    }

    const int32 SlotCount = FMath::Max(1, Config->MarketSlotCount);
    for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
    {
        const int32 Tier = RollTierIndex(Config->EquipmentTierWeights, Row, Stream);
        FACRunEquipment Equipment;
        if (!RollEquipment(Tier, NAME_None, Stream, Equipment))
        {
            continue;
        }

        FACMarketSlot Slot;
        Slot.EquipmentId = Equipment.EquipmentId;
        Slot.DisplayName = Equipment.DisplayName;
        Slot.Tier = Equipment.Tier;
        Slot.Price = GetEquipmentPrice(Equipment.Tier);
        Slot.State = EACShopSlotState::Available;
        Shop.MarketSlots.Add(Slot);
    }
}

bool FACRunEconomy::CanRefreshMarket() const
{
    return Config != nullptr && Config->CanRefreshMarket(Shop.MarketRefreshCount);
}

bool FACRunEconomy::RefreshMarket(int32 Row, FRandomStream& Stream, FACPurchaseResult& OutResult)
{
    OutResult = FACPurchaseResult();

    if (!CanRefreshMarket())
    {
        OutResult.FailReason = FText::FromString(TEXT("刷新次数已用尽"));
        return false;
    }

    const int32 Cost = GetNextMarketRefreshCost();
    if (!TrySpend(Cost))
    {
        OutResult.FailReason = FText::FromString(TEXT("魂晶不足"));
        return false;
    }

    ++Shop.MarketRefreshCount;
    RollMarketPool(Row, Stream);

    OutResult.bSuccess = true;
    OutResult.Cost = Cost;
    return true;
}

bool FACRunEconomy::PurchaseMarketSlot(int32 SlotIndex, FACRunEquipment& OutEquipment, FACPurchaseResult& OutResult)
{
    OutResult = FACPurchaseResult();
    OutEquipment = FACRunEquipment();

    if (!Shop.MarketSlots.IsValidIndex(SlotIndex))
    {
        OutResult.FailReason = FText::FromString(TEXT("栏位不存在"));
        return false;
    }

    FACMarketSlot& Slot = Shop.MarketSlots[SlotIndex];
    if (!IsSlotAvailable(Slot.State))
    {
        OutResult.FailReason = FText::FromString(TEXT("该栏位已售出"));
        return false;
    }
    if (Content == nullptr)
    {
        OutResult.FailReason = FText::FromString(TEXT("内容库不可用"));
        return false;
    }

    if (!TrySpend(Slot.Price))
    {
        OutResult.FailReason = FText::FromString(TEXT("魂晶不足"));
        return false;
    }

    // 从内容池里取回完整定义（货架上只存了 ID/展示字段）。
    TArray<FACRunEquipment> Pool;
    Content->GetEquipmentByTier(Slot.Tier, Pool);
    for (const FACRunEquipment& Candidate : Pool)
    {
        if (Candidate.EquipmentId == Slot.EquipmentId)
        {
            OutEquipment = Candidate;
            break;
        }
    }

    Slot.State = EACShopSlotState::Sold;
    OutResult.bSuccess = true;
    OutResult.Cost = Slot.Price;
    return true;
}

void FACRunEconomy::MarkMarketSlotSold(int32 SlotIndex)
{
    if (Shop.MarketSlots.IsValidIndex(SlotIndex))
    {
        Shop.MarketSlots[SlotIndex].State = EACShopSlotState::Sold;
    }
}

// ---------------------------------------------------------------------------
// 锻体
// ---------------------------------------------------------------------------

void FACRunEconomy::RollFragmentOffer(int32 Row, FRandomStream& Stream)
{
    Shop.FragmentOffer.Options.Reset();
    Shop.FragmentOffer.bPurchased = false;
    Shop.FragmentOffer.Price = GetFragmentPrice();

    if (Content == nullptr || Config == nullptr)
    {
        return;
    }

    // 三个**同品阶**碎片（《经济系统详细设计》§7.3）。
    const int32 Tier = RollTierIndex(Config->FragmentTierWeights, Row, Stream);
    const int32 OptionCount = FMath::Max(1, Config->FragmentOptionCount);

    TArray<FACRunFragment> Pool;
    if (!Content->GetFragmentsByTier(Tier, Pool))
    {
        return;
    }

    TArray<FACRunFragment> Remaining = Pool;
    for (int32 Index = 0; Index < OptionCount && Remaining.Num() > 0; ++Index)
    {
        const int32 Pick = Stream.RandRange(0, Remaining.Num() - 1);
        Shop.FragmentOffer.Options.Add(Remaining[Pick]);
        Remaining.RemoveAt(Pick);
    }
}

bool FACRunEconomy::PurchaseFragmentOffer(FACPurchaseResult& OutResult)
{
    OutResult = FACPurchaseResult();

    if (Shop.FragmentOffer.Options.Num() == 0)
    {
        OutResult.FailReason = FText::FromString(TEXT("没有可选的属性碎片"));
        return false;
    }
    if (Shop.FragmentOffer.bPurchased)
    {
        OutResult.FailReason = FText::FromString(TEXT("本节点已购买过锻体"));
        return false;
    }

    const int32 Cost = Shop.FragmentOffer.Price;
    if (!TrySpend(Cost))
    {
        OutResult.FailReason = FText::FromString(TEXT("魂晶不足"));
        return false;
    }

    Shop.FragmentOffer.bPurchased = true;
    OutResult.bSuccess = true;
    OutResult.Cost = Cost;
    return true;
}

// ---------------------------------------------------------------------------
// 单件随机（遗物节点 / 事件奖励用）
// ---------------------------------------------------------------------------

bool FACRunEconomy::RollEquipment(int32 Tier, FName ExcludeId, FRandomStream& Stream, FACRunEquipment& OutEquipment) const
{
    OutEquipment = FACRunEquipment();
    if (Content == nullptr)
    {
        return false;
    }

    TArray<FACRunEquipment> Pool;
    Content->GetEquipmentByTier(Tier, Pool);
    if (!ExcludeId.IsNone() && Pool.Num() > 1)
    {
        Pool.RemoveAll([ExcludeId](const FACRunEquipment& Candidate)
        {
            return Candidate.EquipmentId == ExcludeId;
        });
    }
    if (Pool.Num() == 0)
    {
        return false;
    }

    OutEquipment = Pool[Stream.RandRange(0, Pool.Num() - 1)];
    return true;
}

bool FACRunEconomy::RollFragment(int32 Tier, FRandomStream& Stream, FACRunFragment& OutFragment) const
{
    OutFragment = FACRunFragment();
    if (Content == nullptr)
    {
        return false;
    }

    TArray<FACRunFragment> Pool;
    if (!Content->GetFragmentsByTier(Tier, Pool) || Pool.Num() == 0)
    {
        return false;
    }

    OutFragment = Pool[Stream.RandRange(0, Pool.Num() - 1)];
    return true;
}

FString FACRunEconomy::ToDebugString() const
{
    return FString::Printf(TEXT("魂晶 %d · 招募刷新 %d/%d · 集市刷新 %d/%d"),
                           SoulCrystal,
                           Shop.RecruitRefreshCount,
                           Config != nullptr ? Config->RecruitRefreshCosts.Num() : 0,
                           Shop.MarketRefreshCount,
                           Config != nullptr ? Config->MarketRefreshCosts.Num() : 0);
}
