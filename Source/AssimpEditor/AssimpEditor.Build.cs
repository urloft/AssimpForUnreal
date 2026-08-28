// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

using UnrealBuildTool;

/// <summary>
/// Editor-only surface: the import pipeline shown in the Interchange import dialog, and project settings.
/// </summary>
public class AssimpEditor : ModuleRules
{
	public AssimpEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"AssimpCore",
				"AssimpInterchange",
				"Core",
				"CoreUObject",
				"Engine",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"DeveloperSettings",
				"Projects",
				"Slate",
				"SlateCore",
				"UnrealEd",
			}
		);
	}
}
