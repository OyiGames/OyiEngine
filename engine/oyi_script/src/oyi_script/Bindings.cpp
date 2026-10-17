#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/World.h>
#include <oyi_script/ScriptVm.h>
#include <oyi_script/detail/Bindings.h>

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

// The Luau C API is a C API: varargs in luaL_error and pointer games for light userdata are
// its idiom, not ours.
// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)

namespace oyi::script::detail {

namespace {

using core::ComponentInfo;
using core::FieldInfo;
using core::FieldKind;
using ecs::ComponentMask;
using ecs::ComponentTypeIndex;
using ecs::EntityId;
using ecs::FirewallWindow;
using ecs::ManagedId;
using ecs::ManagedTypeInfo;
using ecs::MessageTypeIndex;
using ecs::PendingEntity;

/// Codegen hashes the canonical spelling of a handle field, not the alias written in the source.
constexpr core::TypeId kEntityIdType = core::MakeTypeId("oyi::core::Handle<oyi::ecs::EntityTag>");

/// Field names are string_views; the Luau API wants C strings.
struct Key {
    std::array<char, 96> mChars{};

    explicit Key(std::string_view name) noexcept {
        const size_t count = std::min(name.size(), mChars.size() - 1);
        std::memcpy(mChars.data(), name.data(), count);
        mChars[count] = '\0';
    }

    [[nodiscard]] const char* CStr() const noexcept { return mChars.data(); }
};

[[nodiscard]] ScriptVm& VmOf(lua_State* state) noexcept {
    return *static_cast<ScriptVm*>(lua_callbacks(state)->userdata);
}

[[nodiscard]] FirewallWindow& WindowOf(lua_State* state) {
    FirewallWindow* const window = VmOf(state).GetWindow();
    if (window == nullptr) {
        luaL_error(state, "oyi: entity access needs an open firewall window");
    }
    return *window;
}

// --- handles ------------------------------------------------------------------------------------

void PushEntity(lua_State* state, EntityId entity) noexcept {
    lua_pushlightuserdatatagged(state, reinterpret_cast<void*>(static_cast<uintptr_t>(entity.ToU64())), kEntityTag);
}

void PushHandle(lua_State* state, uint64_t packed) noexcept {
    lua_pushlightuserdatatagged(state, reinterpret_cast<void*>(static_cast<uintptr_t>(packed)), kHandleTag);
}

void PushPending(lua_State* state, PendingEntity pending) noexcept {
    lua_pushlightuserdatatagged(state, reinterpret_cast<void*>(static_cast<uintptr_t>(pending.mIndex)), kPendingTag);
}

[[nodiscard]] bool IsTagged(lua_State* state, int index, int tag) noexcept {
    return lua_type(state, index) == LUA_TLIGHTUSERDATA && lua_lightuserdatatag(state, index) == tag;
}

[[nodiscard]] uint64_t PackedOf(lua_State* state, int index) noexcept {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(lua_tolightuserdata(state, index)));
}

[[nodiscard]] EntityId CheckEntity(lua_State* state, int index) {
    if (!IsTagged(state, index, kEntityTag)) {
        luaL_typeerror(state, index, "entity");
    }
    return EntityId::FromU64(PackedOf(state, index));
}

[[nodiscard]] PendingEntity CheckPending(lua_State* state, int index) {
    if (!IsTagged(state, index, kPendingTag)) {
        luaL_typeerror(state, index, "pending entity");
    }
    return PendingEntity{.mIndex = static_cast<uint32_t>(PackedOf(state, index))};
}

[[nodiscard]] ComponentTypeIndex CheckComponent(lua_State* state, int index) {
    const ComponentTypeIndex type{.mValue = static_cast<uint16_t>(luaL_checkinteger(state, index))};
    if (!VmOf(state).GetWorld().GetComponents().IsRegistered(type)) {
        luaL_argerror(state, index, "unknown component type");
    }
    return type;
}

[[nodiscard]] MessageTypeIndex CheckMessage(lua_State* state, int index) {
    const MessageTypeIndex type{.mValue = static_cast<uint16_t>(luaL_checkinteger(state, index))};
    if (!VmOf(state).GetWorld().GetMessages().IsRegistered(type)) {
        luaL_argerror(state, index, "unknown message type");
    }
    return type;
}

/// A managed type handle (oyi.managed): the index into the world's managed type registry.
[[nodiscard]] const ManagedTypeInfo& CheckManaged(lua_State* state, int index) {
    const std::span<const ManagedTypeInfo* const> types = VmOf(state).GetWorld().GetManagedTypes().GetAll();
    const int value                                     = luaL_checkinteger(state, index);
    if (value < 0 || static_cast<size_t>(value) >= types.size()) {
        luaL_argerror(state, index, "unknown managed type");
    }
    return *types[static_cast<size_t>(value)];
}

/// An array table of component type indices at `index`.
[[nodiscard]] ComponentMask CheckMask(lua_State* state, int index) {
    luaL_checktype(state, index, LUA_TTABLE);
    ComponentMask mask;
    const int count = lua_objlen(state, index);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, i);
        mask.Set(CheckComponent(state, -1));
        lua_pop(state, 1);
    }
    return mask;
}

