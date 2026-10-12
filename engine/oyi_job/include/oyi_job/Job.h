#pragma once

#include <oyi_core/FrameAllocator.h>
#include <oyi_core/Handle.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oyi::job {

/// Semantic priority bands (ADR-0009). There is no inheritance and no preemption: a worker always
/// takes the highest band that has a ready job. A lower band that starves a higher one is a bug in
/// the graph, not something the scheduler repairs.
enum class JobPriority : uint8_t {
    Critical = 0, ///< on the critical path of the current frame; the main thread helps with these
    Render   = 1, ///< feeds the renderer; may trail the simulation
    Latent   = 2, ///< pathfinding, AI, streaming post-processing; budgeted per frame
};

inline constexpr size_t kJobPriorityCount = 3;

struct CounterTag {};

/// Handle to a counter owned by a JobSystem. The only dependency primitive: jobs accumulate into
/// a counter and other jobs (or the main thread) wait for it to reach zero.
using CounterId = core::Handle<CounterTag>;

/// What a running job sees. The frame allocator belongs to the worker and is released by tag at
/// the frame boundary (JobSystem::FreeFrame); never hand its memory to a later frame.
struct JobContext {
    uint32_t mWorkerIndex;                 ///< 0..workerCount-1, or workerCount when the main thread helps
    core::FrameAllocator& mFrameAllocator; ///< scratch memory for this job's frame
    std::string_view mJobName;
};

/// Jobs are plain functions plus a data pointer: no std::function, no captures, nothing that needs
/// a destructor. `data` must stay alive until the job has run.
using JobFunction = void (*)(JobContext& context, void* data);

struct JobDesc {
    std::string_view mName; ///< mandatory; profiler marker and debug name, must outlive the job
    JobFunction mFunction;  ///< nullptr makes an empty job that only splices counters
    void* mData;            ///< passed to mFunction unchanged
    JobPriority mPriority;
};

} // namespace oyi::job
