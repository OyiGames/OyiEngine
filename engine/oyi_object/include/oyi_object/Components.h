#pragma once

// The object layer's reflected types (ADR-0024): what every factory-made entity carries and the
// two lifecycle messages. Run through tools/codegen like any engine module.

#include <oyi_core/Annotations.h>
#include <oyi_core/Handle.h>
#include <oyi_ecs/Entity.h>

namespace oyi::object {

/// A registered prototype (PrototypeRegistry): index + generation, like every engine handle.
struct PrototypeTag {};
using PrototypeId = core::Handle<PrototypeTag>;

/// On every entity the factory spawns: which prototype it came from and the root of its object
/// (itself for a root). Despawn(root) takes every entity with that root along.
struct OYI_COMPONENT(ReadOnly, Description = "Prototype and object root of a spawned entity") ObjectInfo {
    PrototypeId mPrototype{};
    oyi::ecs::EntityId mRoot{};
};

/// Sent to an entity at the sync point that materialised it, after every managed object of the
/// same batch exists and ran OnSpawned.
struct OYI_STRUCT() Spawned {
    PrototypeId mPrototype{};
};

/// Sent to an entity one sync point before the factory destroys it; the entity is still alive
/// when the receivers run.
struct OYI_STRUCT() Despawning {
    PrototypeId mPrototype{};
};

} // namespace oyi::object
