#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/World.h>
#include <oyi_script/ScriptManaged.h>
#include <oyi_script/ScriptVm.h>
#include <oyi_script/detail/Bindings.h>

#include <lua.h>
#include <luacode.h>
#include <lualib.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory_resource>
#include <string>
#include <string_view>

namespace oyi::script {

namespace {

/// Luau wants malloc-grade alignment for everything it allocates.
constexpr size_t kAlignment = 16;

using ProtectedFunction = void (*)(lua_State* state, void* data);

struct ProtectedCall {
    ProtectedFunction mFunction;
    void* mData;
};

int ProtectedTrampoline(lua_State* state) {
    const auto* const call = static_cast<const ProtectedCall*>(lua_tolightuserdata(state, lua_upvalueindex(1)));
    call->mFunction(state, call->mData);
    return 0;
}

/// Runs `function` under lua_pcall so that a Luau error raised on the way (out of memory above
/// all) becomes a status with the message on the stack instead of an exception.
[[nodiscard]] bool RunProtected(lua_State* state, ProtectedFunction function, void* data) noexcept {
    ProtectedCall call{.mFunction = function, .mData = data};
    lua_pushlightuserdata(state, &call);
    lua_pushcclosure(state, &ProtectedTrampoline, "oyi.protected", 1);
    return lua_pcall(state, 0, 0, 0) == 0;
}

} // namespace

struct ScriptVm::LoadRequest {
    const char* mChunkName = nullptr;
    const char* mBytecode  = nullptr;
    size_t mSize           = 0;
    lua_State* mThread     = nullptr;
    int mRef               = 0;
    int mStatus            = 0;
};

void* ScriptVm::Alloc(void* ud, void* ptr, size_t osize, size_t nsize) noexcept {
    auto* const vm = static_cast<ScriptVm*>(ud);
    // When `ptr` is null `osize` is a type hint, not a size.
    const size_t oldSize = ptr != nullptr ? osize : 0;
    if (nsize == 0) {
        if (ptr != nullptr) {
            vm->mAllocator.Free(ptr, oldSize, kAlignment);
            vm->mUsed -= oldSize;
        }
        return nullptr;
    }
    if (vm->mUsed - oldSize + nsize > vm->mConfig.mMemoryLimit) {
        return nullptr; // Luau turns this into a memory error the script can observe
    }
    void* const fresh = vm->mAllocator.Allocate(nsize, kAlignment);
    if (fresh == nullptr) {
        return nullptr;
    }
    if (ptr != nullptr) {
        std::memcpy(fresh, ptr, std::min(oldSize, nsize));
        vm->mAllocator.Free(ptr, oldSize, kAlignment);
        vm->mUsed -= oldSize;
    }
    vm->mUsed += nsize;
    vm->mPeak = std::max(vm->mPeak, vm->mUsed);
    return fresh;
}

void ScriptVm::Interrupt(lua_State* state, int gc) {
    if (gc >= 0) {
        return; // GC steps are not the script's time
    }
    auto* const vm = static_cast<ScriptVm*>(lua_callbacks(state)->userdata);
    if (std::chrono::steady_clock::now() < vm->mDeadline) {
        return;
    }
    ++vm->mInterrupts;
    luaL_error(state, "oyi: the script call exceeded its time budget"); // NOLINT(cppcoreguidelines-pro-type-vararg)
}

ScriptVm::ScriptVm(core::Allocator& allocator, ecs::World& world, Config config) noexcept
    : mAllocator(allocator)
    , mWorld(world)
    , mConfig(config)
    , mResource(allocator)
    , mModules(&mResource)
    , mStructs(&mResource)
    , mManagedTypes(&mResource)
    , mLastError(&mResource) {
    if (mConfig.mMemoryLimit < kMinimumMemoryLimit) {
        return;
    }
    mState = lua_newstate(&ScriptVm::Alloc, this);
    if (mState == nullptr) {
        return;
    }
    lua_callbacks(mState)->userdata  = this;
    lua_callbacks(mState)->interrupt = &ScriptVm::Interrupt;
    mDeadline                        = std::chrono::steady_clock::time_point::max();
    if (!RunProtected(mState, &ScriptVm::OpenState, this)) {
        CaptureError(mState);
        lua_close(mState);
        mState = nullptr;
    }
}

void ScriptVm::OpenState(lua_State* state, void* data) {
    auto* const vm = static_cast<ScriptVm*>(data);
    luaL_openlibs(state);
    // ADR-0012: no clock, no file system, no debugger from scripts.
    lua_pushnil(state);
    lua_setglobal(state, "os");
    lua_pushnil(state);
    lua_setglobal(state, "debug");
    detail::OpenOyiLibrary(state, *vm);
    luaL_sandbox(state);
}

void ScriptVm::LoadModule(lua_State* state, void* data) {
    auto* const request = static_cast<LoadRequest*>(data);
    request->mThread    = lua_newthread(state);
    luaL_sandboxthread(request->mThread); // a global table of its own, writable, proxying the sandboxed one
    request->mRef = lua_ref(state, -1);
    lua_pop(state, 1);
    request->mStatus = luau_load(request->mThread, request->mChunkName, request->mBytecode, request->mSize, 0);
}

ScriptVm::~ScriptVm() {
    if (mState != nullptr) {
        lua_close(mState);
    }
    for (ScriptManagedType* const type : mManagedTypes) {
        mAllocator.Delete(type);
    }
}

ScriptVm::Stats ScriptVm::GetStats() const noexcept {
    return Stats{.mUsedBytes   = mUsed,
                 .mPeakBytes   = mPeak,
                 .mInterrupts  = mInterrupts,
                 .mModuleCount = static_cast<uint32_t>(mModules.size())};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void ScriptVm::CaptureError(lua_State* state) noexcept {
    const char* const message = lua_tostring(state, -1);
    mLastError.assign(message != nullptr ? message : "oyi: unknown script error");
    lua_pop(state, 1);
}

void ScriptVm::ReleaseModuleThread(Module& module) noexcept {
    if (module.mThread != nullptr) {
        lua_unref(mState, module.mRef);
        module.mThread = nullptr;
        module.mRef    = 0;
    }
}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<ScriptModuleId> ScriptVm::Load(std::string_view name, std::string_view source) noexcept {
    if (mState == nullptr) {
        return core::Fail(core::ErrorCode::InvalidState, "ScriptVm: no state");
    }
    if (name.empty()) {
        return core::Fail(core::ErrorCode::InvalidArgument, "ScriptVm: a module needs a name");
    }
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel        = 1;
    size_t bytecodeSize       = 0;
    char* const bytecode      = luau_compile(source.data(), source.size(), &options, &bytecodeSize);
    if (bytecode == nullptr) {
        return core::Fail(core::ErrorCode::OutOfMemory, "ScriptVm: cannot compile");
    }

    std::pmr::string chunkName(&mResource);
    chunkName.reserve(name.size() + 1);
    chunkName.push_back('=');
    chunkName.append(name);
    LoadRequest request{.mChunkName = chunkName.c_str(), .mBytecode = bytecode, .mSize = bytecodeSize};
    const bool created = RunProtected(mState, &ScriptVm::LoadModule, &request);
    std::free(bytecode); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    if (!created) {
        CaptureError(mState);
        if (request.mRef != 0) {
            lua_unref(mState, request.mRef);
        }
        return core::Fail(core::ErrorCode::OutOfMemory, "ScriptVm: cannot create the module, see GetLastError");
    }
    lua_State* const thread = request.mThread;
    const int ref           = request.mRef;
    if (request.mStatus != 0) {
        CaptureError(thread);
        lua_unref(mState, ref);
        return core::Fail(core::ErrorCode::CorruptData, "ScriptVm: compile error, see GetLastError");
    }
    // The top level runs under the same budget as a call, without a window: bindings that need
    // one fail, which keeps module initialisation to definitions.
    // The module keeps its id across reloads, so what the top level declares (managed types)
    // follows the new definition.
    ScriptModuleId id = Find(name);
    mCurrentModule    = id.IsValid() ? id : ScriptModuleId{.mValue = static_cast<uint32_t>(mModules.size())};
    mDeadline         = std::chrono::steady_clock::now() + std::chrono::microseconds(mConfig.mCallBudgetMicroseconds);
    const int ran     = lua_pcall(thread, 0, 0, 0);
    mCurrentModule    = ScriptModuleId::Invalid();
    if (ran != 0) {
        CaptureError(thread);
        lua_unref(mState, ref);
        return core::Fail(core::ErrorCode::InvalidState, "ScriptVm: the module failed to run, see GetLastError");
    }

    if (id.IsValid()) {
        Module& module = mModules[id.mValue];
        ReleaseModuleThread(module);
        module.mThread = thread;
        module.mRef    = ref;
        return id;
    }
    id = ScriptModuleId{.mValue = static_cast<uint32_t>(mModules.size())};
    mModules.push_back(Module{.mName = std::pmr::string(name, &mResource), .mThread = thread, .mRef = ref});
    return id;
}

ScriptModuleId ScriptVm::Find(std::string_view name) const noexcept {
    for (uint32_t i = 0; i < mModules.size(); ++i) {
        if (mModules[i].mName == name) {
            return ScriptModuleId{.mValue = i};
        }
    }
    return ScriptModuleId::Invalid();
}

std::string_view ScriptVm::GetModuleName(ScriptModuleId module) const noexcept {
    return module.mValue < mModules.size() ? std::string_view(mModules[module.mValue].mName) : std::string_view{};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result ScriptVm::Call(ecs::FirewallWindow& window, ScriptModuleId module, std::string_view function) noexcept {
    if (mState == nullptr) {
        return core::Fail(core::ErrorCode::InvalidState, "ScriptVm: no state");
    }
    if (module.mValue >= mModules.size() || mModules[module.mValue].mThread == nullptr) {
        return core::Fail(core::ErrorCode::NotFound, "ScriptVm: unknown module");
    }
    lua_State* const thread = mModules[module.mValue].mThread;
    const std::pmr::string functionName(function, &mResource);
    lua_getglobal(thread, functionName.c_str());
    if (!lua_isfunction(thread, -1)) {
        lua_pop(thread, 1);
        return core::Fail(core::ErrorCode::NotFound, "ScriptVm: the module has no such function");
    }
    mWindow          = &window;
    mCurrentModule   = module;
    mDeadline        = std::chrono::steady_clock::now() + std::chrono::microseconds(mConfig.mCallBudgetMicroseconds);
    const int status = lua_pcall(thread, 0, 0, 0);
    mWindow          = nullptr;
    mCurrentModule   = ScriptModuleId::Invalid();
    if (status != 0) {
        CaptureError(thread);
        return core::Fail(core::ErrorCode::InvalidState, "ScriptVm: the call failed, see GetLastError");
    }
    return {};
}

// --- managed types declared from Luau ---------------------------------------------------------

ScriptManagedType::ScriptManagedType(ScriptVm& vm, std::pmr::memory_resource* resource) noexcept
    : mVm(vm), mReadInfos(resource), mWriteInfos(resource), mReceiveInfos(resource), mSendInfos(resource) {}

core::Expected<const ScriptManagedType*>
// NOLINTNEXTLINE(bugprone-exception-escape)
ScriptVm::DeclareManagedType(const core::ComponentInfo& state, ScriptModuleId module, const ecs::ComponentMask& reads,
                             const ecs::ComponentMask& writes, const ecs::MessageMask& receives,
                             const ecs::MessageMask& sends) noexcept {
    if (module.mValue > mModules.size()) { // == size: the module whose top level is running now
        return core::Fail(core::ErrorCode::InvalidArgument, "ScriptVm: unknown module for a managed type");
    }
    ScriptManagedType* type = nullptr;
    for (ScriptManagedType* const candidate : mManagedTypes) {
        if (candidate->mInfo.GetTypeId() == state.mTypeId) {
            type = candidate;
        }
    }
    if (type == nullptr) {
        if (mWorld.GetManagedTypes().Find(state.mTypeId) != nullptr) {
            return core::Fail(core::ErrorCode::AlreadyExists, "ScriptVm: a C++ managed type owns that state block");
        }
        type = mAllocator.New<ScriptManagedType>(*this, &mResource);
        if (type == nullptr) {
            return core::Fail(core::ErrorCode::OutOfMemory, "ScriptVm: cannot allocate the managed type");
        }
        mManagedTypes.push_back(type);
    }
    type->mModule         = module;
    type->mReads          = reads;
    type->mWrites         = writes;
    type->mReceives       = receives;
    type->mSends          = sends;
    const auto components = [this](const ecs::ComponentMask& mask,
                                   std::pmr::vector<const core::ComponentInfo*>& infos) {
        infos.clear();
        mask.ForEach([&](ecs::ComponentTypeIndex index) { infos.push_back(&mWorld.GetComponents().GetInfo(index)); });
    };
    const auto messages = [this](const ecs::MessageMask& mask, std::pmr::vector<const core::ComponentInfo*>& infos) {
        infos.clear();
        mask.ForEach([&](ecs::MessageTypeIndex index) { infos.push_back(&mWorld.GetMessages().GetInfo(index)); });
    };
    components(reads, type->mReadInfos);
    components(writes, type->mWriteInfos);
    messages(receives, type->mReceiveInfos);
    messages(sends, type->mSendInfos);
    type->mInfo = ecs::ManagedTypeInfo{.mState    = &state,
                                       .mReads    = type->mReadInfos,
                                       .mWrites   = type->mWriteInfos,
                                       .mReceives = type->mReceiveInfos,
                                       .mSends    = type->mSendInfos,
                                       .mCreate   = &ScriptManagedObject::Create,
                                       .mUserData = type};
    if (const core::Result registered = mWorld.GetManagedTypes().Register(type->mInfo); !registered.has_value()) {
        return core::Fail(registered.error().mCode, registered.error().mMessage);
    }
    return type;
}

const ScriptManagedType* ScriptVm::FindManagedType(core::TypeId state) const noexcept {
    for (const ScriptManagedType* const type : mManagedTypes) {
        if (type->mInfo.GetTypeId() == state) {
            return type;
        }
    }
    return nullptr;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result ScriptVm::UpdateManaged(ScriptManagedObject& object, ecs::ManagedContext& context) noexcept {
    const ScriptManagedType& type = object.GetScriptType();
    const ScriptModuleId module   = type.GetModule();
    if (mState == nullptr || module.mValue >= mModules.size() || mModules[module.mValue].mThread == nullptr) {
        return core::Fail(core::ErrorCode::NotFound, "ScriptVm: the managed type's module is gone");
    }
    lua_State* const thread = mModules[module.mValue].mThread;
    lua_getglobal(thread, "update");
    if (!lua_isfunction(thread, -1)) {
        lua_pop(thread, 1);
        return core::Fail(core::ErrorCode::NotFound, "ScriptVm: the module has no update function");
    }
    const core::ComponentInfo& state = *type.GetInfo().mState;
    auto* const bytes                = static_cast<std::byte*>(object.GetStateBytesForWrite());
    detail::PushRecord(thread, *this, state, bytes); // self
    lua_pushvalue(thread, -1);                       // kept below the call for the read-back
    lua_insert(thread, -3);                          // [self, update, self]
    detail::PushManagedContext(thread, object);      // [self, update, self, ctx]
    object.SetContext(&context);
    mCurrentModule   = module;
    mDeadline        = std::chrono::steady_clock::now() + std::chrono::microseconds(mConfig.mCallBudgetMicroseconds);
    const int status = lua_pcall(thread, 2, 0, 0);
    mCurrentModule   = ScriptModuleId::Invalid();
    object.SetContext(nullptr);
    if (status != 0) {
        CaptureError(thread);
        lua_pop(thread, 1);                             // self
        ++const_cast<ScriptManagedType&>(type).mErrors; // NOLINT(cppcoreguidelines-pro-type-const-cast)
        return core::Fail(core::ErrorCode::InvalidState, "ScriptVm: update failed, see GetLastError");
    }
    detail::ReadRecord(thread, *this, state, -1, bytes);
    lua_pop(thread, 1);
    return {};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result ScriptVm::CallManagedHook(ScriptManagedObject& object, ecs::FirewallWindow& window,
                                       std::string_view function) noexcept {
    const ScriptManagedType& type = object.GetScriptType();
    const ScriptModuleId module   = type.GetModule();
    if (mState == nullptr || module.mValue >= mModules.size() || mModules[module.mValue].mThread == nullptr) {
        return core::Fail(core::ErrorCode::NotFound, "ScriptVm: the managed type's module is gone");
    }
    lua_State* const thread = mModules[module.mValue].mThread;
    const std::pmr::string functionName(function, &mResource);
    lua_getglobal(thread, functionName.c_str());
    if (!lua_isfunction(thread, -1)) {
        lua_pop(thread, 1);
        return {}; // optional
    }
    const core::ComponentInfo& state = *type.GetInfo().mState;
    auto* const bytes                = static_cast<std::byte*>(object.GetStateBytesForWrite());
    detail::PushRecord(thread, *this, state, bytes);
    lua_pushvalue(thread, -1);
    lua_insert(thread, -3); // [self, fn, self]
    detail::PushEntity(thread, object.GetOwner());
    mWindow          = &window;
    mCurrentModule   = module;
    mDeadline        = std::chrono::steady_clock::now() + std::chrono::microseconds(mConfig.mCallBudgetMicroseconds);
    const int status = lua_pcall(thread, 2, 0, 0);
    mWindow          = nullptr;
    mCurrentModule   = ScriptModuleId::Invalid();
    if (status != 0) {
        CaptureError(thread);
        lua_pop(thread, 1);
        ++const_cast<ScriptManagedType&>(type).mErrors; // NOLINT(cppcoreguidelines-pro-type-const-cast)
        return core::Fail(core::ErrorCode::InvalidState, "ScriptVm: the hook failed, see GetLastError");
    }
    detail::ReadRecord(thread, *this, state, -1, bytes);
    lua_pop(thread, 1);
    return {};
}

// --- the object ---------------------------------------------------------------------------------

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
ScriptManagedObject::ScriptManagedObject(core::Allocator& allocator, const ScriptManagedType& type) noexcept
    : mType(type), mResource(allocator), mState(&mResource) {
    const core::ComponentInfo& state = *type.GetInfo().mState;
    mState.resize(state.mSize);
    std::memcpy(mState.data(), state.mDefault, state.mSize);
}

void ScriptManagedObject::Update(ecs::ManagedContext& context) {
    // A failing update is reported through the VM's last error and the type's error count.
    const core::Result ran = mType.GetVm().UpdateManaged(*this, context);
    (void)ran;
}

void ScriptManagedObject::OnSpawned(ecs::FirewallWindow& window) {
    const core::Result ran = mType.GetVm().CallManagedHook(*this, window, "on_spawned");
    (void)ran;
}

void ScriptManagedObject::OnDespawning(ecs::FirewallWindow& window) {
    const core::Result ran = mType.GetVm().CallManagedHook(*this, window, "on_despawning");
    (void)ran;
}

ecs::ManagedObject* ScriptManagedObject::Create(core::Allocator& allocator, const ecs::ManagedTypeInfo& info) noexcept {
    const auto* const type = static_cast<const ScriptManagedType*>(info.mUserData);
    return type == nullptr ? nullptr : allocator.New<ScriptManagedObject>(allocator, *type);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void ScriptVm::RegisterStruct(const core::ComponentInfo& info) noexcept {
    if (FindInfo(info.mTypeId) == nullptr) {
        mStructs.push_back(&info);
    }
}

const core::ComponentInfo* ScriptVm::FindInfo(core::TypeId typeId) const noexcept {
    for (const core::ComponentInfo* const info : mStructs) {
        if (info->mTypeId == typeId) {
            return info;
        }
    }
    const ecs::ComponentTypeIndex component = mWorld.GetComponents().Find(typeId);
    if (component.IsValid()) {
        return &mWorld.GetComponents().GetInfo(component);
    }
    const ecs::MessageTypeIndex message = mWorld.GetMessages().Find(typeId);
    if (message.IsValid()) {
        return &mWorld.GetMessages().GetInfo(message);
    }
    return nullptr;
}

} // namespace oyi::script
