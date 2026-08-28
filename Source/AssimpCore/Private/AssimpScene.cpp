// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpScene.h"

#include "AssimpAxisConverter.h"
#include "AssimpBridges.h"
#include "AssimpCore.h"
#include "AssimpIncludes.h"
#include "AssimpMeshConverter.h"
#include "AssimpPostProcess.h"
#include "AssimpReadGuard.h"

#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
	/**
	 * Formats whose native unit is the metre, and which do not declare a unit scale in metadata.
	 *
	 * Assimp reports UnitScaleFactor only for formats that carry it (notably FBX). For the rest we
	 * would otherwise assume 1 unit == 1 centimetre, which imports a glTF character at 1/100th size.
	 * Rather than leave the user to discover that and hand-enter 100, apply the format's known
	 * convention and log the decision.
	 */
	float GetDefaultUnitScaleForExtension(const FString& Extension)
	{
		static const TSet<FString> MetreBasedFormats = {
			TEXT("gltf"), TEXT("glb"),  // glTF specifies metres.
			TEXT("dae"),                // Collada usually declares its own unit; this is the fallback.
			TEXT("usd"), TEXT("usda"), TEXT("usdc"), TEXT("usdz"),
			TEXT("stp"), TEXT("step"),  // STEP is millimetre or metre; metre is the safer default.
		};

		return MetreBasedFormats.Contains(Extension.ToLower()) ? 100.0f : 1.0f;
	}

	/** Renders one aiMetadata entry as a string. */
	FString MetadataEntryToString(const aiMetadata& Metadata, unsigned int Index)
	{
		switch (Metadata.mValues[Index].mType)
		{
		case AI_BOOL:
		{
			bool Value = false;
			Metadata.Get(Index, Value);
			return Value ? TEXT("true") : TEXT("false");
		}
		case AI_INT32:
		{
			int32 Value = 0;
			Metadata.Get(Index, Value);
			return FString::FromInt(Value);
		}
		case AI_UINT64:
		{
			uint64 Value = 0;
			Metadata.Get(Index, Value);
			return FString::Printf(TEXT("%llu"), Value);
		}
		case AI_INT64:
		{
			int64 Value = 0;
			Metadata.Get(Index, Value);
			return FString::Printf(TEXT("%lld"), Value);
		}
		case AI_UINT32:
		{
			uint32 Value = 0;
			Metadata.Get(Index, Value);
			return FString::Printf(TEXT("%u"), Value);
		}
		case AI_FLOAT:
		{
			float Value = 0.0f;
			Metadata.Get(Index, Value);
			return FString::SanitizeFloat(Value);
		}
		case AI_DOUBLE:
		{
			double Value = 0.0;
			Metadata.Get(Index, Value);
			return FString::SanitizeFloat(Value);
		}
		case AI_AISTRING:
		{
			aiString Value;
			Metadata.Get(Index, Value);
			return FAssimpAxisConverter::ConvertString(Value);
		}
		case AI_AIVECTOR3D:
		{
			aiVector3D Value;
			Metadata.Get(Index, Value);
			return FString::Printf(TEXT("(%f, %f, %f)"), Value.x, Value.y, Value.z);
		}
		default:
			return FString();
		}
	}

	/** Flattens an aiMetadata table into a string map. */
	void FlattenMetadata(const aiMetadata* Metadata, TMap<FString, FString>& OutMap)
	{
		if (Metadata == nullptr)
		{
			return;
		}

		for (unsigned int Index = 0; Index < Metadata->mNumProperties; ++Index)
		{
			const FString Key = FAssimpAxisConverter::ConvertString(Metadata->mKeys[Index]);
			if (Key.IsEmpty())
			{
				continue;
			}

			OutMap.Add(Key, MetadataEntryToString(*Metadata, Index));
		}
	}

	/** Maps an Assimp texture type onto our slot enum. */
	EAssimpTextureSlot TextureTypeToSlot(aiTextureType Type)
	{
		switch (Type)
		{
		case aiTextureType_BASE_COLOR:        return EAssimpTextureSlot::BaseColor;
		case aiTextureType_DIFFUSE:           return EAssimpTextureSlot::BaseColor;
		case aiTextureType_NORMALS:           return EAssimpTextureSlot::Normal;
		case aiTextureType_NORMAL_CAMERA:     return EAssimpTextureSlot::Normal;
		case aiTextureType_METALNESS:         return EAssimpTextureSlot::Metallic;
		case aiTextureType_DIFFUSE_ROUGHNESS: return EAssimpTextureSlot::Roughness;
		case aiTextureType_SPECULAR:          return EAssimpTextureSlot::Specular;
		case aiTextureType_AMBIENT_OCCLUSION: return EAssimpTextureSlot::AmbientOcclusion;
		case aiTextureType_LIGHTMAP:          return EAssimpTextureSlot::Lightmap;
		case aiTextureType_EMISSIVE:          return EAssimpTextureSlot::Emissive;
		case aiTextureType_EMISSION_COLOR:    return EAssimpTextureSlot::Emissive;
		case aiTextureType_OPACITY:           return EAssimpTextureSlot::Opacity;
		case aiTextureType_DISPLACEMENT:      return EAssimpTextureSlot::Displacement;
		case aiTextureType_HEIGHT:            return EAssimpTextureSlot::Height;
		case aiTextureType_SHININESS:         return EAssimpTextureSlot::Shininess;
		case aiTextureType_REFLECTION:        return EAssimpTextureSlot::Reflection;
		default:                              return EAssimpTextureSlot::Unknown;
		}
	}

	/**
	 * Texture types we look for, in priority order within each slot.
	 *
	 * Order matters where several types map to the same slot: a glTF file populates BASE_COLOR while
	 * an OBJ populates DIFFUSE, and a file providing both should prefer the PBR channel. Because the
	 * extraction loop only writes a slot that is not already filled, listing the PBR type first is
	 * what expresses that preference.
	 */
	const aiTextureType InspectedTextureTypes[] = {
		aiTextureType_BASE_COLOR,
		aiTextureType_DIFFUSE,
		aiTextureType_NORMALS,
		aiTextureType_NORMAL_CAMERA,
		aiTextureType_METALNESS,
		aiTextureType_DIFFUSE_ROUGHNESS,
		aiTextureType_SPECULAR,
		aiTextureType_AMBIENT_OCCLUSION,
		aiTextureType_EMISSION_COLOR,
		aiTextureType_EMISSIVE,
		aiTextureType_OPACITY,
		aiTextureType_DISPLACEMENT,
		aiTextureType_HEIGHT,
		aiTextureType_SHININESS,
		aiTextureType_LIGHTMAP,
		aiTextureType_REFLECTION,
	};

	/** Maps Assimp's light type onto ours. */
	EAssimpLightType ConvertLightType(aiLightSourceType Type)
	{
		switch (Type)
		{
		case aiLightSource_DIRECTIONAL: return EAssimpLightType::Directional;
		case aiLightSource_POINT:       return EAssimpLightType::Point;
		case aiLightSource_SPOT:        return EAssimpLightType::Spot;
		case aiLightSource_AMBIENT:     return EAssimpLightType::Ambient;
		case aiLightSource_AREA:        return EAssimpLightType::Area;
		default:                        return EAssimpLightType::Unknown;
		}
	}
}

