#include <oyi_core/Allocator.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/ManagedObject.h>

#include <cstdint>
#include <cstring>

namespace oyi::ecs {

CommandBuffer::CommandBuffer(core::Allocator& allocator) noexcept
    : mResource(allocator), mCommands(&mResource), mCreateMasks(&mResource), mPayload(&mResource) {}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
void CommandBuffer::Record(Op op, ComponentTypeIndex type, EntityId entity, PendingEntity pending,
                           uint32_t payloadOffset, uint32_t payloadSize, bool flag) noexcept {
    mCommands.push_back(Command{.mSortKey       = mSortKey,
                                .mOp            = op,
                                .mFlag          = flag,
                                .mMark          = EntityMark::None,
                                .mManaged       = ManagedId{},
                                .mType          = type,
                                .mEntity        = entity,
                                .mPending       = pending,
                                .mPayloadOffset = payloadOffset,
                                .mPayloadSize   = payloadSize});
}

// NOLINTNEXTLINE(bugprone-exception-escape)
uint32_t CommandBuffer::StorePayload(const void* data, uint32_t size) noexcept {
    const auto offset = static_cast<uint32_t>(mPayload.size());
    mPayload.resize(mPayload.size() + size);
    std::memcpy(mPayload.data() + offset, data, size);
    return offset;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
PendingEntity CommandBuffer::CreateEntity(const ComponentMask& mask) noexcept {
    const PendingEntity pending{.mIndex = static_cast<uint32_t>(mCreateMasks.size())};
    mCreateMasks.push_back(mask);
    Record(Op::Create, ComponentTypeIndex::Invalid(), EntityId::Invalid(), pending, 0, 0, false);
    return pending;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
PendingEntity CommandBuffer::CloneEntity(EntityId source) noexcept {
    // A clone fills a create slot like a Create does; the mask entry only keeps the slots aligned.
    const PendingEntity pending{.mIndex = static_cast<uint32_t>(mCreateMasks.size())};
    mCreateMasks.emplace_back();
    Record(Op::Clone, ComponentTypeIndex::Invalid(), source, pending, 0, 0, false);
    return pending;
}

void CommandBuffer::DestroyEntity(EntityId entity) noexcept {
    Record(Op::Destroy, ComponentTypeIndex::Invalid(), entity, PendingEntity{}, 0, 0, false);
}

void CommandBuffer::AddComponent(EntityId entity, ComponentTypeIndex type) noexcept {
    Record(Op::Add, type, entity, PendingEntity{}, 0, 0, false);
}

void CommandBuffer::AddComponent(PendingEntity entity, ComponentTypeIndex type) noexcept {
    Record(Op::Add, type, EntityId::Invalid(), entity, 0, 0, false);
}

void CommandBuffer::RemoveComponent(EntityId entity, ComponentTypeIndex type) noexcept {
    Record(Op::Remove, type, entity, PendingEntity{}, 0, 0, false);
}

void CommandBuffer::RemoveComponent(PendingEntity entity, ComponentTypeIndex type) noexcept {
    Record(Op::Remove, type, EntityId::Invalid(), entity, 0, 0, false);
}

void CommandBuffer::SetComponent(EntityId entity, ComponentTypeIndex type, const void* data, uint32_t size) noexcept {
    const uint32_t offset = StorePayload(data, size);
    Record(Op::Set, type, entity, PendingEntity{}, offset, size, false);
}

void CommandBuffer::SetComponent(PendingEntity entity, ComponentTypeIndex type, const void* data,
                                 uint32_t size) noexcept {
    const uint32_t offset = StorePayload(data, size);
    Record(Op::Set, type, EntityId::Invalid(), entity, offset, size, false);
}

void CommandBuffer::SetComponentEnabled(EntityId entity, ComponentTypeIndex type, bool enabled) noexcept {
    Record(Op::Enable, type, entity, PendingEntity{}, 0, 0, enabled);
}

void CommandBuffer::SetComponentEnabled(PendingEntity entity, ComponentTypeIndex type, bool enabled) noexcept {
    Record(Op::Enable, type, EntityId::Invalid(), entity, 0, 0, enabled);
}

void CommandBuffer::WriteManagedState(EntityId owner, ManagedId id, const void* data, uint32_t size) noexcept {
    const uint32_t offset = StorePayload(data, size);
    Record(Op::ManagedState, ComponentTypeIndex::Invalid(), owner, PendingEntity{}, offset, size, false);
    mCommands.back().mManaged = id;
}

void CommandBuffer::SetMark(EntityId entity, EntityMark mark) noexcept {
    Record(Op::Mark, ComponentTypeIndex::Invalid(), entity, PendingEntity{}, 0, 0, false);
    mCommands.back().mMark = mark;
}

void CommandBuffer::SetMark(PendingEntity entity, EntityMark mark) noexcept {
    Record(Op::Mark, ComponentTypeIndex::Invalid(), EntityId::Invalid(), entity, 0, 0, false);
    mCommands.back().mMark = mark;
}

void CommandBuffer::Clear() noexcept {
    mCommands.clear();
    mCreateMasks.clear();
    mPayload.clear();
}

} // namespace oyi::ecs
