# Work Order: RE9 / PRAGMATA Streamline Reflex Availability Quirk

Date: 2026-09-08  
Repository: `onehoon/OptiScaler`  
Planning / PR base branch: `Reflex_RE9_Pragmata`  
Base SHA at planning time: `dfe80e76d0c0dbe711ba542c8462c0bdb8a2e7e3`  
Suggested implementation branch: `fix/re9-pragmata-reflex-availability-quirk`  
Upstream reference: `optiscaler/OptiScaler@da70e61e1542a0b99adcb24168ff941e42109567`

---

# 1. Goal

Implement the smallest upstream-friendly compatibility fix for the following issue:

- On Intel GPUs, OptiScaler Streamline spoofing successfully unlocks DLSS upscaling and DLSS Frame Generation paths in **Resident Evil Requiem (`re9.exe`)** and **PRAGMATA (`pragmata.exe`)**.
- The Streamline Reflex plugin also loads successfully.
- However, the in-game Reflex option remains unavailable/disabled in these two titles.
- The same general OptiScaler spoofing approach can expose Reflex correctly in other Capcom titles such as Monster Hunter Wilds and Dragon's Dogma 2.

The intended change is **not** a global fake-NVAPI behavior change.

The intended change is a **game-specific deep-code quirk**, following existing OptiScaler conventions, which adjusts only the runtime Streamline Reflex availability state returned to the affected games.

Primary design invariant:

> Outside `re9.exe` and `pragmata.exe`, behavior must remain unchanged.

Secondary invariant:

> Even inside those games, native NVIDIA behavior must not be force-overridden. The compatibility correction must only activate for OptiScaler's fake-NVAPI + Streamline-spoof scenario after the original Streamline call succeeds.

---

# 2. Why This Must Be Implemented as an OptiScaler Quirk

OptiScaler already has a mature game-quirk mechanism for exactly this class of title-specific compatibility behavior.

Relevant existing examples include:

- `GameQuirk::FixSlSimulationMarkers`
- `GameQuirk::HitmanReflexHacks`
- `GameQuirk::PregmataFixDLSSModes`

`HitmanReflexHacks` is direct precedent for title-specific Reflex behavior changes.

`PregmataFixDLSSModes` is even closer structural precedent: the quirk is registered for selected Capcom executables and then used inside `Streamline_Hooks.cpp` to replace a specific Streamline plugin function only when the quirk is active.

Therefore this work must follow that pattern rather than introduce:

- hard-coded `re9.exe` / `pragmata.exe` checks inside Streamline hooks;
- a new generic configuration option;
- a global fake-NVAPI capability override;
- a global `slReflexGetState` behavior change for every game.

This is important both technically and for upstream reviewability.

---

# 3. Latest Source Audit

This work order was revalidated against the current `Reflex_RE9_Pragmata` source before implementation.

For the relevant files, the fork currently matches the present upstream source blobs used for this review.

## 3.1 Existing Capcom quirks

Current `OptiScaler/misc/Quirks.h` contains, among others:

```cpp
QUIRK_ENTRY("dd2.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::DisableHudfix, GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::PregmataFixDLSSModes),

QUIRK_ENTRY("pragmata_sketchbook.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::AllowedFrameAhead2, GameQuirk::PregmataFixDLSSModes),

QUIRK_ENTRY("re9.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia),

QUIRK_ENTRY("re9demo.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia),

QUIRK_ENTRY("pragmata.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::PregmataFixDLSSModes),
```

The new Reflex compatibility quirk should initially be added to only:

```text
re9.exe
pragmata.exe
```

Do **not** include `re9demo.exe` or `pragmata_sketchbook.exe` in the first implementation unless the same failure is reproduced and documented there.

This keeps the initial patch evidence-based and minimizes upstream blast radius.

## 3.2 Existing Streamline Reflex plugin-load spoofing is already present

