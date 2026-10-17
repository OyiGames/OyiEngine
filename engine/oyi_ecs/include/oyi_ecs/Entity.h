#pragma once

#include <oyi_core/Handle.h>

namespace oyi::ecs {

struct EntityTag {};

/// Runtime handle of an entity (ADR-0004): 32-bit index plus 32-bit generation, valid only within
/// the World that created it. Persistent identity (save games, networking) is a separate stable id.
using EntityId = core::Handle<EntityTag>;

/// What an entity is to the object layer (ADR-0024). Prototype: lives in a prototype archetype
/// that no query, snapshot or replication visits; cloned into instances. Locked: an instance
/// whose composition is fixed; AddComponent / RemoveComponent are refused, use enable bits.
enum class EntityMark : uint8_t {
    None      = 0,
    Prototype = 1,
    Locked    = 2,
};

} // namespace oyi::ecs
