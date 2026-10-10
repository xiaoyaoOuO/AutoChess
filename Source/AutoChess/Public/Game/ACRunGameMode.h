#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ACRunGameMode.generated.h"

class UACRunSubsystem;

/**
 * 局内循环的驱动者。
 * 若需要在特定关卡加载 Run，可在蓝图子类里改 bStartRunOnBeginPlay / DemoSeed。
 */
UCLASS()
class AUTOCHESS_API AACRunGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    AACRunGameMode();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    /** 开一局（Seed = 0 时随机）。 */
    UFUNCTION(BlueprintCallable, Category = "Run")
    void StartNewRun(int32 Seed);

    UACRunSubsystem* GetRunSubsystem() const;

protected:
    /** 进入关卡即自动开一局（便于"打开编辑器就能看到东西在跑"）。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Run|Demo")
    bool bStartRunOnBeginPlay = true;

    /** 固定种子：非 0 时每局都用它，便于复现同一张图与同一批战斗。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Run|Demo")
    int32 DemoSeed = 0;
};
