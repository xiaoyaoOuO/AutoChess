#include "Game/ACRunGameMode.h"
#include "Run/ACRunSubsystem.h"
#include "Game/ACRunPlayerController.h"
#include "Game/ACRunHUD.h"

AACRunGameMode::AACRunGameMode()
{
    PrimaryActorTick.bCanEverTick = true;

    bStartPlayersAsSpectators = true;
}

void AACRunGameMode::BeginPlay()
{
    Super::BeginPlay();

    if (bStartRunOnBeginPlay)
    {
        StartNewRun(DemoSeed);
    }
}

void AACRunGameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    // Run 层是战斗内核的驱动方：这一行就是"逻辑时间"的唯一入口。
    if (UACRunSubsystem* Run = GetRunSubsystem())
    {
        Run->Tick(DeltaSeconds);
    }
}

UACRunSubsystem* AACRunGameMode::GetRunSubsystem() const
{
    UGameInstance* GameInstance = GetGameInstance();
    return GameInstance != nullptr ? GameInstance->GetSubsystem<UACRunSubsystem>() : nullptr;
}

void AACRunGameMode::StartNewRun(int32 Seed)
{
    if (UACRunSubsystem* Run = GetRunSubsystem())
    {
        Run->StartRun(Seed);
    }
}
