#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/World.h>
#include <oyi_net/Replication.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace oyi::net {

namespace {

using core::FieldKind;
using ecs::ComponentTypeIndex;

[[nodiscard]] bool IsQuantized(const ReplicatedField& field) noexcept {
    return field.mQuantize > 0.0f && (field.mKind == FieldKind::Float32 || field.mKind == FieldKind::Float64);
}

/// Bytes one element of the field takes on the wire.
[[nodiscard]] uint32_t WireElementSize(const ReplicatedField& field) noexcept {
    return IsQuantized(field) ? sizeof(int32_t) : field.mSize / field.mCount;
}

[[nodiscard]] uint32_t WireSize(const ReplicatedField& field) noexcept {
    return WireElementSize(field) * field.mCount;
}

void EncodeField(const ReplicatedField& field, const std::byte* value, std::byte* out) noexcept {
    const uint32_t elementSize = field.mSize / field.mCount;
    for (uint32_t i = 0; i < field.mCount; ++i) {
        const std::byte* const element = value + field.mOffset + (static_cast<size_t>(i) * elementSize);
        std::byte* const target        = out + (static_cast<size_t>(i) * WireElementSize(field));
        if (!IsQuantized(field)) {
            std::memcpy(target, element, elementSize);
            continue;
        }
        double real = 0.0;
        if (field.mKind == FieldKind::Float32) {
            float single = 0.0f;
            std::memcpy(&single, element, sizeof(single));
            real = static_cast<double>(single);
        } else {
            std::memcpy(&real, element, sizeof(real));
        }
        const auto steps = static_cast<int32_t>(std::lround(real / static_cast<double>(field.mQuantize)));
        std::memcpy(target, &steps, sizeof(steps));
    }
}

void DecodeField(const ReplicatedField& field, const std::byte* in, std::byte* value) noexcept {
    const uint32_t elementSize = field.mSize / field.mCount;
    for (uint32_t i = 0; i < field.mCount; ++i) {
        std::byte* const element      = value + field.mOffset + (static_cast<size_t>(i) * elementSize);
        const std::byte* const source = in + (static_cast<size_t>(i) * WireElementSize(field));
        if (!IsQuantized(field)) {
            std::memcpy(element, source, elementSize);
            continue;
        }
        int32_t steps = 0;
        std::memcpy(&steps, source, sizeof(steps));
        const double real = static_cast<double>(steps) * static_cast<double>(field.mQuantize);
        if (field.mKind == FieldKind::Float32) {
            const auto single = static_cast<float>(real);
            std::memcpy(element, &single, sizeof(single));
        } else {
            std::memcpy(element, &real, sizeof(real));
        }
    }
}

} // namespace

ReplicationSchema::ReplicationSchema(core::Allocator& allocator) noexcept : mResource(allocator), mFields(&mResource) {}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result ReplicationSchema::Build(const ecs::ComponentRegistry& registry) noexcept {
    mFields.clear();
    mEntries    = {};
    mReplicated = ecs::ComponentMask{};
    for (uint32_t i = 0; i < registry.GetCount(); ++i) {
        const ComponentTypeIndex type{.mValue = static_cast<uint16_t>(i)};
        const core::ComponentInfo& info = registry.GetInfo(type);
        if (!HasFlag(info.mFlags, core::ComponentFlags::Replicated)) {
            continue;
        }
        Entry entry;
        entry.mFirst = static_cast<uint32_t>(mFields.size());
        for (const core::FieldInfo& field : info.mFields) {
            if (field.mKind == FieldKind::Padding || !HasFlag(field.mFlags, core::FieldFlags::Replicated)) {
                continue;
            }
            const ReplicatedField replicated{.mOffset   = field.mOffset,
                                             .mSize     = field.mSize,
                                             .mCount    = field.mCount,
                                             .mKind     = field.mKind,
                                             .mQuantize = field.mQuantize};
            mFields.push_back(replicated);
            entry.mEncodedSize += WireSize(replicated);
            ++entry.mCount;
        }
        if (entry.mCount > kMaxReplicatedFields) {
            mFields.clear();
            mEntries    = {};
            mReplicated = ecs::ComponentMask{};
            return core::Fail(core::ErrorCode::OutOfRange,
                              "ReplicationSchema: too many replicated fields in one component");
        }
        if (entry.mCount == 0) {
            continue; // a Replicated flag with nothing to send: the tag case
        }
        mEntries[i] = entry;
        mReplicated.Set(type);
    }
    return {};
}

