// 干员基类
// 注意：本类声明为 Abstract，SpawnActor 无法直接实例化它（ULevel::SpawnActor 会拒绝 Abstract 类）；
// 内容侧还没有具体干员类的阶段，UBattleWorld::ResolveUnitClass 会回落到可实例化的 AACBattleUnitBase。
#pragma once

#include "CoreMinimal.h"
#include "Battle/ACBattleUnitBase.h"
#include "ACOperatorActor.generated.h"

UCLASS(Abstract)
class AUTOCHESSBATTLE_API AACOperatorActor : public AACBattleUnitBase
{
    GENERATED_BODY()

public:
    // 后续所有干员继承它。
};
