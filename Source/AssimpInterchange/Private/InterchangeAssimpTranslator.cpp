// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "InterchangeAssimpTranslator.h"

#include "AssimpForUnrealSettings.h"
#include "AssimpImportSettings.h"
#include "AssimpInterchange.h"
#include "AssimpScene.h"
#include "AssimpSceneTypes.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "InterchangeAnimationTrackSetNode.h"
#include "InterchangeCommonAnimationPayload.h"
#include "InterchangeJointNode.h"
#include "InterchangeManager.h"
#include "InterchangeResult.h"
#include "InterchangeMaterialDefinitions.h"
#include "InterchangeMaterialInstanceNode.h"
#include "InterchangeMeshNode.h"
#include "InterchangeSceneNode.h"
#include "InterchangeSourceData.h"
#include "InterchangeTexture2DNode.h"
#include "InterchangeTranslatorHelper.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshAttributes.h"

#define LOCTEXT_NAMESPACE "InterchangeAssimpTranslator"

namespace AssimpInterchangePrivate
{
	/** Parent material every generated material instance derives from. */
	const TCHAR* const PBRParentMaterialPath =
		TEXT("/InterchangeAssets/Materials/PBRSurfaceMaterial_MR.PBRSurfaceMaterial_MR");

	/** Prefix distinguishing an embedded-texture payload key from a file path. */
	const TCHAR* const EmbeddedTexturePrefix = TEXT("assimp-embedded:");

	/** Node UID prefixes, kept together so the scheme is visible in one place. */
	const TCHAR* const MeshNodePrefix  = TEXT("\\Mesh\\");
	const TCHAR* const SceneNodePrefix = TEXT("\\Scene\\");

	FString MakeMeshNodeUid(int32 MeshIndex, const FString& MeshName)
	{
		// The index is part of the UID because mesh names are not unique in most formats -- Assimp
		// happily reports several meshes called "Cube" -- whereas the index always is.
		return FString::Printf(TEXT("%s%d_%s"), MeshNodePrefix, MeshIndex, *MeshName);
	}

	FString MakeSceneNodeUid(int32 NodeIndex, const FString& NodeName)
	{
		return FString::Printf(TEXT("%s%d_%s"), SceneNodePrefix, NodeIndex, *NodeName);
	}

	/** Track-set node UID for one clip driving one skeleton. */
	FString MakeSkeletalAnimationNodeUid(const FString& SkeletonRootUid, int32 AnimationIndex)
	{
		return FString::Printf(TEXT("\\SkeletalAnimation\\%s_%d"), *SkeletonRootUid, AnimationIndex);
	}

	/**
	 * Payload key for one bone track: the clip and the node it drives.
	 *
	 * Indices rather than names on purpose. Node names come from the file, may repeat, and may
	 * contain any character at all -- including whatever separator a key format picks -- so a
	 * name-based key is one oddly named bone away from being unparseable.
	 */
	FString MakeAnimationPayloadKey(int32 AnimationIndex, int32 NodeIndex)
	{
		return FString::Printf(TEXT("%d;%d"), AnimationIndex, NodeIndex);
	}

	/** Decodes a key produced by MakeAnimationPayloadKey. */
	bool ParseAnimationPayloadKey(const FString& Key, int32& OutAnimationIndex, int32& OutNodeIndex)
	{
		FString AnimationPart;
		FString NodePart;
		if (!Key.Split(TEXT(";"), &AnimationPart, &NodePart))
		{
			return false;
		}

		if (!AnimationPart.IsNumeric() || !NodePart.IsNumeric())
		{
			return false;
		}

		OutAnimationIndex = FCString::Atoi(*AnimationPart);
		OutNodeIndex = FCString::Atoi(*NodePart);
		return true;
	}

	/** Material instance node UID for a material index. */
	FString MakeMaterialNodeUid(int32 MaterialIndex, const FString& MaterialName)
	{
		const FString NodeName = FString::Printf(TEXT("%d_%s"), MaterialIndex, *MaterialName);
		return UInterchangeMaterialInstanceNode::MakeNodeUid(NodeName, TEXT(""));
	}

	/**
	 * Interchange parameter name for a given texture slot, or empty when the slot has no
	 * counterpart on the PBR parent material.
	 */
	FString GetParameterNameForSlot(EAssimpTextureSlot Slot)
	{
		using namespace UE::Interchange::Materials;

		switch (Slot)
		{
		case EAssimpTextureSlot::BaseColor:        return PBRMR::Parameters::BaseColor.ToString();
		case EAssimpTextureSlot::Metallic:         return PBRMR::Parameters::Metallic.ToString();
		case EAssimpTextureSlot::Roughness:        return PBRMR::Parameters::Roughness.ToString();
		case EAssimpTextureSlot::Specular:         return PBRMR::Parameters::Specular.ToString();
		case EAssimpTextureSlot::Normal:           return Common::Parameters::Normal.ToString();
		case EAssimpTextureSlot::Emissive:         return Common::Parameters::EmissiveColor.ToString();
		case EAssimpTextureSlot::Opacity:          return Common::Parameters::Opacity.ToString();
		case EAssimpTextureSlot::AmbientOcclusion: return Common::Parameters::Occlusion.ToString();
		case EAssimpTextureSlot::Displacement:     return Common::Parameters::Displacement.ToString();
		default:                                   return FString();
		}
	}

	/** True for slots whose texture must be sampled as linear data rather than sRGB colour. */
	bool IsNonColorSlot(EAssimpTextureSlot Slot)
	{
		switch (Slot)
		{
		case EAssimpTextureSlot::Normal:
		case EAssimpTextureSlot::Metallic:
		case EAssimpTextureSlot::Roughness:
		case EAssimpTextureSlot::AmbientOcclusion:
		case EAssimpTextureSlot::Displacement:
		case EAssimpTextureSlot::Height:
		case EAssimpTextureSlot::Opacity:
			return true;
		default:
			return false;
		}
	}