bool ReplicationSchema::IsReplicated(ComponentTypeIndex type) const noexcept {
    return type.IsValid() && mReplicated.Test(type);
}

std::span<const ReplicatedField> ReplicationSchema::GetFields(ComponentTypeIndex type) const noexcept {
    if (!IsReplicated(type)) {
        return {};
    }
    const Entry& entry = mEntries[type.mValue];
    return {mFields.data() + entry.mFirst, entry.mCount};
}

uint32_t ReplicationSchema::GetEncodedSize(ComponentTypeIndex type) const noexcept {
    return IsReplicated(type) ? mEntries[type.mValue].mEncodedSize : 0;
}

core::Expected<uint32_t> EncodeComponent(const ReplicationSchema& schema, ComponentTypeIndex type, const void* value,
                                         std::span<std::byte> out) noexcept {
    if (!schema.IsReplicated(type)) {
        return core::Fail(core::ErrorCode::InvalidArgument, "replication: the type is not replicated");
    }
    if (out.size() < schema.GetEncodedSize(type)) {
        return core::Fail(core::ErrorCode::OutOfRange, "replication: output too small");
    }
    const auto* const bytes = static_cast<const std::byte*>(value);
    uint32_t written        = 0;
    for (const ReplicatedField& field : schema.GetFields(type)) {
        EncodeField(field, bytes, out.data() + written);
        written += WireSize(field);
    }
    return written;
}

core::Result DecodeComponent(const ReplicationSchema& schema, ComponentTypeIndex type, std::span<const std::byte> in,
                             void* value) noexcept {
    if (!schema.IsReplicated(type)) {
        return core::Fail(core::ErrorCode::InvalidArgument, "replication: the type is not replicated");
    }
    if (in.size() < schema.GetEncodedSize(type)) {
        return core::Fail(core::ErrorCode::CorruptData, "replication: input too short");
    }
    auto* const bytes = static_cast<std::byte*>(value);
    uint32_t read     = 0;
    for (const ReplicatedField& field : schema.GetFields(type)) {
        DecodeField(field, in.data() + read, bytes);
        read += WireSize(field);
    }
    return {};
}

core::Expected<uint32_t> EncodeDelta(const ReplicationSchema& schema, ComponentTypeIndex type, const void* baseline,
                                     const void* current, std::span<std::byte> out) noexcept {
    if (!schema.IsReplicated(type)) {
        return core::Fail(core::ErrorCode::InvalidArgument, "replication: the type is not replicated");
    }
    if (out.size() < sizeof(uint64_t)) {
        return core::Fail(core::ErrorCode::OutOfRange, "replication: output too small");
    }
    const auto* const before = static_cast<const std::byte*>(baseline);
    const auto* const after  = static_cast<const std::byte*>(current);
    uint64_t mask            = 0;
    uint32_t written         = sizeof(uint64_t);
    uint32_t index           = 0;
    std::array<std::byte, 256> scratchBefore{};
    std::array<std::byte, 256> scratchAfter{};
    for (const ReplicatedField& field : schema.GetFields(type)) {
        const uint32_t size = WireSize(field);
        if (size > scratchBefore.size()) {
            return core::Fail(core::ErrorCode::OutOfRange, "replication: a field is too large for a delta");
        }
        EncodeField(field, before, scratchBefore.data());
        EncodeField(field, after, scratchAfter.data());
        if (std::memcmp(scratchBefore.data(), scratchAfter.data(), size) != 0) {
            if (out.size() - written < size) {
                return core::Fail(core::ErrorCode::OutOfRange, "replication: output too small");
            }
            std::memcpy(out.data() + written, scratchAfter.data(), size);
            written += size;
            mask |= uint64_t{1} << index;
        }
        ++index;
    }
    std::memcpy(out.data(), &mask, sizeof(mask));
    return written;
}

