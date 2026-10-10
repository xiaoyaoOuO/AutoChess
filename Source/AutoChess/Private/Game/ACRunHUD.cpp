#include "Game/ACRunHUD.h"
#include "Run/ACRunSubsystem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"

void AACRunHUD::DrawHUD()
{
    Super::DrawHUD();

    if (!bPanelVisible || Canvas == nullptr)
    {
        return;
    }

    UGameInstance* GameInstance = GetGameInstance();
    const UACRunSubsystem* Run = GameInstance != nullptr
        ? GameInstance->GetSubsystem<UACRunSubsystem>()
        : nullptr;
    if (Run == nullptr)
    {
        return;
    }

    // 半透明底衬：无论背后是什么场景都能看清文字。
    const float BoxWidth = FMath::Min(900.f, Canvas->ClipX - PanelX * 2.f);
    DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.55f), PanelX - 8.f, PanelY - 8.f, BoxWidth, Canvas->ClipY - PanelY * 2.f);

    UFont* SmallFont = GEngine != nullptr ? GEngine->GetSmallFont() : nullptr;
    UFont* MediumFont = GEngine != nullptr ? GEngine->GetMediumFont() : nullptr;
    if (SmallFont == nullptr)
    {
        return;
    }

    // ---- 标题 ----
    DrawText(TEXT("AutoChess · Run 层示例（Content 为空也可运行）"),
             FLinearColor(0.6f, 0.9f, 1.f), PanelX, PanelY,
             MediumFont != nullptr ? MediumFont : SmallFont, 1.15f);

    float CursorY = PanelY + 28.f;
}
