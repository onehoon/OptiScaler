# PRAGMATA Intel Reflex Root-Cause Analysis and Production Handoff

Date: 2026-09-09  
Repository: `onehoon/OptiScaler`  
Investigation branch: `poc_reflex`  
Target games: `PRAGMATA.exe`, `re9.exe`  
Primary GPU under test: Intel Arc B390  
Primary scenario: DLSS-G input -> XeFG output with XeLL / Streamline spoofing enabled

---

## 1. Purpose of this document

This document consolidates the completed PRAGMATA Intel Reflex investigation so the next implementation pass can prepare a production cleanup PR without repeating the reverse engineering and diagnostic POC work.

The important conclusion is now narrow:

> PRAGMATA can expose and initialize the Reflex path on Intel when **only calls originating from `PRAGMATA.exe` receive the spoofed NVIDIA DXGI adapter identity**, while global DXGI spoofing remains disabled.

This result strongly shifts the root cause away from Streamline plugin loading itself and toward a **game-side adapter/vendor capability decision made from DXGI adapter identity**.

The POC selector is diagnostic scaffolding. It should not be retained as a production user-facing option after the required caller scope is confirmed for both target games.

---

## 2. Baseline symptom

On Intel + OptiScaler in PRAGMATA / Resident Evil Requiem:

- DLSS upscaling can be unlocked.
- DLSS-G input can be used with XeFG output.
- XeFG itself runs.
- NVIDIA Reflex is unavailable in the game UI / game capability path.
- In the failing PRAGMATA path, Streamline Reflex support queries can still report `eOk`.
- Despite that, the game does not proceed into the normal Reflex lifecycle.
- XeFG dispatches therefore receive `Reflex Id: 0`.

By comparison:

- DD2 and Monster Hunter Wilds can reach the Reflex lifecycle on Intel under the existing compatibility stack.
- AMD was also observed to expose Reflex in the target-game scenario where Intel failed.

Therefore this was not a simple "non-NVIDIA GPU => Reflex disabled" condition.

---

## 3. Earlier hypotheses that were tested

### 3.1 NVAPI `GetSleepStatus` fallback

An early POC added a fallback around `NvAPI_D3D_GetSleepStatus` under:

```cpp
GameQuirk::FixSlReflexAvailabilityOnIntel
```

The fallback returned a successful status when the Intel target-game path failed.

This did **not** explain the root cause because failing PRAGMATA / RE9 runs never advanced far enough to acquire/use the normal Reflex function lifecycle before the availability decision was already lost.

Production implication:

- The old `GetSleepStatus` fallback should be treated as legacy diagnostic logic.
- It is a cleanup/removal candidate once the final DXGI identity fix is proven.

Do not preserve it merely because the existing quirk name sounds correct.

### 3.2 Streamline Reflex plugin support

Failing runs showed that the Reflex plugin could load and support checks could return `eOk`.

Therefore the failure is not simply:

```text
sl.reflex missing
or
slIsFeatureSupported(Reflex) != eOk
```

The relevant failure occurs between basic support discovery and the game choosing to enter its Reflex runtime path.

---

## 4. PRAGMATA targeted reverse-engineering results

Static analysis was intentionally restricted to the feature capability path around:

```text
g_featureMask @ RVA 0xBB89D28
```

No EXE patching, bypassing, or anti-tamper work was performed.

### 4.1 Streamline feature IDs

From Streamline:

```cpp
kFeatureReflex = 3
kFeaturePCL    = 4
```

### 4.2 PCL producer is real

PRAGMATA separately queries PCL before the later feature-list loop:

```asm
RVA 0x4B791FF:
    mov ecx, 4

RVA 0x4B79207:
    call qword ptr [rip + ...]     ; slIsFeatureSupported(kFeaturePCL)

RVA 0x4B7920D:
    test eax, eax

RVA 0x4B7920F:
    sete dl

RVA 0x4B79212:
    mov ecx, 2

RVA 0x4B79217:
    call 0x1487732C0              ; set bit 2
```

