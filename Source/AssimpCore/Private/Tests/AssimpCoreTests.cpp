// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpCore.h"
#include "AssimpExporter.h"
#include "AssimpImportSettings.h"
#include "AssimpScene.h"

#include "HAL/FileManager.h"
#include "Misc/ScopeExit.h"

#include "Interfaces/IPluginManager.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Tests for the Assimp-to-Unreal conversion.
 *
 * The emphasis is deliberately on the coordinate conversion rather than on "does the file load".
 * A handedness or winding mistake still produces a mesh with the right triangle count, so a test
 * that only counts triangles passes while the model arrives mirrored or inside-out. These assert
 * exact converted coordinates and the geometric consistency of winding against normals, which is
 * what actually pins that behaviour down.
 *
 * Fixtures live in Tests/Data and are hand-authored ASCII so the expected values can be derived by
 * reading them.
 */

namespace AssimpTestUtils
{
	/** Absolute path to a file in the plugin's Tests/Data directory. */
	FString GetTestDataPath(const FString& FileName)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AssimpForUnreal"));
		if (!Plugin.IsValid())
		{
			return FString();
		}

		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests"), TEXT("Data"), FileName);
	}

	/** Import settings suited to asserting exact coordinates. */
	FAssimpImportSettings MakeExactSettings()
	{
		FAssimpImportSettings Settings;

		// Any of these would perturb the vertex positions or ordering the tests assert on.
		Settings.bApplyFileUnitScale = false;
		Settings.UniformScale = 1.0f;
		Settings.bJoinIdenticalVertices = false;
		Settings.bOptimizeMeshes = false;
		Settings.bImproveCacheLocality = false;
		Settings.bImportSkeletalMesh = false;

		return Settings;
	}
}

