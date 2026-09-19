// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Templates/PimplPtr.h"

#include "AssimpImportSettings.h"
#include "AssimpSceneTypes.h"

struct FMeshDescription;

/** Severity of a diagnostic raised while parsing or converting a scene. */
enum class EAssimpDiagnosticSeverity : uint8
{
	Info,
	Warning,
	Error
};

/** A single diagnostic message, either from Assimp's own logger or from our conversion. */
struct ASSIMPCORE_API FAssimpDiagnostic
{
	EAssimpDiagnosticSeverity Severity = EAssimpDiagnosticSeverity::Info;
	FString Message;
};

/**
 * Outcome of a load attempt.
 *
 * Diagnostics are collected even on success: many formats parse with warnings that the user should
 * see (missing textures, unsupported features silently dropped by an importer). Surfacing them is
 * how an import that "worked" but produced something odd becomes explicable.
 */
struct ASSIMPCORE_API FAssimpLoadResult
{
	/** True when a scene was produced. Diagnostics may still contain warnings. */
	bool bSucceeded = false;

	/** True when the load stopped because the progress delegate requested cancellation. */
	bool bCancelled = false;

	/**
	 * True when the file parsed but holds no triangles -- a point cloud, or line-only geometry.
	 *
	 * Distinguished from a parse failure because it is a different problem with a different answer:
	 * the file is fine, it simply has nothing a mesh importer can use. Assimp reports it as the
	 * opaque "No meshes remaining", which sends people looking for a corrupt file.
	 */
	bool bContainsNoTriangleGeometry = false;

	/** Assimp's error string, or our own, when bSucceeded is false. */
	FString ErrorMessage;

	/** Everything logged during the load, in order. */
	TArray<FAssimpDiagnostic> Diagnostics;

	/** Wall-clock seconds spent parsing and post-processing. */
	double ElapsedSeconds = 0.0;
};

/**
 * An embedded texture lifted out of a source file.
 *
 * Assimp presents embedded textures in one of two shapes, and callers must handle both: either an
 * intact compressed file (PNG, JPEG, ...) which should be handed to an image decoder, or an already
 * decoded pixel array.
 */
struct ASSIMPCORE_API FAssimpEmbeddedTexture
{
	/** Name Assimp assigned, usually derived from the material reference. */
	FString Name;

	/** True when Data holds a compressed image file rather than raw pixels. */
	bool bIsCompressed = false;

	/** Lower-case extension hint for compressed data, e.g. "png". Empty when unknown. */
	FString FormatHint;

	/** Compressed file bytes. Only populated when bIsCompressed. */
	TArray<uint8> CompressedData;

	/** Decoded pixels, row-major from the top-left. Only populated when !bIsCompressed. */
	TArray<FColor> RawPixels;

	/** Dimensions of RawPixels. Zero when bIsCompressed. */
	int32 Width = 0;
	int32 Height = 0;
};

/**
 * Reports progress and allows cancellation.
 *
 * @param Fraction  Completion in the range [0, 1].
 * @return          false to abort the import.
 */
DECLARE_DELEGATE_RetVal_OneParam(bool, FAssimpProgressDelegate, float /*Fraction*/);

/**
 * Owns a parsed Assimp scene and converts it to Unreal data on demand.
 *
 * Lifetime
 * --------
 * Strict RAII. The instance owns the underlying Assimp::Importer, and the parsed scene is destroyed
 * with it. There is deliberately no way to obtain the raw aiScene pointer: handing that out is what
 * makes lifetime bugs possible, since the pointer is owned by the importer and dies with it. This is
 * also why FAssimpScene is not a UObject -- a UObject's destruction is scheduled by the garbage
 * collector, which is the wrong lifetime model for a large non-UObject allocation held by a
 * third-party library. Callers hold a TSharedPtr; UObject-facing wrappers hold that shared pointer
 * rather than the scene itself.
 *
 * Threading
 * ---------
 * Loading is safe to perform on any thread. A given instance is safe to read concurrently once
 * loaded, because every accessor is const and the underlying scene is immutable after parsing.
 */
class ASSIMPCORE_API FAssimpScene
{
public:
	/**
	 * Parses a file from disk.
	 *
	 * Reading goes through Unreal's IFileManager rather than the C runtime, so paths Unreal
	 * understands but the CRT does not (mounted pak content, virtual file systems) resolve
	 * correctly.
	 *
	 * @param FilePath   File to read.
	 * @param Settings   Import settings; sanitised internally, so callers need not pre-validate.
	 * @param OutResult  Receives success, diagnostics, and timing.
	 * @param Progress   Optional progress and cancellation callback.
	 * @return           The scene, or null on failure. Inspect OutResult for the reason.
	 */
	static TSharedPtr<FAssimpScene> LoadFromFile(
		const FString& FilePath,
		const FAssimpImportSettings& Settings,
		FAssimpLoadResult& OutResult,
		const FAssimpProgressDelegate& Progress = FAssimpProgressDelegate());

