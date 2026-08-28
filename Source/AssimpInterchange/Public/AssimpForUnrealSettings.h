// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpImportSettings.h"
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "AssimpForUnrealSettings.generated.h"

/**
 * Project settings controlling which file extensions the Assimp translator claims.
 *
 * Why this is configurable rather than fixed
 * ------------------------------------------
 * UInterchangeManager picks a translator by walking its registration list in order, so the first
 * registered translator that claims an extension wins. The engine's own translators (FBX, glTF, OBJ)
 * register during InterchangeImport's PreDefault startup; this plugin registers at PostEngineInit,
 * after them. Claiming ".fbx" here would therefore have no effect while looking like it should,
 * which is worse than not claiming it.
 *
 * So the default list covers only formats the engine cannot read at all -- where Assimp is the sole
 * option and there is no contest -- and the formats the engine does handle are listed separately as
 * explicit opt-ins for projects that genuinely prefer Assimp's importer for them.
 */
UCLASS(Config = Engine, DefaultConfig, meta = (DisplayName = "Assimp For Unreal"))
class ASSIMPINTERCHANGE_API UAssimpForUnrealSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UAssimpForUnrealSettings();

	//~ Begin UDeveloperSettings
	virtual FName GetCategoryName() const override;
	//~ End UDeveloperSettings

	/** Convenience accessor for the settings CDO. */
	static const UAssimpForUnrealSettings* Get();

	/**
	 * Extensions the translator claims, lower-case and without a leading dot.
	 *
	 * Defaults to formats no engine translator handles. Removing an entry makes the plugin ignore
	 * that format entirely.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Formats")
	TArray<FString> SupportedExtensions;

	/**
	 * Additionally claim ".obj".
	 *
	 * WARNING -- on a stock engine this does not reliably do anything.
	 *
	 * The engine ships its own OBJ translator, and when two translators claim one extension there is
	 * no defined winner. UInterchangeManager holds translators in a
	 * TSet<TObjectPtr<const UClass>> and GetTranslatorForSourceData returns the first entry whose
	 * CanImportSourceData accepts the file, so selection follows set-iteration order: arbitrary,
	 * and impossible for a plugin to influence. Interchange also exposes no way to unregister or
	 * deny a translator.
	 *
	 * Enabling this is therefore only meaningful on a source engine build where the competing
	 * RegisterTranslator call in InterchangeImportModule.cpp has been removed. On a launcher build,
	 * to put an OBJ/FBX/glTF file through Assimp, use the AssimpRuntime API (which bypasses
	 * Interchange entirely) or rename the file to an extension no other translator claims.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Formats|Engine-Handled Formats")
	bool bClaimObj = false;

	/** Additionally claim ".fbx". Subject to the same unspecified-selection caveat as bClaimObj. */
	UPROPERTY(EditAnywhere, Config, Category = "Formats|Engine-Handled Formats")
	bool bClaimFbx = false;

	/** Additionally claim ".gltf" and ".glb". Subject to the same caveat as bClaimObj. */
	UPROPERTY(EditAnywhere, Config, Category = "Formats|Engine-Handled Formats")
	bool bClaimGltf = false;

	/**
	 * How Assimp parses files during an editor import.
	 *
	 * These live in project settings rather than in the per-import dialog for a structural reason:
	 * Interchange calls a translator's Translate() before any pipeline options are applied, and
	 * these options govern parsing itself -- coordinate conversion, normal generation, mesh
	 * optimisation. Exposing them per-import would mean either re-parsing the file after the dialog
	 * (doubling the cost of every import) or presenting options that silently had no effect. A
	 * project-wide default is the honest place for them.
	 *
	 * Per-import control is still available on the runtime API, which takes settings directly.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Import")
	FAssimpImportSettings ImportSettings;

	/**
	 * Log the extension list the translator reports at startup.
	 *
	 * Useful when diagnosing why a particular file did or did not route to this plugin.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Diagnostics")
	bool bLogSupportedFormatsOnStartup = false;

	/** Builds the effective extension list, applying the opt-in flags. */
	TArray<FString> GetEffectiveExtensions() const;

	/**
	 * Extensions that are claimed but will not take effect because an engine translator registers
	 * earlier and wins. Empty when there is no such conflict.
	 */
	TArray<FString> GetIneffectiveClaimedExtensions() const;
};
