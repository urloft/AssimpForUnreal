// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpSceneObject.h"

#include "AssimpRuntime.h"
#include "AssimpScene.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
// FTexture2DMipMap, needed to write the transient texture's mip data.
#include "TextureResource.h"
#include "UDynamicMesh.h"
// GetTransientPackage() returns a UPackage*, which must be a complete type to convert to UObject*.
#include "UObject/Package.h"

namespace AssimpTexturePrivate
{
	/**
	 * Builds a transient texture from 8-bit BGRA pixels.
	 *
	 * @param bLinearColor  Sample as linear rather than sRGB.
	 */
	UTexture2D* CreateTextureFromBGRA(
		const void* Pixels,
		int64 NumBytes,
		int32 Width,
		int32 Height,
		bool bLinearColor)
	{
		if (Pixels == nullptr || Width <= 0 || Height <= 0)
		{
			return nullptr;
		}

		const int64 ExpectedBytes = static_cast<int64>(Width) * static_cast<int64>(Height) * 4;
		if (NumBytes < ExpectedBytes)
		{
			UE_LOG(LogAssimpRuntime, Warning,
				TEXT("Texture data is %lld bytes but %dx%d BGRA needs %lld; discarding."),
				NumBytes, Width, Height, ExpectedBytes);
			return nullptr;
		}

		UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		if (Texture == nullptr)
		{
			return nullptr;
		}

		// Normal, roughness, metallic and occlusion maps carry measurements. Sampling them as sRGB
		// applies a gamma curve to that data and visibly breaks lighting.
		Texture->SRGB = !bLinearColor;

		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		void* MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(MipData, Pixels, ExpectedBytes);
		Mip.BulkData.Unlock();

		Texture->UpdateResource();

		return Texture;
	}

	/** Decodes a compressed image (PNG, JPEG, ...) into a transient texture. */
	UTexture2D* CreateTextureFromCompressed(
		const uint8* Data,
		int64 NumBytes,
		bool bLinearColor,
		const FString& DebugName)
	{
		if (Data == nullptr || NumBytes <= 0)
		{
			return nullptr;
		}

		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

		const EImageFormat Format = ImageWrapperModule.DetectImageFormat(Data, NumBytes);
		if (Format == EImageFormat::Invalid)
		{
			UE_LOG(LogAssimpRuntime, Warning,
				TEXT("Could not identify the image format of '%s'."), *DebugName);
			return nullptr;
		}

		const TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(Format);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Data, NumBytes))
		{
			UE_LOG(LogAssimpRuntime, Warning, TEXT("Could not decode '%s'."), *DebugName);
			return nullptr;
		}

		TArray64<uint8> Raw;
		if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
		{
			UE_LOG(LogAssimpRuntime, Warning,
				TEXT("Could not convert '%s' to BGRA."), *DebugName);
			return nullptr;
		}

		return CreateTextureFromBGRA(
			Raw.GetData(), Raw.Num(), Wrapper->GetWidth(), Wrapper->GetHeight(), bLinearColor);
	}

	/**
	 * Whether a slot's contents are data rather than colour, and so must be sampled linearly.
	 *
	 * Getting this wrong is not subtle: a roughness or normal map read through the sRGB curve
	 * produces visibly wrong shading rather than a slight tint.
	 */
	bool IsLinearSlot(EAssimpTextureSlot Slot)
	{
		switch (Slot)
		{
		case EAssimpTextureSlot::BaseColor:
		case EAssimpTextureSlot::Emissive:
		case EAssimpTextureSlot::Specular:
		case EAssimpTextureSlot::Lightmap:
		case EAssimpTextureSlot::Reflection:
			return false;

		case EAssimpTextureSlot::Normal:
		case EAssimpTextureSlot::Metallic:
		case EAssimpTextureSlot::Roughness:
		case EAssimpTextureSlot::AmbientOcclusion:
		case EAssimpTextureSlot::Opacity:
		case EAssimpTextureSlot::Displacement:
		case EAssimpTextureSlot::Height:
		case EAssimpTextureSlot::Shininess:
		default:
			return true;
		}
	}
}

