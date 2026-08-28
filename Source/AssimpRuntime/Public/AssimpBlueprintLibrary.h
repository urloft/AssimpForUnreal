// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpImportSettings.h"
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "AssimpBlueprintLibrary.generated.h"

class UAssimpSceneObject;
class UDynamicMeshComponent;
class UMaterialInterface;
class AActor;

/** Outcome of a runtime import, in a form Blueprint can branch on. */
USTRUCT(BlueprintType)
struct ASSIMPRUNTIME_API FAssimpRuntimeImportResult
{
	GENERATED_BODY()

	/** True when a scene was produced. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp")
	bool bSucceeded = false;

	/** True when the import stopped because it was cancelled rather than because it failed. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp")
	bool bCancelled = false;

	/** Why the import failed. Empty on success. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp")
	FString ErrorMessage;

	/** Warnings raised during a successful import, such as textures that could not be found. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp")
	TArray<FString> Warnings;

	/** Seconds spent parsing and post-processing. */
	UPROPERTY(BlueprintReadOnly, Category = "Assimp")
	float ElapsedSeconds = 0.0f;
};

/**
 * Runtime import API, usable from Blueprint and C++ in both the editor and packaged builds.
 *
 * This is the counterpart to the Interchange path: use that to bring a model in as a saved asset at
 * edit time, and this to load a file the game did not ship with -- user-supplied models, downloaded
 * content, or anything chosen at run time.
 *
 * Both paths run on the same conversion code in AssimpCore, so geometry that imports correctly in
 * the editor behaves identically here.
 */
UCLASS()
class ASSIMPRUNTIME_API UAssimpBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Parses a file on the calling thread.
	 *
	 * Blocks until finished, which for a large model is long enough to be visible as a hitch. Prefer
	 * AsyncImportSceneFromFile for anything user-facing; this exists for load screens, editor
	 * utilities, and tests where blocking is acceptable and simpler.
	 *
	 * @param Outer     Object that will own the returned handle. Controls when the scene is released.
	 * @param FilePath  Absolute path, or a path Unreal's file manager can resolve.
	 */
	UFUNCTION(BlueprintCallable, Category = "Assimp|Import",
		meta = (DisplayName = "Import Assimp Scene From File (Blocking)", DefaultToSelf = "Outer"))
	static UAssimpSceneObject* ImportSceneFromFile(
		UObject* Outer,
		const FString& FilePath,
		const FAssimpImportSettings& Settings,
		FAssimpRuntimeImportResult& OutResult);

	/**
	 * Parses a model already in memory.
	 *
	 * @param FormatHint Extension without the dot, e.g. "ply". Supply it whenever it is known:
	 *                   Assimp cannot reliably detect every format from content alone.
	 */
	UFUNCTION(BlueprintCallable, Category = "Assimp|Import",
		meta = (DisplayName = "Import Assimp Scene From Memory (Blocking)", DefaultToSelf = "Outer"))
	static UAssimpSceneObject* ImportSceneFromMemory(
		UObject* Outer,
		const TArray<uint8>& Buffer,
		const FString& FormatHint,
		const FAssimpImportSettings& Settings,
		FAssimpRuntimeImportResult& OutResult);

	/**
	 * Spawns a dynamic mesh component per mesh in the scene, parented to an actor and placed
	 * according to the file's node hierarchy.
	 *
	 * @param Actor           Actor to attach the components to.
	 * @param SceneObject     Scene to build from.
	 * @param bMergeByNode    Combine every mesh a node draws into one component, so a multi-material
	 *                        object becomes a single component with several material slots rather
	 *                        than one component per material.
	 * @param BaseMaterial    Material to instance for each slot. When null, every slot gets the
	 *                        engine's default surface material, which renders the geometry as flat
	 *                        grey.
	 *
	 *                        A material must be assigned either way, and not merely for appearance:
	 *                        a primitive with zero material slots produces an empty
	 *                        FMaterialRelevance, so the renderer gives it no pass flags and draws
	 *                        nothing at all. Geometry with no materials is invisible, not grey.
	 *
	 *                        Supply a material with BaseColor / Metallic / Roughness / Opacity /
	 *                        EmissiveColor parameters to have the source file's values applied to a
	 *                        dynamic instance per slot.
	 * @return                The components created, in creation order.
	 */
	UFUNCTION(BlueprintCallable, Category = "Assimp|Spawn")
	static TArray<UDynamicMeshComponent*> SpawnSceneAsDynamicMeshComponents(
		AActor* Actor,
		UAssimpSceneObject* SceneObject,
		bool bMergeByNode = true,
		UMaterialInterface* BaseMaterial = nullptr);

	/**
	 * Extensions Assimp can read, without the leading dot.
	 *
	 * Reported by the loaded Assimp library itself rather than from a hard-coded list, so it stays
	 * accurate across Assimp versions and build configurations.
	 */
	UFUNCTION(BlueprintPure, Category = "Assimp|Import")
	static TArray<FString> GetSupportedImportExtensions();

	/** True if Assimp claims it can read the given file, judged by extension. */
	UFUNCTION(BlueprintPure, Category = "Assimp|Import")
	static bool IsExtensionSupported(const FString& Extension);

	/** Version string of the loaded Assimp library, or empty if it is unavailable. */
	UFUNCTION(BlueprintPure, Category = "Assimp|Import")
	static FString GetAssimpVersion();
};
