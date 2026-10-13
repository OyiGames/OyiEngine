#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/World.h>
#include <oyi_net/Prediction.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace oyi::net {

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
PredictionHistory::PredictionHistory(core::Allocator& allocator, uint32_t capacity) noexcept
    : mAllocator(allocator), mResource(allocator), mSlots(&mResource), mCapacity(capacity == 0 ? 1 : capacity) {
    mSlots.resize(mCapacity);
    for (Slot& slot : mSlots) {
        slot.mSnapshot = mAllocator.New<ecs::Snapshot>(mAllocator);
    }
}

PredictionHistory::~PredictionHistory() {
    for (const Slot& slot : mSlots) {
        mAllocator.Delete(slot.mSnapshot);
    }
}

void PredictionHistory::Build(const ecs::ComponentRegistry& registry) noexcept {
    mPredicted = ecs::ComponentMask{};
    for (uint32_t i = 0; i < registry.GetCount(); ++i) {
        const ecs::ComponentTypeIndex type{.mValue = static_cast<uint16_t>(i)};
        if (HasFlag(registry.GetInfo(type).mFlags, core::ComponentFlags::Predicted)) {
            mPredicted.Set(type);
        }
    }
}

const PredictionHistory::Slot* PredictionHistory::Find(uint64_t tick) const noexcept {
    const Slot& slot = mSlots[tick % mCapacity];
    return slot.mUsed && slot.mTick == tick ? &slot : nullptr;
}

void PredictionHistory::Record(const ecs::World& world, uint64_t tick) noexcept {
    Slot& slot = mSlots[tick % mCapacity];
    if (!slot.mUsed) {
        ++mCount;
    }
    slot.mUsed = true;
    slot.mTick = tick;
    slot.mSnapshot->Capture(world, mPredicted);
    if (mCount == 1 || tick > mNewest) {
        mNewest = tick;
    }
}

core::Expected<PredictionHistory::RollbackStats> PredictionHistory::Rollback(ecs::World& world,
                                                                             uint64_t tick) noexcept {
    const Slot* const slot = Find(tick);
    if (slot == nullptr) {
        return core::Fail(core::ErrorCode::NotFound, "PredictionHistory: that tick is no longer held");
    }
    const ecs::Snapshot& snapshot = *slot->mSnapshot;
    RollbackStats stats;
    const std::span<const ecs::EntityId> entities = snapshot.GetEntities();
    for (uint32_t row = 0; row < entities.size(); ++row) {
        const ecs::EntityId entity = entities[row];
        if (!world.IsAlive(entity)) {
            ++stats.mSkipped;
            continue;
        }
        mPredicted.ForEach([&](ecs::ComponentTypeIndex type) {
            if (!snapshot.HasRow(row, type)) {
                return;
            }
            void* const target = world.GetComponentForWrite(entity, type);
            if (target == nullptr) {
                ++stats.mSkipped;
                return;
            }
            const uint32_t size                     = world.GetComponents().GetInfo(type).mSize;
            const std::span<const std::byte> column = snapshot.GetColumn(type);
            std::memcpy(target, column.data() + (static_cast<size_t>(row) * size), size);
            ++stats.mRestored;
        });
    }
    return stats;
}

void PredictionHistory::Truncate(uint64_t tick) noexcept {
    for (Slot& slot : mSlots) {
        if (slot.mUsed && slot.mTick > tick) {
            slot.mUsed = false;
            slot.mSnapshot->Clear();
            --mCount;
        }
    }
    mNewest = GetNewestTick();
}

bool PredictionHistory::Contains(uint64_t tick) const noexcept {
    return Find(tick) != nullptr;
}

uint64_t PredictionHistory::GetOldestTick() const noexcept {
    bool any        = false;
    uint64_t oldest = 0;
    for (const Slot& slot : mSlots) {
        if (slot.mUsed && (!any || slot.mTick < oldest)) {
            oldest = slot.mTick;
            any    = true;
        }
    }
    return oldest;
}

uint64_t PredictionHistory::GetNewestTick() const noexcept {
    bool any        = false;
    uint64_t newest = 0;
    for (const Slot& slot : mSlots) {
        if (slot.mUsed && (!any || slot.mTick > newest)) {
            newest = slot.mTick;
            any    = true;
        }
    }
    return newest;
}

} // namespace oyi::net
