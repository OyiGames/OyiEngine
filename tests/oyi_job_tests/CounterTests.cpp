#include <oyi_core/SystemAllocator.h>
#include <oyi_job/CompletionDeferral.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobSystem.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace {

using oyi::core::SystemAllocator;
using oyi::job::CompletionDeferral;
using oyi::job::CounterId;
using oyi::job::JobContext;
using oyi::job::JobDesc;
using oyi::job::JobPriority;
using oyi::job::JobSystem;

constexpr JobSystem::Config kTwoWorkers{.mWorkerCount = 2, .mMaxJobs = 64, .mMaxCounters = 8, .mSpinIterations = 8};

void NoOp(JobContext& /*context*/, void* /*data*/) {}

} // namespace

TEST_CASE("a new counter is zero and valid, a released one is stale", "[job][counter]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);

    const CounterId counter = jobs.CreateCounter();
    REQUIRE(counter.IsValid());
    CHECK(jobs.IsCounterValid(counter));
    CHECK(jobs.IsCounterZero(counter));

    jobs.ReleaseCounter(counter);
    CHECK_FALSE(jobs.IsCounterValid(counter));

    const CounterId reused = jobs.CreateCounter();
    REQUIRE(reused.IsValid());
    CHECK(reused != counter);
    CHECK_FALSE(jobs.IsCounterValid(counter));
    jobs.ReleaseCounter(reused);
}

TEST_CASE("the counter pool reports exhaustion with an invalid handle", "[job][counter]") {
    SystemAllocator allocator;
    constexpr JobSystem::Config kTwoCounters{.mWorkerCount = 1, .mMaxJobs = 8, .mMaxCounters = 2, .mSpinIterations = 8};
    JobSystem jobs(allocator, kTwoCounters);

    const CounterId first  = jobs.CreateCounter();
    const CounterId second = jobs.CreateCounter();
    REQUIRE(first.IsValid());
    REQUIRE(second.IsValid());
    CHECK_FALSE(jobs.CreateCounter().IsValid());

    jobs.ReleaseCounter(first);
    const CounterId third = jobs.CreateCounter();
    CHECK(third.IsValid());
    jobs.ReleaseCounter(second);
    jobs.ReleaseCounter(third);
}

TEST_CASE("a CompletionDeferral keeps a counter pending for its scope", "[job][counter]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    const CounterId counter = jobs.CreateCounter();
    {
        const CompletionDeferral deferral(jobs, counter);
        CHECK(deferral.GetCounter() == counter);
        CHECK_FALSE(jobs.IsCounterZero(counter));
    }
    CHECK(jobs.IsCounterZero(counter));
    jobs.WaitForCounter(counter);
    jobs.ReleaseCounter(counter);
}

TEST_CASE("a released counter stays alive until its last pending job completes", "[job][counter]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    const CounterId gate   = jobs.CreateCounter();
    const CounterId target = jobs.CreateCounter();

    {
        const CompletionDeferral hold(jobs, gate);
        REQUIRE(jobs.RunJob(JobDesc{.mName     = "released-target",
                                    .mFunction = &NoOp,
                                    .mData     = nullptr,
                                    .mPriority = JobPriority::Critical},
                            gate, target)
                    .has_value());
        jobs.ReleaseCounter(target);
        // The pending job still references the counter.
        CHECK(jobs.IsCounterValid(target));
    }

    // Once the gate opens the job runs, completes and drops the last reference.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (jobs.IsCounterValid(target) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK_FALSE(jobs.IsCounterValid(target));
    jobs.WaitForCounter(gate);
    jobs.ReleaseCounter(gate);
    CHECK(jobs.GetStats().mInFlightJobs == 0);
}
