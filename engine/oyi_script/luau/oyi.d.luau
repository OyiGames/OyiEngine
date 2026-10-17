-- Luau type definitions for the oyi library (ADR-0012). Load into luau-analyze (or the editor's
-- language server) with --definitions; the per-module <X>.d.luau files that tools/codegen writes
-- next to <X>.gen.h describe the table shapes of each reflected component and message.

-- An entity id: opaque, comparable, usable as a table key. Only functions do anything with it.
declare class Entity end

-- An entity created by oyi.create in the current window; a real Entity after the window closes.
declare class PendingEntity end

-- Any other 64-bit engine handle (a managed object, for one); opaque to scripts.
declare class Handle end

export type ComponentType = number
export type MessageType = number
-- A managed type (ADR-0024), named by its state block; scripts see the state block only.
export type ManagedType = number

-- What oyi.managed_type declares: the component and message types update may touch.
export type ManagedAccess = {
    reads: { ComponentType }?,
    writes: { ComponentType }?,
    receives: { MessageType }?,
    sends: { MessageType }?,
}

-- The restricted context of a scripted managed object's update(self, ctx): only the declared sets.
export type ManagedContext = {
    get: (type: ComponentType) -> any?,
    prev: (type: ComponentType) -> any?,
    set: (type: ComponentType, value: { [string]: any }) -> (),
    received: (type: MessageType) -> { any },
    send: (type: MessageType, target: Entity?, value: { [string]: any }) -> (),
    owner: () -> Entity,
    env: () -> Environment,
}

export type Environment = {
    frame: number,
    tick: number,
    fixed_step: number,
    time: number,
    delta: number,
}

declare oyi: {
    -- Type lookup by the fully qualified C++ name; usable at module top level.
    component: (name: string) -> ComponentType,
    message: (name: string) -> MessageType,
    managed: (name: string) -> ManagedType,
    -- Declares a managed type implemented by this module (its update(self, ctx), optional
    -- on_spawned(self, owner) / on_despawning(self, owner)); only at module top level.
    managed_type: (stateName: string, access: ManagedAccess) -> ManagedType,
    env: () -> Environment,

    -- Everything below needs an open firewall window: the function a Call runs in.
    is_alive: (entity: Entity) -> boolean,
    has: (entity: Entity, type: ComponentType) -> boolean,
    get: (entity: Entity, type: ComponentType) -> any?,
    prev: (entity: Entity, type: ComponentType) -> any?,
    state: (entity: Entity, type: ManagedType) -> any?,
    set_state: (entity: Entity, type: ManagedType, value: { [string]: any }) -> (),
    set: (entity: Entity | PendingEntity, type: ComponentType, value: { [string]: any }) -> (),
    send: (type: MessageType, target: Entity?, value: { [string]: any }) -> (),
    create: (types: { ComponentType }) -> PendingEntity,
    destroy: (entity: Entity) -> (),
    add: (entity: Entity, type: ComponentType) -> (),
    remove: (entity: Entity, type: ComponentType) -> (),
    entities: (all: { ComponentType }, none: { ComponentType }?) -> { Entity },
    entity_tostring: (entity: Entity) -> string,
}
