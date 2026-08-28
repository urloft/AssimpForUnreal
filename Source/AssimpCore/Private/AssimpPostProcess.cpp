// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpPostProcess.h"

unsigned int FAssimpPostProcess::BuildFlags(const FAssimpImportSettings& Settings, bool bRelaxed)
{
	// Always on, regardless of settings:
	//   Triangulate  -- Unreal's mesh pipeline is triangles only.
	//   SortByPType  -- separates points and lines into their own meshes so the triangle path does
	//                   not have to filter them face by face.
	unsigned int Flags = aiProcess_Triangulate | aiProcess_SortByPType;

	if (Settings.bJoinIdenticalVertices)
	{
		Flags |= aiProcess_JoinIdenticalVertices;
	}

	if (Settings.bGenerateMissingNormals)
	{
		// GenSmoothNormals is a no-op on meshes that already have normals, so this only fills gaps.
		Flags |= aiProcess_GenSmoothNormals;
	}

	if (Settings.bGenerateTangents)
	{
		Flags |= aiProcess_CalcTangentSpace;
	}

	if (Settings.bRemoveDegenerates)
	{
		Flags |= aiProcess_FindDegenerates;
	}

	if (Settings.bFixInvalidData)
	{
		Flags |= aiProcess_FindInvalidData;
	}

	if (Settings.bOptimizeMeshes)
	{
		Flags |= aiProcess_OptimizeMeshes;
	}

	if (Settings.bOptimizeSceneGraph)
	{
		// Sanitisation has already cleared this when skeletons or animations are being imported.
		Flags |= aiProcess_OptimizeGraph;
	}

	if (Settings.bRemoveRedundantMaterials)
	{
		Flags |= aiProcess_RemoveRedundantMaterials;
	}

	if (Settings.bImproveCacheLocality)
	{
		Flags |= aiProcess_ImproveCacheLocality;
	}

	if (Settings.bValidateSceneStructure && !bRelaxed)
	{
		Flags |= aiProcess_ValidateDataStructure;
	}

	if (Settings.bPreTransformVertices)
	{
		// Also cleared by sanitisation when the node hierarchy is needed.
		Flags |= aiProcess_PreTransformVertices;
	}

	// Ensure UVs exist and are usable: GenUVCoords turns parametric mappings (spherical, cylindrical)
	// into explicit coordinates, and TransformUVCoords bakes any UV transform the material declares.
	Flags |= aiProcess_GenUVCoords | aiProcess_TransformUVCoords;

	if (Settings.bImportSkeletalMesh)
	{
		// Fills in the armature/bone relationships that skeleton reconstruction needs; several
		// importers leave that information implicit otherwise.
		Flags |= aiProcess_PopulateArmatureData;
		Flags |= aiProcess_LimitBoneWeights;
	}

	return Flags;
}

void FAssimpPostProcess::ApplyProperties(
	Assimp::Importer& Importer,
	const FAssimpImportSettings& Settings,
	bool bRelaxed)
{
	// Crease angle used by GenSmoothNormals, in degrees.
	Importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, Settings.NormalSmoothingAngle);

	// Assimp's own default here is 4; Unreal supports up to 12, so the default would silently
	// discard influences on densely skinned characters.
	Importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, Settings.MaxBoneInfluencesPerVertex);

	// Keep points and lines out of the imported result. SortByPType moves them into dedicated
	// meshes; this removes those meshes entirely so they never reach the conversion.
	//
	// Relaxed mode keeps them, so a caller classifying a failure can tell whether the file held
	// point/line geometry all along rather than being corrupt.
	Importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
		bRelaxed ? 0 : (aiPrimitiveType_POINT | aiPrimitiveType_LINE));

	// Have FindDegenerates remove degenerate faces outright rather than only flagging them.
	Importer.SetPropertyBool(AI_CONFIG_PP_FD_REMOVE, Settings.bRemoveDegenerates);
}
