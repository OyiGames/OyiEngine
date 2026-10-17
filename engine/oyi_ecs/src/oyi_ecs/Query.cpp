#include <oyi_core/Allocator.h>
#include <oyi_ecs/Archetype.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Query.h>
#include <oyi_ecs/World.h>

#include <cstdint>
#include <span>

namespace oyi::ecs {

Query::Query(core::Allocator& allocator, const ComponentMask& all, const ComponentMask& none) noexcept
    : mResource(allocator), mAll(all), mNone(none), mMatches(&mResource) {}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
std::span<const uint32_t> Query::GetMatches(const World& world) noexcept {
    const uint32_t count = world.GetArchetypeCount();
    for (uint32_t index = mArchetypesSeen; index < count; ++index) {
        const Archetype& archetype = world.GetArchetype(index);
        if (!archetype.IsPrototype() && Matches(archetype.GetMask())) {
            mMatches.push_back(index);
        }
    }
    mArchetypesSeen = count;
    return mMatches;
}

uint32_t Query::CountEntities(const World& world) noexcept {
    uint32_t total = 0;
    for (const uint32_t index : GetMatches(world)) {
        total += world.GetArchetype(index).GetEntityCount();
    }
    return total;
}

} // namespace oyi::ecs
