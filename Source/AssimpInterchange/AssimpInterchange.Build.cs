// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

using UnrealBuildTool;

/// <summary>
/// Interchange translator that turns Assimp-readable files into real Unreal assets.
/// </summary>
/// <remarks>
/// This is the capability that distinguishes the plugin from a pure runtime loader: by translating
/// into Interchange's node graph, imports become genuine UStaticMesh / USkeletalMesh / UMaterial /
/// UTexture assets and inherit Nanite, LOD generation, collision building, and reimport from the
/// engine's existing factories and pipelines rather than reimplementing any of it.
///
/// Module type is Runtime (matching the engine's own InterchangeImport) because Interchange
/// translators are runtime objects; the editor-only pieces live in AssimpEditor.
/// </remarks>
public class AssimpInterchange : ModuleRules
{
	public AssimpInterchange(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"AssimpCore",
				"Core",
				"CoreUObject",
				"Engine",

				"InterchangeCore",
				"InterchangeEngine",
				"InterchangeNodes",
				"InterchangeFactoryNodes",
				"InterchangeImport",

				"MeshDescription",
				"StaticMeshDescription",

				// UAssimpForUnrealSettings derives from UDeveloperSettings in a public header,
				// so this has to be public rather than private.
				"DeveloperSettings",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"InterchangeCommon",
				"InterchangeMessages",
				"SkeletalMeshDescription",

				// IPluginManager, used to locate the plugin's own Tests/Data directory.
				"Projects",

				// Decoding textures embedded inside a source file. External textures are delegated
				// to the engine's own image translators instead, so this covers only the embedded case.
				"ImageCore",
				"ImageWrapper",
				"TextureUtilitiesCommon",
			}
		);
	}
}
