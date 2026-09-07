// Moving skin weights off bones a leader mesh will never drive.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MeshWeightRemapSubsystem.generated.h"

class USkeletalMesh;

/**
 * Which of a leader's bones count as "driven" - the question the whole tool turns on.
 *
 * A follower reads its pose out of the leader's component-space transform array, and the leader only
 * ever writes the entries it evaluates. Everything else in that array still holds the reference pose
 * it was initialised with, so a vertex weighted to one of those bones is pinned. What the leader
 * evaluates is therefore the definition of driven - and it is a per-LOD property of the leader, not
 * anything you can read off a skeleton.
 */
UENUM(BlueprintType)
enum class EMeshWeightRemapDrivenSet : uint8
{
	/**
	 * The LOD's `RequiredBones`. What the leader evaluates before sockets and physics are merged in.
	 *
	 * The right default: it is the set the renderer and the animation system agree on, and being the
	 * smallest honest answer it errs towards moving a weight that did not strictly need moving, which
	 * costs a little detail and never pins anything.
	 */
	RequiredBones UMETA(DisplayName = "LOD Required Bones"),

	/** The LOD's `ActiveBoneIndices` - only the bones the leader's own skin weights use. Narrower. */
	ActiveBones UMETA(DisplayName = "LOD Active Bones"),

	/**
	 * `RequiredBones` plus every bone the leader's physics asset has a body on.
	 *
	 * At runtime those are evaluated too, so this is the widest defensible reading. Use it only if a
	 * measurement says so - a physics asset is not a promise about a component that might not have one.
	 */
	RequiredBonesAndPhysics UMETA(DisplayName = "LOD Required Bones + Physics Asset"),
};

/**
 * What one leader mesh would drive, measured rather than assumed.
 *
 * The counts of the three candidate definitions sit side by side deliberately: one call tells you
 * which of them matches a mesh you already know works, which is cheaper than arguing about it.
 */
USTRUCT(BlueprintType)
struct FMeshWeightRemapLeaderInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	FString LeaderMeshPath;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 LODIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 LODCount = 0;

	/** Size of `EMeshWeightRemapDrivenSet::RequiredBones`. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 RequiredBones = 0;

	/** Size of `EMeshWeightRemapDrivenSet::ActiveBones`. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 ActiveBones = 0;

	/** Bones the physics asset would pin that `RequiredBones` does not already contain. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 ExtraPhysicsAssetBones = 0;

	/**
	 * Bones in the reference skeleton.
	 *
	 * Reported so that nobody quotes it as evidence. It is shared between every mesh built on the same
	 * skeleton, so it is identical on a full body and on a glove, and it distinguishes nothing.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 ReferenceSkeletonBones = 0;

	/** True when the numbers came from cooked render data, false when they came from the imported model. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	bool bFromRenderData = false;

	/** The bones of the requested set, by name, sorted. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	TArray<FName> DrivenBoneNames;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	TArray<FString> Problems;
};

/** One bone's influence on a mesh, counted the way a weight audit counts it. */
USTRUCT(BlueprintType)
struct FMeshWeightRemapBoneWeight
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	FName Bone;

	/** Vertices this bone influences at all. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 Vertices = 0;

	/** Total weight it carries, summed over those vertices. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	float Weight = 0.f;
};

/** One orphan bone and the driven ancestor its weight goes to. */
USTRUCT(BlueprintType)
struct FMeshWeightRemapBoneMove
{
	GENERATED_BODY()

	/** The bone being emptied - weighted on the target, not driven by the leader. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	FName Bone;

	/** Its nearest ancestor that the leader does drive. The root always qualifies, so this always exists. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	FName MovedTo;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 Vertices = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	float Weight = 0.f;
};

/**
 * What a remap did, or - in a dry run - what it would do.
 *
 * The two numbers to read first are `WeightedBonesAfter`, which is the figure an external weight audit
 * reports, and `TotalWeightAfter`, which must equal `TotalWeightBefore`. Weight is moved, never
 * created or destroyed; a total that shifts means something went wrong regardless of how good the
 * bone count looks.
 */
USTRUCT(BlueprintType)
struct FMeshWeightRemapReport
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	bool bSucceeded = false;

