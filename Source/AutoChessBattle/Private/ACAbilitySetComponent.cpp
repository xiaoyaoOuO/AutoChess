
#include "Battle/ACAbilitySetComponent.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/Actor.h"
#include "GAS/ACAbilitySet.h"

UACAbilitySetComponent::UACAbilitySetComponent()
{
    // 本组件不参与逻辑，也不参与表现：只在授予那一刻被调用一次，因此不需要 tick。
    // 与单位 Actor 的 `PrimaryActorTick.bCanEverTick = false` 一致（§2.2：逻辑由 UBattleWorld::Step 统一驱动）。
    PrimaryComponentTick.bCanEverTick = false;
}

void UACAbilitySetComponent::GrantToOwner() const
{
    // 两份都空时静默跳过（内容侧还没给这个单位配清单是**正常状态** —— 蕾拉的 `GrantedEffects`
    // 本来就是空的、召唤物也没有额外被动）。这里刻意不打日志：本单位每次生成都会走到这里，
    // 告警会按单位数量刷屏，而"内容没配全"该由内容校验工具报，不该由运行期热路径报。
    //
    // ⚠️ 阶段 3.3：判据从 `AbilitySet == nullptr` 放宽成"两份都空" ——
    //    追加通道（装备 / 强化 / 词条带来的被动）与清单**互相独立**，
    //    一个没有清单的单位仍可能因为装了装备而拿到被动能力。
    if (AbilitySet == nullptr && ExtraAbilities.Num() == 0)
    {
        return;
    }

    AActor* const Owner = GetOwner();
    if (Owner == nullptr)
    {
        return;
    }

    // 取 ASC 走 `IAbilitySystemInterface`（而不是 `FindComponentByClass`）：
    //   - 接口是"这个 Actor 有 GAS"的**显式契约**，而"能不能在组件树里找到一个 ASC"
    //     只是实现细节，两者将来可能分叉（例如 Avatar 与 Owner 分离的写法）；
    //   - `AACBattleUnitBase` 正是这样实现的（§2.1：单位类实现该接口）。
    // 这里**不** `#include "Battle/ACBattleUnitBase.h"`：那会形成头文件环
    // （ACBattleUnitBase.h → Battle/ACAbilitySetComponent.h → 回到自己），
    // 而本函数只需要接口，不需要具体单位类型。
    IAbilitySystemInterface* const AbilitySystemOwner = Cast<IAbilitySystemInterface>(Owner);
    if (AbilitySystemOwner == nullptr)
    {
        return;
    }

    UAbilitySystemComponent* const ASC = AbilitySystemOwner->GetAbilitySystemComponent();
    if (ASC == nullptr)
    {
        return;
    }

    // 幂等性由调用方保证，**并且调用方必须是 `UBattleWorld::RegisterUnit`**（阶段 3.2b 订正）：
    //   `BeginPlay` 也会调一次，但那一刻清单还没注入（清单由 RegisterUnit 经 `SetAbilitySet` 给），
    //   所以那一次是空转。RegisterUnit 是三条生成路径（玩家 / 敌人 / 召唤）里
    //   "数据 + 位置 + 清单都就绪"的第一个时刻，且此时 BeginPlay 早已跑完
    //   （ASC 的 `InitAbilityActorInfo` 已完成），因此是唯一安全且语义正确的授予点。
    // 组件自己不设 `bGranted` 标志：那会引入"什么时候该清零"的第二个状态，
    // 而本阶段没有任何复用路径（C7：暂不做对象池；将来加池时必须在 `ResetForReuse` 里显式处理）。

    // ---- ① 定义自带的清单（技能 / 普攻 / 天生被动 / 常驻 GE）----
    if (AbilitySet != nullptr)
    {
        AbilitySet->GiveTo(*ASC);
    }

    // ---- ② 追加授予的被动能力（装备 / 强化 / 词条）----
    // 走 `UACAbilitySet::GiveAbilitiesTo` 而不是自己写一遍 `GiveAbility`：
    // 等级口径 / InputID / 空位跳过这几件事只有一处实现，两份必然分叉。
    // `SourceObject` 传组件自身（`const_cast` 只用于把它当不透明句柄传下去）：
    // 被动能力若需要回查"我是被哪条追加通道授予的"，经
    // `GetCurrentAbilitySpec()->SourceObject` 拿到的是本组件，而不是某个清单 —— 这与
    // 清单授予的语义（SourceObject = 清单）保持一致，两者都能定位到"授予来源"。
    if (ExtraAbilities.Num() > 0)
    {
        UACAbilitySet::GiveAbilitiesTo(*ASC, ExtraAbilities, TArray<int32>(),
                                       const_cast<UACAbilitySetComponent*>(this));
    }
}
