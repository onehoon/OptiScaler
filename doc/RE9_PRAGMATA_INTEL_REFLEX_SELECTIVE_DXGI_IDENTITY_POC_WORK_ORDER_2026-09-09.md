# Work Order: RE9 / PRAGMATA Intel Reflex Selective DXGI Identity POC

Date: 2026-09-09  
Repository: `onehoon/OptiScaler`  
Target baseline branch: **`poc_reflex`**  
Target PR base: **`poc_reflex`**  
Suggested implementation branch: `poc/reflex-selective-dxgi-identity`  
Suggested PR mode: **Draft**  
Production merge target: **none yet — this is a diagnostic POC only**

---

# 1. Purpose

Build a narrowly scoped proof-of-concept that determines **which DXGI adapter-identity consumer must see the configured NVIDIA spoof identity for NVIDIA Reflex to become available in Resident Evil Requiem (`re9.exe`) and PRAGMATA (`pragmata.exe`) on Intel GPUs**.

The current runtime A/B is unusually strong:

```text
Intel + StreamlineSpoofing=true + DxgiSpoofing=false
    -> Reflex unavailable

Intel + StreamlineSpoofing=true + DxgiSpoofing=true/Auto
    -> Reflex available
```

The second case is not an acceptable production solution because broad DXGI spoofing has a measurable performance cost and changes adapter identity for far more consumers than Reflex requires.

The POC must therefore answer:

> Can we keep global `Spoofing.Dxgi=false` while exposing the configured NVIDIA DXGI adapter identity only to the exact caller(s) that are necessary for Reflex capability initialization?

The first comparison targets are:

1. `sl.common.dll` only.
2. Game executable only (`PRAGMATA.exe` / `re9.exe`).
3. Both `sl.common.dll` and the game executable.
4. Neither — baseline control.

Do not assume `sl.common.dll` is already proven to be the sole root cause. The existing logs strongly implicate Streamline system-capability creation, but broad DXGI spoofing also changes what the game executable sees. This POC exists to separate those effects.

---

# 2. Branch and Scope Rules

## 2.1 Branch targeting

All implementation work for this order must:

- start from current `poc_reflex`,
- open the PR against `poc_reflex`,
- remain Draft until runtime A/B is completed,
- never target `master`,
- never backport the POC selector or diagnostic logging to `master` during this task.

The `poc_reflex` branch is intentionally disposable experimental lineage. The final production fix, if discovered, will later be extracted cleanly from current `master` rather than merging the whole POC history.

## 2.2 Non-goals

Do not:

- enable global DXGI spoofing automatically,
- remove `GameQuirk::DisableDxgiSpoofing`,
- change the default production DXGI-spoof policy,
- patch PRAGMATA or RE9 executable memory,
- force a game capability bit directly,
- force PCL or Reflex game-side masks directly,
- bypass anti-tamper,
- modify XeSS / XeFG / XeLL adapter identity globally,
- change Streamline function return values solely to make the menu appear,
- merge this diagnostic POC to `master`,
- perform unrelated DXGI/refactor cleanup.

---

# 3. Current Evidence

## 3.1 Target-game behavior

On Intel with current `poc_reflex`:

- DLSS upscaling is available through spoofing.
- DLSS-G input is available and can feed XeFG output.
- `sl.reflex` loads.
- `sl.pcl` loads.
- `slGetFeatureRequirements(kFeatureReflex)` returns `eOk`.
- `slIsFeatureSupported(kFeatureReflex)` returns `eOk`.
- with global DXGI spoof OFF, the game does not continue into the normal Reflex API-acquisition/lifecycle path.
- XeFG `Reflex Id` remains zero.

With global DXGI spoof enabled on the same Intel system and same POC build, Reflex becomes available.

The useful observation is not merely that the menu changes. The Streamline system-capability path also changes materially when DXGI identity spoofing is enabled: the adapter can be treated as NVIDIA and the fake-NVAPI path can populate NVIDIA-style driver/architecture capability data.

## 3.2 Positive controls

