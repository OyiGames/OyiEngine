#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_core/FrameAllocator.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobBuilder.h>
#include <oyi_job/JobSystem.h>
#include <oyi_job/ParallelFor.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace oyi::job {

namespace {

struct ChunkTask {
    ParallelForFunction mFunction = nullptr;
    void* mData                   = nullptr;
    uint32_t mBegin               = 0;
    uint32_t mEnd                 = 0;
};

struct EpilogueTask {
    ParallelForEpilogue mEpilogue = nullptr;
    void* mData                   = nullptr;
    uint32_t mChunkCount          = 0;
};

void RunChunk(JobContext& context, void* data) {
    const auto* const task = static_cast<const ChunkTask*>(data);
    task->mFunction(context, task->mData, task->mBegin, task->mEnd);
}

void RunEpilogue(JobContext& context, void* data) {
    const auto* const task = static_cast<const EpilogueTask*>(data);
    task->mEpilogue(context, task->mData, task->mChunkCount);
}

} // namespace

core::Result ParallelFor(JobBuilder& builder, std::string_view name, uint32_t count, uint32_t chunkSize,
                         ParallelForFunction function, void* data, ParallelForEpilogue epilogue) noexcept {
    OYI_ASSERT(chunkSize != 0, "ParallelFor: chunk size must not be zero");
    OYI_ASSERT(function != nullptr, "ParallelFor: a chunk function is required");
    JobSystem& system             = builder.GetSystem();
    core::FrameAllocator& scratch = system.GetCurrentFrameAllocator();
    const uint32_t chunkCount     = count == 0 ? 0 : ((count - 1) / chunkSize) + 1;
    const JobPriority priority    = builder.GetPriority();
    const CounterId entry         = builder.GetEntry();

    // Everything the step produces accumulates here; the builder takes it over at the end.
    const CounterId chunksDone = system.CreateCounter();
    if (!chunksDone.IsValid()) {
        return core::Fail(core::ErrorCode::OutOfMemory, "ParallelFor: counter pool exhausted");
    }

    if (chunkCount != 0) {
        void* memory = scratch.Allocate(sizeof(ChunkTask) * chunkCount, alignof(ChunkTask));
        if (memory == nullptr) {
            system.ReleaseCounter(chunksDone);
            return core::Fail(core::ErrorCode::OutOfMemory, "ParallelFor: frame memory exhausted");
        }
        auto* const tasks = static_cast<ChunkTask*>(memory);
        for (uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
            const uint32_t begin = chunk * chunkSize;
            const uint32_t end   = begin + chunkSize < count ? begin + chunkSize : count;
            std::construct_at(tasks + chunk,
                              ChunkTask{.mFunction = function, .mData = data, .mBegin = begin, .mEnd = end});
            const JobDesc desc{.mName = name, .mFunction = &RunChunk, .mData = tasks + chunk, .mPriority = priority};
            const core::Result result = system.RunJob(desc, entry, chunksDone);
            if (!result.has_value()) {
                // Chunks already dispatched complete on their own; the builder keeps order through
                // the counter even though this step is incomplete.
                builder.AdvanceChain(chunksDone);
                return result;
            }
        }
    }

    // The epilogue (or an empty job) turns "all chunks done" into the step's completion, so the
    // chain stays ordered even for zero elements.
    const CounterId stepDone = system.CreateCounter();
    if (!stepDone.IsValid()) {
        builder.AdvanceChain(chunksDone);
        return core::Fail(core::ErrorCode::OutOfMemory, "ParallelFor: counter pool exhausted");
    }
    JobDesc tail{.mName = name, .mFunction = nullptr, .mData = nullptr, .mPriority = priority};
    if (epilogue != nullptr) {
        void* memory = scratch.Allocate(sizeof(EpilogueTask), alignof(EpilogueTask));
        if (memory == nullptr) {
            system.ReleaseCounter(stepDone);
            builder.AdvanceChain(chunksDone);
            return core::Fail(core::ErrorCode::OutOfMemory, "ParallelFor: frame memory exhausted");
        }
        auto* const task =
            std::construct_at(static_cast<EpilogueTask*>(memory),
                              EpilogueTask{.mEpilogue = epilogue, .mData = data, .mChunkCount = chunkCount});
        tail.mFunction = &RunEpilogue;
        tail.mData     = task;
    }
    const core::Result result = system.RunJob(tail, chunksDone, stepDone);
    if (!result.has_value()) {
        system.ReleaseCounter(stepDone);
        builder.AdvanceChain(chunksDone);
        return result;
    }
    system.ReleaseCounter(chunksDone);
    builder.AdvanceChain(stepDone);
    return {};
}

} // namespace oyi::job
