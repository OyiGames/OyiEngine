#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_core/Schema.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/World.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::net {

/// The replication hooks of ADR-0017: what the schema marks as replicated (fields, not
/// components; quantized where the annotation says so) can be encoded, decoded, diffed against a
/// baseline and applied, entity by entity. The transport, the authority model and interest
/// management are the networking layer's; nothing here knows about packets.

struct ReplicatedField {
    uint32_t mOffset      = 0;
    uint32_t mSize        = 0; ///< bytes of the whole field in the component
    uint32_t mCount       = 1; ///< array elements
    core::FieldKind mKind = core::FieldKind::Padding;
    float mQuantize       = 0.0f; ///< Float32 / Float64 fields with a step travel as int32 multiples
};

/// At most this many replicated fields per component: the delta mask is 64 bits.
inline constexpr uint32_t kMaxReplicatedFields = 64;

/// The replicated fields of every registered component type, built once per World.
class ReplicationSchema {
public:
    explicit ReplicationSchema(core::Allocator& allocator) noexcept;
    ReplicationSchema(const ReplicationSchema&)            = delete;
    ReplicationSchema(ReplicationSchema&&)                 = delete;
    ReplicationSchema& operator=(const ReplicationSchema&) = delete;
    ReplicationSchema& operator=(ReplicationSchema&&)      = delete;
    ~ReplicationSchema()                                   = default;

    /// Reads the component infos of `registry`. Fails with OutOfRange when a component has more
    /// than kMaxReplicatedFields replicated fields.
    [[nodiscard]] core::Result Build(const ecs::ComponentRegistry& registry) noexcept;

    [[nodiscard]] bool IsReplicated(ecs::ComponentTypeIndex type) const noexcept;
    [[nodiscard]] std::span<const ReplicatedField> GetFields(ecs::ComponentTypeIndex type) const noexcept;
    /// Bytes EncodeComponent writes for `type`; 0 when not replicated.
    [[nodiscard]] uint32_t GetEncodedSize(ecs::ComponentTypeIndex type) const noexcept;
    [[nodiscard]] const ecs::ComponentMask& GetReplicatedTypes() const noexcept { return mReplicated; }
    [[nodiscard]] uint32_t GetReplicatedTypeCount() const noexcept { return mReplicated.Count(); }

private:
    struct Entry {
        uint32_t mFirst       = 0; ///< index into mFields
        uint32_t mCount       = 0;
        uint32_t mEncodedSize = 0;
    };

    core::MemoryResource mResource;
    std::pmr::vector<ReplicatedField> mFields;
    std::array<Entry, ecs::kMaxComponentTypes> mEntries{};
    ecs::ComponentMask mReplicated;
};

/// Writes the replicated fields of `value` (a component of `type`) into `out`: quantized floats
/// as little-endian int32 multiples of the step, everything else as its raw bytes. Returns the
/// byte count. Fails with InvalidArgument for a type that is not replicated and OutOfRange when
/// `out` is too small.
[[nodiscard]] core::Expected<uint32_t> EncodeComponent(const ReplicationSchema& schema, ecs::ComponentTypeIndex type,
                                                       const void* value, std::span<std::byte> out) noexcept;
/// The reverse: overwrites the replicated fields of `value`, leaves the rest alone.
[[nodiscard]] core::Result DecodeComponent(const ReplicationSchema& schema, ecs::ComponentTypeIndex type,
                                           std::span<const std::byte> in, void* value) noexcept;

/// A 64-bit mask of the fields whose encoding differs between `baseline` and `current`, then the
/// encodings of those fields only. Returns the byte count (8 for "nothing changed").
[[nodiscard]] core::Expected<uint32_t> EncodeDelta(const ReplicationSchema& schema, ecs::ComponentTypeIndex type,
                                                   const void* baseline, const void* current,
                                                   std::span<std::byte> out) noexcept;
[[nodiscard]] core::Result ApplyDelta(const ReplicationSchema& schema, ecs::ComponentTypeIndex type,
                                      std::span<const std::byte> in, void* value) noexcept;

/// Every replicated component the entity has, ascending by type: a 16-bit type index and the
/// component's encoding each, closed by 0xFFFF. Fails with NotFound for a dead entity.
[[nodiscard]] core::Expected<uint32_t> EncodeEntity(const ReplicationSchema& schema, const ecs::World& world,
                                                    ecs::EntityId entity, std::span<std::byte> out) noexcept;
/// Applies an EncodeEntity record onto `entity`, which must have every type the record names;
/// a missing type fails with NotFound after the earlier types were applied.
[[nodiscard]] core::Result ApplyEntity(const ReplicationSchema& schema, ecs::World& world, ecs::EntityId entity,
                                       std::span<const std::byte> in) noexcept;

} // namespace oyi::net
