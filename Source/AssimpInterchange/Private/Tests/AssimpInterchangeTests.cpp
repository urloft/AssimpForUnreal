// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpForUnrealSettings.h"
#include "AssimpInterchange.h"
#include "InterchangeAssimpTranslator.h"
#include "InterchangeManager.h"
#include "InterchangeSourceData.h"

#include "Engine/StaticMesh.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

/**
 * End-to-end tests for the Interchange path.
 *
 * The AssimpCore tests already prove the geometry conversion is correct. What these prove is the
 * thing that conversion being correct cannot tell you: that the translator is actually reachable
 * through Interchange, that its node graph is well formed enough for the engine's factories to act
 * on, and that a real UStaticMesh asset comes out the other end. That is the whole claim of the
 * editor import path, and it is not testable at the AssimpCore level.
 */

namespace AssimpInterchangeTestUtils
{
	/** Content path that imported test assets are created under and deleted from. */
	const TCHAR* const TestContentPath = TEXT("/Game/AssimpAutomationTests");

	FString GetTestDataPath(const FString& FileName)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AssimpForUnreal"));
		if (!Plugin.IsValid())
		{
			return FString();
		}

		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests"), TEXT("Data"), FileName);
	}

	/** Imports a file synchronously and returns whatever objects the import produced. */
	bool ImportSynchronously(const FString& FilePath, TArray<UObject*>& OutObjects)
	{
		UInterchangeSourceData* SourceData = UInterchangeManager::CreateSourceData(FilePath);
		if (SourceData == nullptr)
		{
			return false;
		}

		FImportAssetParameters Parameters;

		// Suppresses the import dialog. Without it the import would block forever under -unattended.
		Parameters.bIsAutomated = true;
		Parameters.bReplaceExisting = true;

		return UInterchangeManager::GetInterchangeManager().ImportAsset(
			TestContentPath, SourceData, Parameters, OutObjects);
	}
}

// =================================================================================================
// Translator discovery
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpTranslatorRegistrationTest,
	"AssimpForUnreal.Interchange.TranslatorRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpTranslatorRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	const UAssimpForUnrealSettings* Settings = UAssimpForUnrealSettings::Get();
	if (!TestNotNull(TEXT("Settings CDO available"), Settings))
	{
		return false;
	}

	const TArray<FString> Extensions = Settings->GetEffectiveExtensions();
	TestTrue(TEXT("Some extensions are claimed"), Extensions.Num() > 0);

	// The formats that motivated the plugin: the engine has no translator for any of these.
	for (const TCHAR* Expected : { TEXT("ply"), TEXT("stl"), TEXT("dae"), TEXT("3ds") })
	{
		TestTrue(FString::Printf(TEXT("'%s' is claimed by default"), Expected),
			Extensions.Contains(Expected));
	}

	// Engine-owned formats are gated behind the bClaim* flags. Assert the gating relationship rather
	// than a fixed expectation: these flags are user-editable project settings, so asserting "fbx is
	// absent" fails the moment someone ticks the box in Project Settings -- which is a config choice,
	// not a defect.
	//
	// Note that ticking a claim does not actually hand the format to this translator.
	// UInterchangeManager stores translators in a TSet<TObjectPtr<const UClass>> and
	// GetTranslatorForSourceData iterates it, so when two translators claim one extension the winner
	// is set-iteration order: unspecified, and beyond a plugin's control. There is no API to
	// unregister or deny a translator. The flags exist for source-engine builds where the competing
	// registration can be removed outright.
	const TMap<FString, bool> GatedFormats = {
		{ TEXT("obj"),  Settings->bClaimObj  },
		{ TEXT("fbx"),  Settings->bClaimFbx  },
		{ TEXT("gltf"), Settings->bClaimGltf },
		{ TEXT("glb"),  Settings->bClaimGltf },
	};

	for (const TPair<FString, bool>& Gated : GatedFormats)
	{
		TestEqual(
			FString::Printf(TEXT("'%s' is claimed exactly when its bClaim flag is set"), *Gated.Key),
			Extensions.Contains(Gated.Key),
			Gated.Value);
	}

	// Interchange must be able to see the translator, in the exact "ext;Description" shape it wants.
	const UInterchangeAssimpTranslator* Translator = GetDefault<UInterchangeAssimpTranslator>();
	const TArray<FString> Formats = Translator->GetSupportedFormats();

	TestEqual(TEXT("One format entry per claimed extension"), Formats.Num(), Extensions.Num());

	int32 Malformed = 0;
	for (const FString& Format : Formats)
	{
		if (!Format.Contains(TEXT(";")))
		{
			++Malformed;
		}
	}
	TestEqual(TEXT("Every format entry is 'extension;description'"), Malformed, 0);

	// The translator claims a scene hierarchy and all three asset types it can produce.
	TestTrue(TEXT("Declares scene support"),
		Translator->GetTranslatorType() == EInterchangeTranslatorType::Scenes);

	const EInterchangeTranslatorAssetType AssetTypes = Translator->GetSupportedAssetTypes();
	TestTrue(TEXT("Declares mesh support"),
		EnumHasAnyFlags(AssetTypes, EInterchangeTranslatorAssetType::Meshes));
	TestTrue(TEXT("Declares material support"),
		EnumHasAnyFlags(AssetTypes, EInterchangeTranslatorAssetType::Materials));
	TestTrue(TEXT("Declares texture support"),
		EnumHasAnyFlags(AssetTypes, EInterchangeTranslatorAssetType::Textures));

	return true;
}

