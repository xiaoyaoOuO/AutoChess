// 阶段 0a 新增（GAS 重构实施方案 §2.1 / §2.2）：
// 单位表现挂点：Mesh / 朝向 / 血条等表现侧资源的容器。
//
// 两条硬约束（§2.2）：
//   ① 表现组件**允许自行 Tick**（不参与逻辑，执行顺序无要求）；
//   ② 逻辑不得读写本组件状态——内核只通过 AACBattleUnitBase 的 getter 读数据，
//      本组件的一切字段都只是"表现侧副本"。
//
// 阶段 0a 只做最小骨架：字段与 Tick 开关到位，Mesh / 动画 / GameplayCue 的完整接入在阶段 0c 与阶段 5。
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/ACBattleTypes.h"
#include "ACUnitPresentationComponent.generated.h"

class UStaticMeshComponent;

UCLASS(ClassGroup=(AutoChess), meta=(BlueprintSpawnableComponent))
class AUTOCHESSBATTLE_API UACUnitPresentationComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UACUnitPresentationComponent();

    virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** 只记录朝向；实际旋转 / 动画在阶段 0c 接 Mesh 时实现。 */
    void SetFacing(EACFacing InFacing);

    FORCEINLINE EACFacing GetFacing() const { return Facing; }

protected:
    /** 表现用 Mesh（可空）。阶段 0a 不创建，0c 由内容侧 / BeginPlay 挂载。 */
    UPROPERTY(EditAnywhere, Category="Battle|Presentation")
    TObjectPtr<UStaticMeshComponent> MeshComponent = nullptr;

    /** 朝向对应的 Yaw 偏移（内容侧按模型自身朝向微调）。 */
    UPROPERTY(EditAnywhere, Category="Battle|Presentation")
    float FacingYawOffset = 0.f;

    /** 当前朝向（表现侧副本，逻辑不得读它做判定）。 */
    EACFacing Facing = EACFacing::Up;
};
