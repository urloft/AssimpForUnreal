// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/** Log category for the Interchange translator. */
ASSIMPINTERCHANGE_API DECLARE_LOG_CATEGORY_EXTERN(LogAssimpInterchange, Log, All);

/**
 * AssimpInterchange module.
 *
 * Registers the Assimp translator with the Interchange manager at startup.
 *
 * Registration order is significant: UInterchangeManager selects a translator by walking its
 * registration list in order, so translators registered earlier win for any extension they claim.
 * The engine's own translators register during InterchangeImport's PreDefault startup, and this
 * module loads at PostEngineInit -- deliberately after them. Consequently the engine keeps
 * ownership of .fbx, .gltf, .glb and .obj unless the user opts in via project settings, and this
 * translator picks up the long tail of formats the engine cannot read at all.
 */
class FAssimpInterchangeModule final : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface
};
