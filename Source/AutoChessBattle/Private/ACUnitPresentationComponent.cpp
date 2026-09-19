#include "Battle/Components/ACUnitPresentationComponent.h"

UACUnitPresentationComponent::UACUnitPresentationComponent()
{
    // §2.2：表现允许自行 Tick（逻辑单位 Actor 的 PrimaryActorTick 是关的，只有本组件能 tick）。
    PrimaryComponentTick.bCanEverTick = true;
}

void UACUnitPresentationComponent::TickComponent(float DeltaTime, enum ELevelTick TickType,
                                                 FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    // 阶段 0a：最小骨架，尚无每帧表现逻辑。
    // 阶段 0c 在这里做移动插值与朝向平滑；阶段 5 由 GameplayCue 驱动特效。
}

void UACUnitPresentationComponent::SetFacing(EACFacing InFacing)
{
    Facing = InFacing;
}
