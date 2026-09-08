# Work Order: RE9 / PRAGMATA Reflex Core Capability Gate Diagnostics

Date: 2026-09-08  
Repository: `onehoon/OptiScaler`  
PR base branch: `Reflex_RE9_Pragmata`  
Code baseline before this documentation-only commit: `2ac3f9ba8bb020247085dc27ba34bca4ae35962a`  
Suggested implementation branch: `diagnostic/re9-pragmata-reflex-core-gate`  
Suggested PR mode: **Draft**  
Target PR base: **`Reflex_RE9_Pragmata` — not `master`**

---

# 1. Purpose

Determine exactly where Resident Evil Requiem (`re9.exe`) and PRAGMATA (`pragmata.exe`) stop entering NVIDIA Streamline's Reflex path on Intel GPUs when OptiScaler Streamline spoofing is active.

This is a **diagnostic-only follow-up** to merged PR #3:

> `Fix RE9/PRAGMATA Reflex availability on Intel`

PR #3 was merged into `Reflex_RE9_Pragmata` at:

```text
2ac3f9ba8bb020247085dc27ba34bca4ae35962a
```

The first runtime validation using that merged build disproved the previous working assumption that the failure occurs inside `slReflexGetState()` / `NvAPI_D3D_GetSleepStatus()`.

The new objective is therefore:

> Identify the first Reflex capability / API gate that the two games either fail or never call.

Do **not** add another functional Reflex availability override in this PR.

Do **not** guess the next fix before the diagnostic evidence is collected.

---

# 2. Why A New Diagnostic PR Is Required

The merged PR #3 added two main observation / compatibility points:

1. `slReflexGetState` interception through the Reflex plugin-function path.
2. A guarded Intel-only fallback in fake NVAPI `NvAPI_D3D_GetSleepStatus` after the normal `LowLatencyCtx` call fails.

The new Intel runtime logs show that neither path is reached.

## 2.1 PRAGMATA runtime evidence

Tested build:

```text
OptiScaler v10.0.0-dev (2ac3f9ba)
```

The expected game quirk is active:

```text
Quirk: Fix Streamline Reflex availability on Intel
```

Streamline core initializes as v2.8.0 and the interposer v2 hook is active:

```text
Streamline version: 2.8.0
Hooking v2
```

The Reflex plugin loads successfully:

```text
Loaded plugin 'sl.reflex' - version 2.8.0.b735fd37 - id 3 - priority 100 - adapter mask 0x1 - interposer 'no'
```

XeLL is selected successfully:

```text
LowLatency algo: XeLL (forced)
```

However:

```text
Reflex GetState: 0 calls
Intel GetSleepStatus compatibility fallback: 0 calls
slReflexSetOptions: 0 observed calls
slReflexSleep: 0 observed calls
slReflexSetMarker / slPCLSetMarker: 0 observed calls
```

XeFG dispatch evidence:

```text
6,158 total XeFG dispatch records
6,158 with Reflex Id: 0
0 with non-zero Reflex Id
```

## 2.2 RE9 runtime evidence

Tested build:

```text
OptiScaler v10.0.0-dev (2ac3f9ba)
```

The same Intel-specific quirk is active.

Streamline core also initializes as v2.8.0 and hooks successfully.

The Reflex plugin also loads successfully as:

```text
sl.reflex 2.8.0.b735fd37
adapter mask 0x1
```

XeLL is also selected successfully.

But again:

```text
Reflex GetState: 0 calls
Intel GetSleepStatus compatibility fallback: 0 calls
slReflexSetOptions: 0 observed calls
slReflexSleep: 0 observed calls
slReflexSetMarker / slPCLSetMarker: 0 observed calls
```

XeFG dispatch evidence:

```text
1,653 total XeFG dispatch records
1,653 with Reflex Id: 0
0 with non-zero Reflex Id
```

## 2.3 Conclusion from the runtime evidence

The current flow is effectively:

```text
sl.reflex plugin load                 OK
        |
        v
adapter mask / plugin initialization  OK
        |
        v
[unknown game/core Reflex gate]       FAIL / SKIP / NOT CALLED
        |
        v
slReflexGetState                      NEVER REACHED
        |
        v
NvAPI_D3D_GetSleepStatus              NEVER REACHED
        |
        v
slReflexSetOptions / markers          NEVER REACHED
```

