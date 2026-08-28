// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpIncludes.h"
#include "CoreMinimal.h"

/**
 * Performs the call into Assimp behind a structured-exception guard.
 *
 * Why this exists
 * ---------------
 * Assimp does not survive every malformed file. Several of its importers dereference null or run off
 * the end of a buffer when a file lies about its own structure -- Assimp's own test corpus contains
 * fixtures (glTF2/IndexOutOfRange, glTF2/MissingBin, and neighbours) that reproduce it. The failure
 * arrives as an EXCEPTION_ACCESS_VIOLATION.
 *
 * A C++ try/catch cannot help. On Windows an access violation is a structured (SEH) exception, not a
 * C++ one, and Unreal compiles with /EHsc, under which neither `catch (const std::exception&)` nor
 * even `catch (...)` intercepts it. The process simply dies. For a plugin whose entire purpose is
 * parsing files it did not author -- user-supplied models, downloaded content -- taking the editor
 * or the game down with it is not an acceptable failure mode.
 *
 * So the call is wrapped in __try/__except. MSVC forbids SEH in a function that needs C++ object
 * unwinding, which is why this lives in its own translation-unit-local helper holding nothing but
 * pointers and integers: the caller keeps its FStrings and smart pointers, this frame keeps none.
 *
 * Honest limits
 * -------------
 * This is mitigation, not a cure. After an access violation Assimp's internal state is undefined; we
 * only get away with continuing because the caller destroys the Importer immediately afterwards and
 * reports failure. Memory allocated before the fault leaks. Stack overflow (which glTF2/RecursiveNodes
 * can provoke) is deliberately NOT handled -- the guard page is already gone by then, and pretending
 * to recover is worse than failing. Genuine robustness against hostile input would mean parsing in a
 * separate process, which is beyond this plugin's scope.
 */
namespace AssimpReadGuard
{
	/**
	 * Reads a scene, converting a hardware fault inside Assimp into a null return.
	 *
	 * @param Importer         Configured importer.
	 * @param Utf8FilePath     File to read, or null to read from Buffer instead.
	 * @param Buffer           Bytes to read when Utf8FilePath is null.
	 * @param Utf8FormatHint   Extension hint for the memory case.
	 * @param Flags            Post-processing flags.
	 * @param bOutHardwareFault Set to true when a structured exception was intercepted.
	 * @return                 The scene, or null on failure.
	 */
	const aiScene* Read(
		Assimp::Importer& Importer,
		const char* Utf8FilePath,
		TArrayView<const uint8> Buffer,
		const char* Utf8FormatHint,
		unsigned int Flags,
		bool& bOutHardwareFault);
}
