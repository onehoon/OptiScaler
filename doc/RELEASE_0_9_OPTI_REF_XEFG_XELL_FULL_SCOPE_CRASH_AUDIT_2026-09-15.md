# OptiScaler ↔ REFramework: XeFG / XeLL 범위 전체 호환성 크래시 감사

Date: 2026-09-15 (KST)

## 1. 결론과 감사 범위

**지정된 두 포크의 XeFG / XeLL 상호작용을 소스 진입점에서 다시 추적했다. 기존 F1/F2를 확인하는 데 한정하지 않았다.**

기존 결과와 비교하면 **A2 재생성 교착 경로와 A3 ResizeBuffers1 큐 변경 위험을 추가로 식별했다.** 앞선 추가 감사의 A1과 원본의 F1/F2도 유지한다. 모든 항목은 정적 분석 결과이며, 이 감사에서 게임 크래시나 GPU 장애를 실제 재현하지 않았다.

| ID | 분류 | 문제 | 실제 발생에 필요한 조건 |
|---|---|---|---|
| **A2 — 이번 감사 추가** | **CRASH-RELEVANT** | REF의 바깥쪽 factory 잠금과 Opti Present / 재생성 잠금의 역순 획득으로 교착 | REF factory handler가 Opti handler 바깥에 있고, 활성 Present와 같은 HWND 재생성이 겹침 |
| **A3 — 이번 감사 추가** | **COMPAT-RISK** | ResizeBuffers1에서 presentation queue가 바뀌어도 REF는 이전 큐로 backbuffer 렌더링 | 같은 내부 swapchain에 다른 큐가 성공적으로 적용되고, REF가 render 모드로 재개됨 |
| **A1 — 앞선 추가 감사 유지** | **CRASH-RELEVANT** | pre-retire가 제거한 hook 객체를 이미 진입한 REF callback이 조회 | callback 진입과 monitor 잠금 획득 사이에 detach가 완료됨 |
| **F1 — 원본 재확인** | **CRASH-RELEVANT** | 실행 중인 REF를 Opti의 export 탐색이 발견하지 못해 handoff 생략 | MHRise 경로에서 REF가 실제 모듈 열거에 보이지 않는 조건 |
| **F2 — 원본 재확인** | **COMPAT-RISK** | REF가 받은 초기화 후보를 Opti 초기화 취소 시 제거하지 못함 | REF Destroy hook 실패와 후보 수락 후 Opti 초기화 취소의 조합 |

여기서 CRASH-RELEVANT는 구체적인 AV / 잘못된 객체 접근 / 교착 경로가 있다는 뜻이다. 특정 게임에서 발생 빈도나 재현이 확인되었다는 뜻이 아니다. COMPAT-RISK는 추가 런타임 조건이 필요한 호환성 위험이다. 이 분류는 [사용자가 지정한 원본 감사 원칙][original]을 따른다.

### 고정한 원격 소스

| 포크 | 브랜치 | 감사 커밋 |
|---|---|---|
| onehoon/OptiScaler | reframework-0.9 | `46cb34eda30830f0eaea7846abacc03055377c42` |
| onehoon/REFramework | master | `4bf45b370e602f7f6a3ca54f308daa4e353aab8a` |

감사 말미에 원격 HEAD가 위 값과 같은지 다시 확인했다. 원본 보고서의 Opti `e962942b325033578b249ddc27172322228a48d1`에서 현재 감사 커밋까지의 차이는 원본 보고서 추가뿐이다. 따라서 아래 차이는 코드 변경에 따른 회귀가 아니라 같은 코드에 대한 추가 발견이다. 로컬의 오래된 master 작업 파일을 감사 기준으로 사용하지 않았다.

### 포함 / 제외 기준

- 포함: XeFG 생성, 내부 presentation 관찰, public proxy, REF 바인딩, Present / Present1, Resize / fullscreen, 재생성, 해제, 실패 복구, XeLL 연결·공개·해제. 공유 함수는 이 경로에 실제 영향을 주는 부분만 포함한다.
- 포함: NGX / FFX / Streamline 입력 어댑터가 **XeFG 출력**에 전달하는 자원·프레임 상태 및 lifecycle 호출. 다른 FG 출력 구현의 감사로 확장하지 않는다.
- 제외: 다른 Opti 기능의 독립적인 결함, REF의 일반 MOD·스크립트·VR 기능 구현, 스타일·리팩터링, REF와 연결되지 않는 SDK 사용 논쟁, 실제 gameplay 경로가 아닌 프로세스 종료 정리.
- REF renderer는 XeFG 내부 backbuffer와 큐를 받는 경계만 확인했다. `CommandContext.cpp`가 `mods/vr/` 아래 있더라도, 일반 REF D3D12 renderer가 직접 사용하는 `execute/reset/wait` 경계는 포함된다. VR 기능 자체는 포함하지 않는다.

이 문서는 앞선 `RELEASE_0_9_REF_CAPCOM_INDEPENDENT_COMPATIBILITY_CRASH_AUDIT_2026-09-15.md`보다 감사 범위를 확장한 결과다. 앞선 보고서를 삭제하거나 완료 범위를 소급해서 바꾸지 않는다.

## 2. 원본 체크리스트와 독립적으로 만든 감사 지도

