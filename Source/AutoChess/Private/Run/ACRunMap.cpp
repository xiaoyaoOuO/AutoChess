#include "Run/ACRunMap.h"
#include "Run/ACRunConfig.h"

namespace
{
    /** 按权重抽取下标；权重全为 0 或数组为空时返回 0（保证确定性，不依赖随机状态）。 */
    int32 PickWeightedIndex(FRandomStream& Stream, const TArray<float>& Weights)
    {
        float Total = 0.f;
        for (const float Weight : Weights)
        {
            Total += FMath::Max(0.f, Weight);
        }
        if (Total <= 0.f)
        {
            return 0;
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

    /** 按权重抽取节点类型。 */
    EACRunNodeType PickNodeType(FRandomStream& Stream, const TMap<EACRunNodeType, float>& Weights)
    {
        TArray<EACRunNodeType> Types;
        TArray<float> Values;
        Weights.GetKeys(Types);
        Weights.GenerateValueArray(Values);

        // TMap 的遍历顺序不稳定，排序后再抽取，保证同种子同结果。
        for (int32 Outer = 1; Outer < Types.Num(); ++Outer)
        {
            for (int32 Inner = Outer; Inner > 0; --Inner)
            {
                if (static_cast<uint8>(Types[Inner - 1]) <= static_cast<uint8>(Types[Inner]))
                {
                    break;
                }
                Types.Swap(Inner - 1, Inner);
                Values.Swap(Inner - 1, Inner);
            }
        }

        if (Types.Num() == 0)
        {
            return EACRunNodeType::Combat;
        }
        return Types[PickWeightedIndex(Stream, Values)];
    }
}

void ACRunNodeType::BuildDefaultWeights(TMap<EACRunNodeType, float>& OutWeights)
{
    OutWeights.Reset();
    OutWeights.Add(EACRunNodeType::Combat, 45.f);
    OutWeights.Add(EACRunNodeType::Elite, 12.f);
    OutWeights.Add(EACRunNodeType::Challenge, 12.f);
    OutWeights.Add(EACRunNodeType::Merchant, 10.f);
    OutWeights.Add(EACRunNodeType::Campfire, 11.f);
    OutWeights.Add(EACRunNodeType::Relic, 10.f);
}

FText ACRunNodeType::ToDisplayText(EACRunNodeType Type)
{
    switch (Type)
    {
    case EACRunNodeType::Entrance:  return FText::FromString(TEXT("起点"));
    case EACRunNodeType::Combat:    return FText::FromString(TEXT("遇敌"));
    case EACRunNodeType::Elite:     return FText::FromString(TEXT("精英"));
    case EACRunNodeType::Challenge: return FText::FromString(TEXT("挑战"));
    case EACRunNodeType::Merchant:  return FText::FromString(TEXT("商人"));
    case EACRunNodeType::Campfire:  return FText::FromString(TEXT("篝火"));
    case EACRunNodeType::Relic:     return FText::FromString(TEXT("遗物"));
    case EACRunNodeType::Waypoint:  return FText::FromString(TEXT("中转站"));
    case EACRunNodeType::Boss:      return FText::FromString(TEXT("首领"));
    default:                        return FText::FromString(TEXT("未知"));
    }
}

void FACRunMap::Reset()
{
    Nodes.Reset();
    StartNodeId = INDEX_NONE;
    CurrentNodeId = INDEX_NONE;
    MaxRow = 0;
}

int32 FACRunMap::AddNode(int32 Row, int32 SlotInRow, EACRunNodeType Type, int32 Seed)
{
    FACRunMapNode Node;
    Node.NodeId = Nodes.Num();
    Node.Row = Row;
    Node.SlotInRow = SlotInRow;
    Node.Type = Type;
    // 遭遇种子：RunSeed 混入 NodeId，保证"同一张图 + 同一个节点"永远得到同一批敌人。
    Node.EncounterSeed = static_cast<int32>(HashCombine(static_cast<uint32>(Seed), static_cast<uint32>(Node.NodeId + 1)));
    Nodes.Add(Node);
    return Node.NodeId;
}

void FACRunMap::LinkNodes(int32 FromId, int32 ToId)
{
    if (!Nodes.IsValidIndex(FromId) || !Nodes.IsValidIndex(ToId))
    {
        return;
    }
    if (!Nodes[FromId].NextNodeIds.Contains(ToId))
    {
        Nodes[FromId].NextNodeIds.Add(ToId);
    }
    if (!Nodes[ToId].PrevNodeIds.Contains(FromId))
    {
        Nodes[ToId].PrevNodeIds.Add(FromId);
    }
}

void FACRunMap::Generate(int32 Seed, const UACRunConfig& Config)
{
    Reset();

    FRandomStream Stream(Seed);
    MaxRow = FMath::Max(1, Config.BossRow);

    TMap<EACRunNodeType, float> Weights = Config.RowNodeTypeWeights;
    if (Weights.Num() == 0)
    {
        ACRunNodeType::BuildDefaultWeights(Weights);
    }

    TArray<TArray<int32>> RowNodeIds;
    RowNodeIds.SetNum(MaxRow + 1);

    // ---- Row 0：起点分叉出 EntryBranchCount 条支路 ----
    const int32 EntryCount = FMath::Max(1, Config.EntryBranchCount);
    for (int32 Slot = 0; Slot < EntryCount; ++Slot)
    {
        RowNodeIds[0].Add(AddNode(0, Slot, Slot == 0 ? EACRunNodeType::Entrance : EACRunNodeType::Combat, Seed));
    }
    StartNodeId = RowNodeIds[0][0];

    // ---- Row 1..MaxRow：按行生成并连边 ----
    for (int32 Row = 1; Row <= MaxRow; ++Row)
    {
        const int32 PreviousCount = RowNodeIds[Row - 1].Num();
        int32 Count = 0;

        if (Row == MaxRow)
        {
            Count = 1;                                              // 终点：唯一 BOSS
        }
        else if (Row == Config.WaypointRow)
        {
            Count = FMath::Clamp(1, 1, FMath::Max(1, PreviousCount)); // 中转站收束所有路线
        }
        else
        {
            const int32 MinNodes = FMath::Max(1, Config.NodesPerRowMin);
            const int32 MaxNodes = FMath::Max(MinNodes, Config.NodesPerRowMax);
            Count = Stream.RandRange(MinNodes, MaxNodes);
        }

        for (int32 Slot = 0; Slot < Count; ++Slot)
        {
            EACRunNodeType Type = EACRunNodeType::Combat;
            if (Row == MaxRow)
            {
                Type = EACRunNodeType::Boss;
            }
            else if (Row == Config.WaypointRow)
            {
                Type = EACRunNodeType::Waypoint;
            }
            else
            {
                Type = PickNodeType(Stream, Weights);
            }
            RowNodeIds[Row].Add(AddNode(Row, Slot, Type, Seed));
        }

        // 连边：上一行第 i 个节点 → 本行 index ∈ [i-1, i+1]。
        // 该区间必与本行有效范围相交（首尾节点各至少覆盖 index 0 / Count-1），因此不存在死路。
        for (int32 PrevSlot = 0; PrevSlot < PreviousCount; ++PrevSlot)
        {
            const int32 IntervalStart = FMath::Max(0, PrevSlot - 1);
            const int32 IntervalEnd = FMath::Min(Count - 1, PrevSlot + 1);
            for (int32 Slot = IntervalStart; Slot <= IntervalEnd; ++Slot)
            {
                LinkNodes(RowNodeIds[Row - 1][PrevSlot], RowNodeIds[Row][Slot]);
            }
        }
    }

    // 每行按 SlotInRow 升序排列后继，保证"可选节点列表"的顺序稳定。
    for (FACRunMapNode& Node : Nodes)
    {
        Node.NextNodeIds.Sort();
        Node.PrevNodeIds.Sort();
    }

    RefreshVisibility(Config);
}

const FACRunMapNode* FACRunMap::Find(int32 NodeId) const
{
    return Nodes.IsValidIndex(NodeId) ? &Nodes[NodeId] : nullptr;
}

bool FACRunMap::CanMoveTo(int32 NodeId) const
{
    if (!Nodes.IsValidIndex(NodeId))
    {
        return false;
    }
    if (CurrentNodeId == INDEX_NONE)
    {
        // 尚未上路：只能选起点行。
        return Nodes[NodeId].Row == 0;
    }
    const FACRunMapNode* Current = Find(CurrentNodeId);
    return Current != nullptr && Current->NextNodeIds.Contains(NodeId);
}

bool FACRunMap::MoveTo(int32 NodeId)
{
    if (!CanMoveTo(NodeId))
    {
        return false;
    }
    CurrentNodeId = NodeId;
    return true;
}

void FACRunMap::GetSelectableNodes(TArray<int32>& OutNodeIds) const
{
    OutNodeIds.Reset();
    if (CurrentNodeId == INDEX_NONE)
    {
        for (const FACRunMapNode& Node : Nodes)
        {
            if (Node.Row == 0)
            {
                OutNodeIds.Add(Node.NodeId);
            }
        }
        return;
    }
    if (const FACRunMapNode* Current = Find(CurrentNodeId))
    {
        OutNodeIds.Append(Current->NextNodeIds);
    }
}

void FACRunMap::RefreshVisibility(const UACRunConfig& Config)
{
    const int32 CurrentRow = GetCurrentRow();
    const int32 LastVisibleRow = CurrentRow + FMath::Max(0, Config.VisibleRowsAhead);
    for (FACRunMapNode& Node : Nodes)
    {
        Node.bVisible = Node.Row >= CurrentRow && Node.Row <= LastVisibleRow;
    }
}

int32 FACRunMap::GetCurrentRow() const
{
    const FACRunMapNode* Current = GetCurrentNode();
    return Current != nullptr ? Current->Row : 0;
}

bool FACRunMap::IsAtBoss() const
{
    const FACRunMapNode* Current = GetCurrentNode();
    return Current != nullptr && Current->Type == EACRunNodeType::Boss;
}

FString FACRunMap::ToDebugString() const
{
    const FACRunMapNode* Current = GetCurrentNode();
    if (Current == nullptr)
    {
        return FString::Printf(TEXT("地图 %d 节点，尚未上路（起点 %d）"), Nodes.Num(), StartNodeId);
    }

    TArray<int32> Next;
    GetSelectableNodes(Next);
    FString NextText;
    if (Next.Num() == 0)
    {
        NextText = TEXT("无（终点）");
    }
    else
    {
        for (const int32 NodeId : Next)
        {
            const FACRunMapNode* Node = Find(NodeId);
            NextText += Node != nullptr
                ? FString::Printf(TEXT("#%d %s  "), Node->NodeId, *ACRunNodeType::ToDisplayText(Node->Type).ToString())
                : FString::Printf(TEXT("#%d ?  "), NodeId);
        }
    }
    return FString::Printf(TEXT("第 %d 行 · 当前 #%d %s · 可选 → %s"),
                           Current->Row, Current->NodeId, *ACRunNodeType::ToDisplayText(Current->Type).ToString(), *NextText);
}
