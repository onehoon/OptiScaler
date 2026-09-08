# Work Order: RE9 / PRAGMATA Intel-Only Streamline Reflex Availability Quirk

Date: 2026-09-08  
Repository: `onehoon/OptiScaler`  
Planning / PR base branch: `Reflex_RE9_Pragmata`  
Reviewed source baseline: `dfe80e76d0c0dbe711ba542c8462c0bdb8a2e7e3`  
Suggested implementation branch: `fix/re9-pragmata-reflex-availability-quirk`  
Upstream reference reviewed: `optiscaler/OptiScaler@da70e61e1542a0b99adcb24168ff941e42109567`

---

# 1. Goal

Implement the smallest upstream-friendly compatibility fix for a Reflex availability issue reproduced only on Intel GPUs in:

- Resident Evil Requiem: `re9.exe`
- PRAGMATA: `pragmata.exe`

Observed Intel behavior:

- OptiScaler Streamline spoofing successfully exposes DLSS upscaling and DLSS Frame Generation paths.
- `sl.reflex` loads successfully.
- fake NVAPI and the XeLL low-latency backend initialize successfully.
- the in-game Reflex option remains unavailable/disabled.
- comparison Capcom titles such as Monster Hunter Wilds and Dragon's Dogma 2 do not show the same problem under the same general OptiScaler spoofing approach.

The requested compatibility correction is **Intel-only**.

AMD and NVIDIA do not need this workaround and must remain behaviorally unchanged.

The intended solution is a **game-specific deep-code quirk with an Intel vendor guard**, following existing OptiScaler quirk and Streamline-hook patterns.

Primary invariant:

> The compatibility override may only affect `re9.exe` and `pragmata.exe` when the real primary GPU vendor is Intel.

Secondary invariants:

> AMD must not receive the availability override even when fake NVAPI is in use.

> Native NVIDIA behavior must remain fully native and must never be force-overridden by this quirk.

---

# 2. Scope and Non-Goals

This PR should be intentionally narrow.

Do:

- add one deep-code GameQuirk;
- attach it only to `re9.exe` and `pragmata.exe`;
- intercept `slReflexGetState` using the existing Reflex plugin-function hook path;
- call the original function first;
- only on Intel + fake-NVAPI + Streamline-spoofing + successful original call, expose `lowLatencyAvailable = true`;
- include bounded diagnostic logging in the same PR.

Do not:

- change global fake-NVAPI capability behavior;
- change `NvAPI_D3D_GetSleepStatus` globally;
- alter `LowLatencyCtx` globally;
- alter Reflex behavior for AMD;
- alter native NVIDIA Reflex behavior;
- add a generic user-facing config option;
- add vendor fields or new vendor-specific macros to the global `QuirkEntry` table just for this fix;
- add `re9demo.exe` or `pragmata_sketchbook.exe` without reproduction evidence;
- force `latencyReportAvailable` or any unrelated `ReflexState` member;
- modify Streamline plugin JSON or `slIsFeatureSupported` unless new evidence disproves the current hypothesis.

---

# 3. Why This Should Remain an OptiScaler Game Quirk

OptiScaler already uses title-specific quirks for deeper compatibility behavior.

Relevant existing examples include:

- `GameQuirk::FixSlSimulationMarkers`
- `GameQuirk::HitmanReflexHacks`
- `GameQuirk::PregmataFixDLSSModes`

`HitmanReflexHacks` is direct precedent for game-specific Reflex handling.

`PregmataFixDLSSModes` is the closest structural precedent because it is registered in `Quirks.h` and used inside `Streamline_Hooks.cpp` to replace a specific Streamline plugin function only when the quirk is active.

Follow that model.

Do not hard-code executable names inside `Streamline_Hooks.cpp`.

---

# 4. Vendor Scoping Decision

## 4.1 Current Quirk table is executable-based, not vendor-based

Current `QuirkEntry` contains only:

```cpp
struct QuirkEntry
{
    const char* exeName;
    std::initializer_list<GameQuirk> quirks;
};
```

The existing table therefore selects quirks by executable.

Do **not** redesign the table to add a GPU vendor field for this single fix.

Do **not** add a new macro such as:

```cpp
QUIRK_ENTRY_INTEL(...)
```

That would expand the framework and increase upstream review surface without being necessary.

## 4.2 Existing OptiScaler precedent allows conditional quirk effects

