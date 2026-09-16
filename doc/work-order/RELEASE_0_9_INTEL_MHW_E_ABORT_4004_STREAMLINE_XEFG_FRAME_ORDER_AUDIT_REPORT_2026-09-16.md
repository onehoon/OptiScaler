# Release 0.9 — Intel MHW E_ABORT 4004 Streamline/XeFG Contract/Lifecycle Audit Report

- Audit date: 2026-09-16 KST
- Target repository: onehoon/OptiScaler
- Target branch: reframework-0.9
- Audit baseline (pinned source base): c8f1fd0d831514c55fedb57bec24ec791640a674
- Baseline subject: docs: add Intel MHW E_ABORT 4004 XeFG frame-order work order
- Audited branch tip: 1199b5f8433c0693b01a91d94dd4fb13dfccfaf5
- Post-pinned instruction commit: 41b7d30807d1f16e994b75f9f5bc45050afbc6fe
- Post-pinned implementation commit: 1199b5f8433c0693b01a91d94dd4fb13dfccfaf5 (`fix: serialize Streamline XeFG frame input with present (#36)`)
- Companion source: onehoon/REFramework fork
- Audit disposition: the pinned work order, the later audit instruction, the current branch tip, the companion source, and available runtime evidence were reviewed; no source implementation, build, or new runtime A/B was performed by this audit

## Executive verdict

The available evidence does not prove that a particular Intel XeFG or Intel driver call initiated the MHW E_ABORT 4004 failure. The E-abort capture contains a Capcom Fatal D3D path with an access violation at a MonsterHunterWilds.exe address, while the paired OptiScaler log is absent. The REFramework log shows the last Present entry count at 26655, then a hook-monitor timeout and quarantine after Present activity had already stopped. This makes the timeout a downstream observation, not evidence that the hook monitor initiated the failure.

The source audit identifies a credible architectural race candidate, a current-tip residual in the newly added transaction, and independent crash-capable defects:

1. At the pinned c8 base, Streamline input submission and the OptiScaler XeFG Present/Dispatch path did not share one transaction boundary. Commit 1199 adds `ScopedStreamlineFGTransaction` around `setConstants`, `evaluateState`, `reportResource`, and `markPresent`, using an any-owner current-thread query and the intended `fg->Mutex -> frameBoundary/resource` order. Static review finds that this is the prescribed Phase 1 implementation shape, but no logging-OFF A/B has validated it. The current implementation also reuses logical owner 2, while `XeFG_Dx12::EvaluateState` can unlock owner 2 internally when `FGchanged` is set; this residual is SDLX-015.
2. IFGFeature_Dx12::GetResource returns a raw pointer into a frame-resource map after releasing the shared lock. XeFG Present later dereferences that pointer, while NewFrame can clear the same map under its exclusive lock. This is a real source-level lifetime weakness, but the supplied logs do not prove that this interleaving occurred in the E-abort run.
3. The wrapped DXGI swapchain has definite COM lifetime violations: several QueryInterface results are released immediately and retained as raw member pointers, and LocalPresent and WaitForGPUIdle use interfaces after Release. These are independent high-impact crash paths and must not be confused with proof of the specific E_ABORT root.
4. The E-abort log explicitly records distinct initialization and presentation queues on the same D3D12 device. The inspected adapter forwards DLSSG fence values but does not consume them, and resource readiness is marked before ExecuteCommandLists returns. An explicit cross-queue dependency is therefore not established in the inspected code; the runtime evidence is insufficient to prove that this caused the captured failure.

The first diagnostic experiment prescribed by the work order remains the correct acceptance gate: validate the commit-1199 transaction with logging disabled, then separately address the SDLX-015 owner-2 unlock and the independent COM/resource/queue defects. This audit does not modify those defects or claim that commit 1199 has passed the required runtime matrix.

## 1. Scope, input identity, and evidence rules

### 1.1 Work-order identity

The supplied GitHub URL names:

    RELEASE_0_9_STREAMLINE_DLSSG_TO_XEFG_CONTRACT_LIFECYCLE_AUDIT_INSTRUCTION_2026-09-16.md

Supplied URL: https://github.com/onehoon/OptiScaler/blob/reframework-0.9/doc/work-order/RELEASE_0_9_STREAMLINE_DLSSG_TO_XEFG_CONTRACT_LIFECYCLE_AUDIT_INSTRUCTION_2026-09-16.md

The exact path was added after the pinned c8 commit by 41b7d308 and is present at the audited branch tip. The pinned target commit contains the earlier work order at:

    doc/work-order/RELEASE_0_9_INTEL_MHW_E_ABORT_4004_STREAMLINE_XEFG_FRAME_ORDER_WORK_ORDER_2026-09-16.md

This report is intentionally placed beside both instruction/work-order documents in doc/work-order. The user-requested repository placement takes precedence over the instruction's suggested `doc/audit` placement.

The instruction's central constraints were applied:

- audit the complete Streamline/DLSSG input to OptiScaler XeFG to Present path;
- include frame identity, resource lifecycle, CPU/GPU queue ownership, resize, release, shutdown, and REFramework handoff;
- do not claim that SetResource has no mutex;
- keep source findings separate from runtime proof;
- do not begin with sleeps, logger delays, vendor-name special cases, or a broad lifecycle rewrite;
- preserve the PR34 wrapper final-release guard and PR35 pre-retire behavior while isolating the frame-order experiment;
- do not modify REFramework for this audit.

### 1.2 Revision matrix

| Component | Revision | Role in this audit |
|---|---|---|
| OptiScaler | c8f1fd0d831514c55fedb57bec24ec791640a674 | Pinned target revision containing the original Intel MHW work order |
| OptiScaler | 41b7d30807d1f16e994b75f9f5bc45050afbc6fe | Later commit adding the exact supplied Streamline/DLSSG contract audit instruction |
| OptiScaler | 1199b5f8433c0693b01a91d94dd4fb13dfccfaf5 | Audited current branch tip; implements the Streamline XeFG transaction in #36 |
| OptiScaler parent | d6b0132f4f6faabf7235dba5b8d684edfd8e2831 | PR35 shutdown pre-retire behavior referenced by the instruction |
| REFramework | 4bf45b370e602f7f6a3ca54f308daa4e353aab8a | ABI compatibility reference for the OptiScaler baseline |
| REFramework | 79b29c073eae08e818b5f220f01f25eb6382f086 | Late D3D12 callback fix in the compatibility history |
| REFramework | e4a1a1d1faf06f4e25923dacf765c00213c86ada | Current fork master sensitivity comparison; not silently substituted for the pinned ABI reference |

The remote `reframework-0.9` branch advanced from c8 during the audit. The report was therefore re-based on the current 1199b5f8 tip before publication. Runtime logs were produced by older OptiScaler binaries (e9ca4685, ac1e48fa, d6b0132f, and 865d188c); they are retained as historical comparison evidence and are not claimed as validation of commit 1199.

The current REFramework checkout was read-only during this audit. It already contained unrelated untracked analysis directories; they were not touched.

### 1.3 Post-pinned tip review: commit 1199 / PR #36

The current tip adds a narrow CPU-side transaction adapter in `OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp:7-43`. When `FGUseMutexForSwapchain` is enabled and `activeFgOutput` is `XeFG`, `ScopedStreamlineFGTransaction`:

- skips the transaction when no XeFG output is active;
- detects ownership by the current thread without requiring logical owner 2;
- otherwise acquires `fg->Mutex` with logical owner 2 and releases it only when this scope acquired it.

The scope is entered before `CheckForFrame` or the remaining frame-state work in `setConstants` (`:92-100`), `evaluateState` (`:291-299`), and `reportResource` (`:324-348`), and before `_frameBoundaryMutex` in `markPresent` (`:489-500`). This gives the covered adapter entries the intended CPU order:

~~~text
Streamline transaction: fg->Mutex(owner 2)
  -> _frameBoundaryMutex in CheckForFrame/markPresent
    -> _resourceMutex[index] in SetResource
  -> XeFG state/metadata work
  -> scope release
~~~

This is a meaningful correction to the pinned c8 split and is the correct shape for the first experiment. It is not a complete frame immutability proof: direct `Dispatch`, `StartNewFrame`, `GetDispatchIndex`, `SetFrameCount`, map readers, and lifecycle paths remain outside one universal transaction; the original Streamline API calls also occur outside the adapter scope. In addition, `XeFG_Dx12::EvaluateState` still contains an internal `Mutex.unlockThis(2)` at `OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1361-1362` when `FGchanged` is true. Because commit 1199 uses owner 2 for the new scope, that internal unlock can release the outer transaction before `setConstants` has finished and leave the RAII destructor with no matching ownership. This current-tip residual is SDLX-015.

The helper uses a raw `IFGFeature_Dx12*` obtained from `State::Instance().currentFG` and does not take `_swapchainLifecycleMutex`; this preserves the requested narrow scope but leaves lifecycle crossover for a later phase. No build or logging-OFF runtime A/B was performed against commit 1199.

### 1.4 Evidence labels

The report uses the following distinction:

- Source-confirmed: the current source text establishes the property or defect.
- Runtime-observed: the supplied log or dump directly records the event.
- Candidate: the source permits the chain, but the supplied runtime data does not prove its occurrence.
- NOT_EXERCISED: the supplied session did not reach the required coverage point.
- not evidenced: the claimed failure mechanism is not established by the available artifacts.

For log findings, severity uses the required labels:

- Blocking
- Non-blocking improvement
- Theoretical
- not evidenced

Impact is additionally described as Critical, High, Medium, or Low where useful. A Blocking source defect is not automatically the confirmed cause of the E-abort.

## 2. Runtime evidence inventory

### 2.1 Requested folder resolution

The originally named folder did not exist:

    C:\GoogleDrive\ref-xefg\0914\mhw

The parent C:\GoogleDrive\ref-xefg existed. The available Monster Hunter Wilds candidates were:

- C:\GoogleDrive\ref-xefg\Release-09\mhw
- C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004
- C:\GoogleDrive\ref-xefg\PR19\MHW

No directory was created at the missing requested path. The three existing candidates were analyzed separately and their evidence levels are not merged.

### 2.2 Session matrix

