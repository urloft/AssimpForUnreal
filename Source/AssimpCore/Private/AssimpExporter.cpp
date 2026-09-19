// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpExporter.h"

#include "AssimpAxisConverter.h"
#include "AssimpCore.h"
#include "AssimpImportSettings.h"
#include "AssimpIncludes.h"

#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "StaticMeshAttributes.h"

namespace
{
	/**
	 * Converter used for export.
	 *
	 * Constructed with the default settings and a unit scale of 1, which makes it the exact inverse
	 * of the conversion an import performs with those same settings. Anything else would need the
	 * scale the source file was imported with, and silently applying a different one is how a model
	 * comes back a hundred times too large.
	 */
	FAssimpAxisConverter MakeExportConverter()
	{
		return FAssimpAxisConverter(FAssimpImportSettings(), 1.0f);
	}

	/**
	 * An aiScene assembled on Unreal's heap, and torn down on Unreal's heap.
	 *
	 * This exists because of a genuine trap at the DLL boundary. aiScene, aiNode and aiMaterial are
	 * declared ASSIMP_API: their destructors are compiled into assimp.dll and run there. Unreal
	 * overrides global operator new and delete, so anything this module allocates lives on Unreal's
	 * heap -- and letting ~aiScene() free it means assimp.dll's CRT releasing a pointer it never
	 * allocated. That does not raise an error. The process simply dies, with no crash log, some time
	 * after the export has already reported success, which makes it look like the export corrupted
	 * something rather than that the cleanup did.
	 *
	 * So every array is freed here, deliberately, and then disowned: by the time Assimp's own
	 * destructors run, every pointer they would touch is null. aiMesh is the exception that proves
	 * the rule -- its destructor is inline, so it compiles into whichever module deletes it, and
	 * deleting it from here is correct.
	 */
	struct FExportScene
	{
		aiScene Scene;

		~FExportScene()
		{
			// Runs before the aiScene member's own destructor, which is exactly the point.
			if (Scene.mMeshes != nullptr)
			{
				for (unsigned int Index = 0; Index < Scene.mNumMeshes; ++Index)
				{
					// Inline destructor: frees its vertex arrays on this module's heap, as allocated.
					delete Scene.mMeshes[Index];
				}
				delete[] Scene.mMeshes;
				Scene.mMeshes = nullptr;
			}
			Scene.mNumMeshes = 0;

			if (Scene.mMaterials != nullptr)
			{
				// The material object is ours; the property blocks inside it were allocated by
				// AddProperty inside the DLL and are freed by the DLL's destructor. Each side frees
				// what it allocated, which is the arrangement that works.
				for (unsigned int Index = 0; Index < Scene.mNumMaterials; ++Index)
				{
					delete Scene.mMaterials[Index];
				}
				delete[] Scene.mMaterials;
				Scene.mMaterials = nullptr;
			}
			Scene.mNumMaterials = 0;

			DeleteNode(Scene.mRootNode);
			Scene.mRootNode = nullptr;
		}

	private:
		/** Frees a node's arrays here, then disowns them before the DLL's destructor sees them. */
		static void DeleteNode(aiNode* Node)
		{
			if (Node == nullptr)
			{
				return;
			}

			if (Node->mChildren != nullptr)
			{
				for (unsigned int Index = 0; Index < Node->mNumChildren; ++Index)
				{
					DeleteNode(Node->mChildren[Index]);
				}
				delete[] Node->mChildren;
				Node->mChildren = nullptr;
			}
			Node->mNumChildren = 0;

			delete[] Node->mMeshes;
			Node->mMeshes = nullptr;
			Node->mNumMeshes = 0;

			delete Node;
		}
	};

