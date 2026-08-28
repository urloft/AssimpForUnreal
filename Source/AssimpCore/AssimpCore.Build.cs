// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

using UnrealBuildTool;

/// <summary>
/// Assimp-facing core: scene ownership, IO/log/progress bridges, and conversion to FMeshDescription.
/// </summary>
/// <remarks>
/// This is the only module in the plugin that is permitted to include an Assimp header, and it may
/// only do so from <c>Private/</c>. Nothing in <c>Public/</c> mentions an Assimp type, so consumers
/// (AssimpRuntime, AssimpInterchange, and any downstream game module) never inherit Assimp's include
/// paths, its exception/RTTI requirements, or its <c>Windows.h</c> macro pollution.
///
/// The conversion to <c>FMeshDescription</c> lives here precisely so that both consumers share it:
/// AssimpInterchange hands the result straight to Interchange as mesh payload data, and AssimpRuntime
/// converts it to an <c>FDynamicMesh3</c>. There is exactly one Assimp-to-Unreal geometry path.
/// </remarks>
public class AssimpCore : ModuleRules
{
	public AssimpCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Assimp is built with exceptions and RTTI enabled, and its public headers declare classes
		// with virtual destructors that we derive from (IOSystem, ProgressHandler, LogStream).
		// Both default to false in Unreal, so they must be opted into here. They are scoped to this
		// module only -- one more reason to keep Assimp from leaking outward.
		bEnableExceptions = true;
		bUseRTTI = true;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"MeshDescription",
				"StaticMeshDescription",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// Assimp itself. Private, so it is not re-exported to our consumers.
				"AssimpLibrary",

				// Locating the plugin's Binaries directory to resolve the delay-loaded DLL.
				"Projects",

				// Skeletal mesh attributes for skin weights (Phase 5).
				"SkeletalMeshDescription",

				// FMeshDescription <-> FDynamicMesh3 conversion helpers used by AssimpRuntime.
				"GeometryCore",
				"MeshConversion",
			}
		);
	}
}
