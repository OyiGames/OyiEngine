#include <oyi_core/Expected.h>
#include <oyi_core/Memory.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_job/CompletionDeferral.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobSystem.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::MemoryTag;
using oyi::core::SystemAllocator;
using oyi::job::CompletionDeferral;
using oyi::job::CounterId;
using oyi::job::JobContext;
using oyi::job::JobDesc;
using oyi::job::JobPriority;
using oyi::job::JobSystem;

constexpr JobSystem::Config kTwoWorkers{
    .mWorkerCount = 2, .mMaxJobs = 4096, .mMaxCounters = 4096, .mSpinIterations = 8};
constexpr JobSystem::Config kOneWorker{.mWorkerCount = 1, .mMaxJobs = 64, .mMaxCounters = 16, .mSpinIterations = 8};

[[nodiscard]] JobDesc Describe(std::string_view name, oyi::job::JobFunction function, void* data,
                               JobPriority priority = JobPriority::Critical) {
    return JobDesc{.mName = name, .mFunction = function, .mData = data, .mPriority = priority};
}

/// Occupies a worker until `mRelease` is set, so that tests can control when the queue drains.
struct Blocker {
    std::atomic<bool> mRelease = false;
    std::atomic<bool> mStarted = false;

    static void Run(JobContext& /*context*/, void* data) {
        auto* const self = static_cast<Blocker*>(data);
        self->mStarted.store(true);
        while (!self->mRelease.load()) {
            std::this_thread::yield();
        }
    }

    void WaitUntilStarted() const {
        while (!mStarted.load()) {
            std::this_thread::yield();
        }
    }
};

struct Probe {
    std::atomic<bool> mRan   = false;
    std::atomic<bool> mInJob = false;
    uint32_t mWorkerIndex    = 0;
    bool mFrameMemoryWorked  = false;
    std::string_view mSeenName;

    static void Run(JobContext& context, void* data) {
        auto* const self          = static_cast<Probe*>(data);
        self->mWorkerIndex        = context.mWorkerIndex;
        self->mSeenName           = context.mJobName;
        self->mInJob              = JobSystem::IsInJob();
        const void* const scratch = context.mFrameAllocator.Allocate(256, 16);
        self->mFrameMemoryWorked  = scratch != nullptr;
        self->mRan.store(true);
    }
};

struct Sequence {
    std::atomic<uint32_t> mNext = 0;
    std::atomic<uint32_t> mSlots[3];

    static void RecordCritical(JobContext& /*context*/, void* data) { Record(data, 0); }
    static void RecordRender(JobContext& /*context*/, void* data) { Record(data, 1); }
    static void RecordLatent(JobContext& /*context*/, void* data) { Record(data, 2); }

    static void Record(void* data, size_t slot) {
        auto* const self = static_cast<Sequence*>(data);
        self->mSlots[slot].store(self->mNext.fetch_add(1));
    }
};

struct Accumulate {
    std::atomic<uint64_t> mSum = 0;
    uint64_t mValue            = 0;

    static void Run(JobContext& /*context*/, void* data) {
        auto* const self = static_cast<Accumulate*>(data);
        self->mSum.fetch_add(self->mValue);
    }
};

struct Chain {
    uint32_t mStep = 0; ///< written by every link; the dependency chain makes the writes ordered

    static void Run(JobContext& /*context*/, void* data) { ++static_cast<Chain*>(data)->mStep; }
};

} // namespace

TEST_CASE("a job runs on a worker with a name, a context and frame memory", "[job][system]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    CHECK(jobs.GetStats().mWorkerCount == 2);
    CHECK_FALSE(JobSystem::IsInJob());

    Probe probe;
    const CounterId done = jobs.CreateCounter();
    REQUIRE(jobs.RunJob(Describe("probe", &Probe::Run, &probe), CounterId::Invalid(), done).has_value());
    jobs.WaitForCounter(done);

    CHECK(probe.mRan.load());
    CHECK(probe.mInJob.load());
    CHECK(probe.mWorkerIndex <= 2);
    CHECK(probe.mSeenName == "probe");
    CHECK(probe.mFrameMemoryWorked);
    CHECK(jobs.GetStats().mInFlightJobs == 0);
    jobs.ReleaseCounter(done);
}

