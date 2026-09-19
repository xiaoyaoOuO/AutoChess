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

    // ---- 状态 ----
    TArray<FString> StatusLines;
    Run->BuildStatusText().ParseIntoArrayLines(StatusLines, /*bCullEmpty=*/false);
    for (const FString& Line : StatusLines)
    {
        DrawText(Line, FLinearColor::White, PanelX, CursorY, SmallFont, PanelScale);
        CursorY += 14.f * PanelScale;
    }

    // ---- 最近日志 ----
    CursorY += 10.f;
    DrawText(TEXT("--- 最近事件 ---"), FLinearColor(0.9f, 0.85f, 0.4f), PanelX, CursorY, SmallFont, PanelScale);
    CursorY += 16.f * PanelScale;

    // 注意：BuildLogText(MaxLines) 返回的是**拼好的多行字符串**（不是把行写进 TArray），
    // 因此先取字符串、再按行切分。局部变量名不能叫 LogLines —— 那是本类的成员（C4458 遮蔽告警）。
    TArray<FString> LogTextLines;
    Run->BuildLogText(LogLines).ParseIntoArrayLines(LogTextLines, /*bCullEmpty=*/false);
    for (const FString& Line : LogTextLines)
    {
        DrawText(Line, FLinearColor(0.82f, 0.82f, 0.82f), PanelX, CursorY, SmallFont, PanelScale);
        CursorY += 14.f * PanelScale;
    }

    // ---- 操作提示 ----
    DrawText(TEXT("控制台：AutoChess.Run.Status / Step / Auto 1 / Battle / Start [Seed] / Abandon   |   F1 状态  F2 推进一步  F3 直接开战  F4 新开局"),
             FLinearColor(0.65f, 0.65f, 0.65f), PanelX, Canvas->ClipY - 40.f, SmallFont, 0.85f);
}
