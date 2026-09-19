// Run 层的状态面板（**代码绘制，不需要任何 UMG 资产**）。
//
// 为什么用 AHUD::DrawHUD 而不是 UMG：本工程 Content 为空，UMG 需要 Widget 蓝图；
// 而 HUD 的 Canvas 绘制纯 C++ 就能跑，正好满足"零资产可运行"的约束。
// 正式表现层接入后，这个类可以被 UACRunHUDWidget 取代——它只读 UACRunSubsystem 的公开数据，
// 不含任何逻辑，因此替换成本极低。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ACRunHUD.generated.h"

class UFont;

UCLASS()
class AUTOCHESS_API AACRunHUD : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;

    UFUNCTION(BlueprintCallable, Category = "Run")
    void SetPanelVisible(bool bVisible) { bPanelVisible = bVisible; }

    UFUNCTION(BlueprintPure, Category = "Run")
    bool IsPanelVisible() const { return bPanelVisible; }

protected:
    /** 面板是否显示（正式版本会改为按 UI 状态切换）。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Run|HUD")
    bool bPanelVisible = true;

    /** 状态区左上角坐标。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Run|HUD")
    float PanelX = 24.f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Run|HUD")
    float PanelY = 24.f;

    /** 字号缩放。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Run|HUD")
    float PanelScale = 0.9f;

    /** 最近日志显示行数。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Run|HUD")
    int32 LogLines = 8;
};
