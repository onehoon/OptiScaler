# Work Order: RE9 / PRAGMATA Intel Reflex Provider-Gate Diagnostics v2

Date: 2026-09-09  
Repository: `onehoon/OptiScaler`  
Known-good code baseline: `75e6c7d9c659a04d5430176f080794b2371a1c63`  
Work-order branch: `work-order/reflex-intel-provider-gate`  
Suggested implementation branch: `diagnostic/re9-pragmata-reflex-provider-gate-v2`  
Suggested PR mode: **Draft**  
Target PR base: **the experimental work-order / Reflex lineage only — do not target `master`**

---

# 1. Purpose

Determine why NVIDIA Reflex is exposed correctly in Resident Evil Requiem (`re9.exe`) and PRAGMATA (`pragmata.exe`) on NVIDIA GPUs, is reported to work on AMD GPUs when OptiScaler spoofing is used, but fails specifically on Intel GPUs even though the same Intel setup successfully exposes DLSS upscaling and DLSS Frame Generation input for XeFG output.

This work order replaces the failed PR #8 diagnostic approach.

The new diagnostic must obey one non-negotiable rule:

> **The diagnostic must not change any existing execution path, function pointer, return value, output structure, hook chain, provider choice, or capability result. It may only observe state already produced by the existing code.**

The first objective is not to implement another Reflex compatibility fix. The objective is to identify the first meaningful difference between the Intel path and a working path.

The leading hypothesis to test is now:

> RE9 / PRAGMATA may make their Reflex availability decision while Intel and AMD are exposing different low-latency provider state, even though XeFG may later force both vendors onto XeLL for the final frame-generation path.

Do not assume this hypothesis is correct. Instrument it so the next runtime logs can prove or disprove it.

---

# 2. Current Evidence and Constraints

## 2.1 Confirmed Intel behavior in RE9 / PRAGMATA

On Intel GPUs with OptiScaler Streamline spoofing:

- DLSS upscaling is exposed.
- DLSS Frame Generation input is exposed and can feed XeFG output.
- NVIDIA Reflex is not exposed.
- `sl.reflex` loads successfully.
- Reflex is present in the Streamline `featuresToLoad` list.
- `slGetFeatureRequirements(kFeatureReflex)` returns `eOk`.
- `slIsFeatureSupported(kFeatureReflex)` returns `eOk`.
- The game never proceeds to the later Reflex lifecycle calls that were previously expected.

Observed missing calls include:

```text
slIsFeatureLoaded(kFeatureReflex)
slGetFeatureVersion(kFeatureReflex)
slGetFeatureFunction(kFeatureReflex, ...)
slReflexGetState
NvAPI_D3D_GetSleepStatus
slReflexSetOptions
Reflex marker/sleep runtime activity
```

XeFG continues operating while its logged `Reflex Id` remains `0`.

Therefore the failure occurs after basic Streamline support has already passed, but before the actual Reflex runtime lifecycle begins.

## 2.2 Positive controls

Important comparison cases:

```text
NVIDIA + RE9 / PRAGMATA     -> Reflex works as expected
Intel  + MHW / DD2          -> Reflex works through Streamline spoofing
AMD    + RE9 / PRAGMATA     -> reported working; collect runtime evidence if available
Intel  + RE9 / PRAGMATA     -> Reflex fails
```

The AMD case is currently a user/runtime report, not yet a captured diagnostic baseline. Treat it as a strong comparison target but do not write code that assumes it is already proven.

## 2.3 PR #8 regression

PR #8 attempted caller-aware NVAPI tracing and produced a launch/runtime regression in PRAGMATA.

The critical design mistake was that a diagnostic-only change altered the actual `NvAPI_QueryInterface` execution path instead of merely observing it.

The known-good baseline `75e6c7d9` runs PRAGMATA correctly, while the PR #8 head reproduced a Streamline-side crash / render-stop condition.

Do not reuse PR #8's alternate QueryInterface path, helper routing, hot-path tracing, or function substitution design.

---

# 3. Key Source Finding That Must Drive This Diagnostic

The current `InputCommon::update_low_latency_tech()` logic contains two distinct stages that must not be conflated.

First, when `LowLatencyOutput == Auto`, the vendor selects the default low-latency output:

