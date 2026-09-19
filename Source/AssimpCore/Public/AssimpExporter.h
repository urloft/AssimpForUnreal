// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

#include "AssimpExporter.generated.h"

struct FMeshDescription;

/** One format Assimp can write, as it describes itself. */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpExportFormat
{
	GENERATED_BODY()

	/**
	 * Identifier the exporter is selected by, such as "objnomtl" or "gltf2".
	 *
	 * Assimp's own string, not a friendly name and not the extension: several formats share an
	 * extension (glTF 1 and 2 both write .gltf) and the id is the only thing that picks between them.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Export")
	FString FormatId;

	/** Human-readable description, for putting in front of a user. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Export")
	FString Description;

	/** Conventional file extension, without the dot. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Export")
	FString FileExtension;
};

/** Outcome of an export attempt. */
USTRUCT(BlueprintType)
struct ASSIMPCORE_API FAssimpExportResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Export")
	bool bSucceeded = false;

	/** Assimp's error string, or our own, when bSucceeded is false. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Export")
	FString ErrorMessage;

	/** Meshes actually written. Lower than the number offered when some held no triangles. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Export")
	int32 MeshesWritten = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Assimp|Export")
	float ElapsedSeconds = 0.0f;
};

/**
 * Writes Unreal geometry out through Assimp.
 *
 * This is the import path run backwards, and that framing is the whole design. The coordinate change
 * of basis, the winding reversal and the V flip are each inverted by the function directly beside
 * the one that applied them, in FAssimpAxisConverter -- not reimplemented here. A separate
 * implementation is how an exporter ends up subtly disagreeing with its importer, which shows up as
 * a model that survives one round trip looking mirrored and two round trips looking correct.
 *
 * Scope: geometry, in a single flat node. Materials are written as names only, skinning and
 * animation are not written at all. That is a deliberate floor rather than an oversight -- the
 * reverse of the full import path is a much larger piece of work, and shipping the geometry half
 * verified is better than shipping all of it unverified.
 */
class ASSIMPCORE_API FAssimpExporter
{
public:
	/** One mesh to write, in Unreal space. */
	struct FExportMesh
	{
		/** Name the mesh carries in the written file. */
		FString Name;

		/** Geometry, in Unreal coordinates. Not copied, so it must outlive the call. */
		const FMeshDescription* MeshDescription = nullptr;
	};

	/**
	 * Formats the loaded Assimp library can write.
	 *
	 * Queried from the library rather than hard-coded, for the same reason the import list is: what
	 * a build can write depends on how it was compiled. A library built with ASSIMP_NO_EXPORT
	 * reports none, and every export then fails -- which this makes visible up front instead.
	 */
	static TArray<FAssimpExportFormat> GetSupportedFormats();

	/** True when the loaded library can write that format id. */
	static bool IsFormatSupported(const FString& FormatId);

	/**
	 * Best-guess format id for a file extension, or empty when none matches.
	 *
	 * Where several ids share an extension the first the library reports wins, which is Assimp's own
	 * preference order.
	 */
	static FString FindFormatIdForExtension(const FString& Extension);

	/**
	 * Writes meshes to a file.
	 *
	 * @param Meshes      Geometry to write, in Unreal space.
	 * @param FilePath    Destination. Its directory is created if it does not exist.
	 * @param FormatId    Assimp format id; see GetSupportedFormats. Empty to infer from the
	 *                    extension.
	 * @param OutResult   Receives success, the error, and how much was written.
	 * @return            True on success.
	 */
	static bool ExportMeshes(
		TArrayView<const FExportMesh> Meshes,
		const FString& FilePath,
		const FString& FormatId,
		FAssimpExportResult& OutResult);
};