// --- reflection: bytes <-> tables -------------------------------------------------------------

void PushRecord(lua_State* state, const ScriptVm& vm, const ComponentInfo& info, const std::byte* data);
void ReadRecord(lua_State* state, const ScriptVm& vm, const ComponentInfo& info, int tableIndex, std::byte* data);

template <typename T>
[[nodiscard]] double LoadAs(const std::byte* at) noexcept {
    T value{};
    std::memcpy(&value, at, sizeof(T));
    return static_cast<double>(value);
}

template <typename T>
void StoreAs(std::byte* at, double value) noexcept {
    const T typed = static_cast<T>(value);
    std::memcpy(at, &typed, sizeof(T));
}

[[nodiscard]] double LoadInteger(const std::byte* at, uint32_t size, bool isSigned) noexcept {
    switch (size) {
    case 1:
        return isSigned ? LoadAs<int8_t>(at) : LoadAs<uint8_t>(at);
    case 2:
        return isSigned ? LoadAs<int16_t>(at) : LoadAs<uint16_t>(at);
    case 4:
        return isSigned ? LoadAs<int32_t>(at) : LoadAs<uint32_t>(at);
    default:
        return isSigned ? LoadAs<int64_t>(at) : LoadAs<uint64_t>(at);
    }
}

void StoreInteger(std::byte* at, uint32_t size, bool isSigned, double value) noexcept {
    if (isSigned) {
        switch (size) {
        case 1:
            StoreAs<int8_t>(at, value);
            return;
        case 2:
            StoreAs<int16_t>(at, value);
            return;
        case 4:
            StoreAs<int32_t>(at, value);
            return;
        default:
            StoreAs<int64_t>(at, value);
            return;
        }
    }
    switch (size) {
    case 1:
        StoreAs<uint8_t>(at, value);
        return;
    case 2:
        StoreAs<uint16_t>(at, value);
        return;
    case 4:
        StoreAs<uint32_t>(at, value);
        return;
    default:
        StoreAs<uint64_t>(at, value);
        return;
    }
}

[[nodiscard]] bool IsSignedKind(FieldKind kind) noexcept {
    return kind == FieldKind::Int8 || kind == FieldKind::Int16 || kind == FieldKind::Int32 ||
           kind == FieldKind::Int64 || kind == FieldKind::Enum;
}