Known / reported working cases:

```text
NVIDIA + RE9 / PRAGMATA -> Reflex works
AMD    + RE9 / PRAGMATA -> users report Reflex is always available
Intel  + MHW             -> Reflex works through OptiScaler spoofing
Intel  + RE9 / PRAGMATA  -> Reflex fails when DxgiSpoofing is off
```

MHW is particularly useful because it proves that Intel + XeFG + XeLL is not inherently incompatible with the Streamline Reflex lifecycle.

## 3.3 PRAGMATA static RE findings

Targeted RE established the following game-side capability flow:

```text
kFeaturePCL    = raw feature 4 -> internal bit 2 -> mask 0x4
kFeatureReflex = raw feature 3 -> internal bit 1 -> mask 0x2
```

PCL support producer:

```text
PRAGMATA.exe RVA 0x4B791FF -> raw feature 4
RVA 0x4B79207              -> slIsFeatureSupported(kFeaturePCL)
RVA 0x4B7920D              -> test eax,eax
RVA 0x4B7920F              -> sete dl
RVA 0x4B79212              -> mapped bit 2
RVA 0x4B79217              -> capability writer
```

Reflex support producer:

```text
PRAGMATA.exe RVA 0x4B79277 -> slIsFeatureSupported(kFeatureReflex)
RVA 0x4B7927F             -> sete dl
RVA 0x4B79282             -> mapped bit 1
RVA 0x4B79284             -> capability writer
```

Global game-side capability mask:

```text
RVA 0xBB89D28
```

A consumer performs:

```cpp
bool availability = false;

if (g_featureMask & 0x4)       // PCL
    availability = (g_featureMask & 0x2) != 0; // Reflex

object->field_8 = availability;
```

However, further RE showed that `field_8` is not proven to be the direct Reflex-menu gate. It participates in a broader feature-selection / latency capability path, including a DLSS-related consumer. Therefore PCL remains useful diagnostic context, but it is no longer sufficient to explain the whole symptom by itself.

---

# 4. Key OptiScaler Source Facts

## 4.1 Current target quirks

`re9.exe` and `pragmata.exe` already carry:

```cpp
GameQuirk::DisableDxgiSpoofing
GameQuirk::FixSlReflexAvailabilityOnIntel
```

PRAGMATA also carries its existing DLSS-mode quirk.

Do not remove or weaken `DisableDxgiSpoofing` for this POC.

The POC must coexist with it and selectively expose spoof identity only for the experimental Reflex path.

## 4.2 Current DXGI spoofing mechanism

`OptiScaler/spoofing/Dxgi_Spoofing.cpp` hooks:

```text
IDXGIAdapter::GetDesc
IDXGIAdapter1::GetDesc1
IDXGIAdapter2::GetDesc2
IDXGIAdapter4::GetDesc3
```

Normal broad spoofing changes at least:

```text
VendorId
DeviceId
Description
```

using:

```cpp
Config::Instance()->SpoofedVendorId
Config::Instance()->SpoofedDeviceId
Config::Instance()->SpoofedGPUName
```

Some system / driver callers are intentionally excluded.

Important current behavior in `AttachToAdapter()`:

```cpp
if (!Config::Instance()->DxgiSpoofing.value_or_default() &&
    !Config::Instance()->DxgiVRAM.has_value())
{
    return;
}
```

Therefore with `DxgiSpoofing=false`, the adapter `GetDesc*` detours are normally not attached at all.

The POC must account for this explicitly. A caller-selective spoof cannot work by modifying only the mutation condition; target-game POC mode must also allow the existing adapter hooks to attach while global spoof remains disabled.

This must be done without enabling broad spoof behavior.

---

# 5. Central Hypothesis

Streamline `sl.common` builds shared system capability information by enumerating DXGI adapters and reading adapter descriptors.

A simplified relevant path is:

```cpp
adapter->GetDesc(&desc);
auto vendor = desc.VendorId;

if (isVendorNvidia(vendor))
{
    // NVIDIA path becomes eligible
    // NVAPI-backed driver / architecture capability information can be populated
}
```

