// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpAsyncImport.h"

#include "AssimpRuntime.h"
#include "AssimpScene.h"
#include "AssimpSceneObject.h"

#include "Async/Async.h"

UAssimpAsyncImport* UAssimpAsyncImport::AsyncImportSceneFromFile(
	UObject* WorldContextObject,
	const FString& FilePath,
	const FAssimpImportSettings& Settings)
{
	UAssimpAsyncImport* Action = NewObject<UAssimpAsyncImport>();
	Action->ContextObject = WorldContextObject;
	Action->SourceFilePath = FilePath;
	Action->ImportSettings = Settings;

	return Action;
}

void UAssimpAsyncImport::Activate()
{
	// Nothing else holds a reference to a Blueprint async node once Activate returns, so pin it for
	// the duration of the work.
	SelfReference = TStrongObjectPtr<UAssimpAsyncImport>(this);

	if (SourceFilePath.IsEmpty())
	{
		FAssimpRuntimeImportResult Result;
		Result.ErrorMessage = TEXT("No file path was supplied.");
		FinishOnGameThread(nullptr, Result);
		return;
	}

	// Parsing is CPU-bound with no game-thread affinity, so it goes to the task graph. Only the
	// completion hop back touches the object system.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
	{
		PerformImport();
	});
}

void UAssimpAsyncImport::Cancel()
{
	bCancelRequested.store(true, std::memory_order_relaxed);
}

void UAssimpAsyncImport::PerformImport()
{
	// Progress arrives on this worker thread. Marshal each report to the game thread, because a
	// Blueprint delegate must not be broadcast from anywhere else.
	FAssimpProgressDelegate ProgressDelegate;
	ProgressDelegate.BindLambda([this](float Fraction) -> bool
	{
		if (bCancelRequested.load(std::memory_order_relaxed))
		{
			// Returning false makes Assimp abort and unwind.
			return false;
		}

		// Fire-and-forget: the worker must not block on the game thread, or an import started from
		// a blocked game thread would deadlock.
		AsyncTask(ENamedThreads::GameThread, [this, Fraction]()
		{
			if (!bFinished.load(std::memory_order_acquire))
			{
				Progress.Broadcast(Fraction);
			}
		});

		return true;
	});

	FAssimpLoadResult LoadResult;
	TSharedPtr<FAssimpScene> Scene = FAssimpScene::LoadFromFile(
		SourceFilePath, ImportSettings, LoadResult, ProgressDelegate);

	FAssimpRuntimeImportResult Result;
	Result.bSucceeded = LoadResult.bSucceeded;
	Result.bCancelled = LoadResult.bCancelled || bCancelRequested.load(std::memory_order_relaxed);
	Result.ErrorMessage = LoadResult.ErrorMessage;
	Result.ElapsedSeconds = static_cast<float>(LoadResult.ElapsedSeconds);

	for (const FAssimpDiagnostic& Diagnostic : LoadResult.Diagnostics)
	{
		if (Diagnostic.Severity == EAssimpDiagnosticSeverity::Warning)
		{
			Result.Warnings.Add(Diagnostic.Message);
		}
	}

	FinishOnGameThread(MoveTemp(Scene), Result);
}

void UAssimpAsyncImport::FinishOnGameThread(
	TSharedPtr<FAssimpScene> Scene,
	const FAssimpRuntimeImportResult& Result)
{
	// Creating the handle and broadcasting both touch the object system, so both must happen on the
	// game thread.
	AsyncTask(ENamedThreads::GameThread, [this, Scene = MoveTemp(Scene), Result]()
	{
		// Guard against a double completion, which would otherwise be possible if a future change
		// added another failure path.
		bool bExpected = false;
		if (!bFinished.compare_exchange_strong(bExpected, true))
		{
			return;
		}

		UAssimpSceneObject* SceneObject = Scene.IsValid()
			? UAssimpSceneObject::Create(ContextObject, Scene)
			: nullptr;

		if (Result.bSucceeded && SceneObject != nullptr)
		{
			UE_LOG(LogAssimpRuntime, Log,
				TEXT("Async import of '%s' completed in %.2fs."), *SourceFilePath, Result.ElapsedSeconds);
		}
		else if (Result.bCancelled)
		{
			UE_LOG(LogAssimpRuntime, Log, TEXT("Async import of '%s' was cancelled."), *SourceFilePath);
		}
		else
		{
			UE_LOG(LogAssimpRuntime, Warning,
				TEXT("Async import of '%s' failed: %s"), *SourceFilePath, *Result.ErrorMessage);
		}

		Completed.Broadcast(SceneObject, Result);

		// Release the self-reference last: this may be the final reference, so nothing may touch
		// member state afterwards.
		SelfReference.Reset();
	});
}