OptiScaler already has quirks whose actual effect depends on runtime conditions. For example, config-level quirks such as restore-compute-signature behavior are conditionally applied according to detected GPU state.

For this new deep-code quirk, keep executable registration in `Quirks.h` and apply the vendor restriction at the actual Streamline compatibility boundary.

This yields the intended logical condition:

```text
(re9.exe OR pragmata.exe)
AND primary GPU vendor == Intel
AND StreamlineSpoofing == enabled
AND fake NVAPI is the main NVAPI
AND original slReflexGetState == eOk
```

Only then may OptiScaler change:

```cpp
state.lowLatencyAvailable
```

to `true`.

## 4.3 Prefer an Intel-specific quirk name

Because the workaround is explicitly not required on AMD or NVIDIA, use a vendor-scoped name so the intent is visible in the table and in upstream review.

Preferred name:

```cpp
GameQuirk::FixSlReflexAvailabilityOnIntel
```

This is preferred over the earlier generic name `FixSlReflexAvailability`.

If upstream maintainers explicitly request the shorter generic name during review, the implementation must still retain the Intel vendor guard.

---

# 5. Latest Source Audit

The relevant current code was rechecked before this document update.

## 5.1 Real GPU vendor information is already available

`GpuInformation` contains:

```cpp
VendorId::Value vendorId = VendorId::Invalid;
```

and OptiScaler defines:

```cpp
namespace VendorId
{
enum Value : uint32_t
{
    Invalid = 0,
    Microsoft = 0x1414,
    Nvidia = 0x10DE,
    AMD = 0x1002,
    Intel = 0x8086,
};
}
```

The real primary GPU can be obtained through:

```cpp
IdentifyGpu::getPrimaryGpu()
```

`IdentifyGpu` performs its own unspoofed adapter discovery and keeps cached GPU information, so this is the appropriate existing source for the actual vendor check.

Do not use a spoofed Streamline adapter vendor to decide whether this workaround is Intel-specific.

## 5.2 Existing Capcom quirks

Current `OptiScaler/misc/Quirks.h` contains entries including:

```cpp
QUIRK_ENTRY("re9.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia),

QUIRK_ENTRY("re9demo.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia),

QUIRK_ENTRY("pragmata.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::PregmataFixDLSSModes),

QUIRK_ENTRY("pragmata_sketchbook.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::AllowedFrameAhead2,
            GameQuirk::PregmataFixDLSSModes),
```

Add the new quirk only to the two main executables with reproduced evidence:

```text
re9.exe
pragmata.exe
```

## 5.3 Reflex plugin-load spoof already exists

Current `StreamlineHooks::hkreflex_slOnPluginLoad()` already performs temporary Reflex SystemCaps spoofing when Streamline spoofing is enabled:

```cpp
uint32_t currentArch = 0;
if (Config::Instance()->StreamlineSpoofing.value_or_default())
{
    hookSystemCaps(params);
    currentArch = getSystemCapsArch();
    spoofArch(currentArch, sl::kFeatureReflex);
}

auto result = o_reflex_slOnPluginLoad(params, loaderJSON, pluginJSON);

if (Config::Instance()->StreamlineSpoofing.value_or_default())
    setArch(currentArch);
```

Current `spoofArch()` already contains Reflex/PCL handling:

```cpp
else if (feature == sl::kFeatureReflex || feature == sl::kFeaturePCL)
{
    if (fakenvapi::isUsingAsMainNvapi())
        return setArch(maxArch, altSystemCaps);
}
```

Do not duplicate or redesign this stage.

## 5.4 Current Reflex plugin-function hook still does not intercept `slReflexGetState`

Current `hkreflex_slGetPluginFunction()` intercepts:

- SL1 `slSetConstants`;
- `slOnPluginLoad`;
- `slReflexSetOptions`;
- `slReflexSleep`;
- conditional `slReflexSetMarker`.

It does not currently intercept:

```text
slReflexGetState
```

This remains the narrow missing interception point.

## 5.5 Current fake NVAPI already exposes GetSleepStatus

Current fake NVAPI already exposes:

```cpp
INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_GetSleepStatus)
```

and routes it through:

```cpp
NvAPI_Status __cdecl NvAPI_D3D_GetSleepStatus(IUnknown* pDevice, NV_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams)
{
    if (!pDevice || !pGetSleepStatusParams)
        return ERROR_VALUE(NVAPI_INVALID_ARGUMENT);

    return LowLatencyCtx::get()->GetSleepStatus(pDevice, pGetSleepStatusParams);
}
```

