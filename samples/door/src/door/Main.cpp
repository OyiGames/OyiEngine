// The door, one object end to end (ADR-0024): a prototype with a POD component and a managed brain,
// spawned by the factory, animated by a built system, opened by a message, despawned through its
// lifecycle. One line per frame; exit code 0 when the story holds.
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/Builders.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_object/Components.h>
#include <oyi_object/ObjectFactory.h>
#include <oyi_object/ObjectLayer.h>
#include <oyi_object/Prototype.h>

#include <door/Components.gen.h> // IWYU pragma: keep
#include <door/Components.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <print>

namespace {

using namespace oyi::ecs;
using namespace oyi::object;
using door::DoorBrainRef;
using door::DoorState;
using door::OpenRequest;

/// The managed half of a door: counts complete openings; its state block is what a world file keeps.
class DoorBrain final : public ManagedObject {
    OYI_MANAGED(DoorBrain, door::DoorBrainState, Access<Read<DoorState>>)
public:
    void Update(ManagedContext& context) override { // NOLINT(misc-const-correctness): the override reads only
        const auto* const state = context.Lookup<DoorState>(mDoor, context.GetOwner());
        const bool open         = state != nullptr && state->mOpen != 0 && state->mAngle >= 90.0f;
        if (open && !mCounted) {
            ++mState.mOpened;
            std::println("brain: door opened");
        }
        mCounted = open;
    }
    void OnSpawned(FirewallWindow& window) override {
        mDoor = ComponentOf<DoorState>(window.GetWorld());
        std::println("brain: spawned with the door");
    }
    void OnDespawning(FirewallWindow& /*window*/) override {
        std::println("brain: despawning after {} opening(s)", mState.mOpened);
    }

private:
    ComponentTypeIndex mDoor;
    bool mCounted = false;
};

/// What the system and the window share: the types, the prototype, the door, the clock.
struct Story {
    ComponentTypeIndex mDoor;
    MessageTypeIndex mOpen;
    ObjectLayer* mLayer = nullptr;
    PrototypeId mPrototype{};
    EntityId mEntity{};
    uint32_t mFrame = 0;
};

/// door.animate: open requests set the target, the hinge moves toward it at its speed.
void Animate(SystemContext& context) {
    const auto* const types = context.GetData<Story>();
    const auto requests     = context.Received<OpenRequest>(types->mOpen);
    for (size_t i = 0; i < requests.mTargets.size(); ++i) {
        if (auto* const state = context.LookupForWrite<DoorState>(types->mDoor, requests.mTargets[i])) {
            state->mOpen = requests.mPayloads[i].mOpen;
        }
    }
    const float dt = context.GetEnvironment().mFixedStepSeconds;
    context.ForEachChunk([&](ChunkView& view) {
        for (DoorState& state : context.Write<DoorState>(view, types->mDoor)) {
            const float target = state.mOpen != 0 ? 90.0f : 0.0f;
            state.mAngle += std::clamp(target - state.mAngle, -state.mSpeed * dt, state.mSpeed * dt);
        }
    });
}

/// The "script": a window at the end of every frame that tells the story and reports.
void Direct(FirewallWindow& window) {
    auto* const story = window.GetData<Story>();
    ObjectFactory factory(*story->mLayer, window);
    const DoorState slower{.mSpeed = 72.0f}; // the prototype says 90 deg/s
    const std::array<ComponentValue, 1> overrides{ValueOf(story->mDoor, slower)};
    window.ForEachEntity(ComponentMask{}.With(story->mDoor), ComponentMask{},
                         [story](EntityId entity) { story->mEntity = entity; });
    switch (story->mFrame) {
    case 0:
        (void)factory.Spawn(story->mPrototype, SpawnOverrides{.mValues = overrides});
        break;
    case 2:
        window.Send(story->mOpen, story->mEntity, OpenRequest{});
        break;
    case 8:
        factory.Despawn(story->mEntity);
        break;
    default:
        break;
    }
    const auto* const state =
        window.IsAlive(story->mEntity) ? window.Get<DoorState>(story->mEntity, story->mDoor) : nullptr;
    if (state != nullptr) {
        std::println("frame {:2}: door at {:5.1f} deg", story->mFrame, state->mAngle);
    } else {
        std::println("frame {:2}: no door", story->mFrame);
    }
    ++story->mFrame;
}

} // namespace

int main() {
    oyi::core::SystemAllocator allocator;
    World world(allocator);
    if (!RegisterModule(world, door::kModule).has_value()) {
        return 1;
    }
    const ObjectTypes objectTypes                    = *RegisterObjectTypes(world);
    world.GetEnvironmentForWrite().mFixedStepSeconds = 0.25f;

    // The prototype: a door with its brain, registered from code (a blob later).
    ObjectLayer layer(allocator, world, objectTypes);
    Story story{.mDoor = ComponentOf<DoorState>(world), .mOpen = MessageOf<OpenRequest>(world), .mLayer = &layer};
    const ComponentTypeIndex brainRef = ComponentOf<DoorBrainRef>(world);
    const std::array<ManagedBinding, 1> managed{
        ManagedBinding{.mType = &DoorBrain::GetManagedTypeInfo(), .mReference = brainRef}};
    const PrototypeDesc doorDesc{
        .mName = "door", .mMask = ComponentMask{}.With(story.mDoor).With(brainRef), .mManaged = managed};
    story.mPrototype = *layer.GetPrototypes().Register(doorDesc);

    // The systems: one for the hinge, one that runs the brains; then the story window.
    Schedule schedule(allocator, world);
    const ManagedSystem brains(world.GetManaged(), brainRef, nullptr);
    const auto animate = System("door.animate", world)
                             .Phase(Phase::FixedSimulation)
                             .Writes<DoorState>()
                             .Receives<OpenRequest>()
                             .ReadsEnvironment()
                             .Fn(&Animate, &story);

    const auto think = brains.Describe(*System("door.brains", world).Build(), DoorBrain::GetManagedTypeInfo(), world);
    if (!layer.Install(schedule).has_value() || !schedule.AddSystem(*animate).has_value() ||
        !schedule.AddSystem(*think).has_value() || !schedule.Build().has_value()) {
        return 1;
    }
    schedule.SetWindow(Phase::End, &Direct, &story);
    for (int frame = 0; frame < 12; ++frame) {
        if (!schedule.RunFrameSerial().has_value()) {
            return 1;
        }
    }
    return world.GetManaged().GetLiveCount() == 0 && !world.IsAlive(story.mEntity) ? 0 : 1;
}
