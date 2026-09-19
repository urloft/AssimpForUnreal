// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpAnimationConverter.h"

#include "AssimpCore.h"

namespace
{
	/** Fallback timebase for files that declare none. Matches Assimp's own documented assumption. */
	constexpr double DefaultTicksPerSecond = 25.0;

	/** Frame rate used whenever the file's timebase cannot be read as one. */
	constexpr double DefaultSampleRate = 30.0;

	/**
	 * Range a declared timebase must fall in to be believed as a frame rate.
	 *
	 * Wide enough to cover every rate anyone animates at (film's 24 through a 480 Hz capture), narrow
	 * enough to exclude the two values that are certainly timebases rather than frame rates: 1
	 * (seconds) and 1000 (milliseconds).
	 */
	constexpr double MinPlausibleSampleRate = 8.0;
	constexpr double MaxPlausibleSampleRate = 480.0;

	/**
	 * Index of the last key at or before the given tick.
	 *
	 * Binary search rather than a linear scan because baking walks the key array once per frame: a
	 * 600-key channel sampled at 300 frames is 180,000 comparisons linearly, per bone.
	 */
	template <typename KeyType>
	int32 FindKeyIndexAtOrBefore(const KeyType* Keys, unsigned int NumKeys, double Ticks)
	{
		int32 Low = 0;
		int32 High = static_cast<int32>(NumKeys) - 1;
		int32 Result = 0;

		while (Low <= High)
		{
			const int32 Mid = Low + (High - Low) / 2;
			if (Keys[Mid].mTime <= Ticks)
			{
				Result = Mid;
				Low = Mid + 1;
			}
			else
			{
				High = Mid - 1;
			}
		}

		return Result;
	}

	/** Position of Ticks between two key times, as a 0-1 fraction. */
	double KeyBlendFactor(double LowerTime, double UpperTime, double Ticks)
	{
		const double Span = UpperTime - LowerTime;

		// Coincident keys happen in real files, and dividing by that span would produce a NaN that
		// propagates silently into every transform downstream.
		if (Span <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 0.0;
		}

		return FMath::Clamp((Ticks - LowerTime) / Span, 0.0, 1.0);
	}
}

double FAssimpAnimationConverter::GetTicksPerSecond(const aiAnimation& Animation)
{
	const double Declared = Animation.mTicksPerSecond;
	return (Declared > 0.0 && FMath::IsFinite(Declared)) ? Declared : DefaultTicksPerSecond;
}

double FAssimpAnimationConverter::GetDurationSeconds(const aiAnimation& Animation)
{
	// mDuration is -1 when unset, which would otherwise become a negative length.
	const double DurationTicks = FMath::Max(Animation.mDuration, 0.0);
	if (!FMath::IsFinite(DurationTicks))
	{
		return 0.0;
	}

	return DurationTicks / GetTicksPerSecond(Animation);
}

double FAssimpAnimationConverter::GetSampleRate(const aiAnimation& Animation)
{
	const double Declared = Animation.mTicksPerSecond;

	if (FMath::IsFinite(Declared)
		&& Declared >= MinPlausibleSampleRate
		&& Declared <= MaxPlausibleSampleRate)
	{
		return Declared;
	}

	return DefaultSampleRate;
}

const aiNodeAnim* FAssimpAnimationConverter::FindNodeChannel(
	const aiAnimation& Animation, const FString& NodeName)
{
	if (Animation.mChannels == nullptr)
	{
		return nullptr;
	}

	for (unsigned int Index = 0; Index < Animation.mNumChannels; ++Index)
	{
		const aiNodeAnim* Channel = Animation.mChannels[Index];
		if (Channel == nullptr)
		{
			continue;
		}

		if (FAssimpAxisConverter::ConvertString(Channel->mNodeName) == NodeName)
		{
			return Channel;
		}
	}

	return nullptr;
}

aiVector3D FAssimpAnimationConverter::SampleVectorKeys(
	const aiVectorKey* Keys, unsigned int NumKeys, double Ticks, const aiVector3D& Fallback)
{
	// An empty array is not an error: a channel may animate rotation alone, and the components it
	// leaves alone must keep the node's own value rather than collapsing to zero -- which, for
	// scale, would make the mesh vanish.
	if (Keys == nullptr || NumKeys == 0)
	{
		return Fallback;
	}

	if (NumKeys == 1)
	{
		return Keys[0].mValue;
	}

	const int32 LowerIndex = FindKeyIndexAtOrBefore(Keys, NumKeys, Ticks);
	const int32 UpperIndex = FMath::Min(LowerIndex + 1, static_cast<int32>(NumKeys) - 1);

	if (LowerIndex == UpperIndex)
	{
		return Keys[LowerIndex].mValue;
	}

	const float Alpha = static_cast<float>(
		KeyBlendFactor(Keys[LowerIndex].mTime, Keys[UpperIndex].mTime, Ticks));

	return Keys[LowerIndex].mValue + (Keys[UpperIndex].mValue - Keys[LowerIndex].mValue) * Alpha;
}

