#include <oyi_core/SystemAllocator.h>
#include <oyi_job/CompletionDeferral.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobBuilder.h>
#include <oyi_job/JobSystem.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using oyi::core::SystemAllocator;
using oyi::job::CompletionDeferral;
using oyi::job::CounterId;
using oyi::job::JobBuilder;
using oyi::job::JobContext;
using oyi::job::JobPriority;
using oyi::job::JobSystem;

constexpr JobSystem::Config kTwoWorkers{
    .mWorkerCount = 2, .mMaxJobs = 1024, .mMaxCounters = 1024, .mSpinIterations = 8};

struct Recorder {
    std::vector<uint32_t> mOrder; ///< written without locks: only a serial chain may touch it
};

struct Numbered {
    Recorder* mRecorder = nullptr;
    uint32_t mId        = 0;

    // NOLINTNEXTLINE(misc-const-correctness)
    static void Run(JobContext& /*context*/, void* data) {
        const auto* const self = static_cast<const Numbered*>(data);
        self->mRecorder->mOrder.push_back(self->mId);
    }
};

/// Two of these only finish when both are running at the same time.
struct Rendezvous {
    std::atomic<bool> mMine  = false;
    std::atomic<bool>* mPeer = nullptr;

    static void Run(JobContext& /*context*/, void* data) {
        auto* const self = static_cast<Rendezvous*>(data);
        self->mMine.store(true);
        while (!self->mPeer->load()) {
            std::this_thread::yield();
        }
    }
};

struct Counter {
    std::atomic<uint32_t> mValue = 0;

    static void Increment(JobContext& /*context*/, void* data) { static_cast<Counter*>(data)->mValue.fetch_add(1); }
};

} // namespace

TEST_CASE("a serial builder runs its jobs one after another in dispatch order", "[job][builder]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    Recorder recorder;
    std::vector<Numbered> numbered(50);
    const CounterId done = jobs.CreateCounter();
    {
        JobBuilder builder(jobs, "serial", JobPriority::Critical, CounterId::Invalid(), done, JobBuilder::Fence::Full);
        for (uint32_t i = 0; i < numbered.size(); ++i) {
            numbered[i] = Numbered{.mRecorder = &recorder, .mId = i};
            REQUIRE(builder.DispatchJob("numbered", &Numbered::Run, &numbered[i]).has_value());
        }
        CHECK(builder.GetDispatchedCount() == 50);
    }
    jobs.WaitForCounter(done);

    REQUIRE(recorder.mOrder.size() == 50);
    for (uint32_t i = 0; i < 50; ++i) {
        CHECK(recorder.mOrder[i] == i);
    }
    jobs.ReleaseCounter(done);
}

TEST_CASE("a parallel builder lets its jobs overlap", "[job][builder]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    Rendezvous first;
    Rendezvous second;
    first.mPeer          = &second.mMine;
    second.mPeer         = &first.mMine;
    const CounterId done = jobs.CreateCounter();
    {
        JobBuilder builder(jobs, "parallel", JobPriority::Critical, CounterId::Invalid(), done,
                           JobBuilder::Fence::None);
        REQUIRE(builder.DispatchJob("first", &Rendezvous::Run, &first).has_value());
        REQUIRE(builder.DispatchJob("second", &Rendezvous::Run, &second).has_value());
    }
    // Would deadlock under a serial fence; two workers run them side by side.
    jobs.WaitForCounter(done);
    CHECK(first.mMine.load());
    CHECK(second.mMine.load());
    jobs.ReleaseCounter(done);
}

TEST_CASE("the parent counter stays pending until the builder's jobs finish", "[job][builder]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    Counter counter;
    const CounterId parent = jobs.CreateCounter();
    const CounterId gate   = jobs.CreateCounter();
    {
        const CompletionDeferral hold(jobs, gate);
        {
            JobBuilder builder(jobs, "gated", JobPriority::Critical, gate, parent, JobBuilder::Fence::None);
            for (int i = 0; i < 10; ++i) {
                REQUIRE(builder.DispatchJob("increment", &Counter::Increment, &counter).has_value());
            }
        }
        // Builder destroyed, gate still closed: nothing has run and the parent is still pending.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(counter.mValue.load() == 0);
        CHECK_FALSE(jobs.IsCounterZero(parent));
    }
    jobs.WaitForCounter(parent);
    CHECK(counter.mValue.load() == 10);
    jobs.ReleaseCounter(parent);
    jobs.ReleaseCounter(gate);
}