/// Pushes one element of a field (the element at `at`, `elementSize` bytes).
void PushElement(lua_State* state, const ScriptVm& vm, const FieldInfo& field, const std::byte* at,
                 uint32_t elementSize) {
    switch (field.mKind) {
    case FieldKind::Bool:
        lua_pushboolean(state, LoadAs<uint8_t>(at) != 0 ? 1 : 0);
        return;
    case FieldKind::Float32:
        lua_pushnumber(state, LoadAs<float>(at));
        return;
    case FieldKind::Float64:
        lua_pushnumber(state, LoadAs<double>(at));
        return;
    case FieldKind::Int8:
    case FieldKind::UInt8:
    case FieldKind::Int16:
    case FieldKind::UInt16:
    case FieldKind::Int32:
    case FieldKind::UInt32:
    case FieldKind::Int64:
    case FieldKind::UInt64:
    case FieldKind::Enum:
        lua_pushnumber(state, LoadInteger(at, elementSize, IsSignedKind(field.mKind)));
        return;
    case FieldKind::Struct: {
        if (field.mTypeId == kEntityIdType && elementSize == sizeof(EntityId)) {
            uint64_t packed = 0;
            std::memcpy(&packed, at, sizeof(packed));
            PushEntity(state, EntityId::FromU64(packed));
            return;
        }
        if (const ComponentInfo* const nested = vm.FindInfo(field.mTypeId); nested != nullptr) {
            PushRecord(state, vm, *nested, at);
            return;
        }
        if (elementSize == sizeof(uint64_t)) {
            uint64_t packed = 0;
            std::memcpy(&packed, at, sizeof(packed));
            PushHandle(state, packed);
            return;
        }
        lua_pushnil(state); // an unknown value type: register it with the VM to see it
        return;
    }
    case FieldKind::Padding:
        lua_pushnil(state);
        return;
    }
    lua_pushnil(state);
}

void ReadElement(lua_State* state, const ScriptVm& vm, const FieldInfo& field, int valueIndex, std::byte* at,
                 uint32_t elementSize) {
    switch (field.mKind) {
    case FieldKind::Bool:
        StoreAs<uint8_t>(at, lua_toboolean(state, valueIndex) != 0 ? 1.0 : 0.0);
        return;
    case FieldKind::Float32:
        StoreAs<float>(at, luaL_checknumber(state, valueIndex));
        return;
    case FieldKind::Float64:
        StoreAs<double>(at, luaL_checknumber(state, valueIndex));
        return;
    case FieldKind::Int8:
    case FieldKind::UInt8:
    case FieldKind::Int16:
    case FieldKind::UInt16:
    case FieldKind::Int32:
    case FieldKind::UInt32:
    case FieldKind::Int64:
    case FieldKind::UInt64:
    case FieldKind::Enum:
        StoreInteger(at, elementSize, IsSignedKind(field.mKind), luaL_checknumber(state, valueIndex));
        return;
    case FieldKind::Struct: {
        if (field.mTypeId == kEntityIdType && elementSize == sizeof(EntityId)) {
            const uint64_t packed = CheckEntity(state, valueIndex).ToU64();
            std::memcpy(at, &packed, sizeof(packed));
            return;
        }
        if (const ComponentInfo* const nested = vm.FindInfo(field.mTypeId); nested != nullptr) {
            luaL_checktype(state, valueIndex, LUA_TTABLE);
            ReadRecord(state, vm, *nested, valueIndex, at);
            return;
        }
        if (elementSize == sizeof(uint64_t) && IsTagged(state, valueIndex, kHandleTag)) {
            const uint64_t packed = PackedOf(state, valueIndex);
            std::memcpy(at, &packed, sizeof(packed));
            return;
        }
        luaL_error(state, "oyi: field %s has a type scripts cannot write", Key(field.mName).CStr());
    }
    case FieldKind::Padding:
        return;
    }
}

void PushRecord(lua_State* state, const ScriptVm& vm, const ComponentInfo& info, const std::byte* data) {
    lua_createtable(state, 0, static_cast<int>(info.mFields.size()));
    for (const FieldInfo& field : info.mFields) {
        if (field.mKind == FieldKind::Padding) {
            continue;
        }
        const uint32_t elementSize = field.mSize / field.mCount;
        if (field.mCount == 1) {
            PushElement(state, vm, field, data + field.mOffset, elementSize);
        } else {
            lua_createtable(state, static_cast<int>(field.mCount), 0);
            for (uint32_t i = 0; i < field.mCount; ++i) {
                PushElement(state, vm, field, data + field.mOffset + (static_cast<size_t>(i) * elementSize),
                            elementSize);
                lua_rawseti(state, -2, static_cast<int>(i + 1));
            }
        }
        lua_setfield(state, -2, Key(field.mName).CStr());
    }
}

