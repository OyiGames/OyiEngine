#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_core/FrameAllocator.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/AccessLog.h>
#include <oyi_ecs/Archetype.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Query.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_job/CompletionDeferral.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobBuilder.h>
#include <oyi_job/JobSystem.h>
#include <oyi_job/ParallelFor.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

namespace {

/// The query requires everything the system touches, read or write, except the optional types.
[[nodiscard]] ComponentMask Touched(const SystemDesc& desc) noexcept {
    ComponentMask all = desc.mRead;
    desc.mWrite.ForEach([&all](ComponentTypeIndex type) { all.Set(type); });
    desc.mOptional.ForEach([&all](ComponentTypeIndex type) { all.Clear(type); });
    return all;
}

[[nodiscard]] bool Conflicts(const SystemDesc& a, const SystemDesc& b) noexcept {
    return a.mWrite.Intersects(b.mWrite) || a.mWrite.Intersects(b.mRead) || b.mWrite.Intersects(a.mRead) ||
           a.mWriteResources.Intersects(b.mWriteResources) || a.mWriteResources.Intersects(b.mReadResources) ||
           b.mWriteResources.Intersects(a.mReadResources);
}

/// Conflicts on a resource the schedule orders by itself (ResourceOrdering::Automatic).
[[nodiscard]] bool ConflictsAutomatically(const SystemDesc& a, const SystemDesc& b,
                                          const ResourceMask& automatic) noexcept {
    const ResourceMask aWrites = a.mWriteResources.And(automatic);
    const ResourceMask bWrites = b.mWriteResources.And(automatic);
    return aWrites.Intersects(b.mWriteResources) || aWrites.Intersects(b.mReadResources) ||
           bWrites.Intersects(a.mReadResources);
}

} // namespace

/// The chunk a task owns (ADR-0019 Premark): writes elsewhere are violations.
struct Schedule::TaskOwner {
    const Archetype* mArchetype = nullptr;
    uint32_t mChunk             = 0;
};

/// One chunk task of a ParallelForEachChunk; the array lives in frame memory until the jobs ran.
struct Schedule::ChunkTask {
    SystemRecord* mRecord;
    Archetype* mArchetype;
    uint32_t mChunk;
    uint32_t mIndex; ///< which task buffers it uses
    ChunkTaskFunction mFunction;
};