Therefore, PR #3's compatibility fallback is currently a dead path in these reproductions.

This does **not** prove that the PR #3 code is mechanically broken. It proves that the games do not reach that layer.

---

# 3. Important Branch / PR Structure

Do not target `master` with this diagnostic work.

Current repository structure is divergent:

```text
master
  -> current master XeFG / CI / swapchain work

Reflex_RE9_Pragmata
  -> RE9 / PRAGMATA experimental lineage
  -> merged PR #3
  -> 2ac3f9ba
```

At the time of this work order, `Reflex_RE9_Pragmata` and `master` are not a simple fast-forward relationship.

The correct workflow is:

```text
Reflex_RE9_Pragmata
        |
        +-- diagnostic/re9-pragmata-reflex-core-gate
                |
                +-- Draft PR -> Reflex_RE9_Pragmata
```

Do not reuse:

```text
fix/re9-pragmata-reflex-availability-quirk
```

That branch belongs to already-merged PR #3 and should remain historical.

Do not create a PR to `master` from the current Reflex experimental lineage.

After the real cause is identified and runtime-proven, prepare a separate minimal implementation against the then-current intended integration branch.

---

# 4. Primary Questions This Diagnostic Must Answer

The diagnostic PR must make the next log answer these questions unambiguously.

## Q1. Did the game request Reflex at `slInit`?

Inspect `sl::Preferences::featuresToLoad` passed to `hkslInit`.

We need to know whether the initial feature list explicitly contains:

```cpp
sl::kFeatureReflex
```

Also record whether it contains:

```cpp
sl::kFeaturePCL
sl::kFeatureDLSS_G
sl::kFeatureDLSS
```

This tells us whether the game explicitly asks Streamline to load/use Reflex, or whether `sl.reflex` appears only through another dependency / plugin-manager behavior.

## Q2. Does the game call `slIsFeatureSupported(kFeatureReflex, ...)`?

If yes, record the original result exactly.

Do not override it in this PR.

NVIDIA's Reflex integration guidance uses `slIsFeatureSupported(sl::kFeatureReflex, adapterInfo)` as an adapter capability gate before the runtime Reflex state path.

If this returns an error for RE9 / PRAGMATA, that becomes the leading functional-fix candidate for a later PR.

## Q3. Does the game call `slIsFeatureLoaded(kFeatureReflex, ...)`?

If yes, record:

```text
result
loaded
```

Do not force `loaded = true` in this PR.

## Q4. Does the game query Reflex requirements or version?

Observe:

```cpp
slGetFeatureRequirements(sl::kFeatureReflex, ...)
slGetFeatureVersion(sl::kFeatureReflex, ...)
```

Record the original result.

For version, if the original call succeeds, also record the returned SL / NGX version fields that are safely available in the current struct.

Do not synthesize a version.

## Q5. Does the game request Reflex functions through the Streamline core API?

Observe:

```cpp
slGetFeatureFunction(sl::kFeatureReflex, functionName, function)
```

Record each distinct requested Reflex function name and the original result / returned pointer presence.

Relevant names may include:

```text
slReflexGetState
slReflexSetOptions
slReflexSleep
slReflexSetMarker
```

Do not replace the returned function pointer at this core layer for diagnostic purposes.

Existing plugin-level hooking may still wrap supported functions as it already does.

## Q6. What functions are requested directly from `sl.reflex`'s plugin export?

The existing function:

```cpp
StreamlineHooks::hkreflex_slGetPluginFunction(const char* functionName)
```

currently has its generic diagnostic log commented out.

Add bounded diagnostics so the next run shows which distinct plugin functions are requested.

This is essential because the current `hkslReflexGetState()` diagnostic can only run after the game or Streamline has already requested `slReflexGetState`.

## Q7. If none of the above core Reflex queries occur, is the gate outside Streamline?

If the target games:

- load `sl.reflex`, but
- never query Reflex support/load/version/function/state,

then the strongest remaining hypothesis is a game-side gate before the normal Reflex Streamline API sequence, such as an engine GPU/NVAPI capability decision.

