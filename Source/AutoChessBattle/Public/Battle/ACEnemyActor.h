// 阶段 0a 新增（GAS 重构实施方案 §2.1 / §2.3）：
// 敌人基类（空壳）。后续所有敌人（AACEnemy_HuskGrub / GreatLeech / MotherNest …）继承它。
//
// 为什么是空壳：§2.1 明确"基类只放共有项"，且**禁止**在单位基类里塞
// 装备槽 / 强化 / 敌人专属字段——精英 / Boss 标记等敌人专属内容走子类或组件（D12）。
//
// 注意：本类声明为 Abstract，SpawnActor 无法直接实例化它（ULevel::SpawnActor 会拒绝 Abstract 类）；
// 内容侧还没有具体敌人类的阶段，UBattleWorld::ResolveUnitClass 会回落到可实例化的 AACBattleUnitBase。
#pragma once

#include "CoreMinimal.h"
#include "Battle/ACBattleUnitBase.h"
#include "ACEnemyActor.generated.h"

UCLASS(Abstract)
class AUTOCHESSBATTLE_API AACEnemyActor : public AACBattleUnitBase
{
    GENERATED_BODY()

public:
    // 后续所有敌人继承它。
};