The Reflex plugin then consumes the shared system caps to establish its own low-latency availability state.

The working hypothesis is:

```text
DXGI OFF
real Intel identity
    -> Streamline common system caps remain Intel/non-NVIDIA
    -> NVIDIA-style Reflex LL capability is not established
    -> target game never advances into its normal Reflex lifecycle

DXGI ON
configured NVIDIA identity visible through GetDesc*
    -> Streamline common system caps enter NVIDIA/fake-NVAPI path
    -> driver/architecture Reflex prerequisites become populated
    -> target game advances into Reflex lifecycle
```

But broad DXGI ON also changes the identity visible to the game executable and other consumers.

Therefore we need caller-isolated A/B testing before accepting this causal chain.

---

# 6. Required POC Design

Implement a **POC-only selective DXGI identity mode** that is active only under the existing Intel Reflex target quirk.

## 6.1 Mandatory gating

Selective spoofing must require all of the following:

```text
GameQuirk::FixSlReflexAvailabilityOnIntel == active
physical / primary GPU == Intel
StreamlineSpoofing == true
DxgiSpoofing == false
POC selective mode != Off
```

If any condition is false, the POC path must not modify adapter descriptors.

Do not make AMD or NVIDIA use this path.

Do not make non-target games use this path.

Do not make global `DxgiSpoofing=true` use this special branch; normal broad spoofing should keep its current behavior.

## 6.2 POC selector

Add the smallest practical POC-only selector that allows one build to perform the runtime matrix without rebuilding.

Preferred semantic values:

```text
Off
SlCommonOnly
GameOnly
SlCommonAndGame
```

Use the existing Config architecture and naming style after inspecting it. The exact external key name may be adjusted to fit existing conventions, but it must clearly be experimental and must default to `Off`.

Suggested semantic name:

```text
ReflexDxgiIdentityScope
```

Do not change normal `DxgiSpoofing` semantics.

If adding an INI-visible selector would create disproportionate churn, a tiny diagnostic-only selector mechanism is acceptable, but the final PR must still make A/B switching explicit and easy to reproduce.

## 6.3 Caller classification

Use the caller already available in `hkGetDesc*`:

```cpp
auto caller = Util::WhoIsTheCaller(_ReturnAddress());
```

Classify at minimum:

```text
sl.common.dll
pragmata.exe
re9.exe
```

Comparison must be case-insensitive.

Do not classify every DLL in the process.

Do not introduce stack walking.

Do not add expensive symbol resolution.

Suggested helper shape:

```cpp
enum class ReflexDxgiIdentityScope
{
    Off,
    SlCommonOnly,
    GameOnly,
    SlCommonAndGame,
};

static bool ShouldApplySelectiveReflexIdentity(
    std::string_view caller,
    ReflexDxgiIdentityScope scope,
    bool targetQuirk,
    bool intelGpu,
    bool streamlineSpoofing,
    bool globalDxgiSpoofing);
```

Keep the policy logic testable and separate from descriptor mutation.

## 6.4 Adapter hook attachment with global spoof OFF

Current `AttachToAdapter()` skips all hooks when both global DXGI spoof and DXGI VRAM override are off.

Adjust this carefully so hooks may attach when the selective Reflex POC is eligible.

Conceptual condition:

```cpp
const bool broadDxgiNeeded =
    Config::Instance()->DxgiSpoofing.value_or_default() ||
    Config::Instance()->DxgiVRAM.has_value();

const bool selectiveReflexDxgiNeeded =
    IsSelectiveReflexDxgiPocEligible();

if (!broadDxgiNeeded && !selectiveReflexDxgiNeeded)
    return;
```

Important:

> Hook attachment does not imply spoofing.

After attachment, each `GetDesc*` call must still return the real descriptor unless the exact caller matches the selected POC scope.

## 6.5 Descriptor mutation

For the selective POC path, mutate only the same identity fields required to reproduce the broad-spoof identity effect:

```text
VendorId
DeviceId
Description
```

Use the existing configured spoof values rather than hard-coding a second fake GPU identity:

```cpp
SpoofedVendorId
SpoofedDeviceId
SpoofedGPUName
```

Do not modify:

```text
AdapterLuid
DedicatedVideoMemory
DedicatedSystemMemory
SharedSystemMemory
Flags
GraphicsPreemptionGranularity
ComputePreemptionGranularity
```

unless existing independent config such as `DxgiVRAM` already does so through its normal path.

The selective Reflex POC must not implicitly enable VRAM spoofing.

## 6.6 Preserve existing exclusions

Preserve the current exclusions for system/driver callers such as:

```text
vulkan-1.dll
amdvlk64.dll
dxgi.dll
d3d12.dll
d3d12Core.dll
```

The POC should never use them as selective Reflex callers.

Do not weaken XeSS / FSR-specific identity handling already present in this file.

## 6.7 Avoid duplicated descriptor logic

The four `GetDesc*` functions currently repeat broad-spoof mutation logic.

For this POC, a **small local helper** to apply configured identity to descriptor fields is acceptable if it reduces the chance of inconsistent behavior across `GetDesc`, `GetDesc1`, `GetDesc2`, and `GetDesc3`.

Do not use this task as an excuse for a larger DXGI refactor.

A conceptual helper is sufficient:

```cpp
template <typename DescT>
static void ApplyConfiguredDxgiIdentity(DescT* desc)
{
    desc->VendorId = Config::Instance()->SpoofedVendorId.value_or_default();
    desc->DeviceId = Config::Instance()->SpoofedDeviceId.value_or_default();

    const auto name = Config::Instance()->SpoofedGPUName.value_or_default();
    std::memset(desc->Description, 0, sizeof(desc->Description));
    std::wcscpy(desc->Description, name.c_str());
}
```

Match project safety/style conventions if a bounded-copy helper is already preferred.

---

# 7. Diagnostic Logging

Logging must be bounded and target-scoped.

Do not turn all DXGI descriptor calls into permanent info logs.

## 7.1 Selective identity log

When the selective POC actually changes a descriptor, emit a bounded record containing:

```text
API: GetDesc / GetDesc1 / GetDesc2 / GetDesc3
caller
scope
original VendorId
original DeviceId
spoof VendorId
spoof DeviceId
target exe
```

Suggested prefix:

```text
[ReflexDxgiPOC]
```

Example:

```text
[ReflexDxgiPOC] api=GetDesc caller=sl.common.dll scope=SlCommonOnly originalVendor=0x8086 originalDevice=0x.... spoofVendor=0x10DE spoofDevice=0x....
```

A small once-per `(API, caller)` or globally bounded budget is sufficient.

No per-frame logging.

## 7.2 PCL support observation

Extend the existing target-game Streamline diagnostic so that `kFeaturePCL` support can be observed alongside Reflex.

At minimum log, without changing behavior:

```text
slGetFeatureRequirements(kFeaturePCL) if called
slIsFeatureSupported(kFeaturePCL)
caller module
caller RVA
result
```

The known PRAGMATA PCL callsite is around:

```text
PRAGMATA.exe RVA 0x4B79207
```

Expected subsequent test/bit write:

```text
RVA 0x4B7920D / 0x4B7920F
RVA 0x4B79217
```

Do not alter PCL return values.

Do not force PCL success.

## 7.3 Preserve current Reflex diagnostics

Do not remove the existing `[ReflexProviderGate]` logging from `poc_reflex` unless required to fix a proven bug.

The new POC should make it easy to correlate:

```text
DXGI selective identity event
PCL support result
Reflex support result
Reflex function acquisition
Reflex GetState / SetOptions lifecycle
XeFG Reflex Id
```

---

# 8. Runtime Test Matrix

The runtime test must keep global DXGI spoof disabled for all selective POC cases.

Common configuration:

