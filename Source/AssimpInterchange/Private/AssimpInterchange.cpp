// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpInterchange.h"

#include "AssimpForUnrealSettings.h"
#include "InterchangeAssimpTranslator.h"
#include "InterchangeManager.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogAssimpInterchange);

IMPLEMENT_MODULE(FAssimpInterchangeModule, AssimpInterchange)

void FAssimpInterchangeModule::StartupModule()
{
	UInterchangeManager& InterchangeManager = UInterchangeManager::GetInterchangeManager();

	if (!InterchangeManager.RegisterTranslator(UInterchangeAssimpTranslator::StaticClass()))
	{
		UE_LOG(LogAssimpInterchange, Error,
			TEXT("Failed to register the Assimp Interchange translator; Assimp formats will not be importable."));
		return;
	}

	const UAssimpForUnrealSettings* Settings = UAssimpForUnrealSettings::Get();
	if (Settings == nullptr)
	{
		return;
	}

	const TArray<FString> Extensions = Settings->GetEffectiveExtensions();

	UE_LOG(LogAssimpInterchange, Log,
		TEXT("Registered the Assimp Interchange translator for %d extension(s)."), Extensions.Num());

	if (Settings->bLogSupportedFormatsOnStartup)
	{
		UE_LOG(LogAssimpInterchange, Log, TEXT("Claimed extensions: %s"),
			*FString::Join(Extensions, TEXT(", ")));
	}
}

void FAssimpInterchangeModule::ShutdownModule()
{
	// UInterchangeManager offers no unregister, and it is shut down with the engine, so there is
	// nothing to undo here. Left explicit so the asymmetry is visibly deliberate rather than an
	// oversight.
}
