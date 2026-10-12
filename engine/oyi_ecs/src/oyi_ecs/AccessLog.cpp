#include <oyi_core/Allocator.h>
#include <oyi_ecs/AccessLog.h>
#include <oyi_ecs/Archetype.h>
#include <oyi_ecs/ComponentMask.h>

#include <cstddef>
#include <cstdint>

namespace oyi::ecs {

namespace {

thread_local AccessLog* gCurrentLog = nullptr;

constexpr size_t kInitialSlots = 64;

[[nodiscard]] uint64_t Mix(uint64_t key) noexcept {
    key ^= key >> 33u;
    key *= 0xFF51'AFD7'ED55'8CCDull;
    key ^= key >> 33u;
    return key;
}

} // namespace

AccessLog::AccessLog(core::Allocator& allocator) noexcept
    : mResource(allocator), mEntries(&mResource), mSlots(&mResource) {}

uint64_t AccessLog::KeyOf(const Archetype& archetype, uint32_t chunk, ComponentTypeIndex type) noexcept {
    return (static_cast<uint64_t>(archetype.GetIndex()) << 32u) | (static_cast<uint64_t>(chunk) << 9u) |
           static_cast<uint64_t>(type.mValue);
}

uint64_t AccessLog::KeyOf(const AccessEntry& entry) noexcept {
    return KeyOf(*entry.mArchetype, entry.mChunk, entry.mType);
}

uint32_t AccessLog::Probe(uint64_t key) const noexcept {
    const size_t mask = mSlots.size() - 1;
    size_t slot       = static_cast<size_t>(Mix(key)) & mask;
    while (true) {
        const uint32_t index = mSlots[slot];
        if (index == 0 || KeyOf(mEntries[index - 1]) == key) {
            return static_cast<uint32_t>(slot);
        }
        slot = (slot + 1) & mask;
    }
}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
void AccessLog::Grow() noexcept {
    const size_t capacity = mSlots.empty() ? kInitialSlots : mSlots.size() * 2;
    mSlots.assign(capacity, 0);
    for (uint32_t i = 0; i < mEntries.size(); ++i) {
        mSlots[Probe(KeyOf(mEntries[i]))] = i + 1;
    }
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void AccessLog::Record(const Archetype& archetype, uint32_t chunk, ComponentTypeIndex type, AccessKind kind) noexcept {
    if ((mEntries.size() + 1) * 2 > mSlots.size()) {
        Grow();
    }
    const uint32_t slot = Probe(KeyOf(archetype, chunk, type));
    if (mSlots[slot] != 0) {
        AccessEntry& entry = mEntries[mSlots[slot] - 1];
        if (kind == AccessKind::Write) {
            entry.mKind = AccessKind::Write;
        }
        return;
    }
    mEntries.push_back(AccessEntry{.mArchetype = &archetype, .mChunk = chunk, .mType = type, .mKind = kind});
    mSlots[slot] = static_cast<uint32_t>(mEntries.size());
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void AccessLog::Clear() noexcept {
    mEntries.clear();
    if (!mSlots.empty()) {
        mSlots.assign(mSlots.size(), 0);
    }
}

const AccessEntry* AccessLog::Find(const Archetype& archetype, uint32_t chunk, ComponentTypeIndex type) const noexcept {
    if (mSlots.empty()) {
        return nullptr;
    }
    const uint32_t index = mSlots[Probe(KeyOf(archetype, chunk, type))];
    return index == 0 ? nullptr : &mEntries[index - 1];
}

const AccessEntry* AccessLog::FindConflict(const AccessLog& other) const noexcept {
    for (const AccessEntry& entry : mEntries) {
        const AccessEntry* const theirs = other.Find(*entry.mArchetype, entry.mChunk, entry.mType);
        if (theirs != nullptr && (entry.mKind == AccessKind::Write || theirs->mKind == AccessKind::Write)) {
            return &entry;
        }
    }
    return nullptr;
}

AccessLog* GetCurrentAccessLog() noexcept {
    return gCurrentLog;
}

AccessScope::AccessScope(AccessLog* log) noexcept : mPrevious(gCurrentLog) {
    gCurrentLog = log;
}

AccessScope::~AccessScope() {
    gCurrentLog = mPrevious;
}

} // namespace oyi::ecs
