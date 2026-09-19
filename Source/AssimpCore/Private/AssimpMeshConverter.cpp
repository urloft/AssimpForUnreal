// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpMeshConverter.h"

#include "AssimpCore.h"
#include "AssimpSkeletonBuilder.h"
#include "BoneIndices.h"
#include "BoneWeights.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"

namespace
{
	/**
	 * Maximum UV channels written to the mesh description.
	 *
	 * Assimp allows up to AI_MAX_NUMBER_OF_TEXTURECOORDS (8) and so does Unreal's static mesh
	 * pipeline, so 8 is the shared ceiling.
	 */
	constexpr int32 MaxUVChannels = 8;

	/** Assimp colour set used for vertex colours. Unreal's mesh description carries a single set. */
	constexpr int32 VertexColorSetIndex = 0;

	/** Number of UV channels a mesh actually populates, clamped to what we can represent. */
	int32 GetUsedUVChannelCount(const aiMesh& Mesh)
	{
		int32 Count = 0;
		for (int32 Channel = 0; Channel < MaxUVChannels; ++Channel)
		{
			if (Mesh.HasTextureCoords(static_cast<unsigned int>(Channel)))
			{
				Count = Channel + 1;
			}
		}
		return Count;
	}

	/**
	 * Derives Unreal's binormal sign from a converted normal/tangent/bitangent triple.
	 *
	 * Unreal does not store the bitangent; it stores the tangent plus a sign, and reconstructs
	 * bitangent = (normal x tangent) * sign. Computing the sign from the already-converted vectors
	 * rather than from Assimp's originals is what makes this correct under a handedness-reversing
	 * change of basis: whichever way the basis flipped, the relationship between these three
	 * converted vectors is the one Unreal will reconstruct from.
	 */
	float ComputeBinormalSign(const FVector3f& Normal, const FVector3f& Tangent, const FVector3f& Bitangent)
	{
		const FVector3f Reconstructed = FVector3f::CrossProduct(Normal, Tangent);
		return (FVector3f::DotProduct(Reconstructed, Bitangent) < 0.0f) ? -1.0f : 1.0f;
	}
}

FBox FAssimpMeshConverter::ComputeBounds(const aiMesh& Mesh, const FAssimpAxisConverter& AxisConverter)
{
	FBox Bounds(ForceInit);

	for (unsigned int Index = 0; Index < Mesh.mNumVertices; ++Index)
	{
		Bounds += FVector(AxisConverter.ConvertPosition(Mesh.mVertices[Index]));
	}

	return Bounds;
}

