#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_ecs/Builders.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>

#include <string_view>

namespace oyi::ecs {

core::Result RegisterModule(World& world, const core::ModuleSchema& module) noexcept {
    for (const core::ComponentInfo* const info : module.mComponents) {
        if (const core::Expected<ComponentTypeIndex> type = world.GetComponents().Register(*info); !type.has_value()) {
            return core::Fail(type.error().mCode, type.error().mMessage);
        }
    }
    for (const core::ComponentInfo* const info : module.mMessages) {
        if (const core::Expected<MessageTypeIndex> type = world.GetMessages().Register(*info); !type.has_value()) {
            return core::Fail(type.error().mCode, type.error().mMessage);
        }
    }
    return {};
}

SystemBuilder::SystemBuilder(std::string_view name, const World& world) noexcept
    : mWorld(world)
    , mDesc(SystemDesc{.mName           = name,
                       .mPhase          = ::oyi::ecs::Phase::Variable,
                       .mRead           = ComponentMask{},
                       .mWrite          = ComponentMask{},
                       .mExclude        = ComponentMask{},
                       .mOptional       = ComponentMask{},
                       .mSends          = MessageMask{},
                       .mReceives       = MessageMask{},
                       .mReadResources  = ResourceMask{},
                       .mWriteResources = ResourceMask{},
                       .mMainThread     = false,
                       .mFunction       = nullptr,
                       .mData           = nullptr}) {}

core::Expected<SystemDesc> SystemBuilder::Fn(SystemFunction function, void* data) noexcept {
    if (function == nullptr) {
        return core::Fail(core::ErrorCode::InvalidArgument, "SystemBuilder: a system needs a function");
    }
    mDesc.mFunction = function;
    mDesc.mData     = data;
    return Build();
}

core::Expected<SystemDesc> SystemBuilder::Build() const noexcept {
    if (mDesc.mName.empty()) {
        return core::Fail(core::ErrorCode::InvalidArgument, "SystemBuilder: a system needs a name");
    }
    if (!mMissing.empty()) {
        return core::Fail(core::ErrorCode::NotFound, "SystemBuilder: a type in the access sets is not registered");
    }
    return mDesc;
}

} // namespace oyi::ecs
