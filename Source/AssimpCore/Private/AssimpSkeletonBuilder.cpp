// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpSkeletonBuilder.h"

#include "AssimpCore.h"

bool FAssimpSkeletonBuilder::Build(
	const aiScene& Scene,
	TArrayView<const int32> MeshIndices,
	const FAssimpAxisConverter& AxisConverter)
{
	Bones.Reset();
	BoneNodes.Reset();
	BoneNameToIndex.Reset();

	TSet<const aiNode*> ReferencedNodes;
	TSet<FString> SkinningBoneNames;

	if (!CollectBoneNodes(Scene, MeshIndices, ReferencedNodes, SkinningBoneNames))
	{
		return false;
	}

	const aiNode* Root = FindCommonAncestor(ReferencedNodes);
	if (Root == nullptr)
	{
		UE_LOG(LogAssimp, Warning,
			TEXT("Found %d bone node(s) but no common ancestor; skeleton cannot be built."),
			ReferencedNodes.Num());
		return false;
	}

	// Every node on the path from the root down to a bone must be kept, even when it is not itself a
	// bone: those nodes carry transforms that position the bones below them, and pruning them would
	// silently move the skeleton.
	TSet<const aiNode*> RelevantNodes;
	for (const aiNode* ReferencedNode : ReferencedNodes)
	{
		MarkPathToRoot(ReferencedNode, Root, RelevantNodes);
	}

	AddSubtree(Root, INDEX_NONE, RelevantNodes, SkinningBoneNames, AxisConverter);

	if (Bones.IsEmpty())
	{
		return false;
	}

	for (int32 Index = 0; Index < Bones.Num(); ++Index)
	{
		BoneNameToIndex.Add(Bones[Index].Name, Index);
	}

	int32 SkinningBoneCount = 0;
	for (const FBone& Bone : Bones)
	{
		SkinningBoneCount += Bone.bIsSkinningBone ? 1 : 0;
	}

	UE_LOG(LogAssimp, Verbose,
		TEXT("Built skeleton rooted at '%s': %d bone(s), %d of which are skinned to."),
		*Bones[0].Name, Bones.Num(), SkinningBoneCount);

	return true;
}

bool FAssimpSkeletonBuilder::CollectBoneNodes(
	const aiScene& Scene,
	TArrayView<const int32> MeshIndices,
	TSet<const aiNode*>& OutBoneNodes,
	TSet<FString>& OutSkinningBoneNames) const
{
	if (Scene.mRootNode == nullptr)
	{
		return false;
	}

	int32 UnresolvedBones = 0;

	for (const int32 MeshIndex : MeshIndices)
	{
		if (!Scene.mMeshes || MeshIndex < 0 || static_cast<unsigned int>(MeshIndex) >= Scene.mNumMeshes)
		{
			continue;
		}

		const aiMesh* Mesh = Scene.mMeshes[MeshIndex];
		if (Mesh == nullptr || !Mesh->HasBones())
		{
			continue;
		}

		for (unsigned int BoneIndex = 0; BoneIndex < Mesh->mNumBones; ++BoneIndex)
		{
			const aiBone* Bone = Mesh->mBones[BoneIndex];
			if (Bone == nullptr)
			{
				continue;
			}

			// Assimp links a bone to its node by name. FindNode is the same lookup Assimp's own
			// consumers use, and it is the only link available for most formats.
			const aiNode* BoneNode = Scene.mRootNode->FindNode(Bone->mName);
			if (BoneNode == nullptr)
			{
				// A bone naming a node that does not exist means the file is inconsistent. Skip it
				// rather than fail the whole import: the remaining bones usually still form a
				// workable skeleton.
				++UnresolvedBones;
				continue;
			}

			OutBoneNodes.Add(BoneNode);
			OutSkinningBoneNames.Add(FAssimpAxisConverter::ConvertString(Bone->mName));
		}
	}

	if (UnresolvedBones > 0)
	{
		UE_LOG(LogAssimp, Warning,
			TEXT("%d bone(s) referenced a node that does not exist in the scene and were skipped."),
			UnresolvedBones);
	}

	return !OutBoneNodes.IsEmpty();
}

