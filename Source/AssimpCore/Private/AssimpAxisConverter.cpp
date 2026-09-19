// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpAxisConverter.h"

FAssimpAxisConverter::FAssimpAxisConverter(const FAssimpImportSettings& Settings, float FileUnitScale)
{
	const float UnitScale = Settings.bApplyFileUnitScale ? FileUnitScale : 1.0f;
	AppliedScale = UnitScale * Settings.UniformScale;

	if (!FMath::IsFinite(AppliedScale) || FMath::IsNearlyZero(AppliedScale))
	{
		AppliedScale = 1.0f;
	}

	switch (Settings.CoordinateSystemMode)
	{
	case EAssimpCoordinateSystemMode::Raw:
		// Pass coordinates through untouched. No basis change means no winding flip either.
		bIdentityBasis = true;
		bFlipWinding = false;
		bFlipV = false;
		Basis = FMatrix::Identity;
		InverseBasis = FMatrix::Identity;
		return;

	case EAssimpCoordinateSystemMode::Automatic:
	case EAssimpCoordinateSystemMode::ForceYUpRightHanded:
	default:
		break;
	}

	bIdentityBasis = false;
	bFlipV = true;

	// Unreal.X = -Source.Z, Unreal.Y = Source.X, Unreal.Z = Source.Y
	//
	//      [  0  0 -1 ]
	//  B = [  1  0  0 ]
	//      [  0  1  0 ]
	//
	// Stored with the same index layout as the mathematical matrix, so that the conjugation in
	// ConvertTransform is a literal matrix product.
	Basis = FMatrix::Identity;
	Basis.M[0][0] =  0.0f; Basis.M[0][1] =  0.0f; Basis.M[0][2] = -1.0f;
	Basis.M[1][0] =  1.0f; Basis.M[1][1] =  0.0f; Basis.M[1][2] =  0.0f;
	Basis.M[2][0] =  0.0f; Basis.M[2][1] =  1.0f; Basis.M[2][2] =  0.0f;

	// B is orthogonal, so its inverse is its transpose. Computing it explicitly rather than calling
	// Inverse() keeps this exact and avoids any near-singular fallback path.
	InverseBasis = Basis.GetTransposed();

	// det(B) = -1: handedness is reversed, so triangle winding must be reversed to match.
	const float Determinant = Basis.Determinant();
	bFlipWinding = Determinant < 0.0f;

	checkf(!FMath::IsNearlyZero(Determinant),
		TEXT("AssimpForUnreal: coordinate basis is singular; conversion would be lossy."));
}

FTransform FAssimpAxisConverter::ConvertTransform(const aiMatrix4x4& In) const
{
	// Assimp stores a row-major matrix that acts on column vectors, with translation in the fourth
	// column (a4, b4, c4). Unreal's FMatrix acts on row vectors, with translation in row 3. The two
	// conventions are transposes of one another, so transpose while copying.
	FMatrix Source;
	Source.M[0][0] = In.a1; Source.M[0][1] = In.b1; Source.M[0][2] = In.c1; Source.M[0][3] = In.d1;
	Source.M[1][0] = In.a2; Source.M[1][1] = In.b2; Source.M[1][2] = In.c2; Source.M[1][3] = In.d2;
	Source.M[2][0] = In.a3; Source.M[2][1] = In.b3; Source.M[2][2] = In.c3; Source.M[2][3] = In.d3;
	Source.M[3][0] = In.a4; Source.M[3][1] = In.b4; Source.M[3][2] = In.c4; Source.M[3][3] = In.d4;

	FMatrix Result;
	if (bIdentityBasis)
	{
		Result = Source;
	}
	else
	{
		// Re-express the transform in Unreal's basis: B * M * B^-1.
		//
		// Remapping only the translation would be wrong -- the rotation and scale components are
		// expressed in the source basis too, so a transform that rotates about the source's up axis
		// has to become one that rotates about Unreal's up axis. Conjugation is what achieves that.
		Result = Basis * Source * InverseBasis;
	}

	// Scale acts on translation only; the rotation and scale blocks are unaffected by unit choice.
	Result.M[3][0] *= AppliedScale;
	Result.M[3][1] *= AppliedScale;
	Result.M[3][2] *= AppliedScale;

	return FTransform(Result);
}

aiMatrix4x4 FAssimpAxisConverter::InvertTransform(const FTransform& In) const
{
	FMatrix Result = In.ToMatrixWithScale();

	// Undo the scale that ConvertTransform applied to the translation row.
	const float InverseScale = (AppliedScale != 0.0f) ? 1.0f / AppliedScale : 1.0f;
	Result.M[3][0] *= InverseScale;
	Result.M[3][1] *= InverseScale;
	Result.M[3][2] *= InverseScale;

	if (!bIdentityBasis)
	{
		// ConvertTransform computes B * M * B^-1, so the inverse is B^-1 * M * B.
		Result = InverseBasis * Result * Basis;
	}

	// And transpose back out of Unreal's row-vector convention into Assimp's column-vector one.
	aiMatrix4x4 Out;
	Out.a1 = Result.M[0][0]; Out.b1 = Result.M[0][1]; Out.c1 = Result.M[0][2]; Out.d1 = Result.M[0][3];
	Out.a2 = Result.M[1][0]; Out.b2 = Result.M[1][1]; Out.c2 = Result.M[1][2]; Out.d2 = Result.M[1][3];
	Out.a3 = Result.M[2][0]; Out.b3 = Result.M[2][1]; Out.c3 = Result.M[2][2]; Out.d3 = Result.M[2][3];
	Out.a4 = Result.M[3][0]; Out.b4 = Result.M[3][1]; Out.c4 = Result.M[3][2]; Out.d4 = Result.M[3][3];

	return Out;
}
