#include "Game/ACRunPlayerController.h"
#include "Run/ACRunSubsystem.h"
#include "Components/InputComponent.h"
#include "Flow/ACBattleSubsystem.h"
#include "Flow/ACBattleSession.h"
#include "Battle/ACBattleWorld.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformMemory.h"

UACRunSubsystem* AACRunPlayerController::GetRunSubsystem() const
{
    UGameInstance* GameInstance = GetGameInstance();
    return GameInstance != nullptr ? GameInstance->GetSubsystem<UACRunSubsystem>() : nullptr;
}

void AACRunPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();

}



// ===========================================================================
// 调试命令：BattleDumpGAS / BattleDumpUnits / BattlePerfReport（阶段 4 §7）
//
// 三条命令共用"取当前战斗世界"这一个辅助。
// 为什么路径这么绕（GameInstance → BattleSubsystem → Session → World）：
// `UBattleWorld` 是 `UBattleSession` 的 `NewObject` 子对象，外部唯一合法的持有链就是这一条；
// 而**不能用** `Cast<UBattleWorld>(SomeActor->GetOuter())` —— 单位 Actor 的 Outer 是 `ULevel`，
// 那个 Cast 恒为 nullptr（见 `UBattleWorld::FindFromActor` 的说明）。
// ===========================================================================

UBattleWorld* AACRunPlayerController::GetActiveBattleWorld() const
{
    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance == nullptr)
    {
        return nullptr;
    }
    UBattleSubsystem* Battle = GameInstance->GetSubsystem<UBattleSubsystem>();
    if (Battle == nullptr)
    {
        return nullptr;
    }
    UBattleSession* Session = Battle->GetSession();
    return Session != nullptr ? Session->GetBattleWorld() : nullptr;
}
