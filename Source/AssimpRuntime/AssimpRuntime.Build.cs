// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

using UnrealBuildTool;

/// <summary>
/// Blueprint and C++ API for loading meshes at runtime, in editor and in packaged builds.
/// </summary>
/// <remarks>
/// Depends on AssimpCore but not on AssimpLibrary: Assimp is an implementation detail of AssimpCore,
/// so this module needs neither Assimp's include paths nor its exceptions/RTTI settings.
/// </remarks>
public class AssimpRuntime : ModuleRules
{
	public AssimpRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"AssimpCore",
				"Core",
				"CoreUObject",
				"Engine",
				"MeshDescription",
				"StaticMeshDescription",

				// UDynamicMesh / UDynamicMeshComponent, the primary runtime spawn path.
				"GeometryFramework",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"GeometryCore",
				"MeshConversion",
				"RenderCore",
				"RHI",

				// Decoding PNG/JPEG texture data at runtime, for both external texture files and
				// compressed payloads embedded in a source file.
				"ImageCore",
				"ImageWrapper",

				// IPluginManager, used by the automation tests to locate Tests/Data.
				"Projects",

				// Legacy spawn path, kept for projects already standardised on it.
				"ProceduralMeshComponent",
			}
		);
	}
}
