#include "Content/ACOperatorLibrary.h"
#include "Content/ACBattleContentLibrary.h"
#include "Core/ACBattleTags.h"

void UACOperatorLibrary::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // 兜底属性：**任何定义资产都缺失**时使用。数值取"能互相打得动的量级，
    // 保证零内容工程也能跑完一场并看到胜负，而不是双方都打不动。
    DefaultStatBlock.InitDefaults();
    DefaultStatBlock.Set(EACStat::MaxHP, 100.f);
    DefaultStatBlock.Set(EACStat::ATK, 10.f);
    DefaultStatBlock.Set(EACStat::TECH, 0.f);
    DefaultStatBlock.Set(EACStat::DEF, 5.f);
    DefaultStatBlock.Set(EACStat::RES, 0.f);
    DefaultStatBlock.Set(EACStat::CritValue, 0.f);
    DefaultStatBlock.Set(EACStat::ASPD, 100.f);
    DefaultStatBlock.Set(EACStat::Range, 1.f);
    DefaultStatBlock.Set(EACStat::FocusMax, 100.f);
    DefaultStatBlock.Set(EACStat::FocusInit, 0.f);
    DefaultStatBlock.Set(EACStat::FocusRegen, 5.f);
    DefaultStatBlock.Set(EACStat::FocusPerAttack, 0.f);
    DefaultStatBlock.Set(EACStat::MentalMax, 100.f);

    EnsureContentReady();
}

UACBattleContentLibrary* UACOperatorLibrary::GetContent() const
{
    UGameInstance* GameInstance = GetGameInstance();
    return GameInstance != nullptr ? GameInstance->GetSubsystem<UACBattleContentLibrary>() : nullptr;
}

void UACOperatorLibrary::EnsureContentReady()
{
    UACBattleContentLibrary* Content = GetContent();
    if (Content == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Content] UACBattleContentLibrary 不可用，Run 层将使用兜底属性"));
        return;
    }
    Content->EnsureInitialized();

    OperatorTemplates.Reset();
    OperatorIds.Reset();
    for (const FACContentOperatorEntry& Entry : Content->GetOperators())
    {
        if (Entry.Definition == nullptr || Entry.Definition->DefinitionId.IsNone())
        {
            continue;
        }

        FACOperatorTemplate Template;
        Template.Definition = Entry.Definition;
        Template.RecruitTier = Entry.RecruitTier;
        Template.ClassTag = Entry.ClassTag;
        OperatorTemplates.Add(Entry.Definition->DefinitionId, Template);
        OperatorIds.Add(Entry.Definition->DefinitionId);
    }

    // 确定性顺序：按ID 字典序（避免 TMap 遍历顺序影响招募随机抽取的复现）。
    OperatorIds.Sort(FNameLexicalLess());
}

const FACOperatorTemplate* UACOperatorLibrary::FindOperatorTemplate(FName OperatorId) const
{
    return OperatorTemplates.Find(OperatorId);
}

const UOperatorDefinition* UACOperatorLibrary::FindOperatorDefinition(FName OperatorId) const
{
    const FACOperatorTemplate* Template = OperatorTemplates.Find(OperatorId);
    return Template != nullptr ? Template->Definition.Get() : nullptr;
}

const UEnemyDefinition* UACOperatorLibrary::FindEnemyDefinition(FName EnemyDefinitionId) const
{
    UACBattleContentLibrary* Content = GetContent();
    return Content != nullptr ? Content->FindEnemy(EnemyDefinitionId) : nullptr;
}

void UACOperatorLibrary::GetOperatorIdsByTier(int32 Tier, TArray<FName>& OutIds) const
{
    OutIds.Reset();
    for (const FName& OperatorId : OperatorIds)
    {
        const FACOperatorTemplate* Template = OperatorTemplates.Find(OperatorId);
        if (Template != nullptr && Template->RecruitTier == Tier)
        {
            OutIds.Add(OperatorId);
        }
    }
}

void UACOperatorLibrary::PickRandomOperators(FRandomStream& Stream, int32 Count, TArray<FName>& OutIds) const
{
    OutIds.Reset();
    if (Count <= 0 || OperatorIds.Num() == 0)
    {
        return;
    }

    // 池子较小（几十人量级），直接随机抽取并移除。
    // 这样天然不重复，且抽取次数只与Count 相关，便于复现。
    TArray<FName> Pool = OperatorIds;
    const int32 Take = FMath::Min(Count, Pool.Num());
    for (int32 Index = 0; Index < Take; ++Index)
    {
        const int32 PickIndex = Stream.RandRange(0, Pool.Num() - 1);
        OutIds.Add(Pool[PickIndex]);
        Pool.RemoveAt(PickIndex);
    }
}