struct Schedule::SystemRecord {
    // pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    SystemRecord(Schedule& owner, const SystemDesc& desc, core::Allocator& allocator,
                 std::pmr::memory_resource* resource) noexcept
        : mOwner(owner)
        , mDesc(desc)
        , mQuery(allocator, Touched(desc), desc.mExclude)
        , mCommands(allocator)
        , mOutbox(allocator)
        , mInbox(allocator)
        , mAccesses(allocator)
        , mTaskCommands(resource)
        , mTaskOutboxes(resource)
        , mTaskLogs(resource)
        , mTaskOwners(resource)
        , mPredecessors(resource) {}

    Schedule& mOwner;
    SystemDesc mDesc;
    Query mQuery;
    CommandBuffer mCommands;
    MessageBuffer mOutbox;
    MessageInbox mInbox;
    AccessLog mAccesses;
    std::pmr::deque<CommandBuffer> mTaskCommands; ///< one set per chunk task, grown once, reused
    std::pmr::deque<MessageBuffer> mTaskOutboxes;
    std::pmr::deque<AccessLog> mTaskLogs;
    std::pmr::vector<TaskOwner> mTaskOwners;
    uint32_t mTaskCount   = 0;       ///< chunk tasks of the run in progress
    job::JobSystem* mJobs = nullptr; ///< the job system of the run in progress; nullptr when serial
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
    , mPhaseBuffers(&mResource)
    , mWindowCommands(allocator)
    , mWindowOutbox(allocator)
    , mReach(&mResource)
    , mViolations(&mResource) {}

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
core::Result Schedule::Sort(std::span<uint8_t> reach) noexcept {
    const auto count = static_cast<uint32_t>(mSystems.size());
    std::ranges::fill(reach, uint8_t{0});
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
    }
    return {};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result Schedule::Build() noexcept {
    if (mBuilt) {
        return core::Fail(core::ErrorCode::InvalidState, "Schedule: already built");
    }
    const auto count = static_cast<uint32_t>(mSystems.size());
    // reach[a * count + b] != 0 when a is ordered before b through edges.
    mReach.assign(static_cast<size_t>(count) * count, uint8_t{0});
    const std::span<uint8_t> reach = mReach;
    const core::Result sorted      = Sort(reach);
    if (!sorted.has_value()) {
        return sorted;
    }
    // Resources ordered automatically (ADR-0007: writers of the Environment serialize on their
    // own): every still-unordered conflicting pair gets an edge in the order just derived, which
    // cannot form a cycle, and the order is derived again so that the edges are in effect. Each
    // system looks back for its nearest unordered conflict so that the edges form a chain.
    const ResourceMask& automatic = mWorld.GetResources().GetAutomaticMask();
    bool added                    = false;
    for (const std::pmr::vector<SystemId>& order : mOrder) {
        for (size_t j = 1; j < order.size(); ++j) {
            for (size_t i = j; i-- > 0;) {
                const uint32_t a = order[i].mValue;
                const uint32_t b = order[j].mValue;
                if (reach[(static_cast<size_t>(a) * count) + b] != 0 ||
                    reach[(static_cast<size_t>(b) * count) + a] != 0 ||
                    !ConflictsAutomatically(mSystems[a]->mDesc, mSystems[b]->mDesc, automatic)) {
                    continue;
                }
                mSystems[b]->mPredecessors.push_back(order[i]);
                added = true;
                // Keep reach current so that only the edges that are needed get added.
                for (uint32_t t = 0; t < count; ++t) {
                    if (t != a && reach[(static_cast<size_t>(t) * count) + a] == 0) {
                        continue;
                    }
                    for (uint32_t u = 0; u < count; ++u) {
                        if (u == b || reach[(static_cast<size_t>(b) * count) + u] != 0) {
                            reach[(static_cast<size_t>(t) * count) + u] = 1;
                        }
                    }
                }
            }
        }
    }
    if (added) {
        const core::Result resorted = Sort(reach);
        if (!resorted.has_value()) {
            return resorted;
        }
    }
    for (const std::pmr::vector<SystemId>& order : mOrder) {
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
                                      "Schedule: two systems touch the same component or resource without an "
                                      "explicit order");
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
    record.mRunVersion = version;
    record.mTaskCount  = 0;
    SystemContext context(mWorld, record.mQuery, record.mCommands, record.mOutbox, record.mInbox, record.mDesc, version,
                          record.mLastVersion, scratch,
                          ChunkTaskDispatcher{.mFunction = &Schedule::DispatchChunkTasks, .mOwner = &record});
    record.mAccesses.Clear();
    const AccessScope recording(mAccessMode != AccessCheckMode::Off ? &record.mAccesses : nullptr);
    record.mDesc.mFunction(context);
    // mLastVersion and the inbox are settled at the end of the phase: chunk tasks may still run.
}

void Schedule::RunChunkTask(SystemRecord& record, const ChunkTask& task, core::Allocator& scratch) noexcept {
    SystemContext context(record.mOwner.mWorld, record.mQuery, record.mTaskCommands[task.mIndex],
                          record.mTaskOutboxes[task.mIndex], record.mInbox, record.mDesc, record.mRunVersion,
                          record.mLastVersion, scratch, ChunkTaskDispatcher{}, task.mArchetype, task.mChunk);
    const AccessScope recording(record.mOwner.mAccessMode != AccessCheckMode::Off ? &record.mTaskLogs[task.mIndex]
                                                                                  : nullptr);
    ChunkView view(*task.mArchetype, task.mChunk, record.mRunVersion);
    task.mFunction(context, view);
}

