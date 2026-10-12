#include <oyi_core/SpinLock.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>
#include <vector>

namespace {

using oyi::core::SpinLock;
using oyi::core::SpinLockGuard;

} // namespace

TEST_CASE("SpinLock serialises increments from several threads", "[core][spinlock]") {
    SpinLock lock;
    uint64_t counter               = 0;
    constexpr uint32_t kThreads    = 4;
    constexpr uint32_t kIncrements = 20000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (uint32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&lock, &counter] {
            for (uint32_t i = 0; i < kIncrements; ++i) {
                const SpinLockGuard guard(lock);
                ++counter;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(counter == uint64_t{kThreads} * kIncrements);
}

TEST_CASE("TryLock fails while the lock is held and succeeds after Unlock", "[core][spinlock]") {
    SpinLock lock;
    REQUIRE(lock.TryLock());
    CHECK_FALSE(lock.TryLock());
    lock.Unlock();
    CHECK(lock.TryLock());
    lock.Unlock();
}