In that case this PR has succeeded diagnostically.

Do not continue by blindly forcing Streamline state.

Stop and report that the next investigation must move to the game's upstream capability input, fake NVAPI API surface, or another engine-visible NVIDIA capability source.

---

# 5. Implementation Scope

Expected primary code file:

```text
OptiScaler/hooks/Streamline_Hooks.cpp
```

Modify the header only if strictly required by the chosen bounded-log helper implementation:

```text
OptiScaler/hooks/Streamline_Hooks.h
```

Do not modify unless the evidence requires it:

```text
OptiScaler/misc/Quirks.h
OptiScaler/nvapi/fakenvapi/nvapi_calls.cpp
OptiScaler/nvapi/fakenvapi/*
OptiScaler/hooks/Reflex_Hooks.cpp
```

The existing target quirk is already present and is sufficient as the diagnostic title scope:

```cpp
GameQuirk::FixSlReflexAvailabilityOnIntel
```

Do not add another RE9 / PRAGMATA quirk just for logging.

Do not add the existing quirk to:

```text
re9demo.exe
pragmata_sketchbook.exe
DD2
Monster Hunter Wilds
```

---

# 6. Mandatory Non-Behavioral Rule

This PR is diagnostics only.

For Reflex feature queries, the observable return values must be the original values.

The following are prohibited in this PR:

```cpp
return sl::Result::eOk;              // new Reflex capability override
loaded = true;                       // new Reflex loaded override
state.lowLatencyAvailable = true;    // new state override
function = someDummyReflexFunction;  // new function substitution
```

Do not modify `AdapterInfo` before the original call.

Do not modify Streamline plugin JSON for Reflex.

Do not add a new fake-NVAPI success fallback.

Do not alter `LowLatencyCtx`.

The existing PR #3 compatibility code should remain unchanged for now so the new diagnostic PR changes only observability.

If a later runtime reaches the existing PR #3 path, its current logging will still provide useful evidence.

---

# 7. Diagnostic Logging Design

Use one consistent prefix so field logs can be searched quickly:

```text
[ReflexGate]
```

Prefer `LOG_INFO` for one-time / state-transition diagnostics needed from a normal support log.

Do not add per-frame logging.

Do not emit the same unchanged result thousands of times.

A run should normally produce only a small number of `[ReflexGate]` lines.

## 7.1 Common gate helper

Use the existing game quirk to scope target diagnostics:

```cpp
static bool shouldLogReflexGate()
{
    return static_cast<bool>(
        State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailabilityOnIntel);
}
```

Do not require successful Intel vendor resolution merely to log the game-side call sequence.

Reason:

The quirk itself already restricts these diagnostics to the two reproduced executables, and vendor-resolution failure is itself potentially useful evidence during diagnosis.

Do not change functional vendor gating in PR #3's existing fake-NVAPI fallback.

## 7.2 `hkslInit` feature snapshot

Add one bounded startup line after copying / inspecting the incoming `Preferences`, before returning to the original `slInit`.

Suggested output:

```text
[ReflexGate] slInit features count=4 reflex=1 pcl=1 dlss=1 dlssg=1
```

Suggested implementation shape:

```cpp
if (shouldLogReflexGate())
{
    bool hasReflex = false;
    bool hasPcl = false;
    bool hasDlss = false;
    bool hasDlssg = false;

    for (uint32_t i = 0; i < pref.numFeaturesToLoad; ++i)
    {
        const auto feature = pref.featuresToLoad[i];
        hasReflex |= feature == sl::kFeatureReflex;
        hasPcl |= feature == sl::kFeaturePCL;
        hasDlss |= feature == sl::kFeatureDLSS;
        hasDlssg |= feature == sl::kFeatureDLSS_G;
    }

    LOG_INFO("[ReflexGate] slInit features count={} reflex={} pcl={} dlss={} dlssg={}",
             pref.numFeaturesToLoad, hasReflex, hasPcl, hasDlss, hasDlssg);
}
```

Be defensive if `featuresToLoad == nullptr` while count is zero.

Do not dereference a null feature array.

## 7.3 `slIsFeatureSupported`

Preserve the existing DLSSG behavior exactly.

