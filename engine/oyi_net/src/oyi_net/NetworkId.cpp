#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_ecs/Entity.h>
#include <oyi_net/NetworkId.h>

#include <algorithm>

namespace oyi::net {

NetworkIdMap::NetworkIdMap(core::Allocator& allocator) noexcept : mResource(allocator), mPairs(&mResource) {}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<NetworkId> NetworkIdMap::Assign(ecs::EntityId entity) noexcept {
    if (Find(entity).IsValid()) {
        return core::Fail(core::ErrorCode::AlreadyExists, "NetworkIdMap: the entity already has an id");
    }
    const NetworkId id{.mValue = mNextId++};
    mPairs.push_back(Pair{.mId = id, .mEntity = entity}); // ids grow, so the back keeps the order
    ++mCount;
    return id;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result NetworkIdMap::Bind(NetworkId id, ecs::EntityId entity) noexcept {
    if (!id.IsValid()) {
        return core::Fail(core::ErrorCode::InvalidArgument, "NetworkIdMap: zero is not an id");
    }
    if (Resolve(id).IsValid() || Find(entity).IsValid()) {
        return core::Fail(core::ErrorCode::AlreadyExists, "NetworkIdMap: id or entity already bound");
    }
    const auto at = std::ranges::lower_bound(mPairs, id.mValue, {}, [](const Pair& pair) { return pair.mId.mValue; });
    mPairs.insert(at, Pair{.mId = id, .mEntity = entity});
    mNextId = std::max(mNextId, id.mValue + 1);
    ++mCount;
    return {};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void NetworkIdMap::Release(NetworkId id) noexcept {
    const auto at = std::ranges::lower_bound(mPairs, id.mValue, {}, [](const Pair& pair) { return pair.mId.mValue; });
    if (at != mPairs.end() && at->mId == id) {
        mPairs.erase(at);
        --mCount;
    }
}

ecs::EntityId NetworkIdMap::Resolve(NetworkId id) const noexcept {
    const auto at = std::ranges::lower_bound(mPairs, id.mValue, {}, [](const Pair& pair) { return pair.mId.mValue; });
    return at != mPairs.end() && at->mId == id ? at->mEntity : ecs::EntityId::Invalid();
}

NetworkId NetworkIdMap::Find(ecs::EntityId entity) const noexcept {
    for (const Pair& pair : mPairs) {
        if (pair.mEntity == entity) {
            return pair.mId;
        }
    }
    return NetworkId{};
}

} // namespace oyi::net