Equivalent logic:

```cpp
bool pclSupported =
    slIsFeatureSupported(kFeaturePCL, ...) == sl::Result::eOk;

SetFeatureCapabilityBit(2, pclSupported);
```

Thus `kFeaturePCL` is genuinely queried and contributes to the internal feature mask.

### 4.3 Earlier feature-list contradiction was resolved

The earlier observed list:

```text
{ 0, 3, 1000, 1001 }
```

is not the complete Streamline support-query list.

It is a separate later dynamic array containing:

```text
0     DLSS
3     Reflex
1000  DLSS-G
1001  DLSS-RR
```

PCL is queried separately before this list, so absence of raw feature `4` from that array does not mean PCL is skipped.

The containing capability-init function start is:

```text
RVA 0x4B791C0
```

not the earlier preliminary `0x4B787C0` estimate.

### 4.4 Reflex mapping

Reflex raw feature ID `3` maps to internal bit `1`:

```text
Reflex raw ID 3 -> internal bit 1 -> mask 0x2
PCL raw ID 4    -> internal bit 2 -> mask 0x4
```

### 4.5 PCL + Reflex conjunction exists

The mask reader is:

```asm
RVA 0x566F040:
    mov eax, [g_featureMask]
    bt  eax, ecx
    setb al
    ret
```

A confirmed consumer performs:

```cpp
bool availability = false;

if (g_featureMask & 0x4)               // PCL bit 2
    availability = (g_featureMask & 0x2) != 0; // Reflex bit 1

object->field_8 = availability;
```

Therefore the PCL && Reflex conjunction is real.

### 4.6 `field_8` is not proven to be the direct Reflex UI gate

`field_8` is read in a path that controls whether feature ID `0` / DLSS can be selected or accepted:

```text
PCL && Reflex
    -> field_8
    -> DLSS feature selection / acceptance path
```

A getter also exists at:

```text
RVA 0x725B3A0
```

However static analysis did **not** prove that `field_8` directly controls:

- Reflex UI visibility,
- `slReflexGetState`,
- `slReflexSleep`, or
- `slReflexSetOptions` acquisition.

Therefore PCL remains relevant capability state, but it should no longer be treated as the primary root-cause hypothesis without runtime evidence.

---

## 5. The decisive DXGI spoof A/B result

OptiScaler has two relevant spoofing dimensions:

```ini
StreamlineSpoofing=true
Dxgi=false|true
```

The important observation was:

```text
Intel + StreamlineSpoofing=true + global DXGI spoof OFF
    -> Reflex unavailable

Intel + StreamlineSpoofing=true + global DXGI spoof ON
    -> Reflex available
```

Global DXGI spoof ON is not acceptable as the production solution because it causes performance loss and exposes the spoofed NVIDIA identity too broadly.

This led to the caller-selective POC.

---

## 6. What global DXGI spoof changed

With global DXGI spoof OFF, Streamline `sl.common` sees the real Intel adapter identity.

With global DXGI spoof ON, the existing DXGI adapter hooks change `GetDesc*` results to the configured spoofed NVIDIA identity.

That causes Streamline `sl.common` to enter its NVIDIA system-caps path and invoke the fake NVAPI stack, producing logs such as:

```text
NVIDIA driver 999.99
architecture 0x190
```

Current upstream Streamline source explains this behavior:

```text
IDXGIAdapter::GetDesc
    -> desc.VendorId
    -> NVIDIA adapter detected
    -> NVAPI driver / architecture query path
    -> SystemCaps populated
```

Streamline Reflex also derives its internal low-latency availability from NVIDIA driver / architecture capability state.

This made `sl.common` SystemCaps a strong hypothesis, but it was not enough to prove that `sl.common` was the actual caller that needed spoofing.

---

## 7. Selective DXGI identity POC

PR #11 introduced diagnostic selector scaffolding:

```ini
[Spoofing]
Dxgi=false
StreamlineSpoofing=true
ReflexDxgiIdentityScope=Off
```

Supported diagnostic values:

```text
Off
SlCommonOnly
GameOnly
SlCommonAndGame
```

The POC is gated by:

- Intel primary GPU,
- `GameQuirk::FixSlReflexAvailabilityOnIntel`,
- `StreamlineSpoofing=true`,
- global `DxgiSpoofing=false`, and
- no active `SkipSpoofing()` exclusion.

It mutates only:

- `VendorId`,
- `DeviceId`, and
- adapter `Description`

for the selected caller scope.

It does not intentionally spoof XeSS / XeFG / XeLL / D3D12 system callers.

---

## 8. Runtime test result that changes the root-cause assessment

The most important runtime result is:

```text
ReflexDxgiIdentityScope=GameOnly
    -> Reflex becomes available
```

The user confirmed this directly in PRAGMATA.

The `GameOnly` log in the Drive test set records:

```text
Spoofing.ReflexDxgiIdentityScope: GameOnly
Quirk: Fix Streamline Reflex availability on Intel
```

The test build is:

```text
OptiScaler v10.0.0-dev (d537a323) (20260909_213301)
```

The current Drive folder `pragmata/4tests` contains three uploaded logs at the time this document was written:

```text
off_OptiScaler.log
slcommonlnly_OptiScaler.log
gameonly_OptiScaler.log
```

Despite the folder name, the `SlCommonAndGame` log was not present in that folder when checked. Do not claim a four-file runtime matrix from the Drive contents unless a fourth file is uploaded later.

### Why `GameOnly` is decisive

`GameOnly` means the selective spoof is applied to calls originating from:

```text
PRAGMATA.exe
```

while `sl.common.dll` and Intel/Xe runtime callers can continue to see the real Intel identity.

Therefore the previous strongest hypothesis:

```text
sl.common must see NVIDIA identity or Reflex cannot initialize
```

is no longer required to explain the observed success.

The new strongest causal interpretation is:

```text
PRAGMATA.exe queries DXGI adapter identity
    -> game-side vendor / capability decision
    -> Intel identity blocks or omits Reflex initialization
    -> NVIDIA identity allows Reflex initialization
```

The exact PRAGMATA branch consuming adapter identity has not been statically identified yet, but identifying that branch is no longer necessary for a minimal OptiScaler compatibility fix if caller-selective identity spoofing is stable.

---

## 9. Current root-cause ranking

### 1. Game-side DXGI adapter identity / vendor capability gate — strongest

Evidence:

- global DXGI spoof ON changes failure to success,
- `GameOnly` selective spoof also changes failure to success,
- broad Streamline plugin support can already be `eOk` in failure runs.

This is the primary production target.

### 2. Streamline `sl.common` SystemCaps — secondary / not required by current success result

DXGI ON definitely changes `sl.common` SystemCaps to NVIDIA-like values, but `GameOnly` success shows that this broad change is not necessarily required for PRAGMATA Reflex availability.

Do not build the final fix around `sl.common` unless further RE9 testing proves it is required there.

### 3. PCL internal capability bit — real but downstream / supporting

PCL is genuinely queried and combined with Reflex in PRAGMATA's capability state.

However the known `field_8` consumer is broader than a direct Reflex UI switch, and the decisive `GameOnly` result points upstream toward game-side adapter identity.

PCL should remain logged in the final diagnostic validation but should not be the first code path modified.

### 4. NVAPI `GetSleepStatus` fallback — legacy hypothesis

This occurs too late to explain the original failure path and should be removed or justified independently in the production cleanup.

---

## 10. Recommended production implementation

The production fix should be quirk-driven, not a permanent user-facing INI selector.

### 10.1 Reuse the existing quirk

Use the existing:

```cpp
GameQuirk::FixSlReflexAvailabilityOnIntel
```

Do not add a second overlapping quirk unless RE9 behavior requires materially different policy.

