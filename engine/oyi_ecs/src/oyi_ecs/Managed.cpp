#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

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
core::Result ManagedPool::Restore(ManagedId id, ManagedObject* object, EntityId owner) noexcept {
    if (!id.IsValid() || object == nullptr) {
        return core::Fail(core::ErrorCode::InvalidArgument, "ManagedPool: cannot restore an invalid id");
    }
    if (id.mIndex >= mSlots.size()) {
        const auto first = static_cast<uint32_t>(mSlots.size());
        mSlots.resize(static_cast<size_t>(id.mIndex) + 1);
        for (uint32_t index = first; index < id.mIndex; ++index) {
            mFreeIndices.push_back(index);
        }
    } else if (mSlots[id.mIndex].mObject != nullptr) {
        return core::Fail(core::ErrorCode::AlreadyExists, "ManagedPool: the slot is taken");
    } else {
        std::erase(mFreeIndices, id.mIndex);
    }
    Slot& slot       = mSlots[id.mIndex];
    slot.mObject     = object;
    slot.mGeneration = id.mGeneration;
    object->mOwner   = owner;
    object->mId      = id;
    ++mLiveCount;
    return {};
}

ManagedObject* ManagedPool::FindByOwner(EntityId owner, core::TypeId type) noexcept {
    for (const Slot& slot : mSlots) {
        const ManagedObject* const object = slot.mObject;
        if (object != nullptr && object->mOwner == owner && object->GetTypeInfo() != nullptr &&
            object->GetTypeInfo()->GetTypeId() == type) {
            return slot.mObject;
        }
    }
    return nullptr;
}

const ManagedObject* ManagedPool::FindByOwner(EntityId owner, core::TypeId type) const noexcept {
    for (const Slot& slot : mSlots) {
        const ManagedObject* const object = slot.mObject;
        if (object != nullptr && object->mOwner == owner && object->GetTypeInfo() != nullptr &&
            object->GetTypeInfo()->GetTypeId() == type) {
            return object;
        }
    }
    return nullptr;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void ManagedPool::Clear() noexcept {
    for (uint32_t index = 0; index < mSlots.size(); ++index) {
        if (mSlots[index].mObject != nullptr) {
            Release(index);
        }
    }
    mSlots.clear();
    mFreeIndices.clear();
    mLiveCount = 0;
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

// --- type registry ------------------------------------------------------------------------------

ManagedTypeRegistry::ManagedTypeRegistry(core::Allocator& allocator) noexcept
    : mResource(allocator), mTypes(&mResource) {}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result ManagedTypeRegistry::Register(const ManagedTypeInfo& info) noexcept {
    if (info.mState == nullptr || info.mCreate == nullptr) {
        return core::Fail(core::ErrorCode::InvalidArgument, "ManagedTypeRegistry: the type has no state block");
    }
    if (Find(info.GetTypeId()) != nullptr) {
        return {};
    }
    mTypes.push_back(&info);
    return {};
}

const ManagedTypeInfo* ManagedTypeRegistry::Find(core::TypeId type) const noexcept {
    for (const ManagedTypeInfo* const info : mTypes) {
        if (info->GetTypeId() == type) {
            return info;
        }
    }
    return nullptr;
}

const ManagedTypeInfo* ManagedTypeRegistry::Find(std::string_view name) const noexcept {
    for (const ManagedTypeInfo* const info : mTypes) {
        if (info->GetName() == name) {
            return info;
        }
    }
    return nullptr;
}

// --- access resolution ----------------------------------------------------------------------------

core::Result ApplyManagedAccess(const ManagedTypeInfo& info, const World& world, SystemDesc& desc) noexcept {
    ComponentMask read    = desc.mRead;
    ComponentMask write   = desc.mWrite;
    MessageMask receives  = desc.mReceives;
    MessageMask sends     = desc.mSends;
    const auto components = [&world](std::span<const core::ComponentInfo* const> infos, ComponentMask& mask) {
        for (const core::ComponentInfo* const component : infos) {
            const ComponentTypeIndex type = world.GetComponents().Find(component->mTypeId);
            if (!type.IsValid()) {
                return false;
            }
            mask.Set(type);
        }
        return true;
    };
    const auto messages = [&world](std::span<const core::ComponentInfo* const> infos, MessageMask& mask) {
        for (const core::ComponentInfo* const message : infos) {
            const MessageTypeIndex type = world.GetMessages().Find(message->mTypeId);
            if (!type.IsValid()) {
                return false;
            }
            mask.Set(type);
        }
        return true;
    };
    if (!components(info.mReads, read) || !components(info.mWrites, write) || !messages(info.mReceives, receives) ||
        !messages(info.mSends, sends)) {
        return core::Fail(core::ErrorCode::NotFound, "ApplyManagedAccess: a declared type is not registered");
    }
    desc.mRead     = read;
    desc.mWrite    = write;
    desc.mReceives = receives;
    desc.mSends    = sends;
    return {};
}

// --- system -----------------------------------------------------------------------------------

SystemDesc ManagedSystem::Describe(SystemDesc desc) const noexcept {
    desc.mRead.Set(mReferenceType);
    desc.mMainThread = true;
    desc.mFunction   = &ManagedSystem::Run;
    desc.mData       = const_cast<ManagedSystem*>(this); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    return desc;
}

core::Expected<SystemDesc> ManagedSystem::Describe(SystemDesc desc, const ManagedTypeInfo& info,
                                                   const World& world) const noexcept {
    if (const core::Result applied = ApplyManagedAccess(info, world, desc); !applied.has_value()) {
        return core::Fail(applied.error().mCode, applied.error().mMessage);
    }
    return Describe(desc);
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