Therefore this task must not become a broad fake-NVAPI rewrite.

---

# 6. Why `slReflexGetState` Is the Correct Compatibility Boundary

Streamline exposes runtime Reflex support through `sl::ReflexState`:

```cpp
bool lowLatencyAvailable = false;
bool latencyReportAvailable = false;
```

The application obtains that state through:

```cpp
slReflexGetState(sl::ReflexState& state)
```

NVIDIA Streamline's Reflex runtime logic rechecks backend support before returning the state:

```cpp
if (ctx.compute && ctx.lowLatencyAvailable)
{
    ctx.lowLatencyAvailable = ctx.compute->getSleepStatus(*settings) == chi::ComputeStatus::eOk;
    ctx.latencyReportAvailable = ctx.compute->getLatencyReport(*settings) == chi::ComputeStatus::eOk;
}

settings->lowLatencyAvailable = ctx.lowLatencyAvailable;
settings->latencyReportAvailable = ctx.latencyReportAvailable;
```

This creates two relevant stages:

1. plugin-load/SystemCaps capability;
2. runtime availability returned to the game.

OptiScaler already successfully passes stage 1 in the affected titles.

The leading hypothesis is that RE9 and PRAGMATA gate their UI/activation more strictly on stage 2.

---

# 7. Existing Runtime Evidence

The original reproduction logs were captured with OptiScaler `v0.9.5-pre4` at commit `8dac650`, not the current branch.

Preserve this distinction in the PR description.

For Intel in RE9 / PRAGMATA the logs showed:

- fake NVAPI initialized;
- Reflex hooks initialized;
- `sl.reflex` loaded successfully;
- valid Reflex plugin adapter mask;
- XeLL context later initialized successfully;
- XeLL latency reduction enabled;
- nevertheless XeFG dispatches stayed at `Reflex Id: 0`.

Observed reproduction counts:

```text
PRAGMATA: 15,336 / 15,336 dispatches had Reflex Id = 0
RE9:       3,945 / 3,945 dispatches had Reflex Id = 0
```

Comparison logs from the same OptiScaler build:

```text
Dragon's Dogma 2:      7,376 observed dispatches, all non-zero Reflex IDs
Monster Hunter Wilds:  5,229 observed dispatches, all non-zero Reflex IDs
```

This supports the runtime-state-gating hypothesis, but does not yet directly prove the exact `slReflexGetState` return value on the current branch.

For that reason the diagnostic log and fix should ship together in the test PR.

---

# 8. Required Production Files

Expected production-code diff:

```text
OptiScaler/misc/Quirks.h
OptiScaler/dllmain.cpp
OptiScaler/hooks/Streamline_Hooks.h
OptiScaler/hooks/Streamline_Hooks.cpp
```

Do not modify unless new evidence requires it:

```text
OptiScaler/nvapi/fakenvapi.cpp
OptiScaler/nvapi/fakenvapi/nvapi_calls.cpp
OptiScaler/hooks/Reflex_Hooks.cpp
Config files / public config schema
```

---

# 9. Change 1 — Add the Intel-Specific Deep-Code Quirk

File:

```text
OptiScaler/misc/Quirks.h
```

Do not reorder existing enum values.

Append immediately before the terminal `_` entry:

```cpp
    CreateSLOnThe2ndDevice,
    FixSlReflexAvailabilityOnIntel,
    // Don't forget to add the new entry to printQuirks
    _
```

Then extend only the two reproduced main-game entries.

Recommended result:

```cpp
QUIRK_ENTRY("re9.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::FixSlReflexAvailabilityOnIntel),
```

```cpp
QUIRK_ENTRY("pragmata.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::PregmataFixDLSSModes,
            GameQuirk::FixSlReflexAvailabilityOnIntel),
```

Do not add the new quirk to:

```text
re9demo.exe
pragmata_sketchbook.exe
dd2.exe
monsterhunterwilds.exe
```

unless separately reproduced.

---

# 10. Change 2 — Add `printQuirks()` Reporting

File:

```text
OptiScaler/dllmain.cpp
```

Follow the existing explicit `if` style.

Suggested entry:

```cpp
if (quirks & GameQuirk::FixSlReflexAvailabilityOnIntel)
    stringQuirks.push_back("Fix Streamline Reflex availability on Intel");
```

