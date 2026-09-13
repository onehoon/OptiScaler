# Work Order — Subnautica 2 XeFG Geometry Diagnostics on Forked `release/0.9` (PR #21)

## Objective

Create an isolated `release/0.9` line inside the fork repository and use it for a new diagnostic PR that investigates the Subnautica 2 Streamline/DLSSG -> XeFG resource-geometry mismatch.

This work must remain entirely inside:

`onehoon/OptiScaler`

Do **not** open, update, or target any PR in `optiscaler/OptiScaler`.

The purpose of this separation is intentional:

- upstream `master` / v10 currently crashes during XeFG swapchain initialization in Subnautica 2 before the new geometry diagnostics can run;
- upstream `release/0.9` is the line where Subnautica 2 has already reached XeFG dispatch successfully;
- the original runtime issue on `release/0.9` is not startup initialization but the repeated XeFG validation failure:

```text
XeFG: Invalid argument. ui and backbuffer resource resolutions must match.
```

The diagnostic PR must therefore be based on a fork-local copy of upstream `release/0.9`, not on fork `master`.

---

## 1. Repository and remote model

Expected remotes:

```text
origin   = onehoon/OptiScaler
upstream = optiscaler/OptiScaler
```

Verify them before doing any branch work:

```bash
git remote -v
```

If `upstream` is missing, add it:

```bash
git remote add upstream https://github.com/optiscaler/OptiScaler.git
```

Fetch both remotes:

```bash
git fetch origin --prune
git fetch upstream --prune
```

Do not merge fork `master` into any `release/0.9` branch.

Do not rebase upstream `release/0.9` onto fork `master`.

---

## 2. Create the fork-local base branch

Create a new branch in the fork named exactly:

```text
release/0.9
```

Its initial commit must be the current upstream `release/0.9` HEAD.

At the time this work order was written, upstream `release/0.9` resolves to:

```text
132bc110f371d273834681ae05c73db4212bd337
```

Commit message at that revision:

```text
Remove forced 4x limit at Config.cpp and use value reported from libxess_fg.dll
```

However, **re-fetch and resolve upstream at execution time** rather than blindly assuming the SHA is still current:

```bash
git fetch upstream release/0.9
UPSTREAM_09_SHA=$(git rev-parse upstream/release/0.9)
echo "$UPSTREAM_09_SHA"
```

Then create/reset the local branch from the upstream ref:

```bash
git switch --create release/0.9 upstream/release/0.9
```

If a stale local `release/0.9` already exists, do not silently merge it. Inspect it first and, if it is not meant to contain unique work, recreate it from `upstream/release/0.9` explicitly.

Push the clean base branch to the fork:

```bash
git push -u origin release/0.9
```

### Mandatory verification

Verify all three SHAs match before proceeding:

```bash
git rev-parse upstream/release/0.9
git rev-parse release/0.9
git rev-parse origin/release/0.9
```

Expected relationship:

```text
upstream/release/0.9
        ==
local release/0.9
        ==
origin/release/0.9
```

There must be **zero fork-master commits** in this base branch.

Useful verification:

```bash
git log --oneline --decorate -5 release/0.9
git diff upstream/release/0.9..origin/release/0.9
```

The diff between the upstream and fork base branches should be empty immediately after creation.

---

## 3. Create the PR #21 working branch

Create a dedicated diagnostic branch from the new fork-local base:

```text
diag/subnautica2-xefg-geometry-release-0.9
```

Commands:

```bash
git switch release/0.9
git pull --ff-only origin release/0.9
git switch -c diag/subnautica2-xefg-geometry-release-0.9
```

The branch parent must be `origin/release/0.9`, not `master`.

Do not branch from PR #20.

Do not cherry-pick PR #20's commit wholesale.

PR #20 is a **reference for the desired diagnostics only**; its code was written against the master/v10 structure and must be manually adapted to the `release/0.9` implementation.

---

## 4. Runtime background

Known working `release/0.9` behavior from the Subnautica 2 investigation:

```text
Physical DXGI swapchain: 1920x1200
DLSS/XeSS-side render resolution observed: 904x565
Display/output resolution observed: 1536x960
XeFG interpolation resolution: 1536x960
```

XeFG repeatedly reaches dispatch but then reports:

```text
XeFG: Invalid argument. ui and backbuffer resource resolutions must match.
```

Important observations already established:

