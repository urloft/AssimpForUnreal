// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpForUnrealSettings.h"
#include "AssimpInterchange.h"
#include "InterchangeAssimpTranslator.h"
#include "InterchangeManager.h"
#include "InterchangeSourceData.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
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
	TestTrue(TEXT("Declares animation support"),
		EnumHasAnyFlags(AssetTypes, EInterchangeTranslatorAssetType::Animations));

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

// =================================================================================================
// Skeletal mesh and animation
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpImportSkeletalAnimationTest,
	"AssimpForUnreal.Interchange.ImportSkeletalAnimation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Asserts that a skinned, animated file imports as a skeletal mesh, a skeleton and an animation.
 *
 * This is the assertion the AssimpCore tests cannot make. They prove the bones, weights and baked
 * transforms are correct as data; what they cannot show is that the node graph the translator emits
 * is one Interchange's own pipelines will act on. Animation in particular has several ways to
 * produce silence rather than an error: a bone emitted as a plain scene node rather than a joint
 * yields no skeleton, and therefore no skeletal mesh and no clip; a track set naming a skeleton by
 * anything other than the root joint's node finds no skeleton factory node and is dropped. Both
 * leave a perfectly successful import that simply has no animation in it, which is why the presence
 * of the UAnimSequence has to be asserted end to end.
 */
bool FAssimpImportSkeletalAnimationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace AssimpInterchangeTestUtils;

	// Collada, not the glTF fixture of the same scene. Interchange resolves a translator by
	// iterating a set of registered classes, so where the engine also claims an extension the winner
	// is unspecified -- a .gltf import may well be handled by the engine's own glTF translator and
	// would prove nothing about this one. No engine translator reads .dae.
	const FString FilePath = GetTestDataPath(TEXT("SkinnedQuad.dae"));
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

	USkeletalMesh* ImportedMesh = nullptr;
	USkeleton* ImportedSkeleton = nullptr;
	UAnimSequence* ImportedAnimation = nullptr;

	for (UObject* Object : ImportedObjects)
	{
		if (Object == nullptr)
		{
			continue;
		}

		AddInfo(FString::Printf(TEXT("produced: %s (%s)"),
			*Object->GetName(), *Object->GetClass()->GetName()));

		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object))
		{
			ImportedMesh = SkeletalMesh;
		}
		else if (USkeleton* Skeleton = Cast<USkeleton>(Object))
		{
			ImportedSkeleton = Skeleton;
		}
		else if (UAnimSequence* AnimSequence = Cast<UAnimSequence>(Object))
		{
			ImportedAnimation = AnimSequence;
		}
	}

	/** Leaves no assets behind, so a rerun starts from the same state. */
	auto Cleanup = [&ImportedObjects]()
	{
		for (UObject* Object : ImportedObjects)
		{
			if (Object != nullptr)
			{
				Object->ClearFlags(RF_Standalone);
				Object->SetFlags(RF_Transient);
				Object->MarkAsGarbage();
			}
		}
	};

	if (!TestNotNull(TEXT("A USkeletalMesh asset was created"), ImportedMesh) ||
		!TestNotNull(TEXT("A USkeleton asset was created"), ImportedSkeleton))
	{
		Cleanup();
		return false;
	}

	// The skeleton must be the one reconstructed from the file, not a placeholder: two bones, the
	// root first.
	const FReferenceSkeleton& ReferenceSkeleton = ImportedSkeleton->GetReferenceSkeleton();
	TestEqual(TEXT("Skeleton has both bones"), ReferenceSkeleton.GetNum(), 2);

	if (ReferenceSkeleton.GetNum() == 2)
	{
		TestEqual(TEXT("Root is the first bone"),
			ReferenceSkeleton.GetBoneName(0), FName(TEXT("Root")));
		TestEqual(TEXT("Bone1 is the second bone"),
			ReferenceSkeleton.GetBoneName(1), FName(TEXT("Bone1")));
		TestEqual(TEXT("Bone1's parent is the root"), ReferenceSkeleton.GetParentIndex(1), 0);
	}

	// The mesh must actually be bound to that skeleton; a skeletal mesh pointing at a different one
	// renders as a T-pose statue no matter how good the animation is.
	TestEqual(TEXT("Skeletal mesh is bound to the imported skeleton"),
		ImportedMesh->GetSkeleton(), ImportedSkeleton);

	if (!TestNotNull(TEXT("A UAnimSequence asset was created"), ImportedAnimation))
	{
		AddError(FString::Printf(
			TEXT("Import produced %d object(s) but no animation sequence. The file's one clip ")
			TEXT("either never reached a track set or the track set found no skeleton."),
			ImportedObjects.Num()));
		Cleanup();
		return false;
	}

	AddInfo(FString::Printf(TEXT("Animation '%s': %.4f seconds, %d frame(s), %d bone track(s)"),
		*ImportedAnimation->GetName(),
		ImportedAnimation->GetPlayLength(),
		ImportedAnimation->GetNumberOfSampledKeys(),
		ImportedAnimation->GetDataModel()->GetNumBoneTracks()));

	TestEqual(TEXT("Animation is bound to the imported skeleton"),
		ImportedAnimation->GetSkeleton(), ImportedSkeleton);

	// One second of motion. A clip that arrived empty, or whose length came from mistaking glTF's
	// millisecond timebase for a frame rate, would not land here.
	TestTrue(FString::Printf(TEXT("Animation is about one second long (got %.4f)"),
			ImportedAnimation->GetPlayLength()),
		FMath::IsNearlyEqual(ImportedAnimation->GetPlayLength(), 1.0f, 0.05f));

	TestTrue(TEXT("Animation has keys"), ImportedAnimation->GetNumberOfSampledKeys() > 1);

	// Only Bone1 is animated in the fixture, but the factory writes a track for every bone of the
	// skeleton, so what matters is that Bone1's is among them and that it moves.
	const IAnimationDataModel* DataModel = ImportedAnimation->GetDataModel();
	if (!TestNotNull(TEXT("Animation has a data model"), DataModel))
	{
		Cleanup();
		return false;
	}

	const bool bHasBoneTrack = DataModel->IsValidBoneTrackName(FName(TEXT("Bone1")));
	if (TestTrue(TEXT("Bone1 has an animation track"), bHasBoneTrack))
	{
		TArray<FTransform> BoneTransforms;
		DataModel->GetBoneTrackTransforms(FName(TEXT("Bone1")), BoneTransforms);

		if (TestTrue(TEXT("Bone1's track has keys"), BoneTransforms.Num() > 1))
		{
			const FVector FirstTranslation = BoneTransforms[0].GetTranslation();
			const FVector LastTranslation = BoneTransforms.Last().GetTranslation();

			AddInfo(FString::Printf(TEXT("Bone1 moves from %s to %s over %d key(s)"),
				*FirstTranslation.ToString(), *LastTranslation.ToString(), BoneTransforms.Num()));

			// The fixture slides Bone1 two source units along source X, which is Unreal's +Y, from
			// a rest position one source unit up, which is Unreal's +Z. The absolute sizes depend
			// on the unit scale the import settled on, so what is asserted is the shape of the
			// motion: its direction, and that it covers twice the rest height. A track that merely
			// had the right number of keys, or that was converted with a different basis, fails
			// this even though it would pass a count.
			const FVector Delta = LastTranslation - FirstTranslation;

			TestTrue(FString::Printf(TEXT("Bone1 rests above the root on Unreal's Z (got %s)"),
					*FirstTranslation.ToString()),
				FirstTranslation.Z > 0.0);

			TestTrue(FString::Printf(
					TEXT("Bone1 moves along Unreal's +Y, which is where the source +X goes ")
					TEXT("(delta %s)"), *Delta.ToString()),
				Delta.Y > 0.0
					&& FMath::Abs(Delta.X) < 0.01 * Delta.Y
					&& FMath::Abs(Delta.Z) < 0.01 * Delta.Y);

			TestTrue(FString::Printf(
					TEXT("Bone1 travels twice its rest height (rest %s, delta %s)"),
					*FirstTranslation.ToString(), *Delta.ToString()),
				FMath::IsNearlyEqual(Delta.Y, 2.0 * FirstTranslation.Z, 0.01 * Delta.Y));
		}
	}

	Cleanup();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
