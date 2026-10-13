#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

namespace {

using oyi::core::SystemAllocator;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::kAllWorldKinds;
using oyi::ecs::KindBit;
using oyi::ecs::kPresentationWorldKinds;
using oyi::ecs::MessageMask;
using oyi::ecs::Phase;
using oyi::ecs::ResourceMask;
using oyi::ecs::Schedule;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::SystemId;
using oyi::ecs::World;
using oyi::ecs::WorldKind;
using namespace oyi::ecs_fixture;

struct Trace {
    std::vector<std::string_view> mOrder;
};

void Simulate(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("simulate");
}
void Present(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("present");
}
void Audit(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("audit");
}

[[nodiscard]] SystemDesc Describe(std::string_view name, ComponentMask write, oyi::ecs::SystemFunction function,
                                  void* data) {
    return SystemDesc{.mName           = name,
                      .mPhase          = Phase::Variable,
                      .mRead           = ComponentMask{},
                      .mWrite          = write,
                      .mExclude        = ComponentMask{},
                      .mOptional       = ComponentMask{},
                      .mSends          = MessageMask{},
                      .mReceives       = MessageMask{},
                      .mReadResources  = ResourceMask{},
                      .mWriteResources = ResourceMask{},
                      .mMainThread     = false,
                      .mFunction       = function,
                      .mData           = data};
}

/// simulate (everywhere) -> present (not on servers) -> audit (servers only); present and audit
/// both write Position, ordered through present so that a server without it still has a path.
struct Fixture {
    SystemAllocator mAllocator;
    World mWorld;
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    Schedule mSchedule{mAllocator, mWorld};
    Trace mTrace;
    SystemId mSimulate;
    SystemId mPresent;
    SystemId mAudit;

    explicit Fixture(WorldKind kind) : mWorld(mAllocator, World::Config{.mChunkSize = 16 * 1024, .mKind = kind}) {
        mSimulate = *mSchedule.AddSystem(Describe("simulate", ComponentMask{}.With(mPosition), &Simulate, &mTrace));
        mPresent  = *mSchedule.AddSystem(Describe("present", ComponentMask{}.With(mPosition), &Present, &mTrace),
                                         kPresentationWorldKinds);
        mAudit    = *mSchedule.AddSystem(Describe("audit", ComponentMask{}.With(mPosition), &Audit, &mTrace),
                                         KindBit(WorldKind::Server));
        REQUIRE(mSchedule.AddOrder(mSimulate, mPresent).has_value());
        REQUIRE(mSchedule.AddOrder(mPresent, mAudit).has_value());
        REQUIRE(mSchedule.AddOrder(mSimulate, mAudit).has_value());
        REQUIRE(mSchedule.Build().has_value());
        REQUIRE(mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    }
};

} // namespace

TEST_CASE("a client world runs the presentation systems and skips the server-only ones", "[ecs][worldkind]") {
    const Fixture client(WorldKind::Client);
    CHECK(client.mWorld.GetKind() == WorldKind::Client);
    CHECK(client.mSchedule.IsSystemIncluded(client.mSimulate));
    CHECK(client.mSchedule.IsSystemIncluded(client.mPresent));
    CHECK_FALSE(client.mSchedule.IsSystemIncluded(client.mAudit));
    CHECK(client.mSchedule.GetOrder(Phase::Variable).size() == 2);
    CHECK(client.mTrace.mOrder == std::vector<std::string_view>{"simulate", "present"});
}

TEST_CASE("a server world never instantiates presentation systems", "[ecs][worldkind]") {
    const Fixture server(WorldKind::Server);
    CHECK_FALSE(server.mSchedule.IsSystemIncluded(server.mPresent));
    CHECK(server.mSchedule.IsSystemIncluded(server.mAudit));
    CHECK(server.mSchedule.GetOrder(Phase::Variable).size() == 2);
    // The edge through the missing present system does not block audit.
    CHECK(server.mTrace.mOrder == std::vector<std::string_view>{"simulate", "audit"});
    CHECK(server.mSchedule.GetSystemCount() == 3); // ids stay valid
    CHECK(server.mSchedule.GetDesc(server.mPresent).mName == "present");
}

TEST_CASE("the default world is a client and the default system belongs everywhere", "[ecs][worldkind]") {
    SystemAllocator allocator;
    const World world(allocator);
    CHECK(world.GetKind() == WorldKind::Client);
    CHECK(kAllWorldKinds == 0xFF);
    CHECK((kPresentationWorldKinds & KindBit(WorldKind::Server)) == 0);
    CHECK((kPresentationWorldKinds & KindBit(WorldKind::Editor)) != 0);
    CHECK(KindBit(WorldKind::Baking) == 8);
}
