#pragma once

struct lua_State;

namespace oyi::script {

class ScriptVm;

namespace detail {

/// Light userdata tags (ADR-0012: scripts hold ids, not memory).
inline constexpr int kEntityTag  = 1; ///< an EntityId packed into the pointer
inline constexpr int kPendingTag = 2; ///< a PendingEntity created in the current window
inline constexpr int kHandleTag  = 3; ///< any other 64-bit handle (ManagedId, ...)

/// Registers the global `oyi` table on the main state. Must run before luaL_sandbox.
void OpenOyiLibrary(lua_State* state, ScriptVm& vm) noexcept;

} // namespace detail
} // namespace oyi::script