	/**
	 * Parses a scene already held in memory.
	 *
	 * @param Buffer         Raw file bytes.
	 * @param FormatHint     Extension without the dot, e.g. "ply". Assimp cannot always detect the
	 *                       format from content alone, so supply this whenever it is known.
	 * @param DebugName      Name used in diagnostics; typically the original filename.
	 */
	static TSharedPtr<FAssimpScene> LoadFromMemory(
		TArrayView<const uint8> Buffer,
		const FString& FormatHint,
		const FString& DebugName,
		const FAssimpImportSettings& Settings,
		FAssimpLoadResult& OutResult,
		const FAssimpProgressDelegate& Progress = FAssimpProgressDelegate());

	/**
	 * Extensions the loaded Assimp library can read, lower-case and without a leading dot.
	 *
	 * Queried from Assimp itself rather than hard-coded, so it reflects the actual build -- which
	 * matters because format support depends on the CMake options Assimp was compiled with, not just
	 * its version.
	 */
	static TArray<FString> GetSupportedExtensions();

	/** True if the loaded Assimp library claims support for the given extension, with or without a dot. */
	static bool IsExtensionSupported(const FString& Extension);

	~FAssimpScene();

	// Non-copyable and non-movable: instances are always held behind a TSharedPtr.
	FAssimpScene(const FAssimpScene&) = delete;
	FAssimpScene& operator=(const FAssimpScene&) = delete;
	FAssimpScene(FAssimpScene&&) = delete;
	FAssimpScene& operator=(FAssimpScene&&) = delete;

	/** Flattened Unreal-side description of the whole scene. */
	const FAssimpSceneInfo& GetSceneInfo() const;

	/** The settings actually used, after sanitisation. */
	const FAssimpImportSettings& GetEffectiveSettings() const;

	/**
	 * Converts one mesh to an FMeshDescription.
	 *
	 * This is the single conversion path in the plugin: the Interchange translator returns the
	 * result as mesh payload data, and the runtime path converts it onward to an FDynamicMesh3.
	 *
	 * @param MeshIndex        Index into GetSceneInfo().Meshes.
	 * @param OutMeshDescription  Populated on success; left untouched on failure.
	 * @return                 True on success.
	 */
	bool GetMeshDescription(int32 MeshIndex, FMeshDescription& OutMeshDescription) const;

	/**
	 * Converts several meshes into one FMeshDescription, preserving a separate polygon group and
	 * material slot per source mesh.
	 *
	 * Needed because Assimp splits a model by material while Unreal represents that as a single mesh
	 * with multiple sections.
	 *
	 * @param MeshIndices         Meshes to merge, in the order their material slots should appear.
	 * @param OutMeshDescription  Populated on success.
	 */
	bool GetMergedMeshDescription(
		TArrayView<const int32> MeshIndices,
		FMeshDescription& OutMeshDescription) const;

	/**
	 * As above, additionally reporting the skeleton the skin weights refer to.
	 *
	 * @param OutJointNames  Bone names in the index order the written skin weights use. Empty when
	 *                       the meshes are not skinned or skeletal import is disabled. Interchange
	 *                       needs exactly this to remap weights when it merges meshes.
	 */
	bool GetMergedMeshDescription(
		TArrayView<const int32> MeshIndices,
		FMeshDescription& OutMeshDescription,
		TArray<FString>& OutJointNames) const;

	/**
	 * Converts one morph target to an FMeshDescription holding the deformed shape.
	 *
	 * Not a delta: Assimp stores morph targets as whole replacement arrays, and Unreal's factory
	 * works out the difference itself by comparing this against the base mesh. That comparison is
	 * per vertex index, so this deliberately runs the same conversion as GetMeshDescription with
	 * only the positions and normals substituted -- anything else risks a different vertex order and
	 * a morph target that tears the mesh apart instead of deforming it.
	 *
	 * @param MorphTargetIndex    Index into GetSceneInfo().MorphTargets.
	 * @param OutMeshDescription  Populated on success.
	 */
	bool GetMorphTargetMeshDescription(
		int32 MorphTargetIndex,
		FMeshDescription& OutMeshDescription) const;

	/**
	 * Samples a morph target's animated weight across a time range.
	 *
	 * @param AnimationIndex    Index into GetSceneInfo().Animations.
	 * @param MorphTargetIndex  Index into GetSceneInfo().MorphTargets.
	 * @param OutTimes          Key times in seconds.
	 * @param OutWeights        Weight at each of those times, in the range [0, 1].
	 * @return                  False when the clip does not animate that target's weight.
	 */
	bool GetMorphTargetWeightCurve(
		int32 AnimationIndex,
		int32 MorphTargetIndex,
		TArray<float>& OutTimes,
		TArray<float>& OutWeights) const;

