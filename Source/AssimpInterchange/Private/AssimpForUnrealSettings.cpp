// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpForUnrealSettings.h"

#define LOCTEXT_NAMESPACE "AssimpForUnrealSettings"

UAssimpForUnrealSettings::UAssimpForUnrealSettings()
{
	// Formats with no engine translator, so Assimp is the only way to read them. Grouped by family
	// to keep the list reviewable.
	SupportedExtensions = {
		// Widely used interchange and scan formats.
		TEXT("ply"), TEXT("stl"), TEXT("dae"), TEXT("zae"), TEXT("3ds"), TEXT("3mf"), TEXT("off"),

		// DCC and engine-native formats.
		TEXT("blend"),                                  // Blender; best effort, see the README.
		TEXT("ase"), TEXT("ask"),                       // 3ds Max ASCII scene export.
		TEXT("lwo"), TEXT("lws"), TEXT("lxo"),          // LightWave and Modo.
		TEXT("ms3d"),                                   // Milkshape 3D.
		TEXT("ac"), TEXT("ac3d"), TEXT("acc"),          // AC3D.
		TEXT("cob"), TEXT("scn"),                       // TrueSpace.
		TEXT("sib"),                                    // Silo.
		TEXT("ndo"),                                    // Nendo.
		TEXT("ogex"),                                   // Open Game Engine Exchange.
		TEXT("x"), TEXT("x3d"), TEXT("x3db"),           // DirectX and X3D.
		TEXT("xgl"), TEXT("zgl"),                       // XGL.
		TEXT("pmx"),                                    // MikuMikuDance.
		TEXT("irr"), TEXT("irrmesh"),                   // Irrlicht.
		TEXT("q3o"), TEXT("q3s"),                       // Quick3D.

		// Game and engine model formats.
		TEXT("md2"), TEXT("md3"), TEXT("md5mesh"),      // Quake and Doom 3.
		TEXT("mdc"), TEXT("mdl"),                       // Return to Castle Wolfenstein, Quake 1.
		TEXT("smd"), TEXT("vta"),                       // Valve.
		TEXT("b3d"),                                    // Blitz3D.
		TEXT("iqm"),                                    // Inter-Quake Model.

		// Animation-only formats.
		TEXT("bvh"),                                    // Biovision motion capture.
		TEXT("csm"),                                    // CharacterStudio motion.

		// CAD and architectural.
		TEXT("ifc"), TEXT("ifczip"),                    // Industry Foundation Classes; best effort.
		TEXT("dxf"),                                    // AutoCAD DXF.

		// Terrain and misc.
		TEXT("hmp"), TEXT("ter"), TEXT("raw"),
		TEXT("nff"), TEXT("enff"),
		TEXT("3d"), TEXT("uc"),
	};
}

FName UAssimpForUnrealSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

const UAssimpForUnrealSettings* UAssimpForUnrealSettings::Get()
{
	return GetDefault<UAssimpForUnrealSettings>();
}

TArray<FString> UAssimpForUnrealSettings::GetEffectiveExtensions() const
{
	// A set first, so a duplicate between the configured list and an opt-in flag cannot register the
	// same extension twice.
	TSet<FString> Unique;
	Unique.Reserve(SupportedExtensions.Num() + 4);

	for (const FString& Extension : SupportedExtensions)
	{
		FString Normalised = Extension.TrimStartAndEnd().ToLower();
		Normalised.RemoveFromStart(TEXT("."));

		if (!Normalised.IsEmpty())
		{
			Unique.Add(MoveTemp(Normalised));
		}
	}

	if (bClaimObj)
	{
		Unique.Add(TEXT("obj"));
	}
	if (bClaimFbx)
	{
		Unique.Add(TEXT("fbx"));
	}
	if (bClaimGltf)
	{
		Unique.Add(TEXT("gltf"));
		Unique.Add(TEXT("glb"));
	}

	TArray<FString> Result = Unique.Array();
	Result.Sort();
	return Result;
}

TArray<FString> UAssimpForUnrealSettings::GetIneffectiveClaimedExtensions() const
{
	// Extensions the engine's own translators register for during InterchangeImport's PreDefault
	// startup. Because UInterchangeManager resolves a translator by registration order and this
	// plugin loads at PostEngineInit, anything in this set that we also claim is inert -- so it is
	// worth telling the user rather than letting them conclude the plugin is broken.
	static const TSet<FString> EngineOwnedExtensions = {
		TEXT("fbx"), TEXT("obj"), TEXT("gltf"), TEXT("glb"),
		TEXT("usd"), TEXT("usda"), TEXT("usdc"), TEXT("usdz"),
		TEXT("abc"),
	};

	TArray<FString> Conflicts;
	for (const FString& Extension : GetEffectiveExtensions())
	{
		if (EngineOwnedExtensions.Contains(Extension))
		{
			Conflicts.Add(Extension);
		}
	}

	return Conflicts;
}

#undef LOCTEXT_NAMESPACE