/**
 * Private state of FAssimpScene.
 *
 * Holds the Assimp::Importer, which owns the parsed aiScene: destroying the importer destroys the
 * scene, which is exactly the lifetime coupling we want and the reason the raw pointer is never
 * exposed.
 */
struct FAssimpScene::FImpl
{
	/** Owns the parsed scene. Must outlive every use of Scene. */
	TUniquePtr<Assimp::Importer> Importer;

	/** Borrowed from Importer; valid until Importer is destroyed or re-used. */
	const aiScene* Scene = nullptr;

	/** File system bridge, referenced by Importer for the duration of the load. */
	TUniquePtr<FAssimpFileManagerIO> FileSystem;

	/** Settings after sanitisation. */
	FAssimpImportSettings Settings;

	/** Coordinate conversion derived from Settings and the file's unit scale. */
	TUniquePtr<FAssimpAxisConverter> AxisConverter;

	/** Flattened description handed to callers. */
	FAssimpSceneInfo SceneInfo;

	/** Directory of the source file, used to resolve relative texture paths. */
	FString BaseDirectory;

	/** Builds SceneInfo from the parsed scene. */
	void BuildSceneInfo();

	void BuildMaterials();
	void BuildMeshes();
	void BuildNodeHierarchy();
	void BuildCamerasAndLights();

	/** Recursive helper for BuildNodeHierarchy. */
	int32 AddNodeRecursive(const aiNode* Node, int32 ParentIndex, int32 Depth);

	/**
	 * Depth cap for the node walk.
	 *
	 * A hostile or pathological file can present a hierarchy deep enough to exhaust the stack, and a
	 * stack overflow cannot be recovered from -- it kills the process with no catchable exception and
	 * often no crash log. Assimp's own corpus ships glTF2/RecursiveNodes precisely to provoke this.
	 * 256 is far beyond any legitimate scene graph while leaving the stack intact.
	 */
	static constexpr int32 MaxNodeDepth = 256;

	/** Set once the depth cap has been reported, so one file yields one warning rather than many. */
	bool bNodeDepthExceeded = false;
};

// =================================================================================================
// Construction and loading
// =================================================================================================

FAssimpScene::FAssimpScene()
	: Impl(MakePimpl<FImpl>())
{
}

FAssimpScene::~FAssimpScene() = default;

