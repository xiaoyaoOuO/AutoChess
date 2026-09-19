// AutoChessCore - shared battle types, data definitions and hex geometry.
using UnrealBuildTool;

public class AutoChessCore : ModuleRules
{
    public AutoChessCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "GameplayTags",
            // 阶段 3.3（GAS 重构实施方案 §3.2 表「Core/ACBattleSetup.h」一行）：
            // `FACPlayerUnitSpec` / `FACEnemyUnitSpec` / `FACExternalModifierSpec` /
            // `FACStartingTimelineSpec` / `FACUnitSpawnRequest` 的效果块字段已从 `TArray<FName>`
            // 改为 `TArray<TSubclassOf<UGameplayEffect>>`（另加 `TSubclassOf<UGameplayAbility>` 的
            // 能力授予通道），而这两个类住在 GameplayAbilities 模块里。
            // 本项目其余模块本来就依赖 GAS（AutoChessBattle 的 Public 依赖里已有），但依赖方向是
            // Core ⇐ Battle ⇐ AutoChess，**不能反向传递**，因此这里必须显式声明 ——
            // 这正是 §7 阶段 1 表里"三个 Build.cs 增加 GameplayAbilities + GameplayTasks 依赖"
            // 那条的 Core 部分（阶段 1 只加了 Battle）。
            // 注意依赖方向本身没有变化：Core 仍不引用 AutoChessBattle 的任何类型。
            "GameplayAbilities",
            "GameplayTasks"
        });
    }
}
