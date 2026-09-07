#include "MeshWeightRemapSubsystem.h"

#include "BoneWeights.h"
#include "GPUSkinPublicDefs.h"
#include "MeshBoneReduction.h"
#include "Misc/PackageName.h"
#include "Misc/StringBuilder.h"
#include "Modules/ModuleManager.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalMeshTypes.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogMeshWeightRemap, Log, All);

#define LOCTEXT_NAMESPACE "MeshWeightRemap"

namespace UE::MeshWeightRemap::Private
{
	/** Every bone name in a LOD's index array, resolved through the mesh's own reference skeleton. */
	static void NamesFromIndices(const FReferenceSkeleton& RefSkeleton, const TArray<FBoneIndexType>& Indices,
		TSet<FName>& OutNames)
	{
		for (const FBoneIndexType Index : Indices)
		{
			if (RefSkeleton.IsValidIndex(Index))
			{
				OutNames.Add(RefSkeleton.GetBoneName(Index));
			}
		}
	}

	/**
	 * The three candidate driven sets, all measured in one pass.
	 *
	 * Render data is preferred where it exists because that is literally the array the animation system
	 * reads when it decides which bones to evaluate; the imported model is the editor-side source it was
	 * built from and should agree, but "should" is not a measurement.
	 */
	static bool GatherDrivenSets(const USkeletalMesh* Leader, int32 LODIndex,
		TSet<FName>& OutRequired, TSet<FName>& OutActive, TSet<FName>& OutPhysicsExtra,
		bool& bOutFromRenderData, int32& OutLODCount, TArray<FString>& OutProblems)
	{
		const FReferenceSkeleton& RefSkeleton = Leader->GetRefSkeleton();

		bOutFromRenderData = false;
		OutLODCount = 0;

		if (const FSkeletalMeshRenderData* RenderData = Leader->GetResourceForRendering())
		{
			OutLODCount = RenderData->LODRenderData.Num();
			if (RenderData->LODRenderData.IsValidIndex(LODIndex))
			{
				const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
				NamesFromIndices(RefSkeleton, LOD.RequiredBones, OutRequired);
				NamesFromIndices(RefSkeleton, LOD.ActiveBoneIndices, OutActive);
				bOutFromRenderData = true;
			}
		}

		if (!bOutFromRenderData)
		{
			const FSkeletalMeshModel* Model = Leader->GetImportedModel();
			if (!Model)
			{
				OutProblems.Add(FString::Printf(
					TEXT("'%s' has neither render data nor an imported model, so there is nothing to measure."),
					*Leader->GetPathName()));
				return false;
			}

			OutLODCount = Model->LODModels.Num();
			if (!Model->LODModels.IsValidIndex(LODIndex))
			{
				OutProblems.Add(FString::Printf(TEXT("'%s' has no LOD %d - it has %d."),
					*Leader->GetPathName(), LODIndex, OutLODCount));
				return false;
			}

			const FSkeletalMeshLODModel& LOD = Model->LODModels[LODIndex];
			NamesFromIndices(RefSkeleton, LOD.RequiredBones, OutRequired);
			NamesFromIndices(RefSkeleton, LOD.ActiveBoneIndices, OutActive);
		}

		if (OutRequired.IsEmpty())
		{
			OutProblems.Add(FString::Printf(TEXT("'%s' LOD %d reports no required bones at all."),
				*Leader->GetPathName(), LODIndex));
			return false;
		}

		// A physics body pins its bone, and pinning a bone pins the path back to the root.
		if (const UPhysicsAsset* PhysicsAsset = Leader->GetPhysicsAsset())
		{
			for (const TObjectPtr<USkeletalBodySetup>& BodySetup : PhysicsAsset->SkeletalBodySetups)
			{
				if (!BodySetup)
				{
					continue;
				}

				int32 BoneIndex = RefSkeleton.FindBoneIndex(BodySetup->BoneName);
				while (BoneIndex != INDEX_NONE)
				{
					const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
					if (OutRequired.Contains(BoneName))
					{
						break;
					}
					OutPhysicsExtra.Add(BoneName);
					BoneIndex = RefSkeleton.GetParentIndex(BoneIndex);
				}
			}
		}

		return true;
	}

