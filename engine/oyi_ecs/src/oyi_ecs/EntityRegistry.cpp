#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/EntityRegistry.h>

#include <cstddef>
#include <cstdint>
#include <span>

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

uint32_t EntityRegistry::GetGeneration(uint32_t index) const noexcept {
    return index < mSlots.size() ? mSlots[index].mGeneration : 0;
}

bool EntityRegistry::IsIndexAlive(uint32_t index) const noexcept {
    return index < mSlots.size() && mSlots[index].mAlive;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void EntityRegistry::Restore(std::span<const uint32_t> generations, std::span<const uint8_t> alive) noexcept {
    OYI_ASSERT(generations.size() == alive.size(), "EntityRegistry: table sizes differ");
    Clear();
    mSlots.resize(generations.size());
    for (size_t index = 0; index < generations.size(); ++index) {
        OYI_ASSERT(generations[index] != 0, "EntityRegistry: generation zero is the invalid handle");
        mSlots[index].mGeneration = generations[index];
        mSlots[index].mAlive      = alive[index] != 0;
        mAliveCount += alive[index] != 0 ? 1 : 0;
    }
    // Dead indices are handed out lowest first: Create pops from the back.
    for (size_t index = generations.size(); index-- > 0;) {
        if (alive[index] == 0) {
            mFreeIndices.push_back(static_cast<uint32_t>(index));
        }
    }
}

void EntityRegistry::Clear() noexcept {
    mSlots.clear();
    mFreeIndices.clear();
    mAliveCount = 0;
}

} // namespace oyi::ecs
