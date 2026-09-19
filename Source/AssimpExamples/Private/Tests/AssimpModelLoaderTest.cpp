// Verifies the example runtime loader actually loads and spawns, not merely that it compiles.

#include "AssimpModelLoader.h"

#include "Components/DynamicMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/GarbageCollection.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "SceneManagement.h"
#include "UDynamicMesh.h"

// GetTransientPackage() returns a UPackage*, and passing one where a UObject* is expected needs the
// full type. The editor's unity build supplies it from a neighbouring file; an isolated packaging
// build does not, and the error there is the unhelpful "cannot convert from UPackage* to UObject*".
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpModelLoaderSpawnTest,
	"AssimpForUnreal.Examples.SpawnsComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpModelLoaderSpawnTest::RunTest(const FString& /*Parameters*/)
{
	// A throwaway world, so the test does not depend on any map existing in the project.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
	if (!TestNotNull(TEXT("Test world created"), World))
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);

	bool bResult = true;
	{
		AAssimpModelLoader* Loader = World->SpawnActor<AAssimpModelLoader>();
		if (TestNotNull(TEXT("Loader actor spawned"), Loader))
		{
			// Blocking, so the assertions below can run without waiting on a worker thread.
			Loader->bLoadAsynchronously = false;
			Loader->bMergeMeshesByNode = true;

			// The actor's default FilePath uses FPaths::ProjectPluginsDir(), which resolves through
			// the host project. Set it explicitly so the test does not depend on that layout.
			Loader->FilePath = FPaths::Combine(
				FPaths::ProjectPluginsDir(),
				TEXT("AssimpForUnreal/Tests/Data/Cube.ply"));

			TestTrue(FString::Printf(TEXT("Fixture exists at %s"), *Loader->FilePath),
				FPaths::FileExists(Loader->FilePath));

			Loader->LoadNow();

			TArray<UDynamicMeshComponent*> Components;
			Loader->GetComponents(Components);

			// Cube.ply is a single mesh, so exactly one component is expected.
			TestEqual(TEXT("One dynamic mesh component spawned"), Components.Num(), 1);

			if (Components.Num() == 1 && Components[0] != nullptr)
			{
				const UDynamicMesh* Mesh = Components[0]->GetDynamicMesh();
				if (TestNotNull(TEXT("Component has a dynamic mesh"), Mesh))
				{
					const int32 TriangleCount = Mesh->GetMeshRef().TriangleCount();
					const int32 VertexCount = Mesh->GetMeshRef().VertexCount();

					AddInfo(FString::Printf(TEXT("Spawned mesh: %d triangles, %d vertices"),
						TriangleCount, VertexCount));

					TestEqual(TEXT("Cube has 12 triangles"), TriangleCount, 12);
					TestEqual(TEXT("Cube has 8 vertices"), VertexCount, 8);
				}
			}
		}
		else
		{
			bResult = false;
		}
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);

	return bResult;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS

#include "AssimpBlueprintLibrary.h"
#include "AssimpCore.h"
#include "AssimpSceneObject.h"
#include "AssimpSceneTypes.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpDiagnoseModelTest,
	"AssimpForUnreal.Examples.DiagnoseModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Diagnostic: loads a model from disk and reports everything that governs whether it is visible --
 * applied scale, per-mesh bounds, component transforms, and material counts. Skips cleanly when the
 * file is absent so it does not fail on other machines.
 */