- 1536x960 is exactly 80% of 1920x1200 in each axis.
- 904x565 is approximately 58.8% of 1536x960, consistent with the selected Balanced upscaling ratio.
- Windows display scaling was retested at 100%; the 1536x960 behavior remained.
- OptiScaler Output Scaling was not actually dispatching in the prior logs.
- REF is not involved in this game path.
- Streamline successfully supplies the expected DLSSG resources.
- The failure occurs after XeFG setup/dispatch has otherwise proceeded.

Current strongest hypothesis:

```text
Game/UE presentation surface:       1920x1200
HUD-less / DLSS output region:       1536x960
DLSS Balanced internal render:        904x565
```

and OptiScaler `release/0.9` derives the XeFG interpolation rectangle from the Streamline HUDLessColor geometry, then uses that geometry for the XeFG BACKBUFFER tag while UI geometry differs.

This PR exists to prove the exact geometry chain without guessing.

---

## 5. Scope of PR #21

PR #21 is **diagnostics only**.

Primary files expected to change:

```text
OptiScaler/upscalers/IFeature.cpp
OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

Additional files may be touched only if strictly necessary to expose existing metadata safely.

### Explicit non-goals

Do not:

- change XeFG interpolation behavior;
- force 1920x1200 anywhere;
- change Streamline tag interpretation;
- change HUDLess/UI resource selection;
- alter resource lifetimes;
- alter command-list handling;
- alter swapchain ownership or COM reference logic;
- backport master/v10 lifecycle code;
- add a Subnautica 2 game quirk;
- change the XeFG success/warning policy;
- fix the geometry mismatch in this PR;
- pull unrelated fork-master commits into the release branch.

The PR must remain a low-risk diagnostic patch.

---

## 6. Preserve `release/0.9` semantics exactly

The current upstream `release/0.9` Streamline path already does this:

```cpp
res.width = tag.extent ? tag.extent.width : desc.Width;
res.height = tag.extent ? tag.extent.height : desc.Height;
```

and for HUDLessColor:

```cpp
fgOutput->SetInterpolationRect(res.width, res.height);
fgOutput->SetResource(&res);
```

For UIColorAndAlpha it reads the current interpolation rectangle and only falls back to the UI resource dimensions when the existing width is zero.

Do not modify those decisions in PR #21.

Log them.

---

## 7. Diagnostic A — raw NGX initialization geometry

File:

```text
OptiScaler/upscalers/IFeature.cpp
```

In `IFeature::SetInitParameters(...)`, when `OutWidth` and `OutHeight` are available, log the raw values supplied by the game **before** `GetDynamicOutputResolution(...)` or later normalization changes them.

Capture at minimum:

```text
NVSDK_NGX_Parameter_Width
NVSDK_NGX_Parameter_Height
NVSDK_NGX_Parameter_OutWidth
NVSDK_NGX_Parameter_OutHeight
NVSDK_NGX_Parameter_PerfQualityValue
```

Suggested format:

```cpp
LOG_INFO(
    "[RES-DIAG][NGX] Width={} Height={} OutWidth={} OutHeight={} Quality={}",
    width,
    height,
    outWidth,
    outHeight,
    pqValue);
