#pragma once

#include <oyi_core/Expected.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobBuilder.h>

#include <cstdint>
#include <string_view>

namespace oyi::job {

/// Processes elements [begin, end) of one chunk.
using ParallelForFunction = void (*)(JobContext& context, void* data, uint32_t begin, uint32_t end);

/// Runs once after every chunk has finished, with the number of chunks that ran (possibly zero).
/// Reductions merge per-chunk partials here, in chunk order, so the result does not depend on
/// how many workers there were.
using ParallelForEpilogue = void (*)(JobContext& context, void* data, uint32_t chunkCount);

/// Dispatches `count` elements in chunks of `chunkSize` as one step of `builder`: the chunks run
/// in parallel with each other, the epilogue runs after all of them, and the step as a whole obeys
/// the builder's fence. The split depends only on count and chunkSize, never on the worker count.
/// Zero elements still dispatch the epilogue (or an empty job) so the chain keeps its order.
/// Chunk bookkeeping lives in the calling thread's frame memory.
[[nodiscard]] core::Result ParallelFor(JobBuilder& builder, std::string_view name, uint32_t count, uint32_t chunkSize,
                                       ParallelForFunction function, void* data, ParallelForEpilogue epilogue) noexcept;

} // namespace oyi::job
