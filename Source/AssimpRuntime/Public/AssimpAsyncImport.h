// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpBlueprintLibrary.h"
#include "AssimpImportSettings.h"
#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include "AssimpAsyncImport.generated.h"

class UAssimpSceneObject;

/** Fired when an asynchronous import finishes, whether it succeeded or not. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FAssimpAsyncImportCompleted,
	UAssimpSceneObject*, Scene,
	FAssimpRuntimeImportResult, Result);

/** Fired periodically while an asynchronous import runs. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FAssimpAsyncImportProgress,
	float, Fraction);

/**
 * Loads a model on a worker thread, reporting progress and allowing cancellation.
 *
 * This is the entry point a packaged game should use. Parsing a model is CPU-bound and can take
 * seconds for a large file, so doing it on the game thread stalls rendering and input for that whole
 * time. Assimp's parsing has no affinity for the game thread, so the work moves off it cleanly; only
 * the completion callback and the UObject creation return to it, because both touch the object
 * system.
 *
 * Blueprint usage: the node has Completed and Progress pins, and Cancel() can be called on the
 * returned object at any point.
 */
UCLASS(MinimalAPI)
class UAssimpAsyncImport : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** Fired on the game thread once the import finishes, fails, or is cancelled. */
	UPROPERTY(BlueprintAssignable, Category = "Assimp|Import")
	FAssimpAsyncImportCompleted Completed;

	/** Fired on the game thread as the import progresses. */
	UPROPERTY(BlueprintAssignable, Category = "Assimp|Import")
	FAssimpAsyncImportProgress Progress;

	/**
	 * Starts an asynchronous import.
	 *
	 * @param WorldContextObject  Provides the world; also becomes the owner of the resulting handle.
	 * @param FilePath            File to read.
	 * @param Settings            Import settings.
	 */
	UFUNCTION(BlueprintCallable, Category = "Assimp|Import",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
				DisplayName = "Import Assimp Scene From File (Async)"))
	static ASSIMPRUNTIME_API UAssimpAsyncImport* AsyncImportSceneFromFile(
		UObject* WorldContextObject,
		const FString& FilePath,
		const FAssimpImportSettings& Settings);

	/**
	 * Requests cancellation.
	 *
	 * Assimp is polled during parsing, so this takes effect at the next poll rather than instantly.
	 * Completed still fires, with bCancelled set.
	 */
	UFUNCTION(BlueprintCallable, Category = "Assimp|Import")
	ASSIMPRUNTIME_API void Cancel();

	//~ Begin UBlueprintAsyncActionBase
	ASSIMPRUNTIME_API virtual void Activate() override;
	//~ End UBlueprintAsyncActionBase

private:
	/** Runs on the worker thread. */
	void PerformImport();

	/** Delivers the outcome on the game thread. */
	void FinishOnGameThread(TSharedPtr<class FAssimpScene> Scene, const FAssimpRuntimeImportResult& Result);

	UPROPERTY()
	TObjectPtr<UObject> ContextObject;

	FString SourceFilePath;
	FAssimpImportSettings ImportSettings;

	/**
	 * Set by Cancel() on the game thread, read by the progress callback on the worker thread.
	 * Atomic because those are different threads and the read happens frequently.
	 */
	std::atomic<bool> bCancelRequested{ false };

	/**
	 * Keeps this object alive for the duration of the asynchronous work.
	 *
	 * A Blueprint async node has no other owner once Activate() returns, so without this it could be
	 * garbage collected while the worker thread is still using it.
	 */
	TStrongObjectPtr<UAssimpAsyncImport> SelfReference;

	/** Guards against Completed firing twice. */
	std::atomic<bool> bFinished{ false };
};
