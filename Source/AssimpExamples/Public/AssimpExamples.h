// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/** Log category for the example content. */
ASSIMPEXAMPLES_API DECLARE_LOG_CATEGORY_EXTERN(LogAssimpExamples, Log, All);

/**
 * AssimpExamples module.
 *
 * Sample content only. Nothing in the plugin proper depends on it; see AssimpExamples.Build.cs for
 * how to remove it.
 */
class FAssimpExamplesModule final : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface
};