	/** Encodes a set of mesh indices as a payload key. */
	FString MakeMeshPayloadKey(TArrayView<const int32> MeshIndices)
	{
		TArray<FString> Parts;
		Parts.Reserve(MeshIndices.Num());
		for (const int32 Index : MeshIndices)
		{
			Parts.Add(FString::FromInt(Index));
		}
		return FString::Join(Parts, TEXT(","));
	}

	/** Decodes a payload key produced by MakeMeshPayloadKey. */
	bool ParseMeshPayloadKey(const FString& Key, TArray<int32>& OutMeshIndices)
	{
		OutMeshIndices.Reset();

		TArray<FString> Parts;
		Key.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty*/ true);

		for (const FString& Part : Parts)
		{
			const FString Trimmed = Part.TrimStartAndEnd();
			if (!Trimmed.IsNumeric())
			{
				return false;
			}
			OutMeshIndices.Add(FCString::Atoi(*Trimmed));
		}

		return OutMeshIndices.Num() > 0;
	}
}

const TCHAR* UInterchangeAssimpTranslator::GetEmbeddedTextureKeyPrefix()
{
	return AssimpInterchangePrivate::EmbeddedTexturePrefix;
}

UInterchangeAssimpTranslator::UInterchangeAssimpTranslator() = default;

EInterchangeTranslatorType UInterchangeAssimpTranslator::GetTranslatorType() const
{
	// Scenes rather than Assets: Assimp reports a node hierarchy for nearly every format, and
	// declaring it here is what lets the user choose between importing the assets alone and
	// recreating the hierarchy as actors in a level.
	return EInterchangeTranslatorType::Scenes;
}

EInterchangeTranslatorAssetType UInterchangeAssimpTranslator::GetSupportedAssetTypes() const
{
	return EInterchangeTranslatorAssetType::Meshes
		| EInterchangeTranslatorAssetType::Materials
		| EInterchangeTranslatorAssetType::Textures
		| EInterchangeTranslatorAssetType::Animations;
}

TArray<FString> UInterchangeAssimpTranslator::GetSupportedFormats() const
{
	const UAssimpForUnrealSettings* Settings = UAssimpForUnrealSettings::Get();
	if (Settings == nullptr)
	{
		return TArray<FString>();
	}

	const TArray<FString> Extensions = Settings->GetEffectiveExtensions();

	TArray<FString> Formats;
	Formats.Reserve(Extensions.Num());
	for (const FString& Extension : Extensions)
	{
		// Interchange expects "extension;Human readable description".
		Formats.Add(FString::Printf(TEXT("%s;%s file (Assimp)"), *Extension, *Extension.ToUpper()));
	}

	return Formats;
}

TSharedPtr<FAssimpScene> UInterchangeAssimpTranslator::EnsureSceneLoaded() const
{
	FScopeLock Lock(&SceneLock);

	if (CachedScene.IsValid())
	{
		return CachedScene;
	}

	const UInterchangeSourceData* Source = GetSourceData();
	if (Source == nullptr)
	{
		return nullptr;
	}

	const FString Filename = Source->GetFilename();
	if (Filename.IsEmpty())
	{
		return nullptr;
	}

	// Parsing options come from project settings, not from the import dialog. Interchange calls
	// Translate() before any pipeline options are applied, so a per-import choice could not reach
	// this point without re-parsing the file afterwards.
	FAssimpImportSettings Settings;
	if (const UAssimpForUnrealSettings* ProjectSettings = UAssimpForUnrealSettings::Get())
	{
		Settings = ProjectSettings->ImportSettings;
	}

	// Materials, textures and metadata are always translated. The pipeline decides what to actually
	// create from the node graph, and describing something it then ignores is cheap -- whereas
	// omitting it here could not be recovered downstream.
	Settings.bImportMaterials = true;
	Settings.bImportTextures = true;
	Settings.bImportMetadata = true;

	FAssimpLoadResult LoadResult;
	CachedScene = FAssimpScene::LoadFromFile(Filename, Settings, LoadResult);

	// Surface Assimp's diagnostics through Interchange so they appear in the import report rather
	// than only in the log.
	for (const FAssimpDiagnostic& Diagnostic : LoadResult.Diagnostics)
	{
		if (Diagnostic.Severity == EAssimpDiagnosticSeverity::Error)
		{
			UInterchangeResultError_Generic* Message = AddMessage<UInterchangeResultError_Generic>();
			Message->SourceAssetName = FPaths::GetCleanFilename(Filename);
			Message->Text = FText::FromString(Diagnostic.Message);
		}
		else if (Diagnostic.Severity == EAssimpDiagnosticSeverity::Warning)
		{
			UInterchangeResultWarning_Generic* Message = AddMessage<UInterchangeResultWarning_Generic>();
			Message->SourceAssetName = FPaths::GetCleanFilename(Filename);
			Message->Text = FText::FromString(Diagnostic.Message);
		}
	}

	if (!CachedScene.IsValid())
	{
		UInterchangeResultError_Generic* Message = AddMessage<UInterchangeResultError_Generic>();
		Message->SourceAssetName = FPaths::GetCleanFilename(Filename);
		Message->Text = FText::Format(
			LOCTEXT("AssimpReadFailed", "Assimp could not read this file: {0}"),
			FText::FromString(LoadResult.ErrorMessage));
	}

	return CachedScene;
}

void UInterchangeAssimpTranslator::ReleaseSource()
{
	FScopeLock Lock(&SceneLock);

	// Interchange calls this when the import finishes. Releasing here rather than waiting for
	// garbage collection matters: a parsed scene can be hundreds of megabytes, and the translator
	// object itself may survive much longer than the import that used it.
	CachedScene.Reset();
}