Current code has a special DLSSG path. Do not regress it.

For Reflex only, call the original function and log its original result:

```cpp
sl::Result StreamlineHooks::hkslIsFeatureSupported(sl::Feature feature,
                                                    const sl::AdapterInfo& adapterInfo)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk; // existing behavior, leave as-is

    const auto result = o_slIsFeatureSupported(feature, adapterInfo);

    if (feature == sl::kFeatureReflex && shouldLogReflexGate())
    {
        LOG_INFO("[ReflexGate] slIsFeatureSupported Reflex result={}",
                 magic_enum::enum_name(result));
    }

    return result;
}
```

If this API may be called repeatedly with the same result, bound the log to first call plus state changes.

Do not force success.

If convenient and safe, include whether `adapterInfo.deviceLUID` is present and its size, but do not dump arbitrary memory.

Example:

```text
[ReflexGate] slIsFeatureSupported Reflex result=eOk luidPresent=1 luidSize=8
```

## 7.4 `slIsFeatureLoaded`

Preserve the existing DLSSG special case exactly.

For Reflex:

```cpp
const auto result = o_slIsFeatureLoaded(feature, loaded);

if (feature == sl::kFeatureReflex && shouldLogReflexGate())
{
    LOG_INFO("[ReflexGate] slIsFeatureLoaded Reflex result={} loaded={}",
             magic_enum::enum_name(result), loaded);
}

return result;
```

Bound unchanged repeats.

Do not modify `loaded`.

## 7.5 `slGetFeatureRequirements`

Preserve the existing DLSSG special case.

For Reflex, log the original result after calling the original implementation.

Minimum required output:

```text
[ReflexGate] slGetFeatureRequirements Reflex result=eOk
```

Do not dump the entire requirements struct unless individual fields are already stable and known in the current Streamline header version.

Avoid adding fragile version-dependent diagnostics.

## 7.6 `slGetFeatureVersion`

Preserve the existing DLSSG special case.

For Reflex:

```text
[ReflexGate] slGetFeatureVersion Reflex result=eOk sl=X.Y.Z ngx=A.B.C
```

Only read version fields after a successful original call and only fields that exist in the currently compiled `sl::FeatureVersion` definition.

If the exact struct layout makes this noisy or version-sensitive, logging only the result is acceptable for the first diagnostic pass.

Do not synthesize Reflex version support.

## 7.7 Core `slGetFeatureFunction`

Preserve all existing DLSSG dummy-function logic exactly.

For Reflex calls, use the original core API and then log:

```text
[ReflexGate] slGetFeatureFunction Reflex name=slReflexGetState result=eOk ptr=1
```

Suggested shape:

```cpp
if (feature == sl::kFeatureReflex && shouldLogReflexGate())
{
    const auto result = o_slGetFeatureFunction(feature, functionName, function);

    LOG_INFO("[ReflexGate] slGetFeatureFunction Reflex name={} result={} ptr={}",
             functionName ? functionName : "<null>",
             magic_enum::enum_name(result),
             function != nullptr);

    return result;
}
```

Do not replace `function` here.

Existing plugin-level hooks may still wrap functions through the current normal OptiScaler path.

Bound by distinct function name and changed result if necessary.

## 7.8 `hkreflex_slGetPluginFunction`

Add a diagnostic at the plugin layer so we can see which distinct Reflex functions are requested even if they never execute.

Suggested output:

```text
[ReflexGate] sl.reflex plugin function requested: slOnPluginLoad
[ReflexGate] sl.reflex plugin function requested: slReflexGetState
[ReflexGate] sl.reflex plugin function requested: slReflexSetOptions
```

Do not enable the existing commented generic log without bounding it if it can spam.

Use a small unique-name cache or individual one-time flags.

The implementation must be thread-safe if a shared container is used.

A simple acceptable approach is:

```cpp
static std::mutex logMutex;
static std::unordered_set<std::string> loggedFunctions;

if (shouldLogReflexGate() && functionName)
{
    std::scoped_lock lock(logMutex);
    if (loggedFunctions.emplace(functionName).second)
        LOG_INFO("[ReflexGate] sl.reflex plugin function requested: {}", functionName);
}
```

