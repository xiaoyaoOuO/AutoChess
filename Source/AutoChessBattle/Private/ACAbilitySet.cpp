// 阶段 1 新增骨架，**阶段 3.2a 补完**（GAS 重构实施方案 §3.3 / §4.2 / §7 阶段 3）：
// 能力 / GE 授予清单实现。
//
// 阶段 1 这里是空实现（当时全仓没有任何 `UGameplayAbility` / `UGameplayEffect` 子类）；
// 3.1 产出全部原生内容类之后，本阶段把它真正接上 —— 这是"接线"三步里的第一步：
// 单位一出生就拿到自己的技能 / 被动 / 常驻 GE。
#include "GAS/ACAbilitySet.h"

#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"

void UACAbilitySet::GiveAbilitiesTo(UAbilitySystemComponent& ASC,
                                    const TArray<TSubclassOf<UGameplayAbility>>& Abilities,
                                    const TArray<int32>& Levels,
                                    UObject* SourceObject)
{
    for (int32 Index = 0; Index < Abilities.Num(); ++Index)
    {
        const TSubclassOf<UGameplayAbility>& AbilityClass = Abilities[Index];
        if (AbilityClass.Get() == nullptr)
        {
            // 内容侧数组里留了空位（编辑器里加了一行但没选类）。静默跳过：
            // 授予阶段刷 Warning 会随单位数量线性放大（一场战斗几十条），
            // 而真正的排查入口是内容校验（阶段 4 的调试命令）。
            continue;
        }

        // 等级口径：`Levels` 与 `Abilities` 按**下标**对应；
        // 数组短了（内容只填等级不给全）或填了非正值时一律按 1 处理，
        // 取 `IsValidIndex` 而不是先 `SetNum` 补齐 —— 补齐会改内容资产的数据长度，
        // 那种"读一次就改了别人数据"的行为很难排查。
        const int32 Level = Levels.IsValidIndex(Index) && Levels[Index] > 0
            ? Levels[Index]
            : 1;

        ASC.GiveAbility(FGameplayAbilitySpec(AbilityClass, Level, INDEX_NONE, SourceObject));
    }
}

