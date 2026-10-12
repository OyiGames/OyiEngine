#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/ComponentMask.h>

#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

class World;

/// Which archetypes a system wants: all of `all`, none of `none`. The matching archetype list is
/// cached and extended as new archetypes appear, so a query held by a system costs nothing per
/// frame once the world has settled (ADR-0005: filtering and caching happen at the sync point).
class Query {
public:
    Query(core::Allocator& allocator, const ComponentMask& all, const ComponentMask& none) noexcept;
    Query(const Query&)            = delete;
    Query(Query&&)                 = delete;
    Query& operator=(const Query&) = delete;
    Query& operator=(Query&&)      = delete;
    ~Query()                       = default;

    [[nodiscard]] const ComponentMask& GetAll() const noexcept { return mAll; }
    [[nodiscard]] const ComponentMask& GetNone() const noexcept { return mNone; }
    [[nodiscard]] bool Matches(const ComponentMask& archetype) const noexcept {
        return archetype.Contains(mAll) && !archetype.Intersects(mNone);
    }

    /// Indices of the matching archetypes in `world`, refreshed for archetypes created since the
    /// last call. Archetypes are never removed, so the list only grows.
    [[nodiscard]] std::span<const uint32_t> GetMatches(const World& world) noexcept;
    /// Live entities in the matching archetypes right now.
    [[nodiscard]] uint32_t CountEntities(const World& world) noexcept;

private:
    core::MemoryResource mResource;
    ComponentMask mAll;
    ComponentMask mNone;
    std::pmr::vector<uint32_t> mMatches;
    uint32_t mArchetypesSeen = 0;
};

} // namespace oyi::ecs