TEST_CASE("a job that waits on a counter runs after the jobs accumulating into it", "[job][system]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    Chain chain;
    const CounterId first  = jobs.CreateCounter();
    const CounterId second = jobs.CreateCounter();

    REQUIRE(jobs.RunJob(Describe("link-1", &Chain::Run, &chain), CounterId::Invalid(), first).has_value());
    REQUIRE(jobs.RunJob(Describe("link-2", &Chain::Run, &chain), first, second).has_value());
    jobs.WaitForCounter(second);

    CHECK(chain.mStep == 2);
    CHECK(jobs.IsCounterZero(first));
    jobs.ReleaseCounter(first);
    jobs.ReleaseCounter(second);
}

TEST_CASE("a thousand jobs accumulate into one counter", "[job][system]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    constexpr uint64_t kCount = 1000;
    std::vector<Accumulate> items(kCount);
    const CounterId done = jobs.CreateCounter();
    for (uint64_t i = 0; i < kCount; ++i) {
        items[i].mValue = i;
        REQUIRE(
            jobs.RunJob(Describe("accumulate", &Accumulate::Run, &items[i]), CounterId::Invalid(), done).has_value());
    }
    jobs.WaitForCounter(done);

    uint64_t total = 0;
    for (const Accumulate& item : items) {
        total += item.mSum.load();
    }
    CHECK(total == kCount * (kCount - 1) / 2);
    jobs.ReleaseCounter(done);
}

TEST_CASE("an empty job splices counters without entering a queue", "[job][system]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    Chain chain;
    const CounterId first  = jobs.CreateCounter();
    const CounterId second = jobs.CreateCounter();
    const CounterId third  = jobs.CreateCounter();

    REQUIRE(jobs.RunJob(Describe("link", &Chain::Run, &chain), CounterId::Invalid(), first).has_value());
    REQUIRE(jobs.RunJob(Describe("splice", nullptr, nullptr), first, second).has_value());
    REQUIRE(jobs.RunJob(Describe("after-splice", &Chain::Run, &chain), second, third).has_value());
    jobs.WaitForCounter(third);

    CHECK(chain.mStep == 2);
    CHECK(jobs.IsCounterZero(second));
    jobs.ReleaseCounter(first);
    jobs.ReleaseCounter(second);
    jobs.ReleaseCounter(third);
}

TEST_CASE("a worker takes Critical before Render before Latent", "[job][system]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kOneWorker);
    Blocker blocker;
    Sequence sequence;
    const CounterId done = jobs.CreateCounter();

    REQUIRE(jobs.RunJob(Describe("blocker", &Blocker::Run, &blocker), CounterId::Invalid(), done).has_value());
    blocker.WaitUntilStarted();
    // The single worker is busy, so these queue up; it must drain them by priority, not by arrival.
    REQUIRE(jobs.RunJob(Describe("latent", &Sequence::RecordLatent, &sequence, JobPriority::Latent),
                        CounterId::Invalid(), done)
                .has_value());
    REQUIRE(jobs.RunJob(Describe("render", &Sequence::RecordRender, &sequence, JobPriority::Render),
                        CounterId::Invalid(), done)
                .has_value());
    REQUIRE(jobs.RunJob(Describe("critical", &Sequence::RecordCritical, &sequence, JobPriority::Critical),
                        CounterId::Invalid(), done)
                .has_value());
    blocker.mRelease.store(true);
    // Poll instead of WaitForCounter so the main thread does not take the Critical job itself.
    while (!jobs.IsCounterZero(done)) {
        std::this_thread::yield();
    }

    CHECK(sequence.mSlots[0].load() == 0);
    CHECK(sequence.mSlots[1].load() == 1);
    CHECK(sequence.mSlots[2].load() == 2);
    jobs.ReleaseCounter(done);
}

TEST_CASE("the main thread executes Critical jobs while it waits", "[job][system]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kOneWorker);
    Blocker blocker;
    Probe probe;
    const CounterId blockerDone = jobs.CreateCounter();
    const CounterId probeDone   = jobs.CreateCounter();

    REQUIRE(jobs.RunJob(Describe("blocker", &Blocker::Run, &blocker), CounterId::Invalid(), blockerDone).has_value());
    blocker.WaitUntilStarted();
    REQUIRE(jobs.RunJob(Describe("probe", &Probe::Run, &probe), CounterId::Invalid(), probeDone).has_value());
    jobs.WaitForCounter(probeDone);

    CHECK(probe.mRan.load());
    CHECK(probe.mWorkerIndex == 1); // the main thread's slot comes after the single worker
    blocker.mRelease.store(true);
    jobs.WaitForCounter(blockerDone);
    jobs.ReleaseCounter(blockerDone);
    jobs.ReleaseCounter(probeDone);
}