### 10.2 Intended final behavior

Conceptually:

```cpp
if (State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailabilityOnIntel)
{
    if (primaryGpu.vendorId == VendorId::Intel &&
        Config::Instance()->StreamlineSpoofing.value_or_default() &&
        !Config::Instance()->DxgiSpoofing.value_or_default() &&
        IsTargetGameCaller(caller) &&
        !SkipSpoofing())
    {
        ApplyConfiguredNvidiaIdentity(desc);
    }
}
```

Target game callers currently mean:

```text
PRAGMATA.exe
re9.exe
```

The important property is:

> The game executable sees the spoofed NVIDIA adapter identity, while unrelated modules continue to see the real Intel adapter identity.

### 10.3 Global DXGI spoof must remain OFF

Production behavior must not silently enable:

```ini
Dxgi=true
```

The whole purpose of the selective fix is to avoid the performance and compatibility cost of broad DXGI identity spoofing.

### 10.4 Preserve Intel identity for runtime modules

Unless a future game-specific test proves otherwise, do not spoof the NVIDIA identity to:

- `sl.common.dll`,
- `libxess.dll`,
- `libxess_fg.dll`,
- `libxell.dll`,
- IntelControlLib,
- `d3d12.dll`,
- `d3d12Core.dll`,
- `dxgi.dll`,
- Vulkan callers,
- other system/runtime callers.

This is important both for correctness and for avoiding the performance regression seen with broad DXGI spoofing.

---

## 11. POC code that should be considered temporary

The following items were introduced specifically to isolate the caller requirement and should normally be removed from the final production branch/PR:

### 11.1 User-facing diagnostic selector

Remove:

```ini
ReflexDxgiIdentityScope=Off|SlCommonOnly|GameOnly|SlCommonAndGame
```

and its general-purpose config plumbing once the target behavior is hard-coded behind the quirk.

Production users should not need to know this option exists.

### 11.2 Generic scope helper

`misc/ReflexDxgiIdentityScope.h` is useful for the POC matrix but is probably unnecessary in the production implementation if the only final policy is:

```text
FixSlReflexAvailabilityOnIntel + Intel + target-game executable caller
```

Prefer the simplest existing project pattern for quirk/caller checks.

### 11.3 Excessive POC logging

`[ReflexDxgiPOC]` logging is useful during validation but should either:

- be removed,
- be downgraded to debug/trace, or
- be reduced to a one-shot bounded diagnostic if support value justifies keeping it.

Avoid permanent noisy per-GetDesc logging.

### 11.4 Legacy `GetSleepStatus` fallback

Review the existing code behind `FixSlReflexAvailabilityOnIntel` that fakes a successful `NvAPI_D3D_GetSleepStatus` result.

If runtime validation shows the new selective identity fix works without that fallback, remove the fallback in the production cleanup PR.

Do not preserve two unrelated fixes under one quirk without evidence that both are needed.

---

## 12. Diagnostics worth retaining until final validation

The feature-aware Streamline diagnostic improvement is valuable during final validation:

```text
[ReflexProviderGate] area=SL feature=PCL ...
[ReflexProviderGate] area=SL feature=Reflex ...
```

The diagnostic key now includes `sl::Feature`, preventing PCL and Reflex calls from incorrectly deduplicating each other at a shared callsite.

During final validation, confirm separately:

```text
PCL slIsFeatureSupported result
Reflex slIsFeatureSupported result
slReflexGetState function acquisition
slReflexSleep function acquisition/use
slReflexSetOptions function acquisition/use
```

After root-cause confirmation on both target games, decide whether this extended logging belongs in production or should remain only on the investigation branch.

---

## 13. Production PR validation matrix

The next PR should not be considered complete from compile-only validation.

### 13.1 PRAGMATA / Intel — mandatory

Configuration baseline:

```ini
Dxgi=false
StreamlineSpoofing=true
FGInput=DLSSG
FGOutput=XeFG
```

Expected:

- no diagnostic selector required,
- quirk auto-applies,
- Reflex UI / setting is available,
- DLSS upscaling remains available,
- DLSS-G input remains available,
- XeFG output works,
- XeSS/XeFG/XeLL still identify/use Intel paths normally,
- no global NVIDIA DXGI identity leakage,
- no performance regression relative to the normal `Dxgi=false` baseline.

### 13.2 Reflex runtime evidence — mandatory

Confirm the successful game path reaches normal Reflex lifecycle evidence, ideally including:

```text
slReflexGetState
slReflexSleep
slReflexSetOptions
```

and XeFG Reflex synchronization should no longer remain permanently at:

```text
Reflex Id: 0
```

### 13.3 RE9 / Intel — mandatory before generalizing the final quirk

The same target quirk is currently attached to RE9.

Validate that the game-executable-only selective identity is sufficient there as well.

Possible outcomes:

1. `re9.exe` game-only identity succeeds
   - share the exact same quirk policy.

2. RE9 needs a different caller combination
   - do not broaden PRAGMATA policy globally.
   - split the game-specific handling internally while keeping user-facing behavior automatic.

3. RE9 does not need the fix anymore under current game/build state
   - narrow the quirk accordingly.

### 13.4 Negative controls

At minimum verify:

- AMD: no new selective Intel path.
- NVIDIA: no new selective Intel path.
- DD2: no behavior change.
- Monster Hunter Wilds: no behavior change.
- non-target games: no selective game-caller spoof.
- global `Dxgi=true`: existing broad spoof behavior remains authoritative; selective quirk must not double-apply.
- `StreamlineSpoofing=false`: selective Reflex identity fix should not apply unless there is a separately justified policy.

---

## 14. Performance validation

This investigation began because global DXGI spoof made Reflex work but caused performance loss.

Therefore the final fix must be compared against the real baseline:

```text
Baseline:
Dxgi=false
(no broad DXGI spoof)

Candidate:
Dxgi=false
+ target-game-only Reflex quirk
```

Do not compare only against `Dxgi=true`.

Acceptance target:

> The selective quirk should behave like the normal `Dxgi=false` performance path while enabling Reflex.

If measurable regression remains, inspect whether the descriptor hooks themselves are being installed/called more broadly than necessary. `GetDesc*` is not expected to be a hot per-frame path, but the final implementation should still avoid unnecessary policy work for unrelated callers.

---

## 15. Do not do these things in the production PR

Do not:

- force global `Dxgi=true`,
- spoof every DXGI caller,
- spoof Intel/XeSS/XeFG/XeLL modules without evidence,
- force `slIsFeatureSupported` return values merely to make Reflex appear,
- patch PRAGMATA / RE9 executable memory,
- add anti-tamper bypass logic,
- modify unrelated XeFG swapchain code,
- refactor the general DXGI spoofing system beyond what is needed for this compatibility fix,
- keep the four-way POC selector as a permanent user-facing feature,
- merge the entire experimental `poc_reflex` history into `master` unchanged.

---

## 16. Branch / extraction policy

`poc_reflex` is an experimental diagnostic lineage containing disproven hypotheses and instrumentation.

Once PRAGMATA + RE9 runtime validation is complete:

1. Start a **fresh production branch from the latest `master`**.
2. Extract only the minimum confirmed fix.
3. Reuse the final quirk semantics.
4. Bring over only diagnostics that are intentionally useful in production.
5. Do not merge the full POC branch history wholesale.

The expected production diff should be substantially smaller than the current POC lineage.

---

## 17. Suggested production PR shape

A likely final PR can be small:

### Core change

- Extend/refine `GameQuirk::FixSlReflexAvailabilityOnIntel` so that on Intel target games, when global DXGI spoofing is disabled, only the game executable's adapter descriptor queries receive the configured NVIDIA identity.

### Cleanup