Current `StreamlineHooks::hkreflex_slOnPluginLoad()` already performs temporary SystemCaps spoofing when Streamline spoofing is enabled:

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

The current `spoofArch()` path also already contains Reflex handling:

```cpp
else if (feature == sl::kFeatureReflex || feature == sl::kFeaturePCL)
{
    if (fakenvapi::isUsingAsMainNvapi())
        return setArch(maxArch, altSystemCaps);
}
```

Therefore this PR must **not** duplicate or redesign plugin-load capability spoofing.

## 3.3 The current Reflex plugin-function hook is missing `slReflexGetState`

Current `hkreflex_slGetPluginFunction()` intercepts:

- Streamline 1 `slSetConstants`;
- `slOnPluginLoad`;
- `slReflexSetOptions`;
- `slReflexSleep`;
- conditionally `slReflexSetMarker`.

It does not intercept `slReflexGetState`.

Current relevant structure:

```cpp
void* StreamlineHooks::hkreflex_slGetPluginFunction(const char* functionName)
{
    if (strcmp(functionName, "slSetConstants") == 0 && State::Instance().streamlineVersion.major == 1)
    {
        o_reflex_slSetConstants_sl1 = (PFN_slSetConstants_sl1) o_reflex_slGetPluginFunction(functionName);
        return &hkreflex_slSetConstants_sl1;
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_reflex_slOnPluginLoad = (PFN_slOnPluginLoad) o_reflex_slGetPluginFunction(functionName);
        return &hkreflex_slOnPluginLoad;
    }

    if (strcmp(functionName, "slReflexSetOptions") == 0)
    {
        o_slReflexSetOptions = (decltype(&slReflexSetOptions)) o_reflex_slGetPluginFunction(functionName);
        return &hkslReflexSetOptions;
    }

    if (strcmp(functionName, "slReflexSleep") == 0)
    {
        o_slReflexSleep = (decltype(&slReflexSleep)) o_reflex_slGetPluginFunction(functionName);
        return &hkslReflexSleep;
    }

    if (strcmp(functionName, "slReflexSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        o_slPCLSetMarker = (decltype(&slPCLSetMarker)) o_reflex_slGetPluginFunction(functionName);
        return &hkslPCLSetMarker;
    }

    return o_reflex_slGetPluginFunction(functionName);
}
```

This is the narrow missing interception point for this experiment/fix.

## 3.4 Current fake NVAPI already implements the relevant low-latency calls

Do not implement a broad fake-NVAPI patch for this task.

Current `fakenvapi::queryInterface()` already exposes:

```cpp
INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_GetSleepStatus)
INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_GetLatency)
INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_SetSleepMode)
INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_SetLatencyMarker)
INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_Sleep)
```

Current `nvapi_calls::NvAPI_D3D_GetSleepStatus()` already routes to the low-latency context:

```cpp
NvAPI_Status __cdecl NvAPI_D3D_GetSleepStatus(IUnknown* pDevice, NV_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams)
{
    if (!pDevice || !pGetSleepStatusParams)
        return ERROR_VALUE(NVAPI_INVALID_ARGUMENT);

    return LowLatencyCtx::get()->GetSleepStatus(pDevice, pGetSleepStatusParams);
}
```

This confirms that the current codebase already has a real fake-NVAPI GetSleepStatus implementation.

The proposed quirk should therefore remain above that layer, at the Streamline state returned to the game.

---

# 4. Why `slReflexGetState` Is the Correct Compatibility Boundary

NVIDIA Streamline's `ReflexState` exposes:

```cpp
bool lowLatencyAvailable = false;
bool latencyReportAvailable = false;
```

and `slReflexGetState` returns that structure to the application.

Current NVIDIA Streamline Reflex runtime logic rechecks low-latency availability using the compute backend:

```cpp
if (ctx.compute && ctx.lowLatencyAvailable)
{
    ctx.lowLatencyAvailable = ctx.compute->getSleepStatus(*settings) == chi::ComputeStatus::eOk;
    ctx.latencyReportAvailable = ctx.compute->getLatencyReport(*settings) == chi::ComputeStatus::eOk;
}

settings->lowLatencyAvailable = ctx.lowLatencyAvailable;
settings->latencyReportAvailable = ctx.latencyReportAvailable;
```

This means Reflex capability has at least two observable stages:

1. plugin-load / SystemCaps support;
2. runtime `slReflexGetState()` availability returned to the game.

OptiScaler already spoofs stage 1 successfully in the affected titles.

The missing game-specific compatibility point is therefore stage 2.

---

# 5. Existing Runtime Evidence

The original reproduction logs were captured with OptiScaler `v0.9.5-pre4` at commit `8dac650`, not with the current branch.

That distinction must be preserved in the PR description.

The old logs establish the following behavior:

## RE9 / PRAGMATA

- fake NVAPI initialized;
- Reflex hooks initialized;
- Streamline `sl.reflex` loaded successfully;
- the Reflex plugin received a valid adapter mask;
- XeLL context initialization later succeeded and low-latency reduction was enabled;
- however, every observed XeFG dispatch retained `Reflex Id: 0`.

Observed dispatch counts from the reproduction logs:

```text
PRAGMATA: 15,336 / 15,336 dispatches had Reflex Id = 0
RE9:       3,945 / 3,945 dispatches had Reflex Id = 0
```

Comparison logs using the same OptiScaler build showed active Reflex frame IDs:

```text
Dragon's Dogma 2: 7,376 observed dispatches, all with non-zero Reflex IDs
Monster Hunter Wilds: 5,229 observed dispatches, all with non-zero Reflex IDs
```

This strongly suggests that RE9 and PRAGMATA never enter the normal active Reflex marker path even though the Reflex plugin itself loads.

The leading hypothesis is:

> These games gate their Reflex UI / activation on the runtime `slReflexGetState().lowLatencyAvailable` value more strictly than the comparison titles.

This is still a hypothesis until the new hook logs the original current-branch value.

Therefore **diagnostic logging and the compatibility override belong in the same PR**.

---

# 6. Required Design

Add one new deep-code quirk:

```cpp
GameQuirk::FixSlReflexAvailability
```

The name intentionally follows existing style such as:

```text
FixSlSimulationMarkers
PregmataFixDLSSModes
HitmanReflexHacks
```

Do not add a game name to the hook implementation itself.

The quirk is the game-selection mechanism.

## 6.1 Important enum stability rule

Do not reorder existing `GameQuirk` entries.

Append the new value immediately before the terminal `_` entry so existing enum ordinal/flag positions are not shifted unnecessarily.

Preferred pattern:

```cpp
    CreateSLOnThe2ndDevice,
    FixSlReflexAvailability,
    // Don't forget to add the new entry to printQuirks
    _
```

If the comment placement needs adjusting for clarity, keep the semantic rule: **append; do not reorder existing quirks**.

---

# 7. Required File Changes

The expected production-code diff should be limited to:

```text
OptiScaler/misc/Quirks.h
OptiScaler/dllmain.cpp
OptiScaler/hooks/Streamline_Hooks.h
OptiScaler/hooks/Streamline_Hooks.cpp
```

Do not modify `fakenvapi.cpp`, `nvapi_calls.cpp`, `Reflex_Hooks.cpp`, or general configuration unless new runtime evidence proves this design incorrect.

---

# 8. Change 1 — Add the New Quirk

File:

```text
OptiScaler/misc/Quirks.h
```

Append the enum value near the end:

```cpp
    CreateSLOnThe2ndDevice,
    FixSlReflexAvailability,
    // Don't forget to add the new entry to printQuirks
    _
```

Then extend only the two reproduced main-game entries.

Recommended final entries:

```cpp
QUIRK_ENTRY("re9.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::FixSlReflexAvailability),
```

and:

```cpp
QUIRK_ENTRY("pragmata.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::PregmataFixDLSSModes,
            GameQuirk::FixSlReflexAvailability),
```

Do not add the new quirk to:

```text
re9demo.exe
pragmata_sketchbook.exe
dd2.exe
monsterhunterwilds.exe
```

in this first patch.

---

# 9. Change 2 — Add `printQuirks` Reporting

File:

```text
OptiScaler/dllmain.cpp
```

The existing quirk enum explicitly requires each new quirk to be added to `printQuirks()`.

Follow the current explicit-if style.

Suggested entry:

```cpp
if (quirks & GameQuirk::FixSlReflexAvailability)
    stringQuirks.push_back("Fix Streamline Reflex availability");
```

Keep the message short and implementation-oriented.

Do not introduce a new generic enum-to-string system as part of this PR.

---

# 10. Change 3 — Add the Reflex GetState Hook Declaration

File:

```text
OptiScaler/hooks/Streamline_Hooks.h
```

Inside the existing Reflex section, add the original function pointer:

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

Add the hook declaration:

```cpp
static sl::Result hkslReflexGetState(sl::ReflexState& state);
```

Recommended placement next to `hkslReflexSetOptions` / `hkslReflexSleep`.

Also add signature validation:

```cpp
VALIDATE_MEMBER_HOOK(hkslReflexGetState, decltype(&slReflexGetState))
```

Follow the existing validation block style exactly.

Do not add `pch.h` to the header.

---

# 11. Change 4 — Hook `slReflexGetState` Only for the Quirk

File:

```text
OptiScaler/hooks/Streamline_Hooks.cpp
```

Follow the same approach currently used by `PregmataFixDLSSModes`: only return the replacement function when the game quirk is active.

Add this block to `hkreflex_slGetPluginFunction()` near the other Reflex function checks:

```cpp
if (strcmp(functionName, "slReflexGetState") == 0 &&
    (State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailability))
{
    o_slReflexGetState = (decltype(&slReflexGetState)) o_reflex_slGetPluginFunction(functionName);
    return &hkslReflexGetState;
}
```

This is intentionally preferable to globally hooking `slReflexGetState` for every title and then checking the quirk inside every call.

For non-quirked games, existing behavior should remain:

```cpp
return o_reflex_slGetPluginFunction(functionName);
```

The normal upstream function pointer should therefore be returned directly for MHW, DD2, and all unrelated games.

Do not add executable-name checks here.

---

# 12. Change 5 — Implement the Minimal GetState Compatibility Shim

Add the wrapper near the existing Reflex wrappers:

```cpp
sl::Result StreamlineHooks::hkslReflexGetState(sl::ReflexState& state)
{
    auto result = o_slReflexGetState(state);
    const bool originalAvailable = state.lowLatencyAvailable;

    const bool shouldFixAvailability =
        result == sl::Result::eOk &&
        State::Instance().streamlineVersion.major > 1 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailability) &&
        Config::Instance()->StreamlineSpoofing.value_or_default() &&
        fakenvapi::isUsingAsMainNvapi();

    if (shouldFixAvailability)
        state.lowLatencyAvailable = true;

    // Add low-volume diagnostic logging here; see the logging section below.

    return result;
}
```

The exact formatting may be adjusted by clang-format.

## 12.1 Required guards

The override must require all of the following:

```text
original slReflexGetState result == sl::Result::eOk
Streamline version is SL2+
FixSlReflexAvailability quirk is active
StreamlineSpoofing is enabled
fake NVAPI is being used as the main NVAPI
```

These conditions are intentionally redundant with the function-selection quirk check in some places.

The inner guards document and enforce the exact compatibility boundary.

## 12.2 Why `isLowLatencyActive()` must NOT be a guard

Do not use:

```cpp
fakenvapi::isLowLatencyActive()
```

as a prerequisite for returning availability.

The game may call `slReflexGetState()` specifically to decide whether it should expose the UI that lets the user enable Reflex.

Requiring low latency to already be active would create a circular dependency:

```text
Reflex UI disabled
 -> user cannot enable Reflex
 -> low latency never becomes active
 -> availability guard remains false
 -> Reflex UI stays disabled
```

This would defeat the purpose of the compatibility fix.

## 12.3 Do not change any other ReflexState fields

Only change:

```cpp
state.lowLatencyAvailable
```

Do not force or synthesize:

```text
latencyReportAvailable
flashIndicatorDriverControlled
statsWindowMessage
frameReport[]
frameReport2[]
```

The quirk exists only to correct the game's capability gate.

---

# 13. Diagnostic Logging

Logging is required in this same PR because the original reproduction logs were from the older `8dac650` build and did not directly log the returned `slReflexGetState()` value.

The new build must let us confirm both:

```text
what Streamline originally returned
what OptiScaler returned after the quirk
```

Do not log every `slReflexGetState()` call.

Games may poll this function frequently.

Use first-call or state-change logging.

A simple state-change implementation is preferred over introducing a new logging abstraction.

Example:

```cpp
static int lastOriginalAvailable = -1;
static int lastReturnedAvailable = -1;

const int originalValue = originalAvailable ? 1 : 0;
const int returnedValue = state.lowLatencyAvailable ? 1 : 0;

if (lastOriginalAvailable != originalValue || lastReturnedAvailable != returnedValue)
{
    LOG_INFO("SL Reflex availability quirk: original = {}, returned = {}", originalAvailable,
             state.lowLatencyAvailable);

    lastOriginalAvailable = originalValue;
    lastReturnedAvailable = returnedValue;
}
```

If the project already has a cleaner local once/change logging pattern at implementation time, use it instead.

Requirements are:

- no per-frame log spam;
- original value is visible;
- returned value is visible;
- log only exists because the game quirk routed through this wrapper.

Do not add verbose logging to global fake-NVAPI calls for this PR.

---

# 14. Expected Combined Implementation Shape

The final Reflex section should conceptually resemble the following.

## Header

```cpp
// Reflex
inline static sl::ReflexMode reflexGamesLastMode = sl::ReflexMode::eOff;
inline static PFN_slGetPluginFunction o_reflex_slGetPluginFunction = nullptr;
inline static PFN_slSetConstants_sl1 o_reflex_slSetConstants_sl1 = nullptr;
inline static PFN_slOnPluginLoad o_reflex_slOnPluginLoad = nullptr;
inline static decltype(&slReflexSetOptions) o_slReflexSetOptions = nullptr;
inline static decltype(&slReflexGetState) o_slReflexGetState = nullptr;
inline static decltype(&slReflexSleep) o_slReflexSleep = nullptr;

static bool hkreflex_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                    const char** pluginJSON);
static sl::Result hkslReflexSetOptions(const sl::ReflexOptions& options);
static sl::Result hkslReflexGetState(sl::ReflexState& state);
static sl::Result hkslReflexSleep(const sl::FrameToken& frame);
```

## Implementation

```cpp
sl::Result StreamlineHooks::hkslReflexGetState(sl::ReflexState& state)
{
    auto result = o_slReflexGetState(state);
    const bool originalAvailable = state.lowLatencyAvailable;

    if (result == sl::Result::eOk && State::Instance().streamlineVersion.major > 1 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailability) &&
        Config::Instance()->StreamlineSpoofing.value_or_default() && fakenvapi::isUsingAsMainNvapi())
    {
        state.lowLatencyAvailable = true;
    }

    static int lastOriginalAvailable = -1;
    static int lastReturnedAvailable = -1;

    const int originalValue = originalAvailable ? 1 : 0;
    const int returnedValue = state.lowLatencyAvailable ? 1 : 0;

    if (lastOriginalAvailable != originalValue || lastReturnedAvailable != returnedValue)
    {
        LOG_INFO("SL Reflex availability quirk: original = {}, returned = {}", originalAvailable,
                 state.lowLatencyAvailable);
        lastOriginalAvailable = originalValue;
        lastReturnedAvailable = returnedValue;
    }

    return result;
}
```