```

Also attempt to read:

```text
NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width
NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height
```

If unavailable, log the return codes rather than treating absence as an error.

Do not modify the parameter values.

---

## 8. Diagnostic B — Streamline tag extent vs real D3D12 texture

File:

```text
OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp
```

Inside `Sl_Inputs_Dx12::reportResource(...)`, preserve the existing resource construction and log both coordinate systems separately.

For these resource types:

```text
Depth
HiResDepth
LinearDepth
MotionVectors
HUDLessColor
UIColorAndAlpha
```

capture:

```text
frameId
resolved frame index
resource type
lifecycle
tag.extent.width / height
tag.extent.left / top
ID3D12Resource::GetDesc().Width / Height
resolved res.width / res.height
res.validity
resource state
```

Suggested prefix:

```text
[RES-DIAG][SL]
```

Important: `res.left` / `res.top` are not currently populated from `tag.extent` in this release path. Do **not** alter them just to improve the log. Log `tag.extent.left/top` directly so the diagnostics do not change runtime semantics.

Example conceptual output:

```text
[RES-DIAG][SL] frame=999 index=3 type=HUDLessColor lifecycle=eValidUntilPresent extent=1536x960 extentBase=192,120 texture=1920x1200 resolved=1536x960 validity=UntilPresent state=...
```

Actual values must come from runtime; do not hard-code expectations.

---

## 9. Diagnostic C — explicit HUDLess interpolation source

In the `kBufferTypeHUDLessColor` branch, immediately before the existing:

```cpp
fgOutput->SetInterpolationRect(res.width, res.height);
```

log exactly what will become the interpolation rectangle.

Suggested prefix:

```text
[RES-DIAG][SL-HUDLESS]
```

Include:

```text
frameId
frameIndex
resolved width/height
actual texture Width/Height
tag extent width/height
tag extent left/top
interpolation value being written
```

The purpose is to prove whether the 1536x960 value originates directly from HUDLessColor's Streamline extent/resource geometry.

---

## 10. Diagnostic D — explicit UI geometry

In the `kBufferTypeUIColorAndAlpha` branch, log:

```text
frameId
frameIndex
UI resolved width/height
UI actual D3D12 texture Width/Height
UI tag extent width/height
UI tag extent left/top
interpolation rectangle before UI fallback logic
whether fallback was applied
interpolation rectangle after fallback logic
```

Suggested prefix:

```text
[RES-DIAG][SL-UI]
```

### Index correctness requirement

Be careful when reading the post-fallback interpolation rectangle.

Use the same intended frame/index context consistently. Do not accidentally compare `_currentIndex` before the fallback with the default `GetIndex()` afterward if those could refer to different slots.

Prefer explicit indices in the diagnostic readback where the release/0.9 API allows it.

The logging itself must not change which slot the production code writes.

---

## 11. Diagnostic E — XeFG resource submission geometry

File:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

Adapt the useful geometry instrumentation from PR #20 to the release/0.9 implementation manually.

For each relevant `Dx12Resource` passed toward XeFG, log both:

```text
actual pResource->GetDesc().Width / Height
```

and the XeFG-facing geometry:

```text
resourceBase
resourceSize
validity
incoming state
frame id
resource index
resource type
whether it is actually submitted
```

Suggested prefix:

```text
[RES-DIAG][XEFG-RESOURCE]
```

Prioritize:

```text
HUDLessColor
UIColor
Depth
Velocity
```

Do not infer the physical texture dimensions from `resourceSize`; query `GetDesc()` separately.

---

## 12. Diagnostic F — final XeFG BACKBUFFER tag

In the DLSSG/Streamline path of `XeFG_Dx12::Dispatch()`, log the final BACKBUFFER geometry immediately before `D3D12TagFrameResource` receives it.

Capture:

```text
frame id
buffer/index
actual swapchain width/height
interpolation width/height
interpolation left/top if available
calculated left/top
FGRect overrides and whether each is present
final backbuffer.resourceBase
final backbuffer.resourceSize
active FG input
```

Suggested prefix:

```text
[RES-DIAG][XEFG-BACKBUFFER]
```

Expected diagnostic question:

```text
Is a 1920x1200 physical backbuffer being tagged to XeFG with a 1536x960 resourceSize because the interpolation rectangle came from HUDLessColor?
```

Do not change the answer in this PR; only expose it.

---

## 13. Logging volume and safety

This is a temporary diagnostic branch, so INFO-level geometry logs are acceptable for a short local capture.

Still:

- keep prefixes consistent;
- avoid logging unrelated resources;
- do not dereference null D3D12 resources;
- call `GetDesc()` only after validating the resource pointer;
- avoid acquiring new locks solely for logging unless strictly required;
- do not retain raw pointers beyond their existing lifetime;
- do not add logging that changes resource ownership or synchronization.

This PR must not introduce the master/v10 swapchain initialization code path or its current Subnautica 2 crash behavior.

---

## 14. Build validation

Build the `release/0.9` diagnostic branch in the configuration normally used for the local OptiScaler DLL test.

Minimum validation:

```text
Release|x64 build succeeds
git diff --check succeeds
no unrelated generated/binary files are committed
```

If clang-format applies to the touched files, run it or the repository's normal formatting validation.

The resulting DLL must clearly identify the branch/commit in its log so the capture cannot be confused with PR #20/master builds.

---

## 15. Runtime test configuration

Primary test:

```text
Game: Subnautica 2
GPU: Intel Arc B390
API: DX12
Physical game / swapchain resolution: 1920x1200
Windows scaling: 100%
FG input: DLSSG / Streamline
FG output: XeFG
Upscaling mode: Balanced
Output Scaling: OFF / not dispatching
```

Run only long enough to capture representative initialization and several FG frames.

The first required capture should establish all of the following in the same run:

```text
NGX Width/Height and OutWidth/OutHeight
NGX render subrect if supplied
HUDLess Streamline extent and physical texture size
UI Streamline extent and physical texture size
selected interpolation rectangle
actual swapchain size
final XeFG BACKBUFFER resourceBase/resourceSize
XeFG validation result
```

---

## 16. Optional A/B runtime tests

Only after the Balanced capture works, optionally test:

```text
DLSS Quality
DLSS Performance
DLAA/native if available
```

Key question:

```text
Does OutWidth/OutHeight and HUDLess remain 1536x960 while only the internal render resolution changes?
```

Optional second resolution:

```text
1600x1000
```

If the intermediate output becomes:

```text
1280x800
```

that would strongly support an exact 80% game/UE viewport/screen-percentage stage.

These A/B tests are not required to open PR #21.

---

## 17. PR creation — fork only

Push the diagnostic branch to the fork:

```bash
git push -u origin diag/subnautica2-xefg-geometry-release-0.9
```

Open the PR in:

```text
onehoon/OptiScaler
```

with:

```text
base: release/0.9
head: diag/subnautica2-xefg-geometry-release-0.9
```

The resulting PR is intended to be **PR #21** if #21 is the next available number.

### Hard prohibition

Do not create a PR against:

```text
optiscaler/OptiScaler
```

Do not set the base to upstream `release/0.9` through a cross-repository PR.

Do not target fork `master`.

Everything must remain fork-local.

---

## 18. Suggested PR title

```text
diag: trace Subnautica 2 XeFG geometry on release/0.9
```

---

## 19. Suggested PR body

```markdown
## Summary