1. 고정 소스의 `XeFG` / `XeLL` 선언·호출·상태 사용을 전수 검색했다. Opti 24개 파일, REF 20개 파일에 해당 문자열이 있다. 이 수치는 검색 대상 목록이며 파일 전체를 무차별 감사했다는 뜻이 아니다.
2. 키워드가 없는 간접 경로를 추가했다: `Dxgi_Hooks`, NGX release, `Upscaler_Inputs_Dx12`, `IFGFeature*`, resource-tracking ExecuteCommandLists, wrapper final Release, REF renderer / CommandContext, MHRise startup 경계.
3. 각 진입점에서 바뀌는 context / proxy / internal swapchain / queue / generation / lock을 다음 소비 지점까지 연결했다.
4. 생성·프레임·resize·실패·해제·재후킹을 아래 범위표로 닫고, 원본과 앞선 보고서의 발견 사항을 대조했다.

검색 목록: [full_scope_inventory.json](E:/LocalTemp/opti-ref-compat-audit-20260915-independent/full_scope_inventory.json). 키워드 개수는 감사 완료의 근거로 대신 사용하지 않았다.

### 실제 교차 경계

```text
game factory / fake swapchain context
  -> Opti DXGI entry -> FGHooks -> XeFG_Dx12
  -> XeFG + XeLL context creation / latency connection
  -> vendor InitFromSwapChainDesc
       -> REF runtime dispatch / temporary factory observation
       -> internal swapchain + presentation queue candidate
       -> REF pending handoff or live instance binding
  -> Opti GetSwapChainPtr / public proxy generation publication

game Present -> Opti FG mutex / resource tagging -> vendor XeFG
  -> REF internal Present or Present1 / monitor mutex
  -> REF renderer on selected presentation queue -> internal DXGI Present

Resize / recreate / release
  -> shared factory or proxy entry
  -> REF renderer reset / binding retirement
  -> Opti proxy + XeFG retirement -> expected-old XeLL unpublish -> XeLL destroy
```

**중요 구분:** 생성 함수 바깥에 REF factory handler가 남아 있으면, XeFG 전용 helper 내부의 잠금 해제만으로 전체 호출 스택의 REF 잠금이 해제되지는 않는다. A2는 이 바깥쪽 호출 관계에서 발견했다.

## 3. A2: 바깥쪽 REF factory 잠금이 pre-retire의 잠금 순서를 무효화할 수 있다

**분류: CRASH-RELEVANT — 게임 멈춤 / 교착. 이번 감사에서 추가.**

### 3.1 두 구현을 연결한 경로

REF는 일반 D3D12 discovery에서 factory의 `CreateSwapChainForHwnd[15]`에 자신의 handler를 설치한다. XeFG instance 바인딩은 Present hook을 교체하지만 이 factory handler는 유지한다. Opti의 기본 `DxgiFactoryWrapping=false` 경로는 같은 factory API의 함수에 자신의 handler를 설치한다. 따라서 **Opti handler가 설치된 함수 주소를 REF factory handler의 original로 호출하는 순서**가 소스상 가능하다. [REF factory 설치][r-factory-install], [Opti factory 선택][o-factory-select], [Opti factory 설치][o-factory-hook], [REF XeFG bind][r-bind].

REF `create_swapchain()`은 다음 구간 전체에서 `hook_monitor_mutex`를 잡는다.

1. `on_reset()` 및 기존 `D3D12Hook::unhook()`.
2. `create_swap_chain_fn(...)` 호출.
3. 결과 처리와 `hook_d3d12()` 재설치.

2번이 Opti handler로 연결되면, Opti의 `FGHooks::CreateSwapChainForHwnd()` → `XeFG_Dx12::CreateSwapchain1()` 역시 **바깥쪽 REF 잠금을 잡은 상태**에서 실행된다. [REF factory 호출 전체][r-factory], [Opti factory에서 FG 진입][o-factory-call], [Opti 생성][o-create1].

한편 활성 Opti Present는 FG mutex를 잡고 vendor Present를 호출하며, vendor가 REF의 내부 Present / Present1에 도달하면 REF monitor mutex가 필요하다. FG mutex는 vendor Present 반환 뒤에 해제된다. [Opti Present 잠금][o-present], [REF Present 진입][r-present], [REF Present1 진입][r-present1].

### 3.2 구체적인 교착 순서

`F` = Opti FG mutex, `M` = REF hook-monitor mutex.

| 순서 | Present 스레드 P | 재생성 스레드 R |
|---|---|---|
| 1 | 활성 XeFG public Present에서 F 획득 | |
| 2 | 내부 REF callback에 진입; M 획득 직전에 중단 | |
| 3 | | 바깥쪽 REF factory handler에서 M 획득 |
| 4 | | 기존 REF instance unhook; 이미 진입한 callback은 남아 있음 |
| 5 | | original factory를 통해 Opti의 같은 HWND 재생성에 진입 |
| 6 | M을 기다리며 F 유지 | F를 기다리며 M 유지 |

같은 HWND의 두 분기를 모두 확인했다.

