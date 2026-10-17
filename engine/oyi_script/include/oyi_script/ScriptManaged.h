#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_core/Schema.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/Message.h>
#include <oyi_script/ScriptVm.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

namespace oyi::script {

/// A managed type declared from Luau (RFC-0001 step 7, `oyi.managed_type`): the Luau twin of
/// OYI_MANAGED. The state block is a reflected OYI_STRUCT the VM knows; the access sets are what
/// the module's `update(self, ctx)` may touch through `ctx`; the module is looked up by id at
/// every call, so a reloaded module takes over its objects. Owned by the ScriptVm.
class ScriptManagedType {
public:
    ScriptManagedType(ScriptVm& vm, std::pmr::memory_resource* resource) noexcept;
    ScriptManagedType(const ScriptManagedType&)            = delete;
    ScriptManagedType(ScriptManagedType&&)                 = delete;
    ScriptManagedType& operator=(const ScriptManagedType&) = delete;
    ScriptManagedType& operator=(ScriptManagedType&&)      = delete;
    ~ScriptManagedType()                                   = default;

    [[nodiscard]] const ecs::ManagedTypeInfo& GetInfo() const noexcept { return mInfo; }
    [[nodiscard]] ScriptVm& GetVm() const noexcept { return mVm; }
    [[nodiscard]] ScriptModuleId GetModule() const noexcept { return mModule; }
    [[nodiscard]] const ecs::ComponentMask& GetReads() const noexcept { return mReads; }
    [[nodiscard]] const ecs::ComponentMask& GetWrites() const noexcept { return mWrites; }
    [[nodiscard]] const ecs::MessageMask& GetReceives() const noexcept { return mReceives; }
    [[nodiscard]] const ecs::MessageMask& GetSends() const noexcept { return mSends; }
    /// Calls of update / hooks that raised a Luau error (the message is in ScriptVm::GetLastError).
    [[nodiscard]] uint32_t GetErrorCount() const noexcept { return mErrors; }

private:
    friend class ScriptVm;

    ecs::ManagedTypeInfo mInfo;
    ScriptVm& mVm;
    ScriptModuleId mModule;
    ecs::ComponentMask mReads;
    ecs::ComponentMask mWrites;
    ecs::MessageMask mReceives;
    ecs::MessageMask mSends;
    std::pmr::vector<const core::ComponentInfo*> mReadInfos;
    std::pmr::vector<const core::ComponentInfo*> mWriteInfos;
    std::pmr::vector<const core::ComponentInfo*> mReceiveInfos;
    std::pmr::vector<const core::ComponentInfo*> mSendInfos;
    uint32_t mErrors = 0;
};

/// A managed object whose behaviour is a Luau module: Update runs `update(self, ctx)` with
/// `self` the state block as a table (read back afterwards) and `ctx` the restricted context
/// (get / prev / set / received / send / owner / env, only for the declared sets); OnSpawned and
/// OnDespawning run the optional `on_spawned(self, owner)` / `on_despawning(self, owner)` with
/// the `oyi` library bound to the window.
class ScriptManagedObject final : public ecs::ManagedObject {
public:
    ScriptManagedObject(core::Allocator& allocator, const ScriptManagedType& type) noexcept;

    void Update(ecs::ManagedContext& context) override;
    void OnSpawned(ecs::FirewallWindow& window) override;
    void OnDespawning(ecs::FirewallWindow& window) override;
    [[nodiscard]] const ecs::ManagedTypeInfo* GetTypeInfo() const noexcept override { return &mType.GetInfo(); }
    [[nodiscard]] const void* GetStateBytes() const noexcept override { return mState.data(); }
    [[nodiscard]] void* GetStateBytesForWrite() noexcept override { return mState.data(); }

    [[nodiscard]] const ScriptManagedType& GetScriptType() const noexcept { return mType; }
    /// The context of the Update in progress; nullptr outside one (the ctx bindings check it).
    [[nodiscard]] ecs::ManagedContext* GetContext() const noexcept { return mContext; }
    void SetContext(ecs::ManagedContext* context) noexcept { mContext = context; }

    /// ManagedTypeInfo::mCreate for script types: `info.mUserData` is the ScriptManagedType.
    [[nodiscard]] static ecs::ManagedObject* Create(core::Allocator& allocator,
                                                    const ecs::ManagedTypeInfo& info) noexcept;

private:
    const ScriptManagedType& mType;
    core::MemoryResource mResource;
    std::pmr::vector<std::byte> mState;
    ecs::ManagedContext* mContext = nullptr;
};

} // namespace oyi::script
