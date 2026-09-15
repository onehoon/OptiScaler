# release/0.9 + fork REFramework Capcom end-to-end compatibility crash audit

Date: 2026-09-15

## 1. Immutable audit baseline

This audit is intentionally pinned to the following revisions.

- OptiScaler fork: `onehoon/OptiScaler`
- OptiScaler branch: `reframework-0.9`
- OptiScaler audited HEAD: `e962942b325033578b249ddc27172322228a48d1`
- Fork REFramework: `onehoon/REFramework`
- REFramework audited branch: `master`
- REFramework audited HEAD: `4bf45b370e602f7f6a3ca54f308daa4e353aab8a`
- OptiScaler fork `master` reference-only HEAD: `64874932da7ceb3475ed1e8fd8aaf837249f022a`

The OptiScaler `master` branch is **not** an implementation authority for this audit. It may be used to understand intent or compare patterns, but no finding or recommendation is based on an assumption that master is newer, more correct, or architecturally preferable to `reframework-0.9`.

## 2. Audit principle — the scope is compatibility crashes, not code cleanup

This is the most important rule of the audit.

The primary question is **not** whether the current OptiScaler XeFG/XeLL implementation is the cleanest implementation, whether every path matches a preferred Intel SDK interpretation, or whether either project would benefit from general refactoring.

The primary question is:

> In a Capcom game running the forked REFramework together with OptiScaler `reframework-0.9`, can a lifecycle, generation, ownership, hook-order, or synchronization mismatch between the two components create a real game crash, hang, use-after-free, deadlock, stale callback, or device failure?

The known practical baseline is that the forked 0.9 code is generally usable in non-Capcom titles without this REF compatibility layer. The reason for the compatibility work is that Capcom games require REFramework and therefore add a second D3D12/DXGI/presentation lifecycle owner/observer into the process.

Accordingly, this audit does **not** attempt to turn `reframework-0.9` into a general rewrite of OptiScaler.

### 2.1 A finding must cross the OptiScaler ↔ REF boundary

A problem is in scope when the following kind of chain can be demonstrated:

```text
OptiScaler believes lifecycle/generation/ownership state = A
        ↓
REFramework still observes or owns incompatible state = B
        ↓
that disagreement can reach a live D3D12 / COM / hook / XeFG operation
        ↓
AV / UAF / deadlock / stale callback / wrong-generation release /
device removal / unsafe recreation
```

Examples of in-scope failure classes are:

- REF retaining a borrowed presentation object after Opti consumes the last safe owner.
- Opti entering XeFG teardown while REF still holds its hook-monitor lifecycle over that presentation path.
- opposite lock ordering between REF hook-monitor and Opti FG teardown mutexes.
- REF binding generation N while Opti has already retired generation N and committed generation N+1.
- the two sides retaining different queue/device lifetimes for the same XeFG generation in a way that permits one side to access a released object.
- REF keeping a stale internal XeFG binding after an Opti initialization abort or failed vendor destroy.
- a compatibility handoff being skipped specifically because of Capcom/REF module-hiding or loader behavior.

### 2.2 Classification used by this audit

#### CRASH-RELEVANT

A cross-component state mismatch has a concrete path to an AV, UAF, deadlock, invalid COM call, unsafe hook access, or device failure. The exact timing may still be workload-dependent.

#### COMPAT-RISK

A real OptiScaler ↔ REF state mismatch is present, but a crash additionally requires an uncommon hook-install failure, vendor failure, timing window, or another condition that is not part of the normal successful path.

#### VERIFIED / NO REMAINING MISMATCH FOUND

The audited cross-component path has an explicit ownership/order/reconciliation mechanism and no remaining source-proven crash path was found at the pinned revisions.

#### NOT IN SCOPE

Examples:

- an Opti-only pointer or loader pattern that does not become more dangerous because REF is present,
- a general Intel XeFG/XeLL specification discussion with no REF lifecycle consequence,
- style/refactoring concerns,
- optional telemetry or logging behavior,
- process-exit cleanup that is not used during live gameplay,
- changing architecture simply because another Opti branch implements it differently.

These items may deserve independent engineering work, but they are deliberately not converted into compatibility blockers here.

## 3. End-to-end runtime model audited

The audit followed the paired runtime from process start through retirement rather than starting from P5/P6/P7 patches.

```text
Capcom process start
 │
 ├─ REFramework dinput8 bootstrap
 │   ├─ integrity/anti-tamper preparation
 │   └─ startup thread
 │
 ├─ OptiScaler bootstrap / proxy / hook setup
 │
 ├─ REF XeFG runtime discovery
 │   ├─ Ldr notification
 │   ├─ LdrLoadDll post-load handoff
 │   └─ already-loaded libxess_fg scan
 │
 ├─ DXGI / D3D12 hooks from both projects
 │
 ├─ game swapchain creation intercepted by Opti
 │
 ├─ Opti XeFG context + XeLL context creation
 │
 ├─ Intel InitFromSwapChainDesc
 │   └─ REF observes the internal presentation swapchain and queue
 │
 ├─ REF semantic binding / instance hook commit
 │
 ├─ Opti GetSwapChainPtr and public proxy commit
 │
 ├─ Present / Present1
 │   ├─ REF hook_monitor_mutex
 │   └─ Opti FG mutex / XeFG work
 │
 ├─ ResizeTarget / ResizeBuffers / ResizeBuffers1
 │
 ├─ recreate / final public proxy release / explicit release
 │   ├─ REF pre-retire handoff
 │   ├─ REF binding detach
 │   ├─ Opti FG teardown mutex
 │   ├─ public proxy release
 │   ├─ XeFG Destroy
 │   ├─ fakenvapi expected-old unpublish
 │   └─ XeLL Destroy
 │
 ├─ failed-init and failed-destroy rollback paths
 │
 ├─ REF hook-monitor recovery / rebind
 │
 └─ process shutdown
```

## 4. Executive result

At the pinned revisions, the normal paired lifecycle for **Monster Hunter Wilds / Dragon's Dogma 2 / newer RE Engine Capcom paths** is substantially coherent after the P5-P7 work. The audit did **not** find a new normal-path source-proven crash mismatch in the following previously dangerous areas:

- public-proxy retirement versus REF borrowed presentation lifetime,
- REF hook-monitor versus Opti FG teardown lock ordering,
- XeFG/XeLL uncertain teardown versus recreation,
- D3D12 queue lifetime and generation replacement,
- same-thread versus cross-thread FG reentrancy,
- MHW resize renderer-reset boundary,
- hook-monitor recovery during an active/uncertain XeFG transition.

However, the full audit found **one important Capcom-specific remaining compatibility problem** that is outside the P5-P7 implementation itself:

1. **CRASH-RELEVANT — Monster Hunter Rise REF self-unlink can defeat OptiScaler's Toolhelp-based pre-retire handoff discovery.**

It also found one secondary conditional weakness:

2. **COMPAT-RISK — REF currently treats failure to install the XeFG Destroy hook as optional, although pre-public Opti initialization rollback relies on that hook to clear an already-published REF XeFG binding.**

No broad XeFG/XeLL rewrite is justified by this audit.

---

# 5. Finding F1 — MHRise self-unlink can remove the P5-B/P7-A handoff path

**Classification: CRASH-RELEVANT**

**Affected game path: Monster Hunter Rise (`GameIdentity::is_mhrise()`)**

**Priority: highest remaining compatibility issue found by this audit**

## 5.1 REF deliberately unlinks its own module in MHRise

In fork REFramework `src/Main.cpp`, after construction of the live `REFramework` object, the MHRise path executes:

```cpp
if (gi.is_mhrise()) {
    if (our_dll) {
        if (!g_success_made_ldr_notification) {
            utility::spoof_module_paths_in_exe_dir();
        }
        utility::unlink(*our_dll);
    }
}
```

This is not a generic path for DD2 or every `tdb_ver >= 74` title; it is specifically used by the MHRise branch.

The intent of `unlink` in this context is module hiding from the normal loader/module-list view while leaving the code mapped and executing. This is compatible with the REFramework anti-tamper role, so the audit does **not** recommend disabling the unlink operation as a first solution.

## 5.2 Opti's P5-B/P7-A interop discovery depends on normal module enumeration

Current `XeFG_Dx12.cpp` resolves the frozen REF handoff export only by enumerating process modules:

```cpp
snapshot = CreateToolhelp32Snapshot(
    TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
    GetCurrentProcessId());

...

for (;;) {
    if (auto* proc = GetProcAddress(
            entry.hModule,
            "REFramework_XeFG_PreRetireSwapchainV1");
        proc != nullptr) {
        ...
    }

    if (Module32NextW(snapshot, &entry))
        continue;

    ...
}
```

If enumeration succeeds but no module exposes the export, Opti intentionally interprets the state as:

```text
NotAvailable
```

and preserves old/no-REF compatibility by continuing without the handoff.

That behavior is correct when REF is genuinely absent or is an older build. It is not sufficient when the current REF is present and active but intentionally hidden from the enumeration mechanism.

Windows Toolhelp module enumeration is tied to the process loader/module state. Microsoft documents that module snapshots can fail or return incorrect information when the loader data table is changed/corrupted while snapshotting. PEB/LDR unlinking is specifically used to remove a loaded module from normal user-mode module enumeration. Therefore an MHRise REF build that has completed `utility::unlink(*our_dll)` cannot be safely assumed discoverable by the current Toolhelp-only lookup.

## 5.3 Why this is not merely a missing optional integration

The P5-B/P7-A pre-retire handoff is the mechanism that establishes the safe order:

```text
Opti _swapchainLifecycleMutex
    ↓
REF V1 pre-retire callback
    ↓
REF hook_monitor_mutex
    ↓
REF renderer reset + instance-hook removal + binding detach
    ↓
return from REF
    ↓
Opti FG Mutex
    ↓
final public proxy release / XeFG Destroy
```

If hidden REF is misclassified as `NotAvailable`, Opti falls back to the legacy path even though REF is still actively tracking the XeFG presentation lifecycle.

That re-opens two hazards that the current fork specifically hardened.

### A. Lock-order hazard re-opens

Without the pre-retire detach, Opti can enter `ReleaseSwapchainLocked()` and acquire FG owner `1`, then call XeFG Destroy.

REF hooks `xefgSwapChainDestroy`, and its dispatch path takes the REF lifecycle monitor to snapshot/detach/reconcile the runtime.

The resulting order can again be:

```text
Opti teardown thread:
    FG Mutex
      → XeFG Destroy
        → REF hook_monitor_mutex

REF Present/Resize thread:
    REF hook_monitor_mutex
      → original Present/Resize
        → Opti FG Mutex
```

That is the exact opposing order P7-A was designed to remove.

### B. Final-proxy lifetime hazard re-opens

`ReleaseSwapchainLocked()` releases the final public XeFG proxy before the later vendor Destroy call.

P5-B established the REF pre-retire handoff because REF's internal presentation binding is borrowed and must be detached before that public lifecycle can disappear.

If the handoff is skipped because hidden REF appears `NotAvailable`, REF can still hold semantic/hook state referencing the old presentation lifecycle when Opti consumes the final public proxy reference.

The subsequent REF Destroy hook may eventually detach it, but that is later than the safety boundary that P5-B intentionally established.

## 5.4 Why normal REF Destroy hooking does not fully replace the handoff

It may appear that REF's `xefgSwapChainDestroy` hook will detach the binding anyway.

