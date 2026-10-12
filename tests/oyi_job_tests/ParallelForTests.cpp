#include <oyi_core/Memory.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobBuilder.h>
#include <oyi_job/JobSystem.h>
#include <oyi_job/ParallelFor.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

namespace {

using oyi::core::MemoryTag;
using oyi::core::SystemAllocator;
using oyi::job::CounterId;
using oyi::job::JobBuilder;
using oyi::job::JobContext;
using oyi::job::JobPriority;
using oyi::job::JobSystem;
using oyi::job::ParallelFor;

constexpr JobSystem::Config kTwoWorkers{.mWorkerCount = 2, .mMaxJobs = 1024, .mMaxCounters = 256, .mSpinIterations = 8};

struct Touch {
    std::vector<std::atomic<uint32_t>> mHits;
    std::vector<uint64_t> mChunkSums; ///< one slot per chunk, written by that chunk only
    std::atomic<uint32_t> mEpilogueChunks = 0xFFFF'FFFFu;
    uint64_t mTotal                       = 0;
    uint32_t mChunkSize                   = 0;

    explicit Touch(uint32_t count, uint32_t chunkSize)
        : mHits(count), mChunkSums(count == 0 ? 0 : ((count - 1) / chunkSize) + 1), mChunkSize(chunkSize) {}

    static void Chunk(JobContext& /*context*/, void* data, uint32_t begin, uint32_t end) {
        auto* const self = static_cast<Touch*>(data);
        uint64_t sum     = 0;
        for (uint32_t i = begin; i < end; ++i) {
            self->mHits[i].fetch_add(1);
            sum += i;
        }
        self->mChunkSums[begin / self->mChunkSize] = sum;
    }

    static void Epilogue(JobContext& /*context*/, void* data, uint32_t chunkCount) {
        auto* const self = static_cast<Touch*>(data);
        self->mEpilogueChunks.store(chunkCount);
        for (uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
            self->mTotal += self->mChunkSums[chunk];
        }
    }
};

struct Marker {
    std::atomic<uint32_t> mValue = 0;

    static void Set(JobContext& /*context*/, void* data) { static_cast<Marker*>(data)->mValue.store(1); }
};

struct OrderedChunks {
    Marker* mBefore                  = nullptr;
    std::atomic<uint32_t> mSawBefore = 0;
    std::atomic<uint32_t> mChunksRun = 0;
    uint32_t mChunksAtEpilogue       = 0;

    static void Chunk(JobContext& /*context*/, void* data, uint32_t /*begin*/, uint32_t /*end*/) {
        auto* const self = static_cast<OrderedChunks*>(data);
        self->mSawBefore.fetch_add(self->mBefore->mValue.load());
        self->mChunksRun.fetch_add(1);
    }

    static void Epilogue(JobContext& /*context*/, void* data, uint32_t /*chunkCount*/) {
        auto* const self        = static_cast<OrderedChunks*>(data);
        self->mChunksAtEpilogue = self->mChunksRun.load();
    }
};

} // namespace

TEST_CASE("ParallelFor touches every element once and reduces per chunk", "[job][parallelfor]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    jobs.BeginFrame(MemoryTag::ForFrame(1));
    constexpr uint32_t kCount     = 1000;
    constexpr uint32_t kChunkSize = 64;
    Touch touch(kCount, kChunkSize);
    const CounterId done = jobs.CreateCounter();
    {
        JobBuilder builder(jobs, "touch", JobPriority::Critical, CounterId::Invalid(), done, JobBuilder::Fence::Full);
        REQUIRE(ParallelFor(builder, "touch", kCount, kChunkSize, &Touch::Chunk, &touch, &Touch::Epilogue).has_value());
        CHECK(builder.GetDispatchedCount() == 1);
    }
    jobs.WaitForCounter(done);

    for (const auto& hit : touch.mHits) {
        CHECK(hit.load() == 1);
    }
    CHECK(touch.mEpilogueChunks.load() == 16);
    CHECK(touch.mTotal == uint64_t{kCount} * (kCount - 1) / 2);
    jobs.ReleaseCounter(done);
    jobs.FreeFrame(MemoryTag::ForFrame(1));
    CHECK(jobs.GetFrameHeapStats().mUsedBlocks == 0);
}