- **preserve=false:** Opti pre-retire 후 `ReleaseSwapchainLocked()`가 `Mutex.lock(1)`을 호출한다. 앞선 REF `unhook()`은 handoff target을 null로 만들므로 export가 정상 발견되어도 `SafeNotTracked`를 받을 수 있다. helper의 재귀 잠금 한 단계가 끝나도 바깥 factory의 M은 유지된다. [REF unhook][r-unhook], [REF handoff][r-handoff], [Opti retire 잠금][o-release-locked].
- **preserve=true:** 기존 public proxy의 `ResizeBuffers()`를 호출하고 Opti `hkResizeBuffers()`가 F를 획득하려 한다. 이 분기에는 pre-retire 자체가 없다. [Opti preserve 분기][o-create1], [Opti ResizeBuffers 잠금][o-resize].

### 3.3 기존 보호가 해결하지 못하는 이유

- Opti lifecycle mutex는 `try_to_lock`이지만, 위 순서에서 Present는 그 mutex를 잡지 않으므로 재생성 스레드가 성공적으로 진입할 수 있다. 뒤의 F 획득은 blocking이다.
- pre-retire의 성공은 callback 배출이나 바깥 호출자의 M 해제를 의미하지 않는다.
- `unhook()`은 새 callback 진입을 막을 수 있어도 이미 함수에 진입해 M을 기다리는 callback을 취소하지 않는다.
- GPU idle 확인은 CPU callback의 M 대기를 해결하지 않는다. 이 순서는 GPU wait가 정상 완료되는 경우에도 성립한다.
- 이 항목은 REF가 export 탐색에 정상 노출되는 경우에도 성립한다. F1과 별개의 원인이다.

### 3.4 증거의 한계와 확인 조건

12개 소스 구간을 assertion으로 연결한 추상 모델은 46개 상태에서 교착 상태 1개를 찾았다. 바깥 M을 Opti 진입 전에 해제한다고 가정한 대조 모델에서는 교착 상태가 없었다. 대조 모델은 민감도 확인이며 수정안 검증이 아니다. [모델][models], [모델 결과][model-result].

실제 게임에서 판정하려면 해당 factory의 original 호출이 Opti handler에 이어지는지, P의 F 소유와 REF callback 대기, R의 M 소유와 F 대기가 동시에 존재하는지 확인해야 한다. factory가 안쪽에만 존재하는 실행 순서, 이미 callback이 배출된 경우, 비활성 Present는 이 교착의 증거가 아니다. 게임별 재현과 정확한 hook 설치 순서는 미확인이다.

## 4. A3: ResizeBuffers1 이후 REF presentation queue가 갱신되지 않는다

**분류: COMPAT-RISK — 잘못된 큐에서의 backbuffer 접근 / GPU 동기화 위험. 이번 감사에서 추가.**

### 4.1 발견한 상태 불일치

REF는 XeFG Init 관찰 시 내부 swapchain의 presentation queue를 선택하고 COM 참조를 소유한다. init queue와 다른 동일 device의 DIRECT queue일 때 render 모드로 진입한다. [REF 후보 검증][r-discovery-candidate], [REF binding 소유권][r-binding].

Opti의 public `ResizeBuffers1()`은 `ppPresentQueue`를 vendor에 전달한다. 내부 Opti wrapper도 이 큐 배열을 실제 DXGI `ResizeBuffers1()`에 전달하며, XeFG 경로에서 자신의 presentation용 queue alias를 첫 큐로 바꾼다. [Opti public resize 전달][o-resize1-forward], [Opti wrapper 큐 수신][o-wrapper-queue], [Opti wrapper 실제 resize][o-wrapper-resize-forward].

그런데 REF `resize_buffers1()`은 다음 동작만 한다.

- 기존 tracked instance 여부 및 중첩 호출 확인.
- renderer reset.
- 큐 배열을 original로 전달.
- 결과 및 resize hold 처리.

**성공 후 `present_queues`와 기존 binding queue의 COM identity를 비교하거나 바인딩을 갱신하는 코드가 없다.** 다음 Present에서도 XeFG source는 native queue 재탐색을 건너뛰므로 이전 queue가 유지된다. 일반적인 `SameSwapchainUpdate` 구현은 존재하지만, ResizeBuffers1에서는 호출하지 않는다. [REF ResizeBuffers1 전체][r-resize1], [REF Present queue 선택][r-present-common], [REF same-object update][r-rebind].

### 4.2 크래시와 연결되는 경로

조건을 충족하면 같은 내부 swapchain에 대해 다음 상태가 된다.

```text
초기: DXGI presentation queue = Q0, REF renderer queue = Q0
성공한 ResizeBuffers1: DXGI presentation queue = Q1, REF renderer queue = Q0
다음 frame: REF가 Q0에서 새 backbuffer의 PRESENT -> RENDER_TARGET -> PRESENT 실행
           DXGI / XeFG presentation은 Q1에서 진행
```

`ResizeBuffers1`의 큐 배열은 실제 Present에 사용할 큐를 정하며, buffer별로 큐를 순환시킬 수도 있다. 서로 다른 큐의 자원 접근에는 명시적인 동기화가 필요하다. [Microsoft ResizeBuffers1 계약][ms-resize1], [Microsoft D3D12 동기화 책임][ms-sync].