	/** The chosen set, out of the three. */
	static TSet<FName> SelectDrivenSet(EMeshWeightRemapDrivenSet Which, const TSet<FName>& Required,
		const TSet<FName>& Active, const TSet<FName>& PhysicsExtra)
	{
		switch (Which)
		{
		case EMeshWeightRemapDrivenSet::ActiveBones:
			return Active;

		case EMeshWeightRemapDrivenSet::RequiredBonesAndPhysics:
			return Required.Union(PhysicsExtra);

		case EMeshWeightRemapDrivenSet::RequiredBones:
		default:
			return Required;
		}
	}

	/**
	 * Per-bone influence over a LOD, counted the way a weight audit counts it: a bone appears when some
	 * vertex gives it a non-zero weight, not merely because a section's bone map mentions it.
	 */
	static bool GatherWeightedBones(const USkeletalMesh* Mesh, int32 LODIndex,
		TMap<FName, FMeshWeightRemapBoneWeight>& OutWeights, int32& OutRequiredBoneCount,
		TArray<FString>& OutProblems)
	{
		OutWeights.Reset();
		OutRequiredBoneCount = 0;

		const FSkeletalMeshModel* Model = Mesh->GetImportedModel();
		if (!Model)
		{
			OutProblems.Add(FString::Printf(TEXT("'%s' has no imported model, so its weights cannot be read."),
				*Mesh->GetPathName()));
			return false;
		}

		if (!Model->LODModels.IsValidIndex(LODIndex))
		{
			OutProblems.Add(FString::Printf(TEXT("'%s' has no LOD %d - it has %d."),
				*Mesh->GetPathName(), LODIndex, Model->LODModels.Num()));
			return false;
		}

		const FSkeletalMeshLODModel& LOD = Model->LODModels[LODIndex];
		const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
		OutRequiredBoneCount = LOD.RequiredBones.Num();

		bool bAnyVertices = false;

		for (const FSkelMeshSection& Section : LOD.Sections)
		{
			bAnyVertices |= Section.SoftVertices.Num() > 0;

			for (const FSoftSkinVertex& Vertex : Section.SoftVertices)
			{
				for (int32 InfluenceIndex = 0; InfluenceIndex < MAX_TOTAL_INFLUENCES; ++InfluenceIndex)
				{
					const uint16 RawWeight = Vertex.InfluenceWeights[InfluenceIndex];
					if (RawWeight == 0)
					{
						continue;
					}

					const int32 BoneMapIndex = Vertex.InfluenceBones[InfluenceIndex];
					if (!Section.BoneMap.IsValidIndex(BoneMapIndex))
					{
						continue;
					}

					const FBoneIndexType BoneIndex = Section.BoneMap[BoneMapIndex];
					if (!RefSkeleton.IsValidIndex(BoneIndex))
					{
						continue;
					}

					FMeshWeightRemapBoneWeight& Entry = OutWeights.FindOrAdd(RefSkeleton.GetBoneName(BoneIndex));
					Entry.Bone = RefSkeleton.GetBoneName(BoneIndex);
					Entry.Vertices += 1;
					Entry.Weight += static_cast<float>(RawWeight) * UE::AnimationCore::InvMaxRawBoneWeightFloat;
				}
			}
		}

		if (!bAnyVertices)
		{
			// Bone maps still name the bones, so the orphan set is recoverable; the per-bone numbers are not.
			OutProblems.Add(FString::Printf(
				TEXT("'%s' LOD %d holds no editable vertices, so bones were taken from the section bone maps ")
				TEXT("and the vertex and weight figures in this report are all zero."),
				*Mesh->GetPathName(), LODIndex));

			for (const FSkelMeshSection& Section : LOD.Sections)
			{
				for (const FBoneIndexType BoneIndex : Section.BoneMap)
				{
					if (RefSkeleton.IsValidIndex(BoneIndex))
					{
						OutWeights.FindOrAdd(RefSkeleton.GetBoneName(BoneIndex)).Bone =
							RefSkeleton.GetBoneName(BoneIndex);
					}
				}
			}
		}

		return true;
	}

