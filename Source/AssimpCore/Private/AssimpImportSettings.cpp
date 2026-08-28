// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpImportSettings.h"

#define LOCTEXT_NAMESPACE "AssimpImportSettings"

FAssimpImportSettings FAssimpImportSettings::GetSanitized(TArray<FString>* OutAdjustments) const
{
	FAssimpImportSettings Result = *this;

	const auto Note = [OutAdjustments](FString&& Message)
	{
		if (OutAdjustments != nullptr)
		{
			OutAdjustments->Add(MoveTemp(Message));
		}
	};

	// Animation and morph targets are properties of a skeletal mesh; without one there is nothing
	// for them to attach to.
	if (!Result.bImportSkeletalMesh)
	{
		if (Result.bImportAnimations)
		{
			Result.bImportAnimations = false;
			Note(TEXT("Animation import disabled because skeletal mesh import is disabled."));
		}
		if (Result.bImportMorphTargets)
		{
			Result.bImportMorphTargets = false;
			Note(TEXT("Morph target import disabled because skeletal mesh import is disabled."));
		}
	}

	const bool bNeedsSceneGraph = Result.bImportSkeletalMesh || Result.bImportAnimations;

	// Both of the following destroy the node hierarchy that bones and animation channels are
	// addressed through. Enforced here rather than merely documented, because the resulting failure
	// is otherwise silent: the import appears to succeed but arrives with no skinning.
	if (bNeedsSceneGraph && Result.bOptimizeSceneGraph)
	{
		Result.bOptimizeSceneGraph = false;
		Note(TEXT("Scene graph optimisation disabled: it removes the nodes that bones and animation ")
			 TEXT("channels are bound to by name."));
	}

	if (bNeedsSceneGraph && Result.bPreTransformVertices)
	{
		Result.bPreTransformVertices = false;
		Note(TEXT("Vertex pre-transformation disabled: it collapses the node hierarchy that skinning ")
			 TEXT("and animation require."));
	}

	// Clamp numeric ranges so a programmatic caller cannot bypass the UI metadata limits.
	Result.MaxBoneInfluencesPerVertex = FMath::Clamp(Result.MaxBoneInfluencesPerVertex, 1, 12);
	Result.NormalSmoothingAngle = FMath::Clamp(Result.NormalSmoothingAngle, 0.0f, 175.0f);

	if (Result.UniformScale <= 0.0f)
	{
		Note(FString::Printf(
			TEXT("Uniform scale %f is not positive; reset to 1.0."), Result.UniformScale));
		Result.UniformScale = 1.0f;
	}

	return Result;
}

#undef LOCTEXT_NAMESPACE