core::Result ApplyDelta(const ReplicationSchema& schema, ComponentTypeIndex type, std::span<const std::byte> in,
                        void* value) noexcept {
    if (!schema.IsReplicated(type)) {
        return core::Fail(core::ErrorCode::InvalidArgument, "replication: the type is not replicated");
    }
    if (in.size() < sizeof(uint64_t)) {
        return core::Fail(core::ErrorCode::CorruptData, "replication: input too short");
    }
    uint64_t mask = 0;
    std::memcpy(&mask, in.data(), sizeof(mask));
    auto* const bytes = static_cast<std::byte*>(value);
    uint32_t read     = sizeof(uint64_t);
    uint32_t index    = 0;
    for (const ReplicatedField& field : schema.GetFields(type)) {
        if ((mask & (uint64_t{1} << index)) != 0) {
            const uint32_t size = WireSize(field);
            if (in.size() - read < size) {
                return core::Fail(core::ErrorCode::CorruptData, "replication: input too short");
            }
            DecodeField(field, in.data() + read, bytes);
            read += size;
        }
        ++index;
    }
    return {};
}

core::Expected<uint32_t> EncodeEntity(const ReplicationSchema& schema, const ecs::World& world, ecs::EntityId entity,
                                      std::span<std::byte> out) noexcept {
    const ecs::ComponentMask* const mask = world.GetComponentMask(entity);
    if (mask == nullptr) {
        return core::Fail(core::ErrorCode::NotFound, "replication: the entity is not alive");
    }
    uint32_t written               = 0;
    core::Result failure           = {};
    const ecs::ComponentMask types = mask->And(schema.GetReplicatedTypes());
    types.ForEach([&](ComponentTypeIndex type) {
        if (!failure.has_value()) {
            return;
        }
        const void* const value = world.GetComponent(entity, type);
        if (value == nullptr || out.size() - written < sizeof(uint16_t)) {
            failure = core::Fail(core::ErrorCode::OutOfRange, "replication: output too small");
            return;
        }
        std::memcpy(out.data() + written, &type.mValue, sizeof(uint16_t));
        written += sizeof(uint16_t);
        const core::Expected<uint32_t> encoded = EncodeComponent(schema, type, value, out.subspan(written));
        if (!encoded.has_value()) {
            failure = core::Fail(encoded.error().mCode, encoded.error().mMessage);
            return;
        }
        written += *encoded;
    });
    if (!failure.has_value()) {
        return core::Fail(failure.error().mCode, failure.error().mMessage);
    }
    if (out.size() - written < sizeof(uint16_t)) {
        return core::Fail(core::ErrorCode::OutOfRange, "replication: output too small");
    }
    const uint16_t end = 0xFFFFu;
    std::memcpy(out.data() + written, &end, sizeof(end));
    return static_cast<uint32_t>(written + sizeof(uint16_t));
}

core::Result ApplyEntity(const ReplicationSchema& schema, ecs::World& world, ecs::EntityId entity,
                         std::span<const std::byte> in) noexcept {
    if (!world.IsAlive(entity)) {
        return core::Fail(core::ErrorCode::NotFound, "replication: the entity is not alive");
    }
    uint32_t read = 0;
    while (true) {
        if (in.size() - read < sizeof(uint16_t)) {
            return core::Fail(core::ErrorCode::CorruptData, "replication: record not closed");
        }
        uint16_t value = 0;
        std::memcpy(&value, in.data() + read, sizeof(value));
        read += sizeof(uint16_t);
        if (value == 0xFFFFu) {
            return {};
        }
        const ComponentTypeIndex type{.mValue = value};
        void* const target = world.GetComponentForWrite(entity, type);
        if (target == nullptr || !schema.IsReplicated(type)) {
            return core::Fail(core::ErrorCode::NotFound,
                              "replication: the entity lacks a replicated type of the record");
        }
        const uint32_t size = schema.GetEncodedSize(type);
        if (in.size() - read < size) {
            return core::Fail(core::ErrorCode::CorruptData, "replication: input too short");
        }
        const core::Result decoded = DecodeComponent(schema, type, in.subspan(read, size), target);
        if (!decoded.has_value()) {
            return decoded;
        }
        read += size;
    }
}

} // namespace oyi::net