bool FAssimpDiagnoseModelTest::RunTest(const FString& /*Parameters*/)
{
	// Point ASSIMP_DIAG_MODEL at any model to diagnose it; otherwise the plugin's own fixture is
	// used so this test is meaningful on any machine rather than depending on one developer's
	// Downloads folder.
	FString Target = FPlatformMisc::GetEnvironmentVariable(TEXT("ASSIMP_DIAG_MODEL"));
	if (Target.IsEmpty())
	{
		Target = FPaths::Combine(
			FPaths::ProjectPluginsDir(), TEXT("AssimpForUnreal/Tests/Data/SkinnedQuad.gltf"));
	}

	if (!FPaths::FileExists(Target))
	{
		AddInfo(FString::Printf(TEXT("SKIPPED: '%s' not present."), *Target));
		return true;
	}

	AddInfo(FString::Printf(TEXT("DIAG target='%s'"), *Target));

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);

	AAssimpModelLoader* Loader = World->SpawnActor<AAssimpModelLoader>();
	Loader->bLoadAsynchronously = false;
	Loader->FilePath = Target;
	Loader->LoadNow();

	TArray<UDynamicMeshComponent*> Components;
	Loader->GetComponents(Components);
	AddInfo(FString::Printf(TEXT("DIAG components=%d"), Components.Num()));

	FBox Combined(ForceInit);
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		UDynamicMeshComponent* Component = Components[Index];
		if (Component == nullptr)
		{
			continue;
		}

		const UDynamicMesh* Mesh = Component->GetDynamicMesh();
		const int32 Tris = Mesh ? Mesh->GetMeshRef().TriangleCount() : -1;
		const FBoxSphereBounds Bounds = Component->Bounds;
		Combined += Bounds.GetBox();

		if (Index < 8)
		{
			// Material relevance is the signal that actually decides whether the renderer draws
			// this primitive: with zero material slots it comes back empty and the proxy gets no
			// pass flags, so the geometry is invisible regardless of bounds or visibility flags.
			const FMaterialRelevance Relevance =
				Component->GetMaterialRelevance(GMaxRHIFeatureLevel);

			AddInfo(FString::Printf(
				TEXT("DIAG comp[%d] tris=%d registered=%d visible=%d materials=%d ")
				TEXT("relevance{opaque=%d,masked=%d,translucent=%d,anyPass=%d} ")
				TEXT("relLoc=%s relScale=%s boundsOrigin=%s boundsExtent=%s"),
				Index, Tris,
				Component->IsRegistered() ? 1 : 0,
				Component->IsVisible() ? 1 : 0,
				Component->GetNumMaterials(),
				Relevance.bOpaque ? 1 : 0,
				Relevance.bMasked ? 1 : 0,
				Relevance.bNormalTranslucency ? 1 : 0,
				(Relevance.bOpaque || Relevance.bMasked || Relevance.bNormalTranslucency) ? 1 : 0,
				*Component->GetRelativeLocation().ToCompactString(),
				*Component->GetRelativeScale3D().ToCompactString(),
				*Bounds.Origin.ToCompactString(),
				*Bounds.BoxExtent.ToCompactString()));
		}
	}

	AddInfo(FString::Printf(TEXT("DIAG combinedBounds min=%s max=%s"),
		*Combined.Min.ToCompactString(), *Combined.Max.ToCompactString()));

	// Textures: confirm the recorded relative paths resolve against the model's directory and that
	// the images actually decode into usable textures.
	if (UAssimpSceneObject* Scene = Loader->GetLoadedScene())
	{
		const FAssimpSceneInfo& Info = Scene->GetSceneInfo();

		int32 SlotsDeclared = 0;
		int32 SlotsResolved = 0;
		int32 FirstFailures = 0;

		for (int32 MatIndex = 0; MatIndex < Info.Materials.Num(); ++MatIndex)
		{
			for (const TPair<EAssimpTextureSlot, FAssimpTextureReference>& Pair :
				Info.Materials[MatIndex].Textures)
			{
				++SlotsDeclared;

				if (UTexture2D* Texture = Scene->CreateTextureForMaterialSlot(MatIndex, Pair.Key))
				{
					++SlotsResolved;
					if (SlotsResolved <= 4)
					{
						AddInfo(FString::Printf(
							TEXT("DIAG tex mat=%d slot=%d -> %dx%d sRGB=%d src='%s'"),
							MatIndex, (int32)Pair.Key,
							Texture->GetSizeX(), Texture->GetSizeY(),
							Texture->SRGB ? 1 : 0,
							*Pair.Value.Path));
					}
				}
				else if (++FirstFailures <= 4)
				{
					AddInfo(FString::Printf(TEXT("DIAG tex FAILED mat=%d slot=%d path='%s' embedded=%d"),
						MatIndex, (int32)Pair.Key, *Pair.Value.Path,
						Pair.Value.Source == EAssimpTextureSource::Embedded ? 1 : 0));
				}
			}
		}

		AddInfo(FString::Printf(TEXT("DIAG textures declared=%d resolved=%d materials=%d"),
			SlotsDeclared, SlotsResolved, Info.Materials.Num()));

		// Opacity / translucency drive UseOpacityMask, which can mask geometry away entirely, so
		// report what the file actually claimed.
		for (int32 MatIndex = 0; MatIndex < FMath::Min(Info.Materials.Num(), 6); ++MatIndex)
		{
			const FAssimpMaterialInfo& Mat = Info.Materials[MatIndex];
			AddInfo(FString::Printf(
				TEXT("DIAG mat[%d] '%s' opacity=%.3f translucent=%d twoSided=%d base=%s slots=%d"),
				MatIndex, *Mat.Name, Mat.Opacity, Mat.bIsTranslucent ? 1 : 0,
				Mat.bTwoSided ? 1 : 0, *Mat.BaseColor.ToString(), Mat.Textures.Num()));
		}

		// Assert only that nothing declared failed to load. Requiring SlotsDeclared > 0 would make
		// this test demand a textured model, which the default fixture is not.
		TestEqual(TEXT("No declared texture slot failed to resolve"), SlotsResolved, SlotsDeclared);
	}

	// The plugin's generated material must be reachable, and it must actually be the one bound to
	// the spawned components -- otherwise textures have nowhere to go and the model renders grey.
	UMaterialInterface* PluginDefault = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/AssimpForUnreal/M_AssimpDefault.M_AssimpDefault"));

	if (TestNotNull(TEXT("Plugin default material loads"), PluginDefault))
	{
		TArray<FMaterialParameterInfo> TextureParams;
		TArray<FGuid> TextureGuids;
		PluginDefault->GetAllTextureParameterInfo(TextureParams, TextureGuids);

		TArray<FString> Names;
		for (const FMaterialParameterInfo& Param : TextureParams)
		{
			Names.Add(Param.Name.ToString());
		}
		Names.Sort();
		AddInfo(FString::Printf(TEXT("DIAG default material texture params: %s"),
			*FString::Join(Names, TEXT(", "))));

		for (const TCHAR* Required : { TEXT("BaseColorTexture"), TEXT("NormalTexture"),
			TEXT("RoughnessTexture"), TEXT("MetallicTexture"), TEXT("SpecularTexture") })
		{
			TestTrue(FString::Printf(TEXT("Default material exposes '%s'"), Required),
				Names.Contains(Required));
		}

		// The Use...Texture scalars are what stop an unbound sampler contributing its default
		// texture. Losing them is not a compile error and not visible in any texture-parameter
		// check -- it just silently washes every model out with white emissive again, so assert
		// they survive regeneration.
		TArray<FMaterialParameterInfo> ScalarParams;
		TArray<FGuid> ScalarGuids;
		PluginDefault->GetAllScalarParameterInfo(ScalarParams, ScalarGuids);

		TArray<FString> ScalarNames;
		for (const FMaterialParameterInfo& Param : ScalarParams)
		{
			ScalarNames.Add(Param.Name.ToString());
		}
		ScalarNames.Sort();
		AddInfo(FString::Printf(TEXT("DIAG default material scalars: %s"),
			*FString::Join(ScalarNames, TEXT(", "))));

		for (const TCHAR* Required : { TEXT("UseBaseColorTexture"), TEXT("UseRoughnessTexture"),
			TEXT("UseMetallicTexture"), TEXT("UseEmissiveTexture"), TEXT("UseOcclusionTexture"),
			TEXT("UseSpecularTexture"), TEXT("UseOpacityMask") })
		{
			TestTrue(FString::Printf(TEXT("Default material exposes scalar '%s'"), Required),
				ScalarNames.Contains(Required));
		}

		// Specular has to be a parameter, and it has to default to Unreal's neutral 0.5. A material
		// regenerated with it defaulting to 0 or 1 would change the look of every model that says
		// nothing about specularity, which is most of them -- and nothing else here would notice.
		for (const TCHAR* Required : { TEXT("Specular"), TEXT("Roughness"), TEXT("Metallic") })
		{
			TestTrue(FString::Printf(TEXT("Default material exposes scalar '%s'"), Required),
				ScalarNames.Contains(Required));
		}

		float DefaultSpecular = -1.0f;
		if (PluginDefault->GetScalarParameterValue(
				FMaterialParameterInfo(TEXT("Specular")), DefaultSpecular))
		{
			TestTrue(FString::Printf(TEXT("Specular defaults to Unreal's neutral 0.5 (got %.4f)"),
					DefaultSpecular),
				FMath::IsNearlyEqual(DefaultSpecular, 0.5f, 0.001f));
		}

		int32 BoundToInstance = 0;
		for (UDynamicMeshComponent* Component : Components)
		{
			if (Component == nullptr || Component->GetNumMaterials() == 0)
			{
				continue;
			}

			// A dynamic instance whose parent is our material means the texture parameters written
			// in AssignMaterialSlots actually landed somewhere they can be sampled.
			if (UMaterialInstanceDynamic* Instance =
				Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0)))
			{
				if (Instance->Parent == PluginDefault)
				{
					++BoundToInstance;
				}
			}
		}

		AddInfo(FString::Printf(TEXT("DIAG components bound to a plugin-material instance: %d/%d"),
			BoundToInstance, Components.Num()));

		TestEqual(TEXT("Every component uses an instance of the plugin material"),
			BoundToInstance, Components.Num());
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpCorpusSweepTest,
	"AssimpForUnreal.Corpus.Sweep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Loads every model in a directory tree and reports what happened to each.
 *
 * Aimed at Assimp's own test corpus (test/models), which is the broadest real-world sample of the
 * formats this plugin claims. Point ASSIMP_CORPUS_DIR at it; the test skips itself when unset, so it
 * costs nothing in a normal run.
 *
 * Deliberately reports rather than asserting every file must load: the corpus contains intentionally
 * malformed files, formats built on external references that are not present, and a few formats
 * Assimp itself cannot read in this build configuration. A blanket "all must pass" would be a test
 * that is permanently red and therefore ignored. What IS asserted is that the sweep completes -- no
 * crash, no hang -- and that a floor proportion of files convert, which is what would catch a
 * regression in the conversion path.
 */
