// 阶段 1 新增（GAS 重构实施方案 §3.3 / §7 阶段 1）：自定义 EffectContext 的实现。
// 只有两件事：`GetScriptStruct` / `Duplicate` 在头文件里内联（都是标准扩展点），
// 本文件放 `NetSerialize`（需要在 .cpp 见到 FArchive 的完整定义）。
#include "GAS/ACGameplayEffectContext.h"

#include "Engine/HitResult.h"

bool FACGameplayEffectContext::NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess)
{
    return FGameplayEffectContext::NetSerialize(Ar, Map, bOutSuccess);
}
