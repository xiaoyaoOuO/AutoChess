// M03 事件总线与钩子：内核唯一的事件通道。
// 规则：派发顺序 = (Priority 降序 -> SubscriberId 升序)；派发中禁止直接改订阅表；
//       钩子内禁止 Spawn/Kill/改容器，一律 EnqueueDeferred 延迟到步骤末。
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Core/ACBattleTypes.h"

class FBattleEventBus;

/** 订阅句柄：owner 销毁或 Reset 时自动注销。 */
struct AUTOCHESSBATTLE_API FBattleSubscriptionHandle
{
public:
    FBattleSubscriptionHandle() = default;
    ~FBattleSubscriptionHandle();

    FBattleSubscriptionHandle(const FBattleSubscriptionHandle&) = delete;
    FBattleSubscriptionHandle& operator=(const FBattleSubscriptionHandle&) = delete;
    FBattleSubscriptionHandle(FBattleSubscriptionHandle&& Other) noexcept;
    FBattleSubscriptionHandle& operator=(FBattleSubscriptionHandle&& Other) noexcept;

    void Reset();
    bool IsValid() const { return Bus != nullptr && Key != 0; }

private:
    friend class FBattleEventBus;
    FBattleEventBus* Bus = nullptr;
    uint64 Key = 0;
};

/** 钩子上下文基类：所有具体钩子共用；可被订阅者修改（伤害值/层数/取消标记）。 */
struct AUTOCHESSBATTLE_API FBattleHookContext
{
    FUnitId Source = InvalidUnitId;
    FUnitId Target = InvalidUnitId;
    /**
     * 事件发生的**绝对时间**（秒，`FACBattleTime::Now`）。
     * 阶段 0.5：字段名从 `Tick` 改成 `Time` —— 名字里带着 tick 却装秒，会长期误导后来人
     * （§5.2「字段名也改」）。注意它是绝对时间，不是战斗内相对时间。
     */
    float Time = 0.f;
    FGameplayTagContainer Tags;

    /** 通用可变数值（伤害值、层数、治疗量等，语义由钩子约定）。 */
    float FloatValue = 0.f;
    int32 IntValue = 0;
    bool bBoolValue = false;

    /** 置 true 表示事件被取消。 */
    bool bCancelled = false;
};

/** 钩子返回值。 */
struct AUTOCHESSBATTLE_API FBattleHookResult
{
    bool bCancel = false;
    bool bInterrupt = false;
};

DECLARE_DELEGATE_RetVal_OneParam(FBattleHookResult, FBattleHookDelegate, FBattleHookContext&);

/** 广播事件（只通知，不可回写）。 */
struct AUTOCHESSBATTLE_API FBattleEvent
{
    FGameplayTag Tag;
    FUnitId Source = InvalidUnitId;
    FUnitId Target = InvalidUnitId;
    /** 事件发生的绝对时间（秒）；字段名与语义说明见 FBattleHookContext::Time。 */
    float Time = 0.f;
    float FloatValue = 0.f;
    int32 IntValue = 0;
    bool bBoolValue = false;
};

using FBattleEventListener = TFunction<void(const FBattleEvent&)>;

/**
 * 战斗事件总线（M03）。
 * 仅存活于单场战斗；由 UBattleWorld 持有。
 */
class AUTOCHESSBATTLE_API FBattleEventBus
{
public:
    void Initialize();
    void Shutdown();

    // ---- 钩子订阅 ----
    FBattleSubscriptionHandle Subscribe(FGameplayTag HookTag, FName SubscriberId,
                                        FUnitId OwnerUnit, int32 Priority,
                                        FBattleHookDelegate Delegate);

    /** 同步派发：可取消、可改值。 */
    FBattleHookResult Dispatch(FGameplayTag HookTag, FBattleHookContext& Context);

    // ---- 广播事件 ----
    uint64 Listen(FGameplayTag EventTag, FBattleEventListener Listener);
    void Unlisten(uint64 ListenerKey);
    void Broadcast(const FBattleEvent& Event);

    // ---- 延迟事件 ----
    void EnqueueDeferred(TFunction<void()>&& Action);
    void FlushDeferred();
    int32 GetDeferredQueueNum() const { return DeferredQueue.Num(); }

    // ---- 生命周期 ----
    void Unsubscribe(uint64 Key);
    void UnsubscribeAllByOwner(FUnitId OwnerUnit);
    void UnsubscribeBySubscriber(FName SubscriberId);
    int32 GetSubscriberCount(FGameplayTag HookTag) const;

    /** 调试：输出当前订阅表。 */
    void DumpSubscriptions() const;

private:
    struct FSubscription
    {
        uint64 Key = 0;
        FGameplayTag HookTag;
        FName SubscriberId;
        FUnitId OwnerUnit = InvalidUnitId;
        int32 Priority = 0;
        FBattleHookDelegate Delegate;
        bool bEnabled = true;
    };

    void ApplyPendingRemovals();
    void ApplyPendingAdds();
    void InsertSorted(TArray<FSubscription>& Array, const FSubscription& Subscription);

    TMap<FGameplayTag, TArray<FSubscription>> Subscriptions;
    TMap<uint64, FBattleEventListener> Listeners;
    TArray<uint64> PendingRemovals;
    /** 派发期间新增的订阅：与注销一样延迟到派发结束统一应用。 */
    TArray<FSubscription> PendingAdds;
    TArray<TFunction<void()>> DeferredQueue;
    TArray<TFunction<void()>> DeferredBatch;
    bool bDispatching = false;
    uint64 NextKey = 1;
    uint64 NextListenerKey = 1;
    int32 MaxDeferredBatches = 3;
};