Adds targeted resource-geometry diagnostics to the fork-local `release/0.9` line for the Subnautica 2 DLSSG/Streamline -> XeFG issue.

Subnautica 2 reaches XeFG dispatch on `release/0.9`, but XeFG repeatedly reports:

`ui and backbuffer resource resolutions must match`

Observed geometry is approximately:

- physical swapchain: `1920x1200`
- DLSS/Streamline output: `1536x960`
- Balanced internal render: `904x565`

## Branch isolation

This PR is intentionally based on the fork's `release/0.9` branch, which mirrors upstream `optiscaler/OptiScaler:release/0.9`.

It is not based on fork `master` and is not intended for an upstream PR.

## Diagnostics

The patch traces:

- raw NGX render/output dimensions
- NGX render subrect when present
- Streamline tag extents and offsets
- actual D3D12 texture dimensions
- HUDLess and UI geometry
- interpolation-rectangle selection
- XeFG resource geometry
- final XeFG BACKBUFFER base/size
- actual swapchain dimensions

## Non-goals

This PR does not:

- force a resolution
- change interpolation behavior
- change Streamline resource interpretation
- alter swapchain ownership/lifecycle
- add a Subnautica 2 quirk
- attempt the production fix

The goal is to obtain one authoritative runtime trace before designing a generic fix.
```

---

## 20. Commit guidance

Prefer one focused diagnostic commit.

Suggested message:

```text
diag: trace Streamline XeFG geometry on release/0.9
```

Do not include the creation of the fork `release/0.9` base itself as a synthetic merge or unrelated commit. The base branch should simply point at the upstream release commit history.

---

## 21. Success criteria

PR #21 is ready for local runtime testing when all of the following are true:

1. `onehoon/OptiScaler:release/0.9` exists.
2. Its initial/current base is verified against the fetched upstream `release/0.9` revision used for this work.
3. The diagnostic branch is based on the fork `release/0.9`, not `master`.
4. Only release/0.9-compatible diagnostic instrumentation is added.
5. No PR #20 master-specific behavior or lifecycle changes are backported accidentally.
6. Release|x64 builds successfully.
7. `git diff --check` passes.
8. PR #21 targets `onehoon/OptiScaler:release/0.9`.
9. No upstream PR is created.
10. The PR remains unmerged until Subnautica 2 runtime logs are reviewed.

---

## 22. Required handoff report

After opening PR #21, report:

```text
fork base branch name
fork base SHA
upstream release/0.9 SHA used
whether both SHAs match
working branch name
working branch head SHA
PR URL
PR base/head
changed files
build result
format/diff-check result
confirmation that no upstream PR was opened
```

Do not merge PR #21.