That is insufficient for two reasons:

1. Destroy dispatch is reached **after** Opti has entered the FG teardown critical section, so it does not solve the P7-A reverse-order lock problem.
2. The final public proxy is released before vendor Destroy in the established Opti teardown order, so Destroy-time detach is also too late to reproduce the P5-B pre-retire lifetime guarantee.

Therefore the V1 callback discovery is not merely diagnostic or optional when current fork REF is actively tracking the presentation path.

## 5.5 Recommended direction

Do **not** solve this by disabling MHRise's REF module hiding without first proving that doing so is safe for the game's anti-tamper requirements.

Do **not** replace Toolhelp with fragile memory scanning for an unlinked PE image.

The preferred compatibility direction is to stop making live REF interop depend exclusively on late module enumeration.

A narrow future design should provide a versioned **registration/cache path** for the existing pre-retire callback:

```text
REF becomes available
    ↓
REF registers V1 callback with Opti while a safe discovery path exists
    ↓
Opti stores the callback independently of future PEB/module visibility
    ↓
MHRise may unlink REF
    ↓
Opti retirement uses cached registered callback
```

The existing Toolhelp lookup can remain as an old/no-REF fallback so compatibility is preserved.

A robust design also needs to cover both load orders:

- Opti already loaded before REF unlinks itself.
- Opti loads after REF startup; REF's existing loader-notification infrastructure can be used to notice the later module and perform registration.

The exact ABI/design should be a separate narrow work order. No master-branch architecture should be imported merely to solve this discovery issue.

## 5.6 Required runtime confirmation

Before implementation, MHRise should log/verify one fact on the current build:

```text
after utility::unlink(REFramework):
CreateToolhelp32Snapshot + Module32First/Next
    → is REFramework_XeFG_PreRetireSwapchainV1 still discoverable?
```

The expected result for normal PEB/LDR unlink semantics is **no**, but this should be captured once on the exact fork build so the fix has a runtime artifact as well as the source-level interaction proof.

---

# 6. Finding F2 — REF Destroy hook is optional although pre-public rollback can depend on it

**Classification: COMPAT-RISK**

**Normal successful gameplay path: not affected**

**Requires an additional hook-install or post-Init failure**

## 6.1 Runtime registry behavior

`XeFGRuntimeRegistry::install_for_module()` requires the `InitFromSwapChainDesc` hook to install successfully, but `GetSwapChainPtr` and `Destroy` hooks are optional.

Conceptually:

```text
Init hook failure      → reject runtime
Destroy hook failure   → warn, keep runtime Active
```

The normal successful Opti lifecycle is still protected by the explicit P5-B/P7-A V1 handoff, so a missing Destroy hook does not automatically make every teardown unsafe.

## 6.2 The uncovered conditional path is initialization abort before public lifecycle commit

REF can commit its internal XeFG candidate immediately after the vendor `InitFromSwapChainDesc` returns a non-error result.

Opti still has more work after that point:

```text
Intel InitFromSwapChainDesc
    ↓
REF candidate/binding may now be active
    ↓
Opti GetSwapChainPtr
    ↓
only after exact success does Opti publish/commit the public FG proxy
```

If Opti fails after REF has already published the internal binding but before a public proxy is committed, Opti uses `AbortSwapchainInitialization()` and `DestroySwapchainContext()`.

There is no usable public proxy at that point, so the public-proxy V1 pre-retire handoff is not the rollback mechanism. Normal reconciliation instead depends on REF's Destroy hook:

```text
Opti AbortSwapchainInitialization
    ↓
XeFG Destroy
    ↓
REF dispatch_destroy
    ↓
pending/binding detach
    ↓
vendor Destroy
    ↓
exact-success reconciliation
```

If the REF Destroy hook failed to install but the runtime was left Active, the vendor Destroy can run without REF seeing it. REF may then retain a semantic binding/hook to an internal swapchain whose owning XeFG context was already destroyed by Opti.