```cpp
if (desiredMode == LowLatencyMode::Auto)
{
    auto vendorId = IdentifyGpu::getPrimaryGpu().vendorId;

    if (vendorId == VendorId::Intel)
        desiredMode = LowLatencyMode::XeLL;
    else if (vendorId == VendorId::AMD)
        desiredMode = LowLatencyMode::AntiLag2;
    else
        desiredMode = LowLatencyMode::LatencyFlex;
}
```

Then XeFG can override that vendor-selected mode:

```cpp
// Force XeLL when using XeFG
if (State::Instance().activeFgOutput == FGOutput::XeFG)
    desiredMode = LowLatencyMode::XeLL;
```

This distinction is central to the next investigation.

The diagnostic must explicitly capture:

```text
vendor-selected mode BEFORE XeFG force
whether XeFG force condition is active
final desired mode AFTER XeFG force
current active input
current active output
whether a low-latency tech is already active
```

The working hypothesis is that AMD and Intel may differ during the Reflex-availability window even if both eventually operate XeFG with XeLL.

Do not reduce this to one log line such as `LowLatency algo: XeLL`. That only reports the final initialized technology and loses the decision history.

---

# 4. Diagnostic Architecture

Implement this in phases. Do not combine all possible tracing into one large PR.

The first implementation PR should contain only:

1. Streamline Reflex gate caller/RVA tracing.
2. Read-only low-latency provider decision snapshots.
3. Existing DXGI factory-call caller/RVA tracing only if it can be added without new COM calls.
4. Optional existing `NvAPI_QueryInterface` caller/RVA tracing only through the already-existing hook and without changing its call path.

Do not add new DXGI adapter method hooks in the first PR.

Do not add new NVAPI function wrappers in the first PR.

Do not modify any fake-NVAPI implementation.

---

# 5. Part A — Streamline Reflex Gate Caller / RVA

## 5.1 Target functions

Use the existing Streamline wrappers only:

```cpp
StreamlineHooks::hkslIsFeatureSupported
StreamlineHooks::hkslIsFeatureLoaded
StreamlineHooks::hkslGetFeatureRequirements
StreamlineHooks::hkslGetFeatureVersion
StreamlineHooks::hkslGetFeatureFunction
```

Only emit diagnostic records when:

```cpp
feature == sl::kFeatureReflex
```

Do not alter the existing DLSS_G special cases.

## 5.2 Call original first, then observe

For non-DLSS_G Reflex handling, preserve the original call exactly once.

Pattern:

```cpp
sl::Result StreamlineHooks::hkslIsFeatureSupported(
    sl::Feature feature,
    const sl::AdapterInfo& adapterInfo)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    void* returnAddress = _ReturnAddress();
    const auto result = o_slIsFeatureSupported(feature, adapterInfo);

    if (feature == sl::kFeatureReflex)
        ReflexProviderDiag::LogStreamlineOnce(
            "slIsFeatureSupported",
            returnAddress,
            result);

    return result;
}
```

Equivalent rules apply to the other Reflex wrappers.

Do not call the original function twice for logging.

Do not change any output parameter.

## 5.3 Required caller information

For every unique Streamline Reflex callsite, record:

```text
API name
result
caller module
caller RVA
```

Example:

```text
[ReflexProviderGate] seq=5 area=SL api=slIsFeatureSupported result=eOk caller=pragmata.exe rva=0x01234567
```

The RVA is important. Module name alone is insufficient.

Recommended method:

```cpp
returnAddress = _ReturnAddress();
moduleBase = module containing returnAddress;
rva = reinterpret_cast<uintptr_t>(returnAddress) - reinterpret_cast<uintptr_t>(moduleBase);
```

Do not resolve symbols.

Do not perform stack walking.

Do not call DbgHelp.

Only module name + RVA is required.

## 5.4 Dedupe before expensive caller resolution

Avoid repeating expensive module-name resolution on every call.

Use the raw return address as the first dedupe key.

Conceptual pattern:

```cpp
if (!SeenCallsite(returnAddress, apiId))
    return;

ResolveModuleAndRva(returnAddress, ...);
Log(...);
```

The function name `SeenCallsite` above is only illustrative; implement a thread-safe bounded equivalent appropriate for the existing codebase.