REF renderer는 binding queue에서 그 backbuffer의 barrier와 draw를 제출한다. `CommandContext::execute()`의 fence는 **자신이 제출한 큐**에 Signal하며, 새 presentation queue Q1과의 Wait / Signal 연결을 만들지 않는다. 이전 frame의 자체 fence를 기다리는 것도 Q1과의 새 동기화를 제공하지 않는다. 따라서 큐 변경이 실제 적용되면 잘못된 순서의 GPU 접근이 가능하고, GPU 검증 오류·device failure로 이어질 위험이 있다. **이는 코드와 API 계약에서 도출한 조건부 추론이며 실제 device removal 증거가 아니다.** [REF backbuffer 제출][r-render-submit], [REF queue 실행][r-command-execute].

### 4.3 이 항목의 제한

- 동일 Q0을 다시 전달한 resize, 실패한 resize, observe-only 모드는 위 render 위험의 증거가 아니다.
- vendor가 같은 내부 swapchain에 다른 queue를 실제 적용하는지가 핵심 미확인 조건이다. public proxy의 큐 인자가 내부에서도 그대로 적용된다고 가정해 실제 게임 발생을 단정하지 않는다.
- Q0은 REF가 COM 참조를 소유한다. 따라서 이 항목을 곧바로 해제된 queue의 UAF라고 부르지 않는다. 문제는 queue identity와 동기화의 불일치다.
- 새 Init 관찰과 후보 publish가 실제로 발생해 binding을 갱신한 경우에는 이 상태가 해소될 수 있다.

추상 상태 확인에서는 동일 큐 성공 / 다른 큐 실패가 일치 상태를 유지하고, 다른 큐 성공만 불일치를 만든다. GPU를 실행한 검증은 아니다. 런타임 확인에는 resize 전후 **COM identity**, 결과 코드, 실제 내부 swapchain, render / observe-only 상태가 필요하다. [모델 결과][model-result].

## 5. 기존 발견 사항의 재판정

### A1 — 이미 진입한 callback과 물리 hook 제거 사이의 수명 간격

Opti의 pre-retire는 REF의 `detach_xefg_binding_for_runtime_transition()`을 호출한다. REF는 M 안에서 renderer reset, `m_present_hook.reset()`, `m_swapchain_hook.reset()`, alias / semantic binding 제거를 실행한다. [Opti handoff][o-handoff], [REF detach][r-detach].

이미 REF `Present`에 진입했지만 M을 아직 얻지 못한 callback이 그 뒤에 실행될 수 있다. detach 직후에는 `m_is_phase_1=false`가 유지되고 instance hook은 null이므로 `present()`의 fallback이 null hook을 조회한다. Destroy 성공 reconciliation이 먼저 실행된 경우에도 phase1=true에 대응하는 present hook이 아직 설치되지 않은 구간이 존재한다. [REF Present][r-present], [REF Destroy reconciliation][r-destroy-note].

`ResizeBuffers` / `ResizeTarget`도 제거된 instance hook의 무검사 조회를 갖는다. 반면 `Present1` / `ResizeBuffers1`은 null 상태에서 `E_FAIL`을 반환하므로 같은 직접 AV로 계산하지 않는다. 그 `E_FAIL`을 과거 로그의 `E_ABORT`와 같은 결과로 취급하지 않는다. [REF ResizeBuffers][r-resize-entry], [REF ResizeTarget][r-target-entry], [REF Present1][r-present1], [REF ResizeBuffers1][r-resize1].

앞선 [스케줄 모델](E:/LocalTemp/opti-ref-compat-audit-20260915-independent/late_callback_schedule.py)은 callback의 잠금 대기와 detach 순서만 검증한다. 실제 vendor callback 배출 시점이나 게임 크래시를 재현하지 않았다. A2는 같은 callback 경계가 **교착**으로 이어지는 별도의 바깥 factory 잠금 경로다.

### F1 — 실행 중인 REF가 export 탐색에 보이지 않는 조건

MHRise startup에는 REF 모듈을 일반 모듈 목록에서 제거하는 호출이 있다. Opti는 모듈 열거가 정상 종료되었으나 pre-retire export를 못 찾은 경우 `NotAvailable`로 판단하고 해제를 허용한다. 이는 'REF가 실제로 없는 경우'와 'REF가 실행 중이지만 검색에 보이지 않는 경우'를 구분하지 못한다. REF의 borrowed internal presentation binding과 Opti의 public proxy 해제가 어긋날 수 있다. [REF startup 경계][r-startup], [Opti export 탐색][o-lookup], [Opti handoff 판정][o-handoff], [REF borrowed binding][r-binding].

원본 F1을 유지한다. 모듈 숨김 구현 자체의 감사·변경은 수행하지 않았다. 정확한 게임/REF binary에서 열거 결과가 어떻게 보이는지와 실제 크래시는 미확인이다. DD2 / newer RE Engine 전체에 같은 숨김 조건이 있다고 확대하지 않는다.

### F2 — Destroy hook 부재와 초기화 취소의 조합

REF runtime registry는 Init hook을 필수로 요구하지만 Destroy hook의 실패는 optional로 처리하고 Active 상태에 진입한다. REF는 Init 반환 시 내부 후보를 이미 publish하지만, Opti는 그 뒤의 GetSwapChainPtr 실패 또는 자신이 성공으로 인정하지 않는 Init 결과 때문에 Abort를 실행할 수 있다. [REF registry][r-registry], [REF Init publish][r-init-dispatch], [Opti 생성 / Abort][o-create1], [Opti abort][o-abort].

