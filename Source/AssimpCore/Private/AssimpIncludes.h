// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

//
// The single point at which Assimp headers enter this plugin.
//
// Every translation unit that needs Assimp includes THIS header and never an "assimp/..." header
// directly. That keeps the guard sequence below in exactly one place, and makes the no-leakage rule
// mechanically checkable: `grep -r '#include "assimp/' Source` should only ever match this file.
//
// This header lives in Private/ and must never be included from a Public/ header.
//
// Why the guards matter
// ---------------------
// Assimp's headers reach for <windows.h> on Windows. Left unguarded, that drags in the Win32 macro
// set -- UpdateResource, GetObject, DrawText, CopyFile, SendMessage and friends -- which then
// silently rewrites unrelated Unreal code that happens to use those names as identifiers. The
// classic symptom is a confusing "UpdateResourceW" compile error in a file that has nothing to do
// with Assimp, reported from a module that merely includes a header that includes Assimp.
//
// Unreal's fix is Windows/AllowWindowsPlatformTypes.h, which pulls <windows.h> in through
// WindowsHWrapper.h -> PostWindowsApi.h; the latter #undefs precisely those macros. Including the
// guard BEFORE Assimp means <windows.h> is already fully included and already de-macro'd by the
// time Assimp asks for it, so Assimp's own #include <windows.h> is a no-op include-guard hit.
//
// Note that HideWindowsPlatformTypes.h alone is NOT sufficient: it only undoes INT/UINT/DWORD/
// FLOAT/TRUE/FALSE. The macro cleanup we actually depend on happens on the Allow side.
//

#pragma once

#include "CoreMinimal.h"

// Unreal's check() is a function-like macro. If any Assimp header uses `check` as an identifier the
// expansion is a hard error, so stash it for the duration of the third-party includes.
#pragma push_macro("check")
#undef check

#if PLATFORM_WINDOWS
	#include "Windows/AllowWindowsPlatformTypes.h"
	#include "Windows/AllowWindowsPlatformAtomics.h"
#endif

THIRD_PARTY_INCLUDES_START

#include "assimp/Exporter.hpp"
#include "assimp/Importer.hpp"
#include "assimp/IOStream.hpp"
#include "assimp/IOSystem.hpp"
#include "assimp/LogStream.hpp"
#include "assimp/Logger.hpp"
#include "assimp/DefaultLogger.hpp"
#include "assimp/ProgressHandler.hpp"

#include "assimp/anim.h"
#include "assimp/camera.h"
#include "assimp/light.h"
#include "assimp/material.h"
#include "assimp/mesh.h"
#include "assimp/metadata.h"
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "assimp/version.h"

THIRD_PARTY_INCLUDES_END

#if PLATFORM_WINDOWS
	#include "Windows/HideWindowsPlatformAtomics.h"
	#include "Windows/HideWindowsPlatformTypes.h"
#endif

#pragma pop_macro("check")