/// Overwrites the fields present in the table at `tableIndex`; absent fields keep their bytes.
void ReadRecord(lua_State* state, const ScriptVm& vm, const ComponentInfo& info, int tableIndex, std::byte* data) {
    const int table = lua_absindex(state, tableIndex);
    for (const FieldInfo& field : info.mFields) {
        if (field.mKind == FieldKind::Padding) {
            continue;
        }
        lua_getfield(state, table, Key(field.mName).CStr());
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            continue;
        }
        const uint32_t elementSize = field.mSize / field.mCount;
        if (field.mCount == 1) {
            ReadElement(state, vm, field, -1, data + field.mOffset, elementSize);
        } else {
            luaL_checktype(state, -1, LUA_TTABLE);
            const int count = std::min(lua_objlen(state, -1), static_cast<int>(field.mCount));
            for (int i = 1; i <= count; ++i) {
                lua_rawgeti(state, -1, i);
                ReadElement(state, vm, field, -1, data + field.mOffset + (static_cast<size_t>(i - 1) * elementSize),
                            elementSize);
                lua_pop(state, 1);
            }
        }
        lua_pop(state, 1);
    }
}

/// A value of `info` starting from `base` (or the type's default) with the table's fields applied.
/// Returns the byte count; `buffer` must hold info.mSize bytes.
void BuildRecord(lua_State* state, const ScriptVm& vm, const ComponentInfo& info, const void* base, int tableIndex,
                 std::span<std::byte> buffer) {
    if (info.mSize > buffer.size()) {
        luaL_error(state, "oyi: %s is too large for a script write", Key(info.mName).CStr());
    }
    std::memcpy(buffer.data(), base != nullptr ? base : info.mDefault, info.mSize);
    luaL_checktype(state, tableIndex, LUA_TTABLE);
    ReadRecord(state, vm, info, tableIndex, buffer.data());
}

constexpr size_t kRecordBufferSize = 1024;

// --- the library ----------------------------------------------------------------------------------

/// oyi.component(name) -> type index
int Component(lua_State* state) {
    const char* const name        = luaL_checkstring(state, 1);
    const ComponentTypeIndex type = VmOf(state).GetWorld().GetComponents().Find(core::MakeTypeId(name));
    if (!type.IsValid()) {
        luaL_error(state, "oyi: unknown component %s", name);
    }
    lua_pushinteger(state, type.mValue);
    return 1;
}

/// oyi.message(name) -> type index
int Message(lua_State* state) {
    const char* const name      = luaL_checkstring(state, 1);
    const MessageTypeIndex type = VmOf(state).GetWorld().GetMessages().Find(core::MakeTypeId(name));
    if (!type.IsValid()) {
        luaL_error(state, "oyi: unknown message %s", name);
    }
    lua_pushinteger(state, type.mValue);
    return 1;
}

/// oyi.managed(name) -> managed type handle   (the state block's fully qualified name)
int Managed(lua_State* state) {
    const char* const name                              = luaL_checkstring(state, 1);
    const std::span<const ManagedTypeInfo* const> types = VmOf(state).GetWorld().GetManagedTypes().GetAll();
    for (size_t i = 0; i < types.size(); ++i) {
        if (types[i]->GetName() == name) {
            lua_pushinteger(state, static_cast<int>(i));
            return 1;
        }
    }
    luaL_error(state, "oyi: unknown managed type %s", name);
}

/// oyi.state(entity, managed) -> table | nil   (the live state block of the owner's object)
int State(lua_State* state) {
    const EntityId entity                  = CheckEntity(state, 1);
    const ManagedTypeInfo& info            = CheckManaged(state, 2);
    const FirewallWindow& window           = WindowOf(state);
    const std::span<const std::byte> bytes = window.GetManagedState(window.FindManaged(entity, info.GetTypeId()));
    if (bytes.empty()) {
        lua_pushnil(state);
        return 1;
    }
    PushRecord(state, VmOf(state), *info.mState, bytes.data());
    return 1;
}

