#pragma once

#include <atomic>
#include <thread>

namespace oyi::core {

/// Test-and-test-and-set lock for critical sections of a few instructions, such as linking a job
/// into a wait list. Not reentrant and not fair; where the section can block, use a mutex.
class SpinLock {
public:
    SpinLock()                           = default;
    SpinLock(const SpinLock&)            = delete;
    SpinLock(SpinLock&&)                 = delete;
    SpinLock& operator=(const SpinLock&) = delete;
    SpinLock& operator=(SpinLock&&)      = delete;
    ~SpinLock()                          = default;

    void Lock() noexcept {
        while (true) {
            if (!mLocked.exchange(true, std::memory_order_acquire)) {
                return;
            }
            while (mLocked.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
        }
    }

    [[nodiscard]] bool TryLock() noexcept { return !mLocked.exchange(true, std::memory_order_acquire); }

    void Unlock() noexcept { mLocked.store(false, std::memory_order_release); }

private:
    std::atomic<bool> mLocked = false;
};

/// Holds a SpinLock for the lifetime of the guard.
class SpinLockGuard {
public:
    explicit SpinLockGuard(SpinLock& lock) noexcept : mLock(lock) { mLock.Lock(); }
    SpinLockGuard(const SpinLockGuard&)            = delete;
    SpinLockGuard(SpinLockGuard&&)                 = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(SpinLockGuard&&)      = delete;
    ~SpinLockGuard() { mLock.Unlock(); }

private:
    SpinLock& mLock;
};

} // namespace oyi::core
