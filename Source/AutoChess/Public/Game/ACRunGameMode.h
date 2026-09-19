// Run 层的**驱动外壳**（薄驱动，不含任何规则）。
//
// 为什么需要它：M01 明确约定"战斗内核由调用方 Tick"，而调用方就是 Run 层；
// Run 层自身又是 GameInstanceSubsystem（没有 Tick）。因此需要关卡里的一个 Actor
// 把每帧时间喂进去——这就是本类的全部职责。
//
// 依赖方向（单向，不回头）：
//   AACRunGameMode → UACRunSubsystem → UBattleSubsystem → UBattleSession → UBattleWorld
//
// 关卡与资产说明：本工程当前 Content 为空，因此本类**不引用任何地图/蓝图资产**，
// 挂上 GameMode 即可运行（见 Config/DefaultEngine.ini 的 GameDefaultMap/GameMode 配置项）。
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

    /** 关掉自动演示（改由控制台/UI 驱动）。 */
    UFUNCTION(BlueprintCallable, Category = "Run")
    void SetAutoDemo(bool bEnabled);

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
