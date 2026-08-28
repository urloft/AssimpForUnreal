// Example actor demonstrating runtime model loading with AssimpForUnreal.

#include "AssimpModelLoader.h"

#include "AssimpExamples.h"

#include "AssimpAsyncImport.h"
#include "AssimpBlueprintLibrary.h"
#include "AssimpSceneObject.h"
#include "AssimpSceneTypes.h"
#include "Components/DynamicMeshComponent.h"
#include "Misc/Paths.h"

AAssimpModelLoader::AAssimpModelLoader()
{
	PrimaryActorTick.bCanEverTick = false;

	// Dynamic mesh components are attached to this, so the actor needs a root to parent them to.
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// A reasonable default so the actor does something useful straight out of the box.
	FilePath = TEXT("C:\\Users\\Pratik\\Downloads\\assimp-master\\assimp-master\\test\\models\\X\\Testwuson.X");
}

void AAssimpModelLoader::BeginPlay()
{
	Super::BeginPlay();

	UE_LOG(LogAssimpExamples, Log, TEXT("Assimp version: %s"), *UAssimpBlueprintLibrary::GetAssimpVersion());

	LoadNow();
}

void AAssimpModelLoader::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Do not leave a worker thread parsing into an object that is about to be torn down.
	CancelLoad();

	Super::EndPlay(EndPlayReason);
}

void AAssimpModelLoader::LoadNow()
{
	if (FilePath.IsEmpty())
	{
		UE_LOG(LogAssimpExamples, Warning, TEXT("No FilePath set."));
		return;
	}

	// Reject unreadable files before starting work, so the failure names the real problem rather
	// than surfacing as a parse error later.
	const FString Extension = FPaths::GetExtension(FilePath);
	if (!UAssimpBlueprintLibrary::IsExtensionSupported(Extension))
	{
		UE_LOG(LogAssimpExamples, Error,
			TEXT("Assimp cannot read '.%s'. Supported extensions: %s"),
			*Extension,
			*FString::Join(UAssimpBlueprintLibrary::GetSupportedImportExtensions(), TEXT(", ")));
		return;
	}

	CancelLoad();

	// Clear anything a previous load spawned, so repeated calls replace rather than accumulate.
	for (const TObjectPtr<UDynamicMeshComponent>& Component : SpawnedComponents)
	{
		if (Component != nullptr)
		{
			Component->DestroyComponent();
		}
	}
	SpawnedComponents.Reset();
	LoadedScene = nullptr;
	LastLoggedProgressPercent = -1;

	if (bLoadAsynchronously)
	{
		LoadAsync();
	}
	else
	{
		LoadBlocking();
	}
}

void AAssimpModelLoader::CancelLoad()
{
	if (PendingImport != nullptr)
	{
		// Completed still fires, with bCancelled set, so the handler does the clearing up.
		PendingImport->Cancel();
	}
}

void AAssimpModelLoader::LoadBlocking()
{
	UE_LOG(LogAssimpExamples, Log, TEXT("Loading '%s' (blocking)..."), *FilePath);

	FAssimpRuntimeImportResult Result;

	// `this` becomes the handle's outer, tying the parsed scene's lifetime to this actor.
	UAssimpSceneObject* SceneObject = UAssimpBlueprintLibrary::ImportSceneFromFile(
		this, FilePath, ImportSettings, Result);

	SpawnLoadedScene(SceneObject, Result);
}