void Schedule::RunChunkTasksJob(job::JobContext& context, void* data, uint32_t begin, uint32_t end) {
    const auto* const tasks = static_cast<const ChunkTask*>(data);
    for (uint32_t i = begin; i < end; ++i) {
        RunChunkTask(*tasks[i].mRecord, tasks[i], context.mFrameAllocator);
    }
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result Schedule::DispatchChunkTasks(void* owner, SystemContext& outer, ChunkTaskFunction function) noexcept {
    auto* const record = static_cast<SystemRecord*>(owner);
    Schedule& self     = record->mOwner;
    World& world       = self.mWorld;
    // The query cache was refreshed on the main thread before the phase; this only reads it.
    uint32_t count = 0;
    for (const uint32_t index : record->mQuery.GetMatches(world)) {
        count += world.GetArchetype(index).GetChunkCount();
    }
    while (record->mTaskCommands.size() < count) {
        record->mTaskCommands.emplace_back(self.mAllocator);
        record->mTaskOutboxes.emplace_back(self.mAllocator);
        record->mTaskLogs.emplace_back(self.mAllocator);
    }
    record->mTaskOwners.resize(count);
    record->mTaskCount = count;
    for (uint32_t i = 0; i < count; ++i) {
        record->mTaskCommands[i].Clear();
        record->mTaskOutboxes[i].Clear();
        record->mTaskLogs[i].Clear();
    }
    if (count == 0) {
        return {};
    }
    if (record->mJobs == nullptr) {
        // The serial run: the same tasks, the same buffers, one after another in chunk order.
        uint32_t next = 0;
        for (const uint32_t index : record->mQuery.GetMatches(world)) {
            Archetype& archetype = world.GetArchetypeForWrite(index);
            for (uint32_t chunk = 0; chunk < archetype.GetChunkCount(); ++chunk) {
                record->mTaskOwners[next] = TaskOwner{.mArchetype = &archetype, .mChunk = chunk};
                const ChunkTask task{.mRecord    = record,
                                     .mArchetype = &archetype,
                                     .mChunk     = chunk,
                                     .mIndex     = next,
                                     .mFunction  = function};
                RunChunkTask(*record, task, outer.GetScratch());
                ++next;
            }
        }
        return {};
    }
    // The task table lives in this worker's frame memory until the frame ends.
    core::FrameAllocator& frame = record->mJobs->GetCurrentFrameAllocator();
    void* const memory          = frame.Allocate(sizeof(ChunkTask) * count, alignof(ChunkTask));
    if (memory == nullptr) {
        return core::Fail(core::ErrorCode::OutOfMemory, "Schedule: no frame memory for the chunk tasks");
    }
    auto* const tasks = static_cast<ChunkTask*>(memory);
    uint32_t next     = 0;
    for (const uint32_t index : record->mQuery.GetMatches(world)) {
        Archetype& archetype = world.GetArchetypeForWrite(index);
        for (uint32_t chunk = 0; chunk < archetype.GetChunkCount(); ++chunk) {
            record->mTaskOwners[next] = TaskOwner{.mArchetype = &archetype, .mChunk = chunk};
            std::construct_at(&tasks[next], ChunkTask{.mRecord    = record,
                                                      .mArchetype = &archetype,
                                                      .mChunk     = chunk,
                                                      .mIndex     = next,
                                                      .mFunction  = function});
            ++next;
        }
    }
    // The tasks splice into this system's completion counter, so everything that waits for the
    // system also waits for its chunks (ADR-0009: nested builders accumulate into the parent).
    job::JobBuilder builder(*record->mJobs, record->mDesc.mName, job::JobPriority::Critical, job::CounterId::Invalid(),
                            record->mDone, job::JobBuilder::Fence::None);
    return job::ParallelFor(builder, record->mDesc.mName, count, 1, &Schedule::RunChunkTasksJob, tasks, nullptr);
}

void Schedule::RouteOutbox(MessageBuffer& outbox) noexcept {
    // Records in sending order, receivers in registration order.
    for (const MessageBuffer::Record& record : outbox.GetRecords()) {
        const std::byte* const payload = outbox.GetPayload().data() + record.mPayloadOffset;
        const ComponentMask* const targetMask =
            record.mTarget.IsValid() ? mWorld.GetComponentMask(record.mTarget) : nullptr;
        if (record.mTarget.IsValid() && targetMask == nullptr) {
            continue; // the target died before the sync point
        }
        for (SystemRecord* const receiver : mSystems) {
            if (!receiver->mDesc.mReceives.Test(record.mType)) {
                continue;
            }
            if (targetMask != nullptr && !receiver->mQuery.Matches(*targetMask)) {
                continue;
            }
            receiver->mInbox.Push(record.mType, record.mTarget, payload, record.mPayloadSize);
        }
    }
    outbox.Clear();
}

void Schedule::RouteMessages(Phase phase) noexcept {
    // Senders in phase order, a system's chunk tasks in chunk order after it: the queues come
    // out the same whatever the workers did.
    for (const SystemId sender : mOrder[static_cast<size_t>(phase)]) {
        SystemRecord& record = *mSystems[sender.mValue];
        RouteOutbox(record.mOutbox);
        for (uint32_t i = 0; i < record.mTaskCount; ++i) {
            RouteOutbox(record.mTaskOutboxes[i]);
        }
    }
}

void Schedule::RunSystemJob(job::JobContext& context, void* data) {
    auto* const record = static_cast<SystemRecord*>(data);
    record->mOwner.RunSystem(*record, record->mRunVersion, context.mFrameAllocator);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result Schedule::EndPhase(Phase phase, uint32_t versionsUsed) noexcept {
    ValidateAccesses(phase);
    RouteMessages(phase);
    mPhaseBuffers.clear();
    for (const SystemId system : mOrder[static_cast<size_t>(phase)]) {
        SystemRecord& record = *mSystems[system.mValue];
        record.mCommands.SetSortKey(record.mOrderIndex);
        mPhaseBuffers.push_back(&record.mCommands);
        for (uint32_t i = 0; i < record.mTaskCount; ++i) {
            record.mTaskCommands[i].SetSortKey(record.mOrderIndex);
            mPhaseBuffers.push_back(&record.mTaskCommands[i]);
        }
        record.mLastVersion = record.mRunVersion;
        // Delivered messages are for one run only.
        record.mInbox.Clear();
    }
    // Every system of the phase used its own version; move the world past all of them before
    // the sync point stamps its own writes.
    mWorld.AdvanceVersion(versionsUsed);
    mLastReplay = mWorld.SyncPoint(std::span<CommandBuffer* const>{mPhaseBuffers});
    if (mSnapshot != nullptr && phase == mSnapshotPhase) {
        mSnapshot->Capture(mWorld, mSnapshotTypes);
    }
    const Window& window = mWindows[static_cast<size_t>(phase)];
    if (window.mFunction != nullptr) {
        return RunWindow(window.mFunction, window.mData);
    }
    return {};
}

void Schedule::SetWindow(Phase phase, WindowFunction function, void* data) noexcept {
    mWindows[static_cast<size_t>(phase)] = Window{.mFunction = function, .mData = data};
}

core::Result Schedule::RunWindow(WindowFunction function, void* data) noexcept {
    if (function == nullptr) {
        return core::Fail(core::ErrorCode::InvalidArgument, "Schedule: a window needs a function");
    }
    {
        FirewallWindow window(mWorld, mSnapshot, mWindowCommands, mWindowOutbox, data);
        function(window);
    }
    // The window's own sync point: what it recorded is live before anything else runs.
    RouteOutbox(mWindowOutbox);
    std::array<CommandBuffer*, 1> buffers{&mWindowCommands};
    mLastWindowReplay = mWorld.SyncPoint(std::span<CommandBuffer* const>{buffers});
    return {};
}

const AccessLog& Schedule::GetAccessLog(SystemId system) const noexcept {
    OYI_ASSERT(system.mValue < mSystems.size(), "Schedule: unknown system");
    return mSystems[system.mValue]->mAccesses;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void Schedule::ValidateAccesses(Phase phase) noexcept {
    mViolations.clear();
    if (mAccessMode == AccessCheckMode::Off) {
        return;
    }
    const std::span<const SystemId> order = mOrder[static_cast<size_t>(phase)];
    const size_t count                    = mSystems.size();
    // Chunk tasks: each table folds into the system's, and a write outside the task's own chunk
    // (for a type the system writes, a read of another chunk races with its owner) is reported.
    for (const SystemId system : order) {
        SystemRecord& record = *mSystems[system.mValue];
        for (uint32_t i = 0; i < record.mTaskCount; ++i) {
            const TaskOwner owner = record.mTaskOwners[i];
            for (const AccessEntry& entry : record.mTaskLogs[i].GetEntries()) {
                record.mAccesses.Record(*entry.mArchetype, entry.mChunk, entry.mType, entry.mKind);
                const bool own = entry.mArchetype == owner.mArchetype && entry.mChunk == owner.mChunk;
                if (!own && record.mDesc.mWrite.Test(entry.mType)) {
                    mViolations.push_back(AccessViolation{.mKind      = AccessViolationKind::CrossChunk,
                                                          .mFirst     = system,
                                                          .mSecond    = SystemId::Invalid(),
                                                          .mType      = entry.mType,
                                                          .mAccess    = entry.mKind,
                                                          .mArchetype = entry.mArchetype,
                                                          .mChunk     = entry.mChunk});
                }
            }
        }
    }
    // Every access against the declaration that the static half of the check relied on.
    for (const SystemId system : order) {
        const SystemRecord& record = *mSystems[system.mValue];
        for (const AccessEntry& entry : record.mAccesses.GetEntries()) {
            const bool declared = entry.mKind == AccessKind::Write
                                      ? record.mDesc.mWrite.Test(entry.mType)
                                      : record.mDesc.mRead.Test(entry.mType) || record.mDesc.mWrite.Test(entry.mType);
            if (!declared) {
                mViolations.push_back(AccessViolation{.mKind      = AccessViolationKind::Undeclared,
                                                      .mFirst     = system,
                                                      .mSecond    = SystemId::Invalid(),
                                                      .mType      = entry.mType,
                                                      .mAccess    = entry.mKind,
                                                      .mArchetype = entry.mArchetype,
                                                      .mChunk     = entry.mChunk});
            }
        }
    }
    // Every pair the graph did not order: they may have run side by side, so their tables must
    // not overlap on a write (ADR-0019: compared at the join, timing ignored).
    for (size_t i = 0; i < order.size(); ++i) {
        for (size_t j = i + 1; j < order.size(); ++j) {
            const uint32_t a = order[i].mValue;
            const uint32_t b = order[j].mValue;
            if (mReach[(static_cast<size_t>(a) * count) + b] != 0 ||
                mReach[(static_cast<size_t>(b) * count) + a] != 0) {
                continue;
            }
            const AccessEntry* const entry = mSystems[a]->mAccesses.FindConflict(mSystems[b]->mAccesses);
            if (entry == nullptr) {
                continue;
            }
            mViolations.push_back(AccessViolation{.mKind      = AccessViolationKind::Unordered,
                                                  .mFirst     = order[i],
                                                  .mSecond    = order[j],
                                                  .mType      = entry->mType,
                                                  .mAccess    = entry->mKind,
                                                  .mArchetype = entry->mArchetype,
                                                  .mChunk     = entry->mChunk});
        }
    }
    if (mAccessMode == AccessCheckMode::Assert && !mViolations.empty()) {
        core::ReportAssertionFailure("Schedule access checks",
                                     "a system touched a chunk outside its declaration or beside an unordered "
                                     "system; see Schedule::GetViolations");
    }
}

void Schedule::SetSnapshotCapture(Snapshot* snapshot, const ComponentMask& types, Phase phase) noexcept {
    mSnapshot      = snapshot;
    mSnapshotTypes = types;
    mSnapshotPhase = phase;
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
        record.mJobs = nullptr;
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
            record.mJobs       = &jobs;
            record.mDone       = jobs.CreateCounter();
            OYI_CHECK(record.mDone.IsValid(), "Schedule: counter pool exhausted");
            if (record.mDesc.mMainThread) {
                // Held by the main thread until it has run the system below.
                jobs.IncrementCounter(record.mDone);
            }
        }
        for (const SystemId system : order) {
            SystemRecord& record = *mSystems[system.mValue];
            if (record.mDesc.mMainThread) {
                // Not a job: the main thread runs it after this loop; only its completion joins
                // the phase counter.
                const job::JobDesc finish{.mName     = record.mDesc.mName,
                                          .mFunction = nullptr,
                                          .mData     = nullptr,
                                          .mPriority = job::JobPriority::Critical};
                const core::Result spliced = jobs.RunJob(finish, record.mDone, phaseDone);
                OYI_CHECK(spliced.has_value(), "Schedule: job pool exhausted");
                continue;
            }
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
    // Early dispatch, late wait (ADR-0007): the jobs are in flight; the main thread now runs its
    // own systems in order, helping with jobs whenever it has to wait for a predecessor.
    for (const SystemId system : order) {
        SystemRecord& record = *mSystems[system.mValue];
        if (!record.mDesc.mMainThread) {
            continue;
        }
        for (const SystemId predecessor : record.mPredecessors) {
            jobs.WaitForCounter(mSystems[predecessor.mValue]->mDone);
        }
        RunSystem(record, record.mRunVersion, jobs.GetMainFrameAllocator());
        jobs.DecrementCounter(record.mDone);
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