	/** The nearest ancestor of a bone that the leader drives, itself included. NAME_None if there is none. */
	static FName NearestDrivenAncestor(const FReferenceSkeleton& RefSkeleton, FName Bone,
		const TSet<FName>& Driven)
	{
		int32 BoneIndex = RefSkeleton.FindBoneIndex(Bone);
		while (BoneIndex != INDEX_NONE)
		{
			const FName Name = RefSkeleton.GetBoneName(BoneIndex);
			if (Driven.Contains(Name))
			{
				return Name;
			}
			BoneIndex = RefSkeleton.GetParentIndex(BoneIndex);
		}

		return NAME_None;
	}
}

USkeletalMesh* UMeshWeightRemapSubsystem::LoadMesh(const FString& Path, TArray<FString>& OutProblems)
{
	if (Path.IsEmpty())
	{
		OutProblems.Add(TEXT("No mesh path given."));
		return nullptr;
	}

	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Path);
	if (!Mesh)
	{
		OutProblems.Add(FString::Printf(TEXT("'%s' is not a skeletal mesh that could be loaded."), *Path));
	}

	return Mesh;
}

FMeshWeightRemapLeaderInfo UMeshWeightRemapSubsystem::InspectLeader(const FString& LeaderMeshPath,
	int32 LODIndex, EMeshWeightRemapDrivenSet DrivenSet) const
{
	using namespace UE::MeshWeightRemap::Private;

	FMeshWeightRemapLeaderInfo Info;
	Info.LeaderMeshPath = LeaderMeshPath;
	Info.LODIndex = LODIndex;

	USkeletalMesh* Leader = LoadMesh(LeaderMeshPath, Info.Problems);
	if (!Leader)
	{
		return Info;
	}

	Info.ReferenceSkeletonBones = Leader->GetRefSkeleton().GetNum();

	TSet<FName> Required, Active, PhysicsExtra;
	if (!GatherDrivenSets(Leader, LODIndex, Required, Active, PhysicsExtra,
		Info.bFromRenderData, Info.LODCount, Info.Problems))
	{
		return Info;
	}

	Info.RequiredBones = Required.Num();
	Info.ActiveBones = Active.Num();
	Info.ExtraPhysicsAssetBones = PhysicsExtra.Num();

	Info.DrivenBoneNames = SelectDrivenSet(DrivenSet, Required, Active, PhysicsExtra).Array();
	Info.DrivenBoneNames.Sort(FNameLexicalLess());

	return Info;
}

TArray<FMeshWeightRemapBoneWeight> UMeshWeightRemapSubsystem::GetWeightedBones(const FString& MeshPath,
	int32 LODIndex) const
{
	using namespace UE::MeshWeightRemap::Private;

	TArray<FString> Problems;
	TArray<FMeshWeightRemapBoneWeight> Result;

	USkeletalMesh* Mesh = LoadMesh(MeshPath, Problems);
	if (!Mesh)
	{
		UE_LOG(LogMeshWeightRemap, Warning, TEXT("%s"), *FString::Join(Problems, TEXT(" ")));
		return Result;
	}

	TMap<FName, FMeshWeightRemapBoneWeight> Weights;
	int32 RequiredBoneCount = 0;
	GatherWeightedBones(Mesh, LODIndex, Weights, RequiredBoneCount, Problems);

	Weights.GenerateValueArray(Result);
	Result.Sort([](const FMeshWeightRemapBoneWeight& A, const FMeshWeightRemapBoneWeight& B)
	{
		return A.Weight > B.Weight;
	});

	for (const FString& Problem : Problems)
	{
		UE_LOG(LogMeshWeightRemap, Warning, TEXT("%s"), *Problem);
	}

	return Result;
}

