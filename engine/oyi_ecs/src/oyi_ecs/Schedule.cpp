#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Query.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_job/CompletionDeferral.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobSystem.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

namespace {

/// The query needs everything the system touches, read or write.
[[nodiscard]] ComponentMask Touched(const SystemDesc& desc) noexcept {
    ComponentMask all = desc.mRead;
    desc.mWrite.ForEach([&all](ComponentTypeIndex type) { all.Set(type); });
    return all;
}

[[nodiscard]] bool Conflicts(const SystemDesc& a, const SystemDesc& b) noexcept {
    return a.mWrite.Intersects(b.mWrite) || a.mWrite.Intersects(b.mRead) || b.mWrite.Intersects(a.mRead);
}

} // namespace

struct Schedule::SystemRecord {
    SystemRecord(Schedule& owner, const SystemDesc& desc, core::Allocator& allocator,
                 std::pmr::memory_resource* resource) noexcept
        : mOwner(owner)
        , mDesc(desc)
        , mQuery(allocator, Touched(desc), desc.mExclude)
        , mCommands(allocator)
        , mPredecessors(resource) {}

    Schedule& mOwner;
    SystemDesc mDesc;
    Query mQuery;
    CommandBuffer mCommands;
    std::pmr::vector<SystemId> mPredecessors;
    uint32_t mOrderIndex  = 0; ///< position within the phase after Build
    uint32_t mLastVersion = 0;
    uint32_t mRunVersion  = 0; ///< version assigned for the run in progress
    job::CounterId mDone;      ///< completion counter for the run in progress
};

Schedule::Schedule(core::Allocator& allocator, World& world) noexcept
    : mAllocator(allocator)
    , mWorld(world)
    , mResource(allocator)
    , mSystems(&mResource)
    , mOrder{std::pmr::vector<SystemId>(&mResource), std::pmr::vector<SystemId>(&mResource),
             std::pmr::vector<SystemId>(&mResource), std::pmr::vector<SystemId>(&mResource),
             std::pmr::vector<SystemId>(&mResource), std::pmr::vector<SystemId>(&mResource),
             std::pmr::vector<SystemId>(&mResource), std::pmr::vector<SystemId>(&mResource)}
    , mPhaseBuffers(&mResource) {}

Schedule::~Schedule() {
    for (SystemRecord* const record : mSystems) {
        mAllocator.Delete(record);
    }
}

