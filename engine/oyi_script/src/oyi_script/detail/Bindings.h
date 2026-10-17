#pragma once

#include <oyi_core/Schema.h>
#include <oyi_ecs/Entity.h>

#include <cstddef>

struct lua_State;

namespace oyi::script {

class ScriptManagedObject;
class ScriptVm;

namespace detail {

/// Light userdata tags (ADR-0012: scripts hold ids, not memory).
inline constexpr int kEntityTag  = 1; ///< an EntityId packed into the pointer
inline constexpr int kPendingTag = 2; ///< a PendingEntity created in the current window
inline constexpr int kHandleTag  = 3; ///< any other 64-bit handle (ManagedId, ...)

/// Registers the global `oyi` table on the main state. Must run before luaL_sandbox.
void OpenOyiLibrary(lua_State* state, ScriptVm& vm) noexcept;

/// Reflection marshalling shared with the VM: a record's bytes as a table, and a table's
/// fields back into bytes (absent fields keep theirs).
void PushRecord(lua_State* state, const ScriptVm& vm, const core::ComponentInfo& info, const std::byte* data);
void ReadRecord(lua_State* state, const ScriptVm& vm, const core::ComponentInfo& info, int tableIndex, std::byte* data);
void PushEntity(lua_State* state, ecs::EntityId entity) noexcept;
/// The restricted `ctx` of a scripted managed object's update (ScriptManaged.h): a table of
/// closures over the object that only honour its declared access sets.
void PushManagedContext(lua_State* state, ScriptManagedObject& object);

} // namespace detail
} // namespace oyi::script