Do not introduce a generic enum-to-string refactor.

Note:

Because the current global quirk table is executable-based, this message may be listed for the executable even when the actual runtime vendor guard later declines the override on AMD/NVIDIA. That is acceptable for this narrow PR as long as the runtime effect is strictly vendor-gated.

Do not redesign `CheckQuirks()` or `QuirkEntry` solely to make this printout vendor-aware.

---

# 11. Change 3 — Add the Reflex GetState Hook Declaration

File:

```text
OptiScaler/hooks/Streamline_Hooks.h
```

In the existing Reflex section add:

```cpp
inline static decltype(&slReflexGetState) o_slReflexGetState = nullptr;
```

Recommended placement:

```cpp
inline static PFN_slOnPluginLoad o_reflex_slOnPluginLoad = nullptr;
inline static decltype(&slReflexSetOptions) o_slReflexSetOptions = nullptr;
inline static decltype(&slReflexGetState) o_slReflexGetState = nullptr;
inline static decltype(&slReflexSleep) o_slReflexSleep = nullptr;
```

Add:

```cpp
static sl::Result hkslReflexGetState(sl::ReflexState& state);
```

and add signature validation using the existing style:

```cpp
VALIDATE_MEMBER_HOOK(hkslReflexGetState, decltype(&slReflexGetState))
```

Do not create a custom typedef unless needed by compilation.

---

# 12. Change 4 — Intercept `slReflexGetState` Only for the Game Quirk

File:

```text
OptiScaler/hooks/Streamline_Hooks.cpp
```

Inside `hkreflex_slGetPluginFunction()` add the new function interception near the existing Reflex functions.

Preferred structure:

```cpp
if (strcmp(functionName, "slReflexGetState") == 0 &&
    State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailabilityOnIntel)
{
    o_slReflexGetState = (decltype(&slReflexGetState)) o_reflex_slGetPluginFunction(functionName);

    if (o_slReflexGetState != nullptr)
        return &hkslReflexGetState;

    return nullptr;
}
```

Keep the hook selection driven by the existing GameQuirk bit.

Do not hard-code executable names here.

Do not make this a global Reflex GetState hook for every game unless required by an existing code-style constraint.

---

# 13. Change 5 — Intel-Only `hkslReflexGetState`

Implement the hook close to the existing `hkslReflexSetOptions()` / `hkslReflexSleep()` functions.

The original call must always run first.

Recommended implementation shape:

```cpp
sl::Result StreamlineHooks::hkslReflexGetState(sl::ReflexState& state)
{
    const auto result = o_slReflexGetState(state);
    const bool originalLowLatencyAvailable = state.lowLatencyAvailable;

    bool shouldOverride =
        result == sl::Result::eOk &&
        (State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailabilityOnIntel) &&
        Config::Instance()->StreamlineSpoofing.value_or_default() &&
        fakenvapi::isUsingAsMainNvapi();

    VendorId::Value vendorId = VendorId::Invalid;

    if (shouldOverride)
    {
        const auto primaryGpu = IdentifyGpu::getPrimaryGpu();
        vendorId = primaryGpu.vendorId;

        if (vendorId == VendorId::Intel)
            state.lowLatencyAvailable = true;
    }

    // Bounded / state-change diagnostics here.

    return result;
}
```

The exact local-variable arrangement may be adjusted to match surrounding style, but all semantic guards are mandatory.

## Mandatory guard order

Prefer cheap guards before GPU lookup:

1. original call returned `sl::Result::eOk`;
2. game quirk is active;
3. `StreamlineSpoofing` is enabled;
4. fake NVAPI is the main NVAPI;
5. actual primary GPU vendor is `VendorId::Intel`.

Only after all five conditions are satisfied may the code force:

```cpp
state.lowLatencyAvailable = true;
```

## Vendor failure behavior

If GPU discovery returns:

```cpp
VendorId::Invalid
```

or any vendor other than Intel, do not override.

Fail closed.

Do not treat `Invalid` as Intel and do not cache an early `Invalid` result permanently.

If an implementation chooses to cache the vendor check for efficiency, it must only cache after obtaining a valid non-`Invalid` vendor result.

A simple uncached lookup after the cheap guards is acceptable for the initial patch and is preferable to a fragile early cache.

---

# 14. Do Not Gate on `isLowLatencyActive()`

Do not add:

```cpp
fakenvapi::isLowLatencyActive()
```

