// AutoChessBattle - battle kernel (logic systems M01-M15) + presentation bridge (M17 logic side).
using UnrealBuildTool;

public class AutoChessBattle : ModuleRules
{
    public AutoChessBattle(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "GameplayTags",
            // 阶段 1（GAS 重构实施方案 §7 阶段 1）：GAS 地基。
            // 为什么要显式列 GameplayTasks：UAbilitySystemComponent 继承自 UGameplayTasksComponent
            // （AbilitySystemComponent.h:109），用到 ASC 的头文件就会传递性地需要这个模块。
            "GameplayAbilities",
            "GameplayTasks",
            "AutoChessCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            // 结构化日志落盘（Diagnostics/ACBattleLogWriter）：用 FJsonSerializer/TJsonWriter 写 JSONL，
            // 用 FJsonObjectConverter 把 FACBattleStatsSnapshot（USTRUCT）转成 JSON 对象。
            "Json",
            "JsonUtilities"
        });
    }
}
