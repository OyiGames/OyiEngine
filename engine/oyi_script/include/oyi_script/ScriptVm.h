#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/World.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace oyi::ecs {
class ManagedContext;
} // namespace oyi::ecs

namespace oyi::script {

/// A script module loaded into a ScriptVm: one chunk with a sandboxed environment of its own.
struct ScriptModuleId {
    uint32_t mValue = 0xFFFF'FFFFu;

    [[nodiscard]] static constexpr ScriptModuleId Invalid() noexcept { return ScriptModuleId{}; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return mValue != 0xFFFF'FFFFu; }
    [[nodiscard]] constexpr bool operator==(const ScriptModuleId&) const noexcept = default;
};

/// One Luau virtual machine per World (ADR-0012). Memory comes from the engine allocator under a
/// limit, long calls are interrupted, the standard library is sandboxed, and the `oyi` library
/// only works while a firewall window is open: scripts see functions and entity ids, never
/// component memory. Everything a script writes goes through the window, so it lands at the
/// window's sync point like any other deferred write.
class ScriptManagedObject;
class ScriptManagedType;

class ScriptVm {
public:
    /// A limit below this cannot hold the standard library; the VM refuses to start.
    static constexpr size_t kMinimumMemoryLimit = size_t{512} * 1024;

    struct Config {
        /// Bytes the VM may hold at once; allocation fails (a Luau memory error) beyond it.
        /// ADR-0012: the device profile decides, 64 MiB is the mobile starting point.
        size_t mMemoryLimit = size_t{64} * 1024 * 1024;
        /// A Call that runs longer than this is interrupted with a runtime error.
        uint32_t mCallBudgetMicroseconds = 2000;
    };

    struct Stats {
        size_t mUsedBytes     = 0;
        size_t mPeakBytes     = 0;
        uint32_t mInterrupts  = 0; ///< calls cut short by the time budget
        uint32_t mModuleCount = 0;
    };

    /// Creates the state; IsValid is false when the allocator could not serve it or the limit is
    /// below kMinimumMemoryLimit.
    ScriptVm(core::Allocator& allocator, ecs::World& world, Config config) noexcept;
    ScriptVm(const ScriptVm&)            = delete;
    ScriptVm(ScriptVm&&)                 = delete;
    ScriptVm& operator=(const ScriptVm&) = delete;
    ScriptVm& operator=(ScriptVm&&)      = delete;
    ~ScriptVm();

    [[nodiscard]] bool IsValid() const noexcept { return mState != nullptr; }
    [[nodiscard]] const Config& GetConfig() const noexcept { return mConfig; }
    [[nodiscard]] ecs::World& GetWorld() const noexcept { return mWorld; }
    [[nodiscard]] Stats GetStats() const noexcept;

    /// Compiles `source` and runs its top level in a fresh sandboxed environment. Loading a name
    /// again replaces the module (hot reload). Fails with CorruptData on a compile error and
    /// InvalidState on a runtime error; GetLastError holds the message either way.
    [[nodiscard]] core::Expected<ScriptModuleId> Load(std::string_view name, std::string_view source) noexcept;
    [[nodiscard]] ScriptModuleId Find(std::string_view name) const noexcept;
    [[nodiscard]] std::string_view GetModuleName(ScriptModuleId module) const noexcept;

    /// Calls the module's global function `function` with the `oyi` library bound to `window`
    /// (ADR-0006: scripts only run inside the firewall window). Fails with NotFound when the
    /// function does not exist and InvalidState on a runtime error, including the time budget.
    [[nodiscard]] core::Result Call(ecs::FirewallWindow& window, ScriptModuleId module,
                                    std::string_view function) noexcept;

    [[nodiscard]] std::string_view GetLastError() const noexcept { return mLastError; }

    /// Makes a reflected OYI_STRUCT known so that fields of that type marshal as tables. Component
    /// and message types are found through the World; only nested value types need this.
    void RegisterStruct(const core::ComponentInfo& info) noexcept;

    template <typename T>
    void RegisterStruct() noexcept {
        RegisterStruct(core::ComponentTraits<T>::kInfo);
    }

    /// The reflection table for a TypeId: a registered struct, a component or a message type.
    [[nodiscard]] const core::ComponentInfo* FindInfo(core::TypeId typeId) const noexcept;

    /// The window of the Call in progress; nullptr otherwise.
    [[nodiscard]] ecs::FirewallWindow* GetWindow() const noexcept { return mWindow; }
    /// The module whose top level or function is running; invalid otherwise.
    [[nodiscard]] ScriptModuleId GetCurrentModule() const noexcept { return mCurrentModule; }

    // --- managed types declared from Luau (RFC-0001 step 7, ScriptManaged.h) ---------------------

    /// Declares (or, for a module reload, redefines) the managed type whose state block is
    /// `state`, bound to `module`; the access sets are component / message indices of the
    /// world. Registers it with the world's managed type registry. Fails with AlreadyExists when
    /// a C++ type owns the state block and with InvalidArgument for an unknown module.
    [[nodiscard]] core::Expected<const ScriptManagedType*>
    DeclareManagedType(const core::ComponentInfo& state, ScriptModuleId module, const ecs::ComponentMask& reads,
                       const ecs::ComponentMask& writes, const ecs::MessageMask& receives,
                       const ecs::MessageMask& sends) noexcept;
    [[nodiscard]] const ScriptManagedType* FindManagedType(core::TypeId state) const noexcept;
    /// Runs the module's `update(self, ctx)` for the object and reads `self` back into the
    /// state block. Fails with NotFound without an `update` and InvalidState on a Luau error
    /// (GetLastError); the object's type counts the errors.
    [[nodiscard]] core::Result UpdateManaged(ScriptManagedObject& object, ecs::ManagedContext& context) noexcept;
    /// Runs the module's optional `function(self, owner)` with the `oyi` library bound to
    /// `window` and reads `self` back; a missing function is a no-op.
    [[nodiscard]] core::Result CallManagedHook(ScriptManagedObject& object, ecs::FirewallWindow& window,
                                               std::string_view function) noexcept;
    [[nodiscard]] lua_State* GetState() const noexcept { return mState; }

private:
    struct Module {
        std::pmr::string mName;
        lua_State* mThread = nullptr;
        int mRef           = 0;
    };

    struct LoadRequest;

    static void* Alloc(void* ud, void* ptr, size_t osize, size_t nsize) noexcept;
    static void Interrupt(lua_State* state, int gc);
    static void OpenState(lua_State* state, void* data);
    static void LoadModule(lua_State* state, void* data);
    void CaptureError(lua_State* state) noexcept;
    void ReleaseModuleThread(Module& module) noexcept;

    core::Allocator& mAllocator;
    ecs::World& mWorld;
    Config mConfig;
    core::MemoryResource mResource;
    lua_State* mState = nullptr;
    std::pmr::vector<Module> mModules;
    std::pmr::vector<const core::ComponentInfo*> mStructs;
    std::pmr::vector<ScriptManagedType*> mManagedTypes;
    std::pmr::string mLastError;
    ecs::FirewallWindow* mWindow = nullptr;
    ScriptModuleId mCurrentModule;
    std::chrono::steady_clock::time_point mDeadline;
    size_t mUsed         = 0;
    size_t mPeak         = 0;
    uint32_t mInterrupts = 0;
};

} // namespace oyi::script