void FAssimpSkeletonBuilder::BuildAncestorChain(const aiNode* Node, TArray<const aiNode*>& OutChain)
{
	OutChain.Reset();

	for (const aiNode* Current = Node; Current != nullptr; Current = Current->mParent)
	{
		OutChain.Add(Current);
	}

	// Reverse so the chain reads root-first, which makes comparing two chains a simple prefix walk.
	Algo::Reverse(OutChain);
}

const aiNode* FAssimpSkeletonBuilder::FindCommonAncestor(const TSet<const aiNode*>& BoneNodes)
{
	if (BoneNodes.IsEmpty())
	{
		return nullptr;
	}

	// Start from one bone's full ancestor chain and trim it against each other bone's chain. What
	// survives is the longest shared prefix, whose last element is the deepest common ancestor.
	TArray<const aiNode*> CommonChain;
	TArray<const aiNode*> Chain;

	bool bFirst = true;
	for (const aiNode* BoneNode : BoneNodes)
	{
		BuildAncestorChain(BoneNode, Chain);

		if (bFirst)
		{
			CommonChain = Chain;
			bFirst = false;
			continue;
		}

		const int32 MaxCompare = FMath::Min(CommonChain.Num(), Chain.Num());
		int32 SharedLength = 0;
		while (SharedLength < MaxCompare && CommonChain[SharedLength] == Chain[SharedLength])
		{
			++SharedLength;
		}

		CommonChain.SetNum(SharedLength);

		if (CommonChain.IsEmpty())
		{
			// No shared ancestry at all. Only possible with a malformed scene, since every node
			// descends from the root.
			return nullptr;
		}
	}

	return CommonChain.IsEmpty() ? nullptr : CommonChain.Last();
}

void FAssimpSkeletonBuilder::MarkPathToRoot(
	const aiNode* Node,
	const aiNode* Root,
	TSet<const aiNode*>& InOutRelevantNodes)
{
	for (const aiNode* Current = Node; Current != nullptr; Current = Current->mParent)
	{
		bool bAlreadyPresent = false;
		InOutRelevantNodes.Add(Current, &bAlreadyPresent);

		// Everything above an already-marked node is marked too, so stop early.
		if (bAlreadyPresent || Current == Root)
		{
			break;
		}
	}
}

void FAssimpSkeletonBuilder::AddSubtree(
	const aiNode* Node,
	int32 ParentIndex,
	const TSet<const aiNode*>& RelevantNodes,
	const TSet<FString>& SkinningBoneNames,
	const FAssimpAxisConverter& AxisConverter)
{
	if (Node == nullptr || !RelevantNodes.Contains(Node))
	{
		return;
	}

	FBone Bone;
	Bone.Name = FAssimpAxisConverter::ConvertString(Node->mName);
	Bone.ParentIndex = ParentIndex;
	Bone.LocalTransform = AxisConverter.ConvertTransform(Node->mTransformation);
	Bone.bIsSkinningBone = SkinningBoneNames.Contains(Bone.Name);

	if (Bone.Name.IsEmpty())
	{
		// Unreal keys bones by name, so an unnamed bone could never be referenced. Synthesising a
		// name keeps the hierarchy intact rather than dropping the node and its children with it.
		Bone.Name = FString::Printf(TEXT("Bone_%d"), Bones.Num());
	}

	// Depth-first insertion guarantees the parents-before-children order Unreal requires.
	const int32 ThisIndex = Bones.Add(MoveTemp(Bone));
	BoneNodes.Add(Node);

	for (unsigned int ChildIndex = 0; ChildIndex < Node->mNumChildren; ++ChildIndex)
	{
		AddSubtree(Node->mChildren[ChildIndex], ThisIndex, RelevantNodes, SkinningBoneNames, AxisConverter);
	}
}

int32 FAssimpSkeletonBuilder::FindBoneIndex(const FString& BoneName) const
{
	const int32* Found = BoneNameToIndex.Find(BoneName);
	return Found != nullptr ? *Found : INDEX_NONE;
}

TArray<FString> FAssimpSkeletonBuilder::GetBoneNames() const
{
	TArray<FString> Names;
	Names.Reserve(Bones.Num());

	for (const FBone& Bone : Bones)
	{
		Names.Add(Bone.Name);
	}

	return Names;
}
