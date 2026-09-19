#include "Flow/ACBattleSubsystem.h"
#include "Data/ACBattleDataSubsystem.h"

void UBattleSubsystem::Deinitialize()
{
    if (Session != nullptr)
    {
        Session->Shutdown();
        Session = nullptr;
    }
    bResultReady = false;
    Super::Deinitialize();
}

bool UBattleSubsystem::StartBattle(const FACBattleSetup& InSetup, const FACBattleLaunchOptions& InOptions)
{
    if (Session != nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Battle] StartBattle ignored: a session already exists."));
        return false;
    }

    FACBattleDataContext DataContext;
    if (UGameInstance* GameInstance = GetGameInstance())
    {
        if (UBattleDataSubsystem* DataSubsystem = GameInstance->GetSubsystem<UBattleDataSubsystem>())
        {
            DataContext = DataSubsystem->BuildContext();
        }
    }

    bResultReady = false;
    CachedResult = FACBattleResult();

    Session = NewObject<UBattleSession>(this);
    Session->Initialize(InSetup, InOptions, DataContext);

    if (Session->IsFinished())
    {
        PublishResultIfReady();
    }
    return true;
}

void UBattleSubsystem::PushCommand(const FACBattleCommand& Command)
{
    if (Session != nullptr)
    {
        Session->PushCommand(Command);
    }
}

void UBattleSubsystem::AbandonBattle()
{
    if (Session != nullptr)
    {
        Session->RequestAbandon();
    }
}

void UBattleSubsystem::Tick(float RealDeltaSeconds)
{
    if (Session == nullptr)
    {
        return;
    }
    if (Session->IsFinished())
    {
        PublishResultIfReady();
        return;
    }

    Session->Tick(RealDeltaSeconds);
    if (Session->IsFinished())
    {
        PublishResultIfReady();
    }
}

void UBattleSubsystem::PublishResultIfReady()
{
    if (bResultReady || Session == nullptr)
    {
        return;
    }

    FACBattleResult Result;
    if (Session->TryConsumeBuiltResult(Result))
    {
        CachedResult = Result;
        bResultReady = true;
        OnBattleFinished.Broadcast(CachedResult);
    }
}

bool UBattleSubsystem::TryConsumeResult(FACBattleResult& OutResult)
{
    if (!bResultReady)
    {
        return false;
    }
    OutResult = CachedResult;
    bResultReady = false;

    if (Session != nullptr)
    {
        Session->Shutdown();
        Session = nullptr;
    }
    return true;
}
