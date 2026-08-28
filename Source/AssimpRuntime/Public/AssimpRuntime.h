// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/** Log category for the runtime import API. */
ASSIMPRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogAssimpRuntime, Log, All);

/**
 * AssimpRuntime module.
 *
 * Exposes the Blueprint and C++ surface for loading meshes at runtime. All Assimp work is delegated
 * to AssimpCore, so this module carries no third-party build settings of its own.
 */
class FAssimpRuntimeModule final : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface
};