	/**
	 * Builds one aiMesh from an FMeshDescription.
	 *
	 * Written unwelded -- three vertices per triangle -- rather than reusing the mesh description's
	 * shared vertices. Assimp's exporters expect per-face-corner attributes, and a shared vertex
	 * carrying two different normals cannot be expressed without splitting it anyway. The importers
	 * on the way back in weld by default, so nothing is lost across the round trip.
	 *
	 * @return The mesh, or null when it holds no triangles. Ownership passes to the caller.
	 */
	aiMesh* BuildMesh(
		const FMeshDescription& MeshDescription,
		const FString& Name,
		const FAssimpAxisConverter& AxisConverter)
	{
		const int32 TriangleCount = MeshDescription.Triangles().Num();
		if (TriangleCount == 0)
		{
			return nullptr;
		}

		FStaticMeshConstAttributes Attributes(MeshDescription);
		TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();

		const bool bHasNormals = Normals.IsValid();
		const bool bHasUVs = UVs.IsValid() && UVs.GetNumChannels() > 0;

		const int32 VertexCount = TriangleCount * 3;

		aiMesh* Mesh = new aiMesh();
		Mesh->mName = aiString(TCHAR_TO_UTF8(*Name));
		Mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
		Mesh->mMaterialIndex = 0;
		Mesh->mNumVertices = static_cast<unsigned int>(VertexCount);
		Mesh->mVertices = new aiVector3D[VertexCount];
		Mesh->mNumFaces = static_cast<unsigned int>(TriangleCount);
		Mesh->mFaces = new aiFace[TriangleCount];

		if (bHasNormals)
		{
			Mesh->mNormals = new aiVector3D[VertexCount];
		}
		if (bHasUVs)
		{
			Mesh->mTextureCoords[0] = new aiVector3D[VertexCount];
			Mesh->mNumUVComponents[0] = 2;
		}

		// Reverse the winding back, because the import reversed it on the way in. Skipping this is
		// the classic export defect: the file loads and looks right in a viewer that ignores
		// backfaces, and is inside-out everywhere else.
		const bool bFlipWinding = AxisConverter.ShouldFlipWinding();

		int32 WriteIndex = 0;
		int32 FaceIndex = 0;

		for (const FTriangleID TriangleID : MeshDescription.Triangles().GetElementIDs())
		{
			TArrayView<const FVertexInstanceID> Corners =
				MeshDescription.GetTriangleVertexInstances(TriangleID);

			if (Corners.Num() != 3)
			{
				continue;
			}

			aiFace& Face = Mesh->mFaces[FaceIndex];
			Face.mNumIndices = 3;
			Face.mIndices = new unsigned int[3];

			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 SourceCorner = bFlipWinding ? (2 - Corner) : Corner;
				const FVertexInstanceID InstanceID = Corners[SourceCorner];
				const FVertexID VertexID = MeshDescription.GetVertexInstanceVertex(InstanceID);

				Mesh->mVertices[WriteIndex] = AxisConverter.InvertPosition(Positions[VertexID]);

				if (bHasNormals)
				{
					Mesh->mNormals[WriteIndex] = AxisConverter.InvertDirection(Normals[InstanceID]);
				}
				if (bHasUVs)
				{
					Mesh->mTextureCoords[0][WriteIndex] = AxisConverter.InvertUV(UVs.Get(InstanceID, 0));
				}

				Face.mIndices[Corner] = static_cast<unsigned int>(WriteIndex);
				++WriteIndex;
			}

			++FaceIndex;
		}

		// A triangle that was skipped leaves the trailing faces default-constructed, which Assimp
		// would treat as zero-index faces. Trim instead.
		Mesh->mNumFaces = static_cast<unsigned int>(FaceIndex);
		Mesh->mNumVertices = static_cast<unsigned int>(WriteIndex);

		return Mesh;
	}
}

