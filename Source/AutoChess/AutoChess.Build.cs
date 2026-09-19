// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class AutoChess : ModuleRules
{
	public AutoChess(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "GameplayTags", "GameplayAbilities", "GameplayTasks", "AutoChessCore", "AutoChessBattle" });

		PrivateDependencyModuleNames.AddRange(new string[] {  });
		
		PrivateIncludePaths.AddRange(new string[]
		{
			"AutoChess/Public",
			"AutoChess/Public/Content",
			"AutoChess/Public/Run",
			"AutoChess/Public/Game"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
