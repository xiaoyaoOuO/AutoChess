// Run 层：经济与商店（《经济系统详细设计》§7）。
//
// 本文件只做三件事：
//   ① 记账（魂晶的收支，**唯一**允许改动的入口是 AddSoulCrystal / TrySpend）；
//   ② 定价（全部从 UACRunConfig 读，禁止在这里写死数字）；
//   ③ 掷骰（招募池 / 集市货架 / 锻体三选一）——所有随机都走外部传入的 FRandomStream，
//      因此"同种子 + 同调用序列"必然得到同一批货，这是回放与批测的前提（对应 M04 的要求）。
//
// 它**不**知道干员是谁、装备是什么效果——那些由内容库（UACBattleContentLibrary）提供，
// 本类只按品级抽取并把结果写进 FACRunShopState（纯数据，可直接给 UI 或存档）。
#pragma once

#include "CoreMinimal.h"
#include "Run/ACRunTypes.h"
// 必须包含：FACPurchaseResult 是 USTRUCT，其反射样板由这个头文件提供（见 ACRunSquad.h 的同类说明）。
#include "ACRunEconomy.generated.h"

class UACRunConfig;
class UACBattleContentLibrary;
class UACOperatorLibrary;

/** 一次购买/刷新的结果（拒绝原因直接可显示）。 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACPurchaseResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Run|Economy")
    bool bSuccess = false;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Economy")
    FText FailReason;

    UPROPERTY(BlueprintReadOnly, Category = "Run|Economy")
    int32 Cost = 0;
};

/**
 * 经济与商店。
 * 状态（魂晶 + 货架）与逻辑放在一起，是因为它们共享同一个"本节点内有效"的生命周期：
 * 每到一个新节点就 Roll 一次，离开节点后货架失效。
 */
class AUTOCHESS_API FACRunEconomy
{
public:
    void Initialize(const UACRunConfig* InConfig, const UACBattleContentLibrary* InContent,
                    const UACOperatorLibrary* InLibrary);

    /** 开新局：清空货架、设置初始魂晶。 */
    void Reset(int32 StartingSoulCrystal);

    // ---- 魂晶收支（唯一入口）----
    int32 GetSoulCrystal() const { return SoulCrystal; }
    void AddSoulCrystal(int32 Delta);

    /** 扣款；不足则返回 false 且不改状态。 */
    bool TrySpend(int32 Cost);

    // ---- 价格查询（供 UI 显示与自动演示判断）----
    int32 GetRecruitPrice(int32 Tier) const;
    int32 GetRecruitRecyclePrice(int32 Tier) const;
    int32 GetEquipmentPrice(int32 Tier) const;
    int32 GetEquipmentRecyclePrice(int32 Tier) const;
    int32 GetNextRecruitRefreshCost() const;
    int32 GetNextMarketRefreshCost() const;
    int32 GetFragmentPrice() const;
    int32 GetReviveCost(int32 Row) const;

    // ---- 货架 ----
    const FACRunShopState& GetShop() const { return Shop; }

    /** 生成招募栏位（保留被锁定的那一个）。 */
    void RollRecruitPool(int32 Row, FRandomStream& Stream);

    /** 生成集市栏位。 */
    void RollMarketPool(int32 Row, FRandomStream& Stream);

    /** 生成锻体三选一。 */
    void RollFragmentOffer(int32 Row, FRandomStream& Stream);

    bool CanRefreshRecruit() const;
    bool CanRefreshMarket() const;

    /** 付费刷新招募。 */
    bool RefreshRecruit(int32 Row, FRandomStream& Stream, FACPurchaseResult& OutResult);

    /** 付费刷新集市。 */
    bool RefreshMarket(int32 Row, FRandomStream& Stream, FACPurchaseResult& OutResult);

    /** 购买招募栏位；成功时返回干员定义 ID 与品级（由上层交给编队）。 */
    bool PurchaseRecruitSlot(int32 SlotIndex, FName& OutOperatorId, int32& OutTier, FACPurchaseResult& OutResult);

    /** 购买集市栏位；成功时返回装备（由上层放进库存）。 */
    bool PurchaseMarketSlot(int32 SlotIndex, FACRunEquipment& OutEquipment, FACPurchaseResult& OutResult);

    /** 购买锻体；成功后由上层从 Options 里挑一个交给编队。 */
    bool PurchaseFragmentOffer(FACPurchaseResult& OutResult);

    void MarkRecruitSlotSold(int32 SlotIndex);
    void MarkMarketSlotSold(int32 SlotIndex);

    /** 锁定 / 解锁招募栏位（《经济系统详细设计》§7.1：只能锁 1 个）。 */
    bool ToggleRecruitLock(int32 SlotIndex);

    /** 在指定品级里随机取一件装备（遗物节点等场景）。 */
    bool RollEquipment(int32 Tier, FName ExcludeId, FRandomStream& Stream, FACRunEquipment& OutEquipment) const;

    /** 在指定品阶里随机取一个碎片。 */
    bool RollFragment(int32 Tier, FRandomStream& Stream, FACRunFragment& OutFragment) const;

    FString ToDebugString() const;

private:
    /** 按"行数 → 品级权重偏移"抽取品级；Weights 为配置里的基础权重。 */
    int32 RollTierIndex(const TArray<float>& Weights, int32 Row, FRandomStream& Stream) const;

    /** 随机取一个干员 ID（招募池）。 */
    FName PickOperatorId(int32 Row, FRandomStream& Stream) const;

    const UACRunConfig* Config = nullptr;
    const UACBattleContentLibrary* Content = nullptr;
    const UACOperatorLibrary* Library = nullptr;

    int32 SoulCrystal = 0;
    FACRunShopState Shop;
};
