#include "pch.h"

#include "diagnostics/XeFGTrace.h"

#include <atomic>
#include <cstring>
#include <cwchar>

namespace
{
constexpr size_t kPathCapacity = 32768;
constexpr size_t kTraceFileSize = sizeof(XeFGTrace::TraceHeader) +
                                   sizeof(XeFGTrace::TraceRecord) * XeFGTrace::kTraceCapacity;

std::atomic<LONG> g_state { 0 }; // 0 = unused, 1 = initializing, 2 = ready, 3 = stopped
std::atomic<LONG> g_failureFlushed { 0 };
HANDLE g_file = INVALID_HANDLE_VALUE;
HANDLE g_mapping = nullptr;
void* g_view = nullptr;
XeFGTrace::TraceHeader* g_header = nullptr;
XeFGTrace::TraceRecord* g_records = nullptr;

bool BuildTracePath(wchar_t* current, wchar_t* previous) noexcept
{
    wchar_t modulePath[kPathCapacity] {};
    constexpr size_t modulePathCapacity = sizeof(modulePath) / sizeof(modulePath[0]);
    DWORD length = GetModuleFileNameW(dllModule, modulePath, static_cast<DWORD>(modulePathCapacity));
    if (length == 0 || length >= modulePathCapacity)
        return false;

    wchar_t* separator = std::wcsrchr(modulePath, L'\\');
    const wchar_t* directoryEnd = separator != nullptr ? separator + 1 : modulePath;
    const size_t directoryLength = static_cast<size_t>(directoryEnd - modulePath);
    constexpr wchar_t currentName[] = L"OptiScaler_XeFGTrace.bin";
    constexpr wchar_t previousName[] = L"OptiScaler_XeFGTrace.prev.bin";

    constexpr size_t currentNameLength = sizeof(currentName) / sizeof(currentName[0]);
    constexpr size_t previousNameLength = sizeof(previousName) / sizeof(previousName[0]);
    if (directoryLength + currentNameLength > kPathCapacity || directoryLength + previousNameLength > kPathCapacity)
        return false;

    std::wmemcpy(current, modulePath, directoryLength);
    std::wmemcpy(current + directoryLength, currentName, currentNameLength);
    std::wmemcpy(previous, modulePath, directoryLength);
    std::wmemcpy(previous + directoryLength, previousName, previousNameLength);
    return true;
}

void CloseHandles() noexcept
{
    g_header = nullptr;
    g_records = nullptr;
    if (g_view != nullptr)
    {
        UnmapViewOfFile(g_view);
        g_view = nullptr;
    }
    if (g_mapping != nullptr)
    {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
    if (g_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}
}

namespace XeFGTrace
{
void Initialize() noexcept
{
    LONG expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return;

    wchar_t currentPath[kPathCapacity] {};
    wchar_t previousPath[kPathCapacity] {};
    if (!BuildTracePath(currentPath, previousPath))
    {
        g_state.store(3, std::memory_order_release);
        return;
    }

    DeleteFileW(previousPath);
    MoveFileW(currentPath, previousPath);

    g_file = CreateFileW(currentPath, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE)
    {
        g_state.store(3, std::memory_order_release);
        return;
    }

    LARGE_INTEGER fileSize {};
    fileSize.QuadPart = static_cast<LONGLONG>(kTraceFileSize);
    if (!SetFilePointerEx(g_file, fileSize, nullptr, FILE_BEGIN) || !SetEndOfFile(g_file))
    {
        CloseHandles();
        g_state.store(3, std::memory_order_release);
        return;
    }

    g_mapping = CreateFileMappingW(g_file, nullptr, PAGE_READWRITE, fileSize.HighPart, fileSize.LowPart, nullptr);
    if (g_mapping == nullptr)
    {
        CloseHandles();
        g_state.store(3, std::memory_order_release);
        return;
    }

    g_view = MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (g_view == nullptr)
    {
        CloseHandles();
        g_state.store(3, std::memory_order_release);
        return;
    }

    std::memset(g_view, 0, kTraceFileSize);
    g_header = static_cast<TraceHeader*>(g_view);
    g_records = reinterpret_cast<TraceRecord*>(static_cast<uint8_t*>(g_view) + sizeof(TraceHeader));

    LARGE_INTEGER frequency {}, start {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    g_header->magic = kTraceMagic;
    g_header->version = kTraceVersion;
    g_header->headerSize = sizeof(TraceHeader);
    g_header->recordSize = sizeof(TraceRecord);
    g_header->capacity = kTraceCapacity;
    g_header->processId = GetCurrentProcessId();
    g_header->qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
    g_header->sessionStartQpc = static_cast<uint64_t>(start.QuadPart);
    g_header->writeSequence = 0;
    FlushViewOfFile(g_view, sizeof(TraceHeader));
    g_state.store(2, std::memory_order_release);
}

void Shutdown() noexcept
{
    LONG expected = 2;
    if (!g_state.compare_exchange_strong(expected, 3, std::memory_order_acq_rel))
        return;

    if (g_view != nullptr)
    {
        FlushViewOfFile(g_view, 0);
        FlushFileBuffers(g_file);
    }
    CloseHandles();
}

void FlushAfterFailureBestEffort() noexcept
{
    if (g_state.load(std::memory_order_acquire) != 2 ||
        g_failureFlushed.exchange(1, std::memory_order_acq_rel) != 0)
        return;

    if (g_view != nullptr)
        FlushViewOfFile(g_view, 0);
    if (g_file != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_file);
}

void Record(EventType eventType, uint64_t swapchain, uint64_t objectOrContext, uint64_t auxPointer,
            uint64_t fenceValue, uint32_t mutexOwner, uint32_t mutexOwnerThread, int32_t result,
            uint32_t flagsSnapshot, uint32_t aux0, uint32_t aux1) noexcept
{
    if (g_state.load(std::memory_order_acquire) != 2 || g_header == nullptr || g_records == nullptr)
        return;

    const auto sequence = static_cast<uint64_t>(
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_header->writeSequence)));
    TraceRecord& record = g_records[sequence % kTraceCapacity];
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&record.committedSequence), 0);

    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    record.qpc = static_cast<uint64_t>(now.QuadPart);
    record.swapchain = swapchain;
    record.objectOrContext = objectOrContext;
    record.auxPointer = auxPointer;
    record.fenceValue = fenceValue;
    record.threadId = GetCurrentThreadId();
    record.eventType = static_cast<uint32_t>(eventType);
    record.mutexOwner = mutexOwner;
    record.mutexOwnerThread = mutexOwnerThread;
    record.result = result;
    record.flagsSnapshot = flagsSnapshot;
    record.aux0 = aux0;
    record.aux1 = aux1;
    std::atomic_thread_fence(std::memory_order_release);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&record.committedSequence),
                           static_cast<LONG64>(sequence));
}
}
