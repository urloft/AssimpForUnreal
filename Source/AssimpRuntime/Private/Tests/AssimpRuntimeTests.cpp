// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpBlueprintLibrary.h"
#include "AssimpImportSettings.h"
#include "AssimpRuntime.h"
#include "AssimpScene.h"
#include "AssimpSceneObject.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Tests for the runtime import path.
 *
 * What these add over the AssimpCore tests is the second consumer of the shared conversion: geometry
 * reaching an FDynamicMesh3 rather than staying an FMeshDescription. Since both import paths run the
 * same converter, verifying the counts survive this hop is what shows the sharing actually holds.
 */

namespace AssimpRuntimeTestUtils
{
	FString GetTestDataPath(const FString& FileName)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AssimpForUnreal"));
		if (!Plugin.IsValid())
		{
			return FString();
		}

		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests"), TEXT("Data"), FileName);
	}
}

// =================================================================================================
// Runtime import and dynamic mesh build
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpRuntimeImportTest,
	"AssimpForUnreal.Runtime.ImportAndBuildDynamicMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpRuntimeImportTest::RunTest(const FString& /*Parameters*/)
{
	const FString FilePath = AssimpRuntimeTestUtils::GetTestDataPath(TEXT("Cube.ply"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	FAssimpImportSettings Settings;
	Settings.bApplyFileUnitScale = false;
	Settings.bJoinIdenticalVertices = false;
	Settings.bOptimizeMeshes = false;
	Settings.bImproveCacheLocality = false;
	Settings.bImportSkeletalMesh = false;

	FAssimpRuntimeImportResult Result;
	UAssimpSceneObject* SceneObject = UAssimpBlueprintLibrary::ImportSceneFromFile(
		GetTransientPackage(), FilePath, Settings, Result);

	if (!TestTrue(FString::Printf(TEXT("Runtime import succeeded (%s)"), *Result.ErrorMessage),
		Result.bSucceeded))
	{
		return false;
	}

	if (!TestNotNull(TEXT("Scene handle created"), SceneObject))
	{
		return false;
	}

	TestTrue(TEXT("Scene handle reports valid"), SceneObject->IsValidScene());
	TestEqual(TEXT("Mesh count"), SceneObject->GetMeshCount(), 1);
	TestFalse(TEXT("Not reported as cancelled"), Result.bCancelled);

	// The point of this test: the same conversion that feeds Interchange also feeds the runtime
	// path, so the cube's topology must survive the hop to FDynamicMesh3 unchanged.
	UDynamicMesh* DynamicMesh = NewObject<UDynamicMesh>(GetTransientPackage());
	if (!TestTrue(TEXT("Dynamic mesh built"), SceneObject->BuildDynamicMesh(0, DynamicMesh)))
	{
		return false;
	}

	const UE::Geometry::FDynamicMesh3& Mesh = DynamicMesh->GetMeshRef();

	AddInfo(FString::Printf(TEXT("Dynamic mesh: %d triangles, %d vertices."),
		Mesh.TriangleCount(), Mesh.VertexCount()));

	TestEqual(TEXT("Triangle count survived conversion to FDynamicMesh3"), Mesh.TriangleCount(), 12);
	TestEqual(TEXT("Vertex count survived conversion to FDynamicMesh3"), Mesh.VertexCount(), 8);

	// Bounds must still be the -1..1 cube: no stray scaling on this path either.
	const UE::Geometry::FAxisAlignedBox3d Bounds = Mesh.GetBounds();
	TestTrue(FString::Printf(TEXT("Bounds are -1..1 (got min %s, max %s)"),
			*FVector(Bounds.Min).ToString(), *FVector(Bounds.Max).ToString()),
		FVector(Bounds.Min).Equals(FVector(-1.0), UE_KINDA_SMALL_NUMBER) &&
		FVector(Bounds.Max).Equals(FVector( 1.0), UE_KINDA_SMALL_NUMBER));

	return true;
}

// =================================================================================================
// Capability queries
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpRuntimeCapabilitiesTest,
	"AssimpForUnreal.Runtime.Capabilities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpRuntimeCapabilitiesTest::RunTest(const FString& /*Parameters*/)
{
	// The version comes from calling into the loaded library, so a plausible value here also proves
	// the delay-loaded DLL is genuinely resolved and callable rather than merely present on disk.
	const FString Version = UAssimpBlueprintLibrary::GetAssimpVersion();
	AddInfo(FString::Printf(TEXT("Assimp version: %s"), *Version));
	TestTrue(TEXT("Assimp reports a version"), !Version.IsEmpty());

	// Queried from Assimp itself rather than a hard-coded list, so this also confirms the build's
	// actual format coverage.
	const TArray<FString> Extensions = UAssimpBlueprintLibrary::GetSupportedImportExtensions();
	AddInfo(FString::Printf(TEXT("Assimp reports %d readable extension(s)."), Extensions.Num()));

	TestTrue(TEXT("Assimp reports a substantial format list"), Extensions.Num() > 30);

	for (const TCHAR* Expected : { TEXT("ply"), TEXT("stl"), TEXT("obj"), TEXT("dae"), TEXT("fbx") })
	{
		TestTrue(FString::Printf(TEXT("Assimp can read '%s'"), Expected),
			Extensions.Contains(Expected));
	}

	// Extensions are normalised, so none should retain a glob or dot.
	int32 Malformed = 0;
	for (const FString& Extension : Extensions)
	{
		if (Extension.StartsWith(TEXT(".")) || Extension.Contains(TEXT("*")) ||
			Extension != Extension.ToLower())
		{
			++Malformed;
		}
	}
	TestEqual(TEXT("Extensions are bare and lower-case"), Malformed, 0);

	// The per-extension query must agree with the list, and must tolerate the forms a caller may
	// realistically pass.
	TestTrue(TEXT("'ply' is supported"), UAssimpBlueprintLibrary::IsExtensionSupported(TEXT("ply")));
	TestTrue(TEXT("'.ply' is supported"), UAssimpBlueprintLibrary::IsExtensionSupported(TEXT(".ply")));
	TestTrue(TEXT("'PLY' is supported"), UAssimpBlueprintLibrary::IsExtensionSupported(TEXT("PLY")));
	TestFalse(TEXT("A nonsense extension is not supported"),
		UAssimpBlueprintLibrary::IsExtensionSupported(TEXT("notarealformat")));
	TestFalse(TEXT("An empty extension is not supported"),
		UAssimpBlueprintLibrary::IsExtensionSupported(FString()));

	return true;
}

// =================================================================================================
// Engine-owned formats via the runtime path
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpRuntimeEngineOwnedFormatsTest,
	"AssimpForUnreal.Runtime.EngineOwnedFormats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Asserts that the runtime path handles the four formats the Interchange path cannot claim.
 *
 * The editor path competes with the engine's own translators for .obj, .fbx, .gltf and .glb, and
 * that contest has no defined winner (UInterchangeManager selects from a TSet). The runtime path has
 * no such problem: it calls Assimp directly and never consults UAssimpForUnrealSettings, so the
 * translator registry is irrelevant to it. This pins that property down, because it is the
 * recommended answer for those formats and a regression here would be silent.
 */
bool FAssimpRuntimeEngineOwnedFormatsTest::RunTest(const FString& /*Parameters*/)
{
	// 1. Assimp itself must report all four as readable. This list comes from
	//    Importer::GetExtensionList(), i.e. the linked library's own capabilities, not from any
	//    setting of ours -- which is precisely why the settings cannot gate the runtime path.
	for (const TCHAR* Extension : { TEXT("obj"), TEXT("fbx"), TEXT("gltf"), TEXT("glb") })
	{
		TestTrue(
			FString::Printf(TEXT("Assimp reports '%s' as readable at runtime"), Extension),
			UAssimpBlueprintLibrary::IsExtensionSupported(Extension));
	}

	// 2. And they must actually load. Fixtures exist for three of the four; FBX is binary and not
	//    reasonably hand-authorable, so it is covered by the capability assertion above only.
	struct FCase
	{
		const TCHAR* FileName;
		int32 ExpectedMeshes;
		int32 ExpectedTriangles;
	};

	const FCase Cases[] = {
		{ TEXT("Quad.obj"),         1, 2 },
		{ TEXT("SkinnedQuad.gltf"), 1, 2 },
		{ TEXT("SkinnedQuad.glb"),  1, 2 },
	};

	for (const FCase& Case : Cases)
	{
		const FString FilePath = AssimpRuntimeTestUtils::GetTestDataPath(Case.FileName);
		if (!TestTrue(FString::Printf(TEXT("%s path resolved"), Case.FileName), !FilePath.IsEmpty()))
		{
			continue;
		}

		FAssimpImportSettings Settings;
		Settings.bImportSkeletalMesh = false;

		FAssimpRuntimeImportResult Result;
		UAssimpSceneObject* Scene = UAssimpBlueprintLibrary::ImportSceneFromFile(
			GetTransientPackage(), FilePath, Settings, Result);

		if (!TestTrue(
			FString::Printf(TEXT("%s imported at runtime: %s"), Case.FileName, *Result.ErrorMessage),
			Scene != nullptr && Scene->IsValidScene()))
		{
			continue;
		}

		const FAssimpSceneInfo& Info = Scene->GetSceneInfo();
		TestEqual(FString::Printf(TEXT("%s mesh count"), Case.FileName),
			Info.Meshes.Num(), Case.ExpectedMeshes);

		if (Info.Meshes.Num() == Case.ExpectedMeshes && Case.ExpectedMeshes > 0)
		{
			TestEqual(FString::Printf(TEXT("%s triangle count"), Case.FileName),
				Info.Meshes[0].NumTriangles, Case.ExpectedTriangles);
		}

		AddInfo(FString::Printf(TEXT("%s -> %d mesh(es), %d triangle(s)"),
			Case.FileName, Info.Meshes.Num(),
			Info.Meshes.Num() > 0 ? Info.Meshes[0].NumTriangles : 0));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
