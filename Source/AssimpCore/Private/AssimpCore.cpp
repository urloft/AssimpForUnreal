// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpCore.h"

#include "AssimpIncludes.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogAssimp);

#define LOCTEXT_NAMESPACE "AssimpCore"

namespace
{
	/** Module name as registered with the module manager. */
	const FName AssimpCoreModuleName(TEXT("AssimpCore"));

	/** Plugin name, used to locate our own Binaries directory. */
	const TCHAR* const AssimpPluginName = TEXT("AssimpForUnreal");
}

/**
 * Concrete AssimpCore module.
 *
 * Resolves the delay-loaded Assimp shared library at startup and releases it at shutdown.
 */
class FAssimpCoreModule final : public IAssimpCoreModule
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface

	//~ Begin IAssimpCoreModule
	virtual bool IsAssimpLibraryLoaded() const override { return AssimpLibraryHandle != nullptr; }
	virtual FString GetAssimpVersionString() const override;
	//~ End IAssimpCoreModule

private:
	/**
	 * Builds the ordered list of locations to try when resolving the Assimp shared library.
	 *
	 * AssimpLibrary.Build.cs stages the DLL into the plugin's own Binaries/ThirdParty directory and
	 * publishes that location as ASSIMP_SHARED_LIBRARY_PLUGIN_SUBPATH, so that is tried first. Next
	 * comes the directory holding the host executable, which covers a project that has flattened its
	 * dependencies during packaging. The bare filename is tried last, deferring to the platform's
	 * default library search order.
	 */
	static TArray<FString> BuildSearchCandidates();

	/**
	 * Loads every other DLL in the staged directory, so assimp.dll's imports resolve from
	 * already-loaded modules rather than from the OS search path.
	 */
	void LoadDependencyLibraries();

	/** Handle returned by FPlatformProcess::GetDllHandle, or null if the library is not loaded. */
	void* AssimpLibraryHandle = nullptr;

	/** Handles for dependency DLLs, released in reverse order at shutdown. */
	TArray<void*> DependencyHandles;
};

IMPLEMENT_MODULE(FAssimpCoreModule, AssimpCore)

IAssimpCoreModule& IAssimpCoreModule::Get()
{
	return FModuleManager::LoadModuleChecked<FAssimpCoreModule>(AssimpCoreModuleName);
}

bool IAssimpCoreModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(AssimpCoreModuleName);
}

TArray<FString> FAssimpCoreModule::BuildSearchCandidates()
{
	const FString LibraryName(ASSIMP_SHARED_LIBRARY_NAME);

	TArray<FString> Candidates;

	// 1. Where AssimpLibrary.Build.cs stages it, inside the plugin.
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(AssimpPluginName))
	{
		Candidates.Add(FPaths::Combine(
			Plugin->GetBaseDir(),
			ASSIMP_SHARED_LIBRARY_PLUGIN_SUBPATH,
			LibraryName));
	}

	// 2. Next to the host executable, for layouts that flatten dependencies during packaging.
	Candidates.Add(FPaths::Combine(FPlatformProcess::BaseDir(), LibraryName));

	// 3. Bare name: fall back to the platform's default library search order.
	Candidates.Add(LibraryName);

	return Candidates;
}

void FAssimpCoreModule::StartupModule()
{
	const TArray<FString> Candidates = BuildSearchCandidates();

	// Load dependency DLLs before assimp.dll.
	//
	// assimp.dll imports draco.dll (Assimp's contrib dependencies follow the global
	// BUILD_SHARED_LIBS). The OS resolves a module's imports through the standard search order,
	// which does NOT include the importing module's own directory -- so a draco.dll sitting next to
	// assimp.dll would not be found, and assimp.dll would fail to load entirely. Loading the
	// dependency first by full path puts it in the process under its own module name, and the loader
	// then satisfies assimp.dll's import from the already-loaded module.
	LoadDependencyLibraries();

	for (const FString& Candidate : Candidates)
	{
		// A bare filename has no directory part, so let the OS resolve it. Anything else must
		// actually exist before we try, otherwise GetDllHandle logs its own noisy failure.
		const bool bIsBareName = FPaths::GetPath(Candidate).IsEmpty();
		if (!bIsBareName && !FPaths::FileExists(Candidate))
		{
			UE_LOG(LogAssimp, Verbose, TEXT("Assimp library not present at '%s'."), *Candidate);
			continue;
		}

		AssimpLibraryHandle = FPlatformProcess::GetDllHandle(*Candidate);
		if (AssimpLibraryHandle != nullptr)
		{
			UE_LOG(LogAssimp, Log, TEXT("Loaded Assimp library from '%s' (version %s)."),
				*Candidate, *GetAssimpVersionString());
			return;
		}

		UE_LOG(LogAssimp, Verbose, TEXT("Failed to load Assimp library from '%s'."), *Candidate);
	}

	// Not fatal: AssimpCore's entry points all check IsAssimpLibraryLoaded() and fail cleanly, so a
	// missing library disables importing rather than taking the editor down.
	UE_LOG(LogAssimp, Error,
		TEXT("Could not load the Assimp shared library ('%s'). Assimp import will be unavailable. ")
		TEXT("Tried %d location(s): %s"),
		ASSIMP_SHARED_LIBRARY_NAME,
		Candidates.Num(),
		*FString::Join(Candidates, TEXT("; ")));
}

void FAssimpCoreModule::LoadDependencyLibraries()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(AssimpPluginName);
	if (!Plugin.IsValid())
	{
		return;
	}

	const FString StagedDirectory =
		FPaths::Combine(Plugin->GetBaseDir(), ASSIMP_SHARED_LIBRARY_PLUGIN_SUBPATH);

	TArray<FString> DllNames;
	IFileManager::Get().FindFiles(DllNames, *FPaths::Combine(StagedDirectory, TEXT("*.dll")), true, false);

	const FString MainLibrary(ASSIMP_SHARED_LIBRARY_NAME);

	for (const FString& DllName : DllNames)
	{
		if (DllName.Equals(MainLibrary, ESearchCase::IgnoreCase))
		{
			continue;
		}

		const FString FullPath = FPaths::Combine(StagedDirectory, DllName);
		if (void* Handle = FPlatformProcess::GetDllHandle(*FullPath))
		{
			DependencyHandles.Add(Handle);
			UE_LOG(LogAssimp, Log, TEXT("Loaded Assimp dependency '%s'."), *DllName);
		}
		else
		{
			UE_LOG(LogAssimp, Warning,
				TEXT("Could not load Assimp dependency '%s'; formats relying on it will be ")
				TEXT("unavailable and Assimp itself may fail to load."), *FullPath);
		}
	}
}

void FAssimpCoreModule::ShutdownModule()
{
	if (AssimpLibraryHandle != nullptr)
	{
		FPlatformProcess::FreeDllHandle(AssimpLibraryHandle);
		AssimpLibraryHandle = nullptr;
	}

	// Reverse order: dependencies were loaded first, so they are released last.
	for (int32 Index = DependencyHandles.Num() - 1; Index >= 0; --Index)
	{
		FPlatformProcess::FreeDllHandle(DependencyHandles[Index]);
	}
	DependencyHandles.Reset();
}

FString FAssimpCoreModule::GetAssimpVersionString() const
{
	if (AssimpLibraryHandle == nullptr)
	{
		return FString();
	}

	// Safe to call now that the library is resolved: the delay-load thunk will bind on first use.
	return FString::Printf(TEXT("%u.%u.%u"),
		aiGetVersionMajor(),
		aiGetVersionMinor(),
		aiGetVersionPatch());
}

#undef LOCTEXT_NAMESPACE