If adding a container is considered unnecessary overhead, use explicit once-flags for the known Reflex function names instead.

Do not change existing return behavior beyond the already-merged hooks.

---

# 8. Interposer Hook Attachment: Do Not Broaden It Blindly

Current v2 interposer code already obtains these functions:

```cpp
slIsFeatureSupported
slIsFeatureLoaded
slGetFeatureRequirements
slGetFeatureVersion
slGetFeatureFunction
```

and attaches the related detours when:

```cpp
State::Instance().activeFgInput == FGInput::DLSSG
```

The reproduced PRAGMATA and RE9 sessions both have:

```text
FrameGen.FGInput: DLSSG
Streamline version: 2.8.0
Hooking v2
```

Therefore the current attachment condition is sufficient for this first diagnostic pass.

Do **not** broaden the attachment condition in this PR unless implementation review proves the diagnostic hooks are not installed.

Why:

The existing hook functions contain DLSSG-specific behavior that currently relies on being attached only in the DLSSG path. Broadening attachment without auditing those assumptions could unintentionally make the diagnostic PR behavioral.

If broader attachment becomes necessary later, first move the existing DLSSG substitutions behind explicit `activeFgInput == FGInput::DLSSG` guards, then review that as a separate change.

That is outside this PR unless strictly required.

---

# 9. Existing PR #3 Code Must Remain Intact

Do not remove the following existing code in this diagnostic PR:

```cpp
GameQuirk::FixSlReflexAvailabilityOnIntel
hkslReflexGetState(...)
NvAPI_D3D_GetSleepStatus(...) Intel compatibility fallback
```

Reason:

The current runtime does not reach those paths, but retaining them gives us additional evidence if the new diagnostic or a game state change causes the path to become active later.

Do not expand their behavior.

The next runtime log should allow these layers to be ordered relative to the new `[ReflexGate]` lines.

---

# 10. Required Runtime Interpretation Matrix

After building the diagnostic branch, classify each target run using the first matching case below.

## Case A — `slIsFeatureSupported(Reflex)` is called and fails

Example:

```text
[ReflexGate] slIsFeatureSupported Reflex result=eErrorFeatureNotSupported
```

Interpretation:

> The Streamline core capability query is a real upstream gate before `slReflexGetState`.

Next step:

- inspect why the original call fails under SL 2.8.0;
- compare adapter LUID / plugin external config / capability requirements;
- design a separate narrowly-scoped functional PR only after confirming this is the menu gate.

Do not force success in the diagnostic PR.

## Case B — Supported succeeds, but loaded is false / errors

Example:

```text
slIsFeatureSupported Reflex result=eOk
slIsFeatureLoaded Reflex result=eOk loaded=0
```

Interpretation:

> Plugin mapped successfully, but the game's capability flow may reject the loaded/enabled state.

Next step:

Investigate `slIsFeatureLoaded`, feature enable state, plugin-manager external config, and whether Reflex was requested in `featuresToLoad`.

## Case C — Core capability calls all succeed, but no `slReflexGetState` request occurs

Interpretation:

> The menu decision is likely made by game integration logic after capability enumeration but before runtime Reflex state access.

Next step:

Investigate the game's own capability boolean / engine-side NVAPI result used to decide whether the option is created or exposed.

Do not add another Streamline output override.

## Case D — No Reflex core API calls occur at all

Observed sequence resembles:

```text
sl.reflex loaded
[no Reflex core gate calls]
[no plugin Reflex function requests except plugin load]
```

Interpretation:

> Strong evidence that the game decides Reflex availability before entering the normal Streamline Reflex API sequence.

Next step:

Move investigation outward to game-visible GPU / NVAPI capability APIs.

Candidate areas for a later investigation include:

- fake NVAPI APIs queried by the game outside Streamline;
- GPU architecture / vendor / driver capability APIs;
- game-side NVIDIA low-latency availability checks;
- RE Engine title-specific integration differences.

Do not assume which API until call evidence exists.

## Case E — `slReflexGetState` finally appears

If the new run reaches:

```text
Reflex GetState: ...
```

then compare:

```text
original lowLatencyAvailable
GetSleepStatus fallback activation
returned state
subsequent slReflexSetOptions
Reflex IDs
```

