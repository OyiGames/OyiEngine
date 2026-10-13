#include <oyi_core/Expected.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobSystem.h>
#include <oyi_physics/detail/Jolt.h>
#include <oyi_physics/detail/JoltJobSystem.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace oyi::physics::detail {

namespace {

constexpr JPH::uint kSlotPageSize = 64;

} // namespace

JoltJobSystem::JoltJobSystem(job::JobSystem& jobs, uint32_t maxJobs, uint32_t maxBarriers) noexcept
    : JPH::JobSystemWithBarrier(maxBarriers)
    , mJobs(jobs)
    , mWorkerCount(jobs.GetStats().mWorkerCount)
    , mCounter(jobs.CreateCounter()) {
    mSlots.Init(maxJobs, kSlotPageSize);
}

JoltJobSystem::~JoltJobSystem() {
    if (mCounter.IsValid()) {
        mJobs.ReleaseCounter(mCounter);
    }
}

int JoltJobSystem::GetMaxConcurrency() const {
    return static_cast<int>(mWorkerCount) + 1; // the workers plus the thread that steps
}

JoltJobSystem::JobHandle JoltJobSystem::CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                                                  JPH::uint32 dependencies) {
    // Jolt's own pool does the same: a full pool means jobs are in flight and slots free up as
    // they finish on the workers (or inline, when the engine pool is full as well).
    JPH::uint32 index = mSlots.ConstructObject(name, color, this, function, dependencies);
    while (index == JPH::FixedSizeFreeList<Slot>::cInvalidObjectIndex) {
        std::this_thread::yield();
        index = mSlots.ConstructObject(name, color, this, function, dependencies);
    }
    Slot& slot = mSlots.Get(index);
    JobHandle handle(&slot);
    if (dependencies == 0) {
        QueueJob(&slot);
    }
    return handle;
}

void JoltJobSystem::WaitForJobs(Barrier* barrier) {
    if (!job::JobSystem::IsInJob()) {
        mJobs.WaitForCounter(mCounter); // the main thread runs Critical jobs here instead of sleeping
    }
    JPH::JobSystemWithBarrier::WaitForJobs(barrier);
}

JoltJobSystem::Stats JoltJobSystem::GetStats() const noexcept {
    return Stats{.mJobs      = mDispatched.load(std::memory_order_relaxed),
                 .mOnWorkers = mOnWorkers.load(std::memory_order_relaxed),
                 .mInline    = mInline.load(std::memory_order_relaxed)};
}

void JoltJobSystem::QueueJob(Job* job) {
    // Every Job this system hands out is a Slot (CreateJob is the only maker).
    Dispatch(static_cast<Slot*>(job)); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
}

void JoltJobSystem::QueueJobs(Job** jobs, JPH::uint count) {
    for (JPH::uint i = 0; i < count; ++i) {
        QueueJob(jobs[i]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
}

void JoltJobSystem::FreeJob(Job* job) {
    mSlots.DestructObject(static_cast<Slot*>(job)); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
}

void JoltJobSystem::Dispatch(Slot* slot) {
    slot->AddRef(); // released by Run
    mDispatched.fetch_add(1, std::memory_order_relaxed);
    const job::JobDesc desc{
        .mName = slot->mName, .mFunction = &Run, .mData = slot, .mPriority = job::JobPriority::Critical};
    const core::Result queued = mJobs.RunJob(desc, job::CounterId::Invalid(), mCounter);
    if (!queued.has_value()) {
        mInline.fetch_add(1, std::memory_order_relaxed);
        slot->Execute();
        slot->Release();
    }
}

void JoltJobSystem::Run(job::JobContext& context, void* data) {
    auto* const slot = static_cast<Slot*>(data);
    if (context.mWorkerIndex < slot->mOwner->mWorkerCount) {
        slot->mOwner->mOnWorkers.fetch_add(1, std::memory_order_relaxed);
    }
    slot->Execute(); // a no-op when a waiting barrier already ran it
    slot->Release();
}

} // namespace oyi::physics::detail
