#include <oyi_platform/Clock.h>
#include <oyi_platform/Thread.h>
#include <oyi_platform/Topology.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <thread>

namespace {

using Catch::Approx;
using oyi::platform::CoreTopology;
using oyi::platform::CurrentThreadId;
using oyi::platform::FixedStepper;
using oyi::platform::GetCurrentThreadName;
using oyi::platform::Now;
using oyi::platform::QueryCoreTopology;
using oyi::platform::RecommendedWorkerCount;
using oyi::platform::SecondsBetween;
using oyi::platform::SetCurrentThreadName;
using oyi::platform::TimePoint;
using oyi::platform::UtcMilliseconds;

} // namespace

TEST_CASE("the core topology is consistent with itself", "[platform][topology]") {
    const CoreTopology topology = QueryCoreTopology();
    CHECK(topology.mLogicalCores >= 1);
    CHECK(topology.mPhysicalCores >= 1);
    CHECK(topology.mPhysicalCores <= topology.mLogicalCores);
    CHECK(topology.mPerformanceCores >= 1);
    if (topology.mHybrid) {
        CHECK(topology.mEfficiencyCores >= 1);
        CHECK(topology.mPerformanceCores + topology.mEfficiencyCores == topology.mPhysicalCores);
    } else {
        CHECK(topology.mPerformanceCores == topology.mPhysicalCores);
        CHECK(topology.mEfficiencyCores == 0);
    }
    const uint32_t workers = RecommendedWorkerCount(topology);
    CHECK(workers >= 1);
    CHECK(workers <= topology.mLogicalCores);
}

TEST_CASE("the recommended worker count leaves a big core to the main thread", "[platform][topology]") {
    CHECK(RecommendedWorkerCount(CoreTopology{.mLogicalCores     = 8,
                                              .mPhysicalCores    = 8,
                                              .mPerformanceCores = 8,
                                              .mEfficiencyCores  = 0,
                                              .mHybrid           = false}) == 7);
    CHECK(
        RecommendedWorkerCount(CoreTopology{
            .mLogicalCores = 8, .mPhysicalCores = 8, .mPerformanceCores = 4, .mEfficiencyCores = 4, .mHybrid = true}) ==
        5);
    CHECK(RecommendedWorkerCount(CoreTopology{.mLogicalCores     = 1,
                                              .mPhysicalCores    = 1,
                                              .mPerformanceCores = 1,
                                              .mEfficiencyCores  = 0,
                                              .mHybrid           = false}) == 1);
    CHECK(
        RecommendedWorkerCount(CoreTopology{
            .mLogicalCores = 6, .mPhysicalCores = 6, .mPerformanceCores = 2, .mEfficiencyCores = 4, .mHybrid = true}) ==
        3);
}

TEST_CASE("a thread can be named and asked for its name and id", "[platform][thread]") {
    SetCurrentThreadName("oyi.test.main");
    std::array<char, 64> name{};
    const uint32_t length = GetCurrentThreadName(name);
    CHECK(std::string_view(name.data(), length) == "oyi.test.main");

    const uint64_t mainId = CurrentThreadId();
    CHECK(mainId != 0);
    CHECK(CurrentThreadId() == mainId);
    uint64_t workerId         = 0;
    uint32_t workerNameLength = 0;
    std::array<char, 64> workerName{};
    std::thread worker([&] {
        SetCurrentThreadName("oyi.test.worker");
        workerId         = CurrentThreadId();
        workerNameLength = GetCurrentThreadName(workerName);
    });
    worker.join();
    CHECK(workerId != 0);
    CHECK(workerId != mainId);
    CHECK(std::string_view(workerName.data(), workerNameLength) == "oyi.test.worker");
    // The main thread keeps its own name.
    CHECK(std::string_view(name.data(), GetCurrentThreadName(name)) == "oyi.test.main");
}

TEST_CASE("the monotonic clock moves forward and the wall clock is past 2020", "[platform][clock]") {
    const TimePoint start = Now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const TimePoint later = Now();
    CHECK(SecondsBetween(start, later) >= 0.001);
    CHECK(SecondsBetween(later, start) <= -0.001);
    CHECK(SecondsBetween(start, start) == 0.0);
    CHECK(UtcMilliseconds() > int64_t{1'577'836'800'000}); // 2020-01-01T00:00:00Z
}

TEST_CASE("the fixed stepper converts elapsed time into bounded steps", "[platform][clock]") {
    FixedStepper stepper;
    CHECK(stepper.GetConfig().mStepSeconds == Approx(1.0 / 30.0));
    CHECK(stepper.Advance(0.1) == 3);
    CHECK(stepper.GetAlpha() == Approx(0.0).margin(1e-6));
    CHECK(stepper.Advance(0.05) == 1);
    CHECK(stepper.GetAlpha() == Approx(0.5).margin(1e-6));
    CHECK(stepper.GetTick() == 4);
    CHECK(stepper.Advance(0.0) == 0);
    CHECK(stepper.Advance(-1.0) == 0);
    // A stall: at most four steps, the rest is dropped, the fraction survives.
    CHECK(stepper.Advance(1.0) == 4);
    CHECK(stepper.GetDroppedSteps() == 26);
    CHECK(stepper.GetTick() == 8);
    CHECK(stepper.GetAlpha() >= 0.0);
    CHECK(stepper.GetAlpha() < 1.0);
    stepper.Reset();
    CHECK(stepper.GetTick() == 0);
    CHECK(stepper.GetAlpha() == 0.0);

    FixedStepper fast(FixedStepper::Config{.mStepSeconds = 0.5, .mMaxStepsPerFrame = 1});
    CHECK(fast.Advance(1.75) == 1);
    CHECK(fast.GetDroppedSteps() == 2);
    CHECK(fast.GetAlpha() == Approx(0.5));
}