bool FAssimpCorpusSweepTest::RunTest(const FString& /*Parameters*/)
{
	const FString Root = FPlatformMisc::GetEnvironmentVariable(TEXT("ASSIMP_CORPUS_DIR"));
	if (Root.IsEmpty() || !FPaths::DirectoryExists(Root))
	{
		AddInfo(TEXT("SKIPPED: set ASSIMP_CORPUS_DIR to a directory of models."));
		return true;
	}

	// Directories holding fixtures that are not models, or are deliberately broken.
	const TArray<FString> SkipDirectories = {
		TEXT("/invalid"), TEXT("/fuzzer_data"), TEXT("/ReferenceImages"),
		TEXT("/SourceFiles"), TEXT("/ParsingFiles")
	};

	// Extensions that are companion data rather than models.
	const TSet<FString> SkipExtensions = {
		TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("gif"), TEXT("tga"), TEXT("bmp"), TEXT("dds"),
		TEXT("txt"), TEXT("md"), TEXT("mtl"), TEXT("glsl"), TEXT("xml"), TEXT("json"), TEXT("bin"),
		TEXT("material"), TEXT("skeleton"), TEXT("cfg"), TEXT("ini"), TEXT("log"), TEXT("pdf")
	};

	// The sweep deliberately feeds Assimp malformed and unsupported files, and the plugin logs an
	// error for each one it cannot read. UE's automation framework promotes any logged error to a
	// test failure, so those have to be kept out of the framework's view -- otherwise this test can
	// only pass on a corpus containing no bad files, which defeats its purpose.
	//
	// Silencing the category beats AddExpectedError here: that API requires every declared pattern
	// to ACTUALLY occur (Occurrences = 0 means "at least one", not "any number"), so declaring the
	// full set of possible failures fails the test whenever a corpus happens not to trigger one of
	// them. Failure detail is collected from FAssimpRuntimeImportResult::ErrorMessage regardless, so
	// nothing is lost by muting the log.
	struct FScopedLogSilence
	{
		ELogVerbosity::Type Previous;

		FScopedLogSilence()
			: Previous(LogAssimp.GetVerbosity())
		{
			// ASSIMP_SWEEP_VERBOSE=1 keeps the log on. Silencing hides which file the sweep is
			// working on, which is exactly what you need if a file kills the process outright --
			// AddInfo output is only flushed when the test completes, so a hard exit loses it all.
			if (FPlatformMisc::GetEnvironmentVariable(TEXT("ASSIMP_SWEEP_VERBOSE")).IsEmpty())
			{
				LogAssimp.SetVerbosity(ELogVerbosity::Fatal);
			}
		}

		~FScopedLogSilence()
		{
			LogAssimp.SetVerbosity(Previous);
		}
	} LogSilence;

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.*"), true, false);
	Files.Sort();

	int32 Considered = 0, Loaded = 0, Failed = 0, Unsupported = 0, Skipped = 0, Converted = 0;
	TArray<FString> Failures;
	TArray<FString> SuspiciousOpacity;

	for (const FString& File : Files)
	{
		FString Normalised = File;
		Normalised.ReplaceInline(TEXT("\\"), TEXT("/"));

		bool bInSkippedDirectory = false;
		for (const FString& Skip : SkipDirectories)
		{
			if (Normalised.Contains(Skip))
			{
				bInSkippedDirectory = true;
				break;
			}
		}
		if (bInSkippedDirectory)
		{
			++Skipped;
			continue;
		}

		const FString Extension = FPaths::GetExtension(Normalised).ToLower();
		if (Extension.IsEmpty() || SkipExtensions.Contains(Extension))
		{
			++Skipped;
			continue;
		}

		if (!UAssimpBlueprintLibrary::IsExtensionSupported(Extension))
		{
			++Unsupported;
			continue;
		}

		// Half-Life MDL ships animation SEQUENCE GROUP files alongside the model, sharing the .mdl
		// extension. They are ~92-byte companions holding no geometry, and Assimp rightly refuses
		// them ("Impossible to properly load a model from an MDL sequence file"). Counting them as
		// failures made 19 non-models a quarter of this sweep's failure total.
		//
		// Told apart by magic, not by filename: "IDST" is a studio model, "IDSQ" a sequence group.
		if (Extension == TEXT("mdl"))
		{
			TArray<uint8> Magic;
			if (FFileHelper::LoadFileToArray(Magic, *Normalised) && Magic.Num() >= 4)
			{
				const bool bIsSequenceGroup =
					Magic[0] == 'I' && Magic[1] == 'D' && Magic[2] == 'S' && Magic[3] == 'Q';
				if (bIsSequenceGroup)
				{
					++Skipped;
					continue;
				}
			}
		}

		++Considered;

		const FString Relative = Normalised.RightChop(Root.Len() + 1);

		FAssimpImportSettings Settings;

		// ASSIMP_SWEEP_NOVALIDATE=1 disables our structural validation, to measure how many of the
		// "Validation failed" rejections are the validator's doing rather than the file's.
		if (!FPlatformMisc::GetEnvironmentVariable(TEXT("ASSIMP_SWEEP_NOVALIDATE")).IsEmpty())
		{
			Settings.bValidateSceneStructure = false;
		}

		FAssimpRuntimeImportResult Result;
		UAssimpSceneObject* Scene = UAssimpBlueprintLibrary::ImportSceneFromFile(
			GetTransientPackage(), Normalised, Settings, Result);

		if (!Result.bSucceeded || Scene == nullptr || !Scene->IsValidScene())
		{
			++Failed;
			Failures.Add(FString::Printf(TEXT("%s | %s"), *Relative, *Result.ErrorMessage));
			continue;
		}

		++Loaded;

		const FAssimpSceneInfo& Info = Scene->GetSceneInfo();

		int32 TotalTris = 0;
		FBox Bounds(ForceInit);
		for (const FAssimpMeshInfo& Mesh : Info.Meshes)
		{
			TotalTris += Mesh.NumTriangles;
			if (Mesh.BoundingBox.IsValid)
			{
				Bounds += Mesh.BoundingBox;
			}
		}

		// Verify the geometry actually converts, not merely that the file parsed.
		bool bConverted = false;
		if (Info.Meshes.Num() > 0)
		{
			UDynamicMesh* Probe = NewObject<UDynamicMesh>(GetTransientPackage());
			bConverted = Scene->BuildDynamicMesh(0, Probe);
			if (bConverted)
			{
				++Converted;
			}
		}

		// An opacity of exactly 0 is nearly always a format quirk rather than intent, and it drives
		// UseOpacityMask -- which can mask a model away entirely. Worth surfacing.
		for (const FAssimpMaterialInfo& Mat : Info.Materials)
		{
			if (Mat.Opacity <= 0.0f)
			{
				SuspiciousOpacity.AddUnique(FString::Printf(
					TEXT("%s | material '%s' opacity=%.3f"), *Relative, *Mat.Name, Mat.Opacity));
				break;
			}
		}

		AddInfo(FString::Printf(
			TEXT("SWEEP OK   %-46s meshes=%-4d tris=%-7d mats=%-3d anims=%-3d conv=%d size=%s"),
			*Relative, Info.Meshes.Num(), TotalTris, Info.Materials.Num(),
			Info.Animations.Num(), bConverted ? 1 : 0,
			Bounds.IsValid ? *Bounds.GetSize().ToCompactString() : TEXT("n/a")));

	}

	for (const FString& Failure : Failures)
	{
		AddInfo(FString::Printf(TEXT("SWEEP FAIL %s"), *Failure));
	}
	for (const FString& Odd : SuspiciousOpacity)
	{
		AddInfo(FString::Printf(TEXT("SWEEP ZEROOPACITY %s"), *Odd));
	}

	AddInfo(FString::Printf(
		TEXT("SWEEP TOTALS considered=%d loaded=%d failed=%d converted=%d unsupportedExt=%d skipped=%d"),
		Considered, Loaded, Failed, Converted, Unsupported, Skipped));

	TestTrue(TEXT("The sweep found files to consider"), Considered > 0);

	// A floor rather than a fixed number: the corpus legitimately contains files this build cannot
	// read, but a conversion-path regression would drop this sharply.
	if (Considered > 0)
	{
		const float LoadRate = static_cast<float>(Loaded) / static_cast<float>(Considered);
		AddInfo(FString::Printf(TEXT("SWEEP loadRate=%.1f%%"), LoadRate * 100.0f));
		TestTrue(FString::Printf(TEXT("At least 70%% of considered files load (got %.1f%%)"),
			LoadRate * 100.0f), LoadRate >= 0.70f);
	}

	return true;
}

#endif