This would reactivate the PR #3 hypothesis for that runtime state.

---

# 11. Streamline 2.8.0 Correlation

Record this as a correlation, not a conclusion.

Both failing targets currently use:

```text
Streamline 2.8.0
sl.reflex 2.8.0.b735fd37
```

Previously observed Intel comparison titles with functioning Reflex markers used different Streamline versions:

```text
Dragon's Dogma 2: Streamline 2.9.x family
Monster Hunter Wilds: Streamline 2.7.32 family
```

Do not patch based only on this version difference.

The diagnostic PR should first determine whether SL 2.8's core capability call is actually part of the failing path.

Only after the call sequence is known should we compare specific 2.7 / 2.8 / 2.9 implementation differences.

---

# 12. Logging Volume Requirements

The user-facing field log must remain practical.

Required:

- one `slInit` feature snapshot per Streamline initialization;
- one line for first Reflex core API result;
- additional line only when a result / boolean changes;
- one line per distinct Reflex function name request;
- no per-frame logs;
- no repeated pointer dumps every frame;
- no full plugin JSON dump at INFO.

Expected total `[ReflexGate]` output should normally be in the single digits or low tens, not thousands.

Use pointer presence (`ptr=0/1`) unless the address itself materially helps diagnosis.

---

# 13. Safety Requirements

This diagnostic must preserve current runtime behavior.

Required invariants:

1. Non-target games receive no new `[ReflexGate]` INFO logging from the title-gated diagnostic path.
2. Reflex core API return codes are unchanged.
3. `loaded` values are unchanged.
4. `FeatureRequirements` are unchanged.
5. `FeatureVersion` is unchanged.
6. Reflex function pointers are unchanged by the new core diagnostic layer.
7. Existing DLSSG substitutions remain unchanged.
8. Existing PR #3 fake-NVAPI fallback remains unchanged.
9. Native NVIDIA behavior is not modified.
10. AMD behavior is not modified.
11. No new public config is added.
12. No new permanent executable-specific hack is added beyond using the existing quirk as a diagnostic scope.

---

# 14. Suggested Code Organization

Keep the implementation small and reviewable.

Suggested local helper section near other Streamline hook helpers:

```cpp
static bool shouldLogReflexGate()
{
    return static_cast<bool>(
        State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailabilityOnIntel);
}
```

Optional bounded logger helpers may also remain file-local.

Do not create a new subsystem or diagnostic class for this task.

Do not add new files unless absolutely necessary.

Preferred changed-file count:

```text
1 file preferred
2 files acceptable
```

Likely:

```text
OptiScaler/hooks/Streamline_Hooks.cpp
```

Possibly:

```text
OptiScaler/hooks/Streamline_Hooks.h
```

only if declarations are genuinely required.

---

# 15. Static Validation

Before opening the Draft PR:

```text
git diff --check
```

Run clang-format validation on changed C++ files using the repository's current expected formatter/version.

Build:

```text
Configuration: Release
Platform: x64
Target: OptiScaler.dll / OptiScaler.vcxproj
```

The exact local MSBuild command may follow the repository's existing development environment.

Required result:

```text
Release x64 build: PASS
```

Do not claim runtime success from compilation alone.

---

# 16. Draft PR Requirements

Suggested title:

```text
Diagnose RE9/PRAGMATA Reflex core capability gate
```

Base:

```text
Reflex_RE9_Pragmata
```

Suggested head:

```text
diagnostic/re9-pragmata-reflex-core-gate
```

The PR body must clearly say:

- this is diagnostic-only;
- PR #3 is already merged into the base;
- runtime with PR #3 reached neither `slReflexGetState` nor the GetSleepStatus fallback;
- this PR instruments the earlier Streamline core capability/function path;
- no new Reflex capability value is forced;
- Intel RE9 / PRAGMATA runtime logs are required before merge;
- the PR should remain Draft until those logs are reviewed.

Do not target `master`.

Do not squash/merge the diagnostic PR automatically after build success.

Runtime evidence is the completion criterion.

---

# 17. Required Runtime Tests

## Test A — Intel + PRAGMATA

Use the same general reproduction configuration:

