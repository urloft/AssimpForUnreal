// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpAxisConverter.h"
#include "AssimpIncludes.h"
#include "AssimpScene.h"
#include "CoreMinimal.h"

struct FMeshDescription;

/**
 * Converts Assimp geometry into FMeshDescription.
 *
 * This is the single geometry conversion path in the plugin. Both consumers feed from it:
 * AssimpInterchange returns the result directly as Interchange mesh payload data, and AssimpRuntime
 * converts it onward into an FDynamicMesh3. Keeping it in one place is the reason a fix to, say,
 * tangent handedness benefits the editor and runtime paths at once.
 *
 * FMeshDescription is the right intermediate representation rather than an intermediate of our own:
 * it is what Interchange's payload interface already expects, it is what the engine's static and
 * skeletal mesh builders consume, and it already models the vertex/vertex-instance split that
 * Assimp's per-corner attributes map onto.
 */
class FAssimpMeshConverter
{
public:
	/**
	 * Converts one or more Assimp meshes into a single FMeshDescription.
	 *
	 * Multiple meshes are supported because Assimp splits geometry by material -- one aiMesh has
	 * exactly one material -- whereas Unreal represents that as one mesh with several sections. Each
	 * source mesh therefore becomes its own polygon group and material slot, in the order given.
	 *
	 * @param Scene           Parsed scene owning the meshes.
	 * @param MeshIndices     Indices into Scene.mMeshes, in the order their material slots should appear.
	 * @param AxisConverter   Coordinate and scale conversion to apply.
	 * @param Settings        Effective import settings.
	 * @param OutMeshDescription  Reset and populated on success.
	 * @param OutError        Set when the function returns false.
	 * @return                True on success.
	 */
	static bool Convert(
		const aiScene& Scene,
		TArrayView<const int32> MeshIndices,
		const FAssimpAxisConverter& AxisConverter,
		const FAssimpImportSettings& Settings,
		FMeshDescription& OutMeshDescription,
		FString& OutError)
	{
		TArray<FString> UnusedJointNames;
		return Convert(Scene, MeshIndices, AxisConverter, Settings, OutMeshDescription,
			UnusedJointNames, OutError);
	}

	/**
	 * As above, additionally reconstructing a skeleton and writing skin weights when the meshes are
	 * skinned and Settings requests it.
	 *
	 * @param OutJointNames  Bone names in the index order the skin weights refer to. This is exactly
	 *                       what Interchange's FMeshPayloadData::JointNames wants, and it is the only
	 *                       thing that lets a caller merging several meshes remap their weights
	 *                       consistently. Empty when the result is not skinned.
	 * @param MorphTargetIndex  When set, each mesh's positions and normals are taken from that one of
	 *                       its anim-meshes instead of from the mesh itself, producing the morphed
	 *                       shape. Everything else -- topology, winding, UVs, skin weights -- is
	 *                       produced by the identical code, which is the point: a morph target is
	 *                       only meaningful if its vertices correspond one-for-one with the base
	 *                       mesh's, and running a second, separate conversion is exactly how that
	 *                       correspondence gets quietly broken.
	 */
	static bool Convert(
		const aiScene& Scene,
		TArrayView<const int32> MeshIndices,
		const FAssimpAxisConverter& AxisConverter,
		const FAssimpImportSettings& Settings,
		FMeshDescription& OutMeshDescription,
		TArray<FString>& OutJointNames,
		FString& OutError,
		int32 MorphTargetIndex = INDEX_NONE);

	/**
	 * Computes the axis-aligned bounds of a single mesh in Unreal space.
	 * Cheap enough to run while building the scene description, and avoids a full conversion when
	 * only bounds are wanted.
	 */
	static FBox ComputeBounds(const aiMesh& Mesh, const FAssimpAxisConverter& AxisConverter);
};