/// oyi.set_state(entity, managed, table)   deferred; fields absent from the table keep their value
int SetState(lua_State* state) {
    const EntityId entity                  = CheckEntity(state, 1);
    const ManagedTypeInfo& info            = CheckManaged(state, 2);
    FirewallWindow& window                 = WindowOf(state);
    const ManagedId id                     = window.FindManaged(entity, info.GetTypeId());
    const std::span<const std::byte> bytes = window.GetManagedState(id);
    if (bytes.empty()) {
        luaL_error(state, "oyi: no managed object of %s on the entity", Key(info.GetName()).CStr());
    }
    std::array<std::byte, kRecordBufferSize> buffer{};
    BuildRecord(state, VmOf(state), *info.mState, bytes.data(), 3, buffer);
    window.WriteManagedState(id, buffer.data(), info.GetStateSize());
    return 0;
}

/// oyi.is_alive(entity) -> bool
int IsAlive(lua_State* state) {
    lua_pushboolean(state, WindowOf(state).IsAlive(CheckEntity(state, 1)) ? 1 : 0);
    return 1;
}

/// oyi.has(entity, type) -> bool
int Has(lua_State* state) {
    lua_pushboolean(state, WindowOf(state).HasComponent(CheckEntity(state, 1), CheckComponent(state, 2)) ? 1 : 0);
    return 1;
}

