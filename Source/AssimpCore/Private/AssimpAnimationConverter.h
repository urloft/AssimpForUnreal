// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpAxisConverter.h"
#include "AssimpIncludes.h"
#include "CoreMinimal.h"

/**
 * Samples Assimp's animation channels into the baked local transforms Unreal wants.
 *
 * Why baking rather than curve transfer
 * -------------------------------------
 * An aiNodeAnim is three independent key arrays -- position, rotation and scale -- each with its own
 * times and its own count, and Assimp gives no interpolation mode: every importer has already
 * resampled its source into keys that are meant to be read linearly (spherically, for rotation).
 * Unreal's animation data, in contrast, is one transform per bone per frame. Evaluating the three
 * arrays at a common set of frame times is therefore not a lossy shortcut; it is the only honest
 * translation, and it is exactly what Interchange asks a translator for with its BAKED payload type.
 *
 * Timebases
 * ---------
 * Two different quantities are easily confused and mixing them up silently produces animation that
 * plays at the wrong speed:
 *
 *   - Ticks per second is the file's *timebase*, the unit its key times are expressed in. glTF uses
 *     1000 (milliseconds), Collada uses 1 (seconds), FBX uses whatever its time mode says. It is
 *     needed to turn a key time into seconds and for nothing else.
 *   - The sample rate is the frame rate the clip is baked at. It is our choice, not the file's.
 *
 * Reading the first as the second is the classic defect here: it would bake a glTF clip at 1000 fps
 * and a Collada clip at 1 fps.
 */
class FAssimpAnimationConverter
{
public:
	/**
	 * Timebase to interpret the clip's key times in, never zero.
	 *
	 * Assimp leaves mTicksPerSecond at 0 when the format states no timebase. Falling back to 25
	 * matches Assimp's own sample code and its documentation, so a file that says nothing plays at
	 * the same speed here as it does in every other Assimp consumer.
	 */
	static double GetTicksPerSecond(const aiAnimation& Animation);

	/** Length of the clip in seconds. Zero for a clip with no declared duration. */
	static double GetDurationSeconds(const aiAnimation& Animation);

	/**
	 * Frame rate to bake the clip at.
	 *
	 * Uses the file's declared timebase only when it is plausibly a frame rate; otherwise 30. The
	 * range test is what stops a glTF millisecond timebase (1000) or a Collada second timebase (1)
	 * from being mistaken for one, which would bake either an unusable number of frames or a clip
	 * with no motion left in it. Formats that genuinely state a frame rate -- BVH, MD5 -- fall
	 * inside the range and are honoured.
	 */
	static double GetSampleRate(const aiAnimation& Animation);

	/** The channel animating a given node, or null when the clip does not touch it. */
	static const aiNodeAnim* FindNodeChannel(const aiAnimation& Animation, const FString& NodeName);

	/**
	 * Samples one node's local transform at a fixed rate across a time range.
	 *
	 * Produces RoundToInt((RangeEnd - RangeStart) * SampleRate) + 1 transforms: the fence-post count
	 * Interchange's animation factory expects, where the last key lands on the end of the range
	 * rather than one interval short of it.
	 *
	 * @param Scene             Scene the animation belongs to; used for the node's own transform.
	 * @param Animation         Clip to sample.
	 * @param NodeName          Node to sample. Must be animated by the clip.
	 * @param AxisConverter     Coordinate conversion. The same one the reference pose used, which is
	 *                          what keeps a pose and the animation that drives it in one space.
	 * @param SampleRateHz      Frames per second to bake at.
	 * @param RangeStartSeconds Start of the range, in seconds.
	 * @param RangeEndSeconds   End of the range, in seconds.
	 * @param OutKeys           Receives the baked local transforms, in Unreal space.
	 * @return                  False when the clip does not animate the node, or the request is
	 *                          degenerate.
	 */
	static bool SampleNodeTrack(
		const aiScene& Scene,
		const aiAnimation& Animation,
		const FString& NodeName,
		const FAssimpAxisConverter& AxisConverter,
		double SampleRateHz,
		double RangeStartSeconds,
		double RangeEndSeconds,
		TArray<FTransform>& OutKeys);

	/**
	 * Upper bound on baked keys for one track.
	 *
	 * A file is free to declare an hour-long clip, and a pipeline is free to ask for it at 240 fps;
	 * without a cap that is a near-gigabyte allocation per bone driven entirely by file content.
	 */
	static constexpr int32 MaxBakedKeys = 100000;

private:
	/** Value of a vector key array at a given tick, linearly interpolated and clamped at both ends. */
	static aiVector3D SampleVectorKeys(
		const aiVectorKey* Keys, unsigned int NumKeys, double Ticks, const aiVector3D& Fallback);

	/** As above for rotations, interpolated spherically along the shorter arc. */
	static aiQuaternion SampleQuaternionKeys(
		const aiQuatKey* Keys, unsigned int NumKeys, double Ticks, const aiQuaternion& Fallback);
};
