#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobBuilder.h>
#include <oyi_job/JobSystem.h>

#include <string_view>

namespace oyi::job {

JobBuilder::JobBuilder(JobSystem& system, std::string_view name, JobPriority priority, CounterId waitFor,
                       CounterId parent, Fence fence) noexcept
    : mSystem(system), mName(name), mPriority(priority), mFence(fence), mParent(parent), mEntry(waitFor) {
    OYI_ASSERT(!mName.empty(), "every builder needs a name");
    if (mParent.IsValid()) {
        // The parent stays pending while jobs are being registered; the destructor hands this
        // hold over to the splice job.
        mSystem.IncrementCounter(mParent);
    }
}

JobBuilder::JobBuilder(JobSystem& system, std::string_view name, JobPriority priority) noexcept
    : JobBuilder(system, name, priority, CounterId::Invalid(), CounterId::Invalid(), Fence::Full) {}

JobBuilder::~JobBuilder() {
    if (mParent.IsValid()) {
        const CounterId soFar = CompletionSoFar();
        if (soFar.IsValid()) {
            const core::Result spliced = Splice(soFar, mParent);
            OYI_CHECK(spliced.has_value(), "JobBuilder: job pool exhausted while splicing into the parent");
        }
        mSystem.DecrementCounter(mParent);
    }
    SetTail(CounterId::Invalid());
    SetEntry(CounterId::Invalid(), false);
}

CounterId JobBuilder::CompletionSoFar() const noexcept {
    // The tail covers everything: in serial mode it is the last job, in parallel mode every job
    // accumulates into it, and after DispatchWait the new tail (or the owned entry) waits for all
    // earlier work through its splices.
    if (mTail.IsValid()) {
        return mTail;
    }
    return mOwnsEntry ? mEntry : CounterId::Invalid();
}

core::Result JobBuilder::Splice(CounterId waitFor, CounterId accumulate) noexcept {
    const JobDesc splice{.mName = mName, .mFunction = nullptr, .mData = nullptr, .mPriority = mPriority};
    return mSystem.RunJob(splice, waitFor, accumulate);
}

void JobBuilder::SetEntry(CounterId entry, bool owned) noexcept {
    // Jobs waiting on the old entry are flushed before the counter is recycled (JobSystem
    // contract), so dropping the reference is safe while the chain is still running.
    if (mOwnsEntry) {
        mSystem.ReleaseCounter(mEntry);
    }
    mEntry     = entry;
    mOwnsEntry = owned;
}

void JobBuilder::SetTail(CounterId tail) noexcept {
    if (mTail.IsValid()) {
        mSystem.ReleaseCounter(mTail);
    }
    mTail = tail;
}

void JobBuilder::AdvanceChain(CounterId completion) noexcept {
    ++mDispatched;
    if (mFence == Fence::Full) {
        // The step's completion becomes both the tail and what the next job waits for. The tail
        // owns it; the entry is the same counter, so it must not own it as well.
        SetEntry(completion, false);
        SetTail(completion);
        return;
    }
    if (!mTail.IsValid()) {
        mTail = completion;
        return;
    }
    const core::Result joined = Splice(completion, mTail);
    OYI_CHECK(joined.has_value(), "JobBuilder: job pool exhausted while joining a parallel step");
    mSystem.ReleaseCounter(completion);
}

core::Result JobBuilder::DispatchJob(std::string_view name, JobFunction function, void* data) noexcept {
    const JobDesc desc{.mName = name, .mFunction = function, .mData = data, .mPriority = mPriority};
    if (mFence == Fence::None) {
        // Parallel jobs share one accumulate target; none of them waits for another.
        if (!mTail.IsValid()) {
            mTail = mSystem.CreateCounter();
            if (!mTail.IsValid()) {
                return core::Fail(core::ErrorCode::OutOfMemory, "JobBuilder: counter pool exhausted");
            }
        }
        const core::Result result = mSystem.RunJob(desc, mEntry, mTail);
        if (result.has_value()) {
            ++mDispatched;
        }
        return result;
    }
    const CounterId completion = mSystem.CreateCounter();
    if (!completion.IsValid()) {
        return core::Fail(core::ErrorCode::OutOfMemory, "JobBuilder: counter pool exhausted");
    }
    const core::Result result = mSystem.RunJob(desc, mEntry, completion);
    if (!result.has_value()) {
        mSystem.ReleaseCounter(completion);
        return result;
    }
    AdvanceChain(completion);
    return {};
}

core::Result JobBuilder::DispatchWait(CounterId counter) noexcept {
    OYI_ASSERT(mSystem.IsCounterValid(counter), "DispatchWait on an invalid counter");
    const CounterId joined = mSystem.CreateCounter();
    if (!joined.IsValid()) {
        return core::Fail(core::ErrorCode::OutOfMemory, "JobBuilder: counter pool exhausted");
    }
    // Two empty jobs feed the new counter: one for the work so far (or the initial dependency),
    // one for the external counter.
    const CounterId soFar = mTail.IsValid() ? mTail : mEntry;
    if (soFar.IsValid()) {
        const core::Result result = Splice(soFar, joined);
        if (!result.has_value()) {
            mSystem.ReleaseCounter(joined);
            return result;
        }
    }
    const core::Result result = Splice(counter, joined);
    if (!result.has_value()) {
        // The first splice already accumulates into `joined`, so adopting it keeps the chain
        // consistent; only the new dependency is lost, which the caller learns from the result.
        AdvanceChain(joined);
        return result;
    }
    if (mFence == Fence::Full) {
        SetEntry(joined, false);
        SetTail(joined);
        return {};
    }
    // Parallel: later jobs wait for `joined` and accumulate into a fresh tail, so the wait target
    // and the accumulate target never coincide. The old tail is covered by `joined`.
    SetEntry(joined, true);
    SetTail(CounterId::Invalid());
    return {};
}

CounterId JobBuilder::ExtractWaitCounter() noexcept {
    const CounterId extracted = mSystem.CreateCounter();
    if (!extracted.IsValid()) {
        return CounterId::Invalid();
    }
    const CounterId soFar = CompletionSoFar();
    if (!soFar.IsValid()) {
        // Nothing dispatched yet: the extracted counter is already zero.
        return extracted;
    }
    const core::Result result = Splice(soFar, extracted);
    if (!result.has_value()) {
        mSystem.ReleaseCounter(extracted);
        return CounterId::Invalid();
    }
    return extracted;
}

} // namespace oyi::job