| Session | Pair | OptiScaler evidence | REFramework evidence | Coverage/result |
|---|---|---|---|---|
| Release-09/mhw root | Paired | 0.9.5-pre4, e9ca4685; XeFG; 9424 hkFGPresent1; 4708 XeFG Present; zero nonzero LocalPresent results | c6704372; 2m47.0s; ExternalBind 2; InitDesc 2; ResizeBuffers1 6/0; Last chance 1 at the end | Session health PASS; resize PASS; max Present1 gap 1.236082s; changed-binding and long-minimize NOT_EXERCISED |
| Release-09/mhw/Intel | Paired | 0.9.5-pre4, ac1e48fa; XeFG; 5584 hkFGPresent1; 2788 XeFG Present; zero nonzero LocalPresent results | c6704372; 1m58.8s; ExternalBind 2; InitDesc 2; ResizeBuffers1 6/0; Last chance 1 at the end | Session health PASS; resize PASS; max Present1 gap 1.350831s; changed-binding and long-minimize NOT_EXERCISED |
| Release-09/mhw/새 폴더 | OptiScaler only | 0.9.5-pre4, b0621c37; XeFG Present reached; no paired REF log | No paired REF evidence | Session BLOCKED; resize recovery and periodic rehook coverage cannot be accepted |
| Release-09/mhw/새 폴더 (2) | Paired | 0.9.5-pre4, ac1e48fa; XeFG; 5094 XeFG Present; zero nonzero LocalPresent results | c6704372; 1m22.3s; ExternalBind 2; ResizeBuffers1 6/0; REF exception at final proxy retirement | Runtime fields before exit PASS; separate shutdown/proxy-retire crash evidence |
| Release-09/mhw/새 폴더 (3) | Paired | 0.9.5-pre4, d6b0132f; XeFG; 4674 XeFG Present; zero nonzero LocalPresent results | c6704372; 45.9s; ExternalBind 2; ResizeBuffers1 6/0 | Session health PASS; resize PASS; max Present1 gap 0.948167s; changed-binding and long-minimize NOT_EXERCISED |
| Release-09/E-abort 4004 | REF only | No OptiScaler.log was present | c6704372; 6m51.3s; ExternalBind 2; InitDesc 2; ResizeBuffers1 6/0; Last chance 10; HookMonitor 12 | Pair health BLOCKED; target CrashReport and MiniDump exist; no exact Opti input/Present correlation possible |
| PR19/MHW | Paired but non-runtime | 10.0.0-dev, 865d188c; FGOutput XeFG but zero Present/XeFG/LocalPresent and zero wrapper QI | master, 6ac6da; 27.7s; no D3D12 hook, no ExternalBind, no InitDesc, no resize | Startup/incomplete session; it is not a XeFG runtime test. Periodic rehook coverage FAIL; no cause can be inferred |

The bundled ref-xefg-log-review analyzer was run against the three existing candidate roots. Its generated triage directories were temporary and are not part of the report commit. Raw logs remain the evidence of record.

### 2.3 Selected evidence hashes

These hashes identify the principal artifacts used for the E-abort and comparison claims:

| SHA-256 | Artifact |
|---|---|
| CE69C5ACBA05DBDF06DEE6374803B1F8078D501E7DC82C83782D651FDDA19469 | C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004\re2_framework_log.txt |
| 136EB43656B30A63627136CA047FF7E811480200B6D1E726E009BAC568923EDC | C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004\2026_0916_0005\CrashReport.txt |
| 2849E6C292434E580972B59261CDA2E55BE32B02342F8D1ACF489FE39DE22A73 | C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004\2026_0916_0005\MiniDump.dmp |
| 58AD2CCA24954C01289C2981FA4DEBCFCECC3681AE08B690AE7E95607939E74B | C:\GoogleDrive\ref-xefg\Release-09\mhw\OptiScaler.log |
| 146853D9E4E29548D5491F394E4CDC195A5B546B23FFFBB9CA5B40E2F729D8DD | C:\GoogleDrive\ref-xefg\Release-09\mhw\re2_framework_log.txt |

## 3. Runtime findings

### 3.1 E-abort session timeline

The REF-only E-abort log records the following sequence:

| Time | Evidence | Interpretation |
|---|---|---|
| 00:00:13.096 | XeFG InitDesc: context 0x91e85d60, initialization queue 0x8f5f2980, 1920x1080, 3 buffers | XeFG initialization reached success |
| 00:00:13.096 | QueueIdentity: init queue 0x8f5f2980 and presentation queue 0x7619afa0, both device identity 0x58681450, relation distinct_same_device | Two direct queues on one device are real runtime state |
| 00:00:13.097 | XeFG Bind accepted, queue relation distinct_same_device | Binding was accepted |
| 00:00:14.182 | D3D12 Hooking | Hooking completed |
| 00:00:14.188 | ExternalBind generation 1, queue 0x7619afa0, device 0x58681a80 | REF bound to the presentation path |
| 00:00:14.200 | Present1 call 1 | Presentation reached the normal path |
| 00:00:14.505–00:00:14.646 | ResizeBuffers1 enter/pre_reset/original_return 0 | Initial resize completed successfully |
| Before timeout | Present entry count reached 26655 | The session had substantial Present activity |
| 00:05:06.420 | Last chance, present age 5375 ms, inside_present false, binding generation 1 | Present activity had already stopped when the monitor reacted |
| 00:05:07.421 | HookMonitor preserve_grace, reason present_timeout, age 6375 ms | Monitor grace period, not an initiating GPU error |
| 00:05:29.687 | HookMonitor quarantine, reason sustained_present_timeout, age 28642 ms | Later quarantine after sustained absence of Present |
| 00:05:17.432–00:06:45.745 | Repeated Last chance messages; rehook request count remained 0 | No recurring rehook recovery was observed |

The captured CrashReport is separate from the REF timeline. Its relevant fields are:

- ExceptionCode C0000005, EXCEPTION_ACCESS_VIOLATION.
- ExceptionAddress 000000014CD94BF0, inside MonsterHunterWilds.exe.
- Call stack entries in the report are MonsterHunterWilds.exe followed by KERNEL32.DLL and ntdll.dll.
- Message: Fatal D3D error (7, E_ABORT, 0x80004004).
- DxDiag identifies Intel Arc B390 and driver 32.0.101.8993. DxDiag also says Previously: Crashed in Direct3D (stage 2), while the device status is No Problem.
- The captured config has Algorithm=DLSS, AlgorithmFG=DLSS, FrameGenerationMode=On, and GenerateFramesCount=1.

The artifacts therefore establish a game-side fatal-D3D/AV path and a preceding cessation of Present activity. They do not establish a direct access violation inside igxess_fg.dll, libxess_fg.dll, or an Intel driver routine.

### 3.2 Normal and comparison sessions

The root and Intel comparison sessions both reached active XeFG, Present, and repeated successful ResizeBuffers1 transitions. Their maximum Present1 gaps were 1.236082 seconds and 1.350831 seconds, below the analyzer's six-second threshold. Their OptiScaler LocalPresent result scans contained no nonzero result.

The paired 새 폴더 (2) session is materially different: it reaches normal XeFG operation and then fails during final proxy retirement. REF records LifecycleDetach with reason proxy_retire at 23:23:28.846–23:23:28.849, followed by an access violation at 23:23:28.906. The call stack contains REFramework_XeFG_PreRetireSwapchainV1, ImTextureData_GetTexID, libxess_fg.dll Ordinal21, dxgi.dll ffxGetDeviceDX11_Fsr31, and sl.interposer.dll. The OptiScaler tail records final release completion at 23:23:28.905901. This is a shutdown/proxy-retire crash evidence set, not the normal-gameplay E-abort capture; the stack alone is insufficient to assign ownership.

The PR19/MHW pair never reaches a D3D12 hook or XeFG Present. It is useful only as a startup/incomplete capture and cannot support an initial-run failure diagnosis.

### 3.3 Lua validation

No [LuaSmoke] marker was found in any of the MHW REFramework logs examined:

- Release-09/mhw root
- Release-09/mhw/Intel
- Release-09/mhw/새 폴더 (2)
- Release-09/mhw/새 폴더 (3)
- Release-09/E-abort 4004
- PR19/MHW

The logs do contain normal ScriptRunner Lua-state initialization and autorun-directory creation. No targeted script-failure marker was found, but the required LuaSmoke callback was not exercised. Lua validation is therefore NOT_EXERCISED, not a PASS and not evidence of a Lua-caused failure.

### 3.4 Logging sensitivity

The work order records that heavy synchronous OptiScaler logging reduces or hides the failure. The current artifact set does not contain an OptiScaler log for the E-abort session, so it cannot independently reproduce or quantify that logging-on/logging-off effect. The source still contains synchronous trace/file logging, but no A/B was run in this audit. Logging remains a timing perturbation and must not be treated as the fix.

## 4. End-to-end call graph

### 4.1 Streamline/DLSSG input path

~~~text
Game or Streamline
  -> slSetTag / slSetTagForFrame
     -> StreamlineHooks::hkslSetTag / hkslSetTagForFrame
        -> Sl_Inputs_Dx12::reportResource
           -> ScopedStreamlineFGTransaction
              -> fg->Mutex owner 2 unless already owned by this thread
              -> CheckForFrame
                 -> _frameBoundaryMutex
           -> IndexForFrameId / GetIndexWillBeDispatched / HasResource
           -> IFGFeature_Dx12::SetResource
              -> _resourceMutex[fIndex] exclusive lock
              -> resource map update / copies / barriers
              -> XeFG D3D12TagFrameResource
              -> SetResourceReady
        -> original Streamline tag API

Game or Streamline
  -> slSetConstants
     -> StreamlineHooks::hkslSetConstants
        -> setConstantsMutex
        -> Sl_Inputs_Dx12::setConstants
           -> ScopedStreamlineFGTransaction
           -> CheckForFrame / _frameBoundaryMutex
           -> XeFG_Dx12::EvaluateState
           -> camera/jitter/MV/reset/frame-time writes
        -> original Streamline constants API

Game or Streamline
  -> slEvaluateFeature
     -> StreamlineHooks::hkslEvaluateFeature
        -> reportResource for each supplied ResourceTag
           -> ScopedStreamlineFGTransaction / resource path above
        -> original slEvaluateFeature (not thread-safe per bundled API contract)

Reflex marker path
  -> RENDERSUBMIT_START
     -> ReflexHooks::hkNvAPI_D3D_SetLatencyMarker
        -> Sl_Inputs_Dx12::evaluateState
  -> PRESENT_START
     -> Sl_Inputs_Dx12::markPresent
        -> ScopedStreamlineFGTransaction
        -> _frameBoundaryMutex
        -> IFGFeature::SetFrameCount
~~~

Source anchors:

- StreamlineHooks::hkslSetTag: Streamline_Hooks.cpp:213-228.
- StreamlineHooks::hkslSetTagForFrame: Streamline_Hooks.cpp:231-307.
- StreamlineHooks::hkslEvaluateFeature: Streamline_Hooks.cpp:309-340.
- StreamlineHooks::hkslSetConstants: Streamline_Hooks.cpp:691-700.
- Streamline hook installation: Streamline_Hooks.cpp:1276-1291.
- Input frame and resource handling: Streamline_Inputs_Dx12.cpp:7-43, 45-90, 92-180, 291-475, and 489-500 at commit 1199.
- Reflex caller mapping: Reflex_Hooks.cpp:104-129.

The adapter transaction is deliberately inside the OptiScaler input methods. The hook wrappers still call the original Streamline APIs outside that scope: `hkslSetTag`/`hkslSetTagForFrame` call the original tag API after `reportResource` returns; `hkslEvaluateFeature` calls the original evaluate API after its resource-report loop; and `hkslSetConstants` calls the original constants API after `setConstants` returns. Therefore the current tip serializes the adapter's CPU state, not the full vendor API call plus all external Streamline work.

### 4.2 OptiScaler XeFG Present, resize, and release paths

~~~text
FGHooks::FGPresent
  -> fg->Mutex owner 2
  -> XeFG_Dx12::Present
     -> GetIndexWillBeDispatched
     -> resource/ready/constants reads
     -> TagFrameConstants
     -> SetPresentId
     -> optional UI/hudless command-list work
     -> Dispatch
  -> original DXGI Present/Present1
  -> unlock owner 2

FGHooks::hkResizeBuffers1
  -> fg->Mutex owner 6678
  -> PauseFG
  -> WaitForGPUIdle
  -> original ResizeBuffers1
  -> unlock owner 6678

