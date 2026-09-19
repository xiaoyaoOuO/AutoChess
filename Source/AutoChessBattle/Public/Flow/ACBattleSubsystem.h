// M01 对外入口：启动战斗、提交命令、消费结果。
// 注意：当前版本由调用方（Run 层 / 测试）驱动 Tick；关卡加载/卸载在 Run 层适配中实现（D8）。
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/ACBattleSetup.h"
#include "Core/ACBattleTypes.h"
#include "Flow/ACBattleSession.h"
#include "ACBattleSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBattleFinished, const FACBattleResult&, Result);

UCLASS()
class AUTOCHESSBATTLE_API UBattleSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;

    /**
     * 启动一场战斗。
     * 当前版本直接创建逻辑世界并立即进入 Combat（无关卡加载）；Run 层后续接入关卡切换即可。
     */
    UFUNCTION(BlueprintCallable, Category = "Battle")
    bool StartBattle(const FACBattleSetup& InSetup, const FACBattleLaunchOptions& InOptions);

    UFUNCTION(BlueprintCallable, Category = "Battle")
    void PushCommand(const FACBattleCommand& Command);

    UFUNCTION(BlueprintCallable, Category = "Battle")
    void AbandonBattle();

    /** 结果一次性消费；返回 true 表示 OutResult 有效。 */
    UFUNCTION(BlueprintCallable, Category = "Battle")
    bool TryConsumeResult(FACBattleResult& OutResult);

    /** 由外部驱动（GameMode / 测试）调用，推进真实时间。 */
    UFUNCTION(BlueprintCallable, Category = "Battle")
    void Tick(float RealDeltaSeconds);

    UFUNCTION(BlueprintPure, Category = "Battle")
    UBattleSession* GetSession() const { return Session; }

    UPROPERTY(BlueprintAssignable, Category = "Battle")
    FOnBattleFinished OnBattleFinished;

private:
    void PublishResultIfReady();

    UPROPERTY() TObjectPtr<UBattleSession> Session = nullptr;

    FACBattleResult CachedResult;
    bool bResultReady = false;
};