TEST_CASE("a thread other than the main thread blocks until the counter is zero", "[job][system]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kOneWorker);
    const CounterId gate             = jobs.CreateCounter();
    std::atomic<bool> waiterReturned = false;

    std::thread waiter;
    {
        const CompletionDeferral hold(jobs, gate);
        waiter = std::thread([&jobs, gate, &waiterReturned] {
            jobs.WaitForCounter(gate);
            waiterReturned.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK_FALSE(waiterReturned.load());
    }
    waiter.join();
    CHECK(waiterReturned.load());
    jobs.ReleaseCounter(gate);
}

TEST_CASE("the job pool reports exhaustion and recovers", "[job][system]") {
    SystemAllocator allocator;
    constexpr JobSystem::Config kTinyPool{.mWorkerCount = 1, .mMaxJobs = 2, .mMaxCounters = 4, .mSpinIterations = 8};
    JobSystem jobs(allocator, kTinyPool);
    Blocker blocker;
    Chain chain;
    const CounterId done = jobs.CreateCounter();

    REQUIRE(jobs.RunJob(Describe("blocker", &Blocker::Run, &blocker), CounterId::Invalid(), done).has_value());
    blocker.WaitUntilStarted();
    REQUIRE(jobs.RunJob(Describe("queued", &Chain::Run, &chain), CounterId::Invalid(), done).has_value());
    const auto overflow = jobs.RunJob(Describe("overflow", &Chain::Run, &chain), CounterId::Invalid(), done);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().mCode == ErrorCode::OutOfMemory);
    CHECK(jobs.GetStats().mInFlightJobs == 2);

    blocker.mRelease.store(true);
    jobs.WaitForCounter(done);
    CHECK(chain.mStep == 1);
    CHECK(jobs.GetStats().mInFlightJobs == 0);
    REQUIRE(jobs.RunJob(Describe("after", &Chain::Run, &chain), CounterId::Invalid(), done).has_value());
    jobs.WaitForCounter(done);
    CHECK(chain.mStep == 2);
    jobs.ReleaseCounter(done);
}

TEST_CASE("frame memory used by jobs is released by tag", "[job][system][memory]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    const MemoryTag frame = MemoryTag::ForFrame(1);
    jobs.BeginFrame(frame);
    CHECK(jobs.GetMainFrameAllocator().GetTag() == frame);

    std::vector<Probe> probes(64);
    const CounterId done = jobs.CreateCounter();
    for (Probe& probe : probes) {
        REQUIRE(jobs.RunJob(Describe("scratch", &Probe::Run, &probe), CounterId::Invalid(), done).has_value());
    }
    jobs.WaitForCounter(done);
    CHECK(std::ranges::all_of(probes, [](const Probe& probe) { return probe.mFrameMemoryWorked; }));
    CHECK(jobs.GetFrameHeapStats().mUsedBlocks >= 1);

    jobs.FreeFrame(frame);
    CHECK(jobs.GetFrameHeapStats().mUsedBlocks == 0);
    jobs.ReleaseCounter(done);
}

TEST_CASE("a chain of two thousand dependent jobs runs in order", "[job][system][stress]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    constexpr uint32_t kLinks = 2000;
    Chain chain;
    std::vector<CounterId> counters;
    counters.reserve(kLinks);
    CounterId previous = CounterId::Invalid();
    for (uint32_t i = 0; i < kLinks; ++i) {
        const CounterId current = jobs.CreateCounter();
        REQUIRE(current.IsValid());
        REQUIRE(jobs.RunJob(Describe("link", &Chain::Run, &chain), previous, current).has_value());
        counters.push_back(current);
        previous = current;
    }
    jobs.WaitForCounter(previous);
    CHECK(chain.mStep == kLinks);
    for (const CounterId counter : counters) {
        jobs.ReleaseCounter(counter);
    }
    CHECK(jobs.GetStats().mInFlightJobs == 0);
}
