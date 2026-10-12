#pragma once

#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Message.h>

namespace oyi::ecs {

/// A Facade (ADR-0006) is the interface a complex object shows to scripts and OO code: a value
/// bound to one entity inside a firewall window, with named operations. Whether an operation
/// reads a POD column, pokes a managed object or sends a message to a system is the business of
/// the facade; the caller only sees functions and an entity id. Derive and add the operations.
class Facade {
public:
    Facade(FirewallWindow& window, EntityId entity) noexcept : mWindow(&window), mEntity(entity) {}

    [[nodiscard]] EntityId GetEntity() const noexcept { return mEntity; }
    [[nodiscard]] FirewallWindow& GetWindow() const noexcept { return *mWindow; }
    [[nodiscard]] bool IsAlive() const noexcept { return mWindow->IsAlive(mEntity); }

protected:
    [[nodiscard]] bool Has(ComponentTypeIndex type) const noexcept { return mWindow->HasComponent(mEntity, type); }

    template <typename T>
    [[nodiscard]] const T* Get(ComponentTypeIndex type) const noexcept {
        return mWindow->Get<T>(mEntity, type);
    }

    template <typename T>
    [[nodiscard]] const T* Prev(ComponentTypeIndex type) const noexcept {
        return mWindow->Prev<T>(mEntity, type);
    }

    /// Takes effect when the window closes.
    template <typename T>
    void Set(ComponentTypeIndex type, const T& value) noexcept {
        mWindow->Set(mEntity, type, value);
    }

    /// Sends to this entity; delivered when the window closes.
    template <typename T>
    void Send(MessageTypeIndex type, const T& value) noexcept {
        mWindow->Send(type, mEntity, value);
    }

    [[nodiscard]] CommandBuffer& GetCommands() noexcept { return mWindow->GetCommands(); }

private:
    FirewallWindow* mWindow;
    EntityId mEntity;
};

} // namespace oyi::ecs
