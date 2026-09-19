// 阶段 0a 新增（GAS 重构实施方案 §2.1 / §2.3）：
// 召唤物 / 分身基类（空壳）。后续所有召唤物与分身继承它。
//
// 为什么是空壳：§2.1 明确"基类只放共有项"；召唤物的差异（继承比例、到期、主人死亡策略）
// 已经在 FACSummonSpec / FACUnitSpawnRequest 里数据化，不需要在类层次上再开字段（D12）。
//
// 注意：本类声明为 Abstract，SpawnActor 无法直接实例化它（ULevel::SpawnActor 会拒绝 Abstract 类）；
// 内容侧还没有具体召唤物类的阶段，UBattleWorld::ResolveUnitClass 会回落到可实例化的 AACBattleUnitBase。
#pragma once

#include "CoreMinimal.h"
#include "Battle/ACBattleUnitBase.h"
#include "ACSummonActor.generated.h"

UCLASS(Abstract)
class AUTOCHESSBATTLE_API AACSummonActor : public AACBattleUnitBase
{
    GENERATED_BODY()

public:
    // 后续所有召唤物 / 分身继承它。
};
