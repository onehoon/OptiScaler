# Release 0.9 — OptiScaler / REFramework XeFG D3D12 Coexistence Crash Audit Report

Date: 2026-09-16
Audit instruction: RELEASE_0_9_OPTISCALER_REFRAMEWORK_XEFG_D3D12_COEXISTENCE_CRASH_AUDIT_INSTRUCTION_2026-09-16.md
Audit mode: source audit and evidence correlation only
Implementation performed: none
Pull request created: none

## 1. Executive verdict

The audit reconstructed the intended coexistence contract across the pinned OptiScaler and REFramework source tips. The design is not a single-owner redesign: OptiScaler owns the public frame-generation lifecycle and the game-side queue path, while REFramework observes and temporarily binds the XeFG presentation lifecycle. The two projects coordinate through runtime identity, generation, the pre-retire ABI, monitor serialization, and bounded COM keepalives.

The normal source-controlled paths are coherent:

- OptiScaler retires the REF binding before entering its FG mutex section.
- REF detaches its presentation hooks while holding the REFramework hook-monitor mutex.
- REF binding generation and runtime slot/context identity prevent an old final-release path from detaching a newer binding.
- PR55 late forwarding re-reads the current target instead of calling a stale saved vtable target blindly.
- PR56 releases the monitor around downstream factory work and rehooks once the nested in-flight count reaches zero.
- Opti PR34/35/36/37/38 behavior is present at the audited Opti source baseline.

That reconstruction does not justify a global source-safe verdict. Two residual crash-capable defects remain:

| Finding | Classification | Owner | Current consequence |
|---|---|---|---|
| COEX-001 — concurrent Intel XeFG public-proxy release can skip lifecycle retirement | CONDITIONAL CRASH-CAPABLE DEFECT | OptiScaler public-proxy release hook, with REF stale-binding consequence | The last two legitimate COM releases can drive the Intel proxy to zero without either call entering pre-retire. OptiScaler and REF then retain stale raw relationships; a later recreate or pre-retire can call a freed proxy. |
| COEX-002 — REFramework D3D12Hook destruction locks a destroyed monitor mutex | CONFIRMED CRASH-CAPABLE DEFECT | REFramework shutdown owner lifetime | REFramework does not reset m_d3d12_hook in its destructor body. Reverse member destruction destroys m_hook_monitor_mutex first, then D3D12Hook::~D3D12Hook calls unhook and locks the ended mutex. |

The first finding is the cross-component coexistence escape. The second is a separate REFramework-owned D3D12 shutdown defect found because the instruction explicitly requires the D3D12Hook destruction path to be audited. It is not caused by OptiScaler and should not be presented as an Opti-to-REF callback defect.

Therefore:

- Confirmed residual crash-capable defects: 1.
- Conditional residual crash-capable defects: 1.
- Audited path that escapes the intended coexistence safeguards: yes, the concurrent public-proxy release branch and the REFramework shutdown destruction-order branch.
- Normal single-threaded Present, Present1, ResizeBuffers, ResizeBuffers1, non-preserve recreation, explicit release, runtime destroy, factory transition, and REF detach paths: safe by the source invariants described below, subject to the vendor callback/module-lifetime boundaries called out as insufficient evidence.
- Overall source-safe verdict for every audited lifecycle path: no.
- No broad architecture-quality judgment is made. The existing coordinated coexistence model is retained; only the two causal residual paths are isolated.

### Audit goal disposition

| Goal | PASS / FAIL / BLOCKED | Evidence |
|---|---|---|
| Pin both repositories and reconstruct the combined lifecycle | PASS | OptiScaler 0be0cb7a2bc77ad50762986f4f29eaefce5e79b2 and REFramework e4a1a1d1faf06f4e25923dacf765c00213c86ada were audited in isolated clean worktrees. |
| Trace shared hooks, object lifetimes, locks, generations, shutdown, and runtime transitions | PASS | Sections 3 through 9 and the 26-question completion gate cover every required path. |
| Classify only concrete crash-capable residual paths | PASS | Sections 10 through 12 separate confirmed, conditional, safe, and insufficient-evidence candidates. |
| Produce the requested report without implementation changes | PASS | This report is documentation-only; no source, build, or PR changes were made during the audit. |
| Prove full current runtime safety | FAIL | The available logs are historical and do not exercise the current tips or the two-thread release interleaving; COEX-001 remains conditional and COEX-002 is source-confirmed. |

## 2. Pinned revision matrix

### 2.1 Source revisions

