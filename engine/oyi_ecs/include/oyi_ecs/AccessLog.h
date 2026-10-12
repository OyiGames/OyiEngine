#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/ComponentMask.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

/// OYI_ACCESS_CHECKS (ADR-0019, the runtime half of MAW-lite): when 1, component accessors record
/// what the current execution context touched so that the schedule can compare the tables at
/// the phase boundary. The build settings turn it on in Debug; everything compiles to nothing
/// otherwise.
#ifndef OYI_ACCESS_CHECKS
#define OYI_ACCESS_CHECKS 0
#endif

namespace oyi::ecs {

class Archetype;

enum class AccessKind : uint8_t {
    Read,
    Write,
};

/// One recorded access: a component type in one chunk, with the strongest kind seen.
struct AccessEntry {
    const Archetype* mArchetype = nullptr;
    uint32_t mChunk             = 0;
    ComponentTypeIndex mType;
    AccessKind mKind = AccessKind::Read;
};

/// The access table of one execution context (ADR-0019: per task, not per thread). Entries are
/// deduplicated by (archetype, chunk, type); a later write upgrades an earlier read.
class AccessLog {
public:
    explicit AccessLog(core::Allocator& allocator) noexcept;
    AccessLog(const AccessLog&)            = delete;
    AccessLog(AccessLog&&)                 = delete;
    AccessLog& operator=(const AccessLog&) = delete;
    AccessLog& operator=(AccessLog&&)      = delete;
    ~AccessLog()                           = default;

    void Record(const Archetype& archetype, uint32_t chunk, ComponentTypeIndex type, AccessKind kind) noexcept;
    void Clear() noexcept;

    [[nodiscard]] std::span<const AccessEntry> GetEntries() const noexcept { return mEntries; }
    [[nodiscard]] bool IsEmpty() const noexcept { return mEntries.empty(); }
    [[nodiscard]] const AccessEntry* Find(const Archetype& archetype, uint32_t chunk,
                                          ComponentTypeIndex type) const noexcept;
    /// The first entry of this log that `other` also touched where at least one side wrote.
    [[nodiscard]] const AccessEntry* FindConflict(const AccessLog& other) const noexcept;

private:
    [[nodiscard]] static uint64_t KeyOf(const Archetype& archetype, uint32_t chunk, ComponentTypeIndex type) noexcept;
    [[nodiscard]] static uint64_t KeyOf(const AccessEntry& entry) noexcept;
    [[nodiscard]] uint32_t Probe(uint64_t key) const noexcept;
    void Grow() noexcept;

    core::MemoryResource mResource;
    std::pmr::vector<AccessEntry> mEntries;
    std::pmr::vector<uint32_t> mSlots; ///< open addressing: entry index + 1, 0 = empty
};

/// The log the calling thread records into; nullptr when nothing is recording.
[[nodiscard]] AccessLog* GetCurrentAccessLog() noexcept;

/// Makes `log` the current log of this thread for the scope; nested scopes restore the previous.
class AccessScope {
public:
    explicit AccessScope(AccessLog* log) noexcept;
    AccessScope(const AccessScope&)            = delete;
    AccessScope(AccessScope&&)                 = delete;
    AccessScope& operator=(const AccessScope&) = delete;
    AccessScope& operator=(AccessScope&&)      = delete;
    ~AccessScope();

private:
    AccessLog* mPrevious;
};

/// What the accessors call; a no-op unless checks are compiled in and a scope is open.
inline void RecordAccess([[maybe_unused]] const Archetype& archetype, [[maybe_unused]] uint32_t chunk,
                         [[maybe_unused]] ComponentTypeIndex type, [[maybe_unused]] AccessKind kind) noexcept {
#if OYI_ACCESS_CHECKS
    if (AccessLog* const log = GetCurrentAccessLog(); log != nullptr) {
        log->Record(archetype, chunk, type, kind);
    }
#endif
}

} // namespace oyi::ecs
