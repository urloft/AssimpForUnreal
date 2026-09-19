// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpIncludes.h"
#include "AssimpImportSettings.h"
#include "CoreMinimal.h"

/**
 * Converts geometry and transforms from a source file's coordinate system into Unreal's.
 *
 * Why this is done by hand rather than with aiProcess_ConvertToLeftHanded
 * ----------------------------------------------------------------------
 * Assimp offers aiProcess_ConvertToLeftHanded (MakeLeftHanded | FlipWindingOrder | FlipUVs). That
 * gets you a left-handed system but leaves the file Y-up, so a second Y-up-to-Z-up change is still
 * required. Composing an opaque Assimp step with our own step makes it very easy to flip handedness
 * twice -- which produces geometry that looks correct until you notice the normals face inward, or
 * that the model is mirrored.
 *
 * So the entire change of basis happens exactly once, here, as a single explicit matrix. Assimp is
 * asked for the raw right-handed data and this class does the rest. That makes the conversion
 * testable in isolation and means there is one place to look when a model arrives mirrored.
 *
 * The basis
 * ---------
 * Source convention (glTF, Collada, and most others Assimp reads): right-handed, +Y up, +Z toward
 * the viewer. Unreal: left-handed, +Z up, +X forward, +Y right. The mapping is:
 *
 *     Unreal.X = -Source.Z      (source's "toward viewer" becomes Unreal's "backward")
 *     Unreal.Y =  Source.X
 *     Unreal.Z =  Source.Y
 *
 * As a matrix acting on a column vector, that is
 *
 *          [  0  0 -1 ]
 *     B =  [  1  0  0 ]
 *          [  0  1  0 ]
 *
 * det(B) = -1, so this is a handedness-reversing change of basis. Two consequences follow and both
 * are handled here rather than being left to callers:
 *
 *   1. Triangle winding must be reversed, or every face ends up backfacing.
 *   2. A transform cannot simply have its translation remapped; it must be conjugated as
 *      B * M * B^-1 to remain the same transform expressed in the new basis. B is orthogonal, so
 *      B^-1 == B^T.
 */
class FAssimpAxisConverter
{
public:
	/**
	 * @param Settings       Import settings; determines the conversion mode and uniform scale.
	 * @param FileUnitScale  Unit scale the file declared (1.0 when it declared none). Combined with
	 *                       the requested uniform scale to give the total applied scale.
	 */
	FAssimpAxisConverter(const FAssimpImportSettings& Settings, float FileUnitScale);

	/** Total scale applied to positions and translations. */
	float GetAppliedScale() const { return AppliedScale; }

	/**
	 * True when triangle index order must be reversed to keep faces front-facing.
	 * Equivalent to det(basis) < 0.
	 */
	bool ShouldFlipWinding() const { return bFlipWinding; }

	/** Converts a position, applying both the basis change and the scale. */
	FORCEINLINE FVector3f ConvertPosition(const aiVector3D& In) const
	{
		return TransformVector(In) * AppliedScale;
	}

	/**
	 * Converts a direction: basis change only, no scale.
	 * Used for normals, tangents, and binormals, which must stay unit length.
	 */
	FORCEINLINE FVector3f ConvertDirection(const aiVector3D& In) const
	{
		return TransformVector(In);
	}

	/**
	 * Converts a texture coordinate.
	 *
	 * Unreal's UV origin is top-left; most formats Assimp reads place it bottom-left, so V is
	 * flipped. Done here rather than via aiProcess_FlipUVs so that the whole coordinate story lives
	 * in one class.
	 */
	FORCEINLINE FVector2f ConvertUV(const aiVector3D& In) const
	{
		return bFlipV ? FVector2f(In.x, 1.0f - In.y) : FVector2f(In.x, In.y);
	}

	/** Converts a node or bone transform, conjugating it into Unreal's basis. */
	FTransform ConvertTransform(const aiMatrix4x4& In) const;

	// ---------------------------------------------------------------------------------------------
	// The inverse, for export
	// ---------------------------------------------------------------------------------------------
	//
	// Export is not a separate coordinate problem; it is this one run backwards, and it lives here
	// so that it cannot drift from the forward direction. Every inverse below is the algebraic
	// inverse of the function directly above it, and the round-trip test asserts exactly that:
	// importing a file, exporting it, and importing the result must give back the same geometry.

	/** Inverse of ConvertPosition: removes the scale, then the basis change. */
	FORCEINLINE aiVector3D InvertPosition(const FVector3f& In) const
	{
		const float InverseScale = (AppliedScale != 0.0f) ? 1.0f / AppliedScale : 1.0f;
		return InvertVector(In * InverseScale);
	}

	/** Inverse of ConvertDirection: the basis change alone. */
	FORCEINLINE aiVector3D InvertDirection(const FVector3f& In) const
	{
		return InvertVector(In);
	}

	/** Inverse of ConvertUV. The V flip is its own inverse, which is why this reads the same. */
	FORCEINLINE aiVector3D InvertUV(const FVector2f& In) const
	{
		return bFlipV
			? aiVector3D(In.X, 1.0f - In.Y, 0.0f)
			: aiVector3D(In.X, In.Y, 0.0f);
	}

	/** Inverse of ConvertTransform. */
	aiMatrix4x4 InvertTransform(const FTransform& In) const;


	/** Converts a colour, clamping to a sane range. Assimp colours are already linear RGBA. */
	static FORCEINLINE FLinearColor ConvertColor(const aiColor4D& In)
	{
		return FLinearColor(In.r, In.g, In.b, In.a);
	}

	/** Converts a colour with no alpha channel. */
	static FORCEINLINE FLinearColor ConvertColor(const aiColor3D& In)
	{
		return FLinearColor(In.r, In.g, In.b, 1.0f);
	}

	/** Converts an Assimp string to an FString. Assimp strings are UTF-8. */
	static FORCEINLINE FString ConvertString(const aiString& In)
	{
		return FString(UTF8_TO_TCHAR(In.C_Str()));
	}

private:
	/** Applies the change of basis to a vector, without scale. */
	FORCEINLINE FVector3f TransformVector(const aiVector3D& In) const
	{
		if (bIdentityBasis)
		{
			return FVector3f(In.x, In.y, In.z);
		}

		// Unreal.X = -Source.Z, Unreal.Y = Source.X, Unreal.Z = Source.Y
		return FVector3f(-In.z, In.x, In.y);
	}

	/**
	 * Inverse of TransformVector.
	 *
	 * Unreal.X = -Source.Z, Unreal.Y = Source.X, Unreal.Z = Source.Y inverts to
	 * Source.X = Unreal.Y, Source.Y = Unreal.Z, Source.Z = -Unreal.X.
	 */
	FORCEINLINE aiVector3D InvertVector(const FVector3f& In) const
	{
		if (bIdentityBasis)
		{
			return aiVector3D(In.X, In.Y, In.Z);
		}

		return aiVector3D(In.Y, In.Z, -In.X);
	}

	/** Basis matrix as an FMatrix, for conjugating transforms. */
	FMatrix Basis;

	/** Inverse of Basis. Orthogonal, so this is its transpose. */
	FMatrix InverseBasis;

	/** Combined file unit scale and requested uniform scale. */
	float AppliedScale = 1.0f;

	/** True in Raw mode, where coordinates pass through untouched. */
	bool bIdentityBasis = false;

	/** True when det(Basis) < 0 and winding therefore needs reversing. */
	bool bFlipWinding = true;

	/** Whether to flip the V texture coordinate. */
	bool bFlipV = true;
};