| Component | Audited revision | Commit description | Role in this audit |
|---|---|---|---|
| OptiScaler | 0be0cb7a2bc77ad50762986f4f29eaefce5e79b2 | docs: clarify coexistence baseline for crash audit | Exact final OptiScaler tip named by the instruction; documentation-only tip after the code baseline. |
| OptiScaler code baseline | 036aae89e31447b21e1cab4e8d2c5c72421f9dc7 | fix: harden DX12 wrapper COM ownership (#38) | Last relevant OptiScaler implementation commit before the audit/report documents. |
| OptiScaler PR37 | b0385ad6 | EvaluateState outer transaction ownership | Existing owner-2 transaction behavior applied to the audit. |
| OptiScaler PR36 | 1199b5f8 | Streamline/Present transaction handling | Existing transaction scope applied to the audit. |
| OptiScaler PR35 | d6b0132f | shutdown ownership/pre-retire behavior | Existing shutdown gate applied to the audit. |
| OptiScaler PR34 | ac1e48fa | wrapper final-release reentrancy | Existing wrapper final-release guard applied to the audit. |
| REFramework | e4a1a1d1faf06f4e25923dacf765c00213c86ada | Fix XeFG factory lock inversion with split REF monitor scope (#56) | Exact final REFramework source tip named by the instruction. |
| REFramework PR55 | 79b29c073eae08e818b5f220f01f25eb6382f086 | late callback forwarding | Existing late vtable-target recovery applied to the audit. |
| REFramework PR54 | 4bf45b37 | pre-retire handoff | Pre-retire detach path predecessor and current ABI context. |
| REFramework PR53 | 28c0002c | exact destroy reconciliation | Runtime identity/result reconciliation context. |
| kananlib dependency | 8c27b656734355db0f2893581fd62e838fa130ad | pinned VtableHook dependency | Audited through the CMake FetchContent pin; VtableHook has no COM ownership. |

The OptiScaler delta from the PR38 code baseline to the named final tip is documentation-only. No source commit touching the audited implementation landed during this audit. The REFramework source remained at the exact PR56 tip.

### 2.2 Evidence revisions and limitations

The available paired MHW logs under C:\GoogleDrive\ref-xefg\Release-09 were collected from older binaries:

| Evidence | Observed revision | Limitation |
|---|---|---|
| OptiScaler MHW log | OptiScaler v0.9.5-pre4, e9ca4685 | Not the pinned 0be0 source tip. |
| REFramework MHW log | c6704372c5808ea0c49059a23afa4ee12cc39e8d | Not the pinned e4a1 source tip. |
| E_ABORT capture | Intel Arc G3 Extreme; C0000005; Fatal D3D error E_ABORT 0x80004004 | No paired OptiScaler.log in the immediate crash evidence set; no current-tip attribution is possible. |
| MHW folder (2) final-release sequence | Historical Opti/REF paired logs | Shows a serialized safe-detached path followed closely by a crash, but cannot validate or falsify the current tips. |
| MHW folder (3) shutdown sequence | Historical Opti log | Shows process-shutdown pre-retire suppression; no current REFramework destructor proof. |

The previously named C:\GoogleDrive\ref-xefg\0914\mhw path was not available as a separate evidence directory during this audit. The Release-09 material is retained as historical supporting context only, not as current runtime validation.

## 3. Current coexistence architecture and intended safety invariants

### 3.1 Ownership division

| State/object | OptiScaler role | REFramework role | Intended invariant |
|---|---|---|---|
| Public XeFG proxy | Installs global code hooks for Present, Present1, resize methods, and Release; initiates final-proxy retirement when the first original Release reports refcount 1. | Does not hook Release; receives a pre-retire call and detaches its presentation binding. | REF must detach before Opti destroys the old presentation relationship. |
| Wrapped DXGI swapchain | Opti wrapper controls the wrapper final-release sequence and the real swapchain/interface releases. | REF may observe the physical swapchain through its D3D12 hook and XeFG binding. | Opti wrapper aliases are non-owning state references; concrete COM release stays in the wrapper path. |
| XeFG context | XeFG_Dx12 stores the active vendor context and owns the game command queue through _ownedGameCommandQueue. | REF runtime hooks observe Init/Get/Destroy and bind a presentation session to runtime identity. | Context is unpublished before destroy; failed destroy quarantines rather than silently proceeding. |
| REF binding | Opti invokes the dynamic pre-retire ABI with public proxy, context, and hwnd. | XeFGBinding stores a borrowed swapchain pointer but owns queue/device ComPtrs; generation and runtime identity are explicit. | The borrowed pointer is valid only inside the caller-bounded hook lifetime and detach interval. |
| REF instance hooks | Opti does not own the REF VtableHook objects. | VtableHook copies the target vtable and restores it only when the object still points to that copy. | REF vtable restoration must not overwrite a changed vtable owned by another layer. |
| D3D12 queue/device | Opti has raw State aliases plus an owned game-queue ComPtr and resize synchronization ComPtrs. | REF binding owns canonical queue/device ComPtrs and compares COM/device identity. | Different wrapper addresses are not by themselves different queues; released queue/device use requires a separate proof. |
| Hook monitor | Opti lifecycle and FG mutexes are separate from REF monitor serialization. | REFramework m_hook_monitor_mutex serializes Present/Resize, detach, unhook, monitor recovery, and factory transition bookkeeping. | REF hook mutation cannot overlap a REF callback that holds the monitor mutex. |

### 3.2 OptiScaler invariants reconstructed

1. State::currentSwapchain, currentWrappedSwapchain, currentRealSwapchain, and currentFGSwapchain are explicitly non-owning tracking aliases. Their comments state that concrete lifetime remains with the owner. State also stores currentFGSwapchainGeneration, currentD3D12Device, and currentCommandQueue as raw tracking values: OptiScaler/State.h:300-318.
2. XeFG_Dx12 has a separate _swapchainLifecycleMutex, release-in-progress/owner-thread state, and _ownedGameCommandQueue: OptiScaler/framegen/xefg/XeFG_Dx12.h:21-30.
3. PrepareREFForSwapchainRetire is called before ReleaseSwapchainLocked enters the configured FG Mutex section: XeFG_Dx12.cpp:1897-1939 and 1953-1997.
4. ReleaseSwapchainFromFinalProxyRelease rejects a stale final proxy before touching the current context, and its release closure clears aliases before making the final original Release: XeFG_Dx12.cpp:1911-1933.
5. PR37 leaves the owner-2 frame transaction with the caller. EvaluateState does not own the caller's frame transaction: XeFG_Dx12.cpp:1360-1361. Streamline_Inputs_Dx12.cpp:11-42 acquires/releases the outer transaction, and :92-167 calls EvaluateState inside it.
6. Opti ResizeBuffers1 forwards the complete creation-node-mask and present-queue arrays to the underlying swapchain. It also uses the first present queue as the current raw diagnostic alias in the wrapper path: wrapped_swapchain.cpp:1238-1290 and :1368-1375.
7. The resource tracking ExecuteCommandLists hook marks SetResourceReady before the original ExecuteCommandLists and updates the command queue afterward, without a GPU fence: ResTrack_dx12.cpp:625-698. This remains the separate SDLX-006/007 synchronization issue and is not promoted into this coexistence crash audit because the REF path does not materially change it.

### 3.3 REFramework invariants reconstructed

1. XeFGBinding::m_swapchain is borrowed. m_queue and m_device are ComPtr-owned. commit and clear advance/reset generation as a unit: XeFGBinding.cpp:15-86.
2. XeFGPresentationSession::consistent_with requires an active hook, active binding, source classification, matching renderer swapchain/queue/device aliases, and the correct hook target: XeFGPresentationSession.cpp:13-20.
3. Runtime detach validates slot/context and, where allowed, hwnd; it stores an old_keepalive while resetting renderer state and VtableHook objects, then clears binding: D3D12Hook.cpp:301-367.
4. The public pre-retire ABI creates a temporary public_proxy_keepalive before the monitor checks, then holds the REF monitor through identity validation and detach: XeFGCompatibility.cpp:255-345.
5. Runtime Init/Destroy transitions hold the monitor and pending-candidate coordination, but release the monitor before entering the Intel original call: XeFGCompatibility.cpp:96-117 and :461-509.
6. PR55 late forwarding reads the current vtable slot, rejects null/self targets, and calls that target when the object is readable. It does not create a COM keepalive for an object that has already been destroyed: D3D12Hook.cpp:113-125 and the Present/Resize late paths.
7. PR56 counts nested factory transitions while the monitor is held, unlocks it around downstream factory work, then decrements and performs one final rehook when in_flight reaches zero: D3D12Hook.cpp:788-935.
8. The pinned VtableHook dependency copies/restores vtables but does not AddRef or Release a COM target. Its remove operation restores only if the current vptr still equals its private replacement vtable.

## 4. Combined architecture / callback diagram

~~~text
Game / engine
   |
   | D3D12 device + command queue
   v
OptiScaler DXGI factory hooks
   |  DXGI CreateSwapChain / CreateSwapChainForHwnd
   v
Opti wrapped swapchain ------------------------------+
   |                                                  |
   | D3D12InitFromSwapChainDesc                       | wrapper Present/Resize/Release
   v                                                  v
Opti XeFG_Dx12 -------------------------------> Opti FGHooks
   |  raw context, raw public aliases                  |
   |  owned game queue ComPtr                          | code detours
   |                                                  v
   |                                      Intel XeFG public proxy
   |                                                  |
   |  dynamic pre-retire ABI                           | Present/Resize/Release
   v                                                  v
REFramework XeFGCompatibility <--- runtime Init/Get/Destroy hooks
   |
   | monitor mutex + runtime identity + pending candidate
   v
REFramework D3D12Hook
   |  REF VtableHook on Present/Present1/Resize/Resize1/ResizeTarget
   |  borrowed binding swapchain; owned queue/device; generation
   v
Intel XeFG presentation swapchain
   |
   +--> present_common / resize callbacks
   +--> REF on_reset and VtableHook detach
   +--> original current target / Opti detour / Intel implementation

Normal retirement:
Opti lifecycle mutex
   -> REF pre-retire ABI
      -> REF hook-monitor mutex
         -> renderer reset, VtableHook removal, binding clear
   -> Opti FG mutex
      -> context/object cleanup
      -> exactly one final public-proxy release

Known escape:
two concurrent Opti FGHooks Release calls
   -> each artificial AddRef
   -> neither first original Release returns 1
   -> both final artificial releases consume the object
   -> no pre-retire, raw aliases/binding remain stale
~~~

The diagram intentionally distinguishes the public Intel proxy from the REF binding swapchain. Historical logs show different addresses for those objects, and the source treats the REF binding pointer as borrowed rather than as a second public-proxy owner.

## 5. Shared hook-chain matrix

The actual original target is the vendor/DXGI implementation reached after the active vtable and code-detour layers. A two-hook chain is not itself a defect: REF copies the current vtable target, while Opti detours the target function. The chain remains valid while both modules and the object remain alive.

| Callback / entry | OptiScaler layer | REFramework layer | Before/after target relationship | Install and uninstall behavior | Late/nested behavior | Current safeguard and disposition |
|---|---|---|---|---|---|---|
| Present, vtable slot 8 | FGHooks captures the slot and Detours-attaches hkFGPresent/FGPresent. | D3D12Hook VtableHook installs present at slot 8; present_common holds the REF monitor. | REF copied slot points to the function address currently in the object vtable; that address may be Opti's Detours-patched entry, which then calls Opti's saved trampoline and Intel original. | REF resets its private VtableHook under the monitor; Opti code detour remains process-wide in the audited path. | present_common detects recursion and uses the original target; late path reads current slot and rejects self/null. | Safe by monitor serialization and current-target lookup while object/module lifetime is valid. Post-destruction vendor callback remains insufficient evidence. |
| Present1, vtable slot 22 | FGHooks captures and detours the Intel proxy Present1 path. | D3D12Hook hooks slot 22 and QI's the swapchain before present_common. | Same code-detour/vtable relationship as Present; REF's ComPtr exists for the QI result during the call. | Reset with the other REF instance hooks; Opti static original remains. | Present1 re-enters present_common under the recursive monitor. | Safe for a live caller-owned object; no source proof of an after-final-release vendor entry. |
| ResizeBuffers, vtable slot 13 | FGHooks/wrapper takes the relevant Opti FG/wrapper locks, waits on the Opti resize queue fence where configured, and forwards the original call. | D3D12Hook holds the monitor, performs pre-reset policy, calls the current target, and completes resize hold bookkeeping. | REF current target can be Opti's patched function; no vtable overwrite is performed by REF removal unless its vptr is still its own copy. | Reset and rehook are monitor-serialized. | Thread-local resize depth forwards nested direct calls to the original target. | Safe by monitor + resize depth + vptr identity; GPU fence semantics are a separate SDLX scope. |
| ResizeBuffers1, vtable slot 39 | Opti forwards pCreationNodeMask and ppPresentQueue arrays and records the first queue as a raw diagnostic alias. | REF forwards the full arrays and applies the XeFG resize/reset policy. | Same target chain; arrays are not reduced to one queue at the forwarding boundary. | Both layers reset their state around recreation; no source proof of an array truncation. | Nested depth bypasses duplicate policy and calls the current original. | Safe by source for object lifetime. Queue identity mismatch is diagnostic, not a crash proof. |
| ResizeTarget, vtable slot 14 | Opti captures/detours the slot as part of the FG proxy hook set. | REF hooks slot 14 and applies the reset callback policy. | REF vtable copy and Opti code detour compose without a slot overwrite. | REF removal is guarded by its vptr check. | Late current-target resolution is used when the instance hook is absent/mismatched. | Safe by current-target and monitor rules; vendor post-destruction callback not proven. |
| Public proxy Release, vtable slot 2 | Opti FGHooks detours the Intel proxy Release. It performs AddRef, first original Release, result-1 detection, pre-retire, and the final original Release. | REF does not hook Release; it is entered only through Opti's dynamic pre-retire ABI. | REF VtableHook does not change slot 2. Opti's Release detour is the lifecycle authority for the public proxy. | Opti static pointers remain for process lifetime; there is no REF Release slot to restore. | Same-thread lifecycle reentry is forwarded; concurrent calls are not serialized by the static skip flag. | Normal single-thread path is protected. COEX-001 is a conditional escape when two concurrent calls both miss result 1. |
| Wrapped swapchain Release | Opti WrappedIDXGISwapChain4::Release uses interlocked final-release and PR34 reentrant guards, then calls XeFG retirement before releasing real interfaces. | REF sees the physical/binding object only through its D3D12 callbacks and pre-retire handoff. | This is a separate wrapper object path from the Intel public-proxy Release detour. | Wrapper final flag and alias cleanup are local to the wrapper. | Reentrant final release is consumed once; stale generation is release-only. | Safe for the wrapper path; PR34 does not cover COEX-001 because COEX-001 is the Intel proxy hook. |
| CreateSwapChainForHwnd, factory slot 15 | Opti DXGI factory detours resolve the real factory/device and route creation through FGHooks/XeFG. | REF VtableHook observes factory slot 15 and D3D12Hook splits its monitor scope around downstream creation. | The current factory target is captured before downstream call; REF does not hold the monitor over the downstream call. | PR56 uses in_flight and rehooks once at zero. | Nested calls increment the same monitor-protected count; HRESULT return paths decrement. | Safe on normal return paths; exceptional unwinding/vendor unload is not proven. |
| XeFG InitFromSwapChainDesc | Opti creates the XeFG context and records the public proxy/queue path. | XeFGRuntimeRegistry hook resolves the original, observes the init transaction, and publishes a candidate after the Intel call. | No shared vtable slot; runtime registry returns a synchronized original trampoline. | Registry slots are installed under its mutex; no unload/uninstall notification is present. | RuntimeTransitionScope suppresses conflicting monitor recovery. | Safe while the runtime module remains loaded. Module unload races are insufficient evidence. |
| XeFG GetSwapChainPtr | Opti uses the vendor result to obtain the swapchain pointer. | REF observes and validates the candidate, queue, device, hwnd, and COM identity relation. | REF stores the swapchain as borrowed and queue/device as owned ComPtrs. | Candidate is applied through generation-aware handoff. | Pending candidates are discarded on matching runtime transition. | Safe by identity/generation for source-controlled transitions. Vendor lifetime after callback is insufficient evidence. |
| XeFG Destroy | Opti DestroySwapchainContext clears its context before calling the vendor destroy and quarantines failures. | REF detaches first, calls the original Destroy, then reconciles only an exact detached runtime identity and success result. | Runtime transition releases the monitor before the Intel original call. | Hook and binding clear precede or accompany destroy reconciliation. | A failed result leaves detached uncertainty and suppresses unsafe generic recovery. | Safe by the source contract; actual vendor result/reentrancy behavior is not independently observable. |
| D3D12 device/queue Release | Opti D3D12 hooks manage State device aliases and use ComPtr queue/fence holders in the relevant paths. | REF binding queue/device ComPtrs keep its own binding objects alive. | Raw aliases are not ownership; identity comparison uses underlying COM objects/device. | Opti device hook has explicit Unhook; REF binding clears under monitor. | Resize and runtime transitions clear matching aliases. | No released queue/device dereference caused by the other side is source-proven. |

## 6. Shared object ownership / lifetime matrix

| Object or pointer | Concrete owner | Borrowers / observers | Retirement sequence | Cross-boundary protection | Residual assessment |
|---|---|---|---|---|---|
| State::currentFGSwapchain | No COM owner; raw tracking alias | Opti lifecycle, FGHooks, XeFG_Dx12 recreate/release | Cleared on successful retirement and generation reset; stale-final path clears before release-only return. | Generation comparison and current-proxy comparison. | COEX-001 proves a reachable stale-alias escape when concurrent proxy Release reaches zero without the lifecycle branch. |
| State::currentSwapchain and wrapper aliases | Wrapper/lifecycle code is the concrete owner | DXGI factory and wrapper paths | Cleared during wrapper final-release and XeFG final-proxy release. | PR34 final-release flag, generation checks, release closure. | Safe on the wrapper path; raw alias lifetime is not independently protective. |
| WrappedIDXGISwapChain4 and its real swapchain | Wrapper's final-release path and its real COM references | Opti State, XeFG_Dx12, REF physical binding | Wrapper final flag, optional FG retirement, release of _real1.._real4 and real object, delete wrapper. | Reentrant final release is consumed; current generation is checked. | Source-safe for normal wrapper release. |
| Intel XeFG public proxy | Intel COM reference count | Opti FGHooks; REF pre-retire receives a raw pointer argument | Normal path uses artificial AddRef, first result-1 test, REF detach, then final original Release. | Temporary REF public_proxy_keepalive covers detach call only. | COEX-001: two concurrent hook calls can release the object to zero without retirement. |
| XeFG swapchain context | Vendor context held as raw Opti state | Opti XeFG_Dx12; REF runtime identity | Opti nulls before Destroy; success clears; failure retains/quarantines according to result. | REF exact context/slot identity and detached-uncertain suppression. | Safe for source-controlled destroy; vendor internal lifetime is opaque. |
| Opti _ownedGameCommandQueue | ComPtr in XeFG_Dx12 | _gameCommandQueue raw alias, State currentCommandQueue, resource tracking | Reset in ReleaseSwapchainLocked after context/object cleanup. | Opti lifecycle lock and generation-aware resize wait. | Source does not show REF causing a released queue dereference. |
| Opti currentCommandQueue and resize fence/event | Static ComPtr/fence/event in FG_Hooks | Resize and final-release paths | Generation publish/reset; WaitForGPUIdle signals/waits the stored fence when complete. | Generation checks; queue is held by ComPtr while static state is live. | Queue/fence synchronization contract remains the separate SDLX audit boundary. |
| REF XeFGBinding::m_swapchain | Borrowed pointer | REF present/resize/session policy | Null on complete_runtime_detach or binding clear. | old_keepalive holds the swapchain through renderer reset and VtableHook removal. | Safe for the bounded detach interval; callback after that interval lacks source proof. |
| REF XeFGBinding::m_queue | ComPtr-owned | REF physical binding and diagnostics | Replaced/reset with binding generation. | COM ownership and canonical identity. | Safe against release by Opti unless the vendor violates COM ownership; no source evidence of that. |
| REF XeFGBinding::m_device | ComPtr-owned | REF physical binding and renderer/D3D12 hook | Replaced/reset with binding generation. | COM ownership and device identity comparison. | Safe by current source. |
| REF VtableHook private vtable | VtableHook object owns its copied memory | D3D12Hook callback methods | Reset/remove under the REF monitor; restores only if current vptr is its private copy. | vptr identity check and old_keepalive. | No source-confirmed overwrite/UAF while the COM object remains valid. |
| REF present/resize hook target | D3D12Hook/VtableHook and function addresses | Present, Present1, ResizeBuffers, ResizeBuffers1, ResizeTarget | Removed under monitor; late paths re-read current target. | monitor mutex, current-target check, recursion depth. | Unload/reinstall lifetime is insufficient evidence. |
| REF runtime original trampolines | RuntimeRegistry function-hook state | Init/Get/Destroy dispatch | No visible uninstall/removal on module unload. | Registry mutex protects lookup, not module lifetime. | INSUFFICIENT EVIDENCE for a vendor/REF unload race. |
| REFramework::m_d3d12_hook | REFramework unique_ptr member | REFramework destructor and callbacks | Explicitly reset in hook_d3d12 replacement, but not in REFramework::~REFramework body. | Expected monitor lifetime, but member destruction order violates it. | COEX-002 confirmed source defect. |
| REFramework::m_hook_monitor_mutex | REFramework member | D3D12Hook::unhook, callbacks, monitor, pre-retire | Destroyed in reverse member declaration order before m_d3d12_hook. | None during final member teardown. | COEX-002: D3D12Hook destructor locks an ended mutex. |

## 7. Combined lock-order and callback graph

### 7.1 Intended normal order

~~~text
Opti create/recreate/release
  -> XeFG_Dx12::_swapchainLifecycleMutex
     -> REFramework_XeFG_PreRetireSwapchainV1
        -> REFramework::m_hook_monitor_mutex
           -> XeFGPresentationSession evaluation
           -> REFramework::on_reset
              -> REFramework ImGui/resource mutexes
           -> D3D12Hook VtableHook reset + alias clear
     -> return from REF
     -> Opti FG Mutex owner 1/3/6677/6678 as applicable
        -> DestroyFGContext / ReleaseObjects / final release closure

REF Present/Resize
  -> REFramework::m_hook_monitor_mutex
     -> present_common or resize policy
        -> original current target
           -> Opti code detour / FG hook if installed
              -> Intel original
     -> return

REF factory transition
  -> REFramework::m_hook_monitor_mutex
     -> increment in_flight and mark rehook
  -> unlock monitor
  -> downstream factory / Opti / XeFG work
  -> relock monitor
  -> decrement in_flight
  -> one rehook at zero
~~~

The intentional cross-call is Opti lifecycle mutex to REF monitor mutex. The source does not show REF acquiring the Opti lifecycle mutex while holding the REF monitor. Opti enters its FG mutex only after pre-retire returns. This ordering is why a suspicious cross-module callback is not automatically a deadlock.

### 7.2 Callback and reentry assessment

| Path | Locks held at cross-call | Reentry rule | Result |
|---|---|---|---|
| Opti final public-proxy retirement | Opti lifecycle mutex; REF monitor is acquired inside pre-retire; Opti FG mutex is acquired afterward | release closure bypasses the normal FGHooks lifecycle test and is called once | Safe for the single-thread result-1 path; COEX-001 never enters this path in the two-thread interleaving. |
| Opti explicit/recreate retirement | Opti lifecycle mutex, then REF monitor | REF identity mismatch blocks; successful detach clears binding before Opti FG cleanup | Safe by ordering and identity. |
| REF Present into Opti FG hook | REF monitor, then possibly Opti FG/wrapper mutex through the current target | REF monitor is recursive; Opti same-thread guards handle known reentry | No exact reverse lock cycle found. |
| REF Resize into Opti ResizeBuffers1 | REF monitor, then Opti resize/FG ownership and queue wait | Thread-local resize depth forwards nested calls | No source-confirmed deadlock/UAF. |
| REF factory into downstream creation | REF monitor is released before downstream call | in_flight counts nested/concurrent transitions | PR56 closes the old lock inversion on normal HRESULT returns. |
| REF runtime Init/Destroy | Transition state and monitor are prepared, then monitor is released for Intel original | identity/result reconciliation after return | No source-confirmed cross-lock cycle. |
| Hook-monitor recovery | Monitor acquired with try_lock; mutation is performed only after acquisition | active/inconsistent/detached state changes recovery disposition | Cannot mutate a REF callback that holds the same mutex. |

## 8. Generation / lifecycle matrix

| Phase | OptiScaler state | REFramework state | Queue/device identity | Decision and risk |
|---|---|---|---|---|
| G0 no active XeFG | No active public FG alias/context; raw State aliases may be null. | No active binding; monitor may perform generic recovery. | No binding-owned queue/device. | Safe baseline. |
| Initial D3D12 discovery | DXGI factory hook resolves real device/queue and creates wrapper/XeFG context. | D3D12Hook and runtime hooks observe factory/init. | Candidate queue/device are canonicalized through QI and COM identity. | Safe if all creation calls return live objects. |
| G1 publication | State currentFGSwapchain/currentFGSwapchainGeneration and Opti queue ownership are published. | XeFGBinding commit_initial sets borrowed swapchain, owned queue/device, runtime identity, generation 1. | Identity must match renderer aliases and REF snapshot. | Safe by source when aliases_match and consistent_with hold. |
| Normal Present/Present1 | Opti FG hook and transaction process frame state. | REF present_common holds monitor and calls current target. | Binding queue/device remain ComPtr-owned. | Safe by monitor and recursion rules. |
| Normal Resize/Resize1 | Opti waits configured resize sync, takes FG ownership, forwards complete arguments. | REF holds resize lifecycle/hold, resets renderer when policy requires, forwards complete arguments. | All ppPresentQueue entries pass through; first queue is only an Opti raw diagnostic alias. | Safe by source; GPU happens-before remains out of scope. |
| FG deactivate/reactivate | EvaluateState runs inside caller-owned outer transaction; context/FG state changes without shutdown. | Binding is not implicitly replaced merely by feature-state changes. | Existing queue/device remain current unless swapchain lifecycle changes. | Safe by PR37 transaction ownership. |
| Non-preserve recreation | Opti lifecycle lock; pre-retire exact old proxy/context; release old; create/publish new proxy and generation. | REF detach clears G1 binding before candidate G2 is committed. | New candidate uses canonical queue/device and new runtime identity/generation. | Safe by source if old public proxy remains alive until pre-retire. |
| Preserve recreation | Existing same-hwnd branch directly invokes currentFGSwapchain->ResizeBuffers. | No new binding is necessarily retired first. | Old raw alias must still refer to a live public proxy. | COEX-001 stale alias makes this direct virtual call crash-capable. |
| Ordinary final release | First original Release result 1 triggers pre-retire; release closure and generation clear follow. | REF keepalive/detach/reset/clear completes before final closure. | Old queue/fence generation is waited/reset as configured. | Safe for one serialized final-release path. |
| Concurrent final release | Two artificial AddRefs are added, then first original releases return 3/2; final artificial releases return 1/0. | REF is never called. | No generation transition occurs. | COEX-001 conditional stale-pointer escape. |
| Destroy failure | Opti nulls context before call, retains/quarantines according to result and blocks recreation. | REF keeps detached uncertainty and suppresses unsafe monitor recovery. | Existing owned objects are not silently treated as retired. | Safe fail-closed source behavior; vendor result contract opaque. |
| Process shutdown | State::isShuttingDown causes Opti Present forwarding, skips REF pre-retire, and makes DestroySwapchainContext return early. | REFramework destructor stops monitor and deinitializes renderer but does not reset D3D12Hook in body. | REF hook member is later destroyed after its monitor mutex. | COEX-002 confirmed invalid teardown order. |
| Runtime unload boundary | XeFG proxy function pointers and REF dynamic export are not visibly pinned/cleared for unload. | Runtime registry has loaded installation but no unload removal path. | Function/module lifetime is not represented by generation. | INSUFFICIENT EVIDENCE; diagnostic required. |

## 9. Required lifecycle timing diagrams

### 9.1 Startup / initial hook path

~~~text
Tgame
  -> D3D12 device/queue discovery
  -> Opti DXGI factory hook
  -> WrappedIDXGISwapChain4 construction
  -> XeFG_Dx12 CreateSwapchain / InitFromSwapChainDesc
  -> Intel XeFG runtime is loaded and returns a public proxy/context
  -> REF runtime Init hook observes the init transaction
  -> REF factory slot 15 observes GetSwapChainPtr
  -> REF builds candidate:
       swapchain + queue + device + hwnd + runtime slot/context
  -> relation is canonicalized:
       same COM identity / distinct same device / mismatch
  -> XeFGBinding generation 1 is committed
  -> first Present:
       REF VtableHook -> present_common -> current target
       -> Opti detour/FG hook -> Intel original

Safeguard: REF candidate publication is identity/generation-aware; the
binding swapchain is borrowed and queue/device are ComPtr-owned.
Opaque edge: Intel's internal callback lifetime is not visible.
~~~

### 9.2 Normal Present / Present1

~~~text
Tpresent
  -> REF slot 8 or 22 callback
  -> m_hook_monitor_mutex
  -> present_common
       -> consistent_with(binding, physical aliases)
       -> session policy
       -> current original target from hook / late vtable lookup
  -> Opti code detour and FGPresent when the target is detoured
  -> Intel XeFG original
  -> return through Opti -> REF -> caller

If recursion is detected, present_common calls the original target directly.
The REF recursive monitor permits same-thread callback nesting.
Detach cannot reset REF hooks while this callback holds the monitor.
~~~

### 9.3 ResizeBuffers / ResizeBuffers1

~~~text
Tresize
  -> REF ResizeBuffers or ResizeBuffers1
  -> m_hook_monitor_mutex
  -> pre-reset/resize-hold policy
  -> current target -> Opti hkResizeBuffers or hkResizeBuffers1
  -> Opti FG/wrapper ownership
  -> configured WaitForGPUIdle on the known Opti queue/fence
  -> forward original call
       ResizeBuffers1 receives the complete node-mask and ppPresentQueue arrays
  -> Intel proxy / DXGI implementation
  -> REF post-reset/hold completion
  -> return

Safeguard: monitor serialization, thread-local nested resize depth, complete
array forwarding, and generation-aware resize hold.
Boundary: SetResourceReady-before-ExecuteCommandLists has no GPU fence and is
the separate SDLX-006/007 audit, not a new coexistence finding.
~~~

### 9.4 FG deactivate/reactivate without process shutdown

~~~text
Tframe
  -> Streamline setConstants/setJitter/setMotionVectors
  -> ScopedStreamlineFGTransaction acquires owner 2
  -> CheckForFrame / XeFG EvaluateState
  -> if FGchanged:
       deactivate or update FG state
       apply constants/camera/jitter/MV/reset work
       EvaluateState does not unlock the caller transaction
  -> transaction destructor releases owner 2
  -> next frame reactivates through the same outer transaction

REF binding is not replaced merely because feature state changes.
Safeguard: PR37 keeps owner-2 ownership at the outer caller.
~~~

### 9.5 Swapchain/proxy recreation and binding rollover

~~~text
Trecreate
  -> Opti _swapchainLifecycleMutex
  -> old currentFGSwapchain/current context identified
  -> non-preserve path:
       REFramework pre-retire ABI
       -> REF exact runtime identity check
       -> public-proxy keepalive
       -> renderer reset + VtableHook removal
       -> binding generation 1 clear
  -> Opti releases old FG/context/objects
  -> Opti creates new proxy/context and publishes generation 2
  -> REF candidate handoff commits generation 2
  -> Present uses generation 2 only

Preserve branch:
  -> same-hwnd branch directly calls currentFGSwapchain->ResizeBuffers
  -> if the raw alias was already left stale by COEX-001, this is a direct
     virtual call through a freed proxy.
~~~

### 9.6 Final public-proxy release

~~~text
Trelease, serialized result-1 path
  -> Opti FGHooks hkFGRelease
  -> artificial AddRef
  -> first original Release returns 1
  -> WaitForGPUIdle(old generation queue)
  -> Opti ReleaseSwapchainFromFinalProxyRelease
       -> _swapchainLifecycleMutex
       -> REF pre-retire
            -> public_proxy_keepalive
            -> m_hook_monitor_mutex
            -> old binding keepalive
            -> on_reset
            -> VtableHook removal
            -> alias/binding clear
       -> Opti FG mutex
       -> DestroyFGContext
       -> final release closure calls original Release once
       -> queue/object cleanup and generation clear
  -> caller observes the lifecycle as retired

Parallel wrapper path:
  WrappedIDXGISwapChain4::Release final flag
    -> XeFG retirement when wrapper generation/current alias match
    -> reentrant final release consumed
    -> real interface release and wrapper delete

COEX-001 branch:
  Two hkFGRelease callers each AddRef; neither first original Release returns 1.
  The entire retirement block is skipped, but the two final artificial Releases
  still destroy the public proxy. No REF keepalive or alias clear is executed.
~~~

### 9.7 Normal process shutdown

~~~text
Tshutdown
  -> Opti State::isShuttingDown = true
  -> Present/LocalPresent/FG release paths forward or skip cross-retire
  -> XeFG_Dx12::ReleaseSwapchain logs process_shutdown and skips REF pre-retire
  -> DestroySwapchainContext returns early
  -> Opti clears local objects/queue state as far as its shutdown path allows

REFramework:
  -> REFramework::~REFramework
  -> stop/join monitor
  -> deinit_d3d12
  -> destructor body returns without m_d3d12_hook.reset()
  -> member destruction destroys m_hook_monitor_mutex
  -> later destroys m_d3d12_hook
  -> D3D12Hook::~D3D12Hook -> unhook
  -> unhook attempts to lock g_framework->get_hook_monitor_mutex()
  -> ended mutex: COEX-002

Safeguard that works: Opti does not call REF pre-retire after shutdown starts.
Missing safeguard: explicit REF D3D12Hook teardown before monitor destruction.
~~~

### 9.8 Late callback after REF detach

~~~text
Tcallback enters before detach
  -> present_common/resize holds m_hook_monitor_mutex
  -> Tdetach waits for the same monitor
  -> callback calls the current target and returns
  -> Tdetach resets VtableHook and clears binding

Callback enters after instance hook removal
  -> late path reads the current object vtable slot
  -> rejects null/self
  -> forwards to the current target

Safeguard: PR55 prevents a stale saved vtable target from being called solely
because the REF instance hook disappeared.
Boundary: readability is not COM ownership. If Intel calls with a dead object
after the caller-bounded lifetime, source cannot prove the target is live.
~~~

### 9.9 Concurrent/nested CreateSwapChainForHwnd

~~~text
TA enters REF factory hook
  -> monitor
  -> active XeFG or existing transition
  -> in_flight = 1, rehook_required = true
  -> monitor unlock
  -> downstream factory / Opti / XeFG work
       nested TB may enter:
         monitor -> in_flight = 2 -> unlock -> downstream -> relock -> 1
  -> TA relocks -> in_flight = 0
  -> one final rehook if required
  -> return to game

Safeguard: PR56 split monitor scope and monitor-protected in_flight count.
No source-confirmed escape exists on ordinary HRESULT return paths.
~~~

### 9.10 Hook-monitor timeout/recovery during active XeFG

~~~text
Tmonitor every approximately 500 ms
  -> try_lock m_hook_monitor_mutex
  -> if a REF Present/Resize/detach owns it, monitor defers
  -> after no Present for the configured threshold:
       evaluate active binding, generation, runtime transition, aliases
       consistent active XeFG -> retain/suppress inappropriate generic recovery
       detached uncertain -> suppress
       inconsistent/sustained timeout -> quarantine
       no tracked binding -> generic recovery may rehook
  -> any unhook/rehook is performed while monitor is held

Opti Present/Resize/Release uses its own lifecycle/FG/wrapper guards.
The monitor cannot mutate a REF callback holding the monitor.
Opaque boundary: module unload or an external callback not entering the monitor.
~~~

## 10. Primary residual crash findings

### COEX-001 — concurrent Intel XeFG public-proxy release can skip lifecycle retirement

Classification: CONDITIONAL CRASH-CAPABLE DEFECT
Confidence: high source reachability; runtime not exercised on the current tips
Cross-component status: yes; Opti release-hook escape leaves a stale REF binding
Required preconditions:

1. The Intel XeFG public proxy is the current Opti FG proxy, shutdown has not started, and the active output is XeFG.
2. Two legitimate callers concurrently hold the final two public-proxy COM references. There is no hidden keepalive at the instant the two calls begin.
3. Both callers enter Opti FGHooks::hkFGRelease before either call changes the lifecycle state.
4. The later source path causes same-hwnd recreation, preserve-mode ResizeBuffers, or another path that dereferences the stale State::currentFGSwapchain.

Exact source path:

- FG_Hooks.cpp:1338-1354 uses a process-global static skipReleaseChecks flag, checks the raw currentFGSwapchain alias, then adds an artificial reference for each call.
- FG_Hooks.cpp:1359-1362 performs the first original Release and enters lifecycle retirement only if that one call returns exactly 1.
- FG_Hooks.cpp:1374-1385 calls REF pre-retire only inside that result-1 branch.
- FG_Hooks.cpp:1418-1427 performs the final artificial Release for calls that did not enter the result-1 branch.
- XeFG_Dx12.cpp:487-498 directly calls State::currentFGSwapchain->ResizeBuffers in the preserve same-hwnd branch.
- XeFG_Dx12.cpp:500-512 calls PrepareREFForSwapchainRetire(State::currentFGSwapchain, ...) in non-preserve recreation.
- XeFG_Dx12.cpp:1897-1939 protects only a final-release call that actually reached ReleaseSwapchainFromFinalProxyRelease.
- XeFGCompatibility.cpp:270-273 constructs public_proxy_keepalive before identity checks. That construction invokes AddRef through the supplied proxy vtable.
- XeFGBinding.cpp:48-72 stores m_swapchain as a raw borrowed pointer; only queue/device are ComPtr-owned.

Concrete interleaving:

~~~text
Initial Intel public-proxy COM count = 2.
The two counts are legitimate caller references A and B.
State::currentFGSwapchain == This; REF binding is active; shutdown is false.

Thread A: enters hkFGRelease(This), sees skip=false/current, AddRef -> count 3
Thread B: enters hkFGRelease(This), sees skip=false/current, AddRef -> count 4
Thread A: first o_FGRelease(This) -> count 3; result is not 1
Thread B: first o_FGRelease(This) -> count 2; result is not 1
Thread A: final o_FGRelease(This) -> count 1; no lifecycle test is performed
Thread B: final o_FGRelease(This) -> count 0; Intel proxy is destroyed

Neither thread calls WaitForGPUIdle for lifecycle retirement.
Neither thread calls PrepareREFForSwapchainRetire.
State::currentFGSwapchain and the REF borrowed binding still contain This.
~~~

The important distinction is that each hook invocation adds one temporary reference. Consequently, neither first original Release sees the transition to one reference, even though the final two legitimate caller references are being consumed. The final two artificial Releases then consume the object without executing the retirement branch.

Reachable crash consequences:

1. Preserve mode: XeFG_Dx12::CreateSwapchain reaches the same-hwnd preserve branch and invokes ResizeBuffers through State::currentFGSwapchain, whose object has already been destroyed.
2. Non-preserve recreate: the code passes the stale pointer into PrepareREFForSwapchainRetire. REF constructs a temporary ComPtr before validating that the binding is still current; the AddRef dispatch is a use-after-free.
3. A later REF/Opti callback may also observe the stale binding, but the recreate paths above are already sufficient to establish a concrete crash-capable consequence.

Why existing safeguards do not fully block it:

- PR34 protects WrappedIDXGISwapChain4::Release and its reentrant final-release sequence. COEX-001 is the separate Intel XeFG public-proxy Release detour in FG_Hooks.cpp.
- PR35 suppresses Opti-to-REF pre-retire after shutdown. This finding occurs before shutdown.
- PR36/PR37 protect Streamline frame transactions and EvaluateState owner-2 lifetime. Neither serializes public-proxy COM Release.
- PR38 hardens the Capcom DX12 wrapper's COM interface ownership. It does not make the Intel proxy Release hook a single final-release owner.
- REF PR55 covers late current-vtable forwarding after instance-hook removal; it cannot repair a freed object that Opti never retired.
- REF PR56 covers factory monitor inversion; it does not serialize two public-proxy Release callers.
- The pre-retire ABI is never reached in the interleaving, so its keepalive and identity checks cannot help.
- Runtime identity/generation cannot clear a binding if the old proxy's lifecycle decision is skipped before the generation transition.

Additional aggravating condition:

skipReleaseChecks is a process-global non-atomic bool, not a per-object or per-thread release transaction. It is not required for the interleaving above, which remains false throughout, but it is not a concurrency ownership mechanism and should not be treated as one.

Runtime correlation:

- In C:\GoogleDrive\ref-xefg\Release-09\mhw\새 폴더 (2)\OptiScaler.log:357590-357609, the historical wrapper path is serialized: wrapper final release, reentrant release consumption, safe_detached pre-retire, retired queue, and final completion.
- The paired historical REF log at re2_framework_log.txt:4971-4997 shows exact-runtime detach, renderer reset, hook removal, and complete.
- Those logs demonstrate the intended single-thread sequence, not the two-thread sequence above. The REF binary is c6704372 and the Opti binary is e9ca4685, so there is no current-tip runtime proof.
- The E_ABORT capture at C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004\2026_0916_0005\CrashReport.txt:185-187 records C0000005 at 0x000000014CD94BF0 and a Fatal D3D error E_ABORT 0x80004004, but it has no paired OptiScaler.log and cannot be attributed to COEX-001.

### COEX-001 existing fix / coexistence safeguard assessment

| Existing fix / coexistence safeguard | Relevance to finding | Fully blocks it? | Residual reachable path |
|---|---|---|---|
| Opti PR34 | Protects wrapper final-release reentrancy, not Intel public-proxy Release concurrency. | No | Two Intel proxy hkFGRelease calls can both miss first-release result 1. |
| Opti PR35 | Only gates REF pre-retire and context destroy after Opti shutdown. | No | The interleaving occurs while shutdown is false. |
| Opti PR36 | Streamline/Present transaction scope. | No | Public-proxy Release is outside the frame transaction. |
| Opti PR37 | Keeps EvaluateState from unlocking caller owner 2. | No | No EvaluateState call is required to reproduce the interleaving. |
| Opti PR38 | Wrapper D3D12 COM ownership hardening. | No | The unowned/stale object is the Intel XeFG public proxy handled by FGHooks. |
| REF PR55 | Late callback forwarding to a current vtable target. | No | A later recreate can AddRef or call the already-freed stale proxy before late forwarding is relevant. |
| REF PR56 | Split factory monitor scope and in_flight accounting. | No | No factory transition is required before the stale dereference. |
| Opti/REF pre-retire ABI | Intended owner-to-observer detach and public-proxy keepalive. | No | The ABI is reached only when the first original Release returns 1; both calls can miss it. |
| REF runtime identity/binding generation | Separates old and new bindings. | No | No generation rollover occurs before the object reaches zero in the escape path; the old binding remains stale. |

### COEX-002 — REFramework D3D12Hook destruction locks a destroyed monitor mutex

Classification: CONFIRMED CRASH-CAPABLE DEFECT
Confidence: high source proof; runtime execution not required to prove the C++ lifetime violation
Ownership: REFramework-only shutdown/member-lifetime defect; not caused by OptiScaler
Required precondition: a REFramework object with a non-null D3D12Hook reaches normal C++ destruction while the global g_framework still points to the enclosing object, which is the normal unique_ptr destruction arrangement.

Exact source proof:

1. REFramework.hpp declares m_d3d12_hook at line 269.
2. The same class declares m_hook_monitor_mutex later at line 281.
3. C++ destroys members in reverse declaration order. Therefore m_hook_monitor_mutex is destroyed before m_d3d12_hook.
4. REFramework::~REFramework at REFramework.cpp:912-960 stops and joins the monitor thread and calls deinit_d3d12, but does not call m_d3d12_hook.reset().
5. deinit_d3d12 at REFramework.cpp:2975-3002 deinitializes D3D12 renderer state but does not destroy the D3D12Hook member.
6. After the destructor body, member teardown reaches m_d3d12_hook while the later-declared m_hook_monitor_mutex has already ended.
7. D3D12Hook::~D3D12Hook at D3D12Hook.cpp:267-269 calls unhook.
8. D3D12Hook::unhook at D3D12Hook.cpp:1394-1405 waits for g_framework to be non-null and then constructs a scoped lock from g_framework->get_hook_monitor_mutex().
9. The global g_framework unique_ptr is defined in REFramework.cpp:64 and is populated from Main.cpp:99; no source reset/null assignment was found. It therefore still contains the enclosing REFramework pointer while its managed object is being destroyed. The call addresses the ended mutex, not a null global.

The consequence is undefined behavior at the teardown boundary: locking an object whose destructor has already completed can access freed/invalid synchronization state, producing an access violation, deadlock, or other teardown crash. This is a source-confirmed invalid lifetime operation even though the supplied runtime logs do not include a current-tip REFramework destructor trace.

This finding is intentionally kept separate from the Opti/REF coexistence contract. PR35's Opti shutdown behavior prevents Opti from calling REF pre-retire after isShuttingDown; it does not order REFramework's own D3D12Hook member before its monitor mutex. PR55/56 do not affect member destruction.

### COEX-002 existing fix / coexistence safeguard assessment

| Existing fix / coexistence safeguard | Relevance to finding | Fully blocks it? | Residual reachable path |
|---|---|---|---|
| Opti PR34 | Wrapper reentrancy only. | No | REFramework member destruction is independent of the Opti wrapper. |
| Opti PR35 | Suppresses Opti pre-retire during shutdown. | No | The invalid lock occurs in REFramework D3D12Hook::~unhook. |
| Opti PR36 | Frame transaction scope. | No | No frame transaction is needed at teardown. |
| Opti PR37 | EvaluateState owner-2 lifetime. | No | EvaluateState is not involved. |
| Opti PR38 | Capcom DX12 wrapper COM ownership. | No | The failing operation is a REFramework mutex access. |
| REF PR55 | Late callback forwarding. | No | The failing operation occurs after the destructor body. |
| REF PR56 | Factory split monitor scope. | No | Factory transition is not involved. |
| Opti/REF pre-retire ABI | Public-proxy detach contract. | No | Shutdown deliberately skips the ABI; the later member order is still invalid. |
| REF runtime identity/binding generation | XeFG binding identity. | No | Binding generation does not govern REFramework member destruction. |

## 11. Safe by current coexistence invariant / dismissed candidates

The following candidates were inspected and are not promoted to current primary findings because the current source supplies a concrete guard or because the required crash consequence was not proven.

| Candidate | Current mechanism | Classification / reason |
|---|---|---|
| Two hooks on Present/Present1/Resize methods | REF VtableHook wraps the current vtable target; Opti Detours patches the target function; VtableHook removal checks its own vptr. | SAFE BY PROVEN INVARIANT. Two hook layers alone are not a bug. |
| PR37 EvaluateState owner-2 unlock | ScopedStreamlineFGTransaction owns the outer transaction; EvaluateState explicitly does not unlock it. | SAFE BY PROVEN INVARIANT. No same-thread owner-2 unlock is present in the audited path. |
| Same-thread FG Release reentry | FGHooks checks SwapchainReleaseOwnedByCurrentThread; wrapper PR34 consumes reentrant final release. | SAFE BY PROVEN INVARIANT. |
| Normal single-thread final public-proxy release | Artificial AddRef, first result-1 detection, REF keepalive/detach, one final release closure, alias/generation clear. | SAFE BY PROVEN INVARIANT. COEX-001 is the concurrent exception. |
| Explicit non-preserve recreation | Opti calls exact pre-retire before FG mutex cleanup; REF checks runtime identity and clears old binding before new commit. | SAFE BY PROVEN INVARIANT while the old proxy is live. |
| Stale old final release after a new binding | ReleaseSwapchainFromFinalProxyRelease checks currentFGSwapchain != finalProxy and performs release-only; REF runtime identity requires exact context/slot. | SAFE BY PROVEN INVARIANT for a reached final-release path. |
| REF restoring a vtable over another active hook | VtableHook::remove restores only when current vptr equals its private replacement vtable. | SAFE BY PROVEN INVARIANT. |
| PR55 late callback target | Late forwarding reads the current vtable slot and rejects null/self. | SAFE BY PROVEN INVARIANT for a live object; object-after-final-release lifetime remains an evidence boundary. |
| REF Present/Resize versus hook-monitor recovery | Callback and monitor mutation use the same recursive monitor mutex; monitor uses try_lock and defers when busy. | SAFE BY PROVEN INVARIANT. |
| PR56 nested/concurrent factory monitor inversion | monitor-protected in_flight, unlock around downstream call, rehook once at zero. | SAFE BY PROVEN INVARIANT on normal HRESULT return paths. |
| REF runtime Init/Destroy lock inversion | Transition scope prepares state under the monitor but releases it around Intel original calls, then reconciles identity/result. | SAFE BY PROVEN INVARIANT for source-controlled calls. |
| New REF binding committed before old binding detached | Candidate handoff and lifecycle transition use runtime identity, pending discard, and generation; Opti non-preserve retirement precedes new publication. | SAFE BY PROVEN INVARIANT. |
| ResizeBuffers1 array truncation | Both Opti and REF forward the full node-mask and queue arrays; only Opti's diagnostic alias uses the first queue. | SAFE BY PROVEN INVARIANT for the hook/lifetime question. |
| Different queue wrappers on the same device | REF canonicalizes COM identity/device relation; queue/device are ComPtr-owned; distinct same-device is classified, not blindly treated as a crash. | SAFE BY PROVEN INVARIANT for source lifetime. |
| Queue cache reset during Opti retirement | _ownedGameCommandQueue and raw alias are cleared after context/object cleanup under lifecycle ownership. | SAFE BY PROVEN INVARIANT. |
| Failed Intel destroy | Opti nulls or retains/quarantines based on result; REF keeps detached uncertainty and suppresses unsafe recovery. | SAFE BY PROVEN INVARIANT for fail-closed behavior; vendor internal callback behavior is opaque. |
| SetCommandQueue ignoring FG resource type | It is a separate queue/resource semantic concern; no active REF binding path calls SetResourceCmdList/uses the ignored type to dereference an object. | NOT REACHABLE as a current cross-component crash path; SDLX/resource tracking remains separate. |
| SetResourceReady before ExecuteCommandLists | Source confirms no fence/happens-before proof. | OUT OF SCOPE for this report unless REF materially changes it; it remains SDLX-006/007 audit material, not a new coexistence finding. |
| Opti shutdown calling REF pre-retire | State::isShuttingDown causes ReleaseSwapchain to skip pre-retire and DestroySwapchainContext to return early. | SAFE BY PROVEN INVARIANT for the Opti-to-REF call boundary. |
| REF factory/runtime hook changes while Opti Present runs | REF mutation is monitor-serialized; Opti code detours remain a separate code-hook layer. | SAFE BY PROVEN INVARIANT for source-controlled active calls. |

## 12. Insufficient evidence

These are deliberately not upgraded to findings because the source audit closes the source-controlled portion but the remaining vendor/module boundary is not observable in the supplied evidence.

| Boundary candidate | What source proves | Exact missing evidence | Minimum no-behavior-change diagnostic |
|---|---|---|---|
| Intel invokes Present/Resize after the public proxy's final COM release | REF late forwarding validates readability and current vtable target, but does not own an already-dead object. | A current-tip trace proving whether Intel retains a caller/reference during every callback and whether any callback begins after final Release. | Log proxy pointer, thread, entry/exit, AddRef/Release result, active generation, and callback depth in Opti/REF; record object destruction/final Release order. |
| REF borrowed binding swapchain after detach | old_keepalive covers renderer reset and VtableHook removal. | A vendor callback or asynchronous worker using the borrowed pointer after complete_runtime_detach returns. | Add generation/identity-tagged callback entry and exit logging; assert no callback after detach-complete for the retired object. |
| REF runtime module unload | RuntimeRegistry resolves originals under a mutex. | LdrUnload/FreeLibrary event paired with an in-flight Init/Get/Destroy call. | Log module load/unload addresses and every dispatch entry/exit; retain the existing behavior while proving whether unload can occur. |
| Opti dynamic REF pre-retire export unload | Toolhelp/GetProcAddress finds a function pointer and the call has no module pin. | Whether the REF DLL can unload between lookup and call in the deployed process. | Log module base, export address, lookup-to-call interval, and unload notifications; do not add sleeps or change ownership in the diagnostic. |
| Opti XeFG proxy DLL unload | Static proxy function pointers are resolved and retained. | Whether libxess_fg.dll can unload/reload while hooks or function pointers remain in use. | Pair loader notifications with each XeFG proxy call and module address; validate process-only lifetime or capture the violating sequence. |
| Opti Detours target after module replacement | Static original function pointers are intentionally retained. | A current runtime sequence that unhooks/reinstalls the vendor implementation or unloads the owning module. | Log install/detach/reinstall target addresses and module ownership for each slot; no behavior change. |
| Exception unwinding through PR56 downstream factory call | Normal COM factory APIs return HRESULT and the source decrements in_flight on normal return. | A throwing downstream implementation or asynchronous exception that bypasses the decrement. | Add an existing-style scope guard diagnostic counter/assert around transition exit without changing the call behavior; first establish whether throws are possible. |
| Vendor Destroy callback/reentrancy contract | REF detaches before original Destroy and reconciles exact identity/result. | Whether the Intel original calls back into a retired proxy/context during or after Destroy. | Log Destroy entry/exit and callback identity/generation; retain monitor and keepalive rules. |
| Current-tip reproduction of COEX-001 | The two-thread interleaving is source-reachable. | A two-thread current-tip trace with refcount results and later stale dereference. | Instrument hkFGRelease with thread/refcount/generation/proxy fields and force only a diagnostic concurrent-release test; do not add a timing workaround. |

No opaque boundary is used to dismiss either primary finding. COEX-001 is already source-conditional with a complete interleaving; COEX-002 is already source-confirmed as a lifetime violation.

## 13. Remediation ordering

No implementation is included in this audit. The order below is the smallest causal order for a future implementation review.

1. **COEX-002 first — order REFramework D3D12Hook teardown before monitor destruction.** The local contract must ensure D3D12Hook::unhook runs while m_hook_monitor_mutex is alive. The two minimal design options are an explicit m_d3d12_hook.reset() in the REFramework destructor body after the monitor thread is stopped, or a member-order change that makes the monitor outlive the hook. The fix must preserve the existing monitor/unhook behavior and then validate destructor ordering.
2. **COEX-001 second — make the public-proxy final-release decision one serialized lifecycle decision.** The implementation must ensure that every concurrent last-reference sequence either enters exactly one retirement path before the underlying proxy reaches zero or is recognized as a release-only path with no stale State/REF relationship left behind. The fix should remain local to the Intel public-proxy release/lifecycle contract; it must not replace the existing coexistence model with a global lock or a broad ownership rewrite.
3. **Runtime-boundary diagnostic third — prove module and callback lifetime.** Add only the minimum identity/entry/exit/unload instrumentation described in Section 12. Do not use Sleep, yield, logging timing, or a queue/fence workaround as a fix.

Required post-fix validation for future implementation work:

- two-thread public-proxy Release with the final two legitimate references;
- serialized single-thread final release and reentrant release;
- non-preserve and preserve recreation;
- Present/Present1 and ResizeBuffers/ResizeBuffers1 during detach;
- failed Destroy and detached-uncertain recovery;
- nested/concurrent CreateSwapChainForHwnd;
- normal Opti shutdown and actual REFramework object destruction;
- module/callback lifetime diagnostics with current source tips.

## 14. Completion gate — all mandatory questions

| # | Question | Sourced answer and classification |
|---:|---|---|
| 1 | Can Opti final wrapper/proxy release call REF pre-retire while REF is inside Present/Resize for the same proxy? | Yes, the calls can overlap at entry. REF Present/Resize holds m_hook_monitor_mutex, so pre-retire waits for the monitor before mutating REF state. Opti can already hold its lifecycle mutex when it calls the ABI; it intentionally reaches REF before its FG mutex. No source-confirmed deadlock follows from that order. |
| 2 | If yes, what prevents or permits UAF/deadlock? | Monitor serialization prevents REF hook mutation during a REF callback; recursive ownership handles same-thread REF reentry; Opti enters its FG mutex after pre-retire. UAF is still permitted by COEX-001 because concurrent public-proxy Release can skip pre-retire entirely and leave raw aliases/binding stale. |
| 3 | Can REF pre-retire call DXGI/XeFG/Opti code while Opti lifecycle, FG, or wrapper mutex is held? | Yes for Opti lifecycle mutex: ReleaseSwapchainFromFinalProxyRelease and recreate call pre-retire while _swapchainLifecycleMutex is held. The source explicitly calls pre-retire before the Opti FG mutex. No exact path shows the ABI called while the wrapper local mutex is held; that callback relation is not independently proven. |
| 4 | Can either project retain and later dereference the public proxy after the other project causes its final release? | Opti State aliases are raw and can remain stale in COEX-001. REF binding m_swapchain is borrowed and does not own the public proxy; its temporary public_proxy_keepalive ends with the pre-retire call. A later REF dereference after an out-of-contract vendor callback is not source-proven and is INSUFFICIENT EVIDENCE. |
| 5 | Does REF's temporary ComPtr keepalive cover every callback interval that matters? | No. It covers the pre-retire function's identity/detach interval, and D3D12Hook old_keepalive covers renderer reset and VtableHook removal. It does not cover an asynchronous callback after detach or an object already destroyed before pre-retire. |
| 6 | Can a callback begin before detach and resume after the Opti object has been destroyed? | A REF callback that owns m_hook_monitor_mutex blocks detach until it returns. A callback entering after detach uses PR55 late forwarding. Whether Intel can retain and resume a callback without a valid COM reference after object destruction is not visible: INSUFFICIENT EVIDENCE. |
| 7 | Does PR55 always forward such a callback to a valid current vtable target, or can the object itself already be dead? | For a readable live object, PR55 reads the current slot, rejects null/self, and forwards the current target. Readability does not establish COM lifetime; a dead/reused object remains an opaque vendor boundary. INSUFFICIENT EVIDENCE for that boundary. |
| 8 | Can Opti or REF save an original/trampoline that becomes invalid when the other side unhooks/reinstalls? | While modules remain loaded, REF saves a vtable copy target and Opti's Detours trampoline remains the code target; REF removal does not detach Opti's code patch. RuntimeRegistry lookup is mutex-protected. Module unload/replacement has no pin/removal proof: INSUFFICIENT EVIDENCE. |
| 9 | Can either side restore a vtable slot over the other side's still-active hook? | No source-controlled path proves this. VtableHook::remove restores only if the object's current vptr equals its own private copy; Opti uses code detours rather than a competing vtable restore. SAFE BY PROVEN INVARIANT. |
| 10 | Can nested/concurrent CreateSwapChainForHwnd transitions escape PR56 in_flight accounting? | Not on normal HRESULT return paths. The monitor protects in_flight, downstream work runs unlocked, and rehook occurs only at zero. Exceptional unwinding is not instrumented, but no throwing vendor path is source-proven: safe for the audited normal path, exceptional edge INSUFFICIENT EVIDENCE. |
| 11 | Can a failed downstream factory call leave REF unhooked while Opti holds a live proxy that will later call back? | The old REF binding is cleared before downstream work; failure returns through the final decrement/rehook path. During the unlocked downstream interval REF may be temporarily absent by design, but no stale REF binding is left source-proven and Opti's independent code hook remains. SAFE BY PROVEN INVARIANT on normal return; vendor reentry in that interval is not independently observed. |
| 12 | Can a new Opti XeFG generation be published before REF fully retires the old generation? | In the non-preserve path, Opti calls pre-retire and releases the old lifecycle before publishing the new State generation. REF candidate handoff and generation commit are separate and identity-checked. No source path proves a new generation publication before the required old retirement. SAFE BY PROVEN INVARIANT. |
| 13 | Can REF bind a new generation while an old Opti final release later detaches it by hwnd/context ambiguity? | A reached old final-release path first checks currentFGSwapchain and then passes the old context/hwnd. REF pre-retire requires exact runtime slot/context and the public proxy path does not enable a loose same-hwnd match. The old release becomes release-only or is blocked. SAFE BY PROVEN INVARIANT. COEX-001 is earlier: it skips the final-release path and leaves the old binding stale. |
| 14 | Can ordinary ResizeBuffers1 create a stale old-generation proxy/hook relationship capable of a later call? | Opti and REF both forward the complete queue array, track resize hold/generation, and serialize their callbacks. The source does not show ResizeBuffers1 replacing the binding without the generation transition. SAFE BY PROVEN INVARIANT for the lifecycle question; GPU synchronization remains the separate SDLX boundary. |
| 15 | Can hook-monitor rehook/quarantine mutate the hook chain while Opti is in Present/Resize/Release? | REF callback and monitor mutation share m_hook_monitor_mutex; monitor try_lock defers while a callback owns it. Opti release can wait on the monitor through pre-retire, while Opti code hooks are a separate layer. SAFE BY PROVEN INVARIANT for source-controlled paths. |
| 16 | Do runtime-transition and factory-transition suppression cover every such monitor path? | They cover the enumerated REF runtime Init/Destroy, candidate handoff, factory transition, monitor recovery, Present, Resize, and pre-retire paths. They do not establish a module-unload or vendor asynchronous callback contract. Source-controlled coverage is PASS; unload boundary is INSUFFICIENT EVIDENCE. |
| 17 | Can shutdown call REFramework_XeFG_PreRetireSwapchainV1 after REF has begun unload? | Opti's shutdown gate makes ReleaseSwapchain skip pre-retire and makes DestroySwapchainContext return early. The source has no normal Opti shutdown call after that gate. Dynamic module unload ordering is not proven, so the exact “REF began unload” boundary is INSUFFICIENT EVIDENCE; the Opti call suppression itself is SAFE BY PROVEN INVARIANT. |
| 18 | Can REF runtime hooks call an Intel XeFG original after the vendor module has begun unload? | RuntimeRegistry stores original function hooks and has no visible unload removal/pin path; the loader callback handles loaded notifications, not unload. Whether unload can overlap an in-flight call is not in source/log evidence: INSUFFICIENT EVIDENCE. |
| 19 | Can Opti's dynamic REF export lookup race REF module unload in a crash-capable way? | Yes in principle: Toolhelp/GetProcAddress returns a raw function address and the call has no module pin. No current source or log proves that REF actually unloads in this process. Classification: INSUFFICIENT EVIDENCE, not a current primary finding. |
| 20 | Are queue/device differences purely observational, or can either project dereference a released queue/device due to the other's lifecycle transition? | REF queue/device and Opti owned game queue are ComPtr-held; State and feature aliases are raw observations; COM/device identity is canonicalized. No source path shows one project releasing the other's owned queue/device and then dereferencing it. SAFE BY PROVEN INVARIANT for the source paths; vendor callback timing remains an opaque boundary. |
| 21 | Are PR34/35 safeguards symmetric across explicit release, final proxy release, failed destroy, resize/recreation, and shutdown? | They are effective for their intended wrapper reentrancy and shutdown gates, but not symmetric across the separate Intel public-proxy concurrent Release path. COEX-001 is the escape. Failed-destroy quarantine and resize/recreate ordering are additionally handled by XeFG lifecycle code. Classification: NO, with the exact residual limited to COEX-001. |
| 22 | Are REF PR55/56 safeguards symmetric across Present, Present1, ResizeBuffers, ResizeBuffers1, ResizeTarget, factory recreation, runtime destroy, and binding replacement? | Present/Present1/resize methods use hook-monitor/current-target/late forwarding; factory recreation uses PR56 in_flight; runtime destroy and binding replacement use transition identity/generation. The mechanisms are path-specific rather than one universal guard, but the source-controlled paths are covered. The only unclosed edge is object/module lifetime after the source-controlled interval: SAFE BY PROVEN INVARIANT plus INSUFFICIENT EVIDENCE at that boundary. |
| 23 | Is any cross-component callback executed while both sides believe they own final teardown authority for the same proxy/context? | Normal retirement has one authority: Opti owns final public-proxy release and REF is an observer/detacher; REF does not release the public proxy. COEX-001 is the exception: two Opti Release callers each consume artificial references, neither assumes retirement, and both leave the stale cross-component relationship behind. CONDITIONAL CRASH-CAPABLE DEFECT. |
| 24 | Is there any current path where a borrowed REF swapchain outlives the caller-bounded hook lifetime documented in XeFGBinding? | No source-controlled detach path leaves the binding active after old_keepalive and VtableHook removal; generation clear completes the transition. A vendor callback retaining the raw pointer after the interval is not proven. SAFE BY PROVEN INVARIANT for source-controlled paths; INSUFFICIENT EVIDENCE for the opaque callback contract. |
| 25 | Can a SafeNotTracked or SafeDetached result be returned while a REF callback still contains an unprotected raw pointer that will later be used? | SafeDetached is returned only after monitor-serialized hook removal, renderer reset, alias clear, and binding clear; old_keepalive spans the removal interval. SafeNotTracked means there is no active tracked binding. A callback that continues after that interval without participating in the monitor/COM contract is not visible. INSUFFICIENT EVIDENCE only at the vendor callback boundary, not a source-confirmed escape. |
| 26 | For every candidate, what mechanism was intended to make it safe, and is the candidate a true escape or a restatement? | The mechanisms are: owner-2 outer transaction (restatement ruled safe); lifecycle-before-FG ordering (safe); REF monitor and keepalive (safe for bounded interval); identity/generation (safe for old/new separation); PR55/56 (safe for late/factory paths); ComPtr queue/device ownership (safe for object lifetime); shutdown gate (safe for Opti-to-REF suppression). COEX-001 is a true escape because the Release hook skips the mechanism that invokes pre-retire. COEX-002 is a true REFramework owner-lifetime escape because member destruction occurs after the monitor's lifetime. Module unload and post-destruction vendor callbacks remain explicit insufficient-evidence boundaries. |

## Audit closure

The source audit is complete against the instruction's required paths and completion questions. The report does not implement either remediation, alter existing source, build binaries, or open a PR. The only repository change is this audit report.

The actionable conclusions are limited to:

1. Fix the REFramework D3D12Hook/member destruction order (COEX-002).
2. Make Intel public-proxy final-release retirement a single serialized lifecycle decision under concurrent Release (COEX-001).
3. Add the minimum loader/callback diagnostics if current runtime evidence is required to close the vendor/module boundary.