/// oyi.get(entity, type) -> table | nil   (live value)
int Get(lua_State* state) {
    const EntityId entity         = CheckEntity(state, 1);
    const ComponentTypeIndex type = CheckComponent(state, 2);
    const ScriptVm& vm            = VmOf(state);
    const void* const value       = WindowOf(state).Get<void>(entity, type);
    if (value == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    PushRecord(state, vm, vm.GetWorld().GetComponents().GetInfo(type), static_cast<const std::byte*>(value));
    return 1;
}

/// oyi.prev(entity, type) -> table | nil   (the Prev snapshot)
int Prev(lua_State* state) {
    const EntityId entity         = CheckEntity(state, 1);
    const ComponentTypeIndex type = CheckComponent(state, 2);
    const ScriptVm& vm            = VmOf(state);
    const void* const value       = WindowOf(state).Prev<void>(entity, type);
    if (value == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    PushRecord(state, vm, vm.GetWorld().GetComponents().GetInfo(type), static_cast<const std::byte*>(value));
    return 1;
}

/// oyi.set(entity | pending, type, table)   deferred; fields absent from the table keep their value
int Set(lua_State* state) {
    const ComponentTypeIndex type = CheckComponent(state, 2);
    const ScriptVm& vm            = VmOf(state);
    FirewallWindow& window        = WindowOf(state);
    const ComponentInfo& info     = vm.GetWorld().GetComponents().GetInfo(type);
    std::array<std::byte, kRecordBufferSize> buffer{};
    if (IsTagged(state, 1, kPendingTag)) {
        BuildRecord(state, vm, info, nullptr, 3, buffer);
        window.GetCommands().SetComponent(CheckPending(state, 1), type, buffer.data(), info.mSize);
        return 0;
    }
    const EntityId entity = CheckEntity(state, 1);
    BuildRecord(state, vm, info, window.Get<void>(entity, type), 3, buffer);
    window.GetCommands().SetComponent(entity, type, buffer.data(), info.mSize);
    return 0;
}

/// oyi.send(type, target | nil, table)   deferred message; nil target broadcasts
int Send(lua_State* state) {
    const MessageTypeIndex type = CheckMessage(state, 1);
    const ScriptVm& vm          = VmOf(state);
    FirewallWindow& window      = WindowOf(state);
    const EntityId target       = lua_isnoneornil(state, 2) ? EntityId::Invalid() : CheckEntity(state, 2);
    const ComponentInfo& info   = vm.GetWorld().GetMessages().GetInfo(type);
    std::array<std::byte, kRecordBufferSize> buffer{};
    BuildRecord(state, vm, info, nullptr, 3, buffer);
    window.SendBytes(type, target, buffer.data(), info.mSize);
    return 0;
}

/// oyi.create({types...}) -> pending entity, usable with set in the same window
int Create(lua_State* state) {
    PushPending(state, WindowOf(state).CreateEntity(CheckMask(state, 1)));
    return 1;
}

/// oyi.destroy(entity)
int Destroy(lua_State* state) {
    WindowOf(state).DestroyEntity(CheckEntity(state, 1));
    return 0;
}

/// oyi.add(entity, type)
int Add(lua_State* state) {
    WindowOf(state).AddComponent(CheckEntity(state, 1), CheckComponent(state, 2));
    return 0;
}

/// oyi.remove(entity, type)
int Remove(lua_State* state) {
    WindowOf(state).RemoveComponent(CheckEntity(state, 1), CheckComponent(state, 2));
    return 0;
}

/// oyi.entities({all...}, {none...}?) -> array of entities
int Entities(lua_State* state) {
    const ComponentMask all  = CheckMask(state, 1);
    const ComponentMask none = lua_isnoneornil(state, 2) ? ComponentMask{} : CheckMask(state, 2);
    lua_newtable(state);
    int count = 0;
    WindowOf(state).ForEachEntity(all, none, [&](EntityId entity) {
        PushEntity(state, entity);
        lua_rawseti(state, -2, ++count);
    });
    return 1;
}

/// oyi.env() -> { frame, tick, fixed_step, time, delta }
int Env(lua_State* state) {
    const ecs::Environment& environment = VmOf(state).GetWorld().GetEnvironment();
    lua_createtable(state, 0, 5);
    lua_pushnumber(state, static_cast<double>(environment.mFrameIndex));
    lua_setfield(state, -2, "frame");
    lua_pushnumber(state, static_cast<double>(environment.mSimulationTick));
    lua_setfield(state, -2, "tick");
    lua_pushnumber(state, static_cast<double>(environment.mFixedStepSeconds));
    lua_setfield(state, -2, "fixed_step");
    lua_pushnumber(state, environment.mPresentationSeconds);
    lua_setfield(state, -2, "time");
    lua_pushnumber(state, static_cast<double>(environment.mDeltaSeconds));
    lua_setfield(state, -2, "delta");
    return 1;
}

/// oyi.entity_tostring(entity) -> "entity(index:generation)"
int EntityToString(lua_State* state) {
    const EntityId entity = CheckEntity(state, 1);
    lua_pushfstring(state, "entity(%d:%d)", static_cast<int>(entity.mIndex), static_cast<int>(entity.mGeneration));
    return 1;
}

constexpr std::array<luaL_Reg, 19> kLibrary{{{.name = "component", .func = &Component},
                                             {.name = "message", .func = &Message},
                                             {.name = "managed", .func = &Managed},
                                             {.name = "state", .func = &State},
                                             {.name = "set_state", .func = &SetState},
                                             {.name = "is_alive", .func = &IsAlive},
                                             {.name = "has", .func = &Has},
                                             {.name = "get", .func = &Get},
                                             {.name = "prev", .func = &Prev},
                                             {.name = "set", .func = &Set},
                                             {.name = "send", .func = &Send},
                                             {.name = "create", .func = &Create},
                                             {.name = "destroy", .func = &Destroy},
                                             {.name = "add", .func = &Add},
                                             {.name = "remove", .func = &Remove},
                                             {.name = "entities", .func = &Entities},
                                             {.name = "env", .func = &Env},
                                             {.name = "entity_tostring", .func = &EntityToString},
                                             {.name = nullptr, .func = nullptr}}};

} // namespace

void OpenOyiLibrary(lua_State* state, ScriptVm& vm) noexcept {
    lua_callbacks(state)->userdata = &vm;
    lua_setlightuserdataname(state, kEntityTag, "entity");
    lua_setlightuserdataname(state, kPendingTag, "pending entity");
    lua_setlightuserdataname(state, kHandleTag, "handle");
    luaL_register(state, "oyi", kLibrary.data());
    lua_pop(state, 1);
}

} // namespace oyi::script::detail

// NOLINTEND(cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
