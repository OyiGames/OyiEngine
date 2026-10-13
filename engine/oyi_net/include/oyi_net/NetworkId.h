#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/Entity.h>

#include <cstdint>
#include <memory_resource>
#include <vector>

namespace oyi::net {

/// A stable identity that survives the wire (ADR-0017): the server assigns one per replicated
/// entity, every client maps it to the handle of its own copy. Zero is never an id.
struct NetworkId {
    uint32_t mValue = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return mValue != 0; }
    [[nodiscard]] constexpr bool operator==(const NetworkId&) const noexcept = default;
};

/// Both directions of the mapping for one World.
class NetworkIdMap {
public:
    explicit NetworkIdMap(core::Allocator& allocator) noexcept;
    NetworkIdMap(const NetworkIdMap&)            = delete;
    NetworkIdMap(NetworkIdMap&&)                 = delete;
    NetworkIdMap& operator=(const NetworkIdMap&) = delete;
    NetworkIdMap& operator=(NetworkIdMap&&)      = delete;
    ~NetworkIdMap()                              = default;

    /// The authority's side: a fresh id for `entity`. Fails with AlreadyExists when it has one.
    [[nodiscard]] core::Expected<NetworkId> Assign(ecs::EntityId entity) noexcept;
    /// A replica's side: `id` (chosen elsewhere) now means `entity` here. Fails with
    /// AlreadyExists when either is bound already.
    [[nodiscard]] core::Result Bind(NetworkId id, ecs::EntityId entity) noexcept;
    /// Forgets the pair; a no-op for an unknown id.
    void Release(NetworkId id) noexcept;

    [[nodiscard]] ecs::EntityId Resolve(NetworkId id) const noexcept;
    [[nodiscard]] NetworkId Find(ecs::EntityId entity) const noexcept;
    [[nodiscard]] uint32_t GetCount() const noexcept { return mCount; }

private:
    struct Pair {
        NetworkId mId;
        ecs::EntityId mEntity;
    };

    core::MemoryResource mResource;
    std::pmr::vector<Pair> mPairs; ///< sorted by id
    uint32_t mNextId = 1;
    uint32_t mCount  = 0;
};

} // namespace oyi::net