and in the plugin-function resolver:

```cpp
if (strcmp(functionName, "slReflexGetState") == 0 &&
    (State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailability))
{
    o_slReflexGetState = (decltype(&slReflexGetState)) o_reflex_slGetPluginFunction(functionName);
    return &hkslReflexGetState;
}
```

This code is illustrative but should be very close to the final implementation unless compilation or current local style requires a small adjustment.

---

# 15. Explicit Non-Goals

Do not broaden this PR into any of the following:

- global `NvAPI_D3D_GetSleepStatus` changes;
- global fake-NVAPI capability spoof changes;
- changes to `LowLatencyCtx`;
- changes to XeLL initialization;
- global `slReflexGetState` forcing;
- changes to `slIsFeatureSupported` for Reflex;
- extra plugin JSON capability forcing;
- additional SystemCaps spoofing;
- changes to Reflex marker routing;
- changes to `ReflexHooks::hkNvAPI_D3D_SetLatencyMarker`;
- changes to frame generation logic;
- changes to `latencyReportAvailable`;
- adding new user-facing config options;
- adding demo executables without reproduction evidence;
- unrelated cleanup/refactoring.

If the proposed quirk does not solve the runtime issue, stop and collect the new logs rather than expanding the patch speculatively.

---

# 16. Build / Static Validation

Follow the repository contribution rules.

In particular:

- every modified `.cpp` must keep `"pch.h"` as the first non-comment include;
- do not include `pch.h` in `Streamline_Hooks.h`;
- do not add dependencies to `pch.h` for this change.

Required source checks before opening/updating the PR:

```text
1. FixSlReflexAvailability exists exactly once in the enum.
2. Existing GameQuirk enum entries were not reordered.
3. printQuirks contains the new quirk.
4. Only re9.exe and pragmata.exe receive the new quirk.
5. Streamline_Hooks.h has original pointer + hook declaration + VALIDATE_MEMBER_HOOK.
6. hkreflex_slGetPluginFunction routes slReflexGetState only when the quirk is active.
7. hkslReflexGetState calls the original first.
8. Non-eOk results are never converted into successful capability reports.
9. Only lowLatencyAvailable is modified.
10. No fake-NVAPI production files changed.
```

Run the normal project formatting/build checks available in the development environment.

Do not mark the work complete if the solution only compiles but the routing guards differ from the above semantics.

---

# 17. Runtime Test Matrix

## Test A — RE9 / Intel / affected path

Environment:

```text
Executable: re9.exe
GPU: Intel
StreamlineSpoofing: enabled
fake NVAPI: main NVAPI path
```

Expected startup evidence:

```text
Quirks: includes "Fix Streamline Reflex availability"
sl.reflex loads normally
```

Expected GetState evidence if the hypothesis is correct:

```text
SL Reflex availability quirk: original = false, returned = true
```

Expected game behavior:

```text
Reflex option becomes selectable/enabled
```

After enabling Reflex, expected follow-up behavior:

```text
Reflex marker path becomes active
XeFG dispatch Reflex Id is no longer permanently 0
```

Do not require the exact frame ID to match OptiScaler's internal ID; the important validation is that real non-zero/increasing Reflex frame IDs begin appearing.

## Test B — PRAGMATA / Intel / affected path

Repeat Test A using:

```text
Executable: pragmata.exe
```

Expected results are the same.

## Test C — affected game with Streamline spoofing disabled

Use either affected executable but set Streamline spoofing off.

Expected:

```text
wrapper may be selected because the game quirk is active
original slReflexGetState result is preserved
lowLatencyAvailable is NOT forced to true by this quirk
```

## Test D — affected game on native NVIDIA

Expected:

```text
fakenvapi::isUsingAsMainNvapi() == false
native Streamline result is preserved
no forced lowLatencyAvailable=true
```

The wrapper itself may still be selected by the game quirk; the returned native state must remain untouched.

## Test E — MHW / DD2 regression control

Expected:

```text
FixSlReflexAvailability quirk is absent
slReflexGetState is not replaced by the new wrapper
existing Reflex behavior remains unchanged
```

This is an important architectural acceptance criterion: non-quirked games should bypass the new compatibility shim entirely.

## Test F — original GetState failure

If the original call returns anything other than:

```cpp
sl::Result::eOk
```

Expected:

```text
state is not force-enabled
original result is returned unchanged
```

---

# 18. Interpretation of Test Results

## Case 1 — original=false, returned=true, UI unlocks, markers become active

This is the expected success case.

The patch confirms that RE9 / PRAGMATA were using the runtime Reflex availability state as an additional UI/activation gate.

Keep the PR narrow.

Do not add extra fake-NVAPI changes after success.

## Case 2 — original=true before override

The hypothesis is not confirmed for the current build.

Because the wrapper only returns true when it was already true, the quirk provides no meaningful correction.

Do not upstream a no-op quirk merely because it compiles.

Capture the new log and investigate the next gating stage.

## Case 3 — original=false, returned=true, UI still disabled

The runtime availability check is real but not the only gate.

Do not immediately add more forced state.

Capture:

- plugin load logs;
- GetState logs;
- SetOptions activity;
- marker activity;
- any game-side NVAPI queries visible in OptiScaler.

Then design the next smallest quirk separately.

## Case 4 — UI unlocks but Reflex IDs stay permanently zero

The UI gate was fixed, but activation/marker routing has another problem.

Do not mix a second speculative marker fix into this PR unless the new logs clearly prove it is required and the additional change remains game-quirked.

---

# 19. PR Structure

Create the implementation branch from:

```text
Reflex_RE9_Pragmata
```

Suggested branch:

```text
fix/re9-pragmata-reflex-availability-quirk
```

Open a **Draft PR** with:

```text
base: Reflex_RE9_Pragmata
head: fix/re9-pragmata-reflex-availability-quirk
```

Do not target fork `master` yet.

Do not merge before runtime testing in both main executables.

Keep implementation and diagnostic logging in the same PR.

Do not create a separate logging-only PR.

---

# 20. Suggested PR Title

```text
Fix Streamline Reflex availability in RE9 and PRAGMATA
```

Alternative if the maintainer prefers explicit quirk wording:

```text
Add RE9/PRAGMATA Streamline Reflex availability quirk
```

---

# 21. Suggested PR Description Core

Use wording close to the following:

```markdown
## Summary

Adds a narrowly scoped game quirk for RE9 and PRAGMATA to preserve OptiScaler's Streamline spoofing compatibility through the runtime Reflex availability check.

Both games already load `sl.reflex` successfully when using OptiScaler's fake NVAPI / Streamline spoofing path, but their Reflex option remains unavailable. Older reproduction logs also showed every observed XeFG dispatch retaining `Reflex Id: 0`, while MHW/DD2 control logs on the same OptiScaler build had active non-zero Reflex IDs.

The current code already spoofs Reflex SystemCaps during plugin load and fake NVAPI already implements `NvAPI_D3D_GetSleepStatus`. The missing title-specific compatibility boundary is the `slReflexGetState()` value returned to the game.

This patch:

- adds `GameQuirk::FixSlReflexAvailability`;
- enables it only for `re9.exe` and `pragmata.exe`;
- hooks `slReflexGetState` only for games with that quirk;
- calls Streamline first and only adjusts `lowLatencyAvailable` after a successful original call;
- requires Streamline spoofing + fake NVAPI before applying the override;
- leaves native NVIDIA and all non-quirked games unchanged;
- includes low-volume diagnostics for the original/returned availability value.

No global fake-NVAPI behavior is changed.
```

