// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/** Log category for the editor-side import surface. */
ASSIMPEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogAssimpEditor, Log, All);

/**
 * AssimpEditor module.
 *
 * Hosts the Interchange import pipeline and the plugin's project settings.
 */
class FAssimpEditorModule final : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface
};