Potential later consequences include:

- stale REF XeFG binding,
- REF hook-monitor quarantining or recovering around a dead presentation target,
- a future hook reset/replacement touching stale presentation state,
- failure to bind the fallback/native game swapchain cleanly.

A crash is not part of the normal path and requires the additional Destroy-hook failure plus a post-Init Opti abort, so this is classified as COMPAT-RISK rather than a current normal-path blocker.

## 6.3 Related positive-warning asymmetry does not independently create a finding

REF's general XeFG result helper treats nonnegative status as non-error for candidate discovery, while Opti requires exact success for lifecycle commit.

That can make a positive Init warning produce this transient order:

```text
REF accepts/publishes candidate
Opti refuses exact-success commit
Opti aborts initialization
REF Destroy hook detaches candidate
```

With the Destroy hook present, the two components reconcile immediately. Therefore the result-policy difference alone is **not** elevated into a compatibility bug by this audit.

It only makes the optional-Destroy-hook failure path more important.

## 6.4 Recommended direction

For the fork pairing, the safest narrow rule is:

> REF must not establish a semantic XeFG presentation binding that it cannot reliably detach when the owning context is destroyed before public-proxy commit.

Possible future solutions include either:

- treating the Destroy hook as a required capability before REF enables semantic XeFG binding for that runtime, or
- adding a context-only initialization-abort handoff that does not require a public proxy.

The former is smaller and fail-closed. The final design should be a separate work order if this risk is selected for implementation.

---

# 7. Verified paired lifecycle areas

The following sections were re-audited from current code rather than being accepted merely because a previous phase modified them.

## 7.1 REF runtime discovery versus Opti load order — normal path

**Result: VERIFIED, except MHRise post-unlink callback discovery described in F1.**

REF uses three complementary mechanisms around `libxess_fg.dll`:

1. `LdrRegisterDllNotification` marks XeFG work pending without installing hooks directly under the loader callback.
2. an `LdrLoadDll` hook calls the original loader first and then installs XeFG public API hooks before the loader call returns to the caller,
3. `install_already_loaded_runtimes()` hooks a runtime that was loaded before REF's normal constructor path reached the loader integration.

Opti may load/resolve XeFG relatively early, but actual `InitFromSwapChainDesc` occurs later when the game creates the FG presentation lifecycle. The already-loaded scan therefore covers the ordinary order in which Opti has loaded `libxess_fg.dll` before REF completes initialization.

No normal startup gap was found where the current pair is guaranteed to execute Opti XeFG Init before all three REF discovery mechanisms can run.

## 7.2 DXGI factory hook chain and internal XeFG swapchain observation

**Result: VERIFIED / no remaining paired crash mismatch found.**

Opti's game-facing factory hook enters FG creation under `ScopedSkipFGSCCreation` before Intel's internal presentation initialization is called.

During Intel `InitFromSwapChainDesc`, REF temporarily observes `IDXGIFactory2::CreateSwapChainForHwnd` and captures the presentation swapchain/queue selected by the XeFG runtime.

When Intel internally creates its swapchain, the nested Opti factory path sees the skip state and does not recursively create another XeFG lifecycle.

This prevents the dangerous recursion:

```text
game CreateSwapChain
 → Opti XeFG Init
   → Intel internal CreateSwapChain
     → Opti XeFG Init again
```

No hook-chain break was found in the current paired path.

## 7.3 REF internal binding and Opti public proxy are intentionally not the same identity

**Result: VERIFIED.**

REF tracks the internal presentation swapchain observed during Intel Init. Opti later obtains the public XeFG proxy from `D3D12GetSwapChainPtr`.

The fork correctly does **not** require those COM pointers to be equal.

Cross-component retirement identity is based on the XeFG runtime context, runtime slot and compatible HWND, not public-proxy/internal-swapchain pointer equality.