WrappedIDXGISwapChain4::Release
  -> final-release guard
  -> XeFG_Dx12::ReleaseSwapchain
     -> REF pre-retire handoff
     -> ReleaseSwapchainLocked
        -> fg->Mutex owner 1
        -> DestroyFGContext / DestroySwapchainContext
        -> release queue and command objects
  -> release real swapchain
~~~

Source anchors:

- FGPresent transaction: FG_Hooks.cpp:1184-1326.
- ResizeBuffers: FG_Hooks.cpp:620-649.
- ResizeBuffers1: FG_Hooks.cpp:872-904.
- XeFG Present: XeFG_Dx12.cpp:1485-1598.
- XeFG Dispatch and vendor calls: XeFG_Dx12.cpp:985-1283.
- XeFG release entry and final release: XeFG_Dx12.cpp:1866-2044.
- Wrapper final release: wrapped_swapchain.cpp:554-703.

## 5. Frame identity and ordering model

OptiScaler uses a four-slot ring, BUFFER_COUNT=4, defined in SysUtils.h:31. Streamline tracks a separate frame-ID-to-slot array. XeFG also derives a slot from _frameCount modulo BUFFER_COUNT. At the pinned c8 base these values were not held under one transaction. Commit 1199 adds a transaction around the four Streamline adapter entry points, but direct framegen callers and the GPU dependency remain outside one immutable-frame contract.

### 5.1 Current frame transitions

1. In the covered commit-1199 adapter entries, `ScopedStreamlineFGTransaction` acquires `fg->Mutex` first when needed.
2. CheckForFrame then takes _frameBoundaryMutex; it may call StartNewFrame, advance _frameCount, obtain the current index, and write _frameIdIndex.
3. reportResource selects a slot through IndexForFrameId or GetIndexWillBeDispatched and SetResource takes the selected _resourceMutex slot while the outer transaction is still held.
4. setConstants keeps the outer transaction through XeFG EvaluateState and camera/jitter/MV/reset/frame-time writes; the hook's `setConstantsMutex` is an additional wrapper-level lock.
5. markPresent now takes the same transaction before _frameBoundaryMutex and SetFrameCount.
6. Present takes fg->Mutex owner 2 (or observes same-thread ownership) and may update _lastDispatchedFrame, read readiness, tag constants, set the Present ID, and invoke Dispatch.
7. The transaction does not make raw map pointers, live frame counters, direct Dispatch callers, or GPU queue/fence dependencies immutable. EvaluateState's internal owner-2 unlock is an additional current-tip hazard described in SDLX-015.

### 5.2 Timing diagram A — pinned c8 / pre-1199 candidate input/Present overlap

~~~text
Thread A: Streamline input                         Thread B: Present
----------------------                            ----------------
CheckForFrame()
  lock frameBoundary
  StartNewFrame()
  currentIndex/frameId map
  unlock frameBoundary

Set constants / camera / jitter
reportResource()
  IndexForFrameId / HasResource
  SetResource()
    lock resourceMutex[index]
    D3D12TagFrameResource()
    SetResourceReady()
    unlock resourceMutex[index]
                                                   lock fg->Mutex owner 2
                                                   GetIndexWillBeDispatched()
                                                   read ready/resource/frame state
                                                   Dispatch()
                                                     TagFrameConstants()
                                                     SetPresentId()
                                                   original Present
                                                   unlock fg->Mutex

Potential result: frame N constants and resources are split across the
Present transaction, or Present observes a live ring slot while input
submission is still changing its associated state.
~~~

This is not a current-tip claim that the new wrapper is absent; it is the pinned-base/pre-1199 interleaving that the work order's Phase 1 A/B was designed to distinguish. The same lower-level resource and lifecycle risks remain possible through direct callers.

### 5.3 Timing diagram B — current-tip covered transaction

~~~text
Thread A: Streamline adapter (commit 1199)       Thread B: Present
------------------------------------------       ----------------
ScopedStreamlineFGTransaction()
  lock fg->Mutex owner 2                         waits for fg->Mutex
  CheckForFrame()
    lock frameBoundary
    StartNewFrame / frameId map
    unlock frameBoundary
  constants / EvaluateState / metadata
  reportResource()
    lock resourceMutex[index]
    SetResource / readiness / vendor tag
    unlock resourceMutex[index]
  scope release
                                                   lock fg->Mutex owner 2
                                                   Dispatch / SetPresentId
                                                   original Present
                                                   unlock owner 2
~~~

The order above is the intended CPU serialization for the four covered adapter methods. It is weakened if `EvaluateState` sees `FGchanged` and releases owner 2 internally at `XeFG_Dx12.cpp:1361-1362`; in that case the outer `setConstants` scope can continue after the mutex has been released. It also does not cover the original Streamline API calls after the adapter returns, nor establish a GPU fence between a producer queue and the presentation queue.

### 5.4 Timing diagram C — observed E-abort monitor sequence

~~~text
00:00:13.096  XeFG InitDesc succeeds
00:00:13.096  init queue != presentation queue, same device
00:00:13.097  bind accepted
00:00:14.188  REF ExternalBind generation 1
00:00:14.200  Present1 call 1
00:00:14.505  ResizeBuffers1 enter/pre-reset
00:00:14.646  ResizeBuffers1 original_return = 0
     ...
             Present activity reaches entry count 26655
     ...
00:05:06.420  Last chance, present_age_ms = 5375
00:05:07.421  preserve_grace, present_timeout
00:05:29.687  quarantine, sustained_present_timeout
     ...
00:06:45.745  repeated Last chance, no rehook request

The observed order is Present stop -> monitor timeout/grace/quarantine.
It does not show monitor timeout -> E_ABORT.
~~~

## 6. Shared state and synchronization coverage

| State | Producer/writer | Consumer/reader | Nominal owner | Lock/atomic | Can overlap? | Invariant / audit result |
|---|---|---|---|---|---|---|
| `State.currentFG` | swapchain/context setup and global lifecycle | Streamline, Present, resize, release | global FG/lifecycle owner | raw pointer/global state | Yes across callbacks and teardown | No generation-bound snapshot or universal lifecycle drain |
| `State.currentD3D12Device` | wrapper capture and setup | input, queue wait, vendor calls | wrapper/State owner | raw pointer | Yes with final release and reconfiguration | Lifetime depends on external ownership; related wrapper UAF is SDLX-008 |
| `State.currentCommandQueue` | setup, ResizeBuffers1 publication, queue tracking | Present timing, GPU wait, XeFG | queue-generation owner | raw global plus separate ComPtr aliases | Yes during resize/tracking | Queue identity is not one generation-bound contract |
| `_frameBoundaryMutex` | `CheckForFrame`, `markPresent` | frame-ID mapping and Present indirectly | `Sl_Inputs_Dx12` frame tracker | exclusive `std::scoped_lock`; nested under commit 1199 helper for covered entries | Yes with work after the short section and direct callers | Protects transition bookkeeping only, not all frame state |
| `_frameIdIndex[4]` | `CheckForFrame` | `IndexForFrameId` | frame tracker | writer under frameBoundary; read is unlocked | Yes | Read/write happens-before is absent; stale/ABA candidate |
| `_frameCount`, `_lastDispatchedFrame`, `_targetFrame` | `StartNewFrame`, dispatch selection, frame setters | `GetIndex*`, Dispatch, Present, Reflex markers | `IFGFeature` frame state | mostly plain `UINT64` fields/no common lock | Yes through direct paths | Live counters can disagree with the slot/frame snapshot |
| `_frameResources[4]` | `SetResource`, `NewFrame` clear | `HasResource`, Dispatch, `GetResource` | per-slot resource owner | exclusive lock in writers; shared lock only during `GetResource` lookup | Yes for uncovered readers/returned pointer | Map readers and escaped pointer are not all protected |
| `_resourceCopy[4]` / copy resources | resource copy/setup and frame reset | Dispatch/UI/hudless copy paths | per-slot resource owner | per-slot resource mechanisms, no universal Dispatch lock | Yes | Copy lifetime is not published with an immutable frame context |
| `_resourceReady[4]` | `StartNewFrame`, `SetResourceReady`, queue tracking | Dispatch | per-slot frame/resource owner | plain readiness state in Dispatch; tracking mutex only around producer observation | Yes | CPU-ready can precede GPU completion and can race state consumption |
| frame constants, camera/jitter/MV/reset/frame-time arrays | `setConstants` and state setters | `EvaluateState`, Dispatch, Present | `IFGFeature`/XeFG frame state | commit-1199 outer helper for covered input calls; no independent state lock | Yes via direct callers and SDLX-015 | No immutable publication point; logging must not supply the missing invariant |
| `fg->Mutex` | FGPresent, commit-1199 adapter entries, resize, release | Present/resize/release and covered input paths | multiple logical owners: 2/6677/6678/1 | `OwnedMutex`; helper uses owner 2 plus any-owner current-thread query | Yes on re-entry and external callbacks | Intended CPU domain exists, but internal owner-2 unlock and uncovered callers remain |
| `_swapchainLifecycleMutex` | create/recreate/release | lifecycle methods | XeFG lifecycle owner | lifecycle mutex/try-lock paths | Yes with Shutdown, Present, SetResource, callbacks | No complete callback drain or retirement snapshot |
| `State.isShuttingDown` | process teardown | ReleaseSwapchain and lifecycle branches | global lifecycle state | raw `bool`, not atomic | Yes | Shutdown decision can cross threads without an explicit state protocol |
| `XeFG::_ownedGameCommandQueue` | creation and `SetCommandQueue` tracking | XeFG/UI/SC and vendor paths | XeFG queue owner | `ComPtr` member, type argument ignored | Yes with wrapper queue publication/resize | Owned reference exists, but queue generation/dependency is not unified |
| wrapper `_real1.._real4` | constructor `QueryInterface` | ResizeBuffers, ResizeBuffers1, Present1 | wrapper | QI result immediately released; raw members retained | Yes | Definite dangling-interface defect; SDLX-008 |

At the current tip, the relevant nested lock order is:

~~~text
hkslSetConstants: setConstantsMutex
  -> Streamline transaction fg->Mutex(owner 2)
    -> _frameBoundaryMutex in CheckForFrame
    -> _resourceMutex[index] when reporting a resource

reportResource / setConstants / evaluateState / markPresent:
  Streamline transaction fg->Mutex(owner 2)
    -> frameBoundary and/or resource locks
~~~

No reverse `fg->Mutex -> setConstantsMutex` acquisition was found in the inspected source. The remaining concern is re-entry from external/vendor callbacks and the internal owner-2 unlock, not a claim that every lock path currently forms a proven deadlock cycle.

State declarations are in State.h:288, 313-318. XeFG lifecycle state is in XeFG_Dx12.h:21-30. The four-slot resource arrays and per-slot mutex are in IFGFeature_Dx12.h:14-63.

## 7. Lock-order and re-entry graph

### 7.1 Current lock domains

~~~text
REFramework hook_monitor_mutex
  -> REF present_common callback
     -> OptiScaler fg->Mutex owner 2
        -> XeFG Present/Dispatch
           -> per-frame resource reads/writes are inconsistent