aiQuaternion FAssimpAnimationConverter::SampleQuaternionKeys(
	const aiQuatKey* Keys, unsigned int NumKeys, double Ticks, const aiQuaternion& Fallback)
{
	if (Keys == nullptr || NumKeys == 0)
	{
		return Fallback;
	}

	if (NumKeys == 1)
	{
		return Keys[0].mValue;
	}

	const int32 LowerIndex = FindKeyIndexAtOrBefore(Keys, NumKeys, Ticks);
	const int32 UpperIndex = FMath::Min(LowerIndex + 1, static_cast<int32>(NumKeys) - 1);

	if (LowerIndex == UpperIndex)
	{
		return Keys[LowerIndex].mValue;
	}

	const float Alpha = static_cast<float>(
		KeyBlendFactor(Keys[LowerIndex].mTime, Keys[UpperIndex].mTime, Ticks));

	// Assimp's Interpolate is a shortest-arc slerp: it negates the far quaternion when the pair sits
	// on opposite hemispheres, which is what stops a bone spinning the long way round between two
	// keys that are geometrically adjacent.
	aiQuaternion Result;
	aiQuaternion::Interpolate(Result, Keys[LowerIndex].mValue, Keys[UpperIndex].mValue, Alpha);
	Result.Normalize();

	return Result;
}

bool FAssimpAnimationConverter::SampleNodeTrack(
	const aiScene& Scene,
	const aiAnimation& Animation,
	const FString& NodeName,
	const FAssimpAxisConverter& AxisConverter,
	double SampleRateHz,
	double RangeStartSeconds,
	double RangeEndSeconds,
	TArray<FTransform>& OutKeys)
{
	OutKeys.Reset();

	const aiNodeAnim* Channel = FindNodeChannel(Animation, NodeName);
	if (Channel == nullptr)
	{
		return false;
	}

	if (!FMath::IsFinite(SampleRateHz) || SampleRateHz <= 0.0)
	{
		UE_LOG(LogAssimp, Warning,
			TEXT("Animation '%s': refusing to bake node '%s' at a sample rate of %f."),
			*FAssimpAxisConverter::ConvertString(Animation.mName), *NodeName, SampleRateHz);
		return false;
	}

	// The node's own transform supplies whichever of the three components the channel leaves empty.
	aiVector3D BindScale(1.0f, 1.0f, 1.0f);
	aiQuaternion BindRotation;
	aiVector3D BindPosition(0.0f, 0.0f, 0.0f);

	if (Scene.mRootNode != nullptr)
	{
		if (const aiNode* Node = Scene.mRootNode->FindNode(Channel->mNodeName))
		{
			Node->mTransformation.Decompose(BindScale, BindRotation, BindPosition);
		}
	}

	const double TicksPerSecond = GetTicksPerSecond(Animation);
	const double DurationTicks = FMath::Max(Animation.mDuration, 0.0);

	const double RangeSeconds = FMath::Max(RangeEndSeconds - RangeStartSeconds, 0.0);

	// Fence-post count: sampling a one-second range at 30 fps yields 31 transforms, the last of them
	// exactly at the end of the range. Interchange's factory assumes this, and producing 30 instead
	// shortens every clip by one frame.
	const int64 RequestedKeys = FMath::RoundToInt64(RangeSeconds * SampleRateHz) + 1;
	const int32 KeyCount = static_cast<int32>(FMath::Clamp<int64>(RequestedKeys, 1, MaxBakedKeys));

	if (RequestedKeys > MaxBakedKeys)
	{
		UE_LOG(LogAssimp, Warning,
			TEXT("Animation '%s': node '%s' would need %lld baked keys at %.3f Hz; truncated to %d."),
			*FAssimpAxisConverter::ConvertString(Animation.mName), *NodeName,
			RequestedKeys, SampleRateHz, MaxBakedKeys);
	}

	OutKeys.Reserve(KeyCount);

	const double SampleInterval = 1.0 / SampleRateHz;

	for (int32 KeyIndex = 0; KeyIndex < KeyCount; ++KeyIndex)
	{
		const double Seconds = RangeStartSeconds + KeyIndex * SampleInterval;

		// Clamping past the end holds the final pose rather than extrapolating, which is what every
		// other consumer of these formats does and what the key arrays themselves imply.
		const double Ticks = FMath::Clamp(Seconds * TicksPerSecond, 0.0, DurationTicks);

		const aiVector3D Position =
			SampleVectorKeys(Channel->mPositionKeys, Channel->mNumPositionKeys, Ticks, BindPosition);
		const aiQuaternion Rotation =
			SampleQuaternionKeys(Channel->mRotationKeys, Channel->mNumRotationKeys, Ticks, BindRotation);
		const aiVector3D ScaleValue =
			SampleVectorKeys(Channel->mScalingKeys, Channel->mNumScalingKeys, Ticks, BindScale);

		// Recomposing into a matrix and handing it to the same converter the reference pose used is
		// deliberate. The change of basis is a conjugation, B * M * B^-1, which does not distribute
		// over a decomposed translation/rotation/scale; converting the three components separately
		// would give a different -- wrong -- rotation axis. One conversion, in one place.
		const aiMatrix4x4 Local(ScaleValue, Rotation, Position);
		OutKeys.Add(AxisConverter.ConvertTransform(Local));
	}

	return true;
}