bool UInterchangeAssimpTranslator::Translate(UInterchangeBaseNodeContainer& BaseNodeContainer) const
{
	using namespace AssimpInterchangePrivate;

	const TSharedPtr<FAssimpScene> Scene = EnsureSceneLoaded();
	if (!Scene.IsValid())
	{
		return false;
	}

	const FAssimpSceneInfo& Info = Scene->GetSceneInfo();

	if (Info.Meshes.IsEmpty() && Info.Materials.IsEmpty())
	{
		UInterchangeResultError_Generic* Message = AddMessage<UInterchangeResultError_Generic>();
		Message->SourceAssetName = FPaths::GetCleanFilename(Info.SourceFilePath);
		Message->Text = LOCTEXT("EmptySceneError", "The file contains no meshes or materials.");
		return false;
	}

	// ---------------------------------------------------------------------------------------------
	// Materials and their textures
	// ---------------------------------------------------------------------------------------------
	TArray<FString> MaterialNodeUids;
	MaterialNodeUids.Reserve(Info.Materials.Num());

	for (int32 MaterialIndex = 0; MaterialIndex < Info.Materials.Num(); ++MaterialIndex)
	{
		const FAssimpMaterialInfo& Material = Info.Materials[MaterialIndex];

		const FString NodeUid = MakeMaterialNodeUid(MaterialIndex, Material.Name);
		MaterialNodeUids.Add(NodeUid);

		UInterchangeMaterialInstanceNode* MaterialNode =
			NewObject<UInterchangeMaterialInstanceNode>(&BaseNodeContainer);
		MaterialNode->SetCustomParent(PBRParentMaterialPath);
		BaseNodeContainer.SetupNode(
			MaterialNode, NodeUid, Material.Name, EInterchangeNodeContainerType::TranslatedAsset);

		using namespace UE::Interchange::Materials;

		MaterialNode->AddVectorParameterValue(PBRMR::Parameters::BaseColor.ToString(), Material.BaseColor);
		MaterialNode->AddScalarParameterValue(PBRMR::Parameters::Metallic.ToString(), Material.Metallic);
		MaterialNode->AddScalarParameterValue(PBRMR::Parameters::Roughness.ToString(), Material.Roughness);

		// Unreal treats Specular as reflectance / 0.08, so its neutral value is 0.5 and not 1.
		// FAssimpMaterialInfo already expresses the file's specular strength in that convention,
		// whichever material model the file used, so it is passed through rather than rescaled here.
		MaterialNode->AddScalarParameterValue(PBRMR::Parameters::Specular.ToString(), Material.Specular);
		MaterialNode->AddVectorParameterValue(Common::Parameters::EmissiveColor.ToString(), Material.EmissiveColor);

		if (Material.bIsTranslucent)
		{
			MaterialNode->AddScalarParameterValue(Common::Parameters::Opacity.ToString(), Material.Opacity);

			// BLEND_Translucent. Set explicitly so a partially transparent material does not import
			// as fully opaque.
			MaterialNode->SetCustomBlendMode(2);
		}

		MaterialNode->SetCustomTwoSided(Material.bTwoSided);

		for (const TPair<EAssimpTextureSlot, FAssimpTextureReference>& Pair : Material.Textures)
		{
			const FString ParameterName = GetParameterNameForSlot(Pair.Key);
			if (ParameterName.IsEmpty())
			{
				// A slot the PBR parent has no input for. Skipping is correct; warning about every
				// such slot would be noise on formats that routinely carry them.
				continue;
			}

			const FAssimpTextureReference& TextureRef = Pair.Value;

			// Build the payload key. External textures use their resolved path so the request can be
			// forwarded to the engine's image translators; embedded textures get a key only this
			// translator understands.
			FString PayloadKey;
			FString TextureName;

			if (TextureRef.Source == EAssimpTextureSource::Embedded)
			{
				if (TextureRef.EmbeddedIndex == INDEX_NONE)
				{
					continue;
				}
				PayloadKey = FString::Printf(TEXT("%s%d"), EmbeddedTexturePrefix, TextureRef.EmbeddedIndex);
				TextureName = FString::Printf(TEXT("%s_Embedded_%d"), *Material.Name, TextureRef.EmbeddedIndex);
			}
			else
			{
				FString ResolvedPath;
				if (!Scene->ResolveExternalTexturePath(TextureRef.Path, ResolvedPath))
				{
					// The file the material names is genuinely missing. Report it: a silently
					// untextured import is the single most confusing outcome for a user.
					UInterchangeResultWarning_Generic* Message = AddMessage<UInterchangeResultWarning_Generic>();
					Message->SourceAssetName = FPaths::GetCleanFilename(Info.SourceFilePath);
					Message->Text = FText::Format(
						LOCTEXT("MissingTexture", "Material '{0}' references a texture that could not be found: {1}"),
						FText::FromString(Material.Name),
						FText::FromString(TextureRef.Path));
					continue;
				}

				FPaths::NormalizeFilename(ResolvedPath);
				PayloadKey = ResolvedPath;
				TextureName = FPaths::GetBaseFilename(ResolvedPath);
			}

			if (TextureName.IsEmpty())
			{
				continue;
			}

			// Reuse an existing node when the same image is referenced twice, so a texture shared
			// between slots or materials imports once.
			const FString TextureNodeUid = UInterchangeTextureNode::MakeNodeUid(TextureName);
			const UInterchangeTexture2DNode* ExistingNode =
				Cast<const UInterchangeTexture2DNode>(BaseNodeContainer.GetNode(TextureNodeUid));

			if (ExistingNode == nullptr)
			{
				UInterchangeTexture2DNode* TextureNode =
					UInterchangeTexture2DNode::Create(&BaseNodeContainer, TextureNodeUid, TextureName);
				TextureNode->SetPayLoadKey(PayloadKey);

				// Normal, roughness, metallic and occlusion maps hold measurements, not colour.
				// Importing them as sRGB would apply a gamma curve to that data and visibly break
				// lighting, so the distinction is set here where the slot is still known.
				if (IsNonColorSlot(Pair.Key))
				{
					TextureNode->SetCustomSRGB(false);
				}

				ExistingNode = TextureNode;
			}

			const FString TextureParameterName = ParameterName + TEXT("Map");
			MaterialNode->AddTextureParameterValue(TextureParameterName, ExistingNode->GetUniqueID());
			MaterialNode->AddScalarParameterValue(TextureParameterName + TEXT("Weight"), 1.0f);
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Skeletons
	// ---------------------------------------------------------------------------------------------
	// Resolved before anything else is emitted, because it decides the shape of everything that
	// follows: which scene nodes become joints, which meshes become one skeletal mesh instead of
	// several static ones, and which skeleton each animation clip drives.
	TMap<FString, int32> NodeIndexByName;
	NodeIndexByName.Reserve(Info.Nodes.Num());
	for (int32 NodeIndex = 0; NodeIndex < Info.Nodes.Num(); ++NodeIndex)
	{
		// First occurrence wins. Node names are not unique in most formats, and a bone reference
		// names a node the same way Assimp itself resolves it: by the first match.
		NodeIndexByName.FindOrAdd(Info.Nodes[NodeIndex].Name, NodeIndex);
	}

	// Scene node UIDs are derived from index and name, so they can be spelled before the nodes
	// exist. That is what lets a skeletal mesh node name its skeleton root, which is emitted later.
	TArray<FString> SceneNodeUids;
	SceneNodeUids.Reserve(Info.Nodes.Num());
	for (int32 NodeIndex = 0; NodeIndex < Info.Nodes.Num(); ++NodeIndex)
	{
		SceneNodeUids.Add(MakeSceneNodeUid(NodeIndex, Info.Nodes[NodeIndex].Name));
	}

	TSet<FString> JointNodeNames;
	for (const FAssimpSkinnedMeshGroup& Group : Info.SkinnedMeshGroups)
	{
		for (const FAssimpSkeletonBone& Bone : Group.Bones)
		{
			JointNodeNames.Add(Bone.Name);
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Meshes
	// ---------------------------------------------------------------------------------------------
	// Every mesh maps to the node that carries it. Skinned meshes sharing a skeleton map to one
	// shared skeletal mesh node, which is why this is per mesh index rather than a parallel array.
	TArray<FString> MeshNodeUidForMesh;
	MeshNodeUidForMesh.SetNum(Info.Meshes.Num());

	TArray<FString> MeshNodeUids;
	MeshNodeUids.Reserve(Info.Meshes.Num());

	// Meshes claimed by a skeleton, so the static pass can skip them.
	TSet<int32> SkinnedMeshIndices;
	for (const FAssimpSkinnedMeshGroup& Group : Info.SkinnedMeshGroups)
	{
		SkinnedMeshIndices.Append(Group.MeshIndices);
	}

	/** Attaches a mesh node's material slots. Slot names must match what FAssimpMeshConverter writes. */
	auto BindMaterialSlots = [&Info, &MaterialNodeUids](UInterchangeMeshNode* MeshNode, int32 MeshIndex)
	{
		const FAssimpMeshInfo& Mesh = Info.Meshes[MeshIndex];
		if (!MaterialNodeUids.IsValidIndex(Mesh.MaterialIndex))
		{
			return;
		}

		const FString SlotName = Mesh.Name.IsEmpty()
			? FString::Printf(TEXT("Material_%d"), Mesh.MaterialIndex)
			: Mesh.Name;
		MeshNode->SetSlotMaterialDependencyUid(SlotName, MaterialNodeUids[Mesh.MaterialIndex]);
	};

	for (int32 MeshIndex = 0; MeshIndex < Info.Meshes.Num(); ++MeshIndex)
	{
		if (SkinnedMeshIndices.Contains(MeshIndex))
		{
			continue;
		}

		const FAssimpMeshInfo& Mesh = Info.Meshes[MeshIndex];

		const FString NodeUid = MakeMeshNodeUid(MeshIndex, Mesh.Name);
		MeshNodeUids.Add(NodeUid);
		MeshNodeUidForMesh[MeshIndex] = NodeUid;

		UInterchangeMeshNode* MeshNode = NewObject<UInterchangeMeshNode>(&BaseNodeContainer);
		BaseNodeContainer.SetupNode(
			MeshNode, NodeUid, Mesh.Name, EInterchangeNodeContainerType::TranslatedAsset);

		// The payload key carries the mesh indices to convert, so the expensive work happens only
		// for meshes the pipeline actually asks for.
		const int32 SingleIndex[1] = { MeshIndex };
		MeshNode->SetPayLoadKey(MakeMeshPayloadKey(SingleIndex), EInterchangeMeshPayLoadType::STATIC);

		MeshNode->SetCustomVertexCount(Mesh.NumVertices);
		MeshNode->SetCustomPolygonCount(Mesh.NumTriangles);
		MeshNode->SetCustomBoundingBox(Mesh.BoundingBox);
		MeshNode->SetCustomHasVertexNormal(Mesh.bHasNormals);
		MeshNode->SetCustomHasVertexTangent(Mesh.bHasTangents);
		MeshNode->SetCustomHasVertexBinormal(Mesh.bHasTangents);
		MeshNode->SetCustomHasVertexColor(Mesh.bHasVertexColors);
		MeshNode->SetCustomHasSmoothGroup(false);
		MeshNode->SetSkinnedMesh(false);

		BindMaterialSlots(MeshNode, MeshIndex);
	}

	// One skeletal mesh node per skeleton, merging every mesh bound to it.
	//
	// Merging is not an optimisation: skin weights are written as indices into the skeleton the
	// conversion built, so two meshes sharing a skeleton must be converted together or their weights
	// index two different bone orderings.
	for (const FAssimpSkinnedMeshGroup& Group : Info.SkinnedMeshGroups)
	{
		const int32* RootNodeIndex = NodeIndexByName.Find(Group.RootBoneName);
		if (RootNodeIndex == nullptr || !SceneNodeUids.IsValidIndex(*RootNodeIndex))
		{
			// The skeleton names a node the flattened hierarchy does not contain, which can only
			// happen if the node walk hit its depth cap. Fall back to static import for these
			// meshes rather than emitting a skeletal mesh with no skeleton to bind to.
			UInterchangeResultWarning_Generic* Message = AddMessage<UInterchangeResultWarning_Generic>();
			Message->SourceAssetName = FPaths::GetCleanFilename(Info.SourceFilePath);
			Message->Text = FText::Format(
				LOCTEXT("MissingSkeletonRoot",
					"Skeleton root '{0}' is not present in the scene hierarchy; its meshes were imported as static geometry."),
				FText::FromString(Group.RootBoneName));
			continue;
		}

		const FAssimpMeshInfo& FirstMesh = Info.Meshes[Group.MeshIndices[0]];

		// Named after the mesh and its skeleton root, matching what the engine's own glTF translator
		// does: two characters sharing a mesh name still produce two distinguishable assets.
		const FString NodeName = FString::Printf(TEXT("%s_%s"), *FirstMesh.Name, *Group.RootBoneName);
		const FString NodeUid = MakeMeshNodeUid(Group.MeshIndices[0], NodeName);

		UInterchangeMeshNode* MeshNode = NewObject<UInterchangeMeshNode>(&BaseNodeContainer);
		BaseNodeContainer.SetupNode(
			MeshNode, NodeUid, NodeName, EInterchangeNodeContainerType::TranslatedAsset);

		MeshNode->SetPayLoadKey(
			MakeMeshPayloadKey(Group.MeshIndices), EInterchangeMeshPayLoadType::SKELETAL);
		MeshNode->SetSkinnedMesh(true);
		MeshNode->SetSkeletonDependencyUid(SceneNodeUids[*RootNodeIndex]);

		int32 TotalVertices = 0;
		int32 TotalTriangles = 0;
		bool bHasNormals = false;
		bool bHasTangents = false;
		bool bHasVertexColors = false;
		FBox Bounds(ForceInit);

		for (const int32 MeshIndex : Group.MeshIndices)
		{
			const FAssimpMeshInfo& Mesh = Info.Meshes[MeshIndex];
			TotalVertices += Mesh.NumVertices;
			TotalTriangles += Mesh.NumTriangles;
			bHasNormals |= Mesh.bHasNormals;
			bHasTangents |= Mesh.bHasTangents;
			bHasVertexColors |= Mesh.bHasVertexColors;
			if (Mesh.BoundingBox.IsValid)
			{
				Bounds += Mesh.BoundingBox;
			}

			MeshNodeUidForMesh[MeshIndex] = NodeUid;
			BindMaterialSlots(MeshNode, MeshIndex);
		}

		MeshNode->SetCustomVertexCount(TotalVertices);
		MeshNode->SetCustomPolygonCount(TotalTriangles);
		MeshNode->SetCustomBoundingBox(Bounds);
		MeshNode->SetCustomHasVertexNormal(bHasNormals);
		MeshNode->SetCustomHasVertexTangent(bHasTangents);
		MeshNode->SetCustomHasVertexBinormal(bHasTangents);
		MeshNode->SetCustomHasVertexColor(bHasVertexColors);
		MeshNode->SetCustomHasSmoothGroup(false);

		MeshNodeUids.Add(NodeUid);
	}

	// ---------------------------------------------------------------------------------------------
	// Scene hierarchy
	// ---------------------------------------------------------------------------------------------
	// FAssimpSceneInfo guarantees a parent always precedes its children, so a single forward pass
	// can attach each node to an already-created parent.
	for (int32 NodeIndex = 0; NodeIndex < Info.Nodes.Num(); ++NodeIndex)
	{
		const FAssimpNodeInfo& Node = Info.Nodes[NodeIndex];

		const FString& NodeUid = SceneNodeUids[NodeIndex];

		const FString ParentUid = Info.Nodes.IsValidIndex(Node.ParentIndex)
			? SceneNodeUids[Node.ParentIndex]
			: FString();

		// A bone becomes a joint node rather than a plain scene node. That type is load-bearing: the
		// skeletal mesh pipeline finds a skeleton by looking for a joint node whose parent is not
		// one, so without it there is no skeleton, and therefore no skeletal mesh and no animation.
		const bool bIsJoint = JointNodeNames.Contains(Node.Name);

		UInterchangeSceneNode* SceneNode = bIsJoint
			? NewObject<UInterchangeJointNode>(&BaseNodeContainer)
			: NewObject<UInterchangeSceneNode>(&BaseNodeContainer);

		BaseNodeContainer.SetupNode(
			SceneNode, NodeUid, Node.Name, EInterchangeNodeContainerType::TranslatedScene, ParentUid);

		SceneNode->SetCustomLocalTransform(&BaseNodeContainer, Node.LocalTransform);

		if (bIsJoint)
		{
			UInterchangeJointNode* JointNode = CastChecked<UInterchangeJointNode>(SceneNode);

			// Assimp exposes no separate bind pose: a bone's offset matrix is the inverse of its
			// global bind transform, and the skeleton reconstruction already uses the node transforms
			// as the reference pose. Declaring bind pose and time-zero pose as the same thing is
			// therefore the truth about the data, and it also keeps the pipeline off its
			// "rebind using time zero" fallback, which exists for files that contradict themselves.
			JointNode->SetBindPoseLocalTransform(&BaseNodeContainer, Node.LocalTransform);
			JointNode->SetTimeZeroLocalTransform(&BaseNodeContainer, Node.LocalTransform);
		}

		// Meshes a node draws, deduplicated: several source meshes of one skeleton resolve to the
		// same skeletal mesh node, and instancing it twice would import the asset twice.
		TArray<FString> AssetUids;
		for (const int32 MeshIndex : Node.MeshIndices)
		{
			if (MeshNodeUidForMesh.IsValidIndex(MeshIndex) && !MeshNodeUidForMesh[MeshIndex].IsEmpty())
			{
				AssetUids.AddUnique(MeshNodeUidForMesh[MeshIndex]);
			}
		}

		// A node drawing exactly one mesh maps straight onto a single mesh actor. A node drawing
		// several needs a child per mesh, because an Interchange scene node references at most one
		// asset instance.
		if (AssetUids.Num() == 1)
		{
			SceneNode->SetCustomAssetInstanceUid(AssetUids[0]);
		}
		else if (AssetUids.Num() > 1)
		{
			for (int32 AssetOrdinal = 0; AssetOrdinal < AssetUids.Num(); ++AssetOrdinal)
			{
				const FString ChildUid = FString::Printf(TEXT("%s_Mesh%d"), *NodeUid, AssetOrdinal);

				UInterchangeSceneNode* MeshHolder = NewObject<UInterchangeSceneNode>(&BaseNodeContainer);
				BaseNodeContainer.SetupNode(
					MeshHolder,
					ChildUid,
					FString::Printf(TEXT("%s_%d"), *Node.Name, AssetOrdinal),
					EInterchangeNodeContainerType::TranslatedScene,
					NodeUid);

				// Identity: the parent already carries the placement.
				MeshHolder->SetCustomLocalTransform(&BaseNodeContainer, FTransform::Identity);
				MeshHolder->SetCustomAssetInstanceUid(AssetUids[AssetOrdinal]);
			}
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Animation
	// ---------------------------------------------------------------------------------------------
	const int32 AnimationTrackCount =
		BuildAnimationTracks(BaseNodeContainer, *Scene, NodeIndexByName, SceneNodeUids);

	UE_LOG(LogAssimpInterchange, Log,
		TEXT("Translated '%s': %d mesh node(s) (%d skeletal), %d material node(s), %d scene node(s), ")
		TEXT("%d animation track set(s)."),
		*FPaths::GetCleanFilename(Info.SourceFilePath),
		MeshNodeUids.Num(), Info.SkinnedMeshGroups.Num(),
		MaterialNodeUids.Num(), SceneNodeUids.Num(), AnimationTrackCount);

	return true;
}

int32 UInterchangeAssimpTranslator::BuildAnimationTracks(
	UInterchangeBaseNodeContainer& BaseNodeContainer,
	const FAssimpScene& Scene,
	const TMap<FString, int32>& NodeIndexByName,
	const TArray<FString>& SceneNodeUids) const
{
	using namespace AssimpInterchangePrivate;

	const FAssimpSceneInfo& Info = Scene.GetSceneInfo();

	if (Info.Animations.IsEmpty() || Info.SkinnedMeshGroups.IsEmpty())
	{
		// Clips that drive nothing skinned are dropped rather than imported as empty sequences.
		// A UAnimSequence exists only against a USkeleton, so with no skeleton there is nothing to
		// create; node-only animation would be a level-sequence import, which this translator does
		// not claim to do.
		if (!Info.Animations.IsEmpty())
		{
			UE_LOG(LogAssimpInterchange, Log,
				TEXT("'%s' has %d animation clip(s) but no skinned mesh; no animation was translated."),
				*FPaths::GetCleanFilename(Info.SourceFilePath), Info.Animations.Num());
		}
		return 0;
	}

	int32 TrackSetCount = 0;

	for (int32 AnimationIndex = 0; AnimationIndex < Info.Animations.Num(); ++AnimationIndex)
	{
		const FAssimpAnimationInfo& Animation = Info.Animations[AnimationIndex];

		// A clip is sampled at one rate over one range, chosen once here so that every bone in it is
		// baked on the same frames. The pipeline may override both; these are the defaults it reads
		// when the user asks for the file's own timing.
		const double SampleRate = Scene.GetAnimationSampleRate(AnimationIndex);
		const double StopTime = FMath::Max<double>(
			Animation.DurationSeconds, (SampleRate > 0.0) ? 1.0 / SampleRate : 0.0);

		const TSet<FString> AnimatedNodes(Animation.AnimatedNodeNames);

		for (const FAssimpSkinnedMeshGroup& Group : Info.SkinnedMeshGroups)
		{
			const int32* RootNodeIndex = NodeIndexByName.Find(Group.RootBoneName);
			if (RootNodeIndex == nullptr || !SceneNodeUids.IsValidIndex(*RootNodeIndex))
			{
				continue;
			}

			// Only the bones this clip actually moves get a payload. A clip that touches none of a
			// skeleton's bones produces no track set for it at all, which is what stops a two-rig
			// file importing every clip twice.
			TArray<TPair<FString, FString>> BonePayloads;
			for (const FAssimpSkeletonBone& Bone : Group.Bones)
			{
				if (!AnimatedNodes.Contains(Bone.Name))
				{
					continue;
				}

				const int32* BoneNodeIndex = NodeIndexByName.Find(Bone.Name);
				if (BoneNodeIndex == nullptr || !SceneNodeUids.IsValidIndex(*BoneNodeIndex))
				{
					continue;
				}

				BonePayloads.Emplace(
					SceneNodeUids[*BoneNodeIndex],
					MakeAnimationPayloadKey(AnimationIndex, *BoneNodeIndex));
			}

			if (BonePayloads.IsEmpty())
			{
				continue;
			}

			const FString& SkeletonRootUid = SceneNodeUids[*RootNodeIndex];

			UInterchangeSkeletalAnimationTrackNode* TrackNode =
				NewObject<UInterchangeSkeletalAnimationTrackNode>(&BaseNodeContainer);

			BaseNodeContainer.SetupNode(
				TrackNode,
				MakeSkeletalAnimationNodeUid(SkeletonRootUid, AnimationIndex),
				Animation.Name,
				EInterchangeNodeContainerType::TranslatedAsset);

			// The skeleton is named by its root joint's scene node. The animation pipeline derives
			// the skeleton factory node's UID from exactly this, which is how a clip and the mesh
			// that defines the skeleton end up on the same USkeleton.
			TrackNode->SetCustomSkeletonNodeUid(SkeletonRootUid);

			TrackNode->SetCustomAnimationSampleRate(SampleRate);
			TrackNode->SetCustomAnimationStartTime(0.0);
			TrackNode->SetCustomAnimationStopTime(StopTime);

			for (const TPair<FString, FString>& BonePayload : BonePayloads)
			{
				TrackNode->SetAnimationPayloadKeyForSceneNodeUid(
					BonePayload.Key, BonePayload.Value, EInterchangeAnimationPayLoadType::BAKED);
			}

			++TrackSetCount;
		}

		if (Animation.bHasMeshOrMorphChannels)
		{
			// Said once per clip, because the alternative is a clip that imports looking complete
			// while the deformation the artist authored is simply absent.
			UInterchangeResultWarning_Generic* Message = AddMessage<UInterchangeResultWarning_Generic>();
			Message->SourceAssetName = FPaths::GetCleanFilename(Info.SourceFilePath);
			Message->Text = FText::Format(
				LOCTEXT("MorphChannelsDropped",
					"Animation '{0}' also animates mesh or morph-target channels. Those are not imported; only bone tracks were."),
				FText::FromString(Animation.Name));
		}
	}

	return TrackSetCount;
}

// =================================================================================================
// Mesh payload
// =================================================================================================

TOptional<UE::Interchange::FMeshPayloadData> UInterchangeAssimpTranslator::GetMeshPayloadData(
	const FInterchangeMeshPayLoadKey& PayLoadKey,
	const UE::Interchange::FAttributeStorage& PayloadAttributes) const
{
	using namespace AssimpInterchangePrivate;
	using namespace UE::Interchange;

	const TSharedPtr<FAssimpScene> Scene = EnsureSceneLoaded();
	if (!Scene.IsValid())
	{
		return TOptional<FMeshPayloadData>();
	}

	TArray<int32> MeshIndices;
	if (!ParseMeshPayloadKey(PayLoadKey.UniqueId, MeshIndices))
	{
		UE_LOG(LogAssimpInterchange, Error,
			TEXT("Unrecognised mesh payload key '%s'."), *PayLoadKey.UniqueId);
		return TOptional<FMeshPayloadData>();
	}

	FMeshPayloadData PayloadData;

	// JointNames is what lets Interchange remap skin weights when it merges several meshes into one
	// skeletal mesh: the weights are written as bone indices, and only this mapping says what those
	// indices meant.
	TArray<FString> JointNames;
	if (!Scene->GetMergedMeshDescription(MeshIndices, PayloadData.MeshDescription, JointNames))
	{
		return TOptional<FMeshPayloadData>();
	}

	PayloadData.JointNames = MoveTemp(JointNames);

	// Interchange asks for the mesh baked into a given space when it is combining meshes or baking a
	// node hierarchy into one asset. Applying it here keeps that concern out of AssimpCore.
	// FAttributeKey is fully qualified because a same-named type exists in the global namespace, and
	// the `using namespace UE::Interchange` above makes the unqualified name ambiguous.
	FTransform MeshGlobalTransform = FTransform::Identity;
	PayloadAttributes.GetAttribute(
		UE::Interchange::FAttributeKey{ MeshPayload::Attributes::MeshGlobalTransform },
		MeshGlobalTransform);

	if (!MeshGlobalTransform.Equals(FTransform::Identity))
	{
		FStaticMeshAttributes Attributes(PayloadData.MeshDescription);
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();

		for (const FVertexID VertexID : PayloadData.MeshDescription.Vertices().GetElementIDs())
		{
			Positions[VertexID] =
				FVector3f(MeshGlobalTransform.TransformPosition(FVector(Positions[VertexID])));
		}

		// Directions use TransformVectorNoScale so a non-uniform scale cannot leave them
		// non-normalised, and the winding stays consistent with them.
		for (const FVertexInstanceID InstanceID : PayloadData.MeshDescription.VertexInstances().GetElementIDs())
		{
			Normals[InstanceID] = FVector3f(
				MeshGlobalTransform.TransformVectorNoScale(FVector(Normals[InstanceID]))).GetSafeNormal();
			Tangents[InstanceID] = FVector3f(
				MeshGlobalTransform.TransformVectorNoScale(FVector(Tangents[InstanceID]))).GetSafeNormal();
		}
	}

	return PayloadData;
}

// =================================================================================================
// Animation payload
// =================================================================================================

TArray<UE::Interchange::FAnimationPayloadData> UInterchangeAssimpTranslator::GetAnimationPayloadData(
	const TArray<UE::Interchange::FAnimationPayloadQuery>& PayloadQueries) const
{
	using namespace AssimpInterchangePrivate;
	using namespace UE::Interchange;

	TArray<FAnimationPayloadData> Payloads;

	const TSharedPtr<FAssimpScene> Scene = EnsureSceneLoaded();
	if (!Scene.IsValid())
	{
		return Payloads;
	}

	const FAssimpSceneInfo& Info = Scene->GetSceneInfo();

	Payloads.Reserve(PayloadQueries.Num());

	for (const FAnimationPayloadQuery& Query : PayloadQueries)
	{
		// Only baked transforms are offered, and the track nodes only ever ask for them. Anything
		// else means the graph and this provider have drifted apart, which is worth saying out loud.
		if (Query.PayloadKey.Type != EInterchangeAnimationPayLoadType::BAKED)
		{
			UE_LOG(LogAssimpInterchange, Warning,
				TEXT("Animation payload '%s' requested as type %d; only baked transforms are produced."),
				*Query.PayloadKey.UniqueId, static_cast<int32>(Query.PayloadKey.Type));
			continue;
		}

		int32 AnimationIndex = INDEX_NONE;
		int32 NodeIndex = INDEX_NONE;
		if (!ParseAnimationPayloadKey(Query.PayloadKey.UniqueId, AnimationIndex, NodeIndex))
		{
			UE_LOG(LogAssimpInterchange, Error,
				TEXT("Unrecognised animation payload key '%s'."), *Query.PayloadKey.UniqueId);
			continue;
		}

		if (!Info.Nodes.IsValidIndex(NodeIndex))
		{
			continue;
		}

		// The pipeline owns the timing: it may have been told to resample at a different rate or to
		// import a sub-range, and the payload must follow that rather than the file's own. Falling
		// back to the clip's declared rate covers a query that leaves it unset, which would
		// otherwise bake a single frame.
		double BakeFrequency = Query.TimeDescription.BakeFrequency;
		if (!(BakeFrequency > 0.0))
		{
			BakeFrequency = Scene->GetAnimationSampleRate(AnimationIndex);
		}

		double RangeStart = Query.TimeDescription.RangeStartSecond;
		double RangeStop = Query.TimeDescription.RangeStopSecond;
		if (RangeStop <= RangeStart)
		{
			RangeStart = 0.0;
			RangeStop = Info.Animations.IsValidIndex(AnimationIndex)
				? Info.Animations[AnimationIndex].DurationSeconds
				: 0.0;
		}

		FAnimationPayloadData PayloadData(Query.SceneNodeUniqueID, Query.PayloadKey);
		PayloadData.BakeFrequency = BakeFrequency;
		PayloadData.RangeStartTime = RangeStart;
		PayloadData.RangeEndTime = RangeStop;

		if (!Scene->GetBakedAnimationTrack(
				AnimationIndex,
				Info.Nodes[NodeIndex].Name,
				BakeFrequency,
				RangeStart,
				RangeStop,
				PayloadData.Transforms))
		{
			continue;
		}

		Payloads.Add(MoveTemp(PayloadData));
	}

	return Payloads;
}

// =================================================================================================
// Texture payload
// =================================================================================================

TOptional<UE::Interchange::FImportImage> UInterchangeAssimpTranslator::GetTexturePayloadData(
	const FString& PayloadKey,
	TOptional<FString>& AlternateTexturePath) const
{
	using namespace AssimpInterchangePrivate;
	using namespace UE::Interchange;

	// External texture: hand the file to whichever engine translator owns that image format rather
	// than decoding it here. That inherits the engine's full format coverage, its colour-space
	// handling, and any future format it gains.
	if (!PayloadKey.StartsWith(EmbeddedTexturePrefix))
	{
		AlternateTexturePath = PayloadKey;

		Private::FScopedTranslator ScopedTranslator(PayloadKey, Results, AnalyticsHandler);
		const IInterchangeTexturePayloadInterface* TextureTranslator =
			ScopedTranslator.GetPayLoadInterface<IInterchangeTexturePayloadInterface>();

		if (TextureTranslator == nullptr)
		{
			UE_LOG(LogAssimpInterchange, Warning,
				TEXT("No engine translator handles the texture '%s'."), *PayloadKey);
			return TOptional<FImportImage>();
		}

		return TextureTranslator->GetTexturePayloadData(PayloadKey, AlternateTexturePath);
	}

	// Embedded texture: there is no file to delegate, so decode it ourselves.
	const TSharedPtr<FAssimpScene> Scene = EnsureSceneLoaded();
	if (!Scene.IsValid())
	{
		return TOptional<FImportImage>();
	}

	const FString IndexPart = PayloadKey.RightChop(FCString::Strlen(EmbeddedTexturePrefix));
	if (!IndexPart.IsNumeric())
	{
		UE_LOG(LogAssimpInterchange, Error,
			TEXT("Malformed embedded texture payload key '%s'."), *PayloadKey);
		return TOptional<FImportImage>();
	}

	FAssimpEmbeddedTexture EmbeddedTexture;
	if (!Scene->GetEmbeddedTexture(FCString::Atoi(*IndexPart), EmbeddedTexture))
	{
		UE_LOG(LogAssimpInterchange, Error,
			TEXT("Could not read embedded texture for payload key '%s'."), *PayloadKey);
		return TOptional<FImportImage>();
	}

	FImportImage Image;

	if (EmbeddedTexture.bIsCompressed)
	{
		// Assimp hands back the original file bytes, so an image wrapper decodes them exactly as the
		// engine would have decoded the same file from disk.
		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

		const EImageFormat DetectedFormat = ImageWrapperModule.DetectImageFormat(
			EmbeddedTexture.CompressedData.GetData(), EmbeddedTexture.CompressedData.Num());

		if (DetectedFormat == EImageFormat::Invalid)
		{
			UE_LOG(LogAssimpInterchange, Warning,
				TEXT("Embedded texture '%s' has an unrecognised image format (hint '%s')."),
				*EmbeddedTexture.Name, *EmbeddedTexture.FormatHint);
			return TOptional<FImportImage>();
		}

		const TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(DetectedFormat);
		if (!ImageWrapper.IsValid() ||
			!ImageWrapper->SetCompressed(EmbeddedTexture.CompressedData.GetData(),
				EmbeddedTexture.CompressedData.Num()))
		{
			UE_LOG(LogAssimpInterchange, Warning,
				TEXT("Could not decode embedded texture '%s'."), *EmbeddedTexture.Name);
			return TOptional<FImportImage>();
		}

		TArray64<uint8> RawData;
		if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
		{
			UE_LOG(LogAssimpInterchange, Warning,
				TEXT("Could not convert embedded texture '%s' to BGRA8."), *EmbeddedTexture.Name);
			return TOptional<FImportImage>();
		}

		// Init2DWithParams allocates RawData (an FUniqueBuffer) for us, so copy the decoded pixels
		// into the view it hands back rather than assigning a buffer of our own.
		Image.Init2DWithParams(
			ImageWrapper->GetWidth(),
			ImageWrapper->GetHeight(),
			TSF_BGRA8,
			/*bInSRGB*/ true);

		const TArrayView64<uint8> Destination = Image.GetArrayViewOfRawData();
		if (Destination.Num() != RawData.Num())
		{
			UE_LOG(LogAssimpInterchange, Warning,
				TEXT("Decoded size mismatch for embedded texture '%s': expected %lld bytes, got %lld."),
				*EmbeddedTexture.Name, Destination.Num(), RawData.Num());
			return TOptional<FImportImage>();
		}

		FMemory::Memcpy(Destination.GetData(), RawData.GetData(), RawData.Num());
	}
	else
	{
		if (EmbeddedTexture.RawPixels.IsEmpty())
		{
			return TOptional<FImportImage>();
		}

		Image.Init2DWithParams(
			EmbeddedTexture.Width,
			EmbeddedTexture.Height,
			TSF_BGRA8,
			/*bInSRGB*/ true);

		// FColor is laid out BGRA in memory, matching TSF_BGRA8, so the pixels copy wholesale.
		const int64 SourceBytes = static_cast<int64>(EmbeddedTexture.RawPixels.Num()) * sizeof(FColor);
		const TArrayView64<uint8> Destination = Image.GetArrayViewOfRawData();

		if (Destination.Num() != SourceBytes)
		{
			UE_LOG(LogAssimpInterchange, Warning,
				TEXT("Size mismatch for embedded texture '%s': expected %lld bytes, got %lld."),
				*EmbeddedTexture.Name, Destination.Num(), SourceBytes);
			return TOptional<FImportImage>();
		}

		FMemory::Memcpy(Destination.GetData(), EmbeddedTexture.RawPixels.GetData(), SourceBytes);
	}

	return Image;
}

#undef LOCTEXT_NAMESPACE
