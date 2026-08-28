// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

#include "AssimpImportSettings.generated.h"

/**
 * How to reconcile the source file's coordinate system with Unreal's.
 *
 * Unreal is left-handed, Z-up, and measured in centimetres. Most formats Assimp reads are
 * right-handed and Y-up, so some conversion is nearly always required. Getting this wrong is the
 * single most common cause of models arriving mirrored or lying on their side.
 */
UENUM(BlueprintType)
enum class EAssimpCoordinateSystemMode : uint8
{
	/**
	 * Convert to Unreal's coordinate system, honouring the source file's declared axes where the
	 * format provides them. This is the correct choice for almost every file.
	 */
	Automatic,

	/** Convert using a fixed right-handed Y-up assumption, ignoring any axis metadata in the file. */
	ForceYUpRightHanded,

	/**
	 * Import coordinates verbatim. Use only when the source file was authored specifically for
	 * Unreal, or when a downstream tool applies its own conversion.
	 */
	Raw
};

/**
 * Everything that governs how a file is read and converted.
 *
 * Deliberately a single struct shared by both import paths: the Interchange pipeline surfaces it in
 * the editor import dialog, and the runtime Blueprint API takes it as a parameter. One definition
 * means the two paths cannot drift apart in behaviour or defaults.
 */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpImportSettings
{
	GENERATED_BODY()

	// ---------------------------------------------------------------------------------------------
	// What to import
	// ---------------------------------------------------------------------------------------------

	/** Import materials and create corresponding Unreal material assets or instances. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Content")
	bool bImportMaterials = true;

	/** Import textures, including textures embedded inside the source file. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Content")
	bool bImportTextures = true;

	/** Import bones and skin weights, producing a skeletal mesh rather than a static mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Content")
	bool bImportSkeletalMesh = true;

	/** Import animation clips. Requires bImportSkeletalMesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Content", meta = (EditCondition = "bImportSkeletalMesh"))
	bool bImportAnimations = true;

	/** Import morph targets (Assimp anim-meshes). Requires bImportSkeletalMesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Content", meta = (EditCondition = "bImportSkeletalMesh"))
	bool bImportMorphTargets = true;

	/** Import cameras defined in the source file. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Content")
	bool bImportCameras = false;

	/** Import lights defined in the source file. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Content")
	bool bImportLights = false;

	/** Preserve format-specific metadata on the imported assets for later inspection. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Content")
	bool bImportMetadata = true;

	// ---------------------------------------------------------------------------------------------
	// Transform
	// ---------------------------------------------------------------------------------------------

	/** How to reconcile the file's coordinate system with Unreal's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	EAssimpCoordinateSystemMode CoordinateSystemMode = EAssimpCoordinateSystemMode::Automatic;

	/**
	 * Scale the imported geometry by the unit scale the source file declares, converting to Unreal
	 * centimetres. Files that declare metres arrive 100x too small without this.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	bool bApplyFileUnitScale = true;

	/** Extra uniform scale applied after unit conversion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform", meta = (ClampMin = "0.000001", UIMin = "0.001", UIMax = "1000.0"))
	float UniformScale = 1.0f;

	/**
	 * Collapse the node hierarchy and bake every node transform into the vertices, yielding a single
	 * static shape.
	 *
	 * Destroys the scene graph, so it is incompatible with skeletal meshes and animation. Useful for
	 * CAD and scan data where the hierarchy carries no meaning.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	bool bPreTransformVertices = false;

	// ---------------------------------------------------------------------------------------------
	// Geometry processing
	// ---------------------------------------------------------------------------------------------

	/** Merge vertices that share all attributes. Almost always desirable; reduces vertex count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bJoinIdenticalVertices = true;

	/** Generate vertex normals when the file does not supply them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bGenerateMissingNormals = true;

	/**
	 * Angle in degrees above which an edge is treated as hard when generating normals.
	 * Only meaningful when bGenerateMissingNormals is set.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry", meta = (EditCondition = "bGenerateMissingNormals", ClampMin = "0.0", ClampMax = "175.0"))
	float NormalSmoothingAngle = 66.0f;

	/** Generate tangents and binormals, required for normal mapping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bGenerateTangents = true;

	/** Drop degenerate points and lines, and remove faces with zero area. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bRemoveDegenerates = true;

	/** Detect and repair invalid data such as NaN normals or zero-length tangents. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bFixInvalidData = true;

	/** Merge meshes where possible to reduce draw calls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bOptimizeMeshes = true;

	/**
	 * Collapse redundant nodes in the scene graph.
	 *
	 * Forced off whenever skeletal meshes or animations are being imported: animation channels and
	 * bone bindings are addressed by node name, and this optimisation removes the very nodes they
	 * refer to. AssimpForUnreal enforces that rather than trusting the caller.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bOptimizeSceneGraph = false;

	/** Discard materials that are byte-for-byte duplicates of one another. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bRemoveRedundantMaterials = true;

	/** Reorder indices for better vertex-cache utilisation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bImproveCacheLocality = true;

	/**
	 * Run Assimp's structural validation over the parsed scene before conversion.
	 *
	 * Worth keeping on: several of Assimp's less-travelled importers can emit internally
	 * inconsistent scenes, and catching that here produces a clear diagnostic instead of a crash
	 * further down the conversion.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry", AdvancedDisplay)
	bool bValidateSceneStructure = true;

	/**
	 * Maximum number of bone influences per vertex. Extra influences are dropped, smallest first,
	 * and the remainder renormalised. Unreal supports up to 12; Assimp's own default cap is 4.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry", AdvancedDisplay, meta = (EditCondition = "bImportSkeletalMesh", ClampMin = "1", ClampMax = "12"))
	int32 MaxBoneInfluencesPerVertex = 12;

	/**
	 * Returns a copy with mutually exclusive options reconciled.
	 *
	 * Applies the invariants that must hold regardless of what the caller asked for, so that both
	 * import paths and any future caller get the same corrections:
	 *   - scene-graph optimisation is disabled when skeletons or animations are imported
	 *   - vertex pre-transformation is disabled when skeletons or animations are imported
	 *   - animation and morph target import require skeletal mesh import
	 *
	 * @param OutAdjustments  Optional; receives a human-readable line per correction applied, so
	 *                        callers can surface why a requested option was overridden.
	 */
	FAssimpImportSettings GetSanitized(TArray<FString>* OutAdjustments = nullptr) const;
};
