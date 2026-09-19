// 阶段 0a 新增（GAS 重构实施方案 §2.1 / §2.3）：
// 干员基类（空壳）。后续所有干员（AACOperator_Solivar / AACOperator_Leila / AACOperator_Ironwall …）继承它。
//
// 为什么是空壳：§2.1 明确"基类只放共有项"，且**禁止**在单位基类里塞
// 装备槽 / 强化 / 敌人专属字段——那些走组件或子类；干员特有内容走 UACAbilitySet 数据资产（D12）。
// 因此本文件只是一个类型锚点：阶段 0b / 0c 内容侧开始继承它时，不需要再动内核代码。
//
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