// =================================================================================================
// End-to-end import
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpImportStaticMeshTest,
	"AssimpForUnreal.Interchange.ImportStaticMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpImportStaticMeshTest::RunTest(const FString& /*Parameters*/)
{
	using namespace AssimpInterchangeTestUtils;

	const FString FilePath = GetTestDataPath(TEXT("Cube.ply"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	TArray<UObject*> ImportedObjects;
	const bool bImported = ImportSynchronously(FilePath, ImportedObjects);

	if (!TestTrue(TEXT("Interchange reported a successful import"), bImported))
	{
		return false;
	}

	// The headline assertion: a .ply, which no engine translator can read, became a real UStaticMesh
	// asset. Everything Nanite/LOD/collision-related follows from being a genuine asset rather than
	// runtime-generated geometry.
	UStaticMesh* ImportedMesh = nullptr;
	for (UObject* Object : ImportedObjects)
	{
		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object))
		{
			ImportedMesh = StaticMesh;
			break;
		}
	}

	if (!TestNotNull(TEXT("A UStaticMesh asset was created"), ImportedMesh))
	{
		AddError(FString::Printf(TEXT("Import produced %d object(s), none of them a static mesh."),
			ImportedObjects.Num()));
		for (UObject* Object : ImportedObjects)
		{
			if (Object != nullptr)
			{
				AddInfo(FString::Printf(TEXT("  produced: %s (%s)"),
					*Object->GetName(), *Object->GetClass()->GetName()));
			}
		}
		return false;
	}

	AddInfo(FString::Printf(TEXT("Imported '%s' with %d source model(s)."),
		*ImportedMesh->GetName(), ImportedMesh->GetNumSourceModels()));

	TestTrue(TEXT("Static mesh has at least one LOD"), ImportedMesh->GetNumSourceModels() >= 1);
	TestTrue(TEXT("Static mesh has at least one material slot"),
		ImportedMesh->GetStaticMaterials().Num() >= 1);

	// The cube's 12 triangles must survive the round trip through Interchange's factories.
	if (ImportedMesh->GetNumSourceModels() >= 1)
	{
		const FMeshDescription* MeshDescription = ImportedMesh->GetMeshDescription(0);
		if (TestNotNull(TEXT("LOD 0 has a mesh description"), MeshDescription))
		{
			TestEqual(TEXT("Triangle count survived the import"),
				MeshDescription->Triangles().Num(), 12);
			TestEqual(TEXT("Vertex count survived the import"),
				MeshDescription->Vertices().Num(), 8);
		}
	}

	// Clean up so a rerun starts from the same state and the test leaves no assets behind.
	for (UObject* Object : ImportedObjects)
	{
		if (Object != nullptr)
		{
			Object->ClearFlags(RF_Standalone);
			Object->SetFlags(RF_Transient);
			Object->MarkAsGarbage();
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