UAssimpSceneObject* UAssimpSceneObject::Create(UObject* Outer, TSharedPtr<FAssimpScene> Scene)
{
	if (!Scene.IsValid())
	{
		return nullptr;
	}

	UAssimpSceneObject* Handle = NewObject<UAssimpSceneObject>(
		Outer != nullptr ? Outer : GetTransientPackage());
	Handle->Scene = MoveTemp(Scene);

	return Handle;
}

bool UAssimpSceneObject::IsValidScene() const
{
	return Scene.IsValid();
}

const FAssimpSceneInfo& UAssimpSceneObject::GetSceneInfo() const
{
	if (Scene.IsValid())
	{
		return Scene->GetSceneInfo();
	}

	// A stable empty value, so Blueprint callers that ignore IsValidScene get an empty description
	// rather than a crash.
	static const FAssimpSceneInfo EmptyInfo;
	return EmptyInfo;
}

int32 UAssimpSceneObject::GetMeshCount() const
{
	return Scene.IsValid() ? Scene->GetSceneInfo().Meshes.Num() : 0;
}

bool UAssimpSceneObject::BuildDynamicMesh(int32 MeshIndex, UDynamicMesh* TargetMesh) const
{
	const TArray<int32> Indices = { MeshIndex };
	return BuildMergedDynamicMesh(Indices, TargetMesh);
}

bool UAssimpSceneObject::BuildMergedDynamicMesh(const TArray<int32>& MeshIndices, UDynamicMesh* TargetMesh) const
{
	if (!Scene.IsValid())
	{
		UE_LOG(LogAssimpRuntime, Warning, TEXT("BuildMergedDynamicMesh called on an invalid scene."));
		return false;
	}

	if (TargetMesh == nullptr)
	{
		UE_LOG(LogAssimpRuntime, Warning, TEXT("BuildMergedDynamicMesh called with a null target mesh."));
		return false;
	}

	if (MeshIndices.IsEmpty())
	{
		UE_LOG(LogAssimpRuntime, Warning, TEXT("BuildMergedDynamicMesh called with no mesh indices."));
		return false;
	}

	// The same conversion the editor import path uses. Sharing it is why a fix to tangent handedness
	// or UV flipping lands on both paths at once.
	FMeshDescription MeshDescription;
	if (!Scene->GetMergedMeshDescription(MeshIndices, MeshDescription))
	{
		return false;
	}

	FMeshDescriptionToDynamicMesh Converter;

	// Tangents are worth the copy: without them any normal-mapped material on the result is wrong,
	// and recomputing them here would discard what the source file supplied.
	UE::Geometry::FDynamicMesh3 DynamicMesh;
	Converter.Convert(&MeshDescription, DynamicMesh, /*bCopyTangents*/ true);

	TargetMesh->SetMesh(MoveTemp(DynamicMesh));

	return true;
}

UTexture2D* UAssimpSceneObject::CreateEmbeddedTexture(int32 TextureIndex, bool bIsNormalMap)
{
	if (!Scene.IsValid())
	{
		return nullptr;
	}

	if (const TObjectPtr<UTexture2D>* Cached = EmbeddedTextureCache.Find(TextureIndex))
	{
		if (*Cached != nullptr)
		{
			return *Cached;
		}
	}

	FAssimpEmbeddedTexture EmbeddedTexture;
	if (!Scene->GetEmbeddedTexture(TextureIndex, EmbeddedTexture))
	{
		UE_LOG(LogAssimpRuntime, Warning,
			TEXT("No embedded texture at index %d."), TextureIndex);
		return nullptr;
	}

	UTexture2D* Texture = nullptr;

	if (EmbeddedTexture.bIsCompressed)
	{
		// Assimp hands back the original PNG/JPEG bytes for most formats that embed textures, so
		// decoding is the common path rather than an edge case. ImageWrapper handles it at runtime
		// perfectly well; refusing to decode here would leave every glTF/GLB with embedded textures
		// silently untextured.
		Texture = AssimpTexturePrivate::CreateTextureFromCompressed(
			EmbeddedTexture.CompressedData.GetData(),
			EmbeddedTexture.CompressedData.Num(),
			bIsNormalMap,
			FString::Printf(TEXT("embedded texture %d ('%s')"), TextureIndex, *EmbeddedTexture.FormatHint));
	}
	else
	{
		// FColor is laid out BGRA in memory, matching PF_B8G8R8A8.
		Texture = AssimpTexturePrivate::CreateTextureFromBGRA(
			EmbeddedTexture.RawPixels.GetData(),
			static_cast<int64>(EmbeddedTexture.RawPixels.Num()) * sizeof(FColor),
			EmbeddedTexture.Width,
			EmbeddedTexture.Height,
			bIsNormalMap);
	}

	if (Texture == nullptr)
	{
		return nullptr;
	}

	if (bIsNormalMap)
	{
		Texture->CompressionSettings = TextureCompressionSettings::TC_Normalmap;
	}

	EmbeddedTextureCache.Add(TextureIndex, Texture);

	return Texture;
}

