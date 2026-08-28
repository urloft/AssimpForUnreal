// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * Log category for everything Assimp-related, including messages forwarded from Assimp's own
 * logger by FAssimpLogBridge.
 */
ASSIMPCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogAssimp, Log, All);

/**
 * AssimpCore module.
 *
 * Owns the lifetime of the delay-loaded Assimp shared library. Assimp is linked against an import
 * library but marked delay-load (see AssimpLibrary.Build.cs), so the first call into Assimp would
 * otherwise trigger the OS loader at an arbitrary point. Instead this module resolves the library
 * explicitly at startup, from a known location, and reports a clear error if it cannot.
 */
class IAssimpCoreModule : public IModuleInterface
{
public:
	/** Accessor for the loaded module. Only valid once the module has been loaded. */
	static ASSIMPCORE_API IAssimpCoreModule& Get();

	/** Returns true if the module has been loaded into memory. */
	static ASSIMPCORE_API bool IsAvailable();

	/**
	 * Whether the Assimp shared library was resolved successfully at startup.
	 *
	 * Every public entry point in AssimpCore checks this before calling into Assimp, so a failed
	 * load degrades to clean, logged failures rather than a crash on first use.
	 */
	virtual bool IsAssimpLibraryLoaded() const = 0;

	/** Version string reported by the loaded Assimp library, or empty if it is not loaded. */
	virtual FString GetAssimpVersionString() const = 0;
};
