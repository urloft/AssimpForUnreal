// Example actor demonstrating runtime model loading with AssimpForUnreal.

#pragma once

#include "AssimpImportSettings.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AssimpModelLoader.generated.h"

class UAssimpAsyncImport;
class UAssimpSceneObject;
class UDynamicMeshComponent;
struct FAssimpRuntimeImportResult;

/**
 * Loads a model from disk at run time and spawns it as dynamic mesh components.
 *
 * Drop one in a level, point FilePath at a model, and press Play. Works in the editor and in a
 * packaged build alike, which is the reason to use this path rather than the Interchange importer:
 * the file does not have to exist when the project is cooked.
 *
 * Both approaches are shown. Asynchronous is the default and the one to prefer: Assimp parsing is
 * CPU-bound and blocking the game thread on a large model is long enough to be seen as a hitch.
 */
UCLASS()
class ASSIMPEXAMPLES_API AAssimpModelLoader : public AActor
{
	GENERATED_BODY()

public:
	AAssimpModelLoader();

	//~ Begin AActor
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor

	/**
	 * Model to load. Absolute path, or anything Unreal's file manager can resolve.
	 *
	 * Any of Assimp's 71 readable formats works here, including .fbx, .obj, .gltf and .glb -- this
	 * path calls Assimp directly rather than going through Interchange, so it does not compete with
	 * the engine's own importers for those extensions.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assimp")
	FString FilePath;

	/** Load off the game thread and report progress. Leave enabled unless you need the result immediately. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assimp")
	bool bLoadAsynchronously = true;

	/** Parsing and conversion options. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assimp")
	FAssimpImportSettings ImportSettings;

	/**
	 * Combine every mesh a node draws into one component, so a multi-material object becomes one
	 * component with several material slots instead of one component per material.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assimp")
	bool bMergeMeshesByNode = true;

	/** Load immediately, cancelling anything already in flight. Callable from Blueprint or console. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Assimp")
	void LoadNow();

	/** Cancel an in-flight asynchronous load. Harmless if nothing is running. */
	UFUNCTION(BlueprintCallable, Category = "Assimp")
	void CancelLoad();

	/**
	 * The scene from the last successful load, or null.
	 *
	 * Kept accessible so callers can go on to pull textures, embedded images, or metadata out of the
	 * scene after the meshes have been spawned.
	 */
	UFUNCTION(BlueprintPure, Category = "Assimp")
	UAssimpSceneObject* GetLoadedScene() const { return LoadedScene; }

private:
	/** Blocking variant. Simple, but stalls the game thread for the duration. */
	void LoadBlocking();

	/** Non-blocking variant. Parsing happens on a worker thread; callbacks arrive on the game thread. */
	void LoadAsync();

	/** Completion handler for the asynchronous path. Must be UFUNCTION to bind to a dynamic delegate. */
	UFUNCTION()
	void HandleImportCompleted(UAssimpSceneObject* SceneObject, FAssimpRuntimeImportResult Result);

	/** Progress handler for the asynchronous path. */
	UFUNCTION()
	void HandleImportProgress(float Fraction);

	/** Shared tail: spawn components and log what arrived. */
	void SpawnLoadedScene(UAssimpSceneObject* SceneObject, const FAssimpRuntimeImportResult& Result);

	/**
	 * Keeps the scene alive.
	 *
	 * UAssimpSceneObject is a handle onto a parsed Assimp scene; dropping the last reference releases
	 * the parsed data. Holding it in a UPROPERTY is what stops the garbage collector reclaiming it
	 * while we still intend to build meshes from it.
	 */
	UPROPERTY()
	TObjectPtr<UAssimpSceneObject> LoadedScene;

	/** In-flight asynchronous import, if any. UPROPERTY so it survives GC while running. */
	UPROPERTY()
	TObjectPtr<UAssimpAsyncImport> PendingImport;

	/** Components spawned by the last load, so a reload can remove them first. */
	UPROPERTY()
	TArray<TObjectPtr<UDynamicMeshComponent>> SpawnedComponents;

	/** Throttles progress logging to whole percentage points. */
	int32 LastLoggedProgressPercent = -1;
};