UTexture2D* UAssimpSceneObject::CreateTextureFromFile(
	const FString& AbsolutePath,
	bool bLinearColor,
	UObject* /*Outer*/)
{
	if (AbsolutePath.IsEmpty())
	{
		return nullptr;
	}

	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *AbsolutePath))
	{
		UE_LOG(LogAssimpRuntime, Warning, TEXT("Could not read texture file '%s'."), *AbsolutePath);
		return nullptr;
	}

	return AssimpTexturePrivate::CreateTextureFromCompressed(
		FileData.GetData(), FileData.Num(), bLinearColor, AbsolutePath);
}

UTexture2D* UAssimpSceneObject::CreateTextureForMaterialSlot(
	int32 MaterialIndex,
	EAssimpTextureSlot Slot)
{
	if (!Scene.IsValid())
	{
		return nullptr;
	}

	const FAssimpSceneInfo& Info = Scene->GetSceneInfo();
	if (!Info.Materials.IsValidIndex(MaterialIndex))
	{
		return nullptr;
	}

	const FAssimpTextureReference* Reference = Info.Materials[MaterialIndex].Textures.Find(Slot);
	if (Reference == nullptr)
	{
		return nullptr;
	}

	const bool bLinear = AssimpTexturePrivate::IsLinearSlot(Slot);

	// Resolve to a cache key first. Keyed by source rather than by material so that an image shared
	// across many materials is decoded once.
	FString CacheKey;
	FString ResolvedPath;

	if (Reference->Source == EAssimpTextureSource::Embedded)
	{
		CacheKey = FString::Printf(TEXT("embedded:%d"), Reference->EmbeddedIndex);
	}
	else
	{
		if (!Scene->ResolveExternalTexturePath(Reference->Path, ResolvedPath))
		{
			// ResolveExternalTexturePath already logged which locations were tried.
			return nullptr;
		}
		CacheKey = ResolvedPath.ToLower();
	}

	// Cache on the linear/sRGB choice too: the same file legitimately needs both interpretations
	// when it is used as, say, both a base colour and a mask.
	CacheKey += bLinear ? TEXT("|linear") : TEXT("|srgb");

	if (const TObjectPtr<UTexture2D>* Cached = SlotTextureCache.Find(CacheKey))
	{
		if (*Cached != nullptr)
		{
			return *Cached;
		}
	}

	UTexture2D* Texture = (Reference->Source == EAssimpTextureSource::Embedded)
		? CreateEmbeddedTexture(Reference->EmbeddedIndex, bLinear)
		: CreateTextureFromFile(ResolvedPath, bLinear, this);

	if (Texture == nullptr)
	{
		return nullptr;
	}

	if (Slot == EAssimpTextureSlot::Normal)
	{
		Texture->CompressionSettings = TextureCompressionSettings::TC_Normalmap;
	}

	SlotTextureCache.Add(CacheKey, Texture);

	return Texture;
}

bool UAssimpSceneObject::ResolveTexturePath(const FString& RecordedPath, FString& OutAbsolutePath) const
{
	return Scene.IsValid() && Scene->ResolveExternalTexturePath(RecordedPath, OutAbsolutePath);
}
