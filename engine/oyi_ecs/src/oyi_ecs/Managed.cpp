#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace oyi::ecs {

// --- pool -------------------------------------------------------------------------------------

ManagedPool::ManagedPool(core::Allocator& allocator) noexcept
    : mAllocator(allocator), mResource(allocator), mSlots(&mResource), mFreeIndices(&mResource) {}

ManagedPool::~ManagedPool() {
    for (uint32_t index = 0; index < mSlots.size(); ++index) {
        if (mSlots[index].mObject != nullptr) {
            Release(index);
        }
    }
}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<ManagedId> ManagedPool::Adopt(ManagedObject* object, EntityId owner) noexcept {
    uint32_t index = 0;
    if (!mFreeIndices.empty()) {
        index = mFreeIndices.back();
        mFreeIndices.pop_back();
    } else {
        index = static_cast<uint32_t>(mSlots.size());
        mSlots.emplace_back();
    }
    Slot& slot     = mSlots[index];
    slot.mObject   = object;
    object->mOwner = owner;
    object->mId    = ManagedId{.mIndex = index, .mGeneration = slot.mGeneration};
    ++mLiveCount;
    return object->mId;
}

void ManagedPool::Release(uint32_t index) noexcept {
    Slot& slot = mSlots[index];
    mAllocator.Delete(slot.mObject);
    slot.mObject     = nullptr;
    slot.mGeneration = slot.mGeneration + 1 == 0 ? 1 : slot.mGeneration + 1;
    --mLiveCount;
}

ManagedObject* ManagedPool::Resolve(ManagedId id) noexcept {
    if (!id.IsValid() || id.mIndex >= mSlots.size()) {
        return nullptr;
    }
    const Slot& slot = mSlots[id.mIndex];
    return slot.mGeneration == id.mGeneration ? slot.mObject : nullptr;
}

const ManagedObject* ManagedPool::Resolve(ManagedId id) const noexcept {
    if (!id.IsValid() || id.mIndex >= mSlots.size()) {
        return nullptr;
    }
    const Slot& slot = mSlots[id.mIndex];
    return slot.mGeneration == id.mGeneration ? slot.mObject : nullptr;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void ManagedPool::Destroy(ManagedId id) noexcept {
    if (Resolve(id) == nullptr) {
        return;
    }
    Release(id.mIndex);
    mFreeIndices.push_back(id.mIndex);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
uint32_t ManagedPool::CollectDead(const World& world) noexcept {
    uint32_t collected = 0;
    for (uint32_t index = 0; index < mSlots.size(); ++index) {
        const ManagedObject* const object = mSlots[index].mObject;
        if (object != nullptr && !world.IsAlive(object->mOwner)) {
            Release(index);
            mFreeIndices.push_back(index);
            ++collected;
        }
    }
    return collected;
}

// --- system -----------------------------------------------------------------------------------

SystemDesc ManagedSystem::Describe(SystemDesc desc) const noexcept {
    desc.mRead.Set(mReferenceType);
    desc.mMainThread = true;
    desc.mFunction   = &ManagedSystem::Run;
    desc.mData       = const_cast<ManagedSystem*>(this); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    return desc;
}

void ManagedSystem::Run(SystemContext& context) {
    auto* const self     = context.GetData<ManagedSystem>();
    self->mLastCollected = self->mPool.CollectDead(context.GetWorld());
    uint32_t updates     = 0;
    context.ForEachChunk([&](ChunkView& view) {
        const std::span<const ManagedReference> references = context.Read<ManagedReference>(view, self->mReferenceType);
        const std::span<const EntityId> owners             = view.GetEntities();
        for (size_t i = 0; i < references.size(); ++i) {
            ManagedObject* const object = self->mPool.Resolve(references[i].mObject);
            if (object == nullptr) {
                continue;
            }
            ManagedContext managed(context, owners[i], self->mPrev);
            object->Update(managed);
            ++updates;
        }
    });
    self->mLastUpdates = updates;
}

} // namespace oyi::ecs