Maximum number of unique Streamline Reflex diagnostic records should remain small, e.g. 32.

---

# 6. Part B — Low-Latency Provider Decision Timeline

This is the highest-priority new diagnostic.

## 6.1 Instrument the existing decision point

Target:

```cpp
InputCommon::update_low_latency_tech(
    IUnknown* pDevice,
    std::optional<LowLatencyMode> mode)
```

Do not add new low-latency API calls.

Do not call `init()`, `deinit()`, `get_sleep_status()`, `set_sleep_mode()`, or any provider-specific function merely to generate diagnostics.

Observe values that the function already computes.

## 6.2 Capture the mode before and after XeFG forcing

Create local diagnostic-only snapshots without changing control flow.

Conceptual example:

```cpp
LowLatencyMode requestedMode = desiredMode;
LowLatencyMode vendorResolvedMode = desiredMode;

if (desiredMode == LowLatencyMode::Auto)
{
    const auto vendorId = IdentifyGpu::getPrimaryGpu().vendorId;

    if (vendorId == VendorId::Intel)
        desiredMode = LowLatencyMode::XeLL;
    else if (vendorId == VendorId::AMD)
        desiredMode = LowLatencyMode::AntiLag2;
    else
        desiredMode = LowLatencyMode::LatencyFlex;

    vendorResolvedMode = desiredMode;
}

const bool xefgForce = State::Instance().activeFgOutput == FGOutput::XeFG;

if (xefgForce)
    desiredMode = LowLatencyMode::XeLL;

const LowLatencyMode finalDesiredMode = desiredMode;
```

Do not restructure the real logic merely to match this sample. Keep the existing code flow and only retain diagnostic copies of values already being calculated.

## 6.3 Required provider snapshot fields

Emit a bounded record when the provider decision state changes.

Required fields:

```text
primary GPU vendor
requested mode / configured LowLatencyOutput
vendor-resolved mode before XeFG force
activeFgOutput
xefgForce=true/false
final desired mode after XeFG force
activeInput
activeOutput
currently_active_tech present=true/false
currently_active_tech mode if already present
pDevice present=true/false
explicit mode argument present=true/false
explicit mode value when present
```

Example:

```text
[ReflexProviderGate] seq=2 area=LL phase=decision vendor=Intel configured=Auto vendorMode=XeLL fgOutput=XeFG xefgForce=true finalMode=XeLL activeInput=None activeOutput=None techPresent=false explicitMode=false devicePresent=true
```

AMD example we want to distinguish:

```text
[ReflexProviderGate] seq=2 area=LL phase=decision vendor=AMD configured=Auto vendorMode=AntiLag2 fgOutput=NoFG xefgForce=false finalMode=AntiLag2 ...
```

or, if XeFG has already become active:

```text
[ReflexProviderGate] seq=3 area=LL phase=decision vendor=AMD configured=Auto vendorMode=AntiLag2 fgOutput=XeFG xefgForce=true finalMode=XeLL ...
```

This exact distinction is the point of the diagnostic.

## 6.4 Capture successful provider initialization using existing information

`InputCommon::init_tech()` already logs:

```text
LowLatency algo: XeLL
LowLatency algo: AntiLag2
LowLatency algo: LatencyFlex
```

Keep that log.

Optionally add a bounded diagnostic record immediately after successful initialization using values already available in the function:

```text
phase=initialized
mode=<current_tech->get_mode()>
activeInput=<...>
activeOutput=<...>
```

Do not call provider APIs for extra state.

## 6.5 Avoid hot-path logging

`update_low_latency_tech()` can be reached repeatedly.

Do not log every invocation.

Dedupe on the relevant state tuple or cap transitions aggressively.

Recommended hard ceiling:

```text
16 low-latency decision records per process
```

No per-frame diagnostic is permitted.

Do not resolve caller module/RVA inside `update_low_latency_tech()` in this first PR. The provider state itself is the priority and caller resolution here adds unnecessary risk/cost.

---

# 7. Part C — Existing DXGI Factory Calls Only

Use only already-hooked factory APIs:

```cpp
DxgiFactoryHooks::EnumAdapters
DxgiFactoryHooks::EnumAdapters1
DxgiFactoryHooks::EnumAdapterByLuid
DxgiFactoryHooks::EnumAdapterByGpuPreference
```

Record the caller module/RVA and arguments/results only when diagnostic scope is active.

Do not add an extra `GetDesc`, `GetDesc1`, `GetDesc2`, or `GetDesc3` call for logging.

Do not query additional COM interfaces.

Do not mutate the adapter.

Required example:

```text
[ReflexProviderGate] seq=7 area=DXGI api=EnumAdapterByGpuPreference caller=pragmata.exe rva=0x00ABCDEF adapterIndex=0 preference=HIGH_PERFORMANCE hr=S_OK
```

This data answers whether the game immediately performs an adapter-selection step after Streamline reports Reflex support.

If no useful game-EXE DXGI callsite appears, stop. Do not expand DXGI instrumentation in the same PR.

A later PR may add read-only `GetDesc*` hooks only if this first-stage evidence justifies it.

---

# 8. Part D — Optional Existing NVAPI QueryInterface Observation

NVAPI is now secondary evidence, not the primary diagnostic target.

Only modify the already-existing:

```cpp
NvApiHooks::hkNvAPI_QueryInterface(unsigned int InterfaceId)
```

if useful.

The existing call path must remain exactly:

```cpp
const auto functionPointer = o_NvAPI_QueryInterface(InterfaceId);
```

Do not replace it with:

```cpp
fakenvapi::queryInterface(...)
```

or any caller-aware alternative.

Do not move the original call to a new helper.

Do not bypass `ReflexHooks` routing.

Do not add fake-NVAPI wrappers.

After the existing logic obtains the result, record unique QueryInterface callsites only:

```text
caller module
caller RVA
interface ID
returned pointer null/non-null
```

No function invocation beyond what the existing code already performs.

A raw interface ID is sufficient for unknown IDs. Name resolution is optional if it can be performed from compile-time/static tables without changing runtime routing.

Recommended maximum:

```text
64 unique NVAPI QueryInterface records per process
```

If there is still no direct `re9.exe` / `pragmata.exe` NVAPI callsite, consider that a useful negative result and stop expanding NVAPI tracing.

---

# 9. Diagnostic Scope / Activation

The diagnostic must not run globally.

Primary target executables:

```text
re9.exe
pragmata.exe
```

Positive-control executables may be enabled explicitly for comparison:

```text
dd2.exe
MonsterHunterWilds.exe / actual existing executable name in current game quirk table
```

Do not guess executable names; reuse existing game identification / quirk logic where possible.

AMD RE9 / PRAGMATA should use the same diagnostic code path if an AMD runtime capture becomes available.

Do not make the diagnostic Intel-only at the logging helper level if that would prevent AMD comparison. Instead scope by target game and record the actual primary vendor in every provider-decision record.

The important comparison is:

```text
same game + Intel
same game + AMD
```

not only Intel in isolation.

---

# 10. Unified Sequence Number

Use one monotonic process-local diagnostic sequence across all areas if practical:

```text
SL
LL
DXGI
NVAPI
```

Example:

```text
[ReflexProviderGate] seq=1 area=LL phase=decision ...
[ReflexProviderGate] seq=2 area=SL api=slGetFeatureRequirements ...
[ReflexProviderGate] seq=3 area=SL api=slIsFeatureSupported result=eOk caller=pragmata.exe rva=...
[ReflexProviderGate] seq=4 area=LL phase=decision vendor=Intel vendorMode=XeLL xefgForce=true finalMode=XeLL ...
[ReflexProviderGate] seq=5 area=DXGI api=EnumAdapterByGpuPreference ...
```

This gives the runtime log chronological meaning without requiring high-volume tracing.

An atomic `uint32_t` / `uint64_t` sequence counter is acceptable.

---

# 11. Explicitly Forbidden Changes

The implementation PR must not contain any of the following:

```text
NO function-pointer substitution for diagnostics
NO alternate NvAPI_QueryInterface path
NO fakenvapi::queryInterfaceWithCaller-style routing
NO new fake-NVAPI behavior
NO forced NVAPI_OK
NO Streamline return override for Reflex
NO slIsFeatureLoaded forced true
NO ReflexState mutation
NO DXGI VendorId / DeviceId mutation
NO DXGI spoofing enablement
NO extra GetDesc* calls merely for logging
NO new provider init/deinit call for diagnostics
NO extra XeLL / AntiLag2 API call for diagnostics
NO per-frame Reflex marker logging
NO per-frame XeFG logging
NO stack walking
NO symbol resolution
NO DbgHelp
NO production fix in this PR
```

If the required evidence cannot be collected without violating one of these rules, stop and report the limitation instead of broadening the implementation.

---

# 12. Safety Requirements Learned from PR #8

Before runtime testing, review the final diff specifically for execution-path equivalence.

For every instrumented wrapper confirm:

```text
original function is called the same number of times
original function receives identical arguments
original function is called in the same branch as before
return value is unchanged
output parameters are unchanged
function pointer returned to the game is unchanged
hook attachment set is unchanged
```

A useful review technique is to compare each edited function with baseline `75e6c7d9` and mentally remove only the logging statements. The remaining control flow should be equivalent to baseline.

If removing diagnostic statements does not reconstruct the baseline control flow, the patch is too invasive.

---

# 13. Runtime Test Order

Do not collect all games immediately.

## Stage 1 — PRAGMATA Intel launch-safety test

First run only:

```text
Intel GPU
PRAGMATA
same configuration that launches successfully on 75e6c7d9
```

Acceptance for this first run:

```text
game launches normally
no Streamline exception / render-stop regression
XeFG continues operating normally
bounded [ReflexProviderGate] records appear
no log flood
```

If PRAGMATA crashes or stops rendering, stop immediately and compare the diagnostic diff against `75e6c7d9`.

Do not proceed to RE9 until launch safety is established.

## Stage 2 — RE9 Intel

Capture the same diagnostic timeline.

## Stage 3 — working Intel control

Prefer DD2 because Intel Reflex spoofing is known to work there.

The goal is to compare the first point where DD2 continues into Reflex while RE9 / PRAGMATA stop.

## Stage 4 — AMD RE9 / PRAGMATA if available

This is especially valuable for the provider hypothesis.

The critical question is whether AMD shows a provider timeline such as:

```text
vendorMode=AntiLag2
xefgForce=false initially
Reflex availability succeeds
later fgOutput=XeFG causes finalMode=XeLL
```

or whether AMD is already forced to XeLL before the Reflex decision.

Either result is useful.

---

# 14. Questions the Next Logs Must Answer

The next runtime evidence should answer all of the following.

## Q1. At the moment the game queries Reflex support, what low-latency provider decision has already occurred?

Specifically:

```text
configured mode
vendor-resolved mode
XeFG force state
final desired mode
active output
active provider
```

## Q2. Does Intel reach `slIsFeatureSupported(Reflex)=eOk` before or after XeFG has forced XeLL?

This timing matters.

## Q3. Does a working AMD run expose AntiLag2 during the availability stage and only switch to XeLL later?

If yes, the provider-timing hypothesis becomes strong.

## Q4. Does a working AMD run already use XeLL at the same point?

If yes, the provider-type hypothesis becomes weaker and investigation should move toward Intel-specific identity/capability state.

## Q5. What exact game callsite receives the successful Reflex support result?

Need:

```text
pragmata.exe + RVA
re9.exe + RVA
```

This allows later static/disassembly analysis of the game-side predicate immediately following the Streamline call.

## Q6. Does the game directly query DXGI or NVAPI immediately after that callsite?

If yes, use that as the next targeted diagnostic.

If no, do not keep expanding those systems blindly.

---

# 15. Decision Tree After Runtime Evidence

## Case A — AMD uses AntiLag2 during availability, Intel uses XeLL, AMD works

Then the next investigation should focus narrowly on what availability/state the game sees from those two provider paths.

Do not immediately spoof DXGI vendor ID.

Do not immediately force Intel to AntiLag2 in production.

A separate proof-only experiment may later test whether matching the working provider state changes the menu gate.

## Case B — AMD and Intel both use XeLL during availability, but only Intel fails

Provider selection alone is not the cause.

Next priority:

```text
Intel-specific adapter identity / capability signal
DXGI GetDesc* caller-aware read-only tracing
D3D12 device -> adapter identity path
specific game-side predicate at the captured caller RVA
```