Destroy hook이 존재하면 `dispatch_destroy()`에서 pending 후보와 active binding을 정리한다. hook이 없고 아직 public proxy가 commit되지 않은 초기화 취소라면 이 정리 연결이 빠진다. 그러므로 F2를 COMPAT-RISK로 유지한다. positive warning을 받아들이는 정책 차이만으로 정상 hook 구성을 독립적인 크래시로 판정하지 않는다. [REF Destroy][r-destroy].

## 6. XeLL 감사 결과

**XeLL 자체에서 새로운 Opti ↔ REF 크래시 불일치는 확정하지 못했다.** 이는 XeLL 전체 구현의 무결성 판정이 아니다.

확인한 경계는 다음과 같다.

1. XeLL runtime 로드 및 필수 context export 확인, context 생성과 기존 context 재생성 차단.
2. `SetSleepMode` → XeFG `SetLatencyReduction` → fakenvapi context 공개.
3. XeFG Destroy의 정확한 성공 후 expected-old context 공개 해제, 그 뒤 XeLL Destroy.
4. 공개 해제 실패, XeLL Destroy 실패, XeFG Destroy warning / error 이후의 quarantine 및 재생성 차단.
5. Reflex Sleep / SetSleepMode / latency marker / async marker가 XeFG 사용 시 fakenvapi로 연결되는 경계.

[XeLL context lifecycle][o-xell], [XeFG / XeLL 연결][o-context], [XeFG teardown 순서][o-destroy], [fakenvapi 공개 해제][o-fake-clear], [Reflex bridge][o-reflex].

REF 소스에는 XeLL context를 직접 소유하거나 `xellSleep` / `xellAddMarkerData` / `xellDestroyContext`를 호출하는 경로가 없다. REF와의 연결은 XeFG presentation / retirement 경로에 있다. 따라서 외부 fakenvapi 내부의 marker 동시성이나 독립적인 XeLL loader 결함을 REF 호환성 finding으로 만들지 않았다. 이 감사에서는 Intel 및 fakenvapi binary 내부의 동기화 계약을 실행 검증하지 않았다.

## 7. 전체 범위표 — 확인한 진입점과 닫은 경계

`CLOSED`는 해당 소스 경계를 검토하고 아래 근거로 판정을 마쳤다는 뜻이다. 게임 실행 PASS나 '어떤 상황에서도 크래시 없음'을 뜻하지 않는다. 발견 사항이 있는 행도 감사 작업 자체는 CLOSED이다.