REFramework hook_monitor_mutex
  -> REF ResizeBuffers1 callback
     -> OptiScaler fg->Mutex owner 6678
        -> GPU idle wait
        -> original ResizeBuffers1

OptiScaler _swapchainLifecycleMutex
  -> REF PreRetireSwapchain handoff
  -> OptiScaler fg->Mutex owner 1
     -> context/object/queue retirement

Streamline setConstantsMutex
  -> ScopedStreamlineFGTransaction
     -> fg->Mutex owner 2 unless already owned by this thread
        -> _frameBoundaryMutex inside CheckForFrame
        -> XeFG EvaluateState and metadata writes

Streamline reportResource
  -> ScopedStreamlineFGTransaction
     -> fg->Mutex owner 2 unless already owned by this thread
        -> _frameBoundaryMutex inside CheckForFrame
        -> _resourceMutex[index] inside SetResource

Streamline evaluateState / markPresent
  -> ScopedStreamlineFGTransaction
     -> fg->Mutex owner 2 unless already owned by this thread
        -> frame/state operation
~~~

The corrected first-A/B order specified by the work order is now present in commit 1199 for the four adapter entry points:

~~~text
fg->Mutex
  -> _frameBoundaryMutex
    -> _resourceMutex[index]
~~~

The implementation must not acquire fg->Mutex while holding frameBoundary or a resource mutex. Commit 1199 detects any current-thread ownership regardless of logical owner ID through `OwnedMutex::isOwnedByCurrentThread()` at `OwnedMutex.h:29-33`. That avoids the prescribed same-thread/different-owner self-deadlock for the covered entries. It does not make `OwnedMutex` recursive in general, and `EvaluateState` can still call `unlockThis(2)` internally at `XeFG_Dx12.cpp:1361-1362`; see SDLX-015.

### 7.2 REFramework comparison

The current REFramework fork's present_common holds hook_monitor_mutex across the renderer callback and the original Present call: D3D12Hook.cpp:1679-1829. ResizeBuffers and ResizeBuffers1 similarly hold the monitor across their callback and original call: D3D12Hook.cpp:2022-2255.

The current fork's XeFG factory transition explicitly releases the REF monitor before the downstream factory call and reacquires it afterward: D3D12Hook.cpp:803-855. This is the current split-factory fix and should not be reported as the old unconditional factory-lock inversion.

REF public-proxy pre-retire keeps the public proxy alive, locks the REF lifecycle monitor, verifies context and window identity, discards pending handoff, and detaches the binding: XeFGCompatibility.cpp:255-345. The exported ABI is REFramework_XeFG_PreRetireSwapchainV1 at XeFGCompatibility.cpp:495-509. OptiScaler calls this handoff before entering its FG mutex retirement section: XeFG_Dx12.cpp:1866-1895 and 1954-2008.

The REF side therefore reduces the historical factory/pre-retire inversion risk. It does not serialize OptiScaler Streamline input state with OptiScaler Present/Dispatch state.

## 8. Resource lifecycle, COM lifetime, and vendor contract

### 8.1 Streamline lifecycle contract

The bundled Streamline headers define:

- eOnlyValidNow: the resource may change, be destroyed, or be reused after the provided call completes.
- eValidUntilPresent: it must remain unchanged until the frame is presented.
- eValidUntilEvaluate: it must remain unchanged until slEvaluateFeature returns.

References: external/streamline/sl_core_types.h:380-394.

The DLSSG contract also returns an internal inputs-processing fence and value. A client must wait before modifying or destroying inputs from a previously presented frame on a non-presenting queue, and must retrieve the state on the present thread: external/streamline/sl_dlss_g.h:170-177.

### 8.2 Adapter behavior

Sl_Inputs_Dx12::reportResource converts every lifecycle other than eValidUntilPresent to FG_ResourceValidity::ValidNow at the current tip's Streamline_Inputs_Dx12.cpp:368-375. If the command list is absent, it rewrites the result to UntilPresent at lines 371-375. The hkslEvaluateFeature hook can call reportResource before the original slEvaluateFeature call returns: Streamline_Hooks.cpp:309-338; commit 1199 wraps the adapter portion in the new transaction but does not move the original API call into that scope.

This is not a missing SetResource lock. SetResource does take the selected per-frame exclusive lock at XeFG_Dx12.cpp:1608-1616, mutates the map, tags the resource, and updates readiness at lines 1690-1848. The issue is the semantic conversion and cross-API transaction boundary around that lock.

In the supplied MHW OptiScaler logs, all counted Reporting SL resource entries used eValidUntilPresent:

- Release-09/mhw root: 17,889 reports, 0 eValidUntilEvaluate reports, 0 YOLO validity rewrites.
- Release-09/mhw/Intel: 9,155 reports, 0 eValidUntilEvaluate reports, 0 YOLO validity rewrites.
- Release-09/mhw/새 폴더 (2): 20,374 reports, 0 eValidUntilEvaluate reports, 0 YOLO validity rewrites.
- Release-09/mhw/새 폴더 (3): 18,789 reports, 0 eValidUntilEvaluate reports, 0 YOLO validity rewrites.

Therefore the lifecycle-conversion defect is source-confirmed but NOT_EXERCISED by the observed MHW FG submissions. The startup logs still show Streamline registering eValidUntilEvaluate tags for DLSS and eValidUntilPresent tags for DLSS_G, which confirms both contract families are present in the runtime.

### 8.3 Raw pointer after lock

IFGFeature_Dx12::GetResource takes a shared lock, locates an element, returns a raw pointer, and destroys the lock on return: IFGFeature_Dx12.cpp:159-174. XeFG Present then dereferences the returned UI and hudless pointers at XeFG_Dx12.cpp:1485-1555. NewFrame can clear the same map under an exclusive lock at IFGFeature_Dx12.cpp:176-193.

GetResourceData is read without its own lock at XeFG_Dx12.cpp:417-466, but the production SetResource caller invokes it while holding the selected _resourceMutex at lines 1616-1794. It is not promoted as a separate finding; the escaped GetResource pointer remains independently unsafe for callers that use it after return.

### 8.4 COM pointer ownership

The wrapper's constructor stores QueryInterface results in _real1 through _real4 and immediately releases each result: wrapped_swapchain.cpp:422-451. The stored pointers therefore do not carry an owned reference.

In the D3D12 LocalPresent path, QueryInterface returns cq, cq is released at wrapped_swapchain.cpp:252-254, and cq is then passed to CheckForRealObject and used for GetDevice and timing work at lines 259-293. The queried device12 is likewise released at lines 268-270 and stored/used at lines 277-278.

WaitForGPUIdle releases queue at wrapped_swapchain.cpp:60-64 and then calls queue->Signal at lines 66-96. This is a direct use-after-release pattern. The helper also does not fail closed on CreateFence, Signal, SetEventOnCompletion, or a five-second timeout.

ReflexHooks has the same lifetime pattern for its static device12: QueryInterface followed by Release at Reflex_Hooks.cpp:107-122, then reuse at lines 124-125. The current evaluateState body does not dereference the device argument, so this is a real ownership defect but not a demonstrated E-abort instruction.

## 9. CPU/GPU queue and fence analysis

### 9.1 Producer/consumer contract

Streamline requires the GPU payload that generates a tagged resource to be submitted on the provided command buffer, or on an earlier command buffer guaranteed to execute before it: external/streamline/sl_core_api.h:136-173.

The inspected path has these operations:

1. reportResource passes the game command buffer into SetResource.
2. SetResource calls XeFG D3D12TagFrameResource with the resource and command list at XeFG_Dx12.cpp:1794-1827.
3. Resource tracking recognizes a command list in hkExecuteCommandLists.
4. hkExecuteCommandLists calls SetResourceReady before invoking the original ExecuteCommandLists, then calls SetCommandQueue only after the original returns: ResTrack_dx12.cpp:625-696.
5. SetCommandQueue stores the queue in a ComPtr-backed _ownedGameCommandQueue, but the type argument is ignored: XeFG_Dx12.cpp:1856-1864.
6. XeFG Present/Dispatch runs on the presentation path and has no explicit wait on the DLSSG inputsProcessingCompletionFence in the inspected code.

The ready flag is thus a CPU-side submission observation, not a GPU completion fence. The E-abort log's distinct_same_device queue relation makes this relevant, but no per-resource queue/fence timeline is available because the target folder has no OptiScaler log.

### 9.2 DLSSG state warnings

The paired normal MHW logs contain Streamline warnings that slDLSSGGetState must be synchronized with the present thread, especially when using the inputs-processing completion fence. Examples are:

- Release-09/mhw root OptiScaler.log around line 702 and line 10516.
- Release-09/mhw/Intel OptiScaler.log around line 703 and line 9572.

hkslDLSSGGetState copies the fence fields for older structure versions but does not wait on or otherwise consume them: Streamline_Hooks.cpp:772-838. This confirms that the fence contract is live in the runtime, while the inspected adapter does not turn it into an explicit resource-retirement or cross-queue dependency.

### 9.3 What can and cannot be concluded

The evidence supports:

- two direct queues on one device;
- DLSSG fence state exists and Streamline warns about present-thread synchronization;
- resource readiness and queue capture are separate CPU events;
- no explicit client-side wait on the copied fence was found.

The evidence does not support:

- a claim that the vendor's default eBlockPresentingClientQueue mode failed;
- a claim that the E_ABORT was caused by a particular queue submission;
- a claim that queue selection must be changed before a focused A/B demonstrates it.

## 10. Present immutability and lifecycle crossover

### 10.1 Present transaction

FGHooks::FGPresent holds fg->Mutex owner 2 across fg->Present and the original Present/Present1 call: FG_Hooks.cpp:1224-1322 at commit 1199. Inside XeFG Present, Dispatch can mutate _lastDispatchedFrame and call TagFrameConstants and SetPresentId: IFGFeature.cpp:134-159 and XeFG_Dx12.cpp:985-1283.

That lock gives Present a coherent transaction with the covered commit-1199 adapter methods because they use the same `fg->Mutex` domain and any-owner current-thread check. The original Streamline hook calls remain outside the adapter scope, and direct framegen/resource/lifecycle paths remain only partially protected. The current transaction also has the SDLX-015 exception: `EvaluateState` can release owner 2 internally while `setConstants` is still executing. Consequently the source now has an intended CPU serialization boundary, but not a proof that a frame's constants, resource-ready flags, metadata arrays, resource map, and GPU visibility form one immutable publication.

### 10.2 Resize

FGHooks resize paths use owner 6677 or 6678 and call WaitForGPUIdle before the original resize: FG_Hooks.cpp:620-649 and 872-904. The current REF logs show six ResizeBuffers1 calls with six successful original returns in each complete Release-09 paired session. This is evidence that the observed resize path completed; it does not prove that the queue and resource state are safe under all concurrent input schedules.

Wrapped ResizeBuffers1 publishes the supplied present queue into State.currentCommandQueue and _device at wrapped_swapchain.cpp:1239-1244, but it does not directly commit that queue into XeFG's _gameCommandQueue. XeFG's queue is committed at creation or by resource tracking. This creates a conditional queue identity split when a resize supplies a new queue.

### 10.3 Release and shutdown

XeFG ReleaseSwapchain obtains _swapchainLifecycleMutex and performs REF pre-retire before entering ReleaseSwapchainLocked. The current PR35 behavior skips REF pre-retire only when State.isShuttingDown is true: XeFG_Dx12.cpp:1866-1895.