- remove `ReflexDxgiIdentityScope` INI/config plumbing,
- remove generic POC scope helper if no longer needed,
- remove/downgrade `[ReflexDxgiPOC]` logs,
- remove old `GetSleepStatus` fallback if validation proves unnecessary,
- keep feature-aware PCL/Reflex diagnostics only if deliberately desired.

### Tests

Add focused policy-level tests around the extracted predicate where practical:

```text
PRAGMATA + Intel + quirk + SL spoof + Dxgi=false + caller=PRAGMATA.exe -> true
RE9      + Intel + quirk + SL spoof + Dxgi=false + caller=re9.exe      -> true
sl.common.dll                                                   -> false
Intel runtime / DXGI / D3D12 callers                            -> false
AMD                                                             -> false
NVIDIA                                                          -> false
non-target game                                                 -> false
Dxgi=true                                                       -> selective path false
StreamlineSpoofing=false                                        -> false
SkipSpoofing=true                                               -> false
```

---

## 18. Final causal model for handoff

The investigation should now be handed off with this model:

```text
PRAGMATA + Intel + Dxgi=false
    -> game sees real Intel DXGI adapter identity
    -> game-side capability/vendor decision does not enter Reflex runtime path
    -> basic Streamline Reflex support may still be eOk
    -> no normal Reflex lifecycle
    -> XeFG Reflex IDs remain unusable / zero

PRAGMATA + Intel + GameOnly selective identity
    -> PRAGMATA.exe sees spoofed NVIDIA DXGI adapter identity
    -> unrelated runtime modules can still see Intel
    -> game enables the Reflex path
    -> global DXGI spoof is not required
```

This is currently the strongest evidence-backed root-cause explanation.

The exact internal PRAGMATA vendor branch does not need to be reverse engineered further unless the selective production fix shows instability or RE9 behaves differently.

---

## 19. Source / evidence locations

Investigation logs are under the user's Drive hierarchy:

```text
C:\GoogleDrive\ref-xefg\reflex\log\pragmata\4tests
```

Observed files when this handoff was written:

```text
off_OptiScaler.log
slcommonlnly_OptiScaler.log
gameonly_OptiScaler.log
```

Earlier comparison logs also exist under the PRAGMATA DXGI-off and DXGI-on investigation folders.

Relevant POC PR:

```text
onehoon/OptiScaler PR #11
Add selective DXGI identity POC for Intel Reflex
```

Relevant investigation quirk:

```cpp
GameQuirk::FixSlReflexAvailabilityOnIntel
```

Relevant core files in the POC lineage include:

```text
OptiScaler/spoofing/Dxgi_Spoofing.cpp
OptiScaler/hooks/Streamline_Hooks.cpp
OptiScaler/misc/ReflexDxgiIdentityScope.h
OptiScaler/misc/ReflexProviderDiag.cpp
OptiScaler/misc/ReflexProviderDiag.h
OptiScaler/Config.cpp
OptiScaler/Config.h
OptiScaler.ini
```

---

## 20. Handoff completion criteria

The next developer should be able to proceed directly to a production PR when the following are true:

- [x] PRAGMATA root-cause scope narrowed to game-side DXGI identity.
- [x] Global DXGI spoof shown unnecessary for PRAGMATA when `GameOnly` is used.
- [x] PCL producer / bit mapping statically resolved.
- [x] PCL+Reflex conjunction confirmed.
- [x] `field_8` shown to be broader than a proven direct Reflex UI gate.
- [x] PCL/Reflex diagnostic logging made feature-aware in the POC.
- [ ] RE9 `GameOnly` runtime validation completed.
- [ ] PRAGMATA successful `GameOnly` log fully checked for complete Reflex lifecycle / non-zero Reflex IDs.
- [ ] Performance checked against the real `Dxgi=false` baseline.
- [ ] Fresh production branch created from latest `master`.
- [ ] Minimal confirmed fix extracted from POC lineage.

Until the unchecked runtime items are complete, keep changes in the POC/diagnostic lineage rather than merging the full experiment into `master`.