| # | 진입점 / 상태 전이 | 추적한 교차 경계 및 판정 | 상태 / 근거 |
|---|---|---|---|
| 01 | 최초 runtime load / already loaded / delayed load | Opti가 쓰는 libxess_fg export와 REF registry 연결. slot별 original을 구분하고 registry 잠금을 vendor 호출 전에 해제. Destroy hook 부재는 F2 | CLOSED — [Opti proxy][o-proxy], [REF registry][r-registry], [REF load][r-load] |
| 02 | pre-retire export ABI / 탐색 실패 | WINAPI·인자·uint32 상태 0/1/2 일치. scan 실패 및 unknown 상태는 차단; 검색 불가 REF는 F1 | CLOSED — [Opti lookup][o-lookup], [REF ABI][r-abi] |
| 03 | raw / wrapped DXGI factory | 두 진입 경로에서 XeFG 출력 선택 후 FGHooks에 도달. 공유 REF factory 잠금은 A2. CoreWindow는 이 XeFG HWND 생성 구현의 별도 진입점이 아님 | CLOSED — [factory 선택][o-factory-select], [raw][o-factory-call], [wrapped][o-factory-wrapped] |
| 04 | CreateSwapchain / CreateSwapchain1 | queue·factory COM keepalive를 Init까지 유지. public proxy 취득 후 queue commit. 두 API의 같은 HWND 분기와 Abort 모두 확인 | CLOSED — [legacy create][o-create], [HWND create][o-create1] |
| 05 | 같은 HWND preserve / recreate | preserve는 같은 proxy Resize; non-preserve는 handoff 후 retire. A2는 두 분기에 영향 | CLOSED — [create1][o-create1], [release][o-release-locked] |
| 06 | 다른 HWND / context 재사용 | 기존 context로 Init에 재진입하는 경로 확인. REF는 같은 runtime/context detach 가능. 동시 다중창 지원 부족만으로 paired crash 추가 판정하지 않음; vendor reject는 공통 Abort | CLOSED — [create][o-create], [REF runtime match][r-session-detach] |
| 07 | Init 전 / 도중 실패 | context / GetProperties / factory QI / Init 실패가 공통 Abort로 수렴. 후보 수락 전에는 stale REF 후보를 새로 만들지 않음 | CLOSED — [create][o-create], [abort][o-abort] |
| 08 | Init 후 GetSwapChainPtr / warning 실패 | REF 선행 publish와 Opti 후행 commit 차이 확인. 정상 Destroy hook은 정리; 부재는 F2 | CLOSED — [REF Init][r-init-dispatch], [GetSwapChainPtr][r-get-proxy], [Opti create1][o-create1] |
| 09 | 임시 factory 관찰 / 후보 검증 | HWND, swapchain QI, queue/device COM identity, DIRECT queue 관계 검증. 임의 후보를 render 모드로 받지 않음. vendor가 Init 내 후보 수명을 지킨다는 경계는 binary 검증 밖 | CLOSED — [관찰][r-discovery], [검증][r-discovery-candidate] |
| 10 | constructor-time pending / live handoff | pending 소유권, monitor→pending 순서, consume 전 mutex 해제, runtime별 폐기 확인. 정상 두 factory 중첩 순서에서 pending consume 연결 확인 | CLOSED — [handoff][r-candidate], [hook entry][r-hook-entry] |
| 11 | 첫 bind / native instance 승격 / hook 실패 | 새 hook 준비 후 commit, 같은 target 충돌 거부, 기존 native instance 승격. 실패를 자체 crash로 확대하지 않음 | CLOSED — [bind][r-bind] |
| 12 | 동일 후보 / 같은 객체 큐 변경 / 새 객체 rebind | 명시적으로 전달된 후보는 비교·renderer reset·COM 교체를 수행. ResizeBuffers1은 이 갱신 경로를 호출하지 않아 A3 | CLOSED — [rebind][r-rebind], [session commit][r-session-commit] |
| 13 | NGX / upscaler 입력 → XeFG | EvaluateState·StartNewFrame·SetResource 및 feature release에서 DestroyFGContext까지 추적. upscaler 알고리즘은 제외 | CLOSED — [입력][o-upscaler-input], [NGX release][o-ngx-release] |
| 14 | FFX / legacy FSR3 / SL 입력 → XeFG | fake context 종류와 generation, 자원 전달, 프레임 인덱스, 활성화 / 해제 경계 확인. FSRFG 출력 구현은 제외 | CLOSED — [FFX retire][o-ffx-retire], [legacy retire][o-legacy-retire], [SL input][o-sl-input] |
| 15 | resource readiness / ExecuteCommandLists | Opti는 등록된 command list가 실제 제출된 경우만 resource-ready와 queue를 갱신. REF 자체 command list라는 이유만으로 Opti game queue가 교체되는 경로는 입증되지 않음 | CLOSED — [ResTrack][o-restrack], [XeFG SetResource][o-resources] |
| 16 | XeFG SetEnabled / UI composition / interpolation / Dispatch | 상태 변경은 pause·deactivate·tagging으로 이어짐. 그 자체가 REF binding retirement는 아님. REF가 같은 frame-id / resource table을 소유하는 경로 없음 | CLOSED — [활성화][o-activate], [Dispatch][o-dispatch], [EvaluateState][o-evaluate] |
| 17 | Present / Present1 / 내부 wrapper Present | Opti F와 REF M, original 호출 전후, 중첩 진입, observe-only를 추적. 제거된 callback은 A1, factory 동시 재생성은 A2 | CLOSED — [Opti Present][o-present], [REF Present][r-present], [공통 Present][r-present-common], [wrapper][o-wrapper-present] |
| 18 | REF renderer / backbuffer / queue lifetime | init에서 검증된 큐를 소유하고 internal backbuffer에 제출. pending/reset/rebind까지 추적. resize 후 큐 변경은 A3 | CLOSED — [binding][r-binding], [렌더 제출][r-render-submit], [GPU 실행][r-command-execute] |
| 19 | ResizeBuffers / nested resize | public / internal mutex 경계, REF reset 전 original 호출, format·count 변경 후 reacquire를 확인. late callback은 A1 | CLOSED — [Opti resize][o-resize], [REF resize][r-resize], [renderer acquire][r-render-acquire] |
| 20 | ResizeBuffers1 / queue 배열 | nested original 전달 및 renderer reset / hold 완료 확인. 성공한 큐 변경 미반영은 A3 | CLOSED — [Opti resize1][o-resize1], [REF resize1][r-resize1] |
| 21 | ResizeTarget / fullscreen / Alt+Tab 연결 | borderless bypass, wrapper fullscreen / resize, REF reset, MHW ResizeHold를 확인. 게임별 실제 호출 조합은 미실행 | CLOSED — [Opti target][o-target], [wrapper fullscreen][o-fullscreen], [REF target][r-target], [hold 정책][r-hold] |
| 22 | public proxy final Release | 마지막 참조 probe, lifecycle 재확인, pre-retire, final callback→vendor destroy 순서 추적. A1/F1 포함; 불확실한 해제는 recreate 차단 | CLOSED — [public release hook][o-final-hook], [XeFG final release][o-final] |
| 23 | internal wrapper Release / stale generation | generation·현재 wrapper·HWND 일치 확인 후 공통 release 시도. nested lifecycle try-lock은 즉시 실패. REF 소비 여부까지 추적; 독립적인 wrapper 참조수 설계는 추가 finding 제외 | CLOSED — [wrapper Release][o-wrapper-release], [explicit release][o-explicit] |
| 24 | fake swapchain context destroy | stale token은 현재 세대 retire하지 않음; release 실패 시 claim 복원 / 에러 반환 후 detach 중단. preserve 분기는 lifecycle 유지 | CLOSED — [FFX retire][o-ffx-retire] |
| 25 | NGX shutdown / release / backend 변경 | gameplay 중 호출은 DestroyFGContext로 수렴하며 XeFG swapchain context는 유지. 실제 프로세스 종료 분기와 구분 | CLOSED — [NGX shutdown][o-ngx-shutdown], [NGX release][o-ngx-release], [backend 경계][o-backend], [FG-context only][o-fg-destroy] |
| 26 | vendor Destroy success / warning / failure | REF 선행 detach, 정확한 success reconciliation, Opti uncertain quarantine 확인. positive warning을 live context로 재사용하지 않음. A1 callback 간격은 별도 유지 | CLOSED — [Opti destroy][o-destroy], [REF destroy][r-destroy], [session][r-session-detach] |
| 27 | XeLL 생성·공개·marker·해제 | 제6절의 전체 visible bridge 추적. 새 REF-specific mismatch 미발견. 외부 fakenvapi / Intel 내부 동시성은 미확인 | CLOSED — [XeLL][o-xell], [연결][o-context], [공개 해제][o-fake-clear], [Reflex][o-reflex] |
| 28 | hook-monitor / minimize timeout / 일반 rehook | transition-active와 detached-uncertain에서는 generic recovery 억제, 지속 timeout은 quarantine. 공유 factory를 통한 별도 rehook 경로는 A2로 검토 | CLOSED — [monitor][r-monitor], [monitor 정책][r-monitor-policy], [factory][r-factory] |