TSharedPtr<FAssimpScene> FAssimpScene::LoadInternal(
	const FLoadRequest& Request,
	const FAssimpImportSettings& RequestedSettings,
	FAssimpLoadResult& OutResult,
	const FAssimpProgressDelegate& Progress,
	bool bRelaxed)
{
	const FString& DebugName = Request.DebugName;
	const FString& BaseDirectory = Request.BaseDirectory;
	const FString& Extension = Request.Extension;

	OutResult = FAssimpLoadResult();

	if (!IAssimpCoreModule::IsAvailable() || !IAssimpCoreModule::Get().IsAssimpLibraryLoaded())
	{
		OutResult.ErrorMessage = TEXT("The Assimp shared library is not loaded; import is unavailable.");
		UE_LOG(LogAssimp, Error, TEXT("%s"), *OutResult.ErrorMessage);
		return nullptr;
	}

	const double StartTime = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		OutResult.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	};

	// Capture Assimp's log output for this thread into the result's diagnostics for the duration
	// of the load.
	FAssimpLogBridge::FScopedCapture LogCapture(OutResult.Diagnostics);

	TArray<FString> Adjustments;
	const FAssimpImportSettings Settings = RequestedSettings.GetSanitized(&Adjustments);
	for (const FString& Adjustment : Adjustments)
	{
		UE_LOG(LogAssimp, Log, TEXT("%s: %s"), *DebugName, *Adjustment);
		OutResult.Diagnostics.Add(
			FAssimpDiagnostic{ EAssimpDiagnosticSeverity::Info, Adjustment });
	}

	TSharedPtr<FAssimpScene> Result = MakeShareable(new FAssimpScene());
	FAssimpScene::FImpl& Impl = *Result->Impl;

	Impl.Settings = Settings;
	Impl.BaseDirectory = BaseDirectory;
	Impl.Importer = MakeUnique<Assimp::Importer>();
	Impl.FileSystem = MakeUnique<FAssimpFileManagerIO>(BaseDirectory);

	// Assimp takes ownership of neither: both must outlive the importer's use of them, which the
	// TUniquePtrs above guarantee since they are destroyed after Importer in declaration order.
	Impl.Importer->SetIOHandler(Impl.FileSystem.Get());

	FAssimpProgressBridge ProgressBridge(Progress);
	Impl.Importer->SetProgressHandler(&ProgressBridge);

	// Detach both before the importer is destroyed or the handlers go out of scope, otherwise
	// Assimp would try to delete objects it does not own.
	ON_SCOPE_EXIT
	{
		if (Impl.Importer.IsValid())
		{
			Impl.Importer->SetProgressHandler(nullptr);
			Impl.Importer->SetIOHandler(nullptr);
		}
	};

	FAssimpPostProcess::ApplyProperties(*Impl.Importer, Settings, bRelaxed);
	const unsigned int Flags = FAssimpPostProcess::BuildFlags(Settings, bRelaxed);

	const aiScene* Scene = nullptr;
	bool bHardwareFault = false;
	try
	{
		// The file and memory cases differ only here; everything before and after is shared.
		Scene = AssimpReadGuard::Read(
			*Impl.Importer,
			Request.FilePath.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*Request.FilePath),
			Request.Buffer,
			TCHAR_TO_UTF8(*Request.Extension),
			Flags,
			bHardwareFault);
	}
	catch (const std::exception& Exception)
	{
		// Assimp normally converts its internal DeadlyImportError into a null return plus an
		// error string, but a handful of importers let a std::exception escape (bad_alloc on a
		// corrupt size field, for instance). Containing it here is what keeps a malformed file
		// from taking the editor down, and is why this module is built with exceptions enabled.
		OutResult.ErrorMessage = FString::Printf(
			TEXT("Assimp threw while reading '%s': %s"),
			*DebugName, UTF8_TO_TCHAR(Exception.what()));
		UE_LOG(LogAssimp, Error, TEXT("%s"), *OutResult.ErrorMessage);
		return nullptr;
	}
	catch (...)
	{
		OutResult.ErrorMessage = FString::Printf(
			TEXT("Assimp threw an unrecognised exception while reading '%s'."), *DebugName);
		UE_LOG(LogAssimp, Error, TEXT("%s"), *OutResult.ErrorMessage);
		return nullptr;
	}

	if (bHardwareFault)
	{
		OutResult.ErrorMessage = FString::Printf(
			TEXT("Assimp faulted while reading '%s'; the file is malformed in a way its importer ")
			TEXT("does not handle. The import was abandoned."), *DebugName);
		UE_LOG(LogAssimp, Error, TEXT("%s"), *OutResult.ErrorMessage);
		return nullptr;
	}

	if (ProgressBridge.WasCancelled())
	{
		OutResult.bCancelled = true;
		OutResult.ErrorMessage = TEXT("Import cancelled.");
		UE_LOG(LogAssimp, Log, TEXT("Import of '%s' was cancelled."), *DebugName);
		return nullptr;
	}

	if (Scene == nullptr)
	{
		const FString AssimpError(UTF8_TO_TCHAR(Impl.Importer->GetErrorString()));
		OutResult.ErrorMessage = AssimpError.IsEmpty()
			? FString::Printf(TEXT("Assimp could not read '%s'."), *DebugName)
			: FString::Printf(TEXT("Assimp could not read '%s': %s"), *DebugName, *AssimpError);
		UE_LOG(LogAssimp, Error, TEXT("%s"), *OutResult.ErrorMessage);
		return nullptr;
	}

	if ((Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
	{
		// Worth importing, but the user should know the file was only partly understood.
		const FString Message = FString::Printf(
			TEXT("'%s' was read but is incomplete; some content may be missing."), *DebugName);
		UE_LOG(LogAssimp, Warning, TEXT("%s"), *Message);
		OutResult.Diagnostics.Add(
			FAssimpDiagnostic{ EAssimpDiagnosticSeverity::Warning, Message });
	}

	Impl.Scene = Scene;

	// Establish the unit scale before building the description, since every position and
	// transform in it depends on the conversion.
	float FileUnitScale = 1.0f;
	bool bUnitScaleFromMetadata = false;
	if (Scene->mMetaData != nullptr)
	{
		double MetadataScale = 0.0;
		float MetadataScaleFloat = 0.0f;
		if (Scene->mMetaData->Get("UnitScaleFactor", MetadataScale) && MetadataScale > 0.0)
		{
			FileUnitScale = static_cast<float>(MetadataScale);
			bUnitScaleFromMetadata = true;
		}
		else if (Scene->mMetaData->Get("UnitScaleFactor", MetadataScaleFloat) && MetadataScaleFloat > 0.0f)
		{
			FileUnitScale = MetadataScaleFloat;
			bUnitScaleFromMetadata = true;
		}
	}

	if (!bUnitScaleFromMetadata)
	{
		FileUnitScale = GetDefaultUnitScaleForExtension(Extension);
	}

	if (Settings.bApplyFileUnitScale)
	{
		UE_LOG(LogAssimp, Log,
			TEXT("'%s': unit scale %g (%s)."),
			*DebugName, FileUnitScale,
			bUnitScaleFromMetadata
				? TEXT("declared by the file")
				: TEXT("assumed from the file extension"));
	}

	Impl.AxisConverter = MakeUnique<FAssimpAxisConverter>(Settings, FileUnitScale);

	Impl.SceneInfo.SourceFilePath = DebugName;
	Impl.SceneInfo.FileUnitScale = FileUnitScale;
	Impl.SceneInfo.AppliedScale = Impl.AxisConverter->GetAppliedScale();

	Impl.BuildSceneInfo();

	OutResult.bSucceeded = true;

	UE_LOG(LogAssimp, Log,
		TEXT("Read '%s' in %.2fs: %d node(s), %d mesh(es), %d material(s), %d animation(s)."),
		*DebugName,
		FPlatformTime::Seconds() - StartTime,
		Impl.SceneInfo.Nodes.Num(),
		Impl.SceneInfo.Meshes.Num(),
		Impl.SceneInfo.Materials.Num(),
		Impl.SceneInfo.AnimationNames.Num());

	// Warn when the result is so small it will look like nothing imported.
	//
	// Most formats declare no unit at all, and for those the only defensible assumption is that one
	// file unit is one Unreal unit (a centimetre). Models authored in metres therefore arrive at
	// 1/100th size: a 3-unit character becomes 3 cm, which at any normal camera distance is
	// indistinguishable from a failed import. Rather than silently guess a scale -- which would
	// corrupt files that really are in centimetres -- say so, and name the setting that fixes it.
	{
		FBox SceneBounds(ForceInit);
		for (const FAssimpMeshInfo& Mesh : Impl.SceneInfo.Meshes)
		{
			if (Mesh.BoundingBox.IsValid)
			{
				SceneBounds += Mesh.BoundingBox;
			}
		}

		// 10 cm: below this, an object at a typical viewport distance is a few pixels at most.
		constexpr float SuspiciouslySmallExtent = 10.0f;

		if (SceneBounds.IsValid)
		{
			const float LargestExtent = SceneBounds.GetSize().GetMax();
			if (LargestExtent > 0.0f && LargestExtent < SuspiciouslySmallExtent)
			{
				const FString Message = FString::Printf(
					TEXT("'%s' imported %.2f Unreal units (%.1f cm) across, which will be hard to ")
					TEXT("see. The file declares no unit scale, so 1 file unit was taken as 1 cm. ")
					TEXT("If it was authored in metres, set UniformScale to 100."),
					*DebugName, LargestExtent, LargestExtent);

				UE_LOG(LogAssimp, Warning, TEXT("%s"), *Message);
				OutResult.Diagnostics.Add(
					FAssimpDiagnostic{ EAssimpDiagnosticSeverity::Warning, Message });
			}
		}
	}

	return Result;
}

TSharedPtr<FAssimpScene> FAssimpScene::LoadFromFile(
	const FString& FilePath,
	const FAssimpImportSettings& Settings,
	FAssimpLoadResult& OutResult,
	const FAssimpProgressDelegate& Progress)
{
	if (FilePath.IsEmpty())
	{
		OutResult = FAssimpLoadResult();
		OutResult.ErrorMessage = TEXT("No file path was supplied.");
		return nullptr;
	}

	if (!IFileManager::Get().FileExists(*FilePath))
	{
		OutResult = FAssimpLoadResult();
		OutResult.ErrorMessage = FString::Printf(TEXT("File does not exist: '%s'."), *FilePath);
		UE_LOG(LogAssimp, Error, TEXT("%s"), *OutResult.ErrorMessage);
		return nullptr;
	}

	const FString AbsolutePath = FPaths::ConvertRelativePathToFull(FilePath);
	const FString BaseDirectory = FPaths::GetPath(AbsolutePath);
	const FString Extension = FPaths::GetExtension(AbsolutePath);

	FLoadRequest Request;
	Request.DebugName = AbsolutePath;
	Request.BaseDirectory = BaseDirectory;
	Request.Extension = Extension;
	Request.FilePath = AbsolutePath;

	TSharedPtr<FAssimpScene> Loaded = LoadInternal(Request, Settings, OutResult, Progress);

	if (!Loaded.IsValid() && !OutResult.bCancelled)
	{
		ClassifyFailure(Request, Settings, OutResult);
	}

	return Loaded;
}

TSharedPtr<FAssimpScene> FAssimpScene::LoadFromMemory(
	TArrayView<const uint8> Buffer,
	const FString& FormatHint,
	const FString& DebugName,
	const FAssimpImportSettings& Settings,
	FAssimpLoadResult& OutResult,
	const FAssimpProgressDelegate& Progress)
{
	if (Buffer.IsEmpty())
	{
		OutResult = FAssimpLoadResult();
		OutResult.ErrorMessage = TEXT("The supplied buffer is empty.");
		return nullptr;
	}

	// Even when the primary data comes from memory, the file system bridge stays installed so that
	// sibling files the format references (an .obj naming its .mtl, a material naming a texture) can
	// still be resolved relative to where the data came from.
	const FString BaseDirectory = FPaths::GetPath(DebugName);

	FLoadRequest Request;
	Request.DebugName = DebugName;
	Request.BaseDirectory = BaseDirectory;
	Request.Extension = FormatHint;
	Request.Buffer = Buffer;

	TSharedPtr<FAssimpScene> Loaded = LoadInternal(Request, Settings, OutResult, Progress);

	if (!Loaded.IsValid() && !OutResult.bCancelled)
	{
		ClassifyFailure(Request, Settings, OutResult);
	}

	return Loaded;
}

void FAssimpScene::ClassifyFailure(
	const FLoadRequest& Request,
	const FAssimpImportSettings& Settings,
	FAssimpLoadResult& OutResult)
{
	// Keep the strict run's message; only refine it if the relaxed run reveals a better explanation.
	const FString StrictError = OutResult.ErrorMessage;

	FAssimpLoadResult RelaxedResult;
	const TSharedPtr<FAssimpScene> Relaxed = LoadInternal(
		Request, Settings, RelaxedResult, FAssimpProgressDelegate(), /*bRelaxed*/ true);

	if (!Relaxed.IsValid())
	{
		// It fails either way, so the file really is unreadable. Nothing to add.
		return;
	}

	const FAssimpSceneInfo& Info = Relaxed->GetSceneInfo();

	int32 TotalTriangles = 0;
	for (const FAssimpMeshInfo& Mesh : Info.Meshes)
	{
		TotalTriangles += Mesh.NumTriangles;
	}

	if (TotalTriangles > 0)
	{
		// Readable once the strictness is dropped, so the strict configuration is what rejected it.
		// Name the setting responsible instead of leaving Assimp's bare message.
		OutResult.ErrorMessage = FString::Printf(
			TEXT("%s (the file does parse with structural validation disabled -- try clearing ")
			TEXT("bValidateSceneStructure)"), *StrictError);
		return;
	}

	// Parsed, but there is not a single triangle in it. That is a point cloud, a line drawing, or a
	// file carrying only non-geometry content -- not a corrupt file, and it warrants saying so
	// plainly. The two cases read very differently, so distinguish them: "0 meshes of points or
	// lines only" would be nonsense.
	OutResult.bContainsNoTriangleGeometry = true;

	OutResult.ErrorMessage = (Info.Meshes.Num() > 0)
		? FString::Printf(
			TEXT("'%s' contains no triangle geometry: %d mesh(es) holding only points or lines. ")
			TEXT("There is nothing for a mesh importer to build."),
			*Request.DebugName, Info.Meshes.Num())
		: FString::Printf(
			TEXT("'%s' parsed but contains no meshes at all (it may hold only cameras, lights, ")
			TEXT("animation or metadata). There is nothing for a mesh importer to build."),
			*Request.DebugName);

	UE_LOG(LogAssimp, Warning, TEXT("%s"), *OutResult.ErrorMessage);
}

// =================================================================================================
// Accessors
// =================================================================================================

TArray<FString> FAssimpScene::GetSupportedExtensions()
{
	TArray<FString> Extensions;

	if (!IAssimpCoreModule::IsAvailable() || !IAssimpCoreModule::Get().IsAssimpLibraryLoaded())
	{
		return Extensions;
	}

	// Assimp reports its list as a single semicolon-separated string of glob patterns, e.g.
	// "*.ply;*.stl;*.dae". Normalise it to bare lower-case extensions.
	aiString ExtensionList;
	Assimp::Importer Importer;
	Importer.GetExtensionList(ExtensionList);

	FString Raw(UTF8_TO_TCHAR(ExtensionList.C_Str()));

	TArray<FString> Patterns;
	Raw.ParseIntoArray(Patterns, TEXT(";"), /*InCullEmpty*/ true);

	Extensions.Reserve(Patterns.Num());
	for (const FString& Pattern : Patterns)
	{
		FString Extension = Pattern.TrimStartAndEnd();
		Extension.RemoveFromStart(TEXT("*"));
		Extension.RemoveFromStart(TEXT("."));

		if (!Extension.IsEmpty())
		{
			Extensions.AddUnique(Extension.ToLower());
		}
	}

	Extensions.Sort();
	return Extensions;
}

bool FAssimpScene::IsExtensionSupported(const FString& Extension)
{
	FString Normalised = Extension.TrimStartAndEnd().ToLower();
	Normalised.RemoveFromStart(TEXT("*"));
	Normalised.RemoveFromStart(TEXT("."));

	if (Normalised.IsEmpty())
	{
		return false;
	}

	if (!IAssimpCoreModule::IsAvailable() || !IAssimpCoreModule::Get().IsAssimpLibraryLoaded())
	{
		return false;
	}

	// Ask Assimp directly rather than searching our own list: this is the same check Assimp performs
	// when choosing an importer, so the two cannot disagree.
	Assimp::Importer Importer;
	return Importer.IsExtensionSupported(TCHAR_TO_UTF8(*Normalised));
}

const FAssimpSceneInfo& FAssimpScene::GetSceneInfo() const
{
	return Impl->SceneInfo;
}

const FAssimpImportSettings& FAssimpScene::GetEffectiveSettings() const
{
	return Impl->Settings;
}

bool FAssimpScene::GetMeshDescription(int32 MeshIndex, FMeshDescription& OutMeshDescription) const
{
	const int32 Indices[1] = { MeshIndex };
	return GetMergedMeshDescription(Indices, OutMeshDescription);
}

bool FAssimpScene::GetMergedMeshDescription(
	TArrayView<const int32> MeshIndices,
	FMeshDescription& OutMeshDescription) const
{
	TArray<FString> UnusedJointNames;
	return GetMergedMeshDescription(MeshIndices, OutMeshDescription, UnusedJointNames);
}

bool FAssimpScene::GetMergedMeshDescription(
	TArrayView<const int32> MeshIndices,
	FMeshDescription& OutMeshDescription,
	TArray<FString>& OutJointNames) const
{
	if (Impl->Scene == nullptr || !Impl->AxisConverter.IsValid())
	{
		UE_LOG(LogAssimp, Error, TEXT("GetMergedMeshDescription called on an unloaded scene."));
		return false;
	}

	FString Error;
	const bool bSucceeded = FAssimpMeshConverter::Convert(
		*Impl->Scene,
		MeshIndices,
		*Impl->AxisConverter,
		Impl->Settings,
		OutMeshDescription,
		OutJointNames,
		Error);

	if (!bSucceeded)
	{
		UE_LOG(LogAssimp, Error, TEXT("Mesh conversion failed: %s"), *Error);
	}

	return bSucceeded;
}

bool FAssimpScene::GetEmbeddedTexture(int32 TextureIndex, FAssimpEmbeddedTexture& OutTexture) const
{
	if (Impl->Scene == nullptr || Impl->Scene->mTextures == nullptr)
	{
		return false;
	}

	if (TextureIndex < 0 || static_cast<unsigned int>(TextureIndex) >= Impl->Scene->mNumTextures)
	{
		return false;
	}

	const aiTexture* Texture = Impl->Scene->mTextures[TextureIndex];
	if (Texture == nullptr)
	{
		return false;
	}

	OutTexture = FAssimpEmbeddedTexture();
	OutTexture.Name = FAssimpAxisConverter::ConvertString(Texture->mFilename);

	// Assimp signals a compressed payload by setting mHeight to zero, in which case mWidth is the
	// byte count rather than a pixel dimension.
	if (Texture->mHeight == 0)
	{
		OutTexture.bIsCompressed = true;
		OutTexture.FormatHint = FString(ANSI_TO_TCHAR(Texture->achFormatHint)).ToLower();

		const int32 ByteCount = static_cast<int32>(Texture->mWidth);
		if (ByteCount <= 0)
		{
			return false;
		}

		OutTexture.CompressedData.Append(
			reinterpret_cast<const uint8*>(Texture->pcData), ByteCount);
		return true;
	}

	OutTexture.bIsCompressed = false;
	OutTexture.Width = static_cast<int32>(Texture->mWidth);
	OutTexture.Height = static_cast<int32>(Texture->mHeight);

	const int32 PixelCount = OutTexture.Width * OutTexture.Height;
	if (PixelCount <= 0)
	{
		return false;
	}

	OutTexture.RawPixels.Reserve(PixelCount);
	for (int32 Index = 0; Index < PixelCount; ++Index)
	{
		const aiTexel& Texel = Texture->pcData[Index];
		OutTexture.RawPixels.Emplace(Texel.r, Texel.g, Texel.b, Texel.a);
	}

	return true;
}

bool FAssimpScene::ResolveExternalTexturePath(const FString& RecordedPath, FString& OutAbsolutePath) const
{
	if (RecordedPath.IsEmpty())
	{
		return false;
	}

	FString Normalised = RecordedPath;
	Normalised.ReplaceInline(TEXT("\\"), TEXT("/"));

	const FString BaseDirectory = Impl->BaseDirectory;
	const FString CleanName = FPaths::GetCleanFilename(Normalised);

	// Ordered from most to least faithful to what the file actually said. The later entries exist
	// because source files routinely record absolute paths from the machine they were authored on,
	// where only the filename still means anything.
	TArray<FString> Candidates;
	Candidates.Add(Normalised);
	if (!BaseDirectory.IsEmpty())
	{
		Candidates.Add(FPaths::Combine(BaseDirectory, Normalised));
		Candidates.Add(FPaths::Combine(BaseDirectory, CleanName));

		// Conventional sibling directories for texture sets.
		static const TCHAR* const TextureSubdirectories[] = {
			TEXT("textures"), TEXT("Textures"), TEXT("tex"), TEXT("maps"), TEXT("Maps"), TEXT("images")
		};
		for (const TCHAR* Subdirectory : TextureSubdirectories)
		{
			Candidates.Add(FPaths::Combine(BaseDirectory, Subdirectory, CleanName));
		}

		// One level up, for layouts that place meshes and textures in sibling folders.
		Candidates.Add(FPaths::Combine(BaseDirectory, TEXT(".."), CleanName));
		Candidates.Add(FPaths::Combine(BaseDirectory, TEXT(".."), TEXT("textures"), CleanName));
	}

	for (const FString& Candidate : Candidates)
	{
		if (IFileManager::Get().FileExists(*Candidate))
		{
			OutAbsolutePath = FPaths::ConvertRelativePathToFull(Candidate);
			return true;
		}
	}

	UE_LOG(LogAssimp, Warning,
		TEXT("Could not resolve texture '%s' relative to '%s'."), *RecordedPath, *BaseDirectory);
	return false;
}

// =================================================================================================
// Scene description construction
// =================================================================================================

void FAssimpScene::FImpl::BuildSceneInfo()
{
	check(Scene != nullptr);
	check(AxisConverter.IsValid());

	FlattenMetadata(Scene->mMetaData, SceneInfo.Metadata);

	BuildMaterials();
	BuildMeshes();
	BuildNodeHierarchy();
	BuildCamerasAndLights();

	SceneInfo.NumEmbeddedTextures = static_cast<int32>(Scene->mNumTextures);

	SceneInfo.AnimationNames.Reserve(static_cast<int32>(Scene->mNumAnimations));
	for (unsigned int Index = 0; Index < Scene->mNumAnimations; ++Index)
	{
		const aiAnimation* Animation = Scene->mAnimations[Index];
		FString Name = (Animation != nullptr)
			? FAssimpAxisConverter::ConvertString(Animation->mName)
			: FString();
		if (Name.IsEmpty())
		{
			Name = FString::Printf(TEXT("Animation_%u"), Index);
		}
		SceneInfo.AnimationNames.Add(Name);
	}
}

void FAssimpScene::FImpl::BuildMaterials()
{
	if (!Settings.bImportMaterials || Scene->mMaterials == nullptr)
	{
		return;
	}

	SceneInfo.Materials.Reserve(static_cast<int32>(Scene->mNumMaterials));

	for (unsigned int Index = 0; Index < Scene->mNumMaterials; ++Index)
	{
		const aiMaterial* Material = Scene->mMaterials[Index];
		FAssimpMaterialInfo Info;

		if (Material == nullptr)
		{
			Info.Name = FString::Printf(TEXT("Material_%u"), Index);
			SceneInfo.Materials.Add(MoveTemp(Info));
			continue;
		}

		aiString Name;
		if (Material->Get(AI_MATKEY_NAME, Name) == AI_SUCCESS)
		{
			Info.Name = FAssimpAxisConverter::ConvertString(Name);
		}
		if (Info.Name.IsEmpty())
		{
			Info.Name = FString::Printf(TEXT("Material_%u"), Index);
		}

		// Prefer the PBR base colour, falling back to the legacy diffuse colour.
		aiColor4D Color;
		if (Material->Get(AI_MATKEY_BASE_COLOR, Color) == AI_SUCCESS)
		{
			Info.BaseColor = FAssimpAxisConverter::ConvertColor(Color);
		}
		else if (Material->Get(AI_MATKEY_COLOR_DIFFUSE, Color) == AI_SUCCESS)
		{
			Info.BaseColor = FAssimpAxisConverter::ConvertColor(Color);
		}

		if (Material->Get(AI_MATKEY_COLOR_EMISSIVE, Color) == AI_SUCCESS)
		{
			Info.EmissiveColor = FAssimpAxisConverter::ConvertColor(Color);
		}

		float Scalar = 0.0f;
		if (Material->Get(AI_MATKEY_METALLIC_FACTOR, Scalar) == AI_SUCCESS)
		{
			Info.Metallic = FMath::Clamp(Scalar, 0.0f, 1.0f);
		}
		if (Material->Get(AI_MATKEY_ROUGHNESS_FACTOR, Scalar) == AI_SUCCESS)
		{
			Info.Roughness = FMath::Clamp(Scalar, 0.0f, 1.0f);
		}
		if (Material->Get(AI_MATKEY_OPACITY, Scalar) == AI_SUCCESS)
		{
			Info.Opacity = FMath::Clamp(Scalar, 0.0f, 1.0f);
		}

		Info.bIsTranslucent = Info.Opacity < 1.0f - UE_KINDA_SMALL_NUMBER;

		int32 TwoSided = 0;
		if (Material->Get(AI_MATKEY_TWOSIDED, TwoSided) == AI_SUCCESS)
		{
			Info.bTwoSided = TwoSided != 0;
		}

		for (const aiTextureType Type : InspectedTextureTypes)
		{
			if (Material->GetTextureCount(Type) == 0)
			{
				continue;
			}

			const EAssimpTextureSlot Slot = TextureTypeToSlot(Type);

			// First writer wins, which is what makes InspectedTextureTypes' ordering express the
			// preference for PBR channels over their legacy equivalents.
			if (Info.Textures.Contains(Slot))
			{
				continue;
			}

			aiString TexturePath;
			unsigned int UVIndex = 0;
			if (Material->GetTexture(Type, 0, &TexturePath, nullptr, &UVIndex) != AI_SUCCESS)
			{
				continue;
			}

			FAssimpTextureReference Reference;
			Reference.UVChannel = static_cast<int32>(UVIndex);
			Reference.Path = FAssimpAxisConverter::ConvertString(TexturePath);

			// Assimp denotes an embedded texture with a "*<index>" path, and also offers a lookup
			// that handles the other conventions individual formats use.
			if (const aiTexture* Embedded = Scene->GetEmbeddedTexture(TexturePath.C_Str()))
			{
				Reference.Source = EAssimpTextureSource::Embedded;
				for (unsigned int TexIndex = 0; TexIndex < Scene->mNumTextures; ++TexIndex)
				{
					if (Scene->mTextures[TexIndex] == Embedded)
					{
						Reference.EmbeddedIndex = static_cast<int32>(TexIndex);
						break;
					}
				}
			}
			else
			{
				Reference.Source = EAssimpTextureSource::External;
			}

			Info.Textures.Add(Slot, MoveTemp(Reference));
		}

		SceneInfo.Materials.Add(MoveTemp(Info));
	}
}

void FAssimpScene::FImpl::BuildMeshes()
{
	if (Scene->mMeshes == nullptr)
	{
		return;
	}

	SceneInfo.Meshes.Reserve(static_cast<int32>(Scene->mNumMeshes));

	for (unsigned int Index = 0; Index < Scene->mNumMeshes; ++Index)
	{
		const aiMesh* Mesh = Scene->mMeshes[Index];
		FAssimpMeshInfo Info;

		if (Mesh == nullptr)
		{
			SceneInfo.Meshes.Add(MoveTemp(Info));
			continue;
		}

		Info.Name = FAssimpAxisConverter::ConvertString(Mesh->mName);
		if (Info.Name.IsEmpty())
		{
			Info.Name = FString::Printf(TEXT("Mesh_%u"), Index);
		}

		Info.MaterialIndex = (Scene->mNumMaterials > 0)
			? static_cast<int32>(Mesh->mMaterialIndex)
			: INDEX_NONE;
		Info.NumVertices = static_cast<int32>(Mesh->mNumVertices);
		Info.NumTriangles = static_cast<int32>(Mesh->mNumFaces);
		Info.bHasNormals = Mesh->HasNormals();
		Info.bHasTangents = Mesh->HasTangentsAndBitangents();
		Info.bHasVertexColors = Mesh->HasVertexColors(0);
		Info.bHasBones = Mesh->HasBones();

		for (int32 Channel = 0; Channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++Channel)
		{
			if (Mesh->HasTextureCoords(static_cast<unsigned int>(Channel)))
			{
				Info.NumUVChannels = Channel + 1;
			}
		}

		if (Info.bHasBones)
		{
			SceneInfo.bHasSkinnedMeshes = true;
			Info.BoneNames.Reserve(static_cast<int32>(Mesh->mNumBones));
			for (unsigned int BoneIndex = 0; BoneIndex < Mesh->mNumBones; ++BoneIndex)
			{
				const aiBone* Bone = Mesh->mBones[BoneIndex];
				if (Bone != nullptr)
				{
					Info.BoneNames.Add(FAssimpAxisConverter::ConvertString(Bone->mName));
				}
			}
		}

		Info.BoundingBox = FAssimpMeshConverter::ComputeBounds(*Mesh, *AxisConverter);

		SceneInfo.Meshes.Add(MoveTemp(Info));
	}
}

int32 FAssimpScene::FImpl::AddNodeRecursive(const aiNode* Node, int32 ParentIndex, int32 Depth)
{
	if (Depth >= MaxNodeDepth)
	{
		// Stop descending rather than risk the stack. Reported once per file.
		if (!bNodeDepthExceeded)
		{
			bNodeDepthExceeded = true;
			UE_LOG(LogAssimp, Warning,
				TEXT("Node hierarchy in '%s' exceeds %d levels; deeper nodes were skipped."),
				*SceneInfo.SourceFilePath, MaxNodeDepth);
		}
		return INDEX_NONE;
	}

	if (Node == nullptr)
	{
		return INDEX_NONE;
	}

	// Reserve this node's slot before recursing, so that a parent always precedes its children in
	// the flattened array. Consumers rely on that ordering: Interchange, for one, must create a
	// parent node before it can attach a child to it.
	const int32 ThisIndex = SceneInfo.Nodes.AddDefaulted();

	{
		FAssimpNodeInfo& Info = SceneInfo.Nodes[ThisIndex];
		Info.Name = FAssimpAxisConverter::ConvertString(Node->mName);
		if (Info.Name.IsEmpty())
		{
			Info.Name = FString::Printf(TEXT("Node_%d"), ThisIndex);
		}
		Info.ParentIndex = ParentIndex;
		Info.LocalTransform = AxisConverter->ConvertTransform(Node->mTransformation);

		Info.MeshIndices.Reserve(static_cast<int32>(Node->mNumMeshes));
		for (unsigned int Index = 0; Index < Node->mNumMeshes; ++Index)
		{
			Info.MeshIndices.Add(static_cast<int32>(Node->mMeshes[Index]));
		}

		if (Settings.bImportMetadata)
		{
			FlattenMetadata(Node->mMetaData, Info.Metadata);
		}
	}

	for (unsigned int Index = 0; Index < Node->mNumChildren; ++Index)
	{
		const int32 ChildIndex = AddNodeRecursive(Node->mChildren[Index], ThisIndex, Depth + 1);
		if (ChildIndex != INDEX_NONE)
		{
			// Re-index rather than holding a reference across the recursive call: the array may have
			// reallocated while the subtree was added.
			SceneInfo.Nodes[ThisIndex].ChildIndices.Add(ChildIndex);
		}
	}

	return ThisIndex;
}

void FAssimpScene::FImpl::BuildNodeHierarchy()
{
	if (Scene->mRootNode == nullptr)
	{
		return;
	}

	AddNodeRecursive(Scene->mRootNode, INDEX_NONE, 0);
}

void FAssimpScene::FImpl::BuildCamerasAndLights()
{
	if (Settings.bImportCameras && Scene->mCameras != nullptr)
	{
		SceneInfo.Cameras.Reserve(static_cast<int32>(Scene->mNumCameras));
		for (unsigned int Index = 0; Index < Scene->mNumCameras; ++Index)
		{
			const aiCamera* Camera = Scene->mCameras[Index];
			if (Camera == nullptr)
			{
				continue;
			}

			FAssimpCameraInfo Info;
			Info.Name = FAssimpAxisConverter::ConvertString(Camera->mName);

			// Assimp reports the half-angle in radians; Unreal wants the full angle in degrees.
			Info.HorizontalFieldOfViewDegrees =
				FMath::RadiansToDegrees(Camera->mHorizontalFOV) * 2.0f;

			// Clip planes are distances, so they scale with the scene.
			const float Scale = AxisConverter->GetAppliedScale();
			Info.NearClipPlane = Camera->mClipPlaneNear * Scale;
			Info.FarClipPlane = Camera->mClipPlaneFar * Scale;
			Info.AspectRatio = Camera->mAspect;

			SceneInfo.Cameras.Add(MoveTemp(Info));
		}
	}

	if (Settings.bImportLights && Scene->mLights != nullptr)
	{
		SceneInfo.Lights.Reserve(static_cast<int32>(Scene->mNumLights));
		for (unsigned int Index = 0; Index < Scene->mNumLights; ++Index)
		{
			const aiLight* Light = Scene->mLights[Index];
			if (Light == nullptr)
			{
				continue;
			}

			FAssimpLightInfo Info;
			Info.Name = FAssimpAxisConverter::ConvertString(Light->mName);
			Info.Type = ConvertLightType(Light->mType);
			Info.DiffuseColor = FAssimpAxisConverter::ConvertColor(Light->mColorDiffuse);
			Info.InnerConeAngleDegrees = FMath::RadiansToDegrees(Light->mAngleInnerCone);
			Info.OuterConeAngleDegrees = FMath::RadiansToDegrees(Light->mAngleOuterCone);
			Info.AttenuationConstant = Light->mAttenuationConstant;
			Info.AttenuationLinear = Light->mAttenuationLinear;
			Info.AttenuationQuadratic = Light->mAttenuationQuadratic;

			SceneInfo.Lights.Add(MoveTemp(Info));
		}
	}
}