FMeshWeightRemapReport UMeshWeightRemapSubsystem::RemapToLeader(const FString& TargetMeshPath,
	const FString& LeaderMeshPath, EMeshWeightRemapDrivenSet DrivenSet, int32 LODIndex, bool bDryRun)
{
	FMeshWeightRemapReport Report;
	Report.bDryRun = bDryRun;
	Report.TargetMeshPath = TargetMeshPath;
	Report.LeaderMeshPath = LeaderMeshPath;
	Report.LODIndex = LODIndex;
	Report.DrivenSet = DrivenSet;

	USkeletalMesh* Target = LoadMesh(TargetMeshPath, Report.Problems);
	USkeletalMesh* Leader = LoadMesh(LeaderMeshPath, Report.Problems);
	if (!Target || !Leader)
	{
		return Report;
	}

	return RemapMeshToLeader(Target, Leader, DrivenSet, LODIndex, bDryRun);
}

FMeshWeightRemapReport UMeshWeightRemapSubsystem::RemapMeshToLeader(USkeletalMesh* Target,
	USkeletalMesh* Leader, EMeshWeightRemapDrivenSet DrivenSet, int32 LODIndex, bool bDryRun)
{
	using namespace UE::MeshWeightRemap::Private;

	FMeshWeightRemapReport Report;
	Report.bDryRun = bDryRun;
	Report.LODIndex = LODIndex;
	Report.DrivenSet = DrivenSet;
	Report.TargetMeshPath = Target ? Target->GetPathName() : FString();
	Report.LeaderMeshPath = Leader ? Leader->GetPathName() : FString();

	if (!Target || !Leader)
	{
		Report.Problems.Add(TEXT("Both a target mesh and a leader mesh are needed."));
		return Report;
	}

	if (Target == Leader)
	{
		Report.Problems.Add(TEXT("The target and the leader are the same mesh. A mesh drives itself completely."));
		return Report;
	}

	// 1. What the leader drives.
	TSet<FName> Required, Active, PhysicsExtra;
	bool bFromRenderData = false;
	int32 LeaderLODCount = 0;
	if (!GatherDrivenSets(Leader, LODIndex, Required, Active, PhysicsExtra,
		bFromRenderData, LeaderLODCount, Report.Problems))
	{
		return Report;
	}

	const TSet<FName> Driven = SelectDrivenSet(DrivenSet, Required, Active, PhysicsExtra);
	Report.DrivenBones = Driven.Num();

	// 2. What the target has weight on.
	TMap<FName, FMeshWeightRemapBoneWeight> WeightsBefore;
	if (!GatherWeightedBones(Target, LODIndex, WeightsBefore, Report.TargetRequiredBonesBefore, Report.Problems))
	{
		return Report;
	}

	Report.WeightedBonesBefore = WeightsBefore.Num();
	for (const TPair<FName, FMeshWeightRemapBoneWeight>& Pair : WeightsBefore)
	{
		Report.TotalWeightBefore += Pair.Value.Weight;
	}

	// What a previous run against some other leader already took off this LOD. Removal accumulates.
	if (const FSkeletalMeshLODInfo* LODInfo = Target->GetLODInfo(LODIndex))
	{
		for (const FBoneReference& BoneReference : LODInfo->BonesToRemove)
		{
			if (BoneReference.BoneName != NAME_None)
			{
				Report.PreviouslyRemoved.AddUnique(BoneReference.BoneName);
			}
		}
	}

	// 3. The orphans, and where each one's weight goes.
	const FReferenceSkeleton& TargetRefSkeleton = Target->GetRefSkeleton();
	TSet<FName> Orphans;
	TArray<FName> BonesToRemove;

	for (const TPair<FName, FMeshWeightRemapBoneWeight>& Pair : WeightsBefore)
	{
		if (Driven.Contains(Pair.Key))
		{
			continue;
		}

		const FName Ancestor = NearestDrivenAncestor(TargetRefSkeleton, Pair.Key, Driven);
		if (Ancestor == NAME_None)
		{
			Report.Problems.Add(FString::Printf(
				TEXT("'%s' has no driven ancestor anywhere up to the root, so its weight has nowhere to go. ")
				TEXT("The leader is not driving its own root, which means the two meshes do not share a ")
				TEXT("skeleton and leader posing will not work between them at all."),
				*Pair.Key.ToString()));
			return Report;
		}

		Orphans.Add(Pair.Key);
		BonesToRemove.Add(Pair.Key);

		FMeshWeightRemapBoneMove& Move = Report.Moved.AddDefaulted_GetRef();
		Move.Bone = Pair.Key;
		Move.MovedTo = Ancestor;
		Move.Vertices = Pair.Value.Vertices;
		Move.Weight = Pair.Value.Weight;
	}

	Report.OrphanBones = Orphans.Num();
	Report.Moved.Sort([](const FMeshWeightRemapBoneMove& A, const FMeshWeightRemapBoneMove& B)
	{
		return A.Weight > B.Weight;
	});

	// 4. Refuse rather than damage a driven bone.
	//
	// Bone removal takes a bone's whole subtree with it, which is only safe because a leader's required
	// bones include the complete path from the root to each of them - so an undriven bone can have no
	// driven descendant. That invariant is what makes this operation weight-preserving on driven bones,
	// and it is cheap to check rather than trust.
	for (const TPair<FName, FMeshWeightRemapBoneWeight>& Pair : WeightsBefore)
	{
		if (!Driven.Contains(Pair.Key))
		{
			continue;
		}

		const int32 BoneIndex = TargetRefSkeleton.FindBoneIndex(Pair.Key);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		int32 ParentIndex = TargetRefSkeleton.GetParentIndex(BoneIndex);
		while (ParentIndex != INDEX_NONE)
		{
			const FName ParentName = TargetRefSkeleton.GetBoneName(ParentIndex);
			if (Orphans.Contains(ParentName))
			{
				Report.Problems.Add(FString::Printf(
					TEXT("'%s' is driven and weighted but descends from '%s', which is not driven. Removing a ")
					TEXT("bone removes its whole subtree, so this would move weight off a driven bone - the one ")
					TEXT("thing this tool must not do. Nothing was written."),
					*Pair.Key.ToString(), *ParentName.ToString()));
				return Report;
			}
			ParentIndex = TargetRefSkeleton.GetParentIndex(ParentIndex);
		}
	}

	if (BonesToRemove.IsEmpty())
	{
		Report.bSucceeded = Report.Problems.IsEmpty();
		Report.WeightedBonesAfter = Report.WeightedBonesBefore;
		Report.TotalWeightAfter = Report.TotalWeightBefore;
		Report.TargetRequiredBonesAfter = Report.TargetRequiredBonesBefore;
		UE_LOG(LogMeshWeightRemap, Log, TEXT("%s"), *DescribeReport(Report));
		return Report;
	}

	if (bDryRun)
	{
		Report.bSucceeded = Report.Problems.IsEmpty();
		Report.WeightedBonesAfter = Report.WeightedBonesBefore - Orphans.Num();
		Report.TotalWeightAfter = Report.TotalWeightBefore;
		Report.TargetRequiredBonesAfter = Report.TargetRequiredBonesBefore;
		UE_LOG(LogMeshWeightRemap, Log, TEXT("%s"), *DescribeReport(Report));
		return Report;
	}

	// 5. Redistribute.
	//
	// This is the engine's own bone reduction, the same call the MetaHuman assembly pipeline makes when
	// it optimises a body. It reassigns each removed bone's influences to the nearest surviving parent,
	// accumulates duplicates onto one entry and renormalises. The only thing being added here is the
	// list - the engine's own list builder removes bones with no weight, which is the exact opposite of
	// what is wanted, because these bones have weight and that is the problem.
	IMeshBoneReductionModule& BoneReductionModule =
		FModuleManager::Get().LoadModuleChecked<IMeshBoneReductionModule>(TEXT("MeshBoneReduction"));
	IMeshBoneReduction* BoneReduction = BoneReductionModule.GetMeshBoneReductionInterface();
	if (!BoneReduction)
	{
		Report.Problems.Add(TEXT("The MeshBoneReduction module gave no interface, so nothing was written."));
		return Report;
	}

	Target->Modify();

	// Lower LODs are driven by lower LODs of the leader, which evaluate fewer bones still - so every
	// orphan here is an orphan there too, and carrying the removal down is right as far as it goes.
	const TArray<FName> ForceKeepBones;
	BoneReduction->ReduceBoneCounts(Target, ForceKeepBones, BonesToRemove, LODIndex, /*bIncludeBelowLODs*/ true);

	Target->MarkPackageDirty();

	// 6. Read the result back rather than predicting it.
	TMap<FName, FMeshWeightRemapBoneWeight> WeightsAfter;
	if (GatherWeightedBones(Target, LODIndex, WeightsAfter, Report.TargetRequiredBonesAfter, Report.Problems))
	{
		Report.WeightedBonesAfter = WeightsAfter.Num();
		for (const TPair<FName, FMeshWeightRemapBoneWeight>& Pair : WeightsAfter)
		{
			Report.TotalWeightAfter += Pair.Value.Weight;
		}

		for (const FName Orphan : Orphans)
		{
			if (WeightsAfter.Contains(Orphan))
			{
				Report.Problems.Add(FString::Printf(
					TEXT("'%s' still carries weight after the remap."), *Orphan.ToString()));
			}
		}

		// Weight is moved, never made or lost. A total that has shifted invalidates the bone count above it.
		if (!FMath::IsNearlyEqual(Report.TotalWeightBefore, Report.TotalWeightAfter, 0.5f))
		{
			Report.Problems.Add(FString::Printf(
				TEXT("Total weight changed from %.3f to %.3f. It should not change at all; treat the bone ")
				TEXT("counts as unreliable and discard the asset without saving."),
				Report.TotalWeightBefore, Report.TotalWeightAfter));
		}
	}

	Report.bSucceeded = Report.Problems.IsEmpty();
	UE_LOG(LogMeshWeightRemap, Log, TEXT("%s"), *DescribeReport(Report));

	return Report;
}