void AAssimpModelLoader::LoadAsync()
{
	UE_LOG(LogAssimpExamples, Log, TEXT("Loading '%s' (async)..."), *FilePath);

	PendingImport = UAssimpAsyncImport::AsyncImportSceneFromFile(this, FilePath, ImportSettings);
	if (PendingImport == nullptr)
	{
		UE_LOG(LogAssimpExamples, Error, TEXT("Could not start the import."));
		return;
	}

	// Both delegates fire on the game thread, so the handlers may touch actors and components
	// freely. AddDynamic requires the handlers to be UFUNCTIONs.
	PendingImport->Completed.AddDynamic(this, &AAssimpModelLoader::HandleImportCompleted);
	PendingImport->Progress.AddDynamic(this, &AAssimpModelLoader::HandleImportProgress);

	// UAssimpAsyncImport is a UBlueprintAsyncActionBase. Blueprint calls Activate() for you; from
	// C++ it has to be called explicitly, otherwise the import never starts.
	PendingImport->Activate();
}

void AAssimpModelLoader::HandleImportProgress(float Fraction)
{
	const int32 Percent = FMath::FloorToInt(Fraction * 100.0f);
	if (Percent != LastLoggedProgressPercent)
	{
		LastLoggedProgressPercent = Percent;
		UE_LOG(LogAssimpExamples, Verbose, TEXT("Import progress: %d%%"), Percent);
	}
}

void AAssimpModelLoader::HandleImportCompleted(
	UAssimpSceneObject* SceneObject,
	FAssimpRuntimeImportResult Result)
{
	PendingImport = nullptr;

	if (Result.bCancelled)
	{
		UE_LOG(LogAssimpExamples, Log, TEXT("Import of '%s' was cancelled."), *FilePath);
		return;
	}

	SpawnLoadedScene(SceneObject, Result);
}

void AAssimpModelLoader::SpawnLoadedScene(
	UAssimpSceneObject* SceneObject,
	const FAssimpRuntimeImportResult& Result)
{
	if (!Result.bSucceeded || SceneObject == nullptr || !SceneObject->IsValidScene())
	{
		UE_LOG(LogAssimpExamples, Error, TEXT("Import of '%s' failed: %s"),
			*FilePath, *Result.ErrorMessage);
		return;
	}

	// Warnings accompany a successful import -- a texture that could not be resolved, a feature the
	// importer skipped. Worth surfacing: they explain a model that loads but looks wrong.
	for (const FString& Warning : Result.Warnings)
	{
		UE_LOG(LogAssimpExamples, Warning, TEXT("%s"), *Warning);
	}

	LoadedScene = SceneObject;

	const FAssimpSceneInfo& Info = SceneObject->GetSceneInfo();
	UE_LOG(LogAssimpExamples, Log,
		TEXT("Loaded '%s' in %.2fs: %d node(s), %d mesh(es), %d material(s), %d animation(s), scale %g."),
		*FilePath,
		Result.ElapsedSeconds,
		Info.Nodes.Num(),
		Info.Meshes.Num(),
		Info.Materials.Num(),
		Info.AnimationNames.Num(),
		Info.AppliedScale);

	for (int32 Index = 0; Index < Info.Meshes.Num(); ++Index)
	{
		const FAssimpMeshInfo& Mesh = Info.Meshes[Index];
		UE_LOG(LogAssimpExamples, Log,
			TEXT("  mesh %d '%s': %d tri, %d vert, %d UV set(s), material %d, bounds %s"),
			Index, *Mesh.Name, Mesh.NumTriangles, Mesh.NumVertices,
			Mesh.NumUVChannels, Mesh.MaterialIndex, *Mesh.BoundingBox.ToString());
	}

	// Places one component per mesh (or per node, when merging) using the file's node hierarchy.
	SpawnedComponents.Reset();
	for (UDynamicMeshComponent* Component :
		UAssimpBlueprintLibrary::SpawnSceneAsDynamicMeshComponents(this, SceneObject, bMergeMeshesByNode))
	{
		SpawnedComponents.Add(Component);
	}

	UE_LOG(LogAssimpExamples, Log, TEXT("Spawned %d dynamic mesh component(s)."), SpawnedComponents.Num());

	// The scene handle can be released once the meshes are built, if nothing more is needed from it.
	// Kept here so LoadedScene can still serve BuildDynamicMesh, embedded textures, and metadata.
}
