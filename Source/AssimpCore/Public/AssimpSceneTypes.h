// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

#include "AssimpSceneTypes.generated.h"

/**
 * Unreal-side description of a parsed Assimp scene.
 *
 * These types exist so that the plugin's public surface can describe an imported file completely
 * without exposing a single Assimp type. AssimpCore reads the aiScene once and flattens it into
 * these structs; consumers then work only against them. Two consequences that matter:
 *
 *  - Nothing outside AssimpCore/Private needs Assimp's include paths, exception settings, or its
 *    <windows.h> baggage.
 *  - The structs are Blueprint-visible, so the runtime API can hand a whole scene description to
 *    Blueprint without a bespoke wrapper UObject per Assimp concept.
 *
 * Heavy per-vertex data is deliberately absent: geometry is fetched on demand, one mesh at a time,
 * as an FMeshDescription. Interchange in particular wants exactly this shape -- a cheap up-front
 * description to build its node graph from, then lazy payload fetches.
 */

/** Texture slots recognised when translating an Assimp material. */
UENUM(BlueprintType)
enum class EAssimpTextureSlot : uint8
{
	BaseColor,
	Normal,
	Metallic,
	Roughness,
	Specular,
	AmbientOcclusion,
	Emissive,
	Opacity,
	Displacement,
	Height,
	Shininess,
	Lightmap,
	Reflection,
	Unknown
};

/** How a texture referenced by a material is stored. */
UENUM(BlueprintType)
enum class EAssimpTextureSource : uint8
{
	/** Stored in a separate file; Path is a filesystem path, possibly relative to the source file. */
	External,

	/** Embedded in the source file; EmbeddedIndex identifies it within the scene. */
	Embedded
};

/** A texture referenced by a material. */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpTextureReference
{
	GENERATED_BODY()

	/** Whether the texture lives in a separate file or inside the source file. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Texture")
	EAssimpTextureSource Source = EAssimpTextureSource::External;

	/**
	 * Path exactly as recorded in the source file, for external textures. Frequently relative, and
	 * frequently using the authoring machine's separators, so it must be resolved rather than used
	 * verbatim.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Texture")
	FString Path;

	/** Index into the scene's embedded texture array. INDEX_NONE for external textures. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Texture")
	int32 EmbeddedIndex = INDEX_NONE;

	/** UV channel this texture samples. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Texture")
	int32 UVChannel = 0;
};

/** Description of one Assimp material. */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpMaterialInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	FLinearColor BaseColor = FLinearColor::White;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	FLinearColor EmissiveColor = FLinearColor::Black;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	float Metallic = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	float Roughness = 0.5f;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	float Opacity = 1.0f;

	/** True when the material declares any form of transparency. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	bool bIsTranslucent = false;

	/** True when the material asks not to be backface-culled. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	bool bTwoSided = false;

	/** Textures declared by this material, keyed by the slot they were found in. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	TMap<EAssimpTextureSlot, FAssimpTextureReference> Textures;
};

/** Description of one Assimp mesh. One mesh maps to one material, so a multi-material model yields several. */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpMeshInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	FString Name;

	/** Index into FAssimpSceneInfo::Materials, or INDEX_NONE. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	int32 MaterialIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	int32 NumVertices = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	int32 NumTriangles = 0;

	/** Number of populated UV channels, 0-8. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	int32 NumUVChannels = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	bool bHasNormals = false;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	bool bHasTangents = false;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	bool bHasVertexColors = false;

	/** True when this mesh carries skin weights and should become part of a skeletal mesh. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	bool bHasBones = false;

	/** Names of the bones influencing this mesh, in Assimp's order. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	TArray<FString> BoneNames;

	/** Axis-aligned bounds in Unreal space, after coordinate and scale conversion. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	FBox BoundingBox = FBox(ForceInit);
};

/** A node in the scene hierarchy. Nodes are stored flattened, parents always before children. */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpNodeInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Node")
	FString Name;

	/** Index of the parent node, or INDEX_NONE for the root. Always less than this node's index. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Node")
	int32 ParentIndex = INDEX_NONE;

	/** Transform relative to the parent node, converted to Unreal space. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Node")
	FTransform LocalTransform = FTransform::Identity;

	/** Indices into FAssimpSceneInfo::Meshes drawn by this node. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Node")
	TArray<int32> MeshIndices;

	/** Indices of this node's children. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Node")
	TArray<int32> ChildIndices;

	/** Format-specific metadata attached to this node, flattened to strings. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Node")
	TMap<FString, FString> Metadata;
};

/** A camera defined in the source file. */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpCameraInfo
{
	GENERATED_BODY()

	/** Name of the node this camera is attached to. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Camera")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Camera")
	float HorizontalFieldOfViewDegrees = 90.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Camera")
	float NearClipPlane = 10.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Camera")
	float FarClipPlane = 100000.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Camera")
	float AspectRatio = 0.0f;
};

/** Light types Assimp can report. */
UENUM(BlueprintType)
enum class EAssimpLightType : uint8
{
	Directional,
	Point,
	Spot,
	Ambient,
	Area,
	Unknown
};

/** A light defined in the source file. */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpLightInfo
{
	GENERATED_BODY()

	/** Name of the node this light is attached to. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	EAssimpLightType Type = EAssimpLightType::Point;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	FLinearColor DiffuseColor = FLinearColor::White;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	float InnerConeAngleDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	float OuterConeAngleDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	float AttenuationConstant = 1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	float AttenuationLinear = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	float AttenuationQuadratic = 0.0f;
};

/**
 * Complete Unreal-side description of a parsed scene.
 *
 * Cheap to copy relative to the geometry it describes: no vertex data is held here. Use
 * FAssimpScene::GetMeshDescription to fetch geometry for a given mesh index.
 */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpSceneInfo
{
	GENERATED_BODY()

	/** Absolute path the scene was loaded from, or empty when loaded from memory. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	FString SourceFilePath;

	/** Nodes, flattened depth-first. Index 0 is the root; a parent always precedes its children. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	TArray<FAssimpNodeInfo> Nodes;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	TArray<FAssimpMeshInfo> Meshes;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	TArray<FAssimpMaterialInfo> Materials;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	TArray<FAssimpCameraInfo> Cameras;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	TArray<FAssimpLightInfo> Lights;

	/** Number of embedded textures available via FAssimpScene::GetEmbeddedTexture. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	int32 NumEmbeddedTextures = 0;

	/** Names of every animation clip in the file. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	TArray<FString> AnimationNames;

	/** Unit scale the file declared, before conversion to centimetres. 1.0 when unspecified. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	float FileUnitScale = 1.0f;

	/** Scale actually applied during conversion, combining file unit scale and the requested uniform scale. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	float AppliedScale = 1.0f;

	/** Scene-level metadata, flattened to strings. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	TMap<FString, FString> Metadata;

	/** True when any mesh in the scene carries skin weights. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	bool bHasSkinnedMeshes = false;
};
