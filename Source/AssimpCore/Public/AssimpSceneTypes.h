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

	/**
	 * Unreal's specular input: how reflective the surface is where it is not metallic.
	 *
	 * 0.5 is Unreal's neutral value and corresponds to the 4% normal-incidence reflectance every
	 * dielectric has, so a material that says nothing about specularity must arrive at exactly 0.5.
	 * Anything else silently changes the look of every model that does not use the channel.
	 *
	 * See FAssimpScene's material conversion for how the source file's specular colour, specular
	 * factor and shininess strength are folded into this one scalar.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	float Specular = 0.5f;

	/**
	 * Specular tint exactly as the file declared it, before it was reduced to the Specular scalar.
	 *
	 * Unreal's default shading model has no coloured-specular input for dielectrics -- metals take
	 * their tint from base colour instead -- so this is carried for inspection and for callers
	 * driving their own material, not because the plugin's own materials consume it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Material")
	FLinearColor SpecularColor = FLinearColor::White;

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

	/** Indices into FAssimpSceneInfo::MorphTargets of the morph targets deforming this mesh. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	TArray<int32> MorphTargetIndices;

	/** Axis-aligned bounds in Unreal space, after coordinate and scale conversion. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Mesh")
	FBox BoundingBox = FBox(ForceInit);
};

/**
 * One morph target (blend shape) belonging to a mesh.
 *
 * Assimp calls these anim-meshes, and stores them as whole replacement attribute arrays rather than
 * as deltas: a morph target is the mesh in its deformed shape, not the difference. Unreal wants the
 * difference, and works it out by comparing the two -- which only holds together if both come out of
 * the same conversion with the same vertex order.
 */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpMorphTargetInfo
{
	GENERATED_BODY()

	/** Name from the file, or a synthesised one when it declared none. Becomes the UMorphTarget's name. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Morph")
	FString Name;

	/** Index into FAssimpSceneInfo::Meshes of the mesh this deforms. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Morph")
	int32 MeshIndex = INDEX_NONE;

	/** Index of this morph target within that mesh, which is how a weight channel addresses it. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Morph")
	int32 MorphIndex = INDEX_NONE;

	/** Default influence the file gives it, in the range [0, 1]. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Morph")
	float Weight = 0.0f;

	/** True when the target supplies its own normals rather than leaving the base mesh's in place. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Morph")
	bool bHasNormals = false;
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

/**
 * One bone of a skeleton reconstructed from the loose bone references in a file.
 *
 * Assimp hands back no skeleton: each mesh carries a flat list of named bones, and the hierarchy
 * that relates them is mixed into the node tree with everything else. These are the result of
 * putting that back together -- ordered the way Unreal requires, parents strictly before children.
 */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpSkeletonBone
{
	GENERATED_BODY()

	/** Bone name, matching both the node and the bone reference it was reconstructed from. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Skeleton")
	FString Name;

	/**
	 * Index of this bone's node in FAssimpSceneInfo::Nodes, or INDEX_NONE.
	 *
	 * Carried so consumers never have to find the node by name. Names are not unique, and Unreal's
	 * FString comparison is case-insensitive besides -- which is a real trap here, because Assimp
	 * inserts a synthetic root node called "ROOT" above a glTF scene whose own root bone is commonly
	 * called "Root". Matching on the name marks that synthetic node as a bone, and the skeleton then
	 * appears to be rooted one level above where it is.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Skeleton")
	int32 NodeIndex = INDEX_NONE;

	/** Index of the parent in the containing array, or INDEX_NONE for the root. Always smaller than this bone's index. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Skeleton")
	int32 ParentIndex = INDEX_NONE;

	/** Transform relative to the parent bone, in Unreal space. Together these form the reference pose. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Skeleton")
	FTransform LocalTransform = FTransform::Identity;

	/**
	 * True when a mesh actually skins to this bone.
	 *
	 * False for a bone included only because it lies between the root and a bone that is skinned to.
	 * Those must exist to carry their transforms, but nothing is weighted to them.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Skeleton")
	bool bIsSkinningBone = false;
};

/**
 * A skeleton together with every skinned mesh bound to it.
 *
 * Assimp describes skinning per mesh, so several meshes of one character are several independent
 * bone lists that happen to name the same nodes. Unreal needs the opposite grouping: one skeleton,
 * and the meshes that share it. Reconciling the two is what this struct records -- and it is also
 * what animation needs, since an animation clip targets a skeleton rather than a mesh.
 */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpSkinnedMeshGroup
{
	GENERATED_BODY()

	/** Name of the node the skeleton is rooted at. Also Bones[0].Name. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Skeleton")
	FString RootBoneName;

	/** The skeleton, parents before children. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Skeleton")
	TArray<FAssimpSkeletonBone> Bones;

	/** Indices into FAssimpSceneInfo::Meshes of every mesh skinned to this skeleton. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Skeleton")
	TArray<int32> MeshIndices;
};

/** An animation clip defined in the source file. */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpAnimationInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Animation")
	FString Name;

	/** Length in seconds, after converting the file's ticks. Never negative. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Animation")
	float DurationSeconds = 0.0f;

	/**
	 * Ticks per second the file declared, or 0 when it declared none.
	 *
	 * Reported unconverted because it is diagnostic rather than directive: it is a timebase, not a
	 * frame rate, and treating it as one is wrong for most formats. glTF stores milliseconds
	 * (1000), Collada stores seconds (1). See FAssimpScene::GetAnimationSampleRate.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Animation")
	float TicksPerSecond = 0.0f;

	/** Names of the nodes this clip animates, in the file's order. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Animation")
	TArray<FString> AnimatedNodeNames;

	/**
	 * True when the clip also carries mesh or morph-mesh channels.
	 *
	 * Those are not imported. Surfacing the fact is the point: a clip that only deforms morph
	 * targets otherwise imports as an empty animation with no explanation.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Animation")
	bool bHasMeshOrMorphChannels = false;
};

/** A camera defined in the source file. */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpCameraInfo
{
	GENERATED_BODY()

	/** Name of the node this camera is attached to. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Camera")
	FString Name;

	/** Index of that node in FAssimpSceneInfo::Nodes, or INDEX_NONE when no node carries the name. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Camera")
	int32 NodeIndex = INDEX_NONE;

	/**
	 * Placement within its node, in Unreal space, oriented so the camera looks down +X.
	 *
	 * Assimp keeps a camera's position and orientation separate from the node transform: the node
	 * says where the rig is, this says where the lens sits on it. Composing the two is the caller's
	 * job, because only the caller knows the node's world transform.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Camera")
	FTransform LocalTransform = FTransform::Identity;

	/**
	 * Full horizontal field of view, in degrees -- the same quantity and units as a
	 * UCameraComponent's FieldOfView.
	 *
	 * Normalised here because Assimp is not consistent about it: its own header documents a
	 * half-angle and FBX stores one, while Collada and glTF store the full angle. The source format
	 * is what settles it, and doing that once here keeps every consumer from having to know.
	 */
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

	/** Index of that node in FAssimpSceneInfo::Nodes, or INDEX_NONE when no node carries the name. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	int32 NodeIndex = INDEX_NONE;

	/**
	 * Placement within its node, in Unreal space, oriented so the light shines down +X.
	 *
	 * +X because that is the axis Unreal's directional and spot lights emit along. A point light
	 * ignores the rotation entirely, which is why one transform serves all three types.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	FTransform LocalTransform = FTransform::Identity;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	EAssimpLightType Type = EAssimpLightType::Point;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	FLinearColor DiffuseColor = FLinearColor::White;

	/**
	 * Full cone angle, not the half-angle Unreal's spot light takes.
	 *
	 * Reported as the file states it -- Assimp's own header notes this is 2*PI for a point light,
	 * which only makes sense as a full angle. Consumers driving a USpotLightComponent must halve it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Light")
	float InnerConeAngleDegrees = 0.0f;

	/** Full cone angle, as above. */
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

	/**
	 * Every morph target in the file, flattened across meshes. Empty when morph import is disabled.
	 *
	 * Flat rather than nested under each mesh because a weight animation channel names a mesh and a
	 * morph index, and both consumers want to look one up without walking the mesh list.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	TArray<FAssimpMorphTargetInfo> MorphTargets;

	/** Every animation clip in the file. Empty when animation import is disabled. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	TArray<FAssimpAnimationInfo> Animations;

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

	/**
	 * Skeletons reconstructed from the scene, each with the meshes skinned to it.
	 *
	 * Empty when skeletal import is disabled or nothing in the file is skinned. Built during the
	 * initial description pass rather than on demand because both consumers need it before they
	 * request any geometry: the editor import has to emit joint nodes while building its node
	 * graph, and animation is addressed per skeleton.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Scene")
	TArray<FAssimpSkinnedMeshGroup> SkinnedMeshGroups;
};
