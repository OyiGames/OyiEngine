#pragma once

#include <oyi_job/Job.h>
#include <oyi_job/JobSystem.h>

namespace oyi::job {

/// Keeps a counter pending for the lifetime of the guard, for work that finishes outside the job
/// graph (a loading thread, an I/O callback) or to hold a counter open while its jobs are being
/// registered. Pairs IncrementCounter with DecrementCounter so neither can be forgotten.
class CompletionDeferral {
public:
    CompletionDeferral(JobSystem& system, CounterId counter) noexcept : mSystem(system), mCounter(counter) {
        mSystem.IncrementCounter(mCounter);
    }
    CompletionDeferral(const CompletionDeferral&)            = delete;
    CompletionDeferral(CompletionDeferral&&)                 = delete;
    CompletionDeferral& operator=(const CompletionDeferral&) = delete;
    CompletionDeferral& operator=(CompletionDeferral&&)      = delete;
    ~CompletionDeferral() { mSystem.DecrementCounter(mCounter); }

    [[nodiscard]] CounterId GetCounter() const noexcept { return mCounter; }

private:
    JobSystem& mSystem;
    CounterId mCounter;
};

} // namespace oyi::job
