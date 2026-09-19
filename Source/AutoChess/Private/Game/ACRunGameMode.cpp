#include "Game/ACRunGameMode.h"
#include "Run/ACRunSubsystem.h"
#include "Game/ACRunPlayerController.h"
#include "Game/ACRunHUD.h"

AACRunGameMode::AACRunGameMode()
{
    // 全部指向 C++ 类，不引用任何蓝图资产（Content 为空也能跑）。
    PlayerControllerClass = AACRunPlayerController::StaticClass();
    HUDClass = AACRunHUD::StaticClass();

    // 本作没有可操控 Pawn（棋盘是逻辑驱动 + HUD 展示）：
    // DefaultPawnClass = nullptr + bStartPlayersAsSpectators = true，
    // 可以保证 PlayerController 仍被创建（控制台命令与 HUD 依赖它），同时不做 Pawn 生成/碰撞处理。
    DefaultPawnClass = nullptr;
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

void AACRunGameMode::SetAutoDemo(bool bEnabled)
{
    if (UACRunSubsystem* Run = GetRunSubsystem())
    {
        Run->SetAutoDemo(bEnabled);
    }
}

void AACRunGameMode::StartNewRun(int32 Seed)
{
    if (UACRunSubsystem* Run = GetRunSubsystem())
    {
        Run->StartRun(Seed);
    }
}
