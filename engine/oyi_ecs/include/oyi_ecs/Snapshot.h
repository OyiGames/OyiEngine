#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/World.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

/// The Prev snapshot (ADR-0006): a read-only copy of chosen component types taken at a sync
/// point, so that main-thread code (managed components, scripts, tools) can read entity state
/// while jobs write the live data. Rows are memcpy'd per type into contiguous arrays and indexed
/// by entity, which is what the POD rules exist for (ADR-0011).
class Snapshot {
public:
    explicit Snapshot(core::Allocator& allocator) noexcept;
    Snapshot(const Snapshot&)            = delete;
    Snapshot(Snapshot&&)                 = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot& operator=(Snapshot&&)      = delete;
    ~Snapshot()                          = default;

    /// Replaces the contents with a copy of every live entity that has at least one of `types`.
    /// Only the types in the mask are copied. Memory is reused between captures.
    void Capture(const World& world, const ComponentMask& types) noexcept;
    void Clear() noexcept;

    /// World version at the time of the last capture; 0 before the first.
    [[nodiscard]] uint32_t GetVersion() const noexcept { return mVersion; }
    [[nodiscard]] uint32_t GetEntityCount() const noexcept { return static_cast<uint32_t>(mEntities.size()); }
    [[nodiscard]] const ComponentMask& GetTypes() const noexcept { return mTypes; }

    /// True when `entity` was alive and captured.
    [[nodiscard]] bool Contains(EntityId entity) const noexcept;
    /// The captured value, or nullptr when the entity was not captured, lacked the type, or the
    /// type is not part of the snapshot. Valid until the next Capture or Clear.
    [[nodiscard]] const void* Get(EntityId entity, ComponentTypeIndex type) const noexcept;

    template <typename T>
    [[nodiscard]] const T* Get(EntityId entity, ComponentTypeIndex type) const noexcept {
        return static_cast<const T*>(Get(entity, type));
    }

    /// Every captured entity, in capture order (archetype, chunk, row).
    [[nodiscard]] std::span<const EntityId> GetEntities() const noexcept { return mEntities; }
    /// Column of `type` for every captured entity: `GetEntities()[i]` owns element `i`, or holds
    /// an unset element when it lacks the type (see HasRow).
    [[nodiscard]] std::span<const std::byte> GetColumn(ComponentTypeIndex type) const noexcept;
    [[nodiscard]] bool HasRow(uint32_t row, ComponentTypeIndex type) const noexcept;

private:
    struct Column {
        explicit Column(std::pmr::memory_resource* resource) noexcept : mData(resource), mPresent(resource) {}

        ComponentTypeIndex mType;
        uint32_t mElementSize = 0;
        std::pmr::vector<std::byte> mData;
        std::pmr::vector<uint8_t> mPresent; ///< one flag per row
    };

    [[nodiscard]] const Column* FindColumn(ComponentTypeIndex type) const noexcept;
    [[nodiscard]] uint32_t RowOf(EntityId entity) const noexcept;

    static constexpr uint32_t kNoRow = 0xFFFF'FFFFu;

    core::MemoryResource mResource;
    ComponentMask mTypes;
    uint32_t mVersion = 0;
    std::pmr::vector<EntityId> mEntities;
    std::pmr::vector<uint32_t> mRowByIndex; ///< entity index -> row, kNoRow when absent
    std::pmr::vector<Column> mColumns;
};

} // namespace oyi::ecs
