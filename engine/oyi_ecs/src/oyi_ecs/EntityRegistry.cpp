#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/EntityRegistry.h>

#include <cstdint>

namespace oyi::ecs {

EntityRegistry::EntityRegistry(core::Allocator& allocator) noexcept
    : mResource(allocator), mSlots(&mResource), mFreeIndices(&mResource) {}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
EntityId EntityRegistry::Create() noexcept {
    uint32_t index = 0;
    if (!mFreeIndices.empty()) {
        index = mFreeIndices.back();
        mFreeIndices.pop_back();
    } else {
        index = static_cast<uint32_t>(mSlots.size());
        mSlots.emplace_back();
    }
    Slot& slot     = mSlots[index];
    slot.mAlive    = true;
    slot.mLocation = EntityLocation{};
    ++mAliveCount;
    return EntityId{.mIndex = index, .mGeneration = slot.mGeneration};
}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
void EntityRegistry::Destroy(EntityId entity) noexcept {
    OYI_ASSERT(IsAlive(entity), "EntityRegistry: destroying an entity that is not alive");
    Slot& slot  = mSlots[entity.mIndex];
    slot.mAlive = false;
    // Generation zero is the invalid handle; skip it on wrap-around.
    slot.mGeneration = slot.mGeneration + 1 == 0 ? 1 : slot.mGeneration + 1;
    slot.mLocation   = EntityLocation{};
    mFreeIndices.push_back(entity.mIndex);
    --mAliveCount;
}

bool EntityRegistry::IsAlive(EntityId entity) const noexcept {
    if (!entity.IsValid() || entity.mIndex >= mSlots.size()) {
        return false;
    }
    const Slot& slot = mSlots[entity.mIndex];
    return slot.mAlive && slot.mGeneration == entity.mGeneration;
}

EntityLocation EntityRegistry::GetLocation(EntityId entity) const noexcept {
    OYI_ASSERT(IsAlive(entity), "EntityRegistry: location of a dead entity");
    return mSlots[entity.mIndex].mLocation;
}

void EntityRegistry::SetLocation(EntityId entity, EntityLocation location) noexcept {
    OYI_ASSERT(IsAlive(entity), "EntityRegistry: locating a dead entity");
    mSlots[entity.mIndex].mLocation = location;
}

} // namespace oyi::ecs