XeFG Shutdown itself does not acquire _swapchainLifecycleMutex: XeFG_Dx12.cpp:970-982. Present, Dispatch, SetResource, and Streamline callbacks also do not take that lifecycle mutex. State.isShuttingDown is a raw bool at State.h:288. This leaves an internal drain/snapshot gap even though the REF handoff and wrapper final-release guard reduce several historical teardown races.

The paired 새 폴더 (2) crash proves that a final proxy-retire sequence can still be followed by an exception in the combined application, but it does not prove which raw object or callback caused that exception. It must be tracked separately from the E-abort gameplay path.

## 11. Findings

### 11.1 Findings summary

The detailed records below each include exact source/evidence, producer-consumer or reader-writer pair, reachable interleaving, consequence, runtime boundary, and a proposed correction that was not implemented by this audit.

| ID | Log severity | Source impact | Confidence | Category | Current status |
|---|---|---|---|---|---|
| SDLX-001 | Blocking | High | High source / low occurrence | Streamline lifecycle / resource lifetime | Source-confirmed; MHW `eValidUntilEvaluate` path NOT_EXERCISED |
| SDLX-002 | Blocking | High | High source / medium reachability | Resource-map data race | Source-confirmed; runtime not evidenced |
| SDLX-003 | Blocking | Critical | High | Resource lifetime / escaped pointer | Source-confirmed; runtime occurrence not evidenced |
| SDLX-004 | Blocking | High | High c8 / medium current residual | Frame ordering / missing happens-before | c8 split; 1199 transaction added; logging-OFF validation NOT_EXERCISED |
| SDLX-005 | Blocking | High | High source / runtime candidate | Frame identity / ring ABA | Source-confirmed risk; runtime not evidenced |
| SDLX-006 | Blocking | High | Medium | D3D12 queue / GPU ordering | Missing explicit dependency; runtime candidate |
| SDLX-007 | Blocking | High | Medium | Resize / queue generation | Conditional source split; runtime not evidenced |
| SDLX-008 | Blocking | Critical | High | COM lifetime / UAF | Source-confirmed; E-abort causality not evidenced |
| SDLX-009 | Blocking | High | High general / medium covered-path | Mutex ownership / re-entry | General nonrecursive risk remains; covered any-owner guard added |
| SDLX-010 | Blocking | High | Medium | Lifecycle drain / context lifetime | Source-confirmed gap; runtime not evidenced |
| SDLX-011 | Non-blocking improvement | Medium | High | Fence/error handling | Source-confirmed robustness gap |
| SDLX-012 | Theoretical | Medium | High | Frame arithmetic | Source-confirmed dead logic; runtime not evidenced |
| SDLX-013 | not evidenced | Medium diagnostic | High ordering | Monitor interpretation / recovery | Present stop precedes timeout; not an initiating-failure finding |
| SDLX-014 | Non-blocking improvement | Low | High limited claim | Instrumentation | Balanced counters do not clear UAF |
| SDLX-015 | Blocking | High | High source / runtime not evidenced | Mutex ownership / transaction scope | Current-tip residual: `EvaluateState` can unlock outer owner 2 |

### SDLX-001 — Streamline lifecycle semantic collapse

- Log severity: Blocking
- Source impact: High
- Confidence: High for source defect; low for occurrence in the E-abort run
- Category: Streamline contract / resource lifetime
- Status: Source-confirmed; runtime NOT_EXERCISED
- Exact source: c8f1fd0d, OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp:298-335; OptiScaler/hooks/Streamline_Hooks.cpp:309-338; external/streamline/sl_core_types.h:380-394
- Producer: Streamline/game ResourceTag carrying eValidUntilEvaluate or eOnlyValidNow
- Consumer: Sl_Inputs_Dx12::reportResource, IFGFeature_Dx12::SetResource, and XeFG D3D12TagFrameResource
- Bad interleaving: hkslEvaluateFeature calls reportResource before the original slEvaluateFeature returns; reportResource converts eValidUntilEvaluate to ValidNow; SetResource can submit a resource to XeFG with only-now semantics before the Streamline evaluate call has completed.
- Consequence: XeFG may receive a weaker lifetime guarantee than the Streamline contract requires, or the no-command-list fallback may rewrite the contract in the opposite direction to UntilPresent. Either case can cause invalid asynchronous resource consumption or retention behavior.
- Runtime boundary: All counted MHW FG resource submissions were eValidUntilPresent, with zero eValidUntilEvaluate reports and zero YOLO rewrites. This finding is not the observed E-abort root.
- Proposed correction, not implemented: preserve the lifecycle meaning through the adapter, reject unsupported combinations explicitly, or make a deliberate copy/retention decision tied to slEvaluateFeature completion. Do not hide the conversion behind logging or delay.

### SDLX-002 — HasResource reads the frame map without its mutex

- Log severity: Blocking
- Source impact: High
- Confidence: High for source defect; medium for reachability
- Category: Resource map data race / frame selection
- Status: Source-confirmed; runtime not evidenced
- Exact source: c8f1fd0d, OptiScaler/framegen/IFGFeature_Dx12.cpp:41-47; callers in OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp:358-432; writers in OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1616-1699 and OptiScaler/framegen/IFGFeature_Dx12.cpp:176-193
- Writer: SetResource inserts or updates _frameResources[fIndex]; NewFrame clears it.
- Reader: HasResource directly calls _frameResources[index].contains(type) without a lock; reportResource and frame-index selection call it.
- Bad interleaving: Thread A executes HasResource while Thread B clears or mutates the same unordered_map during NewFrame or SetResource.
- Consequence: C++ container data-race/undefined behavior, a false resource-present result, or selection of the wrong ring slot.
- Runtime boundary: No log records the exact overlapping threads or map operation. Successful normal sessions do not disprove a race.
- Proposed correction, not implemented: protect the read with the same per-frame mutex or use a lock-owned/snapshot accessor. Keep this separate from the first causal A/B unless needed for deterministic execution.

### SDLX-003 — GetResource returns a pointer after releasing the lock

- Log severity: Blocking
- Source impact: Critical
- Confidence: High
- Category: Resource lifetime / use-after-invalidation
- Status: Source-confirmed; runtime occurrence not evidenced
- Exact source: c8f1fd0d, OptiScaler/framegen/IFGFeature_Dx12.cpp:159-174 and 176-193; consumers in OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1485-1555
- Writer/invalidation: NewFrame obtains _resourceMutex[fIndex] and clears _frameResources[fIndex].
- Reader/consumer: GetResource returns a raw pointer to an unordered_map element; XeFG Present later dereferences ui or hudless and calls GetResource on it.
- Bad interleaving: Present calls GetResource, the shared lock is destroyed at return, NewFrame acquires the exclusive lock and clears the map, then Present dereferences the stale Dx12Resource pointer.
- Consequence: use-after-invalidation or use-after-free-like behavior in UI/hudless processing, with potential corruption or crash.
- Runtime boundary: The available MHW logs do not record pointer reuse or the exact concurrent frame-clear interleaving. The complete normal sessions only establish that this path can run without visibly failing in those captures.
- Proposed correction, not implemented: return a value/snapshot or hold ownership/lock through the full consumer operation. Apply as a separate resource-hardening phase; do not naively hold the lock across code that re-enters SetResource.

### SDLX-004 — Pinned Streamline-input/Present split; current transaction unvalidated

- Log severity: Blocking
- Source impact: High
- Confidence: High that the split existed at c8; Medium that the current residuals caused the target failure
- Category: Frame ordering / missing happens-before
- Status: Pinned-base defect; commit 1199 adds the prescribed Phase 1 adapter transaction; logging-OFF runtime validation NOT_EXERCISED
- Exact source: c8f1fd0d, Streamline_Hooks.cpp:213-340 and 691-700; Streamline_Inputs_Dx12.cpp:7-41 and 54-249 and 284-446; FG_Hooks.cpp:1184-1326. Current tip 1199b5f8, Streamline_Inputs_Dx12.cpp:7-43, 92-100, 291-299, 324-348, and 489-500; OwnedMutex.h:29-33
- Producer: Streamline tags, constants, EvaluateState, and Reflex PRESENT_START frame bookkeeping.
- Consumer: FGPresent, XeFG Present, Dispatch, TagFrameConstants, SetPresentId, and original DXGI Present.
- Bad interleaving at c8: CheckForFrame releases _frameBoundaryMutex before the remainder of setConstants/reportResource; setConstants uses only setConstantsMutex; reportResource enters per-resource locking only inside SetResource; meanwhile Present owns fg->Mutex owner 2 and reads/updates the same frame state. At commit 1199 the four adapter methods acquire the same FG mutex first, but direct callers, original Streamline API calls, GPU visibility, and the SDLX-015 internal unlock remain outside a complete transaction.
- Consequence: constants, frame identity, readiness, resource metadata, and vendor frame tags can be assembled from different logical frames even when each individual map operation is locally protected.
- Runtime boundary: The E-abort session confirms Present activity stopped before REF timeout, but has no OptiScaler log to show the input/Present interleaving. The current-tip implementation was not built or exercised in the prescribed logging-OFF A/B passes.
- Proposed correction, not implemented by this audit: validate commit 1199 after reconciling the SDLX-015 owner-2 unlock, with order fg->Mutex -> _frameBoundaryMutex -> _resourceMutex[index]. Keep same-thread re-entry owner-agnostic and preserve the separation from the later resource/GPU/lifecycle phases.

### SDLX-005 — Live frame-count/index arithmetic permits stale or ABA mapping

- Log severity: Blocking
- Source impact: High
- Confidence: High for source risk; runtime not evidenced
- Category: Frame identity / four-slot ring
- Status: Source-confirmed risk; runtime candidate
- Exact source: c8f1fd0d, SysUtils.h:31; Streamline_Inputs_Dx12.cpp:337-354; IFGFeature.cpp:5-32, 134-159, and 180-193; XeFG_Dx12.cpp:1817-1827
- Producer: CheckForFrame writes _frameIdIndex; StartNewFrame and SetFrameCount update _frameCount; markPresent writes a frame count from Reflex.
- Consumer: reportResource selects a slot, and SetResource later derives frameId from live _frameCount, live GetIndex, and indexDiff.
- Bad interleaving: A resource captures fIndex for frame N, then a frame advance or markPresent changes _frameCount before SetResource computes frameId; after four slots, the same fIndex can represent a later frame.
- Consequence: D3D12TagFrameResource may be submitted with a frame ID that does not match the resource's Streamline tag or current constants.
- Runtime boundary: No captured log correlates every tag, constants, resource slot, and Present ID across a ring wrap. The observed reports have sequential frame IDs in the sampled tails, but that is not a proof of all interleavings.
- Proposed correction, not implemented: carry an immutable frame transaction/snapshot from frame selection through resource tagging, or serialize the relevant operations under the first A/B transaction. Do not infer correctness from sequential log output alone.

### SDLX-006 — CPU readiness and queue/fence dependency are not one contract