	/**
	 * Frame rate one animation clip should be baked at.
	 *
	 * Not simply the file's ticks-per-second: that is a timebase (glTF counts in milliseconds,
	 * Collada in seconds) and only some formats state something that is genuinely a frame rate.
	 * Reading the timebase as a frame rate would bake a glTF clip at 1000 fps and a Collada clip at
	 * 1 fps, so it is used only when it is plausibly one, and 30 is used otherwise.
	 *
	 * @param AnimationIndex  Index into GetSceneInfo().Animations.
	 * @return                Frames per second, or 0 for an invalid index.
	 */
	double GetAnimationSampleRate(int32 AnimationIndex) const;

	/**
	 * Samples one node's animated local transform at a fixed rate.
	 *
	 * Baked rather than curve-shaped on purpose: Assimp's channels are three independent key arrays
	 * with no interpolation mode, whereas Unreal stores one transform per bone per frame. Evaluating
	 * all three at common frame times is the honest translation, and it is what Interchange requests
	 * of a translator.
	 *
	 * The transforms are node-local and in Unreal space, converted by the same path as the reference
	 * pose, so a pose and the animation driving it cannot end up in different spaces.
	 *
	 * @param AnimationIndex     Index into GetSceneInfo().Animations.
	 * @param NodeName           Node to sample; typically a bone name from FAssimpSkinnedMeshGroup.
	 * @param SampleRateHz       Frames per second to bake at. See GetAnimationSampleRate.
	 * @param RangeStartSeconds  Start of the sampled range.
	 * @param RangeEndSeconds    End of the sampled range, inclusive.
	 * @param OutKeys            Receives RoundToInt(Range * Rate) + 1 transforms.
	 * @return                   False when the clip does not animate that node.
	 */
	bool GetBakedAnimationTrack(
		int32 AnimationIndex,
		const FString& NodeName,
		double SampleRateHz,
		double RangeStartSeconds,
		double RangeEndSeconds,
		TArray<FTransform>& OutKeys) const;

	/** Fetches an embedded texture by index into [0, FAssimpSceneInfo::NumEmbeddedTextures). */
	bool GetEmbeddedTexture(int32 TextureIndex, FAssimpEmbeddedTexture& OutTexture) const;

	/**
	 * Resolves a material's texture reference to an absolute path on disk.
	 *
	 * Source files routinely record absolute paths from the authoring machine, or paths using the
	 * other platform's separators. This searches the source file's directory and its common sibling
	 * directories before giving up.
	 *
	 * @return True when an existing file was found, with OutAbsolutePath set.
	 */
	bool ResolveExternalTexturePath(const FString& RecordedPath, FString& OutAbsolutePath) const;

private:
	FAssimpScene();

	/**
	 * Describes one load, without naming an Assimp type.
	 *
	 * Both entry points funnel into a single implementation, and this is what lets them do so while
	 * keeping this header free of Assimp declarations: the file and memory cases differ only in
	 * which of FilePath and Buffer is populated.
	 */
	struct FLoadRequest
	{
		/** Name used in log messages and diagnostics. */
		FString DebugName;

		/** Directory that relative references (sibling .mtl files, textures) resolve against. */
		FString BaseDirectory;

		/** Extension without the dot, used for format detection and unit-scale defaults. */
		FString Extension;

		/** File to read. Empty for a memory load. */
		FString FilePath;

		/** Bytes to read. Only inspected when FilePath is empty. */
		TArrayView<const uint8> Buffer;
	};

	/** Shared implementation of LoadFromFile and LoadFromMemory. */
	static TSharedPtr<FAssimpScene> LoadInternal(
		const FLoadRequest& Request,
		const FAssimpImportSettings& Settings,
		FAssimpLoadResult& OutResult,
		const FAssimpProgressDelegate& Progress,
		bool bRelaxed = false);

	/**
	 * Re-reads a file that already failed, with validation and point/line removal off, purely to
	 * work out WHY it failed.
	 *
	 * The strict configuration collapses two very different situations into one message: a genuinely
	 * corrupt file, and a perfectly valid point cloud or line drawing that a mesh importer has no use
	 * for. Assimp reports both as "No meshes remaining", which sends people hunting for corruption
	 * that is not there. Reading again without the strictness tells them apart, and refines
	 * OutResult's message accordingly.
	 *
	 * Only ever runs on the failure path, so a successful import pays nothing for it.
	 */
	static void ClassifyFailure(
		const FLoadRequest& Request,
		const FAssimpImportSettings& Settings,
		FAssimpLoadResult& OutResult);

	struct FImpl;
	TPimplPtr<FImpl> Impl;
};