This is important because forcing equality would reject a valid Intel/Opti wrapper/proxy topology.

## 7.4 REF binding ownership during active lifecycle

**Result: VERIFIED.**

REF deliberately treats the presentation swapchain in its semantic binding as borrowed, while queue/device ownership is retained with `ComPtr`.

The borrowed swapchain is protected at transition boundaries by a local keepalive while REF performs renderer reset and hook removal.

The corresponding Opti-side guarantee is that retirement invokes REF detach before consuming the public lifecycle that can indirectly retire the internal presentation object.

At the current revisions, those two rules match for discoverable REF.

## 7.5 Queue identity — game/init queue versus presentation queue

**Result: VERIFIED.**

The two components may intentionally use different D3D12 command queues for the same XeFG generation:

- Opti owns the XeFG game/init queue for XeFG execution and lifecycle drain.
- REF may select the internal presentation queue observed during Intel swapchain creation.

REF validates that a distinct presentation queue belongs to the same device and is a usable direct queue before selecting it.

P7-B gives the Opti XeFG lifecycle its own `ComPtr` ownership and prevents a new queue candidate from replacing the old lifecycle queue until the new lifecycle has committed.

This is an intentional two-queue model, not a mismatch.

## 7.6 Create/recreate generation ordering

**Result: VERIFIED.**

For replacement of a current Opti XeFG lifecycle, Opti now drains the old generation using the old lifecycle queue and performs the REF pre-retire handoff before entering teardown.

A new candidate queue is not published as the new lifecycle owner until Intel initialization and public-proxy acquisition succeed.

Resize fence/queue state is generation-scoped and is retired with the matching generation.

No remaining source path was found where a successful generation N+1 commit causes generation N's REF binding to remain live on the normal path.

## 7.7 Present and Present1 lock order

**Result: VERIFIED for current paired normal path.**

REF holds its recursive `hook_monitor_mutex` across the instance presentation callback and original presentation chain.

Opti may acquire its FG mutex in the nested chain.

The unsafe opposite order used to exist during teardown because Opti held the FG mutex and then entered REF's XeFG Destroy hook.

Current retirement performs REF's V1 pre-retire detach before the FG teardown mutex is taken. Consequently normal current-lifecycle teardown does not leave the reverse dependency active.

P7-C additionally separates same-thread reentrancy from actual cross-thread mutex ownership, removing process-global false recursion on Present/Resize.

F1 is important precisely because MHRise can make this pre-retire mechanism undiscoverable and re-open the old ordering.

## 7.8 Resize / renderer reset / Monster Hunter Wilds transition hold

**Result: VERIFIED / specifically relevant to MHW.**

REF tracks ResizeTarget / ResizeBuffers / ResizeBuffers1 with thread-local nesting depth under the lifecycle monitor.

For Monster Hunter Wilds, REF can arm a resize-transition hold after the renderer is reset at ResizeTarget. While the hold is active, XeFG Presents continue through the real presentation path but REF renderer/mod callbacks are suppressed until a successful resize completion or lifecycle replacement clears the hold.

This is compatible with Opti's current resize hooks and P7-C recursion handling.

Failure to complete a resize keeps the REF renderer fail-closed rather than immediately re-entering stale renderer resources.

No remaining normal-path MHW resize crash mismatch was found in source at the pinned revisions.

## 7.9 Public final Release and stale wrapper/proxy generations

**Result: VERIFIED for discoverable REF.**

Opti probes final public-proxy release, validates current/stale identity, drains the correct XeFG queue, and serializes current lifecycle retirement.

Stale final proxies do not tear down a newer lifecycle.

REF's pre-retire callback retains the public proxy only for the duration of the REF detach operation and does not require it to equal the internal binding object.

The REF internal wrapper/binding keepalive may release while Opti's outer lifecycle mutex is already held. The nested Opti release path uses nonblocking lifecycle acquisition and therefore does not recursively tear down the same lifecycle; the outer retire transaction remains authoritative.