```text
GPU: Intel
Game: PRAGMATA first
StreamlineSpoofing=true
DxgiSpoofing=false
FGInput=DLSSG
FGOutput=XeFG
same OptiScaler build
same game files
```

PRAGMATA is the primary validation game because the current static and log evidence is strongest there.

Run these four cases:

## Case 0 — Baseline control

```text
ReflexDxgiIdentityScope=Off
DxgiSpoofing=false
```

Expected based on current baseline:

```text
Reflex unavailable
slIsFeatureSupported(Reflex)=eOk
no normal Reflex runtime lifecycle
XeFG Reflex Id=0
```

Capture PCL support result.

## Case 1 — `sl.common.dll` only

```text
ReflexDxgiIdentityScope=SlCommonOnly
DxgiSpoofing=false
```

Only `sl.common.dll` may receive configured NVIDIA descriptor identity.

The game executable and XeSS/XeFG/XeLL callers must still see the real Intel identity.

Key observations:

```text
Does Streamline common now report NVIDIA-style system capability data?
Does PCL support change?
Does Reflex lifecycle begin?
Does Reflex menu become available?
Do XeFG Reflex IDs become non-zero?
```

If this case alone works, the leading root cause becomes Streamline common system-cap creation.

## Case 2 — game executable only

```text
ReflexDxgiIdentityScope=GameOnly
DxgiSpoofing=false
```

Only `PRAGMATA.exe` may receive configured NVIDIA descriptor identity.

`sl.common.dll` must still see the real Intel descriptor.

If this case alone works, the leading root cause becomes a game-owned DXGI identity/capability path rather than Streamline common system caps.

## Case 3 — `sl.common.dll` + game executable

```text
ReflexDxgiIdentityScope=SlCommonAndGame
DxgiSpoofing=false
```

Only those two caller categories may receive configured NVIDIA descriptor identity.

Use this case only after Cases 1 and 2.

If neither isolated case works but this one works, then the target likely requires both sides of the identity contract.

---

# 9. A/B Interpretation Rules

## Outcome A — `SlCommonOnly` restores Reflex

This is the preferred result.

Interpretation:

```text
Global DXGI spoof was unnecessary.
The required effect is primarily Streamline system-capability identity construction.
```

Next step after runtime confirmation:

1. keep `DxgiSpoofing=false`,
2. evaluate whether caller-scoped `sl.common` identity can itself be production-safe,
3. then investigate an even cleaner targeted SystemCaps correction that avoids DXGI descriptor spoof entirely.

Do not immediately promote the POC implementation to `master`.

## Outcome B — `GameOnly` restores Reflex

Interpretation:

```text
PRAGMATA / RE9 has an additional game-owned adapter-identity path that is not visible in the current targeted static analysis.
```

Next step:

- use the known DXGI caller evidence to target the game-side consumer,
- do not broaden global spoofing,
- evaluate whether a caller-scoped game-only production quirk is safe.

## Outcome C — only `SlCommonAndGame` restores Reflex

Interpretation:

```text
The working state depends on both Streamline system caps and game-side adapter identity.
```

Next step:

- preserve the two sides as separate hypotheses,
- gather exact `GetDesc*` callsites / timing,
- avoid assuming one can be removed until another A/B proves it.

## Outcome D — none of the selective cases restore Reflex

Interpretation:

Broad DXGI spoof changes some additional caller or initialization order not covered by the initial hypothesis.

Next step:

- compare bounded caller lists between broad DXGI ON and selective mode,
- identify the smallest additional caller category,
- do not fall back to global DXGI ON as the fix.

## Outcome E — PCL differs while Reflex support remains `eOk`

If PCL is `eOk` only in a successful DXGI identity mode, record that as a meaningful part of the causal chain.

If PCL remains `eOk` in both successful and failing modes, deprioritize PCL as the root gate.

---

# 10. Performance and Safety Requirements

The purpose of this work is specifically to avoid the performance cost of broad DXGI spoofing.

Therefore the implementation must not recreate broad spoofing under another name.

## 10.1 Caller scope

