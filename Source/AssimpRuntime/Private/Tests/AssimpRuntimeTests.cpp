// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpBlueprintLibrary.h"
#include "AssimpImportSettings.h"
#include "AssimpRuntime.h"
#include "AssimpScene.h"
#include "AssimpSceneObject.h"

#include "Camera/CameraComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
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

// =================================================================================================
// Cameras and lights
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpSpawnCamerasAndLightsTest,
	"AssimpForUnreal.Runtime.SpawnCamerasAndLights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Asserts that a file's cameras and lights become placed components.
 *
 * The scene description has always carried cameras and lights; what did not exist was anything that
 * turned them into components. The conversion is the interesting half, because Assimp keeps a
 * camera's orientation as a look-at and an up vector in its node's space, while Unreal expresses the
 * same thing as a rotation with +X as the direction looked along -- and a cone angle that Collada
 * and Assimp both state as a full angle has to arrive as Unreal's half-angle or every spot light in
 * the file is twice as wide as it should be.
 */
bool FAssimpSpawnCamerasAndLightsTest::RunTest(const FString& /*Parameters*/)
{
	const FString FilePath = AssimpRuntimeTestUtils::GetTestDataPath(TEXT("CameraLight.dae"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	// Both default to off, so a scene imported with the defaults describes neither.
	FAssimpImportSettings Settings;
	Settings.bImportCameras = true;
	Settings.bImportLights = true;
	Settings.bApplyFileUnitScale = false;

	FAssimpRuntimeImportResult ImportResult;
	UAssimpSceneObject* SceneObject = UAssimpBlueprintLibrary::ImportSceneFromFile(
		GetTransientPackage(), FilePath, Settings, ImportResult);

	if (!TestTrue(FString::Printf(TEXT("CameraLight.dae imported (%s)"), *ImportResult.ErrorMessage),
		ImportResult.bSucceeded && SceneObject != nullptr))
	{
		return false;
	}

	const FAssimpSceneInfo& Info = SceneObject->GetSceneInfo();

	AddInfo(FString::Printf(TEXT("Described %d camera(s) and %d light(s)."),
		Info.Cameras.Num(), Info.Lights.Num()));

	// --- Described --------------------------------------------------------------------------------
	if (!TestEqual(TEXT("One camera was described"), Info.Cameras.Num(), 1))
	{
		return false;
	}

	const FAssimpCameraInfo& Camera = Info.Cameras[0];

	AddInfo(FString::Printf(TEXT("Camera '%s' on node %d, local %s, hfov %.2f, aspect %.2f"),
		*Camera.Name, Camera.NodeIndex, *Camera.LocalTransform.ToString(),
		Camera.HorizontalFieldOfViewDegrees, Camera.AspectRatio));

	// The camera has to find the node that carries it, or it cannot be placed at all.
	TestTrue(TEXT("The camera resolved to a node"),
		Info.Nodes.IsValidIndex(Camera.NodeIndex));

	if (!TestTrue(TEXT("At least one light was described"), Info.Lights.Num() >= 1))
	{
		return false;
	}

	const FAssimpLightInfo* Spot = Info.Lights.FindByPredicate(
		[](const FAssimpLightInfo& Light) { return Light.Type == EAssimpLightType::Spot; });

	if (TestNotNull(TEXT("The spot light was described"), Spot))
	{
		AddInfo(FString::Printf(TEXT("Spot '%s' on node %d, cone %.2f/%.2f, colour %s"),
			*Spot->Name, Spot->NodeIndex,
			Spot->InnerConeAngleDegrees, Spot->OuterConeAngleDegrees,
			*Spot->DiffuseColor.ToString()));

		TestTrue(TEXT("The spot light resolved to a node"),
			Info.Nodes.IsValidIndex(Spot->NodeIndex));
	}

	// --- Spawned ----------------------------------------------------------------------------------
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
	if (!TestNotNull(TEXT("Test world created"), World))
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);

	AActor* Actor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Host actor spawned"), Actor))
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		return false;
	}

	Actor->SetRootComponent(
		NewObject<USceneComponent>(Actor, TEXT("Root")));
	Actor->GetRootComponent()->RegisterComponent();

	const TArray<USceneComponent*> Created =
		UAssimpBlueprintLibrary::SpawnSceneCamerasAndLights(Actor, SceneObject);

	AddInfo(FString::Printf(TEXT("Spawned %d component(s)."), Created.Num()));

	UCameraComponent* CameraComponent = nullptr;
	USpotLightComponent* SpotComponent = nullptr;
	UPointLightComponent* PointComponent = nullptr;

	for (USceneComponent* Component : Created)
	{
		if (UCameraComponent* AsCamera = Cast<UCameraComponent>(Component))
		{
			CameraComponent = AsCamera;
		}
		else if (USpotLightComponent* AsSpot = Cast<USpotLightComponent>(Component))
		{
			SpotComponent = AsSpot;
		}
		else if (UPointLightComponent* AsPoint = Cast<UPointLightComponent>(Component))
		{
			// A spot light is a point light, so this must come after the spot check.
			PointComponent = AsPoint;
		}
	}

	if (TestNotNull(TEXT("A camera component was spawned"), CameraComponent))
	{
		const FVector CameraLocation = CameraComponent->GetRelativeLocation();
		const FVector CameraForward = CameraComponent->GetRelativeRotation().Vector();

		AddInfo(FString::Printf(TEXT("Camera component at %s facing %s, fov %.2f"),
			*CameraLocation.ToString(), *CameraForward.ToString(), CameraComponent->FieldOfView));

		// Source (2, 0, 0) becomes Unreal (0, 2, 0).
		TestTrue(FString::Printf(TEXT("Camera sits where the file put it (got %s)"),
				*CameraLocation.ToString()),
			CameraLocation.Equals(FVector(0.0, 2.0, 0.0), 0.01));

		// Collada cameras look down -Z, which is Unreal's +X. A camera facing anywhere else means
		// the look-at vector was converted wrongly, and the framing of every imported shot is off.
		TestTrue(FString::Printf(TEXT("Camera looks along Unreal's +X (got %s)"),
				*CameraForward.ToString()),
			CameraForward.Equals(FVector(1.0, 0.0, 0.0), 0.01));

		TestTrue(FString::Printf(TEXT("Field of view came through (got %.2f)"),
				CameraComponent->FieldOfView),
			FMath::IsNearlyEqual(CameraComponent->FieldOfView, 60.0f, 1.0f));
	}

	if (TestNotNull(TEXT("A spot light component was spawned"), SpotComponent))
	{
		AddInfo(FString::Printf(TEXT("Spot at %s, cone %.2f/%.2f, colour %s, radius %.1f"),
			*SpotComponent->GetRelativeLocation().ToString(),
			SpotComponent->InnerConeAngle, SpotComponent->OuterConeAngle,
			*SpotComponent->GetLightColor().ToString(),
			SpotComponent->AttenuationRadius));

		// Source (0, 3, 0) becomes Unreal (0, 0, 3).
		TestTrue(FString::Printf(TEXT("Spot sits where the file put it (got %s)"),
				*SpotComponent->GetRelativeLocation().ToString()),
			SpotComponent->GetRelativeLocation().Equals(FVector(0.0, 0.0, 3.0), 0.01));

		// The file states a 30 degree falloff as a FULL angle, and Unreal takes half-angles, so the
		// inner cone must arrive at 15. Getting this wrong makes every spot light in every file
		// twice as wide as its author drew it.
		//
		// Asserted against the described value rather than a literal, because the fixture states no
		// outer angle and Assimp supplies one of its own -- the relationship is ours to get right,
		// the number it starts from is not.
		if (Spot != nullptr)
		{
			TestTrue(FString::Printf(
					TEXT("The inner cone is half the described full angle (%.2f from %.2f)"),
					SpotComponent->InnerConeAngle, Spot->InnerConeAngleDegrees),
				FMath::IsNearlyEqual(
					SpotComponent->InnerConeAngle, Spot->InnerConeAngleDegrees * 0.5f, 0.1f));
		}

		TestTrue(FString::Printf(TEXT("The inner cone is 15 degrees, half of the file's 30 (got %.2f)"),
				SpotComponent->InnerConeAngle),
			FMath::IsNearlyEqual(SpotComponent->InnerConeAngle, 15.0f, 0.1f));

		TestTrue(FString::Printf(TEXT("The outer cone is no tighter than the inner (%.2f vs %.2f)"),
				SpotComponent->OuterConeAngle, SpotComponent->InnerConeAngle),
			SpotComponent->OuterConeAngle >= SpotComponent->InnerConeAngle);

		// Colour transfers, intensity deliberately does not.
		const FLinearColor SpotColour = SpotComponent->GetLightColor();
		TestTrue(FString::Printf(TEXT("Light colour came through (got %s)"), *SpotColour.ToString()),
			SpotColour.R > SpotColour.B);
	}

	if (TestNotNull(TEXT("A point light component was spawned"), PointComponent))
	{
		AddInfo(FString::Printf(TEXT("Point at %s, radius %.1f"),
			*PointComponent->GetRelativeLocation().ToString(),
			PointComponent->AttenuationRadius));

		// Quadratic 0.0625 reaches the 1/256 cutoff at distance 64, which is where the conversion
		// should put the radius. A radius left at the component default would be 1000.
		TestTrue(FString::Printf(TEXT("Attenuation became a radius (got %.1f)"),
				PointComponent->AttenuationRadius),
			FMath::IsNearlyEqual(PointComponent->AttenuationRadius, 64.0f, 1.0f));
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