	/** True when nothing was written. Every other field still holds what a real run would have produced. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	bool bDryRun = false;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	FString TargetMeshPath;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	FString LeaderMeshPath;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 LODIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	EMeshWeightRemapDrivenSet DrivenSet = EMeshWeightRemapDrivenSet::RequiredBones;

	/** How many bones the leader drives, under the chosen definition. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 DrivenBones = 0;

	/**
	 * Bones carrying any weight on the target, before and after.
	 *
	 * This is the number a per-bone weight audit of an exported mesh reports, so it is the one to
	 * compare against a mesh already known to work.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 WeightedBonesBefore = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 WeightedBonesAfter = 0;

	/** Bones that were weighted and not driven. The length of `Moved`, named for readability. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 OrphanBones = 0;

	/** Total influence weight over the whole LOD. Before and after must match. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	float TotalWeightBefore = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	float TotalWeightAfter = 0.f;

	/** The target's own `RequiredBones` for this LOD. A different quantity from the weighted-bone count. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 TargetRequiredBonesBefore = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	int32 TargetRequiredBonesAfter = 0;

	/** Every orphan and where its weight went, heaviest first. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	TArray<FMeshWeightRemapBoneMove> Moved;

	/**
	 * Bones already on this LOD's removal list before the tool ran.
	 *
	 * Bone removal is a persistent, accumulating mesh setting, so a second remap against a different
	 * leader adds to the first rather than replacing it. Non-empty here means the mesh has a history.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	TArray<FName> PreviouslyRemoved;

	/** Empty on a clean run. Each entry is one reason to look before trusting the result. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Weight Remap")
	TArray<FString> Problems;
};

/**
 * Makes a skeletal mesh safe to leader-pose off another one.
 *
 * `SetLeaderPoseComponent` copies only what the leader evaluates. Any follower vertex weighted to a
 * bone outside that set falls back to the reference pose and stops moving - a sleeve cuff that stays
 * at the wrist while the arm swings. It is not a weight-transfer failure and it is not a skeleton
 * mismatch; the weights are right and the leader simply never computes those bones.
 *
 * So: find the bones the target has weight on and the leader will not drive, and move each one's
 * influence to the nearest ancestor the leader does drive. Weights on driven bones are not touched at
 * all, which is what separates this from re-transferring weights off a body-only mesh - that also
 * fixes the cuff, and loses every bit of resolution the original transfer had at the neck and
 * shoulders.
 *
 * Nothing here is specific to any character system. Two meshes, one posing the other, is the whole
 * precondition.
 */
UCLASS()
class MESHWEIGHTREMAP_API UMeshWeightRemapSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:

	/**
	 * Measure what a leader mesh drives, writing nothing.
	 *
	 * Run this first. It reports all three candidate definitions of driven side by side, which is how
	 * you pick one on evidence instead of on a guess.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mesh Weight Remap")
	FMeshWeightRemapLeaderInfo InspectLeader(const FString& LeaderMeshPath, int32 LODIndex = 0,
		EMeshWeightRemapDrivenSet DrivenSet = EMeshWeightRemapDrivenSet::RequiredBones) const;

	/**
	 * Per-bone influence on a mesh, counted the way an external weight audit counts it.
	 *
	 * Reads the mesh, changes nothing. This is the in-editor reading of the same quantity an FBX audit
	 * measures, and the two agreeing is worth more than either alone.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mesh Weight Remap")
	TArray<FMeshWeightRemapBoneWeight> GetWeightedBones(const FString& MeshPath, int32 LODIndex = 0) const;

	/**
	 * Move the target's weights off everything the leader will not drive.
	 *
	 * Edits the target in place and leaves the package dirty but unsaved. There is no undo - bone
	 * removal rebuilds the LOD model - so run it dry first and read the report.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mesh Weight Remap")
	FMeshWeightRemapReport RemapToLeader(const FString& TargetMeshPath, const FString& LeaderMeshPath,
		EMeshWeightRemapDrivenSet DrivenSet = EMeshWeightRemapDrivenSet::RequiredBones,
		int32 LODIndex = 0, bool bDryRun = false);

	/** As `RemapToLeader`, on meshes already loaded. What the context-menu entry calls. */
	FMeshWeightRemapReport RemapMeshToLeader(USkeletalMesh* Target, USkeletalMesh* Leader,
		EMeshWeightRemapDrivenSet DrivenSet, int32 LODIndex, bool bDryRun);

	/** A report as a few lines of text, for a dialogue or the log. */
	static FString DescribeReport(const FMeshWeightRemapReport& Report);

private:

	static USkeletalMesh* LoadMesh(const FString& Path, TArray<FString>& OutProblems);
};