No new deadlock or double-destroy was found in that current path.

## 7.10 XeFG Destroy uncertainty and REF reconciliation

**Result: VERIFIED.**

REF only clears detached lifecycle state after exact successful XeFG Destroy reconciliation.

Opti similarly does not authorize clean recreation after uncertain Destroy results.

This avoids the dangerous disagreement:

```text
Opti: old context uncertain/dead
REF: old binding treated fully clean
        or
Opti: old context retained
REF: already assumes new generation is safe
```

The current pairing remains fail-closed on uncertain retirement.

## 7.11 XeLL / fakenvapi state

**Result for this audit: no additional REF crash finding.**

XeLL is intentionally a separate Opti subsystem used by XeFG for latency reduction. REF does not own the XeLL context.

P6 ensures that known-owned fakenvapi publication is removed before XeLL destruction and that uncertain XeLL teardown blocks recreation.

There are still implementation choices inside Opti such as the treatment of `fakenvapi::setModeAndContext()` failure after latency binding. Those may deserve independent functional review, but the audit did not find a path where REF observes a conflicting XeFG presentation generation because of that result alone.

Therefore this is not expanded into a compatibility finding here.

## 7.12 REF hook-monitor recovery

**Result: VERIFIED.**

REF's generic D3D hook recovery is XeFG-aware.

During runtime transition it suppresses generic recovery. A detached-but-uncertain lifecycle is also suppressed, and inconsistent/sustained-timeout XeFG state is quarantined rather than immediately replacing hooks behind Opti's lifecycle.

This prevents the hook monitor from becoming an independent third actor that silently replaces the presentation hook while Opti is retiring or rebuilding the XeFG generation.

## 7.13 Shutdown

**Result: no live-game paired crash finding.**

Opti treats process shutdown specially and does not perform the same full vendor teardown used for live recreation. Hook cleanup is also intentionally limited during process detach.

That is not promoted to a compatibility blocker because the audited deployment is process termination, not supported mid-game hot-unload/reload of Opti or REF.

If dynamic module unload becomes a supported requirement later, shutdown must be audited separately. It is outside this compatibility-crash scope today.

---

# 8. Items explicitly reviewed and not promoted into work

The following were noticed during the whole-tree review but are intentionally excluded from the compatibility backlog unless future evidence connects them to a paired Capcom crash.

## 8.1 General loader-lock / early initialization concerns in Opti

Opti performs substantial startup work during DLL initialization and some paths may load additional modules.

This can be discussed as general Windows DLL engineering, but no source chain was found showing that REF's presence specifically converts it into the current Capcom XeFG lifecycle crash mechanism.

**Status: NOT IN SCOPE for this audit.**

## 8.2 Opti-only raw aliases outside the hardened XeFG lifecycle

Some state aliases elsewhere in Opti are raw COM pointers or are based on short-lived QI discovery.

P7-B hardened the queue that is actually authoritative for the XeFG lifecycle and its resize synchronization generation.

No additional REF-owned lifetime was found that turns the remaining general aliases into a paired crash path.

**Status: NOT IN SCOPE unless a concrete REF interaction is later demonstrated.**

## 8.3 Intel specification differences without paired-state consequence

Differences in exact warning handling, optional API behavior, or preferred SDK usage are not automatically compatibility findings.

The audit only promotes them when they cause REF and Opti to commit incompatible lifecycle states that remain unreconciled.

**Status: explicitly outside the audit's default scope.**

## 8.4 Fork master implementation differences

Opti fork `master` was used only as a reference point.

No recommendation in this document requires moving the 0.9 branch to master architecture, adopting master XeLL ownership, or cherry-picking unmerged PR #17/#18/#19.

**Status: reference only.**

---

# 9. Recommended next actions

## 9.1 Required compatibility follow-up: make REF pre-retire discovery independent of module visibility

