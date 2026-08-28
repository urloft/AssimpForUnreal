// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "InterchangeAssimpTranslator.h"

#include "AssimpForUnrealSettings.h"
#include "AssimpImportSettings.h"
#include "AssimpInterchange.h"
#include "AssimpScene.h"
#include "AssimpSceneTypes.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
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
		| EInterchangeTranslatorAssetType::Textures;
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
	// Meshes
	// ---------------------------------------------------------------------------------------------
	TArray<FString> MeshNodeUids;
	MeshNodeUids.Reserve(Info.Meshes.Num());

	for (int32 MeshIndex = 0; MeshIndex < Info.Meshes.Num(); ++MeshIndex)
	{
		const FAssimpMeshInfo& Mesh = Info.Meshes[MeshIndex];

		const FString NodeUid = MakeMeshNodeUid(MeshIndex, Mesh.Name);
		MeshNodeUids.Add(NodeUid);

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
		MeshNode->SetSkinnedMesh(Mesh.bHasBones);

		// Bind the mesh's material slot. The slot name must match the polygon group's imported
		// material slot name that FAssimpMeshConverter writes, or the assignment silently misses.
		if (MaterialNodeUids.IsValidIndex(Mesh.MaterialIndex))
		{
			const FString SlotName = Mesh.Name.IsEmpty()
				? FString::Printf(TEXT("Material_%d"), Mesh.MaterialIndex)
				: Mesh.Name;
			MeshNode->SetSlotMaterialDependencyUid(SlotName, MaterialNodeUids[Mesh.MaterialIndex]);
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Scene hierarchy
	// ---------------------------------------------------------------------------------------------
	// FAssimpSceneInfo guarantees a parent always precedes its children, so a single forward pass
	// can attach each node to an already-created parent.
	TArray<FString> SceneNodeUids;
	SceneNodeUids.SetNum(Info.Nodes.Num());

	for (int32 NodeIndex = 0; NodeIndex < Info.Nodes.Num(); ++NodeIndex)
	{
		const FAssimpNodeInfo& Node = Info.Nodes[NodeIndex];

		const FString NodeUid = MakeSceneNodeUid(NodeIndex, Node.Name);
		SceneNodeUids[NodeIndex] = NodeUid;

		const FString ParentUid = Info.Nodes.IsValidIndex(Node.ParentIndex)
			? SceneNodeUids[Node.ParentIndex]
			: FString();

		UInterchangeSceneNode* SceneNode = NewObject<UInterchangeSceneNode>(&BaseNodeContainer);
		BaseNodeContainer.SetupNode(
			SceneNode, NodeUid, Node.Name, EInterchangeNodeContainerType::TranslatedScene, ParentUid);

		SceneNode->SetCustomLocalTransform(&BaseNodeContainer, Node.LocalTransform);

		// A node drawing exactly one mesh maps straight onto a single mesh actor. A node drawing
		// several needs a child per mesh, because an Interchange scene node references at most one
		// asset instance.
		if (Node.MeshIndices.Num() == 1 && MeshNodeUids.IsValidIndex(Node.MeshIndices[0]))
		{
			SceneNode->SetCustomAssetInstanceUid(MeshNodeUids[Node.MeshIndices[0]]);
		}
		else if (Node.MeshIndices.Num() > 1)
		{
			for (const int32 MeshIndex : Node.MeshIndices)
			{
				if (!MeshNodeUids.IsValidIndex(MeshIndex))
				{
					continue;
				}

				const FString ChildUid = FString::Printf(TEXT("%s_Mesh%d"), *NodeUid, MeshIndex);

				UInterchangeSceneNode* MeshHolder = NewObject<UInterchangeSceneNode>(&BaseNodeContainer);
				BaseNodeContainer.SetupNode(
					MeshHolder,
					ChildUid,
					Info.Meshes[MeshIndex].Name,
					EInterchangeNodeContainerType::TranslatedScene,
					NodeUid);

				// Identity: the parent already carries the placement.
				MeshHolder->SetCustomLocalTransform(&BaseNodeContainer, FTransform::Identity);
				MeshHolder->SetCustomAssetInstanceUid(MeshNodeUids[MeshIndex]);
			}
		}
	}

	UE_LOG(LogAssimpInterchange, Log,
		TEXT("Translated '%s': %d mesh node(s), %d material node(s), %d scene node(s)."),
		*FPaths::GetCleanFilename(Info.SourceFilePath),
		MeshNodeUids.Num(), MaterialNodeUids.Num(), SceneNodeUids.Num());

	return true;
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