TArray<FAssimpExportFormat> FAssimpExporter::GetSupportedFormats()
{
	TArray<FAssimpExportFormat> Formats;

	if (!IAssimpCoreModule::IsAvailable() || !IAssimpCoreModule::Get().IsAssimpLibraryLoaded())
	{
		return Formats;
	}

	Assimp::Exporter Exporter;
	const size_t Count = Exporter.GetExportFormatCount();

	Formats.Reserve(static_cast<int32>(Count));

	for (size_t Index = 0; Index < Count; ++Index)
	{
		const aiExportFormatDesc* Description = Exporter.GetExportFormatDescription(Index);
		if (Description == nullptr)
		{
			continue;
		}

		FAssimpExportFormat Format;
		Format.FormatId = FString(UTF8_TO_TCHAR(Description->id));
		Format.Description = FString(UTF8_TO_TCHAR(Description->description));
		Format.FileExtension = FString(UTF8_TO_TCHAR(Description->fileExtension)).ToLower();

		Formats.Add(MoveTemp(Format));
	}

	return Formats;
}

bool FAssimpExporter::IsFormatSupported(const FString& FormatId)
{
	if (FormatId.IsEmpty())
	{
		return false;
	}

	for (const FAssimpExportFormat& Format : GetSupportedFormats())
	{
		// Case-sensitive: Assimp's ids are lower-case tokens it matches exactly, and FString's
		// default comparison is not.
		if (Format.FormatId.Equals(FormatId, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}

	return false;
}

FString FAssimpExporter::FindFormatIdForExtension(const FString& Extension)
{
	FString Normalised = Extension.ToLower();
	Normalised.RemoveFromStart(TEXT("."));

	if (Normalised.IsEmpty())
	{
		return FString();
	}

	for (const FAssimpExportFormat& Format : GetSupportedFormats())
	{
		if (Format.FileExtension == Normalised)
		{
			return Format.FormatId;
		}
	}

	return FString();
}

bool FAssimpExporter::ExportMeshes(
	TArrayView<const FExportMesh> Meshes,
	const FString& FilePath,
	const FString& FormatId,
	FAssimpExportResult& OutResult)
{
	OutResult = FAssimpExportResult();

	const double StartTime = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		OutResult.ElapsedSeconds = static_cast<float>(FPlatformTime::Seconds() - StartTime);
	};

	if (!IAssimpCoreModule::IsAvailable() || !IAssimpCoreModule::Get().IsAssimpLibraryLoaded())
	{
		OutResult.ErrorMessage = TEXT("The Assimp shared library is not loaded; export is unavailable.");
		UE_LOG(LogAssimp, Error, TEXT("%s"), *OutResult.ErrorMessage);
		return false;
	}

	if (FilePath.IsEmpty())
	{
		OutResult.ErrorMessage = TEXT("No destination path was given.");
		return false;
	}

	FString ResolvedFormatId = FormatId;
	if (ResolvedFormatId.IsEmpty())
	{
		ResolvedFormatId = FindFormatIdForExtension(FPaths::GetExtension(FilePath));
	}

	if (ResolvedFormatId.IsEmpty())
	{
		OutResult.ErrorMessage = FString::Printf(
			TEXT("No export format was given and none matches the extension '%s'. ")
			TEXT("Ask FAssimpExporter::GetSupportedFormats what this build can write."),
			*FPaths::GetExtension(FilePath));
		UE_LOG(LogAssimp, Error, TEXT("%s"), *OutResult.ErrorMessage);
		return false;
	}

	if (!IsFormatSupported(ResolvedFormatId))
	{
		// Almost always means the vendored library was built with ASSIMP_NO_EXPORT, in which case it
		// reports no formats at all -- worth saying, because the alternative is a caller staring at
		// "unknown format" for a format Assimp documents as supported.
		const int32 FormatCount = GetSupportedFormats().Num();
		OutResult.ErrorMessage = FormatCount == 0
			? FString::Printf(
				TEXT("This Assimp build exports nothing at all; it was compiled with exporters ")
				TEXT("disabled. Rebuild it with Scripts/BuildAssimp.ps1."))
			: FString::Printf(
				TEXT("'%s' is not a format this Assimp build can write (%d available)."),
				*ResolvedFormatId, FormatCount);
		UE_LOG(LogAssimp, Error, TEXT("%s"), *OutResult.ErrorMessage);
		return false;
	}

	// --- Build the scene ------------------------------------------------------------------------
	const FAssimpAxisConverter AxisConverter = MakeExportConverter();

	TArray<aiMesh*> BuiltMeshes;
	for (const FExportMesh& Mesh : Meshes)
	{
		if (Mesh.MeshDescription == nullptr)
		{
			continue;
		}

		const FString MeshName = Mesh.Name.IsEmpty()
			? FString::Printf(TEXT("Mesh_%d"), BuiltMeshes.Num())
			: Mesh.Name;

		if (aiMesh* Built = BuildMesh(*Mesh.MeshDescription, MeshName, AxisConverter))
		{
			BuiltMeshes.Add(Built);
		}
	}

	if (BuiltMeshes.IsEmpty())
	{
		OutResult.ErrorMessage = TEXT("Nothing to export: no mesh held any triangles.");
		UE_LOG(LogAssimp, Warning, TEXT("%s"), *OutResult.ErrorMessage);
		return false;
	}

	// See FExportScene: Assimp's own destructors must not be the ones freeing any of this.
	FExportScene Holder;
	aiScene* const Scene = &Holder.Scene;

	Scene->mNumMeshes = static_cast<unsigned int>(BuiltMeshes.Num());
	Scene->mMeshes = new aiMesh*[BuiltMeshes.Num()];
	for (int32 Index = 0; Index < BuiltMeshes.Num(); ++Index)
	{
		Scene->mMeshes[Index] = BuiltMeshes[Index];
	}

	// At least one material, always. Most exporters dereference a mesh's material without checking,
	// so a scene with none crashes inside Assimp rather than failing politely.
	Scene->mNumMaterials = 1;
	Scene->mMaterials = new aiMaterial*[1];
	Scene->mMaterials[0] = new aiMaterial();
	{
		const aiString MaterialName("AssimpForUnrealExport");
		Scene->mMaterials[0]->AddProperty(&MaterialName, AI_MATKEY_NAME);
	}

	Scene->mRootNode = new aiNode();
	Scene->mRootNode->mName = aiString("Root");
	Scene->mRootNode->mNumMeshes = static_cast<unsigned int>(BuiltMeshes.Num());
	Scene->mRootNode->mMeshes = new unsigned int[BuiltMeshes.Num()];
	for (int32 Index = 0; Index < BuiltMeshes.Num(); ++Index)
	{
		Scene->mRootNode->mMeshes[Index] = static_cast<unsigned int>(Index);
	}

	// --- Write it -------------------------------------------------------------------------------
	const FString AbsolutePath = FPaths::ConvertRelativePathToFull(FilePath);

	// Assimp writes through the C runtime, which will not create intermediate directories.
	const FString Directory = FPaths::GetPath(AbsolutePath);
	if (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
	{
		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
	}

	Assimp::Exporter Exporter;
	const aiReturn ExportResult = Exporter.Export(
		Scene, TCHAR_TO_UTF8(*ResolvedFormatId), TCHAR_TO_UTF8(*AbsolutePath));

	if (ExportResult != AI_SUCCESS)
	{
		OutResult.ErrorMessage = FString(UTF8_TO_TCHAR(Exporter.GetErrorString()));
		if (OutResult.ErrorMessage.IsEmpty())
		{
			OutResult.ErrorMessage = FString::Printf(
				TEXT("Assimp refused to write '%s' as '%s' and gave no reason."),
				*AbsolutePath, *ResolvedFormatId);
		}
		UE_LOG(LogAssimp, Error, TEXT("Export failed: %s"), *OutResult.ErrorMessage);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.MeshesWritten = BuiltMeshes.Num();

	UE_LOG(LogAssimp, Log, TEXT("Exported %d mesh(es) to '%s' as '%s'."),
		OutResult.MeshesWritten, *AbsolutePath, *ResolvedFormatId);

	return true;
}