// --- registration -----------------------------------------------------------------------------

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<SystemId> Schedule::AddSystem(const SystemDesc& desc) noexcept {
    if (mBuilt) {
        return core::Fail(core::ErrorCode::InvalidState, "Schedule: systems cannot be added after Build");
    }
    if (desc.mName.empty() || desc.mFunction == nullptr) {
        return core::Fail(core::ErrorCode::InvalidArgument, "Schedule: a system needs a name and a function");
    }
    if (static_cast<uint32_t>(desc.mPhase) >= kPhaseCount) {
        return core::Fail(core::ErrorCode::InvalidArgument, "Schedule: unknown phase");
    }
    auto* const record = mAllocator.New<SystemRecord>(*this, desc, mAllocator, &mResource);
    if (record == nullptr) {
        return core::Fail(core::ErrorCode::OutOfMemory, "Schedule: cannot allocate a system record");
    }
    mSystems.push_back(record);
    return SystemId{.mValue = static_cast<uint32_t>(mSystems.size() - 1)};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result Schedule::AddOrder(SystemId before, SystemId after) noexcept {
    if (mBuilt) {
        return core::Fail(core::ErrorCode::InvalidState, "Schedule: order cannot change after Build");
    }
    if (before.mValue >= mSystems.size() || after.mValue >= mSystems.size() || before == after) {
        return core::Fail(core::ErrorCode::InvalidArgument, "Schedule: AddOrder needs two distinct registered systems");
    }
    if (mSystems[before.mValue]->mDesc.mPhase != mSystems[after.mValue]->mDesc.mPhase) {
        return core::Fail(core::ErrorCode::InvalidArgument,
                          "Schedule: order edges only exist within a phase; phases are already ordered");
    }
    mSystems[after.mValue]->mPredecessors.push_back(before);
    return {};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result Schedule::Build() noexcept {
    if (mBuilt) {
        return core::Fail(core::ErrorCode::InvalidState, "Schedule: already built");
    }
    const auto count = static_cast<uint32_t>(mSystems.size());
    // reach[a * count + b] != 0 when a is ordered before b through explicit edges.
    std::pmr::vector<uint8_t> reach(static_cast<size_t>(count) * count, uint8_t{0}, &mResource);
    std::pmr::vector<uint32_t> indegree(count, 0, &mResource);
    std::pmr::vector<SystemId> ready(&mResource);
    for (uint32_t s = 0; s < count; ++s) {
        indegree[s] = static_cast<uint32_t>(mSystems[s]->mPredecessors.size());
    }

    for (uint32_t phase = 0; phase < kPhaseCount; ++phase) {
        std::pmr::vector<SystemId>& order = mOrder[phase];
        order.clear();
        // Kahn's algorithm; registration order breaks ties so the result is stable.
        ready.clear();
        for (uint32_t s = 0; s < count; ++s) {
            if (static_cast<uint32_t>(mSystems[s]->mDesc.mPhase) == phase && indegree[s] == 0) {
                ready.push_back(SystemId{.mValue = s});
            }
        }
        while (!ready.empty()) {
            // Take the lowest registration index among the ready systems.
            size_t best = 0;
            for (size_t i = 1; i < ready.size(); ++i) {
                if (ready[i].mValue < ready[best].mValue) {
                    best = i;
                }
            }
            const SystemId current = ready[best];
            ready[best]            = ready.back();
            ready.pop_back();
            mSystems[current.mValue]->mOrderIndex = static_cast<uint32_t>(order.size());
            order.push_back(current);
            for (uint32_t s = 0; s < count; ++s) {
                const SystemRecord& candidate = *mSystems[s];
                if (static_cast<uint32_t>(candidate.mDesc.mPhase) != phase) {
                    continue;
                }
                for (const SystemId predecessor : candidate.mPredecessors) {
                    if (predecessor != current) {
                        continue;
                    }
                    // current precedes s, and so does everything that precedes current.
                    reach[(static_cast<size_t>(current.mValue) * count) + s] = 1;
                    for (uint32_t t = 0; t < count; ++t) {
                        if (reach[(static_cast<size_t>(t) * count) + current.mValue] != 0) {
                            reach[(static_cast<size_t>(t) * count) + s] = 1;
                        }
                    }
                    if (--indegree[s] == 0) {
                        ready.push_back(SystemId{.mValue = s});
                    }
                }
            }
        }
        uint32_t inPhase = 0;
        for (uint32_t s = 0; s < count; ++s) {
            inPhase += static_cast<uint32_t>(mSystems[s]->mDesc.mPhase) == phase ? 1 : 0;
        }
        if (order.size() != inPhase) {
            for (std::pmr::vector<SystemId>& cleared : mOrder) {
                cleared.clear();
            }
            return core::Fail(core::ErrorCode::InvalidState, "Schedule: the order edges form a cycle");
        }
        for (size_t i = 0; i < order.size(); ++i) {
            for (size_t j = i + 1; j < order.size(); ++j) {
                const uint32_t a = order[i].mValue;
                const uint32_t b = order[j].mValue;
                if (!Conflicts(mSystems[a]->mDesc, mSystems[b]->mDesc)) {
                    continue;
                }
                if (reach[(static_cast<size_t>(a) * count) + b] == 0 &&
                    reach[(static_cast<size_t>(b) * count) + a] == 0) {
                    mLastConflict = Conflict{.mFirst = SystemId{.mValue = a}, .mSecond = SystemId{.mValue = b}};
                    for (std::pmr::vector<SystemId>& cleared : mOrder) {
                        cleared.clear();
                    }
                    return core::Fail(core::ErrorCode::InvalidState,
                                      "Schedule: two systems touch the same component without an explicit order");
                }
            }
        }
    }
    mBuilt = true;
    return {};
}

const SystemDesc& Schedule::GetDesc(SystemId system) const noexcept {
    OYI_ASSERT(system.mValue < mSystems.size(), "Schedule: unknown system");
    return mSystems[system.mValue]->mDesc;
}

std::span<const SystemId> Schedule::GetOrder(Phase phase) const noexcept {
    return mOrder[static_cast<size_t>(phase)];
}

std::span<const SystemId> Schedule::GetPredecessors(SystemId system) const noexcept {
    OYI_ASSERT(system.mValue < mSystems.size(), "Schedule: unknown system");
    return mSystems[system.mValue]->mPredecessors;
}

// --- execution --------------------------------------------------------------------------------

void Schedule::RefreshQueries(Phase phase) noexcept {
    // Query caches are refreshed on the calling thread so that the jobs only read them.
    for (const SystemId system : mOrder[static_cast<size_t>(phase)]) {
        (void)mSystems[system.mValue]->mQuery.GetMatches(mWorld);
    }
}

void Schedule::RunSystem(SystemRecord& record, uint32_t version, core::Allocator& scratch) noexcept {
    SystemContext context(mWorld, record.mQuery, record.mCommands, record.mDesc.mRead, record.mDesc.mWrite, version,
                          record.mLastVersion, record.mDesc.mData, scratch);
    record.mDesc.mFunction(context);
    record.mLastVersion = version;
}

void Schedule::RunSystemJob(job::JobContext& context, void* data) {
    auto* const record = static_cast<SystemRecord*>(data);
    record->mOwner.RunSystem(*record, record->mRunVersion, context.mFrameAllocator);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result Schedule::EndPhase(Phase phase, uint32_t versionsUsed) noexcept {
    mPhaseBuffers.clear();
    for (const SystemId system : mOrder[static_cast<size_t>(phase)]) {
        SystemRecord& record = *mSystems[system.mValue];
        record.mCommands.SetSortKey(record.mOrderIndex);
        mPhaseBuffers.push_back(&record.mCommands);
    }
    // Every system of the phase used its own version; move the world past all of them before
    // the sync point stamps its own writes.
    mWorld.AdvanceVersion(versionsUsed);
    mLastReplay = mWorld.SyncPoint(std::span<CommandBuffer* const>{mPhaseBuffers});
    return {};
}

core::Result Schedule::RunPhaseSerial(Phase phase) noexcept {
    if (!mBuilt) {
        return core::Fail(core::ErrorCode::InvalidState, "Schedule: Build before running");
    }
    RefreshQueries(phase);
    const uint32_t base = mWorld.GetVersion();
    uint32_t used       = 0;
    for (const SystemId system : mOrder[static_cast<size_t>(phase)]) {
        SystemRecord& record = *mSystems[system.mValue];
        ++used;
        RunSystem(record, base + used, core::GetSystemAllocator());
    }
    return EndPhase(phase, used);
}

core::Result Schedule::RunPhase(Phase phase, job::JobSystem& jobs) noexcept {
    if (!mBuilt) {
        return core::Fail(core::ErrorCode::InvalidState, "Schedule: Build before running");
    }
    const std::span<const SystemId> order = mOrder[static_cast<size_t>(phase)];
    RefreshQueries(phase);
    const uint32_t base = mWorld.GetVersion();

    const job::CounterId phaseDone = jobs.CreateCounter();
    OYI_CHECK(phaseDone.IsValid(), "Schedule: counter pool exhausted");
    {
        // Held until every job is registered, so the wait below cannot see zero too early.
        const job::CompletionDeferral registering(jobs, phaseDone);
        uint32_t used = 0;
        for (const SystemId system : order) {
            SystemRecord& record = *mSystems[system.mValue];
            ++used;
            record.mRunVersion = base + used;
            record.mDone       = jobs.CreateCounter();
            OYI_CHECK(record.mDone.IsValid(), "Schedule: counter pool exhausted");
        }
        for (const SystemId system : order) {
            SystemRecord& record = *mSystems[system.mValue];
            // One predecessor: wait on it directly. Several: join them through empty jobs.
            job::CounterId waitFor = job::CounterId::Invalid();
            job::CounterId join    = job::CounterId::Invalid();
            if (record.mPredecessors.size() == 1) {
                waitFor = mSystems[record.mPredecessors[0].mValue]->mDone;
            } else if (record.mPredecessors.size() > 1) {
                join = jobs.CreateCounter();
                OYI_CHECK(join.IsValid(), "Schedule: counter pool exhausted");
                const job::JobDesc splice{.mName     = record.mDesc.mName,
                                          .mFunction = nullptr,
                                          .mData     = nullptr,
                                          .mPriority = job::JobPriority::Critical};
                for (const SystemId predecessor : record.mPredecessors) {
                    const core::Result joined = jobs.RunJob(splice, mSystems[predecessor.mValue]->mDone, join);
                    OYI_CHECK(joined.has_value(), "Schedule: job pool exhausted");
                }
                waitFor = join;
            }
            const job::JobDesc run{.mName     = record.mDesc.mName,
                                   .mFunction = &RunSystemJob,
                                   .mData     = &record,
                                   .mPriority = job::JobPriority::Critical};
            const core::Result dispatched = jobs.RunJob(run, waitFor, record.mDone);
            OYI_CHECK(dispatched.has_value(), "Schedule: job pool exhausted");
            const job::JobDesc finish{.mName     = record.mDesc.mName,
                                      .mFunction = nullptr,
                                      .mData     = nullptr,
                                      .mPriority = job::JobPriority::Critical};
            const core::Result spliced = jobs.RunJob(finish, record.mDone, phaseDone);
            OYI_CHECK(spliced.has_value(), "Schedule: job pool exhausted");
            if (join.IsValid()) {
                jobs.ReleaseCounter(join);
            }
        }
    }
    jobs.WaitForCounter(phaseDone);
    jobs.ReleaseCounter(phaseDone);
    for (const SystemId system : order) {
        jobs.ReleaseCounter(mSystems[system.mValue]->mDone);
        mSystems[system.mValue]->mDone = job::CounterId::Invalid();
    }
    return EndPhase(phase, static_cast<uint32_t>(order.size()));
}

core::Result Schedule::RunFrame(job::JobSystem& jobs) noexcept {
    for (uint32_t phase = 0; phase < kPhaseCount; ++phase) {
        const core::Result result = RunPhase(static_cast<Phase>(phase), jobs);
        if (!result.has_value()) {
            return result;
        }
    }
    return {};
}

core::Result Schedule::RunFrameSerial() noexcept {
    for (uint32_t phase = 0; phase < kPhaseCount; ++phase) {
        const core::Result result = RunPhaseSerial(static_cast<Phase>(phase));
        if (!result.has_value()) {
            return result;
        }
    }
    return {};
}

} // namespace oyi::ecs