This is the only finding from this audit that should be treated as a direct next-step compatibility item.

The follow-up work should be narrowly scoped to preserving the existing P5-B/P7-A lifecycle contract even when REF hides/unlinks its image in MHRise.

Requirements for that future work:

- preserve the current V1 pre-retire semantics,
- do not remove MHRise anti-tamper/module-hiding behavior without separate proof,
- do not memory-scan for hidden modules,
- support either module load order,
- prefer an explicit versioned callback registration/cache mechanism,
- keep current Toolhelp enumeration only as fallback compatibility for old/no REF,
- fail closed when current fork REF is known to be present but the registered callback becomes invalid,
- do not import Opti master architecture.

## 9.2 Secondary hardening candidate: require reliable Destroy observation before REF semantic bind

This is conditional rather than a current normal-path blocker.

A future small work order can decide whether:

- the REF Destroy hook should become required for a runtime to participate in semantic XeFG binding, or
- a context-only abort handoff should cover the pre-public-proxy rollback interval.

Do not combine this with F1 unless implementation proves that the same small ABI can safely solve both.

## 9.3 Runtime validation matrix after F1 is fixed

Static audit cannot prove absence of all timing-dependent failures. Runtime validation should target paired lifecycle boundaries rather than generic gameplay duration.

### Monster Hunter Wilds

- cold launch with REF + Opti + XeFG,
- repeated scene/load transitions,
- borderless/fullscreen changes,
- ResizeTarget followed by ResizeBuffers/ResizeBuffers1,
- Alt+Tab and display resize,
- FG enable/disable/recreate where supported,
- verify no sustained REF resize hold,
- verify no reverse-order mutex stall,
- game exit.

### Dragon's Dogma 2 / RE9-class path

- cold launch,
- repeated loading/save transitions,
- swapchain recreation,
- Alt+Tab and display changes,
- XeFG failure/retry where reproducible,
- game exit.

### Monster Hunter Rise

First validate the audit finding itself:

- confirm REF performs `utility::unlink`,
- confirm whether Opti's Toolhelp scan can still discover `REFramework_XeFG_PreRetireSwapchainV1`,
- after the future interop-discovery fix, verify every current lifecycle retirement reaches the registered REF handoff despite module hiding,
- stress Present/Resize while forcing or naturally triggering swapchain retirement/recreation.

---

# 10. Final conclusion

The current fork pair should **not** be treated as needing a broad XeFG/XeLL refactor.

For the main Capcom + REF compatibility target, the P5-P7 work now forms a coherent normal lifecycle:

```text
REF observes/binds Intel presentation lifecycle
    ↓
Opti commits public lifecycle and queue generation
    ↓
REF serializes renderer callbacks around Present/Resize
    ↓
Opti owns XeFG queue/context lifetime
    ↓
retirement first drains/detaches REF
    ↓
Opti enters FG teardown
    ↓
public proxy + vendor context retire
    ↓
REF and Opti reconcile exact successful destroy
```

For MHW/DD2/newer RE Engine paths where the current REF module remains normally discoverable, this full source audit found **no additional normal-path source-proven Opti↔REF lifecycle crash blocker** beyond the already merged hardening.

The main remaining exception is **Monster Hunter Rise**, where REF's own post-startup module unlinking conflicts with Opti's current assumption that the V1 interop export can always be rediscovered through Toolhelp module enumeration. That can bypass the very handoff that protects final-proxy lifetime and lock ordering, so it is a genuine compatibility finding and should be addressed before declaring the fork pair lifecycle-complete for all supported Capcom titles.

The optional REF Destroy-hook behavior is a secondary fail-path risk, not evidence that the normal XeFG/XeLL architecture needs rewriting.

Future work should remain narrow: fix proven cross-component lifecycle boundaries, preserve the working 0.9 standalone behavior, and do not convert general code-quality or specification discussions into compatibility work without a concrete paired crash path.
