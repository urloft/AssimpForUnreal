// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpIncludes.h"
#include "AssimpImportSettings.h"
#include "CoreMinimal.h"

namespace Assimp
{
	class Importer;
}

/**
 * Translates FAssimpImportSettings into Assimp's post-processing configuration.
 *
 * Kept separate from the settings struct itself so that FAssimpImportSettings stays a plain,
 * Assimp-free UStruct that public headers can expose.
 */
class FAssimpPostProcess
{
public:
	/**
	 * Builds the aiProcess_* bitmask for the given settings.
	 *
	 * Assumes Settings has already been through GetSanitized(): the mutually exclusive combinations
	 * (scene-graph optimisation with skeletal import, in particular) are resolved there rather than
	 * here, so that both import paths share one set of invariants.
	 *
	 * Note that aiProcess_MakeLeftHanded, aiProcess_FlipWindingOrder and aiProcess_FlipUVs are
	 * deliberately never set. The entire coordinate conversion is done by FAssimpAxisConverter, so
	 * enabling Assimp's versions as well would flip handedness twice and mirror the result.
	 */
	static unsigned int BuildFlags(const FAssimpImportSettings& Settings, bool bRelaxed = false);

	/**
	 * Applies the property values that accompany the flags.
	 *
	 * Several post-processing steps are parameterised through importer properties rather than
	 * flags -- the normal smoothing angle and the bone influence limit among them -- so flags alone
	 * are not enough to configure an import.
	 *
	 * @param bRelaxed  Drops structural validation and stops discarding point/line meshes. Used only
	 *                  to classify a failure after the fact: it distinguishes "this file is corrupt"
	 *                  from "this file is a point cloud", which the strict configuration collapses
	 *                  into one opaque error. Never used for a successful import, so it cannot
	 *                  affect normal behaviour.
	 */
	static void ApplyProperties(
		Assimp::Importer& Importer,
		const FAssimpImportSettings& Settings,
		bool bRelaxed = false);
};
