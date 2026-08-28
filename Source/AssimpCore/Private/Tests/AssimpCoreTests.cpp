// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpCore.h"
#include "AssimpImportSettings.h"
#include "AssimpScene.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
