#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/World.h>
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
    auto* const call = static_cast<ProtectedCall*>(lua_tolightuserdata(state, lua_upvalueindex(1)));
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
}

ScriptVm::Stats ScriptVm::GetStats() const noexcept {
    return Stats{.mUsedBytes   = mUsed,
                 .mPeakBytes   = mPeak,
                 .mInterrupts  = mInterrupts,
                 .mModuleCount = static_cast<uint32_t>(mModules.size())};
}

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
    mDeadline = std::chrono::steady_clock::now() + std::chrono::microseconds(mConfig.mCallBudgetMicroseconds);
    if (lua_pcall(thread, 0, 0, 0) != 0) {
        CaptureError(thread);
        lua_unref(mState, ref);
        return core::Fail(core::ErrorCode::InvalidState, "ScriptVm: the module failed to run, see GetLastError");
    }

    ScriptModuleId id = Find(name);
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
    mDeadline        = std::chrono::steady_clock::now() + std::chrono::microseconds(mConfig.mCallBudgetMicroseconds);
    const int status = lua_pcall(thread, 0, 0, 0);
    mWindow          = nullptr;
    if (status != 0) {
        CaptureError(thread);
        return core::Fail(core::ErrorCode::InvalidState, "ScriptVm: the call failed, see GetLastError");
    }
    return {};
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
