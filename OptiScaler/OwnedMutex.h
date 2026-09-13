#pragma once

#include "SysUtils.h"

#include <atomic>
#include <shared_mutex>

class OwnedMutex
{
  private:
    std::shared_mutex mtx;
    std::atomic<uint32_t> owner { 0 }; // don't use 0
    std::atomic<DWORD> ownerThread { 0 };

  public:
    void lock(uint32_t _owner)
    {
        mtx.lock();
        ownerThread.store(GetCurrentThreadId(), std::memory_order_release);
        owner.store(_owner, std::memory_order_release);
    }

    // Only unlocks if both owner tag and owning thread match
    void unlockThis(uint32_t _owner)
    {
        uint32_t current_owner = owner.load(std::memory_order_acquire);
        DWORD current_owner_thread = ownerThread.load(std::memory_order_acquire);
        DWORD current_thread = GetCurrentThreadId();

        if (current_owner == 0 || current_owner != _owner || current_owner_thread != current_thread)
        {
            LOG_WARN("current_owner: {}, current_owner_thread: {}, current_thread: {}, _owner: {}", current_owner,
                     current_owner_thread, current_thread, _owner);
            return;
        }

        owner.store(0, std::memory_order_release);
        ownerThread.store(0, std::memory_order_release);
        mtx.unlock();
    }

    bool isOwnedByCurrentThread(uint32_t _owner) const
    {
        return owner.load(std::memory_order_acquire) == _owner &&
               ownerThread.load(std::memory_order_acquire) == GetCurrentThreadId();
    }

    uint32_t getOwner() { return owner.load(std::memory_order_seq_cst); }

    DWORD getOwnerThread() const { return ownerThread.load(std::memory_order_acquire); }
};

class OwnedLockGuard
{
  private:
    OwnedMutex& _mutex;
    uint32_t _owner_id;

  public:
    OwnedLockGuard(OwnedMutex& mutex, uint32_t owner_id) : _mutex(mutex), _owner_id(owner_id)
    {
        _mutex.lock(_owner_id);
    }

    ~OwnedLockGuard() { _mutex.unlockThis(_owner_id); }
};