- Log severity: Blocking
- Source impact: High
- Confidence: Medium
- Category: D3D12 queue ownership / GPU ordering
- Status: Source-confirmed missing explicit dependency; runtime candidate
- Exact source: c8f1fd0d, external/streamline/sl_core_api.h:136-173; external/streamline/sl_dlss_g.h:170-177; OptiScaler/hooks/Streamline_Hooks.cpp:772-838; OptiScaler/resource_tracking/ResTrack_dx12.cpp:625-696; OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1856-1864
- Producer: Game command lists and Streamline/DLSSG input resources.
- Consumer: XeFG presentation queue, Dispatch, and vendor frame processing.
- Bad interleaving: Resource tracking marks SetResourceReady before original ExecuteCommandLists returns; SetCommandQueue is committed only afterward; a separate presentation queue may dispatch the ready resource without an explicit wait on the producer queue or the DLSSG inputs-processing fence.
- Consequence: XeFG can observe a CPU-ready resource before its producer GPU work is ordered for the presentation queue, causing stale/invalid input or a device/fatal-D3D failure.
- Runtime evidence: E-abort REF explicitly records init queue 0x8f5f2980 and presentation queue 0x7619afa0 on the same device. Normal logs also contain Streamline warnings about present-thread synchronization of DLSSG state.
- Runtime boundary: No per-resource fence value, queue submission, or device-removal reason is available for E-abort because OptiScaler.log is missing. Vendor default blocking behavior may add an implicit dependency; this audit cannot claim that it failed.
- Proposed correction, not implemented: instrument and prove producer/consumer queue ordering, then use the contractually required fence/wait or a verified vendor blocking mode. Do not change queue selection without a reproducing evidence pair.

### SDLX-007 — ResizeBuffers1 queue publication can diverge from XeFG queue ownership

- Log severity: Blocking
- Source impact: High
- Confidence: Medium
- Category: Resize / queue generation
- Status: Source-confirmed conditional split; runtime not evidenced
- Exact source: c8f1fd0d, OptiScaler/wrapped/wrapped_swapchain.cpp:1207-1276; OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1856-1864; OptiScaler/resource_tracking/ResTrack_dx12.cpp:682-689
- Producer: ResizeBuffers1 ppPresentQueue and wrapper State.currentCommandQueue/_device publication.
- Consumer: XeFG _gameCommandQueue used for UI/SC execution and vendor processing.
- Bad interleaving: ResizeBuffers1 receives a new present queue and updates global raw aliases, but XeFG's owned _gameCommandQueue is not directly updated by that branch; it changes only through creation or later resource tracking.
- Consequence: GPU idle waits, command-list execution, and XeFG input processing can refer to different queue identities across a resize generation.
- Runtime boundary: E-abort and complete comparison sessions show ResizeBuffers1 success 6/0, and E-abort's binding queue is stable in the REF view. No log proves a queue change at a failing resize.
- Proposed correction, not implemented: correlate queue identity with swapchain generation and update ownership only after direct evidence shows a real transition. Keep queue changes separate from the first Streamline ordering A/B.

### SDLX-008 — Wrapped DXGI and Reflex interfaces are used after Release

- Log severity: Blocking
- Source impact: Critical
- Confidence: High
- Category: COM lifetime / use-after-release
- Status: Source-confirmed; runtime cause not evidenced
- Exact source: c8f1fd0d, OptiScaler/wrapped/wrapped_swapchain.cpp:53-99, 148-279, and 422-451; OptiScaler/hooks/Reflex_Hooks.cpp:107-125
- Producer: QueryInterface returns interface references.
- Consumer: Wrapper member interfaces, LocalPresent, WaitForGPUIdle, and Reflex evaluateState.
- Bad interleaving: The constructor QIs _real1.._real4 and releases them immediately; LocalPresent releases cq and device12 before subsequent use; WaitForGPUIdle releases queue before Signal; Reflex retains a released static device12.
- Consequence: deterministic use-after-release when the path is reached, with undefined vtable access and potential startup, Present, resize, or marker-path crash.
- Runtime boundary: Normal comparison sessions can survive this path, and dinput8 QI/AddRef/Release counts are numerically balanced, but those counters do not prove ownership of the retained raw interface pointers. The supplied E-abort artifact has no OptiScaler log tying this defect to the failure.
- Proposed correction, not implemented: retain owned ComPtr/interface references for the entire consumer operation and release only after the final use. Add HRESULT and lifetime assertions before relying on the path. This should be a separate safety fix and independently validated from the frame-order A/B.

### SDLX-009 — General OwnedMutex re-entry remains nonrecursive

- Log severity: Blocking
- Source impact: High
- Confidence: High for the general mechanism; Medium that the covered commit-1199 paths would hit it after the helper; runtime target occurrence not evidenced
- Category: Mutex ownership / re-entry
- Status: The original owner-specific hazard is mitigated for the four commit-1199 adapter entries by an any-owner query; `OwnedMutex` remains nonrecursive in general, and SDLX-015 is a separate current-tip residual
- Exact source: c8f1fd0d, OptiScaler/OwnedMutex.h:8-48; FG_Hooks.cpp:1221-1322 and 898-904; XeFG_Dx12.cpp:1360-1362 and 1984-2041. Current tip 1199b5f8, OwnedMutex.h:8-69 and Streamline_Inputs_Dx12.cpp:7-43
- Producer/owner: FGPresent owner 2, resize owner 6678, release owner 1, and a future Streamline transaction owner.
- Consumer: Same OS thread entering another path with a different logical owner.
- Bad interleaving for an uncovered/general path: A thread holds fg->Mutex under one logical owner and re-enters another path that calls `lock` with a different owner. The underlying shared_mutex is nonrecursive, so the second lock can block itself. The current helper's any-owner query avoids acquisition for the four covered adapter entries, but it does not change `lock` semantics for other callers. EvaluateState's explicit owner-2 unlock is tracked separately in SDLX-015.
- Consequence: same-thread deadlock or ownership confusion remains possible if another wrapper or direct path enters `OwnedMutex::lock` without the any-owner guard.
- Runtime boundary: Logs show owner/re-entry diagnostics in several paths, but no captured stack proves this exact target deadlock. The E-abort monitor evidence shows a stopped Present, not a mutex wait stack.
- Proposed correction, not implemented by this audit: require the any-owner guard on every intended re-entry path, and reconcile all internal unlocks with scope ownership before extending the transaction. Preserve the required lock order.

### SDLX-010 — XeFG shutdown does not share the lifecycle mutex with active work

- Log severity: Blocking
- Source impact: High
- Confidence: Medium
- Category: Lifecycle drain / context lifetime
- Status: Source-confirmed gap; runtime not evidenced for E-abort
- Exact source: c8f1fd0d, OptiScaler/framegen/xefg/XeFG_Dx12.cpp:970-982, 1293-1366, 1485-1598, and 1866-2044; OptiScaler/State.h:288; OptiScaler/framegen/xefg/XeFG_Dx12.h:21-30
- Producer: Shutdown, final proxy release, ReleaseSwapchain, and state transitions.
- Consumer: Present, Dispatch, Streamline callbacks, command-list execution, and vendor context calls.
- Bad interleaving: Shutdown releases or destroys FG objects without taking _swapchainLifecycleMutex while Present/Dispatch or a Streamline callback is still reading the same raw context/resources; isShuttingDown is a raw global flag.
- Consequence: callback-after-destroy, stale context use, or a teardown crash.
- Runtime boundary: The current REF pre-retire handoff and wrapper final-release guard reduce the risk, and the E-abort run has no detach/rebind before the Present stop. No direct Opti callback-after-shutdown stack is present.
- Proposed correction, not implemented: define a bounded drain/snapshot protocol after the causal A/B, keeping REF lifecycle ABI and PR35 behavior unchanged. Do not pull the lifecycle mutex into the first frame-input experiment without a demonstrated need.

### SDLX-011 — GPU-idle helpers fail open on synchronization errors

- Log severity: Non-blocking improvement
- Source impact: Medium
- Confidence: High
- Category: Fence/error handling
- Status: Source-confirmed robustness gap; runtime not evidenced
- Exact source: c8f1fd0d, OptiScaler/hooks/FG_Hooks.cpp:83-100 and OptiScaler/wrapped/wrapped_swapchain.cpp:53-99
- Producer: CreateFence, Signal, SetEventOnCompletion, and WaitForSingleObject.
- Consumer: ResizeBuffers and ResizeBuffers1, which proceed after the helper returns.
- Bad interleaving: Fence creation or Signal fails, or the five-second wait times out; the helper logs or ignores the result and resize continues as if the GPU were idle.
- Consequence: backbuffer/resource destruction can overlap outstanding GPU work.
- Runtime boundary: The supplied REF logs show successful ResizeBuffers1 returns but do not expose every Opti fence HRESULT or timeout result.
- Proposed correction, not implemented: check and propagate HRESULTs, treat timeout as an explicit failed synchronization state, and record the queue/fence generation. This is supporting hardening, not proof of E-abort cause.

### SDLX-012 — Unsigned frame-difference negative check is dead logic

- Log severity: Theoretical
- Source impact: Medium
- Confidence: High
- Category: Frame arithmetic
- Status: Source-confirmed code defect; runtime not evidenced
- Exact source: c8f1fd0d, OptiScaler/framegen/IFGFeature.cpp:7-12 and 134-145
- Producer: UINT64 _frameCount and _lastDispatchedFrame.
- Consumer: GetIndexWillBeDispatched and GetDispatchIndex.
- Bad interleaving/condition: diff is unsigned, so diff < 0 can never be true; underflow is interpreted as a large positive value instead of a negative frame distance.
- Consequence: the intended branch for backward/underflow detection is ineffective, potentially changing dispatch-slot selection during unusual frame-count transitions.
- Runtime boundary: No captured log proves that an underflow occurred. This is not promoted as the E-abort root.
- Proposed correction, not implemented: use explicit unsigned underflow-safe comparisons or a signed checked difference, then add targeted ring-wrap tests.

### SDLX-013 — Hook-monitor timeout is an observed effect, not an initiating failure

- Log severity: not evidenced
- Source impact: Medium as a diagnostic misinterpretation
- Confidence: High for ordering; no root-cause inference
- Category: Runtime interpretation / recovery
- Status: Runtime-observed effect
- Exact evidence: C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004\re2_framework_log.txt:1843-1849, 4817-4829, and subsequent Last chance lines
- Producer: Game/graphics path ceasing Present activity.
- Consumer: REFramework hook_monitor at REFramework.cpp:66-145.
- Bad interleaving: Present age exceeds five seconds while inside_present is false; the monitor emits Last chance, then XeFG preserve_grace and quarantine. It does not receive a paired Opti call trace.
- Consequence: attributing E_ABORT to monitor recovery would invert the observed causal order.
- Runtime boundary: The CrashReport message and game AV are separate artifacts; no direct call chain from the monitor to E_ABORT is shown.
- Proposed correction, not implemented: retain the monitor markers as post-failure telemetry and collect an OptiScaler log plus device-removed reason around the first missing Present.

### SDLX-014 — Balanced wrapper counters are not ownership proof