TEST_CASE("an empty builder with a parent completes at once", "[job][builder]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    const CounterId parent = jobs.CreateCounter();
    {
        const JobBuilder builder(jobs, "empty", JobPriority::Render, CounterId::Invalid(), parent,
                                 JobBuilder::Fence::Full);
        CHECK_FALSE(jobs.IsCounterZero(parent));
    }
    CHECK(jobs.IsCounterZero(parent));
    jobs.ReleaseCounter(parent);
    CHECK(jobs.GetStats().mInFlightJobs == 0);
}

TEST_CASE("DispatchWait makes later jobs wait for an external counter", "[job][builder]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    Counter before;
    Counter after;
    const CounterId external = jobs.CreateCounter();
    const CounterId done     = jobs.CreateCounter();
    {
        const CompletionDeferral hold(jobs, external);
        JobBuilder builder(jobs, "waiting", JobPriority::Critical, CounterId::Invalid(), done, JobBuilder::Fence::Full);
        REQUIRE(builder.DispatchJob("before", &Counter::Increment, &before).has_value());
        REQUIRE(builder.DispatchWait(external).has_value());
        REQUIRE(builder.DispatchJob("after", &Counter::Increment, &after).has_value());

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (before.mValue.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        CHECK(before.mValue.load() == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(after.mValue.load() == 0);
    }
    jobs.WaitForCounter(done);
    CHECK(after.mValue.load() == 1);
    jobs.ReleaseCounter(done);
    jobs.ReleaseCounter(external);
}

TEST_CASE("ExtractWaitCounter hands out a counter that completes with the work so far", "[job][builder]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    Counter first;
    Counter second;
    JobBuilder builder(jobs, "extract", JobPriority::Critical);
    REQUIRE(builder.DispatchJob("first", &Counter::Increment, &first).has_value());
    const CounterId firstDone = builder.ExtractWaitCounter();
    REQUIRE(firstDone.IsValid());
    REQUIRE(builder.DispatchJob("second", &Counter::Increment, &second).has_value());
    const CounterId secondDone = builder.ExtractWaitCounter();
    REQUIRE(secondDone.IsValid());

    jobs.WaitForCounter(firstDone);
    CHECK(first.mValue.load() == 1);
    jobs.WaitForCounter(secondDone);
    CHECK(second.mValue.load() == 1);
    jobs.ReleaseCounter(firstDone);
    jobs.ReleaseCounter(secondDone);
}

TEST_CASE("a job spawns children through a builder that splices into its completion", "[job][builder]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    struct Spawner {
        Counter mChildren;
        JobSystem* mSystem            = nullptr;
        std::atomic<bool> mDispatchOk = true;

        static void Run(JobContext& context, void* data) {
            auto* const self = static_cast<Spawner*>(data);
            JobBuilder children(*self->mSystem, "children", JobPriority::Critical, CounterId::Invalid(),
                                context.mCompletion, JobBuilder::Fence::None);
            for (int i = 0; i < 8; ++i) {
                if (!children.DispatchJob("child", &Counter::Increment, &self->mChildren).has_value()) {
                    self->mDispatchOk.store(false);
                }
            }
        }
    };
    Spawner spawner;
    spawner.mSystem      = &jobs;
    const CounterId done = jobs.CreateCounter();
    {
        JobBuilder builder(jobs, "outer", JobPriority::Critical, CounterId::Invalid(), done, JobBuilder::Fence::Full);
        REQUIRE(builder.DispatchJob("spawner", &Spawner::Run, &spawner).has_value());
    }
    jobs.WaitForCounter(done);
    CHECK(spawner.mDispatchOk.load());
    CHECK(spawner.mChildren.mValue.load() == 8);
    jobs.ReleaseCounter(done);
    CHECK(jobs.GetStats().mInFlightJobs == 0);
}

TEST_CASE("a fire-and-forget builder leaves nothing in flight once its work is done", "[job][builder]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    Counter counter;
    CounterId last = CounterId::Invalid();
    {
        JobBuilder builder(jobs, "forget", JobPriority::Latent);
        REQUIRE(builder.DispatchJob("a", &Counter::Increment, &counter).has_value());
        REQUIRE(builder.DispatchJob("b", &Counter::Increment, &counter).has_value());
        last = builder.ExtractWaitCounter();
    }
    jobs.WaitForCounter(last);
    jobs.ReleaseCounter(last);
    CHECK(counter.mValue.load() == 2);
    CHECK(jobs.GetStats().mInFlightJobs == 0);
}
