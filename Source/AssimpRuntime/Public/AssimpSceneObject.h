// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpSceneTypes.h"
#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "AssimpSceneObject.generated.h"

class FAssimpScene;
class UDynamicMesh;
class UDynamicMeshComponent;
class UProceduralMeshComponent;
class UTexture2D;

/**
 * Blueprint-facing handle to a scene parsed at runtime.
 *
 * Lifetime
 * --------
 * This is a handle, not the scene. It holds a TSharedPtr to the parsed FAssimpScene; the geometry
 * itself is never a UObject field and no raw Assimp pointer is stored or exposed.
 *
 * That distinction is the whole point. A UObject's destruction is scheduled by the garbage
 * collector, which is the wrong lifetime model for a large non-UObject allocation owned by a
 * third-party library: the memory outlives its last use by an unpredictable margin, and any raw
 * pointer into it becomes a use-after-free the moment the owner is collected first. Keeping the
 * scene behind a shared pointer means it is released deterministically when the last reference goes,
 * and there is no hand-written BeginDestroy to get wrong.
 */
UCLASS(BlueprintType)
class ASSIMPRUNTIME_API UAssimpSceneObject : public UObject
{
	GENERATED_BODY()

public:
	/** Wraps an already-parsed scene. Returns null if Scene is invalid. */
	static UAssimpSceneObject* Create(UObject* Outer, TSharedPtr<FAssimpScene> Scene);

	/** True while this handle refers to a parsed scene. */
	UFUNCTION(BlueprintPure, Category = "Assimp|Scene")
	bool IsValidScene() const;

	/** Description of everything the file contained. Cheap: carries no vertex data. */
	UFUNCTION(BlueprintPure, Category = "Assimp|Scene")
	const FAssimpSceneInfo& GetSceneInfo() const;

	/** Number of meshes available to build. */
	UFUNCTION(BlueprintPure, Category = "Assimp|Scene")
	int32 GetMeshCount() const;

	/**
	 * Builds one mesh into a UDynamicMesh.
	 *
	 * @param MeshIndex   Index into GetSceneInfo().Meshes.
	 * @param TargetMesh  Mesh to write into. Its existing contents are replaced.
	 * @return            True on success.
	 */
	UFUNCTION(BlueprintCallable, Category = "Assimp|Scene", meta = (DisplayName = "Build Dynamic Mesh"))
	bool BuildDynamicMesh(int32 MeshIndex, UDynamicMesh* TargetMesh) const;

	/**
	 * Builds several meshes into one UDynamicMesh, preserving a material slot per source mesh.
	 *
	 * Assimp splits geometry by material, so a model that looks like one object is usually several
	 * meshes. Use this to reassemble them into a single component.
	 */
	UFUNCTION(BlueprintCallable, Category = "Assimp|Scene", meta = (DisplayName = "Build Merged Dynamic Mesh"))
	bool BuildMergedDynamicMesh(const TArray<int32>& MeshIndices, UDynamicMesh* TargetMesh) const;

	/**
	 * Creates a transient UTexture2D from a texture embedded in the source file.
	 *
	 * @param TextureIndex  Index in [0, FAssimpSceneInfo::NumEmbeddedTextures).
	 * @param bIsNormalMap  Import as linear rather than sRGB. Required for normal, roughness,
	 *                      metallic and occlusion maps, whose values are data rather than colour.
	 * @return              The texture, or null on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Assimp|Scene")
	UTexture2D* CreateEmbeddedTexture(int32 TextureIndex, bool bIsNormalMap = false);

	/** Resolves a material's texture path against the source file's location. */
	UFUNCTION(BlueprintCallable, Category = "Assimp|Scene")
	bool ResolveTexturePath(const FString& RecordedPath, FString& OutAbsolutePath) const;

	/**
	 * Creates a transient UTexture2D for one slot of one material, wherever the texture lives.
	 *
	 * This is the entry point worth using: it hides whether the texture is embedded in the source
	 * file or sitting in a sibling directory, resolves the recorded path (which is routinely
	 * absolute-from-the-authoring-machine, or uses the other platform's separators), decodes the
	 * image, and picks sRGB versus linear from the slot's meaning.
	 *
	 * Results are cached per material and slot, so a texture shared by many materials is decoded
	 * once.
	 *
	 * @param MaterialIndex  Index into FAssimpSceneInfo::Materials.
	 * @param Slot           Which map to fetch.
	 * @return               The texture, or null when the material has no such slot or it cannot be
	 *                       loaded.
	 */
	UFUNCTION(BlueprintCallable, Category = "Assimp|Scene")
	UTexture2D* CreateTextureForMaterialSlot(int32 MaterialIndex, EAssimpTextureSlot Slot);

	/**
	 * Creates a transient UTexture2D from an image file on disk.
	 *
	 * Decodes with the ImageWrapper module, so any format Unreal can read (PNG, JPEG, BMP, TGA,
	 * EXR, ...) works.
	 *
	 * @param AbsolutePath  File to read.
	 * @param bLinearColor  Sample as linear rather than sRGB. Required for normal, roughness,
	 *                      metallic and occlusion maps, whose values are data rather than colour.
	 * @param Outer         Owner for the created texture.
	 */
	static UTexture2D* CreateTextureFromFile(
		const FString& AbsolutePath,
		bool bLinearColor,
		UObject* Outer);

	/** Non-Blueprint access to the underlying scene, for C++ callers. */
	TSharedPtr<FAssimpScene> GetScene() const { return Scene; }

private:
	/**
	 * The parsed scene. Deliberately a TSharedPtr rather than raw storage, and deliberately not
	 * exposed to Blueprint.
	 */
	TSharedPtr<FAssimpScene> Scene;

	/** Cache of textures already created, so repeated requests do not re-decode. */
	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<UTexture2D>> EmbeddedTextureCache;

	/**
	 * Cache of textures created for material slots, keyed by resolved source.
	 *
	 * Keyed by the resolved absolute path (or "embedded:<index>") rather than by material and slot,
	 * because a model with dozens of materials commonly shares a handful of texture files between
	 * them -- this MiG-23 has 44 materials over 24 images. Keying by source decodes each image once.
	 */
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> SlotTextureCache;
};