For `SlCommonOnly`, successful mutation count should correspond only to `sl.common.dll` calls.

For `GameOnly`, successful mutation count should correspond only to the current target executable.

For `SlCommonAndGame`, no unrelated DLL should receive spoof identity.

## 10.2 Hot-path cost

Keep the additional per-`GetDesc*` work minimal:

- simple precomputed POC-enabled state where practical,
- cheap case-insensitive caller comparison,
- no stack walking,
- no filesystem operations,
- no dynamic symbol lookup,
- no repeated GPU enumeration,
- no high-volume formatting/logging after the bounded diagnostic budget is exhausted.

## 10.3 Real Intel path preservation

Verify from logs/code that the selective POC does **not** make these callers see NVIDIA identity unless separately proven necessary:

```text
libxess.dll
libxess_fg.dll
libxell.dll
d3d12.dll
d3d12Core.dll
dxgi.dll
Intel driver modules
AMD FSR libraries
Steam overlay / unrelated overlays
```

This is a central acceptance criterion.

---

# 11. Recommended Code Structure

Keep implementation changes narrow.

Expected files may include:

```text
OptiScaler/spoofing/Dxgi_Spoofing.cpp
OptiScaler/spoofing/Dxgi_Spoofing.h        only if needed
OptiScaler/Config.h / Config.cpp           only for the POC selector
OptiScaler/hooks/Streamline_Hooks.cpp      PCL observation only
OptiScaler/misc/...                        only if a tiny shared diagnostic helper is required
OptiScaler.ini / Config.md                 only if the POC selector follows normal exposed-config conventions
```

Do not modify fake NVAPI behavior for this PR unless runtime evidence from the selective matrix specifically proves a missing fake-NVAPI response.

Do not modify low-latency provider selection.

Do not modify XeFG swapchain or present logic.

Do not modify REFramework integration.

---

# 12. Suggested Policy Helper

A small pure policy helper is strongly preferred so the caller scope can be unit-tested independently from COM hooks.

Conceptual example:

```cpp
static bool ShouldApplySelectiveReflexIdentity(
    std::string_view caller,
    ReflexDxgiIdentityScope scope,
    bool targetGame,
    bool intelGpu,
    bool streamlineSpoofing,
    bool globalDxgiSpoofing)
{
    if (!targetGame || !intelGpu || !streamlineSpoofing || globalDxgiSpoofing)
        return false;

    const bool slCommon = iequals(caller, "sl.common.dll");
    const bool game = iequals(caller, "pragmata.exe") || iequals(caller, "re9.exe");

    switch (scope)
    {
    case ReflexDxgiIdentityScope::SlCommonOnly:
        return slCommon;
    case ReflexDxgiIdentityScope::GameOnly:
        return game;
    case ReflexDxgiIdentityScope::SlCommonAndGame:
        return slCommon || game;
    default:
        return false;
    }
}
```

Do not copy this blindly if the existing project has better helpers for executable name / quirk detection. Preserve current project patterns.

---

# 13. Tests

Add focused automated tests if the current test layout makes this practical.

At minimum, policy-level tests should cover:

```text
target Intel + SL spoof + global DXGI off + Off            -> false
target Intel + SL spoof + global DXGI off + SlCommonOnly   + sl.common.dll -> true
target Intel + SL spoof + global DXGI off + SlCommonOnly   + pragmata.exe  -> false
target Intel + SL spoof + global DXGI off + GameOnly       + pragmata.exe  -> true
target Intel + SL spoof + global DXGI off + GameOnly       + sl.common.dll -> false
target Intel + SL spoof + global DXGI off + Both           + both callers  -> true
non-target game                                             -> false
AMD                                                         -> false
NVIDIA                                                      -> false
StreamlineSpoofing=false                                    -> false
global DxgiSpoofing=true                                    -> selective POC branch false
system callers                                              -> false
```

If no suitable unit-test seam exists without disproportionate restructuring, keep production code simple and document the manual runtime matrix instead of introducing a large test-only refactor.