- Log severity: Non-blocking improvement
- Source impact: Low
- Confidence: High for the limited claim
- Category: Runtime instrumentation
- Status: Runtime-observed non-finding
- Exact evidence: Complete Release-09 paired sessions report dinput8 QI/AddRef/Release counts of 12601/12604/12604, 7459/7462/7462, 9922/9925/9925, and 9082/9084/9084; nonzero LocalPresent results are empty.
- Producer: Wrapper instrumentation.
- Consumer: Audit interpretation of COM safety.
- Bad interleaving: A QueryInterface result can be released and later reused even when aggregate AddRef/Release counts balance.
- Consequence: a balanced count cannot clear SDLX-008.
- Runtime boundary: No nonzero LocalPresent result or repeated rehook pattern was found in these captures.
- Proposed correction, not implemented: add lifetime-specific assertions or owned-pointer diagnostics; do not use aggregate counters as a substitute for ownership analysis.

### SDLX-015 — Commit-1199 transaction can be released inside EvaluateState

- Log severity: Blocking
- Source impact: High
- Confidence: High for the current-tip mechanism; runtime occurrence not evidenced
- Category: Mutex ownership / transaction scope
- Status: Source-confirmed current-tip residual; runtime NOT_EXERCISED
- Exact source: 1199b5f8433c0693b01a91d94dd4fb13dfccfaf5, OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp:7-43 and 92-100; OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1293-1362; OptiScaler/OwnedMutex.h:29-52
- Producer/owner: `ScopedStreamlineFGTransaction` acquires `fg->Mutex` as logical owner 2 and records `_locked = true`.
- Consumer/releaser: `XeFG_Dx12::EvaluateState` and the transaction destructor.
- Reachable bad interleaving: A thread enters `Sl_Inputs_Dx12::setConstants`, passes the any-owner check, and acquires owner 2. `setConstants` calls `fgOutput->EvaluateState`. If `State::FGchanged` is true, `EvaluateState` checks `Mutex.isOwnedByCurrentThread(2)` and calls `Mutex.unlockThis(2)` at lines 1361-1362. Control then returns to `setConstants`, which continues its metadata/state writes without the outer transaction. On scope exit, the RAII object still believes it owns the mutex and calls `unlockThis(2)` again; with owner 0 it logs and returns, or it can observe a different owner if another thread acquires the mutex first.
- Consequence: the intended input-to-Present serialization can be lost in the middle of `setConstants`; a competing Present/input path can mutate or consume the same live frame state, and the scope's ownership accounting no longer matches the underlying mutex. The same early release is possible when the standalone `evaluateState` adapter entry is wrapped, although its post-call window is smaller.
- Runtime boundary: Existing MHW logs were produced by older binaries and do not contain a current-tip marker proving `FGchanged` overlapped this scope. No commit-1199 build or logging-OFF A/B was run.
- Proposed correction, not implemented: reconcile the internal FGchanged transition with the outer transaction ownership before accepting the Phase 1 experiment. The correction must preserve the intended owner-agnostic same-thread re-entry and must not turn the audit into a broad lifecycle rewrite.

## 12. Work-order hypothesis classification

| Work-order hypothesis | Classification | Evidence |
|---|---|---|
| 4.1 SetResource already has an exclusive per-frame resource lock | Confirmed source fact; not root by itself | XeFG_Dx12.cpp:1608-1616 and 1690-1848 |
| 4.2 GetResource pointer can outlive the map lock | Confirmed source defect; runtime not evidenced | IFGFeature_Dx12.cpp:159-174; XeFG_Dx12.cpp:1485-1555 |
| 4.3 HasResource is an unlocked map read | Confirmed source defect; runtime not evidenced | IFGFeature_Dx12.cpp:41-47 |
| 4.4 Dispatch reads shared state outside the resource lock | Confirmed source condition; candidate chain | XeFG_Dx12.cpp:985-1283; IFGFeature.cpp:134-193 |
| 4.5 CheckForFrame lock is narrower than the input transaction | Confirmed at c8; current tip adds an outer transaction for four adapter entries; full frame transaction not proven | c8 Streamline_Inputs_Dx12.cpp:7-41 and 54-446; current 1199 Streamline_Inputs_Dx12.cpp:7-43, 92-100, 291-299, 324-348, 489-500 |
| 4.6 Present's FG mutex does not cover Streamline input hooks | Confirmed at c8; commit 1199 adds intended shared FG mutex coverage; runtime validation and SDLX-015 remain | c8 FG_Hooks.cpp:1221-1322 versus Streamline_Hooks.cpp:213-340 and 691-700; current 1199 Streamline_Inputs_Dx12.cpp:7-43 |
| 4.7 OwnedMutex is nonrecursive and owner-specific | Any-owner query now mitigates the covered four-entry re-entry; general nonrecursive semantics remain and internal owner-2 unlock is SDLX-015 | c8 OwnedMutex.h:8-48; current 1199 OwnedMutex.h:8-69; XeFG_Dx12.cpp:1361-1362 |
| 4.8 Logging changes timing | Work-order observation; no independent A/B in this audit | E-abort has no OptiScaler.log; source logging is synchronous |
| 4.9 Distinct same-device queues require a proven GPU dependency | Runtime queue split confirmed; missing dependency candidate | E-abort REF QueueIdentity line 986; Streamline/DLSSG contracts; ResTrack source |
| 4.10 EvaluateState can release the new transaction's owner 2 internally | Confirmed current-tip residual; runtime not evidenced | 1199 XeFG_Dx12.cpp:1293-1362; Streamline_Inputs_Dx12.cpp:92-100; OwnedMutex.h:35-52 |

### 12.1 Explicit answers to the work-order questions

The answers below distinguish the pinned c8 behavior, the current 1199 source, and what the available runtime artifacts actually prove.

1. **Can `GetResource` return a pointer that is invalidated immediately after its lock is released?** Yes. `IFGFeature_Dx12::GetResource` holds a shared lock only while finding the map element, then returns a raw `Dx12Resource*`; `NewFrame` can subsequently clear the map under an exclusive lock. The exact E-abort occurrence is not evidenced.
2. **Can `HasResource` race with map mutation?** Yes. It reads the per-frame map without the corresponding resource mutex. This is a source-confirmed data-race/undefined-behavior candidate, not a logged E-abort proof.
3. **Can `SetFrameCount` or `StartNewFrame` overlap `Dispatch`?** Yes through direct/uncovered paths: both frame-counter operations are not universally guarded by `fg->Mutex`, while Dispatch relies on the Present-side domain. Commit 1199 covers `CheckForFrame` and `markPresent` when the helper is active, but it does not serialize every direct caller or make all frame state atomic.
4. **Can an N+1 callback mutate state while N is in Present?** At c8, yes, through the split input/Present domains. At 1199, covered adapter callbacks on another thread wait on the same FG mutex; same-thread re-entry intentionally skips reacquisition. The original Streamline API calls, direct framegen callers, and the SDLX-015 early-unlock path remain residual overlap routes.
5. **Can the four-slot ring reuse a slot while an older frame still uses it?** Yes. The ring is finite and the slot is selected from live frame/index state. No immutable generation token or ownership proof spans input publication, GPU completion, and Present consumption.
6. **Are tags, constants, and Present IDs proven to belong to the same logical frame?** No. The logs show sequential frame IDs in sampled paths, but source still derives some IDs from live counters and separate arrays, and no end-to-end correlation binds every resource tag, constants packet, and Present ID to one immutable frame context.
7. **Is tag/resource publication ordered before Present for the same frame?** The current adapter gives the covered CPU calls an explicit `fg->Mutex` order before Present, with frame/resource locks nested afterward. The original Streamline calls and GPU command execution are outside that CPU scope, so the full tag-to-Present contract is not proven.
8. **Is there a concrete GPU synchronization dependency?** Not in the inspected frame-input path. A CPU readiness flag is set before the original `ExecuteCommandLists` returns, and no explicit wait on the DLSSG inputs-processing fence was found in the adapter.
9. **What happens with different queues and fences?** The E-abort REF log records distinct initialization and presentation queues on the same device. The inspected source has no demonstrated per-resource cross-queue wait/fence handoff for this path; the missing OptiScaler log prevents correlating the failing run's submissions.
10. **What Streamline thread/order contract is established?** The bundled headers state that tag/constants APIs are thread-safe, while `slEvaluateFeature` is not thread-safe; DLSSG state warns that fence retrieval is present-thread synchronized. They do not provide evidence that all relevant callbacks share one thread or one ordering with OptiScaler Present.
11. **Can resize, deactivate, release, or shutdown overlap a callback?** Yes in the source model. Lifecycle locking is not shared by all Streamline/Present/Shutdown paths. Resize has its own FG owner and REF monitor coverage, while `Shutdown` does not take `_swapchainLifecycleMutex`.
12. **Can a callback use a retired context?** Yes, as a reachable lifetime candidate: callbacks load `State.currentFG` as a raw pointer and do not participate in the lifecycle mutex/drain protocol. REF pre-retire reduces the proxy handoff window but does not prove all OptiScaler callbacks are drained.
13. **Can `OwnedMutex` self-deadlock when the same thread re-enters with another owner?** Before 1199, yes. Current `isOwnedByCurrentThread()` mitigates this for the four new adapter scopes, but `OwnedMutex::lock` remains nonrecursive for general callers. The owner-2 internal unlock is the separate SDLX-015 residual.
14. **Can vendor/DXGI calls occur while reacquirable locks are held?** Yes. FGPresent holds owner 2 across XeFG/vendor work and original Present; resize holds its owner across GPU-idle/original resize; REF holds its monitor across callbacks/original calls. A concrete cycle is not proven, but re-entry and lock-order risk is real.
15. **Is logging itself a required dependency?** No proof supports that conclusion. The work order reports timing sensitivity, the E-abort folder lacks the paired OptiScaler log, and this audit did not run a logging-on/off A/B. Logging should be treated as perturbation telemetry, not synchronization.
16. **What invariant would make logging on/off equivalent?** The frame must be published as one immutable generation-bound snapshot, with owned resource lifetime and an explicit producer-to-consumer GPU dependency; logging must not decide ownership, readiness, or retirement. The same snapshot and fence outcome must hold with logging enabled or disabled.
17. **What is the narrowest architecture that makes a frame immutable?** Keep the current narrow FG transaction as the CPU gate after fixing SDLX-015, carry a generation/slot/frame-ID snapshot through resource publication, publish an owned frame context once, and have Present consume that snapshot only after the producer queue/fence dependency is satisfied. This avoids a global lock while separating later resource/lifecycle hardening from the first A/B.

### 12.2 Per-function lock and ownership table

