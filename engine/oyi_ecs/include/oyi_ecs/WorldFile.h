#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace oyi::ecs {

/// What a saved world (World::Save) says about itself, read without a registry: the tool side
/// of ADR-0013 (`oyi world inspect`) and the first check a loader makes before it trusts a file.
struct WorldFileSummary {
    struct Component {
        std::pmr::string mName;
        uint64_t mTypeId     = 0;
        uint64_t mLayoutHash = 0;
        uint32_t mSize       = 0;
        uint32_t mAlign      = 0;
    };

    struct Archetype {
        std::pmr::vector<uint16_t> mTypes; ///< indices into mComponents
        uint32_t mChunkCount  = 0;
        uint32_t mEntityCount = 0;
        bool mPrototype       = false; ///< ADR-0024: holds prototype entities, skipped by queries
    };

    explicit WorldFileSummary(core::Allocator& allocator) noexcept;

    core::MemoryResource mResource;
    uint32_t mFormatVersion  = 0;
    uint64_t mSchemaHash     = 0;
    uint32_t mChunkSize      = 0;
    uint32_t mWorldVersion   = 0;
    uint32_t mEntityCapacity = 0; ///< slots in the entity table
    uint32_t mEntityCount    = 0; ///< alive entities
    uint32_t mPrototypeCount = 0; ///< entities marked Prototype (ADR-0024)
    uint32_t mLockedCount    = 0; ///< entities marked Locked
    uint32_t mManagedCount   = 0; ///< saved managed objects (ADR-0024: the typed ones)
    std::pmr::vector<Component> mComponents;
    std::pmr::vector<Archetype> mArchetypes;
};

/// Reads the tables of a saved world; the chunks are only measured. Fails with CorruptData for
/// anything that is not a complete world file.
[[nodiscard]] core::Result DescribeSavedWorld(std::span<const std::byte> data, WorldFileSummary& out) noexcept;

} // namespace oyi::ecs
