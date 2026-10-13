#pragma once

#include <oyi_job/Job.h>
#include <oyi_job/JobSystem.h>
#include <oyi_physics/detail/Jolt.h>

#include <atomic>
#include <cstdint>

namespace oyi::physics::detail {

/// Jolt's JobSystem interface on top of the engine JobSystem (ADR-0009). Every job Jolt queues
/// becomes one Critical engine job named after Jolt's job; barriers stay Jolt's own, but waiting
/// on one first drains our counter so that the main thread helps instead of sleeping.
class JoltJobSystem final : public JPH::JobSystemWithBarrier {
public:
    struct Stats {
        uint32_t mJobs      = 0;
        uint32_t mOnWorkers = 0;
        uint32_t mInline    = 0;
    };

    JoltJobSystem(job::JobSystem& jobs, uint32_t maxJobs, uint32_t maxBarriers) noexcept;
    JoltJobSystem(const JoltJobSystem&)            = delete;
    JoltJobSystem(JoltJobSystem&&)                 = delete;
    JoltJobSystem& operator=(const JoltJobSystem&) = delete;
    JoltJobSystem& operator=(JoltJobSystem&&)      = delete;
    ~JoltJobSystem() override;

    [[nodiscard]] int GetMaxConcurrency() const override;
    [[nodiscard]] JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                                      JPH::uint32 dependencies) override;
    void WaitForJobs(Barrier* barrier) override;

    [[nodiscard]] Stats GetStats() const noexcept;

protected:
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;

private:
    /// A Jolt job plus what the engine job needs: the name for the marker and the owner for the
    /// statistics. Jolt hands back the Job pointer, which is this object.
    struct Slot : Job {
        Slot(const char* name, JPH::ColorArg color, JoltJobSystem* owner, const JobFunction& function,
             JPH::uint32 dependencies) noexcept
            : Job(name, color, owner, function, dependencies), mName(name), mOwner(owner) {}

        const char* mName;
        JoltJobSystem* mOwner;
    };

    static void Run(job::JobContext& context, void* data) noexcept;
    void Dispatch(Slot* slot) noexcept;

    job::JobSystem& mJobs;
    uint32_t mWorkerCount;
    job::CounterId mCounter;
    JPH::FixedSizeFreeList<Slot> mSlots;
    std::atomic<uint32_t> mDispatched{0};
    std::atomic<uint32_t> mOnWorkers{0};
    std::atomic<uint32_t> mInline{0};
};

} // namespace oyi::physics::detail