TEST_CASE("ParallelFor with zero elements still runs the epilogue", "[job][parallelfor]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    Touch touch(0, 8);
    const CounterId done = jobs.CreateCounter();
    {
        JobBuilder builder(jobs, "empty", JobPriority::Critical, CounterId::Invalid(), done, JobBuilder::Fence::Full);
        REQUIRE(ParallelFor(builder, "empty", 0, 8, &Touch::Chunk, &touch, &Touch::Epilogue).has_value());
    }
    jobs.WaitForCounter(done);
    CHECK(touch.mEpilogueChunks.load() == 0);
    jobs.ReleaseCounter(done);
}

TEST_CASE("ParallelFor without an epilogue keeps the chain ordered, even with zero elements", "[job][parallelfor]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    Marker before;
    Marker after;
    OrderedChunks ordered;
    ordered.mBefore      = &before;
    const CounterId done = jobs.CreateCounter();
    {
        JobBuilder builder(jobs, "ordered", JobPriority::Critical, CounterId::Invalid(), done, JobBuilder::Fence::Full);
        REQUIRE(builder.DispatchJob("before", &Marker::Set, &before).has_value());
        REQUIRE(ParallelFor(builder, "none", 0, 4, &OrderedChunks::Chunk, &ordered, nullptr).has_value());
        REQUIRE(ParallelFor(builder, "chunks", 100, 7, &OrderedChunks::Chunk, &ordered, &OrderedChunks::Epilogue)
                    .has_value());
        REQUIRE(builder.DispatchJob("after", &Marker::Set, &after).has_value());
        CHECK(builder.GetDispatchedCount() == 4);
    }
    jobs.WaitForCounter(done);

    CHECK(ordered.mChunksRun.load() == 15);
    CHECK(ordered.mSawBefore.load() == 15); // every chunk ran after "before"
    CHECK(ordered.mChunksAtEpilogue == 15); // the epilogue ran after every chunk
    CHECK(after.mValue.load() == 1);
    jobs.ReleaseCounter(done);
}

TEST_CASE("ParallelFor can be issued from inside a job", "[job][parallelfor]") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, kTwoWorkers);
    struct Nested {
        Touch mTouch{500, 32};
        JobSystem* mSystem            = nullptr;
        std::atomic<bool> mDispatchOk = false;

        static void Run(JobContext& context, void* data) {
            auto* const self = static_cast<Nested*>(data);
            JobBuilder inner(*self->mSystem, "inner", JobPriority::Critical, CounterId::Invalid(), context.mCompletion,
                             JobBuilder::Fence::Full);
            self->mDispatchOk.store(
                ParallelFor(inner, "inner-touch", 500, 32, &Touch::Chunk, &self->mTouch, &Touch::Epilogue).has_value());
        }
    };
    Nested nested;
    nested.mSystem       = &jobs;
    const CounterId done = jobs.CreateCounter();
    {
        JobBuilder outer(jobs, "outer", JobPriority::Critical, CounterId::Invalid(), done, JobBuilder::Fence::Full);
        REQUIRE(outer.DispatchJob("nested", &Nested::Run, &nested).has_value());
    }
    jobs.WaitForCounter(done);
    CHECK(nested.mDispatchOk.load());
    CHECK(nested.mTouch.mEpilogueChunks.load() == 16);
    CHECK(nested.mTouch.mTotal == uint64_t{500} * 499 / 2);
    jobs.ReleaseCounter(done);
    CHECK(jobs.GetStats().mInFlightJobs == 0);
}