void UACAbilitySet::GiveTo(UAbilitySystemComponent& ASC) const
{
    // -----------------------------------------------------------------------
    // ① 能力：`FGameplayAbilitySpec(AbilityClass, Level, InputID, SourceObject)`
    // -----------------------------------------------------------------------
    // 引擎签名已核实（GameplayAbilitySpec.h:184）：
    //   `FGameplayAbilitySpec(TSubclassOf<UGameplayAbility> InAbilityClass, int32 InLevel = 1,
    //                         int32 InInputID = INDEX_NONE, UObject* InSourceObject = nullptr)`
    // 因此四个实参的顺序是"类 / 等级 / InputID / SourceObject"，不是"类 / 等级 / SourceObject"。
    //
    // 为什么 InputID 传 INDEX_NONE：本项目的行动由 `UBattleWorld::Step` 用
    // `TryActivateAbility(句柄)` 直接驱动，不经过输入绑定（§2.3 第 5 步）；
    // 给一个 InputID 只会让"这个能力能被输入激活"变成一个假象。
    //
    // 为什么 SourceObject 传 `this`：§4.1 把技能内容数据的落点定在
    // "`FGameplayAbilitySpec::SourceObject` 指向的内容资产"上。传 `this`（清单自身）意味着
    // 能力可以经 `GetCurrentAbilitySpec()->SourceObject` 找回自己所属的清单，
    // 从而读到清单上的内容参数；这比"能力自己去数据子系统查表"少一次全局查找。
    //
    // 阶段 3.3：循环体整体搬进静态 `GiveAbilitiesTo`，让"追加授予"通道
    //（`UACAbilitySetComponent::SetExtraAbilities`）复用同一份实现。
    GiveAbilitiesTo(ASC, GrantedAbilities, GrantedAbilityLevels, const_cast<UACAbilitySet*>(this));

    // -----------------------------------------------------------------------
    // ② 常驻效果：`MakeOutgoingSpec` + `ApplyGameplayEffectSpecToSelf`
    // -----------------------------------------------------------------------
    // 关于 `ApplyGameplayEffectToSelf` 的重载（任务书要求先核实，结论如下）：
    //   引擎在 5.6 里提供两个同名函数（AbilitySystemComponent.h）：
    //     · :796 `BP_ApplyGameplayEffectToSelf(TSubclassOf<UGameplayEffect>, float Level,
    //                                          FGameplayEffectContextHandle)` —— 那是
    //       `UFUNCTION(BlueprintCallable)` 的**蓝图包装**，名字带 `BP_` 前缀；
    //     · :797 `ApplyGameplayEffectToSelf(const UGameplayEffect* GameplayEffect, float Level,
    //                                       const FGameplayEffectContextHandle&, FPredictionKey)` ——
    //       C++ 版收的是**已实例化的 GE 对象指针**，不是 `TSubclassOf`。
    //   也就是说 **`TSubclassOf<UGameplayEffect>` 版本的 C++ 重载并不存在**：
    //   直接写 `ApplyGameplayEffectToSelf(EffectClass, 1.f, Context)` 会因为
    //   `TSubclassOf` 无法隐式转成 `const UGameplayEffect*` 而编译失败
    //   （要写得通就得显式写 `EffectClass->GetDefaultObject()`，那是一条"绕开 spec"的路）。
    //
    //   因此本实现选 **`MakeOutgoingSpec` + `ApplyGameplayEffectSpecToSelf`**（任务书推荐的姿势），
    //   理由是它**多给一次拿到 `FGameplayEffectSpec` 的机会**，而这条路径很快就要用到：
    //     ① 强化 / 装备 / 词条的 GE 里已经有一批走 `SetByCaller` 的幅度
    //        （`FACEffectMagnitude::SetByCaller`），必须能在施加前 `SetSetByCallerMagnitude`；
    //     ② §4.4 的状态层数走 `SetStackCount`；
    //     ③ 等级（`MakeOutgoingSpec` 的 Level 形参）将来要接干员等级档位。
    //   走 `ApplyGameplayEffectToSelf(CDO, …)` 这三件事都做不到（它内部才建 spec，调用方拿不到）。
    //   两份实现的行为差异只有一处：`BP_`/`ApplyGameplayEffectToSelf` 会在类为空时打一条
    //   Warning（AbilitySystemComponent.cpp 里 `ABILITY_LOG`），而下面已经先判空了 —— 等价。
    //
    // Level 固定 1：本项目的 GE 数值都是内容常量或 SetByCaller，没有按 GE 等级缩放的幅度
    //（`FGameplayModifierInfo` 的 `FScalableFloat` 在等级 1 时就是原值），
    // 与 `UACBattleAbility::ApplyEffectToUnit` 的口径一致。
    for (const TSubclassOf<UGameplayEffect>& EffectClass : GrantedEffects)
    {
        if (EffectClass.Get() == nullptr)
        {
            continue;
        }

        // context 由**目标自己的 ASC** 生成：`MakeEffectContext` 内部用 OwnerActor/AvatarActor 填
        // instigator（AbilitySystemComponent.cpp），单位 Actor 两者都是自己，因此
        // context 的 instigator 就是本单位 —— 这正是 `AggregateBySource` 状态 GE 需要的
        // "同源才叠加"判据，也是 `UACSummonEffectComponent` 取召唤归属的依据。
        const FGameplayEffectContextHandle Context = ASC.MakeEffectContext();
        const FGameplayEffectSpecHandle SpecHandle = ASC.MakeOutgoingSpec(EffectClass, /*Level=*/1.f, Context);
        if (SpecHandle.IsValid() && SpecHandle.Data.IsValid())
        {
            ASC.ApplyGameplayEffectSpecToSelf(*SpecHandle.Data);
        }
    }
}
