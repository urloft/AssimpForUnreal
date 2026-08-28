// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

using UnrealBuildTool;

/// <summary>
/// Worked example of the runtime import API, plus the tests that exercise it end to end.
/// </summary>
/// <remarks>
/// This module is sample content, not part of the plugin's functionality. Nothing in AssimpCore,
/// AssimpRuntime, AssimpInterchange or AssimpEditor depends on it, so it can be deleted outright --
/// remove the directory and its entry from AssimpForUnreal.uplugin.
///
/// It is shipped rather than kept in a separate demo project for two reasons: someone cloning the
/// plugin gets something runnable immediately, and the corpus sweep test lives here. That sweep is
/// what caught the three crashes hand-written fixtures missed, so it belongs with the plugin where
/// it will actually be run after a change to the conversion path.
/// </remarks>
public class AssimpExamples : ModuleRules
{
	public AssimpExamples(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"AssimpCore",
				"AssimpRuntime",
				"Core",
				"CoreUObject",
				"Engine",

				// UDynamicMeshComponent, which the spawn helper returns.
				"GeometryFramework",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// FMaterialRelevance and GMaxRHIFeatureLevel, used by the diagnostic test to report
				// whether a spawned component would actually be drawn.
				"RenderCore",
				"RHI",

				// IPluginManager, for locating Tests/Data.
				"Projects",
			}
		);
	}
}
