// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "InterchangeTranslatorBase.h"
#include "Mesh/InterchangeMeshPayload.h"
#include "Mesh/InterchangeMeshPayloadInterface.h"
#include "Nodes/InterchangeBaseNodeContainer.h"
#include "Texture/InterchangeTexturePayloadInterface.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "InterchangeAssimpTranslator.generated.h"

class FAssimpScene;

/**
 * Interchange translator backed by Assimp.
 *
 * Translating into Interchange's node graph -- rather than building meshes directly -- is what makes
 * an Assimp import produce genuine Unreal assets. The engine's existing factories and pipelines then
 * supply Nanite, LOD generation, collision building, lightmap UVs, material instancing, and reimport,
 * none of which this plugin has to implement or keep working across engine versions.
 *
 * Division of labour
 * ------------------
 * Translate() is the cheap pass: it walks the scene Assimp parsed and emits nodes describing what is
 * in the file, carrying no vertex data. Interchange then requests payloads only for the assets its
 * pipeline actually decided to import, and GetMeshPayloadData does the expensive conversion at that
 * point. A file with fifty meshes of which the user imports two therefore converts two.
 *
 * Both payload entry points delegate to AssimpCore, so the geometry path is shared with the runtime
 * import API and cannot diverge from it.
 */
UCLASS(BlueprintType)
class ASSIMPINTERCHANGE_API UInterchangeAssimpTranslator : public UInterchangeTranslatorBase
	, public IInterchangeMeshPayloadInterface
	, public IInterchangeTexturePayloadInterface
{
	GENERATED_BODY()

public:
	UInterchangeAssimpTranslator();

	//~ Begin UInterchangeTranslatorBase
	virtual EInterchangeTranslatorType GetTranslatorType() const override;
	virtual EInterchangeTranslatorAssetType GetSupportedAssetTypes() const override;
	virtual TArray<FString> GetSupportedFormats() const override;
	virtual bool Translate(UInterchangeBaseNodeContainer& BaseNodeContainer) const override;
	virtual void ReleaseSource() override;
	//~ End UInterchangeTranslatorBase

	//~ Begin IInterchangeMeshPayloadInterface
	UE_DEPRECATED(5.6, "Deprecated. Use GetMeshPayloadData(const FInterchangeMeshPayLoadKey&, const FAttributeStorage&) instead.")
	virtual TOptional<UE::Interchange::FMeshPayloadData> GetMeshPayloadData(
		const FInterchangeMeshPayLoadKey& PayLoadKey,
		const FTransform& MeshGlobalTransform) const override
	{
		UE::Interchange::FAttributeStorage Attributes;
		Attributes.RegisterAttribute(
			UE::Interchange::FAttributeKey{ UE::Interchange::MeshPayload::Attributes::MeshGlobalTransform },
			MeshGlobalTransform);
		return GetMeshPayloadData(PayLoadKey, Attributes);
	}

	virtual TOptional<UE::Interchange::FMeshPayloadData> GetMeshPayloadData(
		const FInterchangeMeshPayLoadKey& PayLoadKey,
		const UE::Interchange::FAttributeStorage& PayloadAttributes) const override;
	//~ End IInterchangeMeshPayloadInterface

	//~ Begin IInterchangeTexturePayloadInterface
	virtual TOptional<UE::Interchange::FImportImage> GetTexturePayloadData(
		const FString& PayloadKey,
		TOptional<FString>& AlternateTexturePath) const override;
	//~ End IInterchangeTexturePayloadInterface

	/**
	 * Prefix marking a texture payload key as referring to a texture embedded in the source file
	 * rather than to a path on disk. Followed by the embedded texture's index.
	 *
	 * External textures use their resolved absolute path as the key, which lets the request be
	 * forwarded to whichever engine image translator handles that format. Embedded data has no path,
	 * so it needs a key the translator can recognise as its own and decode itself.
	 */
	static const TCHAR* GetEmbeddedTextureKeyPrefix();

private:
	/** Parses the source file if it has not been parsed already. Returns null on failure. */
	TSharedPtr<FAssimpScene> EnsureSceneLoaded() const;

	/**
	 * Parsed scene, cached between Translate() and the payload requests that follow it.
	 *
	 * Mutable because the whole translator interface is const, yet Interchange's design expects the
	 * translator to hold the parsed file across the translate-then-fetch-payloads sequence. Parsing
	 * once and reusing it is the entire point: re-reading the file per payload would make importing
	 * a many-mesh scene quadratic.
	 */
	mutable TSharedPtr<FAssimpScene> CachedScene;

	/** Guards CachedScene: Interchange may request payloads from several threads at once. */
	mutable FCriticalSection SceneLock;
};