## Case C — working DD2 continues with additional Streamline feature calls but RE9/PRAGMATA do not

Use the captured game RVA to inspect the immediate post-support-check game logic before adding more broad instrumentation.

## Case D — direct game NVAPI call appears

Resolve only the specific interface ID(s) unique to failing games.

Do not restore PR #8's broad NVAPI instrumentation.

## Case E — direct game DXGI adapter query appears

A second diagnostic PR may add read-only `GetDesc*` hooks.

That later hook must return the original descriptor unchanged and must not enable normal OptiScaler DXGI spoofing.

---

# 16. Suggested File Scope for First Implementation PR

Expected files should be limited approximately to:

```text
OptiScaler/hooks/Streamline_Hooks.cpp
OptiScaler/low_latency/input/input_common.cpp
OptiScaler/hooks/DxgiFactory_Hooks.cpp        # optional first-stage caller/RVA only
OptiScaler/nvapi/NvApiHooks.cpp              # optional QueryInterface observation only
```

A small dedicated diagnostic helper may be added if it materially reduces duplication, for example:

```text
OptiScaler/misc/ReflexProviderDiag.h
OptiScaler/misc/ReflexProviderDiag.cpp
```

If adding a helper, keep it observational only. It must not own or redirect any existing OptiScaler function pointer.

Avoid touching:

```text
OptiScaler/nvapi/fakenvapi.cpp
OptiScaler/nvapi/fakenvapi.h
OptiScaler/nvapi/fakenvapi/nvapi_calls.cpp
OptiScaler/spoofing/Dxgi_Spoofing.cpp
XeFG dispatch / frame-generation hot paths
```

unless a later evidence-driven work order explicitly requires them.

---

# 17. Validation Before Runtime

Required static validation:

```text
git diff --check
clang-format / repository formatting check for touched files
Release x64 build
```

Additionally perform an execution-path review against:

```text
75e6c7d9c659a04d5430176f080794b2371a1c63
```

For each touched function, document in the PR description that:

```text
no return value changed
no output data changed
no new underlying API call was introduced
no original API call was removed or rerouted
no new hot-path hook was attached
```

CI/build success is not enough to merge this diagnostic PR.

Runtime launch validation is mandatory.

---

# 18. PR Requirements

Create the implementation as a **Draft PR**.

Do not target `master`.

Do not merge before PRAGMATA Intel runtime testing.

The PR description should include:

```text
Known-good baseline: 75e6c7d9
PR #8 regression explicitly avoided
No execution-path changes
No behavioral Reflex fix
No new provider call
No new DXGI/NVAPI capability override
```

After runtime logs are collected, update the PR description with a compact matrix:

```text
Game       GPU     Reflex UI   vendorMode   XeFG force at Reflex check   finalMode   next observed gate
PRAGMATA   Intel   No          ...          ...                           ...         ...
RE9        Intel   No          ...          ...                           ...         ...
DD2        Intel   Yes         ...          ...                           ...         ...
PRAGMATA   AMD     Yes/NA      ...          ...                           ...         ...
```

---

# 19. Acceptance Criteria

The first implementation PR is complete only when all of the following are true:

1. It is based on the known-good `75e6c7d9` lineage, not PR #8 runtime code.
2. It does not alter the `NvAPI_QueryInterface` routing path.
3. It does not modify fake-NVAPI behavior.
4. It does not alter Streamline Reflex results.
5. It does not alter low-latency provider selection.
6. It records Reflex Streamline caller module + RVA + original result.
7. It records vendor-resolved low-latency mode before XeFG force.
8. It records whether XeFG force is active.
9. It records final desired low-latency mode after XeFG force.
10. It records active input/output/provider state without invoking extra provider APIs.
11. Logging is bounded and not per-frame.
12. PRAGMATA Intel launches and continues rendering normally with the diagnostic build.
13. The runtime log is sufficient to decide whether the next step is provider-specific, game-callsite-specific, DXGI-specific, or NVAPI-specific.

Do not claim the root cause is fixed merely because these diagnostics compile or because the provider differs between vendors.

The purpose of this PR is to obtain the evidence required for the next minimal proof patch.