// =================================================================================================
// Coordinate conversion
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpAxisConversionTest,
	"AssimpForUnreal.Core.AxisConversion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpAxisConversionTest::RunTest(const FString& /*Parameters*/)
{
	const FString FilePath = AssimpTestUtils::GetTestDataPath(TEXT("AxisProbe.ply"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	FAssimpLoadResult LoadResult;
	const TSharedPtr<FAssimpScene> Scene =
		FAssimpScene::LoadFromFile(FilePath, AssimpTestUtils::MakeExactSettings(), LoadResult);

	if (!TestTrue(FString::Printf(TEXT("AxisProbe.ply loaded (%s)"), *LoadResult.ErrorMessage),
		Scene.IsValid()))
	{
		return false;
	}

	FMeshDescription MeshDescription;
	if (!TestTrue(TEXT("Mesh converted"), Scene->GetMeshDescription(0, MeshDescription)))
	{
		return false;
	}

	TestEqual(TEXT("Vertex count"), MeshDescription.Vertices().Num(), 3);
	TestEqual(TEXT("Triangle count"), MeshDescription.Polygons().Num(), 1);

	FStaticMeshAttributes Attributes(MeshDescription);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();

	// The fixture's three vertices lie on the source basis axes, so each one pins down one column
	// of the change-of-basis matrix:
	//   source (1,0,0) -> Unreal ( 0, 1, 0)
	//   source (0,1,0) -> Unreal ( 0, 0, 1)
	//   source (0,0,1) -> Unreal (-1, 0, 0)
	const TArray<FVector3f> Expected = {
		FVector3f( 0.0f, 1.0f, 0.0f),
		FVector3f( 0.0f, 0.0f, 1.0f),
		FVector3f(-1.0f, 0.0f, 0.0f),
	};

	// PLY preserves vertex order, and vertex welding is disabled, so index i is fixture line i.
	int32 Index = 0;
	for (const FVertexID VertexID : MeshDescription.Vertices().GetElementIDs())
	{
		if (!Expected.IsValidIndex(Index))
		{
			break;
		}

		const FVector3f Actual = Positions[VertexID];
		TestTrue(
			FString::Printf(TEXT("Vertex %d converted to %s (expected %s)"),
				Index, *Actual.ToString(), *Expected[Index].ToString()),
			Actual.Equals(Expected[Index], UE_KINDA_SMALL_NUMBER));

		++Index;
	}

	return true;
}

// =================================================================================================
// Winding and normals
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpCubeWindingTest,
	"AssimpForUnreal.Core.CubeWindingAndNormals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpCubeWindingTest::RunTest(const FString& /*Parameters*/)
{
	const FString FilePath = AssimpTestUtils::GetTestDataPath(TEXT("Cube.ply"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	FAssimpImportSettings Settings = AssimpTestUtils::MakeExactSettings();
	Settings.bGenerateMissingNormals = true;
	Settings.bGenerateTangents = false;

	FAssimpLoadResult LoadResult;
	const TSharedPtr<FAssimpScene> Scene =
		FAssimpScene::LoadFromFile(FilePath, Settings, LoadResult);

	if (!TestTrue(FString::Printf(TEXT("Cube.ply loaded (%s)"), *LoadResult.ErrorMessage),
		Scene.IsValid()))
	{
		return false;
	}

	FMeshDescription MeshDescription;
	if (!TestTrue(TEXT("Mesh converted"), Scene->GetMeshDescription(0, MeshDescription)))
	{
		return false;
	}

	TestEqual(TEXT("Triangle count"), MeshDescription.Polygons().Num(), 12);

	FStaticMeshAttributes Attributes(MeshDescription);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();

	// An unwelded cube is 8 shared vertices with one instance per face corner (12 triangles x 3).
	TestEqual(TEXT("Vertex count"), MeshDescription.Vertices().Num(), 8);
	TestEqual(TEXT("Vertex instance count"), MeshDescription.VertexInstances().Num(), 36);
	TestEqual(TEXT("Polygon group count"), MeshDescription.PolygonGroups().Num(), 1);

	AddInfo(FString::Printf(
		TEXT("Topology: %d vertices, %d vertex instances, %d triangles, %d polygon groups."),
		MeshDescription.Vertices().Num(),
		MeshDescription.VertexInstances().Num(),
		MeshDescription.Polygons().Num(),
		MeshDescription.PolygonGroups().Num()));

	// A cube spanning -1..1 is symmetric under the change of basis, so the converted bounds must
	// still be exactly -1..1. This catches a scale applied where none was asked for.
	FBox Bounds(ForceInit);
	for (const FVertexID VertexID : MeshDescription.Vertices().GetElementIDs())
	{
		Bounds += FVector(Positions[VertexID]);
	}
	TestTrue(FString::Printf(TEXT("Bounds are -1..1 (got %s)"), *Bounds.ToString()),
		Bounds.Min.Equals(FVector(-1.0), UE_KINDA_SMALL_NUMBER) &&
		Bounds.Max.Equals(FVector( 1.0), UE_KINDA_SMALL_NUMBER));

	int32 InconsistentWinding = 0;
	int32 InwardFacing = 0;

	// Reused across iterations. Note the out-parameter overload: the convenience overload that
	// returns a TArray does so BY VALUE, so binding a TArrayView to its result leaves the view
	// dangling the moment the temporary dies.
	TArray<FVertexInstanceID, TInlineAllocator<4>> Corners;

	for (const FPolygonID PolygonID : MeshDescription.Polygons().GetElementIDs())
	{
		MeshDescription.GetPolygonVertexInstances(PolygonID, Corners);

		if (Corners.Num() != 3)
		{
			continue;
		}

		// Validate the instance IDs before dereferencing them: an out-of-range ID asserts inside
		// GetVertexInstanceVertex and tears down the whole test process, hiding every later result.
		if (!MeshDescription.IsVertexInstanceValid(Corners[0]) ||
			!MeshDescription.IsVertexInstanceValid(Corners[1]) ||
			!MeshDescription.IsVertexInstanceValid(Corners[2]))
		{
			UE_LOG(LogAssimp, Error,
				TEXT("Polygon %d references vertex instances (%d, %d, %d) but only %d exist."),
				PolygonID.GetValue(),
				Corners[0].GetValue(), Corners[1].GetValue(), Corners[2].GetValue(),
				MeshDescription.VertexInstances().Num());
			AddError(TEXT("Converted mesh has polygons referencing non-existent vertex instances."));
			break;
		}

		const FVertexID V0 = MeshDescription.GetVertexInstanceVertex(Corners[0]);
		const FVertexID V1 = MeshDescription.GetVertexInstanceVertex(Corners[1]);
		const FVertexID V2 = MeshDescription.GetVertexInstanceVertex(Corners[2]);

		if (!MeshDescription.IsVertexValid(V0) ||
			!MeshDescription.IsVertexValid(V1) ||
			!MeshDescription.IsVertexValid(V2))
		{
			AddError(FString::Printf(
				TEXT("Polygon %d references invalid vertices (%d, %d, %d)."),
				PolygonID.GetValue(), V0.GetValue(), V1.GetValue(), V2.GetValue()));
			break;
		}

		const FVector3f P0 = Positions[V0];
		const FVector3f P1 = Positions[V1];
		const FVector3f P2 = Positions[V2];

		// Geometric normal implied by the stored winding order.
		const FVector3f GeometricNormal =
			FVector3f::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();

		const FVector3f FaceCentre = (P0 + P1 + P2) / 3.0f;

		// THE winding assertion. For a closed convex shape centred on the origin, the normal
		// implied by a correctly wound face points away from the centre.
		//
		// This is what catches a handedness mistake. For a change of basis B with det(B) < 0,
		// cross(B*u, B*v) == -B*cross(u, v) -- the cross product flips. The source faces are wound
		// counter-clockwise as seen from outside, so if the index order were carried across
		// unchanged every face here would come out inward. Equally, flipping twice (ours plus
		// Assimp's aiProcess_FlipWindingOrder) would also invert it. Only exactly one flip passes.
		//
		// Deliberately independent of the stored normals: with GenSmoothNormals a cube's 8 shared
		// vertices get corner-diagonal normals, so a face normal and its corner normals genuinely
		// differ and comparing them would prove nothing about winding.
		if (FVector3f::DotProduct(GeometricNormal, FaceCentre.GetSafeNormal()) <= 0.0f)
		{
			++InconsistentWinding;
		}
	}

	// Separately: every stored normal must point outward. True for a convex shape centred on the
	// origin whether the normals are faceted or smoothed, so this holds independently of how
	// Assimp generated them.
	for (const FVertexInstanceID InstanceID : MeshDescription.VertexInstances().GetElementIDs())
	{
		const FVector3f Normal = Normals[InstanceID].GetSafeNormal();
		const FVector3f Position =
			Positions[MeshDescription.GetVertexInstanceVertex(InstanceID)].GetSafeNormal();

		if (FVector3f::DotProduct(Normal, Position) <= 0.0f)
		{
			++InwardFacing;
		}
	}

	TestEqual(TEXT("Every face is wound so its geometric normal points outward"),
		InconsistentWinding, 0);
	TestEqual(TEXT("Every stored normal points outward"), InwardFacing, 0);

	return true;
}

// =================================================================================================
// UVs and materials
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpUVAndMaterialTest,
	"AssimpForUnreal.Core.UVsAndMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpUVAndMaterialTest::RunTest(const FString& /*Parameters*/)
{
	const FString FilePath = AssimpTestUtils::GetTestDataPath(TEXT("Quad.obj"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	FAssimpLoadResult LoadResult;
	const TSharedPtr<FAssimpScene> Scene =
		FAssimpScene::LoadFromFile(FilePath, AssimpTestUtils::MakeExactSettings(), LoadResult);

	if (!TestTrue(FString::Printf(TEXT("Quad.obj loaded (%s)"), *LoadResult.ErrorMessage),
		Scene.IsValid()))
	{
		return false;
	}

	const FAssimpSceneInfo& Info = Scene->GetSceneInfo();

	TestTrue(TEXT("At least one mesh"), Info.Meshes.Num() >= 1);
	TestTrue(TEXT("At least one material"), Info.Materials.Num() >= 1);
	TestTrue(TEXT("Scene has a node hierarchy"), Info.Nodes.Num() >= 1);

	// The sibling .mtl is found through our IFileManager-backed IOSystem, so a material named
	// after it proves that path resolved.
	bool bFoundNamedMaterial = false;
	for (const FAssimpMaterialInfo& Material : Info.Materials)
	{
		if (Material.Name.Contains(TEXT("QuadMaterial")))
		{
			bFoundNamedMaterial = true;

			// Kd 0.25 0.5 0.75 from Quad.mtl.
			TestTrue(FString::Printf(TEXT("Base colour read from .mtl (got %s)"),
					*Material.BaseColor.ToString()),
				FMath::IsNearlyEqual(Material.BaseColor.R, 0.25f, 0.01f) &&
				FMath::IsNearlyEqual(Material.BaseColor.G, 0.50f, 0.01f) &&
				FMath::IsNearlyEqual(Material.BaseColor.B, 0.75f, 0.01f));

			// d 0.5 makes the material translucent.
			TestTrue(TEXT("Opacity read from .mtl"),
				FMath::IsNearlyEqual(Material.Opacity, 0.5f, 0.01f));
			TestTrue(TEXT("Material flagged translucent"), Material.bIsTranslucent);
		}
	}
	TestTrue(TEXT("Material from the sibling .mtl was resolved"), bFoundNamedMaterial);

	FMeshDescription MeshDescription;
	if (!TestTrue(TEXT("Mesh converted"), Scene->GetMeshDescription(0, MeshDescription)))
	{
		return false;
	}

	FStaticMeshAttributes Attributes(MeshDescription);
	TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();

	TestTrue(TEXT("At least one UV channel"), UVs.GetNumChannels() >= 1);

	// The fixture's UVs are the unit square, so after V-flipping every V must still lie in [0,1]
	// and the set must span it. A missing flip would still satisfy the range check, so also assert
	// that the corner paired with source (0,0) came through as (0,1).
	bool bFoundFlippedOrigin = false;
	int32 OutOfRange = 0;

	for (const FVertexInstanceID InstanceID : MeshDescription.VertexInstances().GetElementIDs())
	{
		const FVector2f UV = UVs.Get(InstanceID, 0);

		if (UV.X < -UE_KINDA_SMALL_NUMBER || UV.X > 1.0f + UE_KINDA_SMALL_NUMBER ||
			UV.Y < -UE_KINDA_SMALL_NUMBER || UV.Y > 1.0f + UE_KINDA_SMALL_NUMBER)
		{
			++OutOfRange;
		}

		if (UV.Equals(FVector2f(0.0f, 1.0f), UE_KINDA_SMALL_NUMBER))
		{
			bFoundFlippedOrigin = true;
		}
	}

	TestEqual(TEXT("All UVs within the unit square"), OutOfRange, 0);
	TestTrue(TEXT("Source UV (0,0) was V-flipped to (0,1)"), bFoundFlippedOrigin);

	// The quad declares one explicit flat normal (vn 0 0 1), so unlike the cube its stored normals
	// really are face normals. That makes this the place to assert the stricter invariant: the
	// normal implied by the stored winding must agree with the stored normal.
	//
	// The two together pin the conversion down. Source normal (0,0,1) maps to Unreal (-1,0,0) under
	// Unreal.X = -Source.Z, and for that to match the winding-implied normal the index order must
	// have been reversed exactly once.
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();

	TArray<FVertexInstanceID, TInlineAllocator<4>> Corners;
	int32 Disagreements = 0;
	int32 WrongDirection = 0;
	int32 TrianglesChecked = 0;

	for (const FPolygonID PolygonID : MeshDescription.Polygons().GetElementIDs())
	{
		MeshDescription.GetPolygonVertexInstances(PolygonID, Corners);
		if (Corners.Num() != 3)
		{
			continue;
		}

		const FVector3f P0 = Positions[MeshDescription.GetVertexInstanceVertex(Corners[0])];
		const FVector3f P1 = Positions[MeshDescription.GetVertexInstanceVertex(Corners[1])];
		const FVector3f P2 = Positions[MeshDescription.GetVertexInstanceVertex(Corners[2])];

		const FVector3f GeometricNormal =
			FVector3f::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();
		const FVector3f StoredNormal = Normals[Corners[0]].GetSafeNormal();

		if (FVector3f::DotProduct(GeometricNormal, StoredNormal) < 0.99f)
		{
			++Disagreements;
		}

		// Source (0,0,1) -> Unreal (-1,0,0).
		if (!StoredNormal.Equals(FVector3f(-1.0f, 0.0f, 0.0f), 0.01f))
		{
			++WrongDirection;
		}

		++TrianglesChecked;
	}

	TestEqual(TEXT("Two triangles checked"), TrianglesChecked, 2);
	TestEqual(TEXT("Winding agrees with the explicit flat normal"), Disagreements, 0);
	TestEqual(TEXT("Explicit normal (0,0,1) converted to (-1,0,0)"), WrongDirection, 0);

	return true;
}

// =================================================================================================
// Settings sanitisation
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpSettingsSanitisationTest,
	"AssimpForUnreal.Core.SettingsSanitisation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpSettingsSanitisationTest::RunTest(const FString& /*Parameters*/)
{
	// The invariant that matters most: the two options that destroy the node hierarchy must be
	// cleared whenever skinning or animation is being imported. Without this the import appears to
	// succeed but silently arrives with no skinning, which is exactly the kind of failure that is
	// expensive to diagnose from the outside.
	{
		FAssimpImportSettings Settings;
		Settings.bImportSkeletalMesh = true;
		Settings.bOptimizeSceneGraph = true;
		Settings.bPreTransformVertices = true;

		TArray<FString> Adjustments;
		const FAssimpImportSettings Sanitised = Settings.GetSanitized(&Adjustments);

		TestFalse(TEXT("Scene graph optimisation cleared for skeletal import"),
			Sanitised.bOptimizeSceneGraph);
		TestFalse(TEXT("Vertex pre-transformation cleared for skeletal import"),
			Sanitised.bPreTransformVertices);
		TestTrue(TEXT("Both corrections were reported"), Adjustments.Num() >= 2);
	}

	// Without skeletal import there is nothing to protect, so the options must survive.
	{
		FAssimpImportSettings Settings;
		Settings.bImportSkeletalMesh = false;
		Settings.bImportAnimations = false;
		Settings.bOptimizeSceneGraph = true;
		Settings.bPreTransformVertices = true;

		const FAssimpImportSettings Sanitised = Settings.GetSanitized();

		TestTrue(TEXT("Scene graph optimisation preserved for static import"),
			Sanitised.bOptimizeSceneGraph);
		TestTrue(TEXT("Vertex pre-transformation preserved for static import"),
			Sanitised.bPreTransformVertices);
	}

	// Animation and morph targets cannot outlive the skeletal mesh they belong to.
	{
		FAssimpImportSettings Settings;
		Settings.bImportSkeletalMesh = false;
		Settings.bImportAnimations = true;
		Settings.bImportMorphTargets = true;

		const FAssimpImportSettings Sanitised = Settings.GetSanitized();

		TestFalse(TEXT("Animation import cleared"), Sanitised.bImportAnimations);
		TestFalse(TEXT("Morph target import cleared"), Sanitised.bImportMorphTargets);
	}

	// Programmatic callers bypass the UI's clamp metadata, so the struct must clamp itself.
	{
		FAssimpImportSettings Settings;
		Settings.MaxBoneInfluencesPerVertex = 999;
		Settings.NormalSmoothingAngle = 400.0f;
		Settings.UniformScale = -3.0f;

		const FAssimpImportSettings Sanitised = Settings.GetSanitized();

		TestEqual(TEXT("Bone influences clamped to 12"), Sanitised.MaxBoneInfluencesPerVertex, 12);
		TestTrue(TEXT("Smoothing angle clamped"), Sanitised.NormalSmoothingAngle <= 175.0f);
		TestTrue(TEXT("Non-positive uniform scale reset"), Sanitised.UniformScale > 0.0f);
	}

	return true;
}

// =================================================================================================
// Skeletal import
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpSkinnedMeshTest,
	"AssimpForUnreal.Core.SkinnedMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpSkinnedMeshTest::RunTest(const FString& /*Parameters*/)
{
	// Fixture generated by Tests/Data/MakeSkinnedQuad.py: a quad bound to two joints, Root at the
	// origin and Bone1 translated +1 on the source's Y axis.
	const FString FilePath = AssimpTestUtils::GetTestDataPath(TEXT("SkinnedQuad.gltf"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	FAssimpImportSettings Settings = AssimpTestUtils::MakeExactSettings();
	Settings.bImportSkeletalMesh = true;

	// glTF declares metres, so the plugin would otherwise scale by 100 and the expected bone
	// translation below would be 100 rather than 1.
	Settings.bApplyFileUnitScale = false;

	FAssimpLoadResult LoadResult;
	const TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromFile(FilePath, Settings, LoadResult);

	if (!TestTrue(FString::Printf(TEXT("SkinnedQuad.gltf loaded (%s)"), *LoadResult.ErrorMessage),
		Scene.IsValid()))
	{
		return false;
	}

	const FAssimpSceneInfo& Info = Scene->GetSceneInfo();

	TestTrue(TEXT("Scene reports skinned meshes"), Info.bHasSkinnedMeshes);

	if (!TestTrue(TEXT("At least one mesh"), Info.Meshes.Num() >= 1))
	{
		return false;
	}

	TestTrue(TEXT("Mesh reports bones"), Info.Meshes[0].bHasBones);
	TestEqual(TEXT("Mesh reports two bone names"), Info.Meshes[0].BoneNames.Num(), 2);

	// Convert, asking for the joint mapping the skin weights were written against.
	FMeshDescription MeshDescription;
	TArray<FString> JointNames;
	if (!TestTrue(TEXT("Skinned mesh converted"),
		Scene->GetMergedMeshDescription(TArray<int32>{ 0 }, MeshDescription, JointNames)))
	{
		return false;
	}

	AddInfo(FString::Printf(TEXT("Skeleton: %s"), *FString::Join(JointNames, TEXT(" -> "))));

	// The reconstructed skeleton must contain both joints, parents before children.
	if (!TestEqual(TEXT("Two joints were reconstructed"), JointNames.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("Root is the first bone"), JointNames[0], FString(TEXT("Root")));
	TestEqual(TEXT("Bone1 is the second bone"), JointNames[1], FString(TEXT("Bone1")));

	// Bone poses must be present and correctly converted.
	FSkeletalMeshConstAttributes SkeletalAttributes(MeshDescription);

	if (!TestEqual(TEXT("Mesh description holds two bones"), SkeletalAttributes.GetNumBones(), 2))
	{
		return false;
	}

	FSkeletalMeshAttributes::FBoneNameAttributesConstRef BoneNames = SkeletalAttributes.GetBoneNames();
	FSkeletalMeshAttributes::FBoneParentIndexAttributesConstRef BoneParents =
		SkeletalAttributes.GetBoneParentIndices();
	FSkeletalMeshAttributes::FBonePoseAttributesConstRef BonePoses = SkeletalAttributes.GetBonePoses();

	int32 BoneOrdinal = 0;
	for (const FBoneID BoneID : SkeletalAttributes.Bones().GetElementIDs())
	{
		const FName Name = BoneNames[BoneID];
		const int32 ParentIndex = BoneParents[BoneID];
		const FTransform Pose = BonePoses[BoneID];

		AddInfo(FString::Printf(TEXT("Bone %d '%s': parent %d, translation %s"),
			BoneOrdinal, *Name.ToString(), ParentIndex, *Pose.GetTranslation().ToString()));

		if (BoneOrdinal == 0)
		{
			TestEqual(TEXT("Root has no parent"), ParentIndex, INDEX_NONE);
			TestTrue(TEXT("Root sits at the origin"),
				Pose.GetTranslation().Equals(FVector::ZeroVector, UE_KINDA_SMALL_NUMBER));
		}
		else if (BoneOrdinal == 1)
		{
			TestEqual(TEXT("Bone1's parent is the root"), ParentIndex, 0);

			// This is the assertion that makes the test worth having. The source translation is
			// (0, 1, 0) in glTF's right-handed Y-up space; under Unreal.X = -Source.Z,
			// Unreal.Y = Source.X, Unreal.Z = Source.Y that becomes (0, 0, 1). Getting a bone
			// transform's basis change wrong is the classic way a skinned import arrives with the
			// skeleton rotated, and only checking a known value catches it.
			TestTrue(FString::Printf(
					TEXT("Bone1's translation converted to (0,0,1) (got %s)"),
					*Pose.GetTranslation().ToString()),
				Pose.GetTranslation().Equals(FVector(0.0, 0.0, 1.0), UE_KINDA_SMALL_NUMBER));
		}

		++BoneOrdinal;
	}

	// Every vertex must carry exactly one influence of weight 1, since the fixture binds rigidly.
	FSkinWeightsVertexAttributesConstRef SkinWeights = SkeletalAttributes.GetVertexSkinWeights();

	int32 VerticesChecked = 0;
	int32 BadInfluenceCount = 0;
	int32 BadWeightSum = 0;
	TSet<int32> ReferencedBones;

	for (const FVertexID VertexID : MeshDescription.Vertices().GetElementIDs())
	{
		int32 InfluenceCount = 0;
		float WeightSum = 0.0f;

		for (UE::AnimationCore::FBoneWeight Influence : SkinWeights.Get(VertexID))
		{
			++InfluenceCount;
			WeightSum += Influence.GetWeight();
			ReferencedBones.Add(static_cast<int32>(Influence.GetBoneIndex()));
		}

		if (InfluenceCount != 1)
		{
			++BadInfluenceCount;
		}
		if (!FMath::IsNearlyEqual(WeightSum, 1.0f, 0.01f))
		{
			++BadWeightSum;
		}

		++VerticesChecked;
	}

	TestEqual(TEXT("Four vertices were skinned"), VerticesChecked, 4);
	TestEqual(TEXT("Every vertex has exactly one influence"), BadInfluenceCount, 0);
	TestEqual(TEXT("Every vertex's weights sum to 1"), BadWeightSum, 0);

	// Both joints must actually be used; binding everything to the root would satisfy the checks
	// above but mean the joint indices were lost.
	TestEqual(TEXT("Both bones are referenced by skin weights"), ReferencedBones.Num(), 2);
	TestTrue(TEXT("Root is referenced"), ReferencedBones.Contains(0));
	TestTrue(TEXT("Bone1 is referenced"), ReferencedBones.Contains(1));

	return true;
}

// =================================================================================================
// Progress reporting and cancellation
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpProgressAndCancellationTest,
	"AssimpForUnreal.Core.ProgressAndCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpProgressAndCancellationTest::RunTest(const FString& /*Parameters*/)
{
	const FString FilePath = AssimpTestUtils::GetTestDataPath(TEXT("Cube.ply"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	// Cancellation is the substantive half of the asynchronous runtime path -- the Blueprint node
	// around it is thin plumbing, but if Assimp cannot actually be interrupted then a long import
	// becomes unstoppable. Test the mechanism directly.
	{
		int32 ProgressCallCount = 0;
		bool bAllFractionsInRange = true;

		FAssimpProgressDelegate ProgressDelegate;
		ProgressDelegate.BindLambda([&ProgressCallCount, &bAllFractionsInRange](float Fraction) -> bool
		{
			++ProgressCallCount;

			// The bridge clamps whatever Assimp reports, including the -1 some importers emit when
			// they cannot estimate progress.
			if (Fraction < 0.0f || Fraction > 1.0f)
			{
				bAllFractionsInRange = false;
			}

			// Returning false asks Assimp to abort.
			return false;
		});

		FAssimpLoadResult Result;
		const TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromFile(
			FilePath, AssimpTestUtils::MakeExactSettings(), Result, ProgressDelegate);

		AddInfo(FString::Printf(TEXT("Progress delegate was called %d time(s) before cancelling."),
			ProgressCallCount));

		TestTrue(TEXT("Progress fractions stayed within [0,1]"), bAllFractionsInRange);

		// Assimp may finish a file this small before it ever polls progress, so cancelling is not
		// guaranteed to take effect. What must hold is that the two outcomes are consistent: either
		// it cancelled and produced nothing, or it completed and produced a scene. A cancelled load
		// that still returns a scene, or a failure reported as neither, would be the real bug.
		if (Result.bCancelled)
		{
			TestFalse(TEXT("A cancelled load returns no scene"), Scene.IsValid());
			TestFalse(TEXT("A cancelled load is not reported as succeeded"), Result.bSucceeded);
			TestTrue(TEXT("A cancelled load explains itself"), !Result.ErrorMessage.IsEmpty());
		}
		else
		{
			TestTrue(TEXT("A load that was not cancelled either succeeded or reported an error"),
				Result.bSucceeded || !Result.ErrorMessage.IsEmpty());
			TestEqual(TEXT("Success and scene validity agree"), Result.bSucceeded, Scene.IsValid());
		}
	}

	// A delegate that keeps returning true must not interfere with a normal load.
	{
		int32 ProgressCallCount = 0;

		FAssimpProgressDelegate ProgressDelegate;
		ProgressDelegate.BindLambda([&ProgressCallCount](float) -> bool
		{
			++ProgressCallCount;
			return true;
		});

		FAssimpLoadResult Result;
		const TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromFile(
			FilePath, AssimpTestUtils::MakeExactSettings(), Result, ProgressDelegate);

		TestTrue(TEXT("Load succeeds when progress is never cancelled"), Result.bSucceeded);
		TestTrue(TEXT("Scene is produced"), Scene.IsValid());
		TestFalse(TEXT("Not reported as cancelled"), Result.bCancelled);
		TestTrue(TEXT("Elapsed time was recorded"), Result.ElapsedSeconds >= 0.0);
	}

	// An unbound delegate is the common case and must be handled without a null-delegate call.
	{
		FAssimpLoadResult Result;
		const TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromFile(
			FilePath, AssimpTestUtils::MakeExactSettings(), Result);

		TestTrue(TEXT("Load succeeds with no progress delegate bound"), Result.bSucceeded);
		TestTrue(TEXT("Scene is produced"), Scene.IsValid());
	}

	return true;
}

// =================================================================================================
// Failure handling
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpFailureHandlingTest,
	"AssimpForUnreal.Core.FailureHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssimpFailureHandlingTest::RunTest(const FString& /*Parameters*/)
{
	// This test deliberately provokes failures, and AssimpCore logs those at Error level -- which
	// the automation framework counts as test failures unless declared expected. Declaring them
	// with Occurrences == 0 ("one or more") keeps the assertion meaningful: the test still fails if
	// a bad input is rejected *silently*, which would leave a user with no idea why an import did
	// nothing.
	AddExpectedErrorPlain(TEXT("File does not exist"),
		EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedErrorPlain(TEXT("Assimp could not read"),
		EAutomationExpectedErrorFlags::Contains, 0);

	// A malformed or missing file must produce a clean, described failure rather than a crash or a
	// silently empty scene.
	{
		FAssimpLoadResult Result;
		const TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromFile(
			TEXT("C:/definitely/not/a/real/path/model.ply"), FAssimpImportSettings(), Result);

		TestFalse(TEXT("Missing file yields no scene"), Scene.IsValid());
		TestFalse(TEXT("Missing file reports failure"), Result.bSucceeded);
		TestTrue(TEXT("Missing file explains itself"), !Result.ErrorMessage.IsEmpty());
	}

	{
		FAssimpLoadResult Result;
		const TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromFile(
			FString(), FAssimpImportSettings(), Result);

		TestFalse(TEXT("Empty path yields no scene"), Scene.IsValid());
		TestTrue(TEXT("Empty path explains itself"), !Result.ErrorMessage.IsEmpty());
	}

	// Garbage bytes exercise the exception containment around Assimp's parsers.
	{
		const uint8 Garbage[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03,
								  0xFF, 0xFE, 0xFD, 0xFC, 0x7F, 0x80, 0x81, 0x82 };

		FAssimpLoadResult Result;
		const TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromMemory(
			MakeArrayView(Garbage, UE_ARRAY_COUNT(Garbage)),
			TEXT("ply"),
			TEXT("Garbage.ply"),
			FAssimpImportSettings(),
			Result);

		TestFalse(TEXT("Garbage input yields no scene"), Scene.IsValid());
		TestTrue(TEXT("Garbage input explains itself"), !Result.ErrorMessage.IsEmpty());
	}

	{
		FAssimpLoadResult Result;
		const TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromMemory(
			TArrayView<const uint8>(),
			TEXT("ply"),
			TEXT("Empty.ply"),
			FAssimpImportSettings(),
			Result);

		TestFalse(TEXT("Empty buffer yields no scene"), Scene.IsValid());
		TestTrue(TEXT("Empty buffer explains itself"), !Result.ErrorMessage.IsEmpty());
	}

	return true;
}

// =================================================================================================
// Extension-independent format detection
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpMismatchedExtensionTest,
	"AssimpForUnreal.Core.MismatchedExtension",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Asserts that Assimp identifies a file by its contents, not by its extension.
 *
 * This underpins the only reliable way to route an engine-owned format (.fbx, .gltf, .glb, .obj)
 * through this plugin's Interchange translator. Interchange picks a translator by iterating a TSet
 * of registered translator classes, so when two translators claim the same extension the winner is
 * hash order -- unspecified, and not something a plugin can influence. There is no API to
 * unregister or deny a translator either. Renaming the file to an extension only this plugin claims
 * sidesteps the contest entirely, but that is only useful if Assimp can still recognise the format,
 * which is exactly what this pins down.
 *
 * The extension must be one no Assimp importer claims. Assimp resolves an importer in two passes:
 * first by extension, then by sniffing the header. A rename to an extension Assimp DOES know (.ogex,
 * for instance) is claimed by that format's importer in the first pass and never reaches sniffing --
 * measured, not assumed: that case fails with "Validation failed: A node of the scene-graph is
 * nullptr", because the OpenGEX importer accepts the file and emits a broken scene.
 *
 * The fixture is Tests/Data/ExtensionMismatch.aimesh: glTF content under an extension Assimp does
 * not recognise.
 */
bool FAssimpMismatchedExtensionTest::RunTest(const FString& Parameters)
{
	const FString FilePath = AssimpTestUtils::GetTestDataPath(TEXT("ExtensionMismatch.aimesh"));
	if (!TestTrue(TEXT("Fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	FAssimpImportSettings Settings = AssimpTestUtils::MakeExactSettings();

	FAssimpLoadResult Result;
	const TSharedPtr<FAssimpScene> Scene =
		FAssimpScene::LoadFromFile(FilePath, Settings, Result);

	if (!TestTrue(
		FString::Printf(TEXT("glTF content under an unknown .aimesh extension still loads: %s"), *Result.ErrorMessage),
		Scene.IsValid()))
	{
		return false;
	}

	// The fixture is a single quad, so recognising it as glTF rather than falling back to a
	// zero-mesh "success" is what actually proves detection worked.
	const FAssimpSceneInfo& Info = Scene->GetSceneInfo();
	TestEqual(TEXT("Detected as glTF and yielded its mesh"), Info.Meshes.Num(), 1);

	if (Info.Meshes.Num() == 1)
	{
		TestEqual(TEXT("Quad has two triangles"), Info.Meshes[0].NumTriangles, 2);
	}

	return true;
}

// =================================================================================================
// Specular response
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpMaterialSpecularTest,
	"AssimpForUnreal.Core.MaterialSpecular",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Asserts that a Phong material's specular response survives into Unreal's terms.
 *
 * Most of the formats this plugin exists to read predate physically based materials: they carry a
 * specular colour and an exponent, and nothing a metallic/roughness renderer can use directly. The
 * conversion is therefore a real translation, not a copy, and it has two ways to go quietly wrong.
 * Dropping the exponent gives every material in a file the same default roughness, so nothing has a
 * highlight where its author put one; mapping the specular colour onto Unreal's Specular input as a
 * gain, rather than against the 4% dielectric baseline it actually means, doubles the reflectance of
 * every file that never expressed an opinion. Both produce a model that renders -- just not the one
 * in the file.
 */
bool FAssimpMaterialSpecularTest::RunTest(const FString& /*Parameters*/)
{
	// The neutral values are a contract with the materials: anything that says nothing about
	// specularity must arrive at Unreal's own defaults, not at zero.
	{
		const FAssimpMaterialInfo Defaults;
		TestEqual(TEXT("Default specular is Unreal's neutral 0.5"), Defaults.Specular, 0.5f);
		TestEqual(TEXT("Default roughness is 0.5"), Defaults.Roughness, 0.5f);
	}

	const FString FilePath = AssimpTestUtils::GetTestDataPath(TEXT("Specular.obj"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	FAssimpLoadResult LoadResult;
	const TSharedPtr<FAssimpScene> Scene =
		FAssimpScene::LoadFromFile(FilePath, AssimpTestUtils::MakeExactSettings(), LoadResult);

	if (!TestTrue(FString::Printf(TEXT("Specular.obj loaded (%s)"), *LoadResult.ErrorMessage),
		Scene.IsValid()))
	{
		return false;
	}

	const FAssimpSceneInfo& Info = Scene->GetSceneInfo();

	const FAssimpMaterialInfo* Shiny = Info.Materials.FindByPredicate(
		[](const FAssimpMaterialInfo& Material) { return Material.Name.Contains(TEXT("MatShiny")); });
	const FAssimpMaterialInfo* Dull = Info.Materials.FindByPredicate(
		[](const FAssimpMaterialInfo& Material) { return Material.Name.Contains(TEXT("MatDull")); });

	if (!TestNotNull(TEXT("MatShiny was read from the .mtl"), Shiny) ||
		!TestNotNull(TEXT("MatDull was read from the .mtl"), Dull))
	{
		return false;
	}

	AddInfo(FString::Printf(TEXT("MatShiny: specular=%.4f roughness=%.4f Ks=%s"),
		Shiny->Specular, Shiny->Roughness, *Shiny->SpecularColor.ToString()));
	AddInfo(FString::Printf(TEXT("MatDull:  specular=%.4f roughness=%.4f Ks=%s"),
		Dull->Specular, Dull->Roughness, *Dull->SpecularColor.ToString()));

	// The colour is carried through untouched, so a caller driving its own material can still see
	// what the file said before it was reduced to one number.
	TestTrue(FString::Printf(TEXT("MatShiny keeps Ks white (got %s)"), *Shiny->SpecularColor.ToString()),
		Shiny->SpecularColor.Equals(FLinearColor::White, 0.01f));
	TestTrue(FString::Printf(TEXT("MatDull keeps Ks 0.2 grey (got %s)"), *Dull->SpecularColor.ToString()),
		FMath::IsNearlyEqual(Dull->SpecularColor.R, 0.2f, 0.01f));

	// White Ks is what an exporter writes when nobody chose anything, so it must land on Unreal's
	// neutral 0.5 and leave the model looking exactly as it would with no specular data at all.
	TestTrue(FString::Printf(TEXT("White Ks maps to the neutral 0.5 (got %.4f)"), Shiny->Specular),
		FMath::IsNearlyEqual(Shiny->Specular, 0.5f, 0.01f));

	// A fifth of that reflectance is a fifth of the value: 0.5 * 0.2.
	TestTrue(FString::Printf(TEXT("Ks 0.2 maps to 0.1 (got %.4f)"), Dull->Specular),
		FMath::IsNearlyEqual(Dull->Specular, 0.1f, 0.01f));

	// Roughness comes from the exponent. The exact figure depends on how the OBJ importer scales Ns,
	// which is Assimp's business and has changed between releases, so what is asserted is the part
	// that is ours: that the exponent is used at all, and that it orders the two materials the way
	// the file does. A conversion that ignored it would leave both at the 0.5 default, which fails
	// both halves.
	TestTrue(FString::Printf(TEXT("A tight highlight gives low roughness (got %.4f)"), Shiny->Roughness),
		Shiny->Roughness > 0.0f && Shiny->Roughness < 0.35f);
	TestTrue(FString::Printf(TEXT("A broad highlight gives high roughness (got %.4f)"), Dull->Roughness),
		Dull->Roughness > 0.5f);
	TestTrue(TEXT("The shinier material is the less rough one"),
		Shiny->Roughness < Dull->Roughness);

	return true;
}

// =================================================================================================
// Animation
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpAnimationTest,
	"AssimpForUnreal.Core.Animation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Asserts that an animation clip is described, grouped with its skeleton, and baked correctly.
 *
 * The fixture's clip drives one bone with a translation along one source axis and a rotation about
 * a different one, which is what makes this a test of the conversion rather than of parsing. The
 * decisive assertion is the last: applying a converted transform to a converted point must give the
 * same answer as converting the point the source transform produces. That identity holds only if the
 * change of basis was applied as a conjugation. Remapping the translation alone -- the obvious
 * shortcut, and one that passes any translation-only clip -- breaks it the moment a rotation is
 * involved, and the symptom in a real import is a character whose limbs bend the wrong way.
 */
bool FAssimpAnimationTest::RunTest(const FString& /*Parameters*/)
{
	const FString FilePath = AssimpTestUtils::GetTestDataPath(TEXT("SkinnedQuad.gltf"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	FAssimpImportSettings Settings = AssimpTestUtils::MakeExactSettings();
	Settings.bImportSkeletalMesh = true;
	Settings.bImportAnimations = true;

	// glTF declares metres; without this every expected translation below would be 100x larger.
	Settings.bApplyFileUnitScale = false;

	FAssimpLoadResult LoadResult;
	const TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromFile(FilePath, Settings, LoadResult);

	if (!TestTrue(FString::Printf(TEXT("SkinnedQuad.gltf loaded (%s)"), *LoadResult.ErrorMessage),
		Scene.IsValid()))
	{
		return false;
	}

	const FAssimpSceneInfo& Info = Scene->GetSceneInfo();

	// --- The clip is described ------------------------------------------------------------------
	if (!TestEqual(TEXT("One animation clip"), Info.Animations.Num(), 1))
	{
		return false;
	}

	const FAssimpAnimationInfo& Animation = Info.Animations[0];

	AddInfo(FString::Printf(TEXT("Clip '%s': %.4fs, ticks/second %.1f, %d animated node(s)"),
		*Animation.Name, Animation.DurationSeconds, Animation.TicksPerSecond,
		Animation.AnimatedNodeNames.Num()));

	TestEqual(TEXT("Clip keeps its name"), Animation.Name, FString(TEXT("Wave")));

	// The fixture's keys span exactly one second. glTF states its timebase in milliseconds, so a
	// conversion that mistook the timebase for a frame rate, or skipped it, would not land here.
	TestTrue(FString::Printf(TEXT("Clip is one second long (got %.4f)"), Animation.DurationSeconds),
		FMath::IsNearlyEqual(Animation.DurationSeconds, 1.0f, 0.01f));

	TestEqual(TEXT("Exactly one node is animated"), Animation.AnimatedNodeNames.Num(), 1);
	TestTrue(TEXT("Bone1 is the animated node"), Animation.AnimatedNodeNames.Contains(TEXT("Bone1")));

	// The fixture also animates its morph target's weight, which is a morph channel rather than a
	// node channel -- so the clip reports one even though only one node moves. Morph weights are
	// covered by Core.MorphTargets; what matters here is that the two kinds are counted separately.
	TestTrue(TEXT("The clip reports its morph channel"), Animation.bHasMeshOrMorphChannels);

	// --- The skeleton the clip can drive --------------------------------------------------------
	if (!TestEqual(TEXT("One skinned mesh group"), Info.SkinnedMeshGroups.Num(), 1))
	{
		return false;
	}

	const FAssimpSkinnedMeshGroup& Group = Info.SkinnedMeshGroups[0];
	TestEqual(TEXT("Group is rooted at Root"), Group.RootBoneName, FString(TEXT("Root")));
	TestEqual(TEXT("Group holds both bones"), Group.Bones.Num(), 2);
	TestEqual(TEXT("Group holds the one skinned mesh"), Group.MeshIndices.Num(), 1);

	if (Group.Bones.Num() == 2)
	{
		TestEqual(TEXT("Root is first"), Group.Bones[0].Name, FString(TEXT("Root")));
		TestEqual(TEXT("Bone1 is second"), Group.Bones[1].Name, FString(TEXT("Bone1")));
		TestEqual(TEXT("Bone1's parent is the root"), Group.Bones[1].ParentIndex, 0);
	}

	// --- Baking ---------------------------------------------------------------------------------
	const double SampleRate = 30.0;

	TArray<FTransform> Keys;
	if (!TestTrue(TEXT("Bone1's track baked"),
		Scene->GetBakedAnimationTrack(0, TEXT("Bone1"), SampleRate, 0.0, 1.0, Keys)))
	{
		return false;
	}

	// Fence-post count: one second at 30 fps is 31 samples, the last of them at t = 1.
	TestEqual(TEXT("One second at 30 Hz gives 31 keys"), Keys.Num(), 31);

	if (Keys.Num() != 31)
	{
		return false;
	}

	// A clip does not animate the root, so asking for it must fail rather than silently return a
	// static track -- otherwise an untouched bone would be baked into every animation.
	TArray<FTransform> RootKeys;
	TestFalse(TEXT("An unanimated bone has no track"),
		Scene->GetBakedAnimationTrack(0, TEXT("Root"), SampleRate, 0.0, 1.0, RootKeys));

	// Source translation runs from (0, 1, 0) to (2, 1, 0) along the source X axis. Under
	// Unreal.X = -Source.Z, Unreal.Y = Source.X, Unreal.Z = Source.Y that is (0, 0, 1) to (0, 2, 1).
	const FVector ExpectedStart(0.0, 0.0, 1.0);
	const FVector ExpectedMiddle(0.0, 1.0, 1.0);
	const FVector ExpectedEnd(0.0, 2.0, 1.0);

	AddInfo(FString::Printf(TEXT("Baked translations: first %s, middle %s, last %s"),
		*Keys[0].GetTranslation().ToString(),
		*Keys[15].GetTranslation().ToString(),
		*Keys.Last().GetTranslation().ToString()));

	TestTrue(FString::Printf(TEXT("First key sits at the start pose (got %s)"),
			*Keys[0].GetTranslation().ToString()),
		Keys[0].GetTranslation().Equals(ExpectedStart, UE_KINDA_SMALL_NUMBER));

	TestTrue(FString::Printf(TEXT("Last key sits at the end pose (got %s)"),
			*Keys.Last().GetTranslation().ToString()),
		Keys.Last().GetTranslation().Equals(ExpectedEnd, UE_KINDA_SMALL_NUMBER));

	// Key 15 of 31 is exactly halfway. With only two source keys, hitting the midpoint proves the
	// samples are interpolated rather than snapped to the nearest stored key.
	TestTrue(FString::Printf(TEXT("Halfway key is interpolated (got %s)"),
			*Keys[15].GetTranslation().ToString()),
		Keys[15].GetTranslation().Equals(ExpectedMiddle, UE_KINDA_SMALL_NUMBER));

	// The rotation. At the end of the clip the source transform is a quarter turn about the source's
	// up axis, applied after the translation. Rather than assert a quaternion -- whose sign and axis
	// under an orientation-reversing basis change are exactly the thing that is easy to reason about
	// wrongly -- assert the property that must hold for any correct conversion:
	//
	//     Convert(SourceTransform * p) == Convert(SourceTransform) * Convert(p)
	//
	// Take the source point (1, 0, 0). The quarter turn about +Y sends it to (0, 0, -1), and the
	// translation puts it at (2, 1, -1). Converting that point gives Unreal (1, 2, 1).
	const FVector SourcePointConverted(0.0, 1.0, 0.0);   // source (1, 0, 0)
	const FVector ExpectedPointConverted(1.0, 2.0, 1.0); // source (2, 1, -1)

	const FVector ActualPoint = Keys.Last().TransformPosition(SourcePointConverted);

	TestTrue(FString::Printf(
			TEXT("The baked rotation moves a point the way the source transform does ")
			TEXT("(expected %s, got %s)"),
			*ExpectedPointConverted.ToString(), *ActualPoint.ToString()),
		ActualPoint.Equals(ExpectedPointConverted, 0.001));

	// Scale is untouched by the clip, so it must come through as the node's own.
	TestTrue(FString::Printf(TEXT("Scale is left alone (got %s)"),
			*Keys.Last().GetScale3D().ToString()),
		Keys.Last().GetScale3D().Equals(FVector::OneVector, 0.001));

	// --- Requesting a sub-range -----------------------------------------------------------------
	// The editor import path lets the user narrow the range, so the second half of the clip must
	// start where the halfway sample was rather than restarting from the beginning.
	TArray<FTransform> HalfKeys;
	if (TestTrue(TEXT("Second half of the clip baked"),
		Scene->GetBakedAnimationTrack(0, TEXT("Bone1"), SampleRate, 0.5, 1.0, HalfKeys)))
	{
		TestEqual(TEXT("Half a second at 30 Hz gives 16 keys"), HalfKeys.Num(), 16);
		if (HalfKeys.Num() > 0)
		{
			TestTrue(FString::Printf(TEXT("Sub-range starts at the halfway pose (got %s)"),
					*HalfKeys[0].GetTranslation().ToString()),
				HalfKeys[0].GetTranslation().Equals(ExpectedMiddle, UE_KINDA_SMALL_NUMBER));
		}
	}

	// --- The same scene in a second format -------------------------------------------------------
	// Collada states its timebase in seconds where glTF states it in milliseconds, and Assimp hands
	// its matrix-valued channels back already decomposed rather than as separate key arrays. Reading
	// the same motion out of both is what shows the conversion is reasoning about the timebase
	// rather than having been tuned to one format's idea of it.
	{
		const FString ColladaPath = AssimpTestUtils::GetTestDataPath(TEXT("SkinnedQuad.dae"));

		FAssimpLoadResult ColladaResult;
		const TSharedPtr<FAssimpScene> ColladaScene =
			FAssimpScene::LoadFromFile(ColladaPath, Settings, ColladaResult);

		if (TestTrue(FString::Printf(TEXT("SkinnedQuad.dae loaded (%s)"), *ColladaResult.ErrorMessage),
			ColladaScene.IsValid()))
		{
			const FAssimpSceneInfo& ColladaInfo = ColladaScene->GetSceneInfo();

			if (TestEqual(TEXT("Collada fixture has one clip"), ColladaInfo.Animations.Num(), 1))
			{
				AddInfo(FString::Printf(TEXT("Collada clip '%s': %.4fs, ticks/second %.1f"),
					*ColladaInfo.Animations[0].Name,
					ColladaInfo.Animations[0].DurationSeconds,
					ColladaInfo.Animations[0].TicksPerSecond));

				TestTrue(FString::Printf(TEXT("Collada clip is one second long (got %.4f)"),
						ColladaInfo.Animations[0].DurationSeconds),
					FMath::IsNearlyEqual(ColladaInfo.Animations[0].DurationSeconds, 1.0f, 0.01f));
			}

			TestEqual(TEXT("Collada fixture has one skinned mesh group"),
				ColladaInfo.SkinnedMeshGroups.Num(), 1);

			TArray<FTransform> ColladaKeys;
			if (TestTrue(TEXT("Collada Bone1 track baked"),
				ColladaScene->GetBakedAnimationTrack(0, TEXT("Bone1"), SampleRate, 0.0, 1.0, ColladaKeys))
				&& ColladaKeys.Num() > 1)
			{
				AddInfo(FString::Printf(TEXT("Collada baked translations: first %s, last %s (%d keys)"),
					*ColladaKeys[0].GetTranslation().ToString(),
					*ColladaKeys.Last().GetTranslation().ToString(),
					ColladaKeys.Num()));

				TestTrue(FString::Printf(TEXT("Collada first key matches the glTF fixture (got %s)"),
						*ColladaKeys[0].GetTranslation().ToString()),
					ColladaKeys[0].GetTranslation().Equals(ExpectedStart, 0.001));

				TestTrue(FString::Printf(TEXT("Collada last key matches the glTF fixture (got %s)"),
						*ColladaKeys.Last().GetTranslation().ToString()),
					ColladaKeys.Last().GetTranslation().Equals(ExpectedEnd, 0.001));
			}
		}
	}

	// --- Disabling animation import --------------------------------------------------------------
	{
		FAssimpImportSettings NoAnimations = Settings;
		NoAnimations.bImportAnimations = false;

		FAssimpLoadResult QuietResult;
		const TSharedPtr<FAssimpScene> QuietScene =
			FAssimpScene::LoadFromFile(FilePath, NoAnimations, QuietResult);

		if (TestTrue(TEXT("Scene loads with animation import disabled"), QuietScene.IsValid()))
		{
			TestEqual(TEXT("No clips are described when animation import is off"),
				QuietScene->GetSceneInfo().Animations.Num(), 0);
			TestTrue(TEXT("Skinning is unaffected by disabling animation"),
				QuietScene->GetSceneInfo().SkinnedMeshGroups.Num() == 1);
		}
	}

	return true;
}

// =================================================================================================
// Morph targets
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpMorphTargetTest,
	"AssimpForUnreal.Core.MorphTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Asserts that a morph target is described, converted, and its weight curve read back.
 *
 * Two things make a morph target go wrong in ways that still import cleanly. The first is the vertex
 * correspondence: Unreal derives the deltas by comparing the morphed mesh against the base one, per
 * vertex index, so if the two conversions disagree about vertex order the result is a morph target
 * that tears the mesh apart rather than deforming it. That is why the fixture displaces every vertex
 * by the same vector -- a correct conversion moves all four identically, and any reordering shows up
 * immediately as vertices that moved differently.
 *
 * The second is the direction: a morph target that moves along the wrong axis is still a valid morph
 * target, so only a known expected vector catches it.
 */
bool FAssimpMorphTargetTest::RunTest(const FString& /*Parameters*/)
{
	const FString FilePath = AssimpTestUtils::GetTestDataPath(TEXT("SkinnedQuad.gltf"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !FilePath.IsEmpty()))
	{
		return false;
	}

	FAssimpImportSettings Settings = AssimpTestUtils::MakeExactSettings();
	Settings.bImportSkeletalMesh = true;
	Settings.bImportMorphTargets = true;
	Settings.bApplyFileUnitScale = false;

	FAssimpLoadResult LoadResult;
	const TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromFile(FilePath, Settings, LoadResult);

	if (!TestTrue(FString::Printf(TEXT("SkinnedQuad.gltf loaded (%s)"), *LoadResult.ErrorMessage),
		Scene.IsValid()))
	{
		return false;
	}

	const FAssimpSceneInfo& Info = Scene->GetSceneInfo();

	// --- Described ------------------------------------------------------------------------------
	if (!TestEqual(TEXT("One morph target"), Info.MorphTargets.Num(), 1))
	{
		return false;
	}

	const FAssimpMorphTargetInfo& MorphTarget = Info.MorphTargets[0];

	AddInfo(FString::Printf(TEXT("Morph target '%s' on mesh %d (index %d), rest weight %.3f"),
		*MorphTarget.Name, MorphTarget.MeshIndex, MorphTarget.MorphIndex, MorphTarget.Weight));

	// glTF carries a morph target's name only in an extras field. Losing it would leave Unreal with
	// nothing to key the morph target by, so the name surviving is a real assertion.
	TestEqual(TEXT("Morph target keeps its name"), MorphTarget.Name, FString(TEXT("Bulge")));
	TestEqual(TEXT("Morph target belongs to mesh 0"), MorphTarget.MeshIndex, 0);
	TestEqual(TEXT("Morph target is the mesh's first"), MorphTarget.MorphIndex, 0);

	if (TestTrue(TEXT("The mesh lists its morph target"), Info.Meshes.Num() > 0))
	{
		TestEqual(TEXT("Mesh lists exactly one morph target"),
			Info.Meshes[0].MorphTargetIndices.Num(), 1);
		TestTrue(TEXT("Mesh points at morph target 0"),
			Info.Meshes[0].MorphTargetIndices.Contains(0));
	}

	// --- Converted ------------------------------------------------------------------------------
	FMeshDescription BaseMesh;
	if (!TestTrue(TEXT("Base mesh converted"), Scene->GetMeshDescription(0, BaseMesh)))
	{
		return false;
	}

	FMeshDescription MorphedMesh;
	if (!TestTrue(TEXT("Morph target converted"),
		Scene->GetMorphTargetMeshDescription(0, MorphedMesh)))
	{
		return false;
	}

	// Identical topology is not a nicety. Unreal pairs the two by vertex index, so a different count
	// means the deltas are computed against the wrong vertices -- or not at all.
	TestEqual(TEXT("Morph target has the same vertex count as the base mesh"),
		MorphedMesh.Vertices().Num(), BaseMesh.Vertices().Num());
	TestEqual(TEXT("Morph target has the same triangle count as the base mesh"),
		MorphedMesh.Triangles().Num(), BaseMesh.Triangles().Num());

	FStaticMeshAttributes BaseAttributes(BaseMesh);
	FStaticMeshAttributes MorphedAttributes(MorphedMesh);

	TVertexAttributesConstRef<FVector3f> BasePositions = BaseAttributes.GetVertexPositions();
	TVertexAttributesConstRef<FVector3f> MorphedPositions = MorphedAttributes.GetVertexPositions();

	// Source displacement (0, 0, 1) becomes (-1, 0, 0) under Unreal.X = -Source.Z.
	const FVector3f ExpectedDelta(-1.0f, 0.0f, 0.0f);

	int32 VerticesChecked = 0;
	int32 WrongDeltas = 0;

	TArray<FVertexID> BaseVertexIDs;
	for (const FVertexID VertexID : BaseMesh.Vertices().GetElementIDs())
	{
		BaseVertexIDs.Add(VertexID);
	}

	TArray<FVertexID> MorphedVertexIDs;
	for (const FVertexID VertexID : MorphedMesh.Vertices().GetElementIDs())
	{
		MorphedVertexIDs.Add(VertexID);
	}

	for (int32 Index = 0; Index < FMath::Min(BaseVertexIDs.Num(), MorphedVertexIDs.Num()); ++Index)
	{
		const FVector3f Delta =
			MorphedPositions[MorphedVertexIDs[Index]] - BasePositions[BaseVertexIDs[Index]];

		if (!Delta.Equals(ExpectedDelta, UE_KINDA_SMALL_NUMBER))
		{
			++WrongDeltas;
			AddInfo(FString::Printf(TEXT("vertex %d moved %s, expected %s"),
				Index, *Delta.ToString(), *ExpectedDelta.ToString()));
		}

		++VerticesChecked;
	}

	TestEqual(TEXT("All four vertices were compared"), VerticesChecked, 4);
	TestEqual(TEXT("Every vertex moved by the converted displacement"), WrongDeltas, 0);

	// --- Weight curve ---------------------------------------------------------------------------
	TArray<float> Times;
	TArray<float> Weights;
	if (TestTrue(TEXT("Morph weight curve read"),
		Scene->GetMorphTargetWeightCurve(0, 0, Times, Weights)))
	{
		AddInfo(FString::Printf(TEXT("Weight curve: %d key(s), %.3f at %.3fs to %.3f at %.3fs"),
			Times.Num(),
			Weights.Num() > 0 ? Weights[0] : -1.0f, Times.Num() > 0 ? Times[0] : -1.0f,
			Weights.Num() > 0 ? Weights.Last() : -1.0f, Times.Num() > 0 ? Times.Last() : -1.0f));

		TestEqual(TEXT("Key times and weights come in pairs"), Times.Num(), Weights.Num());

		if (TestTrue(TEXT("The curve has at least two keys"), Times.Num() >= 2))
		{
			// The fixture runs the weight from 0 to 1 across one second. Times arrive in seconds,
			// which means the clip's timebase was applied -- glTF states it in milliseconds, so a
			// conversion that skipped it would put the last key at 1000.
			TestTrue(FString::Printf(TEXT("Curve starts at t=0 (got %.4f)"), Times[0]),
				FMath::IsNearlyEqual(Times[0], 0.0f, 0.01f));
			TestTrue(FString::Printf(TEXT("Curve ends at t=1s (got %.4f)"), Times.Last()),
				FMath::IsNearlyEqual(Times.Last(), 1.0f, 0.01f));

			TestTrue(FString::Printf(TEXT("Weight starts at 0 (got %.4f)"), Weights[0]),
				FMath::IsNearlyEqual(Weights[0], 0.0f, 0.01f));
			TestTrue(FString::Printf(TEXT("Weight ends at 1 (got %.4f)"), Weights.Last()),
				FMath::IsNearlyEqual(Weights.Last(), 1.0f, 0.01f));
		}
	}

	// --- Disabling morph import -------------------------------------------------------------------
	{
		FAssimpImportSettings NoMorphs = Settings;
		NoMorphs.bImportMorphTargets = false;

		FAssimpLoadResult QuietResult;
		const TSharedPtr<FAssimpScene> QuietScene =
			FAssimpScene::LoadFromFile(FilePath, NoMorphs, QuietResult);

		if (TestTrue(TEXT("Scene loads with morph import disabled"), QuietScene.IsValid()))
		{
			TestEqual(TEXT("No morph targets are described when morph import is off"),
				QuietScene->GetSceneInfo().MorphTargets.Num(), 0);

			// Turning morph targets off must not take the geometry with it.
			TestTrue(TEXT("The mesh still imports"),
				QuietScene->GetSceneInfo().Meshes.Num() == 1);
		}
	}

	return true;
}

// =================================================================================================
// Export
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssimpExportRoundTripTest,
	"AssimpForUnreal.Core.ExportRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Asserts that exporting undoes importing, by doing both and comparing.
 *
 * A round trip is the only test that can catch the defect that matters here. Every individual piece
 * of the export -- the basis change, the winding reversal, the V flip -- produces a perfectly valid
 * file when inverted wrongly; the model simply comes back mirrored, or inside out, or with its UVs
 * upside down. Comparing the re-imported geometry against the original is what makes each of those
 * a failure rather than a plausible-looking file.
 *
 * The comparison is on the converted, Unreal-space geometry at both ends. That is the point: if the
 * export inverted the basis change incorrectly, the second import would apply the forward conversion
 * to already-wrong coordinates and the two would disagree.
 */
bool FAssimpExportRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	// The refusal case below deliberately provokes an error, which the automation framework counts
	// as a failure unless it is declared expected.
	AddExpectedErrorPlain(TEXT("is not a format this Assimp build can write"),
		EAutomationExpectedErrorFlags::Contains, 0);

	// Export is only possible if the vendored library was built with exporters. Say so plainly
	// rather than failing with something opaque, because the fix is a rebuild, not a code change.
	const TArray<FAssimpExportFormat> Formats = FAssimpExporter::GetSupportedFormats();

	AddInfo(FString::Printf(TEXT("Assimp reports %d export format(s)."), Formats.Num()));

	if (!TestTrue(
		TEXT("This Assimp build can export. If not, it was compiled with ASSIMP_NO_EXPORT; ")
		TEXT("rebuild it with Scripts/BuildAssimp.ps1."),
		Formats.Num() > 0))
	{
		return false;
	}

	// Named explicitly rather than inferred: several ids share the .obj extension, and the one
	// without a material sidecar keeps the test to a single file.
	const FString FormatId = TEXT("objnomtl");
	if (!TestTrue(FString::Printf(TEXT("The '%s' exporter is available"), *FormatId),
		FAssimpExporter::IsFormatSupported(FormatId)))
	{
		TArray<FString> FormatIds;
		for (const FAssimpExportFormat& Format : Formats)
		{
			FormatIds.Add(Format.FormatId);
		}
		AddInfo(FString::Printf(TEXT("Available: %s"), *FString::Join(FormatIds, TEXT(", "))));
		return false;
	}

	// --- Import ----------------------------------------------------------------------------------
	const FString SourcePath = AssimpTestUtils::GetTestDataPath(TEXT("Cube.ply"));
	if (!TestTrue(TEXT("Test fixture path resolved"), !SourcePath.IsEmpty()))
	{
		return false;
	}

	const FAssimpImportSettings Settings = AssimpTestUtils::MakeExactSettings();

	FAssimpLoadResult LoadResult;
	const TSharedPtr<FAssimpScene> Original =
		FAssimpScene::LoadFromFile(SourcePath, Settings, LoadResult);

	if (!TestTrue(FString::Printf(TEXT("Cube.ply loaded (%s)"), *LoadResult.ErrorMessage),
		Original.IsValid()))
	{
		return false;
	}

	FMeshDescription OriginalMesh;
	if (!TestTrue(TEXT("Original mesh converted"), Original->GetMeshDescription(0, OriginalMesh)))
	{
		return false;
	}

	// --- Export ----------------------------------------------------------------------------------
	const FString DestinationPath = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("AssimpExportTests"), TEXT("CubeRoundTrip.obj"));

	// Leave nothing behind whichever way the test exits.
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DestinationPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
	};

	FAssimpExporter::FExportMesh ExportMesh;
	ExportMesh.Name = TEXT("Cube");
	ExportMesh.MeshDescription = &OriginalMesh;

	FAssimpExportResult ExportResult;
	const bool bExported = FAssimpExporter::ExportMeshes(
		MakeArrayView(&ExportMesh, 1), DestinationPath, FormatId, ExportResult);

	if (!TestTrue(FString::Printf(TEXT("Export succeeded (%s)"), *ExportResult.ErrorMessage), bExported))
	{
		return false;
	}

	TestEqual(TEXT("One mesh was written"), ExportResult.MeshesWritten, 1);

	if (!TestTrue(TEXT("The exported file exists"),
		IFileManager::Get().FileExists(*DestinationPath)))
	{
		return false;
	}

	AddInfo(FString::Printf(TEXT("Wrote %lld bytes to '%s' in %.3fs."),
		IFileManager::Get().FileSize(*DestinationPath), *DestinationPath, ExportResult.ElapsedSeconds));

	// --- Import it back ----------------------------------------------------------------------------
	FAssimpLoadResult ReloadResult;
	const TSharedPtr<FAssimpScene> RoundTripped =
		FAssimpScene::LoadFromFile(DestinationPath, Settings, ReloadResult);

	if (!TestTrue(FString::Printf(TEXT("The exported file reimports (%s)"), *ReloadResult.ErrorMessage),
		RoundTripped.IsValid()))
	{
		return false;
	}

	FMeshDescription ReloadedMesh;
	if (!TestTrue(TEXT("Round-tripped mesh converted"),
		RoundTripped->GetMeshDescription(0, ReloadedMesh)))
	{
		return false;
	}

	// --- Compare -----------------------------------------------------------------------------------
	TestEqual(TEXT("Triangle count survived the round trip"),
		ReloadedMesh.Triangles().Num(), OriginalMesh.Triangles().Num());

	FStaticMeshConstAttributes OriginalAttributes(OriginalMesh);
	FStaticMeshConstAttributes ReloadedAttributes(ReloadedMesh);

	TVertexAttributesConstRef<FVector3f> OriginalPositions = OriginalAttributes.GetVertexPositions();
	TVertexAttributesConstRef<FVector3f> ReloadedPositions = ReloadedAttributes.GetVertexPositions();

	/** Axis-aligned bounds of a mesh description, which are order-independent. */
	auto ComputeBounds = [](const FMeshDescription& Mesh, TVertexAttributesConstRef<FVector3f> Positions)
	{
		FBox Bounds(ForceInit);
		for (const FVertexID VertexID : Mesh.Vertices().GetElementIDs())
		{
			Bounds += FVector(Positions[VertexID]);
		}
		return Bounds;
	};

	const FBox OriginalBounds = ComputeBounds(OriginalMesh, OriginalPositions);
	const FBox ReloadedBounds = ComputeBounds(ReloadedMesh, ReloadedPositions);

	AddInfo(FString::Printf(TEXT("Original bounds %s .. %s"),
		*OriginalBounds.Min.ToString(), *OriginalBounds.Max.ToString()));
	AddInfo(FString::Printf(TEXT("Reloaded bounds %s .. %s"),
		*ReloadedBounds.Min.ToString(), *ReloadedBounds.Max.ToString()));

	// Bounds rather than per-vertex equality, because the exporter writes unwelded triangles and the
	// importer welds on the way back, so vertex ORDER is not preserved and need not be. What must be
	// preserved is where the geometry is -- and a mirrored export moves the bounds, because the
	// fixture's cube is not centred on every axis.
	TestTrue(FString::Printf(TEXT("Bounds minimum survived (expected %s, got %s)"),
			*OriginalBounds.Min.ToString(), *ReloadedBounds.Min.ToString()),
		ReloadedBounds.Min.Equals(OriginalBounds.Min, 0.01));

	TestTrue(FString::Printf(TEXT("Bounds maximum survived (expected %s, got %s)"),
			*OriginalBounds.Max.ToString(), *ReloadedBounds.Max.ToString()),
		ReloadedBounds.Max.Equals(OriginalBounds.Max, 0.01));

	// Winding is the half a bounds comparison cannot see: a cube exported with reversed faces
	// occupies exactly the same space. Every face of a closed convex mesh must have its geometric
	// normal pointing away from the centre, which is the same check the import test makes.
	const FVector Centre = ReloadedBounds.GetCenter();
	int32 InwardFacing = 0;
	int32 FacesChecked = 0;

	for (const FTriangleID TriangleID : ReloadedMesh.Triangles().GetElementIDs())
	{
		TArrayView<const FVertexID> Corners = ReloadedMesh.GetTriangleVertices(TriangleID);
		if (Corners.Num() != 3)
		{
			continue;
		}

		const FVector A(ReloadedPositions[Corners[0]]);
		const FVector B(ReloadedPositions[Corners[1]]);
		const FVector C(ReloadedPositions[Corners[2]]);

		const FVector GeometricNormal = FVector::CrossProduct(B - A, C - A);
		const FVector Outward = ((A + B + C) / 3.0) - Centre;

		if ((GeometricNormal | Outward) <= 0.0)
		{
			++InwardFacing;
		}

		++FacesChecked;
	}

	AddInfo(FString::Printf(TEXT("Checked %d face(s); %d faced inward."), FacesChecked, InwardFacing));

	TestTrue(TEXT("Every face was checked"), FacesChecked > 0);
	TestEqual(TEXT("No face came back inside out"), InwardFacing, 0);

	// --- A shape that is not symmetric --------------------------------------------------------------
	// The cube above is symmetric about every axis, which means it cannot see the one mistake most
	// worth catching: applying the basis change again on export instead of inverting it. Doing that
	// maps (x,y,z) to (-y,-z,x), which leaves a symmetric cube's bounds exactly where they were.
	// AxisProbe's three vertices sit on the source axes, so any basis error moves its bounds.
	{
		const FString ProbePath = AssimpTestUtils::GetTestDataPath(TEXT("AxisProbe.ply"));
		const FString ProbeDestination = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("AssimpExportTests"), TEXT("AxisProbeRoundTrip.obj"));

		ON_SCOPE_EXIT
		{
			IFileManager::Get().Delete(*ProbeDestination, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		};

		FAssimpLoadResult ProbeLoadResult;
		const TSharedPtr<FAssimpScene> Probe =
			FAssimpScene::LoadFromFile(ProbePath, Settings, ProbeLoadResult);

		FMeshDescription ProbeMesh;
		if (Probe.IsValid() && Probe->GetMeshDescription(0, ProbeMesh))
		{
			FAssimpExporter::FExportMesh ProbeExport;
			ProbeExport.Name = TEXT("AxisProbe");
			ProbeExport.MeshDescription = &ProbeMesh;

			FAssimpExportResult ProbeExportResult;
			if (TestTrue(FString::Printf(TEXT("AxisProbe exported (%s)"), *ProbeExportResult.ErrorMessage),
				FAssimpExporter::ExportMeshes(
					MakeArrayView(&ProbeExport, 1), ProbeDestination, FormatId, ProbeExportResult)))
			{
				FAssimpLoadResult ProbeReloadResult;
				const TSharedPtr<FAssimpScene> ProbeReloaded =
					FAssimpScene::LoadFromFile(ProbeDestination, Settings, ProbeReloadResult);

				FMeshDescription ProbeReloadedMesh;
				if (TestTrue(TEXT("AxisProbe reimports"),
					ProbeReloaded.IsValid() && ProbeReloaded->GetMeshDescription(0, ProbeReloadedMesh)))
				{
					FStaticMeshConstAttributes ProbeAttributes(ProbeMesh);
					FStaticMeshConstAttributes ProbeReloadedAttributes(ProbeReloadedMesh);

					const FBox ProbeBounds =
						ComputeBounds(ProbeMesh, ProbeAttributes.GetVertexPositions());
					const FBox ProbeReloadedBounds =
						ComputeBounds(ProbeReloadedMesh, ProbeReloadedAttributes.GetVertexPositions());

					AddInfo(FString::Printf(TEXT("AxisProbe bounds %s .. %s round-tripped to %s .. %s"),
						*ProbeBounds.Min.ToString(), *ProbeBounds.Max.ToString(),
						*ProbeReloadedBounds.Min.ToString(), *ProbeReloadedBounds.Max.ToString()));

					TestTrue(FString::Printf(
							TEXT("An asymmetric shape keeps its bounds (expected %s .. %s, got %s .. %s)"),
							*ProbeBounds.Min.ToString(), *ProbeBounds.Max.ToString(),
							*ProbeReloadedBounds.Min.ToString(), *ProbeReloadedBounds.Max.ToString()),
						ProbeReloadedBounds.Min.Equals(ProbeBounds.Min, 0.01) &&
						ProbeReloadedBounds.Max.Equals(ProbeBounds.Max, 0.01));
				}
			}
		}
	}

	// --- Refusals ------------------------------------------------------------------------------------
	// An unknown format must fail with an explanation rather than writing a broken file.
	{
		FAssimpExportResult BadResult;
		const bool bBadExport = FAssimpExporter::ExportMeshes(
			MakeArrayView(&ExportMesh, 1), DestinationPath, TEXT("definitely-not-a-format"), BadResult);

		TestFalse(TEXT("An unknown format is refused"), bBadExport);
		TestTrue(TEXT("The refusal explains itself"), !BadResult.ErrorMessage.IsEmpty());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