FACContentUpgradeOption UACOperatorLibrary::ResolveUpgradeChoice(FName OperatorId, int32 TierIndex) const
{
    UACBattleContentLibrary* Content = GetContent();
    return Content != nullptr ? Content->ResolveUpgradeChoice(OperatorId, TierIndex) : FACContentUpgradeOption();
}

// ---------------------------------------------------------------------------
// 干员属性表（Run 层的属性口径）
//
// 为什么属性放在这里而不用UOperatorDefinition::StatsByLevel。
//   `AACBattleUnitBase::InitializeFromPlayerSpec` 的优先级是StatsByLevel **优先于** Spec.BaseStats。
//   只有Spec.BaseStats 才是 Run 层把"等级 + 锻体 + 装备 + 复活惩罚"折算后的唯一口径
//   （ACRunStatUtils::ComposeBaseStats）。若定义里也填一份，就会把成长整份丢掉。
//   因此约定：**代码侧干员定义不填StatsByLevel**，属性由本表提供。
//
// 数据来源：doc/tables/Operators.csv（顺序与 MakeStatBlock 的参数一致）。
// 接入资产后：把本表换成DataTable 读取即可，其余代码不动。
// ---------------------------------------------------------------------------
namespace
{
    using ACBattleContent::MakeStatBlock;

    /** 单个干员的五档属性（D→S）*/
    struct FOperatorStatRow
    {
        FName OperatorId;
        FACStatBlock ByLevel[5];
    };

    const FOperatorStatRow* FindOperatorStatRow(FName OperatorId)
    {
        // 索利瓦尔（OP_01）：HP 160/260/380/580/700、DEF 10/40/70/120/150、RES 10/15/20/20/25。
        //                  ATK 30/45/60/80/110、ASPD 80、射程1、专注回复5、专注上限100
        static const FOperatorStatRow Rows[] =
        {
            {
                FName(TEXT("OP_01")),
                {
                    MakeStatBlock(160.f,  30.f, 0.f,  10.f, 10.f, 0.f, 80.f, 1.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(260.f,  45.f, 0.f,  40.f, 15.f, 0.f, 80.f, 1.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(380.f,  60.f, 0.f,  70.f, 20.f, 0.f, 80.f, 1.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(580.f,  80.f, 0.f, 120.f, 20.f, 0.f, 80.f, 1.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(700.f, 110.f, 0.f, 150.f, 25.f, 0.f, 80.f, 1.f, 100.f, 0.f, 5.f, 0.f)
                }
            },
            {
                // 蕾拉中尉（OP_17）：射手，射程6（远程）；专注上限100，普攻回专注 A11 默认 5
                FName(TEXT("OP_17")),
                {
                    MakeStatBlock(140.f,  35.f, 0.f,   8.f, 10.f, 0.f, 70.f, 6.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(220.f,  50.f, 0.f,  20.f, 15.f, 0.f, 70.f, 6.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(320.f,  70.f, 0.f,  35.f, 20.f, 0.f, 70.f, 6.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(450.f,  95.f, 0.f,  55.f, 20.f, 0.f, 70.f, 6.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(560.f, 125.f, 0.f,  80.f, 25.f, 0.f, 70.f, 6.f, 100.f, 0.f, 5.f, 0.f)
                }
            },
            {
                // 铁壁（OP_Tank_Ironwall）：坦克，射程1
                FName(TEXT("OP_Tank_Ironwall")),
                {
                    MakeStatBlock(240.f,  20.f, 0.f,  40.f, 15.f, 0.f, 60.f, 1.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(400.f,  32.f, 0.f,  80.f, 25.f, 0.f, 60.f, 1.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(560.f,  45.f, 0.f, 130.f, 35.f, 0.f, 60.f, 1.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(780.f,  60.f, 0.f, 190.f, 40.f, 0.f, 60.f, 1.f, 100.f, 0.f, 5.f, 0.f),
                    MakeStatBlock(980.f,  80.f, 0.f, 250.f, 45.f, 0.f, 60.f, 1.f, 100.f, 0.f, 5.f, 0.f)
                }
            }
        };

        for (const FOperatorStatRow& Row : Rows)
        {
            if (Row.OperatorId == OperatorId)
            {
                return &Row;
            }
        }
        return nullptr;
    }
}

void UACOperatorLibrary::GetStatsByLevel(FName OperatorId, int32 Level, FACStatBlock& OutBlock) const
{
    const FOperatorStatRow* Row = FindOperatorStatRow(OperatorId);
    if (Row != nullptr)
    {
        OutBlock = Row->ByLevel[FMath::Clamp(Level, 0, 4)];
        return;
    }

    // 没登记的干员：回退兜底块，保证战斗仍能跑完（而不是0 血 0 攻的"稻草人"）。
    OutBlock = DefaultStatBlock;
}