FString UMeshWeightRemapSubsystem::DescribeReport(const FMeshWeightRemapReport& Report)
{
	TStringBuilder<2048> Builder;

	Builder.Appendf(TEXT("%s%s -> leader %s, LOD %d\n"),
		Report.bDryRun ? TEXT("[dry run] ") : TEXT(""),
		*FPackageName::GetShortName(Report.TargetMeshPath),
		*FPackageName::GetShortName(Report.LeaderMeshPath),
		Report.LODIndex);

	Builder.Appendf(TEXT("  leader drives      %d bones\n"), Report.DrivenBones);
	Builder.Appendf(TEXT("  bones with weight  %d -> %d  (%d moved)\n"),
		Report.WeightedBonesBefore, Report.WeightedBonesAfter, Report.Moved.Num());
	Builder.Appendf(TEXT("  total weight       %.1f -> %.1f\n"),
		Report.TotalWeightBefore, Report.TotalWeightAfter);
	Builder.Appendf(TEXT("  target required    %d -> %d\n"),
		Report.TargetRequiredBonesBefore, Report.TargetRequiredBonesAfter);

	if (!Report.PreviouslyRemoved.IsEmpty())
	{
		Builder.Appendf(TEXT("  already removed    %d bones before this run\n"), Report.PreviouslyRemoved.Num());
	}

	for (const FMeshWeightRemapBoneMove& Move : Report.Moved)
	{
		Builder.Appendf(TEXT("    %-32s -> %-24s %5d verts  %8.3f\n"),
			*Move.Bone.ToString(), *Move.MovedTo.ToString(), Move.Vertices, Move.Weight);
	}

	for (const FString& Problem : Report.Problems)
	{
		Builder.Appendf(TEXT("  PROBLEM: %s\n"), *Problem);
	}

	return Builder.ToString();
}

#undef LOCTEXT_NAMESPACE
