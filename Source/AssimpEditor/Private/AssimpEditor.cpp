// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpEditor.h"

#include "AssimpForUnrealSettings.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogAssimpEditor);

IMPLEMENT_MODULE(FAssimpEditorModule, AssimpEditor)

void FAssimpEditorModule::StartupModule()
{
	const UAssimpForUnrealSettings* Settings = UAssimpForUnrealSettings::Get();
	if (Settings == nullptr)
	{
		return;
	}

	// Warn about claimed extensions that cannot take effect.
	//
	// UInterchangeManager resolves a translator by registration order, and the engine's translators
	// register during InterchangeImport's PreDefault startup while this plugin loads at
	// PostEngineInit. An extension the engine already owns is therefore silently ignored no matter
	// what the settings say. Surfacing that here turns a confusing non-event -- "I enabled FBX and
	// nothing changed" -- into an explanation.
	const TArray<FString> Ineffective = Settings->GetIneffectiveClaimedExtensions();
	if (!Ineffective.IsEmpty())
	{
		UE_LOG(LogAssimpEditor, Warning,
			TEXT("These extensions are claimed by AssimpForUnreal but an engine translator will ")
			TEXT("handle them instead, because engine translators register first: %s. ")
			TEXT("To route them through Assimp, disable the corresponding engine translator."),
			*FString::Join(Ineffective, TEXT(", ")));
	}
}

void FAssimpEditorModule::ShutdownModule()
{
}