bool FAssimpMeshConverter::Convert(
	const aiScene& Scene,
	TArrayView<const int32> MeshIndices,
	const FAssimpAxisConverter& AxisConverter,
	const FAssimpImportSettings& Settings,
	FMeshDescription& OutMeshDescription,
	TArray<FString>& OutJointNames,
	FString& OutError,
	int32 MorphTargetIndex)
{
	OutJointNames.Reset();

	if (MeshIndices.IsEmpty())
	{
		OutError = TEXT("No meshes were requested.");
		return false;
	}

	// Validate every index up front so a bad request fails before any partial work is done.
	for (const int32 MeshIndex : MeshIndices)
	{
		if (!Scene.mMeshes || MeshIndex < 0 || static_cast<unsigned int>(MeshIndex) >= Scene.mNumMeshes)
		{
			OutError = FString::Printf(
				TEXT("Mesh index %d is out of range; the scene has %u mesh(es)."),
				MeshIndex, Scene.mNumMeshes);
			return false;
		}
		if (Scene.mMeshes[MeshIndex] == nullptr)
		{
			OutError = FString::Printf(TEXT("Mesh index %d is null."), MeshIndex);
			return false;
		}
	}

	OutMeshDescription.Empty();

	// Decide up front whether this is a skinned conversion, because it changes which attribute set
	// has to be registered before any element is created.
	bool bAnyBones = false;
	for (const int32 MeshIndex : MeshIndices)
	{
		const aiMesh* Mesh = Scene.mMeshes[MeshIndex];
		bAnyBones |= (Mesh != nullptr && Mesh->HasBones());
	}

	const bool bBuildSkinWeights = bAnyBones && Settings.bImportSkeletalMesh;

	FAssimpSkeletonBuilder SkeletonBuilder;
	if (bBuildSkinWeights)
	{
		if (!SkeletonBuilder.Build(Scene, MeshIndices, AxisConverter))
		{
			UE_LOG(LogAssimp, Warning,
				TEXT("Meshes report bones but no skeleton could be reconstructed; ")
				TEXT("importing as static geometry instead."));
		}
	}

	const bool bSkinned = bBuildSkinWeights && !SkeletonBuilder.GetBones().IsEmpty();

	// FSkeletalMeshAttributes is a superset of FStaticMeshAttributes, so registering it also
	// provides positions, normals, tangents and UVs. Registration must happen before any element is
	// created, which is why the skinned/static decision cannot be deferred.
	FStaticMeshAttributes StaticAttributes(OutMeshDescription);
	FSkeletalMeshAttributes SkeletalAttributes(OutMeshDescription);

	if (bSkinned)
	{
		SkeletalAttributes.Register();
	}
	else
	{
		StaticAttributes.Register();
	}

	FStaticMeshAttributes& Attributes = StaticAttributes;

	// UV channel count is a property of the whole mesh description, not of an individual polygon
	// group, so it must be the maximum across every mesh being merged and must be set before any
	// vertex instance is written.
	int32 TotalVertices = 0;
	int32 TotalTriangles = 0;
	int32 RequiredUVChannels = 1;
	bool bAnyVertexColors = false;

	for (const int32 MeshIndex : MeshIndices)
	{
		const aiMesh& Mesh = *Scene.mMeshes[MeshIndex];
		TotalVertices += static_cast<int32>(Mesh.mNumVertices);
		TotalTriangles += static_cast<int32>(Mesh.mNumFaces);
		RequiredUVChannels = FMath::Max(RequiredUVChannels, GetUsedUVChannelCount(Mesh));
		bAnyVertexColors |= Mesh.HasVertexColors(VertexColorSetIndex);
	}

	TVertexAttributesRef<FVector3f>         VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals         = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents        = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float>     BinormalSigns   = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> Colors          = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> UVs             = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName>       MaterialSlots   = Attributes.GetPolygonGroupMaterialSlotNames();

	UVs.SetNumChannels(RequiredUVChannels);

	// Populate the skeleton before any vertex exists, so skin weights can reference bone indices as
	// vertices are created.
	FSkinWeightsVertexAttributesRef SkinWeights;
	if (bSkinned)
	{
		const TArray<FAssimpSkeletonBuilder::FBone>& SkeletonBones = SkeletonBuilder.GetBones();

		SkeletalAttributes.ReserveNewBones(SkeletonBones.Num());

		FSkeletalMeshAttributes::FBoneNameAttributesRef        BoneNames   = SkeletalAttributes.GetBoneNames();
		FSkeletalMeshAttributes::FBoneParentIndexAttributesRef BoneParents = SkeletalAttributes.GetBoneParentIndices();
		FSkeletalMeshAttributes::FBonePoseAttributesRef        BonePoses   = SkeletalAttributes.GetBonePoses();

		for (const FAssimpSkeletonBuilder::FBone& Bone : SkeletonBones)
		{
			// Bones are created in the builder's order, which is already parents-before-children, so
			// each FBoneID's index matches the builder's index and ParentIndex needs no remapping.
			const FBoneID BoneID = SkeletalAttributes.CreateBone();
			BoneNames[BoneID]   = FName(*Bone.Name);
			BoneParents[BoneID] = Bone.ParentIndex;
			BonePoses[BoneID]   = Bone.LocalTransform;
		}

		SkinWeights = SkeletalAttributes.GetVertexSkinWeights();

		// Interchange remaps weights by name when it merges meshes, so it needs the index-to-name
		// mapping this conversion used.
		OutJointNames = SkeletonBuilder.GetBoneNames();
	}

	// Reserving up front matters: without it every CreateVertexInstance can reallocate the attribute
	// arrays, which on a multi-million-triangle scan file dominates the import time.
	OutMeshDescription.ReserveNewVertices(TotalVertices);
	OutMeshDescription.ReserveNewVertexInstances(TotalTriangles * 3);
	OutMeshDescription.ReserveNewPolygons(TotalTriangles);
	OutMeshDescription.ReserveNewEdges(TotalTriangles * 3);

	const bool bFlipWinding = AxisConverter.ShouldFlipWinding();

	int32 SkippedNonTriangles = 0;
	int32 SkippedDegenerate = 0;
	int32 EmittedTriangles = 0;

	for (const int32 MeshIndex : MeshIndices)
	{
		const aiMesh& Mesh = *Scene.mMeshes[MeshIndex];

		// One polygon group per source mesh. Assimp gives each aiMesh exactly one material, whereas
		// Unreal models that as a mesh with several sections, so this is where the two models meet:
		// source mesh -> polygon group -> material slot -> mesh section.
		const FPolygonGroupID PolygonGroupID = OutMeshDescription.CreatePolygonGroup();

		FString SlotName = FAssimpAxisConverter::ConvertString(Mesh.mName);
		if (SlotName.IsEmpty())
		{
			SlotName = FString::Printf(TEXT("Material_%d"), Mesh.mMaterialIndex);
		}
		MaterialSlots[PolygonGroupID] = FName(*SlotName);

		// A morph target replaces the mesh's positions, and usually its normals, leaving everything
		// else to come from the base mesh. Assimp promises the replacement arrays have the same
		// vertex count; files do not always keep that promise, so it is checked rather than trusted.
		const aiVector3D* PositionSource = Mesh.mVertices;
		const aiVector3D* NormalSource = Mesh.mNormals;

		if (MorphTargetIndex != INDEX_NONE)
		{
			const aiAnimMesh* MorphTarget =
				(Mesh.mAnimMeshes != nullptr
					&& MorphTargetIndex >= 0
					&& static_cast<unsigned int>(MorphTargetIndex) < Mesh.mNumAnimMeshes)
				? Mesh.mAnimMeshes[MorphTargetIndex]
				: nullptr;

			if (MorphTarget == nullptr || MorphTarget->mNumVertices != Mesh.mNumVertices)
			{
				OutError = FString::Printf(
					TEXT("Morph target %d of mesh '%s' is missing or does not match the mesh's %u vertices."),
					MorphTargetIndex, *FAssimpAxisConverter::ConvertString(Mesh.mName), Mesh.mNumVertices);
				return false;
			}

			if (MorphTarget->mVertices != nullptr)
			{
				PositionSource = MorphTarget->mVertices;
			}
			if (MorphTarget->mNormals != nullptr)
			{
				NormalSource = MorphTarget->mNormals;
			}
		}

		// Invert Assimp's skinning layout before creating vertices.
		//
		// Assimp stores skinning bone-major: each aiBone lists the vertices it influences. Unreal
		// needs it vertex-major: each vertex lists its influences. Building the transpose once per
		// mesh is what avoids rescanning every bone's weight list for every vertex.
		TArray<TArray<UE::AnimationCore::FBoneWeight>> WeightsPerVertex;
		if (bSkinned && Mesh.HasBones())
		{
			WeightsPerVertex.SetNum(static_cast<int32>(Mesh.mNumVertices));

			int32 UnmappedBones = 0;
			for (unsigned int BoneIndex = 0; BoneIndex < Mesh.mNumBones; ++BoneIndex)
			{
				const aiBone* Bone = Mesh.mBones[BoneIndex];
				if (Bone == nullptr)
				{
					continue;
				}

				const FString BoneName = FAssimpAxisConverter::ConvertString(Bone->mName);
				const int32 SkeletonBoneIndex = SkeletonBuilder.FindBoneIndex(BoneName);
				if (SkeletonBoneIndex == INDEX_NONE)
				{
					// The builder skipped this bone, normally because it named a node the scene does
					// not contain. Its weights have nowhere to go.
					++UnmappedBones;
					continue;
				}

				for (unsigned int WeightIndex = 0; WeightIndex < Bone->mNumWeights; ++WeightIndex)
				{
					const aiVertexWeight& Weight = Bone->mWeights[WeightIndex];

					if (Weight.mVertexId >= Mesh.mNumVertices || Weight.mWeight <= 0.0f)
					{
						continue;
					}

					WeightsPerVertex[static_cast<int32>(Weight.mVertexId)].Emplace(
						static_cast<FBoneIndexType>(SkeletonBoneIndex),
						Weight.mWeight);
				}
			}

			if (UnmappedBones > 0)
			{
				UE_LOG(LogAssimp, Warning,
					TEXT("Mesh '%s': %d bone(s) could not be mapped into the skeleton; ")
					TEXT("their influences were dropped."),
					*FAssimpAxisConverter::ConvertString(Mesh.mName), UnmappedBones);
			}
		}

		// Assimp presents geometry as a shared vertex array indexed by faces, which maps directly
		// onto the mesh description's vertex/vertex-instance split: one vertex per Assimp vertex,
		// one vertex instance per face corner carrying the per-corner attributes.
		TArray<FVertexID> VertexIDs;
		VertexIDs.Reserve(static_cast<int32>(Mesh.mNumVertices));

		// Assimp's LimitBoneWeights step already caps and renormalises influences, so the settings
		// here only need to enforce Unreal's own limit for the case where that step was skipped.
		UE::AnimationCore::FBoneWeightsSettings WeightSettings;
		WeightSettings.SetMaxWeightCount(Settings.MaxBoneInfluencesPerVertex);
		WeightSettings.SetNormalizeType(UE::AnimationCore::EBoneWeightNormalizeType::Always);

		for (unsigned int Index = 0; Index < Mesh.mNumVertices; ++Index)
		{
			const FVertexID VertexID = OutMeshDescription.CreateVertex();
			VertexPositions[VertexID] = AxisConverter.ConvertPosition(PositionSource[Index]);
			VertexIDs.Add(VertexID);

			if (bSkinned)
			{
				const int32 VertexIndex = static_cast<int32>(Index);

				if (WeightsPerVertex.IsValidIndex(VertexIndex) && !WeightsPerVertex[VertexIndex].IsEmpty())
				{
					SkinWeights.Set(VertexID, UE::AnimationCore::FBoneWeights::Create(
						WeightsPerVertex[VertexIndex], WeightSettings));
				}
				else
				{
					// Every vertex of a skinned mesh must be bound to something. An unbound vertex
					// renders at the origin rather than merely staying still, so bind it rigidly to
					// the root -- the same fallback the engine's own importers use.
					SkinWeights.Set(VertexID, UE::AnimationCore::FBoneWeights::Create(
						{ UE::AnimationCore::FBoneWeight(FBoneIndexType(0), 1.0f) }, WeightSettings));
				}
			}
		}

		const bool bMeshHasNormals    = NormalSource != nullptr;
		const bool bMeshHasTangents   = Mesh.HasTangentsAndBitangents();
		const bool bMeshHasColors     = Mesh.HasVertexColors(VertexColorSetIndex);
		const int32 MeshUVChannels    = GetUsedUVChannelCount(Mesh);

		TArray<FVertexInstanceID, TInlineAllocator<3>> CornerInstances;

		for (unsigned int FaceIndex = 0; FaceIndex < Mesh.mNumFaces; ++FaceIndex)
		{
			const aiFace& Face = Mesh.mFaces[FaceIndex];

			// Triangulation is always requested, and SortByPType separates out points and lines, so
			// anything else here is an importer quirk rather than something to handle.
			if (Face.mNumIndices != 3 || Face.mIndices == nullptr)
			{
				++SkippedNonTriangles;
				continue;
			}

			const unsigned int I0 = Face.mIndices[0];
			const unsigned int I1 = Face.mIndices[1];
			const unsigned int I2 = Face.mIndices[2];

			if (I0 >= Mesh.mNumVertices || I1 >= Mesh.mNumVertices || I2 >= Mesh.mNumVertices)
			{
				++SkippedDegenerate;
				continue;
			}

			// A triangle referencing the same vertex twice has no area and would produce a
			// degenerate polygon that later mesh operations trip over.
			if (I0 == I1 || I1 == I2 || I0 == I2)
			{
				++SkippedDegenerate;
				continue;
			}

			// Reverse winding when the change of basis reversed handedness, otherwise every face
			// ends up backfacing.
			const unsigned int Ordered[3] = { I0, bFlipWinding ? I2 : I1, bFlipWinding ? I1 : I2 };

			CornerInstances.Reset();

			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const unsigned int SourceIndex = Ordered[Corner];
				const FVertexInstanceID InstanceID =
					OutMeshDescription.CreateVertexInstance(VertexIDs[static_cast<int32>(SourceIndex)]);

				if (bMeshHasNormals)
				{
					const FVector3f Normal = AxisConverter.ConvertDirection(NormalSource[SourceIndex]).GetSafeNormal();
					Normals[InstanceID] = Normal;

					if (bMeshHasTangents)
					{
						const FVector3f Tangent =
							AxisConverter.ConvertDirection(Mesh.mTangents[SourceIndex]).GetSafeNormal();
						const FVector3f Bitangent =
							AxisConverter.ConvertDirection(Mesh.mBitangents[SourceIndex]).GetSafeNormal();

						Tangents[InstanceID] = Tangent;
						BinormalSigns[InstanceID] = ComputeBinormalSign(Normal, Tangent, Bitangent);
					}
				}

				// Unreal's vertex colours are linear; Assimp's aiColor4D already is.
				if (bMeshHasColors)
				{
					const aiColor4D& Color = Mesh.mColors[VertexColorSetIndex][SourceIndex];
					Colors[InstanceID] = FVector4f(Color.r, Color.g, Color.b, Color.a);
				}
				else if (bAnyVertexColors)
				{
					// Another mesh in this merge has colours, so this one must be explicitly white
					// rather than left at the attribute default.
					Colors[InstanceID] = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
				}

				for (int32 Channel = 0; Channel < RequiredUVChannels; ++Channel)
				{
					// Test each channel individually rather than against a count. Assimp's UV
					// channels are permitted to be SPARSE: a mesh may populate channel 0 and
					// channel 2 while leaving channel 1 null, so "below the highest populated
					// channel" does not imply "populated". Indexing a null channel is an access
					// violation, which is exactly what Collada/box_nested_animation.dae produced.
					const bool bChannelPresent =
						Channel < MeshUVChannels &&
						Mesh.HasTextureCoords(static_cast<unsigned int>(Channel));

					const FVector2f UV = bChannelPresent
						? AxisConverter.ConvertUV(Mesh.mTextureCoords[Channel][SourceIndex])
						: FVector2f::ZeroVector;

					UVs.Set(InstanceID, Channel, UV);
				}

				CornerInstances.Add(InstanceID);
			}

			OutMeshDescription.CreatePolygon(PolygonGroupID, CornerInstances);
			++EmittedTriangles;
		}
	}

	if (EmittedTriangles == 0)
	{
		OutError = FString::Printf(
			TEXT("No usable triangles were produced (%d non-triangle face(s), %d degenerate face(s) skipped)."),
			SkippedNonTriangles, SkippedDegenerate);
		OutMeshDescription.Empty();
		return false;
	}

	if (SkippedNonTriangles > 0 || SkippedDegenerate > 0)
	{
		UE_LOG(LogAssimp, Warning,
			TEXT("Mesh conversion skipped %d non-triangle and %d degenerate face(s); kept %d triangle(s)."),
			SkippedNonTriangles, SkippedDegenerate, EmittedTriangles);
	}

	UE_LOG(LogAssimp, Verbose,
		TEXT("Converted %d mesh(es) to %d triangle(s), %d vertex/vertices, %d UV channel(s)."),
		MeshIndices.Num(), EmittedTriangles, TotalVertices, RequiredUVChannels);

	return true;
}