Run the repository's normal build/test/format validation before opening the Draft PR.

---

# 14. Required Runtime Evidence in PR Description / Follow-up

Do not claim the POC solves the issue based on build success.

Runtime validation must report a compact matrix similar to:

| Scope | Global DXGI | sl.common sees | Game sees | PCL result | Reflex support | Reflex lifecycle | Reflex IDs | Menu |
|---|---|---|---|---|---|---|---|---|
| Off | false | Intel | Intel | ? | eOk | no | 0 | off |
| SlCommonOnly | false | spoof NV | Intel | ? | ? | ? | ? | ? |
| GameOnly | false | Intel | spoof NV | ? | ? | ? | ? | ? |
| Both | false | spoof NV | spoof NV | ? | ? | ? | ? | ? |

For a successful case, capture evidence of the later lifecycle, preferably including:

```text
slGetFeatureFunction / Reflex function acquisition
slReflexGetState
slReflexSleep
slReflexSetOptions
lowLatencyAvailable state if already observable
XeFG Reflex Id non-zero
```

For all selective cases verify:

```text
Spoofing.Dxgi=false
```

remains true in the effective config/log state.

---

# 15. Stop Conditions

Stop and report instead of expanding scope if:

- selective `GetDesc*` hook attachment causes a new crash,
- caller detection is unstable or ambiguous,
- `sl.common.dll` is not the direct caller in the relevant capability path,
- broad DXGI ON changes additional descriptor APIs that this POC does not cover,
- a required change would alter real Intel identity for XeSS/XeFG/XeLL,
- the POC requires game memory patching,
- the POC requires changing Streamline support return values before the caller-isolation test is complete.

Do not add more hooks speculatively.

---

# 16. Acceptance Criteria for the Draft POC PR

The PR is ready for runtime testing when all of the following are true:

1. PR base is `poc_reflex`.
2. Global `DxgiSpoofing=false` remains supported and unchanged.
3. The selective POC can attach adapter descriptor hooks even when global DXGI spoof is off, but only on eligible target Intel runs.
4. Descriptor identity mutation occurs only for the selected caller scope.
5. `Off`, `SlCommonOnly`, `GameOnly`, and `SlCommonAndGame` can be reproduced without source edits if the chosen selector implementation supports runtime selection.
6. Existing broad DXGI spoof behavior is not changed when it is explicitly enabled.
7. AMD/NVIDIA/non-target games cannot enter the selective POC path.
8. PCL support result is observable without modifying it.
9. Existing Reflex diagnostic behavior is preserved.
10. No fake-NVAPI call path is replaced or rerouted for diagnostics.
11. No game executable patching is introduced.
12. Build/tests/format checks pass.
13. PR remains Draft pending PRAGMATA runtime A/B.

---

# 17. Post-POC Decision

Do not decide the production implementation before runtime results.

Preferred direction if `SlCommonOnly` succeeds:

```text
Keep normal DXGI identity real Intel globally
        +
Provide only the minimum Streamline common capability identity required for Reflex
```

Then investigate whether even the `sl.common.dll`-only DXGI descriptor spoof can be replaced by a cleaner correction at the Streamline shared `SystemCaps` boundary.

The eventual production objective is:

```text
DxgiSpoofing=false
StreamlineSpoofing=true
Intel remains Intel for game/render/driver/XeSS/XeFG/XeLL
Reflex compatibility data is supplied only where required
no broad-DXGI performance regression
```

If `GameOnly` or `Both` is required, preserve that evidence and design the next work order around the smallest proven caller set rather than enabling global spoofing.

---

# 18. Final Implementation Principle

The POC must answer one narrow question:

> **Which DXGI adapter-identity consumer is responsible for the Intel-only Reflex failure when broad DXGI spoofing is disabled?**

The task is not complete merely because Reflex can be forced on.

The valuable result is a reproducible causal separation between:

```text
Streamline common system-cap identity
vs.
game-side adapter identity
vs.
a combination of both
```

while preserving the performance benefit of keeping global DXGI spoofing disabled.