### 검색 결과에서 의도적으로 제외한 것

- `framegen/ffx/FSRFG_Dx12.cpp`의 XeFG 문자열은 다른 FG 출력 구현 내부의 주석·문구이며 이 감사의 실행 대상이 아니다.
- `LibraryLoad_Hooks.cpp`의 XeFG/XeLL include, `DllNames.h` 이름 목록, REF debug-log 설정만으로 추가 lifecycle 소유자가 생기지는 않는다.
- Opti residency workaround, 자원 copy·flip·HUD 추출의 독립적인 구현 문제, REF 일반 renderer의 고정 buffer 수나 timeout 정책은 구체적인 **두 구성요소 사이 불일치** 없이 finding으로 올리지 않았다.
- Opti resource-input 경쟁이나 외부 fakenvapi 내부의 marker 경쟁을 REF 존재 때문에 생기는 결함으로 추정하지 않았다.
- 동일 스레드 재귀 호출만으로 두 스레드 교착을 주장하지 않았다. A2에는 서로 다른 스레드의 F/M 소유가 필요하다.

## 8. 검증 결과와 한계

| Goal | PASS / FAIL / BLOCKED | Evidence |
|---|---|---|
| 두 지정 포크의 고정 소스를 사용 | PASS | 원격 HEAD 재조회 및 감사 snapshot을 Git blob과 줄바꿈 정규화 후 대조 |
| F1/F2 외 XeFG / XeLL 진입점에서 독립적인 감사 수행 | PASS | 제2절 조사 방법, 제7절 28개 CLOSED 경계, A2/A3 추가 |
| 다른 Opti 기능·REF MOD 구현·코드 정리 제외 | PASS | 제1절 범위, 제7절 제외 판정; production source 수정 없음 |
| 소스 근거 / 호출 순서 / 조건부 위험 분리 | PASS | 고정 커밋 링크, A1/A2 추상 스케줄, A3 큐 identity 모델 |
| 보고서 근거와 로컬 산출물 검증 | PASS | [검증 결과][validation]: 소스 링크 범위·snapshot 내용·참조 label·모델 실행 확인 |

**소스 감사 작업은 완료했다. 게임 / Intel runtime / GPU에서의 검증은 수행하지 않았다.** 빌드·CI·게임 실행 PASS를 주장하지 않으며, 과거 crash dump의 원인이 위 항목이라고 단정하지 않는다. 런타임 미실행은 감사 자체의 발견 사항과 별개로 남는 증거 한계다.

원본 보고서에서 정상 경로의 lock order와 queue lifetime을 포괄적으로 안전하다고 읽을 수 있는 부분은 더 좁혀야 한다. pre-retire helper만 보면 순서가 맞더라도 바깥 factory 잠금(A2)이 남을 수 있고, 처음 큐를 COM으로 소유해도 resize 후 같은 큐가 맞다는 보장(A3)은 별개다. A1 역시 물리 hook 제거와 이미 진입한 callback 사이의 간격을 남긴다.

수정 코드를 작성하거나 게임 설정·DLL·브랜치·원격 저장소를 변경하지 않았다. 수정 우선순위를 정한다면 A1/A2의 callback·잠금 경계, F1의 handoff 가용성, F2의 초기화 rollback, A3의 resize 후 queue 검증 조건을 각각 별도 문제로 다루는 근거가 된다.

<!-- SOURCE_REFERENCES -->

