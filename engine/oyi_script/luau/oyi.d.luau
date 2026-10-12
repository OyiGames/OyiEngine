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
    env: () -> Environment,

    -- Everything below needs an open firewall window: the function a Call runs in.
    is_alive: (entity: Entity) -> boolean,
    has: (entity: Entity, type: ComponentType) -> boolean,
    get: (entity: Entity, type: ComponentType) -> any?,
    prev: (entity: Entity, type: ComponentType) -> any?,
    set: (entity: Entity | PendingEntity, type: ComponentType, value: { [string]: any }) -> (),
    send: (type: MessageType, target: Entity?, value: { [string]: any }) -> (),
    create: (types: { ComponentType }) -> PendingEntity,
    destroy: (entity: Entity) -> (),
    add: (entity: Entity, type: ComponentType) -> (),
    remove: (entity: Entity, type: ComponentType) -> (),
    entities: (all: { ComponentType }, none: { ComponentType }?) -> { Entity },
    entity_tostring: (entity: Entity) -> string,
}
