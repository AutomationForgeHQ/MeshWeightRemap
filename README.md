# Mesh Weight Remap

Makes a skeletal mesh safe to **leader-pose** off another one.

Nothing in it knows about MetaHumans, Narrative Pro, or any other character system. Two meshes, one
posing the other, is the whole precondition.

**Version 0.1.1. Experimental.**

---

## The problem

`SetLeaderPoseComponent` does not copy a pose. The follower reads bone transforms straight out of the
leader's component-space transform array, and the leader only ever writes the entries it evaluates.
Everything else in that array still holds the reference pose it was initialised with.

So a follower vertex weighted to a bone the leader does not evaluate is not merely unanimated — it is
**held at the reference pose** while everything around it moves. On a garment that reads as a cuff
pinned at the wrist, a collar that will not turn, or a web of stretched triangles between the two.

The weights are not wrong. The skeletons are not mismatched. The leader simply never computes those
bones, and a name lookup that succeeds against a stale array succeeds silently.

This bites hardest where a garment was fitted against a **full-resolution** body and is then worn by an
**optimised** one: the fit is perfect and the optimiser has thrown away exactly the corrective bones the
fit leaned on.

## What it does

```
driven   = leader mesh, LOD RequiredBones
weighted = bones with any influence on the target mesh
orphans  = weighted AND NOT driven

for each orphan:
    move its influence to the nearest ancestor in `driven`
```

The root is always driven, so the walk always terminates.

The redistribution itself is the engine's own `IMeshBoneReduction::ReduceBoneCounts` — the same call
the MetaHuman assembly pipeline makes when it optimises a body. It reassigns influences to the nearest
surviving parent, accumulates duplicates and renormalises. The only thing this plugin adds is **the
list**: the engine's own list builder removes bones with *no* weight, which is the exact opposite of
what is wanted here, because these bones have weight and that is the problem.

**Weights on driven bones are not touched.** That is the point, and it is what separates this from the
obvious alternative — re-transferring the follower's weights from the leader mesh. That also stops the
pinning, and quietly throws away all the resolution the original binding had wherever the leader has no
geometry. On a garment that is the neck and the shoulders, and the loss does not show up until somebody
turns their head.

## Using it

**From the content browser.** Right-click one or more skeletal meshes → *Remap Weights to Leader…*,
pick the leader, press **Measure**, read the report, then **Remap**.

**Over MCP.** Enable `MeshWeightRemapToolset` and the same three operations are tools: measure a
leader, measure a mesh's weights, remap. That is the path for doing a whole folder of garments at once.

**From Blueprint or C++.** `UMeshWeightRemapSubsystem`, an editor subsystem.

## What counts as "driven"

The bones a leader evaluates are a property of **one LOD of one mesh**. Three readings are offered, and
the tool reports all three side by side so the choice is made on evidence:

| | |
|---|---|
| **LOD Required Bones** | What the leader evaluates. The default, and the smallest honest answer — it errs towards moving a weight that did not strictly need moving, which costs a little detail and never pins anything. |
| **LOD Active Bones** | Only the bones the leader's own skin weights use. Narrower. |
| **+ Physics Asset** | Plus every bone the leader's physics asset has a body on, which is also evaluated at runtime. The widest defensible reading. |

**Reference skeleton bone counts prove nothing.** Meshes built on a shared skeleton all report the same
reference bone list, identically, whether they are a whole body or a single glove. The report includes
that number only so it is not mistaken for the answer.

## Reading a report

Two things to check on every run:

- **Total weight must be identical before and after.** Weight is moved, never created or destroyed. A
  total that has shifted means the bone count above it cannot be trusted — discard the asset unsaved.
- **The emptied bones must genuinely carry nothing afterwards.** A bone that survives the pass is one
  the engine refused to remove, and the reason will be in the log.

If another mesh in the project already leader-poses off the same leader correctly, measure its weighted
bones too. Matching its count exactly is a far stronger result than a fix that merely looks better in
the viewport.

## Known limitations

**The dialogue is modal, and that is worse than it sounds.** It is shown with
`SCustomDialog::ShowModal()`, which runs its own event loop and locks the rest of the editor while it
is open. The practical cost is that the content browser is frozen behind it, so the leader picker's
*Use Selected* arrow has nothing to read and the only way to choose a leader is to type into the
picker's own dropdown search. That is exactly backwards for a tool whose whole job is relating two
assets you are looking at side by side.

The fix is to show the window non-modally — `FSlateApplication::AddWindow` instead of `ShowModal`,
with the **Remap** button driving the commit through its own delegate rather than through a returned
button index, and the window keeping a weak handle to its targets. Deliberately not done in this
version.

## Cautions

- **No undo.** Removing a bone rebuilds the LOD model. Run the dry pass first; the package is left dirty
  and unsaved, which is the one escape hatch.
- **Removal accumulates.** It is stored on the mesh, so remapping the same mesh against a second leader
  adds to what the first run took off rather than replacing it. Remapping against the wrong leader is
  not something you can walk back by running it again correctly. The report names any bones a previous
  run already took off.
- **Lower LODs inherit LOD 0's removals.** Lower LODs of the leader evaluate fewer bones still, so every
  orphan found at LOD 0 is an orphan below it too — correct as far as it goes, and conservative.