Also explicitly disclose:

```text
The original reproduction logs were captured on 0.9.5-pre4 / 8dac650.
The current patch is against newer source, so the included GetState logging is used to validate that the same runtime gating behavior still reproduces before upstreaming.
```

This transparency is preferable to presenting the older logs as if they came from the current branch.

---

# 22. Upstream-Friendly Review Constraints

Before considering an upstream PR, verify the final diff has these characteristics:

```text
+ one GameQuirk enum value
+ two existing game-table entries extended
+ one printQuirks string
+ one Streamline original function pointer
+ one Streamline hook declaration/validation
+ one small GetState wrapper
+ one quirk-gated resolver branch
+ low-volume diagnostic logging
```

There should be no unrelated architectural changes.

The patch should be understandable to an upstream reviewer without needing to accept a new global policy for fake NVAPI.

The explanation should focus on:

1. both games already pass initial Streamline Reflex plugin capability/loading;
2. current fake NVAPI already has the low-latency API surface;
3. the games appear to apply a stricter runtime availability gate;
4. OptiScaler already uses deep game quirks for comparable Streamline/Reflex compatibility exceptions;
5. the patch changes behavior only for the two reproduced executables.

---

# 23. Stop Conditions

Stop implementation and report results rather than broadening scope if any of the following occurs:

- `slReflexGetState` is not requested by either target game on the current build;
- the original returned `lowLatencyAvailable` is already true before the override;
- the wrapper causes a crash or Streamline integration error;
- native NVIDIA behavior is altered;
- non-quirked games route through the new wrapper;
- the issue requires changing fields other than `lowLatencyAvailable` without new evidence;
- fixing the UI requires broad fake-NVAPI changes.

A failed hypothesis with strong diagnostic evidence is a valid result.

Do not turn this targeted experiment into a general Reflex rewrite.

---

# 24. Completion Checklist

The task is complete only when all applicable items below are satisfied:

- [ ] implementation branch was created from `Reflex_RE9_Pragmata`;
- [ ] `FixSlReflexAvailability` was appended without reordering existing quirk values;
- [ ] only `re9.exe` and `pragmata.exe` gained the new quirk;
- [ ] `printQuirks()` reports the new quirk;
- [ ] `Streamline_Hooks.h` contains pointer, declaration, and signature validation;
- [ ] `hkreflex_slGetPluginFunction()` hooks GetState only for the new quirk;
- [ ] original `slReflexGetState()` is always called first;
- [ ] non-`eOk` results are never overridden;
- [ ] the override requires SL2+, Streamline spoofing, and fake NVAPI main mode;
- [ ] only `lowLatencyAvailable` is modified;
- [ ] logging is state-change/low-volume rather than per-frame spam;
- [ ] no fake-NVAPI production code was changed;
- [ ] project builds successfully;
- [ ] RE9 runtime result recorded;
- [ ] PRAGMATA runtime result recorded;
- [ ] MHW/DD2 remain outside the new hook path;
- [ ] native NVIDIA path remains unforced;
- [ ] Draft PR targets `Reflex_RE9_Pragmata`;
- [ ] PR description clearly distinguishes old `8dac650` reproduction logs from current-source validation;
- [ ] PR remains unmerged until runtime results are reviewed.

---

# 25. Final Implementation Principle

Use the existing OptiScaler quirk system as the compatibility boundary.

Do not make fake NVAPI globally claim more than it already does.

Do not change Reflex semantics for unrelated games.

For RE9 and PRAGMATA only, preserve the existing Streamline path and correct the single runtime availability value the games appear to use as their additional Reflex gate.