as a prerequisite for the availability override.

The affected game may query capability before the user is able to enable Reflex.

That creates a circular dependency:

```text
UI needs lowLatencyAvailable == true
    -> user can enable Reflex
    -> low-latency mode becomes active
```

Requiring low latency to already be active would defeat the purpose of the compatibility fix.

---

# 15. Do Not Modify Other `ReflexState` Fields

Only change:

```cpp
state.lowLatencyAvailable
```

Do not force:

```cpp
state.latencyReportAvailable
state.flashIndicatorDriverControlled
state.statsWindowMessage
frame reports
```

Preserve every other value returned by the original Streamline function.

If the original call returns anything other than `sl::Result::eOk`, return it unchanged and do not force availability.

---

# 16. Diagnostic Logging Requirements

The diagnostic and fix stay in the same PR.

We need to confirm:

- the hook is actually requested by the target game;
- the original current-branch `lowLatencyAvailable` value;
- the detected real GPU vendor;
- whether the Intel-only override was applied;
- whether Reflex markers/IDs become active after the user enables Reflex.

Do not log every `slReflexGetState` call.

Use a once-only or state-change pattern consistent with the existing logger style.

Useful diagnostic content:

```text
Reflex GetState: result=eOk, vendor=Intel, original lowLatencyAvailable=false, returned=true, quirk=true
```

For AMD/NVIDIA negative tests, a single debug-level state record is enough if useful:

```text
Reflex GetState: vendor=AMD, Intel availability quirk not applied
Reflex GetState: vendor=Nvidia, Intel availability quirk not applied
```

Do not produce per-frame INFO spam.

Suggested behavior:

- first relevant call: DEBUG or INFO once;
- override transition: INFO once;
- subsequent identical calls: no log.

---

# 17. Required Runtime Behavior Matrix

## 17.1 Intel + RE9

Conditions:

```text
exe = re9.exe
real primary GPU = Intel
StreamlineSpoofing = true
fake NVAPI = main NVAPI
```

Expected:

```text
FixSlReflexAvailabilityOnIntel quirk present
slReflexGetState intercepted
original result logged
original lowLatencyAvailable observed
returned lowLatencyAvailable = true when original result == eOk
Reflex menu becomes selectable
when user enables Reflex, normal marker path becomes active
XeFG Reflex Id should become non-zero/increasing rather than remaining 0
```

## 17.2 Intel + PRAGMATA

Same expectations as RE9.

## 17.3 AMD + RE9 / PRAGMATA

Expected:

```text
quirk may be present because selection is executable-based
slReflexGetState may be intercepted
VendorId::AMD guard fails
state.lowLatencyAvailable remains exactly what original Streamline returned
no forced Reflex availability
```

This is a mandatory negative test.

## 17.4 Native NVIDIA + RE9 / PRAGMATA

Expected:

```text
VendorId::Nvidia guard fails
native Streamline result passes through unchanged
no forced state
no fake-NVAPI-specific behavior introduced
```

This is a mandatory negative test.

## 17.5 Intel + DD2 / MHW

Expected:

```text
new quirk absent
new GetState compatibility path not selected
existing working Reflex behavior unchanged
```

## 17.6 Original GetState returns error

Expected:

```text
result != eOk
no override regardless of vendor
original error returned unchanged
```

## 17.7 GPU vendor unavailable

Expected:

```text
VendorId::Invalid
no override
retry naturally on later calls if the implementation does not permanently cache Invalid
```

---

# 18. Build / Static Verification

Before runtime testing:

1. build the normal project configuration used by upstream development;
2. confirm no new warnings from the changed files;
3. verify `VALIDATE_MEMBER_HOOK` accepts the new function signature;
4. search the final diff for accidental global behavior changes;
5. confirm no changes under fake NVAPI / LowLatencyCtx;
6. confirm the new quirk appears only on `re9.exe` and `pragmata.exe`;
7. confirm `VendorId::Intel` is an explicit runtime requirement for the override;
8. confirm AMD/NVIDIA paths preserve the original state.

Useful searches:

```text
FixSlReflexAvailabilityOnIntel
slReflexGetState
lowLatencyAvailable
VendorId::Intel
```

---

# 19. PR Structure

Create the implementation branch from:

```text
Reflex_RE9_Pragmata
```

Suggested head:

```text
fix/re9-pragmata-reflex-availability-quirk
```

Open a **Draft PR** with:

