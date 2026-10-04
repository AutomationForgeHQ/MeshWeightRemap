# MeshWeightRemap

Every released version of MeshWeightRemap, newest first. A release publishes **one** section of this
file — the one whose heading matches its tag — as its release notes; for an `open` plugin those
notes are posted to Discord `#releases` automatically. Write for someone who installs the plugin,
not for the commit log.

Headings are `## <x.y.z> — <date>`. Use `Added` / `Changed` / `Fixed` / `Compatibility` /
`Known issues`, only the ones that apply.

## 0.1.3 — 2026-10-04

### Changed
- Copyright and licence notices now name Bojan Andrejek / MetaWorx LLC. It is still Apache 2.0, and nothing about how you may use it changed.

## 0.1.2 — 2026-09-08
- Packaging fix: the release now carries everything the register allows. `BuildPlugin`'s filter excludes `Config/` and every `public_extra` path, so earlier zips shipped without them.

## 0.1.1 — 2026-09-07
- Aligned the plugin descriptor with its release tag and author attribution.
- Repointed Docs/Support URLs at kovati.dev (superseded the same day by the distribution flip to `open` with a GitHub mirror).
- Flipped distribution from unlisted to `open`, 0.1.1.

## 0.1.0 — 2026-08-28
- Initial release: moving weights off bones a leader mesh will never drive, using the engine's own `IMeshBoneReduction::ReduceBoneCounts`.
