#pragma once

#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/System.h>
#include <oyi_object/Components.h>
#include <oyi_object/ObjectLayer.h>
#include <oyi_object/Prototype.h>

#include <span>

namespace oyi::object {

/// Values that replace the prototype's on the root entity of a spawn (whole components).
struct SpawnOverrides {
    std::span<const ComponentValue> mValues;
};

/// The only way to create or destroy an object (ADR-0024). A thin recorder over a command
/// buffer: Spawn clones the prototype and its children, applies the overrides, links the roots
/// and parents and leaves a note for the layer's sync hook; Despawn leaves a note. Everything
/// takes effect at the next sync point, so a factory is as thread-private as its buffer.
class ObjectFactory {
public:
    ObjectFactory(ObjectLayer& layer, ecs::CommandBuffer& commands) noexcept : mLayer(layer), mCommands(commands) {}
    ObjectFactory(ObjectLayer& layer, ecs::SystemContext& context) noexcept
        : ObjectFactory(layer, context.GetCommands()) {}
    ObjectFactory(ObjectLayer& layer, ecs::ManagedContext& context) noexcept
        : ObjectFactory(layer, context.GetCommands()) {}
    ObjectFactory(ObjectLayer& layer, ecs::FirewallWindow& window) noexcept
        : ObjectFactory(layer, window.GetCommands()) {}

    /// The root entity of the new object, usable by later commands in the same buffer; invalid
    /// for an unknown prototype. Overrides whose type is not in the prototype are ignored.
    [[nodiscard]] ecs::PendingEntity Spawn(PrototypeId prototype, const SpawnOverrides& overrides = {}) noexcept;
    /// `entity` and every entity whose object root it is leave together: Despawning at the next
    /// sync point, destruction at the one after. Entities spawned at the same sync point wait
    /// one more, so nobody receives Spawned and Despawning together.
    void Despawn(ecs::EntityId entity) noexcept;

    [[nodiscard]] ObjectLayer& GetLayer() const noexcept { return mLayer; }

private:
    [[nodiscard]] ecs::PendingEntity SpawnNode(const Prototype& prototype, ecs::PendingEntity root,
                                               ecs::PendingEntity parent, const SpawnOverrides* overrides) noexcept;

    ObjectLayer& mLayer;
    ecs::CommandBuffer& mCommands;
};

} // namespace oyi::object
