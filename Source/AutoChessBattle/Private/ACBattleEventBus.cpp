#include "Events/ACBattleEventBus.h"

// ---------------------------------------------------------------------------
// FBattleSubscriptionHandle
// ---------------------------------------------------------------------------

FBattleSubscriptionHandle::~FBattleSubscriptionHandle()
{
    Reset();
}

FBattleSubscriptionHandle::FBattleSubscriptionHandle(FBattleSubscriptionHandle&& Other) noexcept
{
    Bus = Other.Bus;
    Key = Other.Key;
    Other.Bus = nullptr;
    Other.Key = 0;
}

FBattleSubscriptionHandle& FBattleSubscriptionHandle::operator=(FBattleSubscriptionHandle&& Other) noexcept
{
    if (this != &Other)
    {
        Reset();
        Bus = Other.Bus;
        Key = Other.Key;
        Other.Bus = nullptr;
        Other.Key = 0;
    }
    return *this;
}

void FBattleSubscriptionHandle::Reset()
{
    if (FBattleEventBus* EventBus = Bus)
    {
        EventBus->Unsubscribe(Key);
    }
    Bus = nullptr;
    Key = 0;
}

// ---------------------------------------------------------------------------
// FBattleEventBus
// ---------------------------------------------------------------------------

void FBattleEventBus::Initialize()
{
    Subscriptions.Reset();
    Listeners.Reset();
    PendingRemovals.Reset();
    PendingAdds.Reset();
    DeferredQueue.Reset();
    DeferredBatch.Reset();
    NextKey = 1;
    NextListenerKey = 1;
    bDispatching = false;
}

void FBattleEventBus::Shutdown()
{
    Subscriptions.Reset();
    Listeners.Reset();
    PendingRemovals.Reset();
    PendingAdds.Reset();
    DeferredQueue.Reset();
    DeferredBatch.Reset();
    bDispatching = false;
}

FBattleSubscriptionHandle FBattleEventBus::Subscribe(FGameplayTag HookTag, FName SubscriberId,
                                                     FUnitId OwnerUnit, int32 Priority,
                                                     FBattleHookDelegate Delegate)
{
    FSubscription Subscription;
    Subscription.Key = NextKey++;
    Subscription.HookTag = HookTag;
    Subscription.SubscriberId = SubscriberId;
    Subscription.OwnerUnit = OwnerUnit;
    Subscription.Priority = Priority;
    Subscription.Delegate = MoveTemp(Delegate);

    const uint64 NewKey = Subscription.Key;

    // 派发中禁止直接改订阅表（00A §2 / M03 §1.3）：新增与注销一样延迟到派发结束。
    if (bDispatching)
    {
        PendingAdds.Add(MoveTemp(Subscription));
    }
    else
    {
        TArray<FSubscription>& Array = Subscriptions.FindOrAdd(HookTag);
        InsertSorted(Array, Subscription);
    }

    FBattleSubscriptionHandle Handle;
    Handle.Bus = this;
    Handle.Key = NewKey;
    return Handle;
}

void FBattleEventBus::InsertSorted(TArray<FSubscription>& Array, const FSubscription& Subscription)
{
    // 文档约定的派发顺序：Priority 降序 -> SubscriberId 升序。
    // 比较为全序（相同 Key 不可能同时存在），因此插入位置唯一、与插入次序无关。
    int32 InsertIndex = Array.Num();
    for (int32 Index = 0; Index < Array.Num(); ++Index)
    {
        const FSubscription& Existing = Array[Index];

        if (Subscription.Priority > Existing.Priority)
        {
            InsertIndex = Index;
            break;
        }
        if (Subscription.Priority == Existing.Priority)
        {
            if (Subscription.SubscriberId.LexicalLess(Existing.SubscriberId))
            {
                InsertIndex = Index;
                break;
            }
        }
    }
    Array.Insert(Subscription, InsertIndex);
}

FBattleHookResult FBattleEventBus::Dispatch(FGameplayTag HookTag, FBattleHookContext& Context)
{
    FBattleHookResult Result;

    const TArray<FSubscription>* Array = Subscriptions.Find(HookTag);
    if (Array == nullptr || Array->Num() == 0)
    {
        return Result;
    }

    // 快照：回调内可能注册/注销订阅（延迟到派发结束应用），直接引用 TMap 内的数组会悬垂。
    TArray<FSubscription> Snapshot = *Array;

    // 派发期间不允许修改订阅表：注销进入 PendingRemovals、新增进入 PendingAdds，派发结束后统一应用。
    const bool bWasDispatching = bDispatching;
    bDispatching = true;

    for (const FSubscription& Subscription : Snapshot)
    {
        if (!Subscription.bEnabled || !Subscription.Delegate.IsBound())
        {
            continue;
        }
        if (PendingRemovals.Contains(Subscription.Key))
        {
            continue;
        }

        const FBattleHookResult SubscriberResult = Subscription.Delegate.Execute(Context);
        Result.bCancel |= SubscriberResult.bCancel;
        Result.bInterrupt |= SubscriberResult.bInterrupt;

        if (Context.bCancelled || Result.bCancel)
        {
            break;
        }
    }

    bDispatching = bWasDispatching;
    if (!bDispatching)
    {
        ApplyPendingRemovals();
        ApplyPendingAdds();
    }
    return Result;
}

uint64 FBattleEventBus::Listen(FGameplayTag EventTag, FBattleEventListener Listener)
{
    const uint64 Key = NextListenerKey++;
    Listeners.Add(Key, MoveTemp(Listener));
    return Key;
}