```text
StreamlineSpoofing: enabled
FGInput: DLSSG
FGOutput: XeFG
```

Capture full `OptiScaler.log`.

Required grep targets:

```text
[ReflexGate]
Reflex GetState:
Applying Intel Streamline Reflex availability compatibility fallback
sl.reflex
Reflex Id:
LowLatency algo:
```

Report:

- whether the in-game Reflex menu is visible;
- full `[ReflexGate]` sequence;
- whether GetState occurs;
- whether fallback occurs;
- count of non-zero Reflex IDs.

## Test B — Intel + RE9

Same requirements as Test A.

## Test C — Optional known-good comparison after target evidence

Do **not** expand quirks to DD2 / MHW in the first implementation merely to obtain comparison logs.

If target evidence shows a specific core API failure and a known-good comparison is still needed, prepare a temporary comparison strategy separately after review.

Do not pollute the permanent quirk table just for comparison logging.

---

# 18. Completion Criteria

This task is complete only when all of the following are true:

- [ ] New implementation branch is based on latest `Reflex_RE9_Pragmata`.
- [ ] Only diagnostic observability is added.
- [ ] `slInit` Reflex feature presence is logged.
- [ ] Reflex `slIsFeatureSupported` calls/results are logged if called.
- [ ] Reflex `slIsFeatureLoaded` calls/results are logged if called.
- [ ] Reflex `slGetFeatureRequirements` calls/results are logged if called.
- [ ] Reflex `slGetFeatureVersion` calls/results are logged if called.
- [ ] Reflex core `slGetFeatureFunction` names/results are logged if called.
- [ ] Reflex plugin `slGetPluginFunction` distinct names are logged.
- [ ] Logs are bounded and non-per-frame.
- [ ] No new Reflex availability override is added.
- [ ] Existing PR #3 behavior remains unchanged.
- [ ] Existing DLSSG behavior remains unchanged.
- [ ] `git diff --check` passes.
- [ ] formatting checks pass.
- [ ] Release x64 build passes.
- [ ] Draft PR targets `Reflex_RE9_Pragmata`.
- [ ] PR remains unmerged pending Intel PRAGMATA and RE9 runtime logs.

---

# 19. Stop Conditions

Stop implementation and report instead of adding speculative fixes if any of these occur:

1. The current source no longer matches the assumptions in this work order because `Reflex_RE9_Pragmata` moved significantly after this document was written.
2. Core hook attachment cannot be observed without broadening behavior outside the existing DLSSG path.
3. A required Streamline struct field is version-sensitive or unsafe to inspect.
4. The diagnostic would require changing a Reflex return value to obtain evidence.
5. Build failure is unrelated to the diagnostic changes and cannot be isolated.

A partial but trustworthy diagnostic is preferred over an invasive speculative fix.

---

# 20. Expected Handoff After Runtime

The implementation agent must summarize the two logs in a compact table similar to:

| Gate | PRAGMATA | RE9 |
|---|---|---|
| `slInit` contains Reflex | yes/no | yes/no |
| `slIsFeatureSupported(Reflex)` called | yes/no | yes/no |
| Supported result | result / N/A | result / N/A |
| `slIsFeatureLoaded(Reflex)` called | yes/no | yes/no |
| Loaded value | value / N/A | value / N/A |
| Requirements queried | yes/no | yes/no |
| Version queried | yes/no | yes/no |
| Core Reflex function requested | list / none | list / none |
| Plugin Reflex function requested | list / none | list / none |
| `slReflexGetState` executed | yes/no | yes/no |
| GetSleepStatus fallback executed | yes/no | yes/no |
| non-zero XeFG Reflex IDs | count | count |
| Reflex menu visible | yes/no | yes/no |

Then state exactly one leading diagnosis based on the matrix.

Do not propose the final functional patch until this evidence is reviewed.

---

# 21. Final Instruction

The goal of this PR is not to make the menu appear by any means necessary.

The goal is to find the first real gate.

The required progression is:

```text
observe
  -> identify first failing/skipped gate
  -> review evidence
  -> design minimal fix
  -> runtime validate
  -> only then prepare integration/upstream work
```

Avoid another speculative capability override before the call sequence is known.