| Function/path | Caller/thread role | Locks or ownership actually used at current tip | Re-entry/lifetime observation |
|---|---|---|---|
| `hkslSetTag` / `hkslSetTagForFrame` | Streamline tag hook | No hook-wide lock; `reportResource` enters the scoped FG transaction for each adapter report | Original Streamline tag call runs after the adapter scope returns |
| `hkslEvaluateFeature` | Streamline evaluate hook | No outer hook-wide lock; each supplied resource can enter `reportResource`'s scoped transaction | Original `slEvaluateFeature` is called outside it and is not thread-safe per header contract |
| `hkslSetConstants` | Streamline constants hook | `setConstantsMutex` around custom handling and original call; custom handling enters the scoped FG transaction | Lock nesting is `setConstantsMutex -> fg->Mutex`; no reverse acquisition found |
| `Sl_Inputs_Dx12::CheckForFrame` | Frame detection from input/Present marker paths | `_frameBoundaryMutex` exclusive for frame transition and `_frameIdIndex` write | No universal FG lock when called directly; covered by 1199 callers that enter the helper first |
| `Sl_Inputs_Dx12::setConstants` | Streamline input state | Scoped FG transaction, then `_frameBoundaryMutex` inside `CheckForFrame`; no resource lock for metadata setters | Calls `XeFG_Dx12::EvaluateState`; SDLX-015 can release the outer owner 2 early |
| `Sl_Inputs_Dx12::reportResource` | Streamline/resource tagging | Scoped FG transaction, `_frameBoundaryMutex` in `CheckForFrame`, selected `_resourceMutex[index]` in `SetResource` | Intended order is correct; lifecycle and GPU fence are not included |
| `Sl_Inputs_Dx12::evaluateState` | Reflex render-submit marker | Scoped FG transaction only | `EvaluateState` can internally unlock owner 2 when `FGchanged` is true |
| `Sl_Inputs_Dx12::markPresent` | Reflex Present marker | Scoped FG transaction then `_frameBoundaryMutex`; calls `SetFrameCount` | Current tip joins this marker path to the Present mutex domain |
| `IFGFeature::StartNewFrame` | Frame advance | No `fg->Mutex`; virtual `NewFrame` handles its own resource locks | Direct calls can overlap Present/Dispatch unless caller supplies the outer transaction |
| `IFGFeature::GetDispatchIndex` / getters | Present/input frame selection | No mutex | Reads live counters/readiness/maps; ring ABA candidate |
| `IFGFeature::SetFrameCount` and setters | Reflex/input state writes | No mutex | Live state can be changed outside the Present transaction by direct callers |
| `IFGFeature_Dx12::HasResource` | Resource presence query | No per-frame resource mutex | Direct unordered-map read can race `SetResource`/`NewFrame` |
| `IFGFeature_Dx12::GetResource` | UI/hudless lookup | Shared lock only through lookup/return | Raw pointer escapes after unlock; map invalidation candidate |
| `IFGFeature_Dx12::NewFrame` | Ring-slot retirement | Exclusive per-frame resource locks while clearing | Does not establish ownership of pointers already returned |
| `XeFG_Dx12::SetResource` | Resource publication | Selected `_resourceMutex[fIndex]` exclusive through map/vendor tag/readiness work | Calls external D3D12/XeFG operations under the resource lock; outer FG lock only on covered input path |
| `XeFG_Dx12::EvaluateState` | Constants/marker state | No independent mutex | Relies on caller; contains the current owner-2 internal unlock |
| `XeFG_Dx12::Dispatch` | Framegen vendor dispatch | No independent mutex | Normally reached under FGHooks owner 2, but direct state reads are not self-protected |
| `XeFG_Dx12::Present` | XeFG presentation | No independent mutex | Expects FGHooks to hold `fg->Mutex`; dereferences resources after `GetResource` releases its lock |
| `FGHooks::FGPresent` | DXGI Present/Present1 hook | `fg->Mutex` owner 2 across XeFG Present and original Present | Re-entry uses owner-specific check in hook; current input helper uses any-owner check |
| `FGHooks::hkResizeBuffers` / `hkResizeBuffers1` | Swapchain resize hooks | FG owner 6677/6678 across pause, GPU-idle wait, and original resize | Long critical section around external/DXGI work; separate from Streamline transaction |
| `XeFG_Dx12::ReleaseSwapchain` / `ReleaseSwapchainLocked` | Final proxy retirement | Lifecycle mutex at entry, REF pre-retire, then FG owner 1 in locked retirement | `Shutdown` does not use the same lifecycle mutex |
| `XeFG_Dx12::Shutdown` | Global teardown | No `_swapchainLifecycleMutex` | Callback-after-retire remains a source candidate |
| `hkExecuteCommandLists` | D3D12 queue submission tracking | Tracking mutex around lookup/readiness; `SetResourceReady` before original Execute, queue commit after | CPU readiness is not GPU completion/fence proof |
| `WrappedIDXGISwapChain::Present` / `LocalPresent` | Wrapper Present path | Wrapper local mutex owner 4 when enabled; LocalPresent performs transient QIs | QI results are released before later use in D3D12 path |
| `WrappedIDXGISwapChain::ResizeBuffers1` | Wrapper resize path | Wrapper local owner 2 and optional FG owner 3 around wait/original resize | Publishes State queue/device but does not directly update XeFG owned queue |
| REF `present_common` / resize hooks | REFramework monitor boundary | `hook_monitor_mutex` across callback and original Present/resize | Current factory split releases monitor before downstream factory; current Present/resize still span external callbacks |

## 13. Corrective priorities

This is an audit result, not an implementation plan to be applied automatically. The following order preserves causal clarity:

### P0 — Safety gate: reconcile current transaction ownership and definite COM violations

Review SDLX-015 before accepting commit 1199 as a valid transaction experiment: the internal owner-2 unlock must not release an outer scope that still has work to do. Separately correct SDLX-008 before relying on long-duration A/B results. Keep owned interface references alive through their last use, and validate all QueryInterface/fence operations. Run focused transaction and wrapper smoke tests after the corrections. Do not claim that either correction proves the E-abort root.

### P1 — Causal A/B required by the instruction

Commit 1199 already contains the narrow transaction shape. After SDLX-015 is reconciled, validate that implementation with:

- an any-owner same-thread query to OwnedMutex;
- conditional acquisition of the existing fg->Mutex before frameBoundary/resource locks;
- coverage of setConstants, reportResource, evaluateState, and markPresent;
- use order fg->Mutex -> _frameBoundaryMutex -> _resourceMutex[index];
- handle same-thread re-entry without lock/unlock ownership confusion;
- do not add sleeps, forced logging, Intel/MHW/vendor conditions, or REF changes.

The decisive test is logging OFF. Logging-on controls are for deadlock/performance comparison and must not be treated as proof. Do not stack resource, queue, or lifecycle rewrites onto this experiment until its result is isolated.

### P2 — Resource and queue hardening after A/B result

If the A/B confirms the frame-order hypothesis:

- replace raw pointer-after-unlock access with an owned snapshot or lock-contained operation;
- make HasResource and Dispatch state access explicit and consistent;
- avoid holding _resourceMutex across SetResource re-entry;
- instrument resource producer queue, ExecuteCommandLists submission, XeFG consumer queue, fence values, and Present ID;
- resolve ResizeBuffers1 queue-generation transitions only when direct evidence requires it;
- audit lifecycle drain and Shutdown separately from the frame-input A/B.

If the A/B does not change the failure rate, isolate or revert that experiment before stacking these changes.

### P3 — Acceptance matrix

The work order requires, at minimum:

- five independent Intel MHW launches with logging off;
- at least ten minutes of actual gameplay per run or longer than the historical failure window;
- no Fatal D3D E_ABORT, Capcom crash report, REF exception dump, Present hang, or unclean exit;
- two logging-on control sessions;
- three NVIDIA MHW shutdown exits preserving PR35;
- three Intel MHW shutdown exits;
- LuaSmoke marker coverage, changed-binding, long-minimize, resize, and runtime toggle coverage where applicable;
- clang-format and normal CI.

None of those new acceptance runs was performed in this audit. Existing sessions have successful partial resize/Present coverage, but they are not a substitute for the prescribed matrix.

## 14. Validation performed for this report

- Pinned OptiScaler worktree created at c8f1fd0d in an isolated detached worktree; the final report was then reviewed/rebased on audited tip 1199b5f8 after the remote branch advanced.
- Original D:\repo\OptiScaler checkout was not modified; its pre-existing M .gitignore and untracked doc/ state were preserved.
- REFramework source was inspected from the fork without modifying its pre-existing untracked analysis directories.
- Bundled log analyzer executed for Release-09/mhw, Release-09/E-abort 4004, and PR19/MHW.
- Raw log context checked for initialization, binding, Present, resize, queue identity, timeout, proxy retirement, crash, and Lua markers.
- No code build was run because the request was an audit/report, not an implementation or build request.
- No source, binary, or configuration implementation change was made as part of the audit. The only authored repository change is this report; no PR was opened.

## 15. Completion checklist

| Requirement | Result | Evidence |
|---|---|---|
| Audit the pinned Release 0.9 work order and supplied instruction | PASS | c8f1fd0d work-order base, 41b7d308 supplied instruction, and current 1199b5f8 tip recorded in Sections 1 and 2 |
| Analyze OptiScaler fork code | PASS | End-to-end source anchors and SDLX findings |
| Compare the REFramework fork code | PASS | ABI reference, current fork sensitivity comparison, Present/resize/factory/pre-retire review |
| Analyze available MHW logs/dumps | PASS for available artifacts | Release-09, E-abort, and PR19 candidates separated by evidence level |
| Keep session health separate from feature coverage | PASS | Section 2.2 and Section 3.3 |
| Classify Lua validation accurately | PASS | No LuaSmoke marker; NOT_EXERCISED |
| Do not claim an unproven E_ABORT root | PASS | Executive verdict and every runtime boundary |
| Place report beside the instruction | PASS | Published under doc/work-order beside both the original work order and the exact supplied instruction path |
| Review remote branch movement before publication | PASS | Current origin/reframework-0.9 tip 1199b5f8 reviewed; report rebased onto it |
| Publish the audit report to the fork | PASS | Report commit is published to onehoon/OptiScaler `reframework-0.9` |
| Preserve PR34/PR35 scope | PASS | Existing wrapper final-release and shutdown pre-retire behavior were reviewed and no source change was made |
| Exact requested C:\GoogleDrive\ref-xefg\0914\mhw folder available | BLOCKED by external state | Folder absent; no artificial folder created; existing candidates analyzed separately |
| Implement the Phase 1 fix | NOT REQUESTED / NOT DONE BY THIS AUDIT | Commit 1199 contains the transaction implementation; SDLX-015 remains unvalidated and no source fix was authored here |
| Run the Phase 1 acceptance matrix | NOT REQUESTED for this audit / NOT DONE | No new runtime A/B or gameplay validation performed |

## 16. Source and artifact index

### OptiScaler source

- OptiScaler/hooks/Streamline_Hooks.cpp
- OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp
- OptiScaler/framegen/IFGFeature.h
- OptiScaler/framegen/IFGFeature.cpp
- OptiScaler/framegen/IFGFeature_Dx12.h
- OptiScaler/framegen/IFGFeature_Dx12.cpp
- OptiScaler/framegen/xefg/XeFG_Dx12.h
- OptiScaler/framegen/xefg/XeFG_Dx12.cpp
- OptiScaler/hooks/FG_Hooks.cpp
- OptiScaler/hooks/Reflex_Hooks.cpp
- OptiScaler/resource_tracking/ResTrack_dx12.cpp
- OptiScaler/wrapped/wrapped_swapchain.cpp
- OptiScaler/OwnedMutex.h
- OptiScaler/State.h
- external/streamline/sl_core_api.h
- external/streamline/sl_core_types.h
- external/streamline/sl_dlss_g.h

### REFramework source

- src/REFramework.cpp
- src/D3D12Hook.cpp
- src/compatibility/xefg/XeFGCompatibility.cpp
- src/compatibility/xefg/XeFGCandidateHandoff.cpp

### Runtime artifacts

- C:\GoogleDrive\ref-xefg\Release-09\mhw
- C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004
- C:\GoogleDrive\ref-xefg\PR19\MHW

This report records what the available evidence establishes. It does not replace the required logging-OFF causal A/B and does not authorize a code change by itself.
