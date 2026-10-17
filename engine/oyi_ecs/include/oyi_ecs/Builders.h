#pragma once

#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>

#include <string_view>

namespace oyi::ecs {

/// Registers every component and message of a generated module (its `kModule`, ADR-0024) with
/// the world. Idempotent: a module already registered is a no-op.
[[nodiscard]] core::Result RegisterModule(World& world, const core::ModuleSchema& module) noexcept;

/// The index a registered type got; invalid when the type is not registered.
template <typename T>
[[nodiscard]] ComponentTypeIndex ComponentOf(const World& world) noexcept {
    return world.GetComponents().Find<T>();
}

template <typename T>
[[nodiscard]] MessageTypeIndex MessageOf(const World& world) noexcept {
    return world.GetMessages().Find<T>();
}

/// Writes a SystemDesc by type instead of by index (RFC-0001 step 5):
///
///   const auto desc = System("door.animate", world).Phase(Phase::FixedSimulation)
///                         .Writes<DoorState>().Receives<OpenRequest>().Fn(&Animate, &types);
///
/// Every type must already be registered with the world; the first one that is not makes Fn /
/// Build fail with NotFound and GetMissingType names it.
class SystemBuilder {
public:
    SystemBuilder(std::string_view name, const World& world) noexcept;

    SystemBuilder& Phase(::oyi::ecs::Phase phase) noexcept {
        mDesc.mPhase = phase;
        return *this;
    }

    template <typename... Ts>
    SystemBuilder& Reads() noexcept {
        (Component<Ts>(mDesc.mRead), ...);
        return *this;
    }

    template <typename... Ts>
    SystemBuilder& Writes() noexcept {
        (Component<Ts>(mDesc.mWrite), ...);
        return *this;
    }

    template <typename... Ts>
    SystemBuilder& Excludes() noexcept {
        (Component<Ts>(mDesc.mExclude), ...);
        return *this;
    }

    /// Types from the read / write sets an archetype may lack (SystemDesc::mOptional).
    template <typename... Ts>
    SystemBuilder& Optional() noexcept {
        (Component<Ts>(mDesc.mOptional), ...);
        return *this;
    }

    template <typename... Ms>
    SystemBuilder& Sends() noexcept {
        (Message<Ms>(mDesc.mSends), ...);
        return *this;
    }

    template <typename... Ms>
    SystemBuilder& Receives() noexcept {
        (Message<Ms>(mDesc.mReceives), ...);
        return *this;
    }

    SystemBuilder& ReadsResource(ResourceTypeIndex resource) noexcept {
        mDesc.mReadResources.Set(resource);
        return *this;
    }

    SystemBuilder& WritesResource(ResourceTypeIndex resource) noexcept {
        mDesc.mWriteResources.Set(resource);
        return *this;
    }

    /// The Environment resource (frame, tick, fixed step): SystemContext::GetEnvironment needs it declared.
    SystemBuilder& ReadsEnvironment() noexcept { return ReadsResource(kEnvironmentResource); }

    SystemBuilder& MainThread() noexcept {
        mDesc.mMainThread = true;
        return *this;
    }

    /// The description with its function; fails with InvalidArgument without one.
    [[nodiscard]] core::Expected<SystemDesc> Fn(SystemFunction function, void* data = nullptr) noexcept;
    /// The description without a function, for code that binds one itself (ManagedSystem::Describe).
    [[nodiscard]] core::Expected<SystemDesc> Build() const noexcept;

    [[nodiscard]] std::string_view GetMissingType() const noexcept { return mMissing; }

private:
    template <typename T>
    void Component(ComponentMask& mask) noexcept {
        const ComponentTypeIndex type = mWorld.GetComponents().Find<T>();
        if (type.IsValid()) {
            mask.Set(type);
        } else {
            Missing(core::ComponentTraits<T>::kInfo.mName);
        }
    }

    template <typename M>
    void Message(MessageMask& mask) noexcept {
        const MessageTypeIndex type = mWorld.GetMessages().Find<M>();
        if (type.IsValid()) {
            mask.Set(type);
        } else {
            Missing(core::ComponentTraits<M>::kInfo.mName);
        }
    }

    void Missing(std::string_view name) noexcept {
        if (mMissing.empty()) {
            mMissing = name;
        }
    }

    const World& mWorld;
    SystemDesc mDesc;
    std::string_view mMissing;
};

[[nodiscard]] inline SystemBuilder System(std::string_view name, const World& world) noexcept {
    return {name, world};
}

} // namespace oyi::ecs