```text
base: Reflex_RE9_Pragmata
head: fix/re9-pragmata-reflex-availability-quirk
```

Do not target `master` yet.

Do not merge before Intel runtime validation.

Keep diagnostics and the compatibility correction in the same PR so the first test build can both prove the hypothesis and validate the fix.

---

# 20. PR Description Requirements

The PR description should clearly distinguish old reproduction evidence from the current patch.

Recommended explanation:

```text
The original reproduction logs came from OptiScaler 0.9.5-pre4 (8dac650).
Both RE9 and PRAGMATA loaded sl.reflex and initialized the low-latency backend, but all observed XeFG dispatches retained Reflex Id 0. Comparison logs from MHW and DD2 showed non-zero Reflex IDs.

Current OptiScaler already implements fake-NVAPI NvAPI_D3D_GetSleepStatus and Reflex plugin-load SystemCaps spoofing, but does not intercept the runtime slReflexGetState value returned to the game.

This patch adds a game quirk for re9.exe and pragmata.exe and adjusts only lowLatencyAvailable at the Streamline GetState boundary. The override is additionally restricted to real Intel primary GPUs, Streamline spoofing, fake NVAPI as the main NVAPI, and successful original GetState calls. AMD and native NVIDIA retain the original Streamline result unchanged.
```

Do not claim the hypothesis is proven until the new diagnostic log captures the original current-branch state.

---

# 21. Acceptance Criteria

The task is complete only when all of the following are true:

- [ ] New deep-code quirk added without reordering existing enum entries.
- [ ] Preferred quirk name is `FixSlReflexAvailabilityOnIntel`.
- [ ] Quirk registered only for `re9.exe` and `pragmata.exe`.
- [ ] `printQuirks()` updated using existing style.
- [ ] `slReflexGetState` original pointer and hook declaration added.
- [ ] `VALIDATE_MEMBER_HOOK` added.
- [ ] `hkreflex_slGetPluginFunction()` intercepts GetState only through the game quirk path.
- [ ] Original `slReflexGetState()` always runs first.
- [ ] Override requires `result == eOk`.
- [ ] Override requires `StreamlineSpoofing` enabled.
- [ ] Override requires fake NVAPI as main NVAPI.
- [ ] Override requires actual `IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Intel`.
- [ ] `VendorId::Invalid` fails closed.
- [ ] AMD receives no forced availability.
- [ ] NVIDIA receives no forced availability.
- [ ] Only `lowLatencyAvailable` is modified.
- [ ] Diagnostic logging is bounded and not per-frame spam.
- [ ] No global fake-NVAPI / LowLatencyCtx behavior changes.
- [ ] RE9 Intel runtime test performed.
- [ ] PRAGMATA Intel runtime test performed.
- [ ] At least one non-Intel negative-path verification performed if hardware/testing access permits.
- [ ] Draft PR targets `Reflex_RE9_Pragmata`.
- [ ] Do not merge before runtime results are reviewed.

---

# 22. Stop Conditions

Stop and report rather than broadening the patch if any of the following occurs:

- the target game never requests `slReflexGetState`;
- current-branch original `lowLatencyAvailable` is already consistently true before the fix;
- the Intel vendor cannot be determined reliably at the hook point;
- forcing only `lowLatencyAvailable` does not unlock the game option;
- the option unlocks but enabling it still produces no Reflex markers/IDs;
- the override causes errors inside Streamline or XeLL;
- fixing the issue appears to require changing fake NVAPI globally.

In those cases, preserve the diagnostic evidence and return for design review rather than expanding scope automatically.

---

# 23. Final Design Summary

The desired upstream-friendly implementation is:

```text
Quirks.h
    re9.exe / pragmata.exe
        -> FixSlReflexAvailabilityOnIntel

Streamline_Hooks.cpp
    game asks for slReflexGetState
        -> quirk active?
            no  -> original function
            yes -> return narrow hook

hkslReflexGetState
    -> call original first
    -> require eOk
    -> require StreamlineSpoofing
    -> require fake NVAPI main
    -> inspect real primary GPU via IdentifyGpu
    -> require VendorId::Intel
    -> set only lowLatencyAvailable = true
    -> preserve all other state
    -> bounded diagnostic log
```

This keeps the workaround restricted by both **game** and **real GPU vendor**, avoids changing global fake-NVAPI semantics, and mirrors OptiScaler's existing quirk-driven compatibility style as closely as practical.