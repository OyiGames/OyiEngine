#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Message.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace oyi::ecs {

// --- registry ---------------------------------------------------------------------------------

core::Expected<MessageTypeIndex> MessageRegistry::Register(const core::ComponentInfo& info) noexcept {
    if (!info.mTypeId.IsValid()) {
        return core::Fail(core::ErrorCode::InvalidArgument, "MessageRegistry: message has no TypeId");
    }
    const MessageTypeIndex existing = Find(info.mTypeId);
    if (existing.IsValid()) {
        if (mInfos[existing.mValue]->mLayoutHash != info.mLayoutHash) {
            return core::Fail(core::ErrorCode::AlreadyExists,
                              "MessageRegistry: a type with this TypeId but another layout is registered");
        }
        return existing;
    }
    if (mCount >= kMaxMessageTypes) {
        return core::Fail(core::ErrorCode::OutOfRange, "MessageRegistry: too many message types");
    }
    const MessageTypeIndex index{.mValue = static_cast<uint16_t>(mCount)};
    mInfos[mCount] = &info;
    ++mCount;
    return index;
}

MessageTypeIndex MessageRegistry::Find(core::TypeId typeId) const noexcept {
    for (uint32_t i = 0; i < mCount; ++i) {
        if (mInfos[i]->mTypeId == typeId) {
            return MessageTypeIndex{.mValue = static_cast<uint16_t>(i)};
        }
    }
    return MessageTypeIndex::Invalid();
}

const core::ComponentInfo& MessageRegistry::GetInfo(MessageTypeIndex type) const noexcept {
    OYI_ASSERT(IsRegistered(type), "MessageRegistry: unknown message type");
    return *mInfos[type.mValue];
}

// --- outbox -----------------------------------------------------------------------------------

MessageBuffer::MessageBuffer(core::Allocator& allocator) noexcept
    : mResource(allocator), mRecords(&mResource), mPayload(&mResource) {}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
void MessageBuffer::Send(MessageTypeIndex type, EntityId target, const void* data, uint32_t size) noexcept {
    const auto offset = static_cast<uint32_t>(mPayload.size());
    mPayload.resize(mPayload.size() + size);
    if (size != 0) {
        std::memcpy(mPayload.data() + offset, data, size);
    }
    mRecords.push_back(Record{.mType = type, .mTarget = target, .mPayloadOffset = offset, .mPayloadSize = size});
}

void MessageBuffer::Clear() noexcept {
    mRecords.clear();
    mPayload.clear();
}

// --- inbox ------------------------------------------------------------------------------------

MessageInbox::MessageInbox(core::Allocator& allocator) noexcept : mResource(allocator), mLanes(&mResource) {}

// NOLINTNEXTLINE(bugprone-exception-escape)
MessageInbox::Lane& MessageInbox::LaneFor(MessageTypeIndex type) noexcept {
    while (mLanes.size() <= type.mValue) {
        mLanes.emplace_back(&mResource);
    }
    return mLanes[type.mValue];
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void MessageInbox::Push(MessageTypeIndex type, EntityId target, const void* data, uint32_t size) noexcept {
    Lane& lane        = LaneFor(type);
    const size_t head = lane.mPayload.size();
    lane.mPayload.resize(head + size);
    if (size != 0) {
        std::memcpy(lane.mPayload.data() + head, data, size);
    }
    lane.mTargets.push_back(target);
    ++mTotal;
}

uint32_t MessageInbox::Count(MessageTypeIndex type) const noexcept {
    return type.mValue < mLanes.size() ? static_cast<uint32_t>(mLanes[type.mValue].mTargets.size()) : 0;
}

std::span<const EntityId> MessageInbox::Targets(MessageTypeIndex type) const noexcept {
    return type.mValue < mLanes.size() ? std::span<const EntityId>{mLanes[type.mValue].mTargets}
                                       : std::span<const EntityId>{};
}

std::span<const std::byte> MessageInbox::Payload(MessageTypeIndex type) const noexcept {
    return type.mValue < mLanes.size() ? std::span<const std::byte>{mLanes[type.mValue].mPayload}
                                       : std::span<const std::byte>{};
}

void MessageInbox::Clear() noexcept {
    for (Lane& lane : mLanes) {
        lane.mTargets.clear();
        lane.mPayload.clear();
    }
    mTotal = 0;
}

} // namespace oyi::ecs