[model-result]: E:/LocalTemp/opti-ref-compat-audit-20260915-independent/full_scope_models_result.json
[models]: E:/LocalTemp/opti-ref-compat-audit-20260915-independent/full_scope_models.py
[ms-resize1]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-resizebuffers1
[ms-sync]: https://learn.microsoft.com/en-us/windows/win32/direct3d12/important-changes-from-directx-11-to-directx-12
[o-abort]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L248-L263
[o-activate]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L872-L958
[o-backend]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/upscalers/FeatureProvider_Dx12.cpp#L108-L117
[o-context]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L151-L245
[o-create]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L469-L685
[o-create1]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L688-L869
[o-destroy]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L337-L415
[o-dispatch]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L985-L1282
[o-evaluate]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L1293-L1365
[o-explicit]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L1866-L1879
[o-factory-call]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/DxgiFactory_Hooks.cpp#L579-L647
[o-factory-hook]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/DxgiFactory_Hooks.cpp#L45-L102
[o-factory-select]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/Dxgi_Hooks.cpp#L148-L301
[o-factory-wrapped]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/DxgiFactory_WrappedCalls.cpp#L570-L647
[o-fake-clear]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/nvapi/fakenvapi.cpp#L133-L199
[o-ffx-retire]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp#L540-L617
[o-fg-destroy]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L960-L982
[o-final]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L1882-L1935
[o-final-hook]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/FG_Hooks.cpp#L1329-L1428
[o-fullscreen]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/wrapped/wrapped_swapchain.cpp#L709-L757
[o-handoff]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L296-L334
[o-legacy-retire]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/inputs/FG/FSR3_Dx12_FG.cpp#L680-L710
[o-lookup]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L37-L125
[o-ngx-release]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp#L704-L719
[o-ngx-shutdown]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp#L391-L411
[o-present]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/FG_Hooks.cpp#L1221-L1326
[o-proxy]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/proxies/XeFG_Proxy.h#L156-L294
[o-reflex]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/Reflex_Hooks.cpp#L13-L182
[o-release-locked]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L1938-L2028
[o-resize]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/FG_Hooks.cpp#L620-L859
[o-resize1]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/FG_Hooks.cpp#L872-L1113
[o-resize1-forward]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/FG_Hooks.cpp#L1080-L1113
[o-resources]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/framegen/xefg/XeFG_Dx12.cpp#L1600-L1864
[o-restrack]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/resource_tracking/ResTrack_dx12.cpp#L625-L698
[o-sl-input]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp#L7-L457
[o-target]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/hooks/FG_Hooks.cpp#L861-L870
[o-upscaler-input]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/inputs/FG/Upscaler_Inputs_Dx12.cpp#L85-L232
[o-wrapper-present]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/wrapped/wrapped_swapchain.cpp#L673-L699
[o-wrapper-queue]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/wrapped/wrapped_swapchain.cpp#L1147-L1216
[o-wrapper-release]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/wrapped/wrapped_swapchain.cpp#L554-L643
[o-wrapper-resize-forward]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/wrapped/wrapped_swapchain.cpp#L1291-L1312
[o-xell]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/OptiScaler/proxies/XeLL_Proxy.h#L230-L428
[original]: https://github.com/onehoon/OptiScaler/blob/46cb34eda30830f0eaea7846abacc03055377c42/doc/RELEASE_0_9_REF_CAPCOM_END_TO_END_COMPATIBILITY_CRASH_AUDIT_2026-09-15.md
[r-abi]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGCompatibility.hpp#L24-L70
[r-bind]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L370-L534
[r-binding]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGBinding.hpp#L15-L68
[r-candidate]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGCandidateHandoff.cpp#L12-L112
[r-command-execute]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/mods/vr/d3d12/CommandContext.cpp#L234-L251
[r-destroy]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGCompatibility.cpp#L461-L492
[r-destroy-note]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L329-L348
[r-detach]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L257-L327
[r-discovery]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGDiscovery.cpp#L138-L176
[r-discovery-candidate]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGDiscovery.cpp#L182-L261
[r-factory]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L748-L825
[r-factory-install]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L1257-L1283
[r-get-proxy]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGCompatibility.cpp#L434-L459
[r-handoff]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGCompatibility.cpp#L255-L350
[r-hold]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGPresentationSession.cpp#L165-L303
[r-hook-entry]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L884-L913
[r-init-dispatch]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGCompatibility.cpp#L372-L431
[r-load]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGCompatibility.cpp#L177-L233
[r-monitor]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/REFramework.cpp#L66-L157
[r-monitor-policy]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGPresentationSession.cpp#L58-L94
[r-present]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L1341-L1366
[r-present-common]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L1543-L1693
[r-present1]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L1838-L1863
[r-rebind]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L536-L600
[r-registry]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGRuntimeRegistry.cpp#L84-L198
[r-render-acquire]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/REFramework.cpp#L2822-L2880
[r-render-submit]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/REFramework.cpp#L1209-L1274
[r-resize]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L1903-L1978
[r-resize-entry]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L1867-L1904
[r-resize1]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L1983-L2058
[r-session-commit]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGPresentationSession.cpp#L312-L430
[r-session-detach]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/compatibility/xefg/XeFGPresentationSession.cpp#L96-L163
[r-startup]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/Main.cpp#L98-L115
[r-target]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L2063-L2190
[r-target-entry]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L2063-L2099
[r-unhook]: https://github.com/onehoon/REFramework/blob/4bf45b370e602f7f6a3ca54f308daa4e353aab8a/src/D3D12Hook.cpp#L1285-L1337
[validation]: E:/LocalTemp/opti-ref-compat-audit-20260915-independent/full_scope_validation.json
