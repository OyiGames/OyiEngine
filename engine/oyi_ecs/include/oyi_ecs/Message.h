#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

/// Messages (ADR-0008): POD values a system sends to an entity (or to everyone) and that the
/// receiving systems read on their next run. Every receiving system has its own queue; routing
/// happens at the sync point in a deterministic order, so there is no central dispatcher and no
/// lock on the sending path. Message types are OYI_STRUCT records, registered like components.

inline constexpr uint32_t kMaxMessageTypes = 256;

struct MessageTypeIndex {
    uint16_t mValue = 0xFFFFu;

    [[nodiscard]] static constexpr MessageTypeIndex Invalid() noexcept { return MessageTypeIndex{}; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return mValue < kMaxMessageTypes; }

    [[nodiscard]] constexpr auto operator<=>(const MessageTypeIndex&) const noexcept = default;
};

/// Which message types a system sends or receives.
using MessageMask = TypeMask<MessageTypeIndex>;

/// The message types one World knows about.
class MessageRegistry {
public:
    MessageRegistry() = default;

    /// Registering the same type again returns the existing index; a different layout under the
    /// same TypeId fails with AlreadyExists, a full registry with OutOfRange.
    [[nodiscard]] core::Expected<MessageTypeIndex> Register(const core::ComponentInfo& info) noexcept;

    template <typename T>
    [[nodiscard]] core::Expected<MessageTypeIndex> Register() noexcept {
        return Register(core::ComponentTraits<T>::kInfo);
    }

    [[nodiscard]] MessageTypeIndex Find(core::TypeId typeId) const noexcept;

    template <typename T>
    [[nodiscard]] MessageTypeIndex Find() const noexcept {
        return Find(core::ComponentTraits<T>::kInfo.mTypeId);
    }

    [[nodiscard]] const core::ComponentInfo& GetInfo(MessageTypeIndex type) const noexcept;
    [[nodiscard]] bool IsRegistered(MessageTypeIndex type) const noexcept { return type.mValue < mCount; }
    [[nodiscard]] uint32_t GetCount() const noexcept { return mCount; }

private:
    std::array<const core::ComponentInfo*, kMaxMessageTypes> mInfos{};
    uint32_t mCount = 0;
};

/// A sending system's outbox: messages recorded during its run, routed at the sync point.
/// Single-threaded by contract.
class MessageBuffer {
public:
    struct Record {
        MessageTypeIndex mType;
        EntityId mTarget; ///< invalid for a broadcast
        uint32_t mPayloadOffset = 0;
        uint32_t mPayloadSize   = 0;
    };

    explicit MessageBuffer(core::Allocator& allocator) noexcept;
    MessageBuffer(const MessageBuffer&)            = delete;
    MessageBuffer(MessageBuffer&&)                 = delete;
    MessageBuffer& operator=(const MessageBuffer&) = delete;
    MessageBuffer& operator=(MessageBuffer&&)      = delete;
    ~MessageBuffer()                               = default;

    /// Copies `size` bytes now. An invalid target broadcasts to every receiver of the type.
    void Send(MessageTypeIndex type, EntityId target, const void* data, uint32_t size) noexcept;

    template <typename T>
    void Send(MessageTypeIndex type, EntityId target, const T& value) noexcept {
        Send(type, target, &value, static_cast<uint32_t>(sizeof(T)));
    }

    [[nodiscard]] std::span<const Record> GetRecords() const noexcept { return mRecords; }
    [[nodiscard]] std::span<const std::byte> GetPayload() const noexcept { return mPayload; }
    [[nodiscard]] bool IsEmpty() const noexcept { return mRecords.empty(); }
    void Clear() noexcept;

private:
    core::MemoryResource mResource;
    std::pmr::vector<Record> mRecords;
    std::pmr::vector<std::byte> mPayload;
};

/// A receiving system's queue: one lane per message type, targets and payloads side by side.
/// Filled at the sync point, read by the system on its next run, cleared afterwards.
class MessageInbox {
public:
    explicit MessageInbox(core::Allocator& allocator) noexcept;
    MessageInbox(const MessageInbox&)            = delete;
    MessageInbox(MessageInbox&&)                 = delete;
    MessageInbox& operator=(const MessageInbox&) = delete;
    MessageInbox& operator=(MessageInbox&&)      = delete;
    ~MessageInbox()                              = default;

    void Push(MessageTypeIndex type, EntityId target, const void* data, uint32_t size) noexcept;

    [[nodiscard]] uint32_t Count(MessageTypeIndex type) const noexcept;
    /// Targets of the queued messages of `type`, in delivery order; an invalid id marks a
    /// broadcast.
    [[nodiscard]] std::span<const EntityId> Targets(MessageTypeIndex type) const noexcept;
    [[nodiscard]] std::span<const std::byte> Payload(MessageTypeIndex type) const noexcept;

    template <typename T>
    [[nodiscard]] std::span<const T> Payloads(MessageTypeIndex type) const noexcept {
        const std::span<const std::byte> bytes = Payload(type);
        return {reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T)};
    }

    [[nodiscard]] bool IsEmpty() const noexcept { return mTotal == 0; }
    void Clear() noexcept;

private:
    struct Lane {
        explicit Lane(std::pmr::memory_resource* resource) noexcept : mTargets(resource), mPayload(resource) {}

        std::pmr::vector<EntityId> mTargets;
        std::pmr::vector<std::byte> mPayload;
    };

    [[nodiscard]] Lane& LaneFor(MessageTypeIndex type) noexcept;

    core::MemoryResource mResource;
    std::pmr::vector<Lane> mLanes; ///< indexed by message type, grown on demand
    uint32_t mTotal = 0;
};

} // namespace oyi::ecs
