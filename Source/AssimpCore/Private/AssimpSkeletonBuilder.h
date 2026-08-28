// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpAxisConverter.h"
#include "AssimpIncludes.h"
#include "CoreMinimal.h"

/**
 * Reconstructs a usable skeleton from the loose bone references Assimp reports.
 *
 * The problem this solves
 * ----------------------
 * Assimp does not hand back a skeleton. Each aiMesh carries a flat list of aiBones, and each aiBone
 * is just a name plus an offset matrix; the actual hierarchy lives in the aiNode tree, mixed in with
 * every other node in the file. Unreal, by contrast, needs an ordered bone array with parent indices
 * and a reference pose, rooted at a single bone, parents strictly before children.
 *
 * Bridging that gap means three things, none of which Assimp does for you:
 *
 *   1. Finding the nodes the bones name. A bone influencing a mesh may sit anywhere in the tree.
 *   2. Choosing a root. The bones' common ancestor is the natural skeleton root, but the chain from
 *      it down to each bone may pass through nodes that are not themselves bones -- and those still
 *      have to be included, because dropping them would lose the transforms that position the bones
 *      beneath them.
 *   3. Ordering. Unreal requires parents to precede children, which a depth-first walk gives.
 */
class FAssimpSkeletonBuilder
{
public:
	/** One bone in the reconstructed skeleton. */
	struct FBone
	{
		/** Bone name, matching the aiNode and aiBone name it came from. */
		FString Name;

		/** Index of the parent in Bones, or INDEX_NONE for the root. Always less than this bone's index. */
		int32 ParentIndex = INDEX_NONE;

		/** Transform relative to the parent bone, in Unreal space. Forms the reference pose. */
		FTransform LocalTransform;

		/**
		 * True when a mesh actually skins to this bone.
		 *
		 * False for a node included only because it lies on the path between the root and a real
		 * bone. Such nodes must exist in the skeleton to carry their transforms, but nothing is
		 * weighted to them.
		 */
		bool bIsSkinningBone = false;
	};

	/**
	 * Builds a skeleton covering every bone the given meshes reference.
	 *
	 * @param Scene          Parsed scene, used for its node hierarchy.
	 * @param MeshIndices    Meshes whose bones should be covered.
	 * @param AxisConverter  Coordinate conversion for the bone transforms.
	 * @return               True when a skeleton was built; false when the meshes have no bones.
	 */
	bool Build(
		const aiScene& Scene,
		TArrayView<const int32> MeshIndices,
		const FAssimpAxisConverter& AxisConverter);

	/** Bones in Unreal order: parents strictly before children, root first. */
	const TArray<FBone>& GetBones() const { return Bones; }

	/** Index into GetBones() for a bone name, or INDEX_NONE. */
	int32 FindBoneIndex(const FString& BoneName) const;

	/** Bone names in index order, which is the form Interchange's mesh payload expects. */
	TArray<FString> GetBoneNames() const;

private:
	/** Locates the aiNode for each named bone. Returns false when none are found. */
	bool CollectBoneNodes(
		const aiScene& Scene,
		TArrayView<const int32> MeshIndices,
		TSet<const aiNode*>& OutBoneNodes,
		TSet<FString>& OutSkinningBoneNames) const;

	/**
	 * Finds the node to root the skeleton at: the deepest node that is an ancestor of (or is) every
	 * bone node.
	 */
	static const aiNode* FindCommonAncestor(const TSet<const aiNode*>& BoneNodes);

	/** Ancestor chain from the tree root down to and including Node. */
	static void BuildAncestorChain(const aiNode* Node, TArray<const aiNode*>& OutChain);

	/** Adds Node and the parts of its subtree that lead to a bone, depth-first. */
	void AddSubtree(
		const aiNode* Node,
		int32 ParentIndex,
		const TSet<const aiNode*>& RelevantNodes,
		const TSet<FString>& SkinningBoneNames,
		const FAssimpAxisConverter& AxisConverter);

	/**
	 * Marks Node and every ancestor up to Root as relevant, so the chain connecting a bone to the
	 * root survives pruning even where the intermediate nodes are not bones themselves.
	 */
	static void MarkPathToRoot(
		const aiNode* Node,
		const aiNode* Root,
		TSet<const aiNode*>& InOutRelevantNodes);

	TArray<FBone> Bones;

	/** Name-to-index lookup, so skin weights can resolve a bone name in constant time. */
	TMap<FString, int32> BoneNameToIndex;
};