void FBattleEventBus::Unlisten(uint64 ListenerKey)
{
    Listeners.Remove(ListenerKey);
}

void FBattleEventBus::Broadcast(const FBattleEvent& Event)
{
    // 无监听者时直接返回：无头运行下这是常态，避免每次都分配快照数组（00A §7）。
    if (Listeners.Num() == 0)
    {
        return;
    }

    // 事件不参与规则判定，允许订阅者增删；使用值拷贝遍历，避免迭代器失效。
    TArray<FBattleEventListener> Snapshot;
    Snapshot.Reserve(Listeners.Num());
    for (const TPair<uint64, FBattleEventListener>& Pair : Listeners)
    {
        Snapshot.Add(Pair.Value);
    }
    for (FBattleEventListener& Listener : Snapshot)
    {
        if (Listener)
        {
            Listener(Event);
        }
    }
}

void FBattleEventBus::EnqueueDeferred(TFunction<void()>&& Action)
{
    DeferredQueue.Add(MoveTemp(Action));
}

void FBattleEventBus::FlushDeferred()
{
    int32 BatchCount = 0;
    while (DeferredQueue.Num() > 0 && BatchCount < MaxDeferredBatches)
    {
        DeferredBatch = MoveTemp(DeferredQueue);
        DeferredQueue.Reset();

        for (TFunction<void()>& Action : DeferredBatch)
        {
            if (Action)
            {
                Action();
            }
        }
        DeferredBatch.Reset();
        ++BatchCount;
    }

    if (DeferredQueue.Num() > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] Deferred event queue exceeded %d batches, dropping %d entries."),
               MaxDeferredBatches, DeferredQueue.Num());
        DeferredQueue.Reset();
    }
}

void FBattleEventBus::Unsubscribe(uint64 Key)
{
    if (Key == 0)
    {
        return;
    }

    if (bDispatching)
    {
        PendingRemovals.AddUnique(Key);
        return;
    }

    for (TPair<FGameplayTag, TArray<FSubscription>>& Pair : Subscriptions)
    {
        const int32 Removed = Pair.Value.RemoveAll([Key](const FSubscription& Subscription)
        {
            return Subscription.Key == Key;
        });
        if (Removed > 0)
        {
            return;
        }
    }
}

void FBattleEventBus::UnsubscribeAllByOwner(FUnitId OwnerUnit)
{
    if (OwnerUnit == InvalidUnitId)
    {
        return;
    }

    if (bDispatching)
    {
        for (const TPair<FGameplayTag, TArray<FSubscription>>& Pair : Subscriptions)
        {
            for (const FSubscription& Subscription : Pair.Value)
            {
                if (Subscription.OwnerUnit == OwnerUnit)
                {
                    PendingRemovals.AddUnique(Subscription.Key);
                }
            }
        }
        return;
    }

    for (TPair<FGameplayTag, TArray<FSubscription>>& Pair : Subscriptions)
    {
        Pair.Value.RemoveAll([OwnerUnit](const FSubscription& Subscription)
        {
            return Subscription.OwnerUnit == OwnerUnit;
        });
    }
}

void FBattleEventBus::UnsubscribeBySubscriber(FName SubscriberId)
{
    for (TPair<FGameplayTag, TArray<FSubscription>>& Pair : Subscriptions)
    {
        Pair.Value.RemoveAll([SubscriberId](const FSubscription& Subscription)
        {
            return Subscription.SubscriberId == SubscriberId;
        });
    }
}

int32 FBattleEventBus::GetSubscriberCount(FGameplayTag HookTag) const
{
    const TArray<FSubscription>* Array = Subscriptions.Find(HookTag);
    return Array != nullptr ? Array->Num() : 0;
}

void FBattleEventBus::DumpSubscriptions() const
{
    for (const TPair<FGameplayTag, TArray<FSubscription>>& Pair : Subscriptions)
    {
        UE_LOG(LogTemp, Log, TEXT("[Battle] Hook %s : %d subscriber(s)"), *Pair.Key.ToString(), Pair.Value.Num());
        for (const FSubscription& Subscription : Pair.Value)
        {
            UE_LOG(LogTemp, Log, TEXT("    [%llu] %s (Owner=%d, Priority=%d)"),
                   Subscription.Key, *Subscription.SubscriberId.ToString(), Subscription.OwnerUnit, Subscription.Priority);
        }
    }
}

void FBattleEventBus::ApplyPendingRemovals()
{
    for (const uint64 Key : PendingRemovals)
    {
        for (TPair<FGameplayTag, TArray<FSubscription>>& Pair : Subscriptions)
        {
            Pair.Value.RemoveAll([Key](const FSubscription& Subscription)
            {
                return Subscription.Key == Key;
            });
        }
    }
    PendingRemovals.Reset();
}

void FBattleEventBus::ApplyPendingAdds()
{
    if (PendingAdds.Num() == 0)
    {
        return;
    }

    // 先取出再插入：InsertSorted 可能扩容 Subscriptions，遍历期间不得持有其元素引用。
    TArray<FSubscription> Adds = MoveTemp(PendingAdds);
    PendingAdds.Reset();
    for (const FSubscription& Subscription : Adds)
    {
        TArray<FSubscription>& Array = Subscriptions.FindOrAdd(Subscription.HookTag);
        InsertSorted(Array, Subscription);
    }
}
