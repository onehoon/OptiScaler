#pragma once

#include <cstddef>
#include <cstdint>

namespace XeFGTrace
{
constexpr uint32_t kTraceMagic = 0x54474658u; // "XFGT" in little-endian files
constexpr uint32_t kTraceVersion = 1;
constexpr uint32_t kTraceCapacity = 65536;

enum class EventType : uint32_t
{
    PresentEnter = 1,
    Present1Enter,
    PresentInternalBypass,
    Present1InternalBypass,
    PresentDispatchBegin,
    PresentDispatchEnd,
    DxgiPresentBegin,
    DxgiPresentEnd,
    DxgiPresent1Begin,
    DxgiPresent1End,
    PresentMutexDecision,
    PresentMutexWaitBegin,
    PresentMutexAcquired,
    PresentMutexSameThreadBypass,
    PresentMutexUnlock,
    ResizeEnter,
    Resize1Enter,
    ResizeInternalBypass,
    Resize1InternalBypass,
    ResizeDxgiBegin,
    ResizeDxgiEnd,
    Resize1DxgiBegin,
    Resize1DxgiEnd,
    FenceSignalBegin,
    FenceSignalEnd,
    FenceWaitBegin,
    FenceWaitEnd,
    FenceRecreateBegin,
    FenceRecreateEnd,
    SwapchainCreateBegin,
    SwapchainCreateEnd,
    SwapchainCreate1Begin,
    SwapchainCreate1End,
    SwapchainReleaseEnter,
    SwapchainFinalProxyRelease,
    SwapchainReleaseLifecycleBegin,
    SwapchainReleaseLifecycleEnd,
    XeFGCreateContextResult,
    XeFGInitSwapchainResult,
    XeFGGetSwapchainPtrResult,
    XeFGDestroyBegin,
    XeFGDestroyResult,
    XeFGSetEnabledResult,
    XeFGTagFrameConstantsResult,
    XeFGSetPresentIdResult,
    XeFGTagFrameResourceResult,
    XeFGSetNumInterpolatedFramesResult,
    XeFGSetUiCompositionResult,
    PrimaryFailureTrigger,
    XeFGSetLoggingCallbackResult,
    XeFGSetLatencyReductionResult,
    XeFGGetPropertiesResult,
    XeFGEnableDebugFeatureResult,
    XeFGPresentEnter,
    XeFGPresentBeforeUiWork,
    XeFGPresentAfterUiWork,
    XeFGPresentBeforeScWork,
    XeFGPresentAfterScWork,
    XeFGPresentBeforeDispatch,
    XeFGPresentAfterDispatch,
    UiCommandListCloseResult,
    UiExecuteCommandListsBegin,
    UiExecuteCommandListsEnd,
    UiQueueSignalResult,
    UiFenceSetEventResult,
    UiFenceWaitResult,
    UiAllocatorResetResult,
    UiCommandListResetResult,
    ScCommandListCloseResult,
    ScExecuteCommandListsBegin,
    ScExecuteCommandListsEnd,
    ScAllocatorResetResult,
    ScCommandListResetResult,
    DispatchEnter,
    DispatchAfterIndexResolve,
    DispatchBeforeHudlessLookup,
    DispatchAfterHudlessLookup,
    DispatchBeforeHudlessSetResource,
    SetResourceEnter,
    SetResourceBeforeMutexWait,
    SetResourceAfterMutexAcquire,
    SetResourceBeforeTagFrameResource,
};

enum TraceFlagBits : uint32_t
{
    SkipResize = 1u << 0,
    SkipResize1 = 1u << 1,
    SkipPresent = 1u << 2,
    SkipPresent1 = 1u << 3,
    XeFGActive = 1u << 4,
    XeFGPaused = 1u << 5,
};

struct alignas(8) TraceHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t recordSize;
    uint32_t capacity;
    uint32_t processId;
    uint64_t qpcFrequency;
    uint64_t sessionStartQpc;
    uint64_t writeSequence;
};

struct alignas(8) TraceRecord
{
    uint64_t committedSequence;
    uint64_t qpc;
    uint64_t swapchain;
    uint64_t objectOrContext;
    uint64_t auxPointer;
    uint64_t fenceValue;
    uint32_t threadId;
    uint32_t eventType;
    uint32_t mutexOwner;
    uint32_t mutexOwnerThread;
    int32_t result;
    uint32_t flagsSnapshot;
    uint32_t aux0;
    uint32_t aux1;
};

static_assert(sizeof(TraceHeader) == 48);
static_assert(sizeof(TraceRecord) == 80);
static_assert(offsetof(TraceRecord, committedSequence) == 0);
static_assert(offsetof(TraceRecord, qpc) == 8);
static_assert(alignof(TraceRecord) >= 8);

constexpr int32_t kEAbortResult = static_cast<int32_t>(0x80004004u);

void Initialize() noexcept;
void Shutdown() noexcept;
void FlushAfterFailureBestEffort() noexcept;

void RecordPrimaryFailureTrigger(EventType sourceEvent,
                                 uint64_t swapchain,
                                 uint64_t objectOrContext,
                                 int32_t rawResult,
                                 uint32_t flagsSnapshot) noexcept;

void Record(EventType eventType,
            uint64_t swapchain = 0,
            uint64_t objectOrContext = 0,
            uint64_t auxPointer = 0,
            uint64_t fenceValue = 0,
            uint32_t mutexOwner = 0,
            uint32_t mutexOwnerThread = 0,
            int32_t result = 0,
            uint32_t flagsSnapshot = 0,
            uint32_t aux0 = 0,
            uint32_t aux1 = 0) noexcept;
}
