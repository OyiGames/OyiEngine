#include <oyi_asset/Blob.h>
#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_object/Components.gen.h> // IWYU pragma: keep
#include <oyi_object/Components.h>
#include <oyi_object/ObjectFactory.h>
#include <oyi_object/ObjectLayer.h>
#include <oyi_object/Prototype.h>
#include <oyi_object/PrototypeBlob.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

namespace {

using oyi::asset::BlobExpectation;
using oyi::asset::BlobView;
using oyi::asset::BlobWriter;
using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::EntityMark;
using oyi::ecs::FirewallWindow;
using oyi::ecs::ManagedContext;
using oyi::ecs::ManagedObject;
using oyi::ecs::Schedule;
using oyi::ecs::World;
using oyi::object::ComponentValue;
using oyi::object::kPrototypeAssetKind;
using oyi::object::kPrototypeBakerVersion;
using oyi::object::ManagedBinding;
using oyi::object::ObjectFactory;
using oyi::object::ObjectInfo;
using oyi::object::ObjectLayer;
using oyi::object::ObjectTypes;
using oyi::object::Prototype;
using oyi::object::PrototypeBlob;
using oyi::object::PrototypeBlobExpectation;
using oyi::object::PrototypeDesc;
using oyi::object::PrototypeId;
using oyi::object::PrototypeNode;
using oyi::object::RegisterObjectTypes;
using oyi::object::ValueOf;
using oyi::object::WritePrototypeBlob;
using namespace oyi::ecs_fixture;

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

class Brain final : public ManagedObject {
    OYI_MANAGED(Brain, BrainState, oyi::ecs::Access<>)
public:
    void Update(ManagedContext& /*context*/) override { ++mState.mUpdates; }
};

/// A world with the fixture's components, the object types and (optionally) the Brain type.
struct Side {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mHealth   = *mWorld.GetComponents().Register<Health>();
    ComponentTypeIndex mBrainRef = *mWorld.GetComponents().Register<BrainRef>();
    ObjectTypes mTypes           = *RegisterObjectTypes(mWorld);
    ObjectLayer mLayer{mAllocator, mWorld, mTypes};

    explicit Side(bool withBrain = true) {
        if (withBrain) {
            REQUIRE(mWorld.RegisterManaged<Brain>().has_value());
        }
    }
};

/// The door tree described from code: root with defaults and a brain, one child with defaults.
struct DoorDesc {
    Position mDoorPosition{.mX = 1.0f, .mY = 2.0f, .mZ = 3.0f};
    Position mPanelPosition{.mX = 9.0f, .mY = 9.0f, .mZ = 9.0f};
    Health mPanelHealth{.mCurrent = 5, .mMax = 5, .mLastAttacker = {}};
    std::array<ComponentValue, 1> mDoorDefaults;
    std::array<ComponentValue, 2> mPanelDefaults;
    std::array<ManagedBinding, 1> mDoorManaged;
    PrototypeDesc mPanel;
    std::array<const PrototypeDesc*, 1> mChildren;
    PrototypeDesc mDoor;

    explicit DoorDesc(const Side& side)
        : mDoorDefaults{ValueOf(side.mPosition, mDoorPosition)}
        , mPanelDefaults{ValueOf(side.mPosition, mPanelPosition), ValueOf(side.mHealth, mPanelHealth)}
        , mDoorManaged{ManagedBinding{
              .mType = &Brain::GetManagedTypeInfo(), .mReference = side.mBrainRef, .mOffset = 0}}
        , mPanel{.mName = "door.panel", .mMask = MaskOf({side.mPosition, side.mHealth}), .mDefaults = mPanelDefaults}
        , mChildren{&mPanel}
        , mDoor{.mName     = "door",
                .mMask     = MaskOf({side.mPosition, side.mHealth, side.mBrainRef}),
                .mDefaults = mDoorDefaults,
                .mManaged  = mDoorManaged,
                .mChildren = mChildren} {}
};

[[nodiscard]] std::pmr::vector<std::byte> WriteDoor(Side& side) {
    const DoorDesc desc(side);
    std::pmr::vector<std::byte> bytes;
    REQUIRE(WritePrototypeBlob(side.mWorld, desc.mDoor, side.mAllocator, bytes).has_value());
    return bytes;
}

struct SpawnOnce {
    ObjectLayer* mLayer = nullptr;
    PrototypeId mPrototype;
};

void SpawnInWindow(FirewallWindow& window) {
    const auto* const self = window.GetData<SpawnOnce>();
    (void)ObjectFactory(*self->mLayer, window).Spawn(self->mPrototype);
}

} // namespace

TEST_CASE("a prototype tree round-trips through a blob into another world", "[object][prototype][blob]") {
    Side writer;
    const std::pmr::vector<std::byte> bytes = WriteDoor(writer);
    CHECK(bytes == WriteDoor(writer));                    // deterministic
    CHECK(writer.mLayer.GetPrototypes().GetCount() == 0); // writing registers nothing

    Side reader;
    const BlobExpectation expectation = PrototypeBlobExpectation(reader.mWorld);
    CHECK(expectation.mKind == kPrototypeAssetKind);
    CHECK(expectation.mVersion == kPrototypeBakerVersion);
    CHECK(expectation.mSchemaHash == writer.mWorld.ComputeSchemaHash());
    const auto blob = BlobView::Open(bytes, expectation);
    REQUIRE(blob.has_value());
    CHECK(blob->Root<PrototypeBlob>()->mNodes.Get().size() == 2);

    const auto door = reader.mLayer.GetPrototypes().Load(*blob);
    REQUIRE(door.has_value());
    CHECK(reader.mLayer.GetPrototypes().GetCount() == 2);
    CHECK(reader.mLayer.GetPrototypes().Find("door") == *door);
    const Prototype& root = reader.mLayer.GetPrototypes().Get(*door);
    CHECK(root.mName == "door");
    CHECK_FALSE(root.mParent.IsValid());
    CHECK(root.mMask == MaskOf({reader.mPosition, reader.mHealth, reader.mBrainRef}).With(reader.mTypes.mObjectInfo));
    REQUIRE(root.mManaged.size() == 1);
    CHECK(root.mManaged[0].mType == &Brain::GetManagedTypeInfo());
    CHECK(root.mManaged[0].mReference == reader.mBrainRef);
    CHECK(root.mManaged[0].mOffset == 0);
    REQUIRE(root.mChildren.size() == 1);
    const Prototype& panel = reader.mLayer.GetPrototypes().Get(root.mChildren[0]);
    CHECK(panel.mName == "door.panel");
    CHECK(panel.mParent == *door);
    CHECK(panel.mMask == MaskOf({reader.mPosition, reader.mHealth}).With(reader.mTypes.mObjectInfo));
    CHECK(panel.mManaged.empty());
    CHECK(panel.mChildren.empty());

    // The prototype entities carry the defaults the blob brought along.
    CHECK(reader.mWorld.GetMark(root.mEntity) == EntityMark::Prototype);
    CHECK(reader.mWorld.Get<Position>(root.mEntity)->mY == 2.0f);
    CHECK(reader.mWorld.Get<Health>(root.mEntity)->mCurrent == 100);
    CHECK(reader.mWorld.Get<Position>(panel.mEntity)->mZ == 9.0f);
    CHECK(reader.mWorld.Get<Health>(panel.mEntity)->mCurrent == 5);
    CHECK(reader.mWorld.Get<ObjectInfo>(root.mEntity)->mPrototype == *door);

    // And the loaded prototype spawns like one registered from code.
    Schedule schedule(reader.mAllocator, reader.mWorld);
    REQUIRE(reader.mLayer.Install(schedule).has_value());
    SpawnOnce spawn{.mLayer = &reader.mLayer, .mPrototype = *door};
    REQUIRE(schedule.RunWindow(&SpawnInWindow, &spawn).has_value());
    CHECK(reader.mWorld.GetEntityCount() == 4);
    CHECK(reader.mWorld.GetManaged().GetLiveCount() == 1);
    CHECK(reader.mLayer.GetLastHookStats().mSpawned == 2);

    // Loading it again under the same names is a clash, as from code.
    CHECK(reader.mLayer.GetPrototypes().Load(*blob).error().mCode == ErrorCode::AlreadyExists);
}

TEST_CASE("a prototype blob is refused by worlds that cannot resolve it and by damaged trees",
          "[object][prototype][blob]") {
    Side writer;
    const std::pmr::vector<std::byte> bytes = WriteDoor(writer);

    // No Health, no Brain: the components and the managed type must exist first.
    {
        SystemAllocator allocator;
        World world(allocator);
        REQUIRE(world.GetComponents().Register<Position>().has_value());
        REQUIRE(world.GetComponents().Register<BrainRef>().has_value());
        const ObjectTypes types = *RegisterObjectTypes(world);
        ObjectLayer layer(allocator, world, types);
        const auto blob = BlobView::Open(bytes, BlobExpectation{.mKind = kPrototypeAssetKind});
        REQUIRE(blob.has_value());
        CHECK(layer.GetPrototypes().Load(*blob).error().mCode == ErrorCode::NotFound);
        CHECK(BlobView::Open(bytes, PrototypeBlobExpectation(world)).error().mCode == ErrorCode::InvalidArgument);
    }
    {
        Side noBrain(false);
        const auto blob = BlobView::Open(bytes, PrototypeBlobExpectation(noBrain.mWorld));
        REQUIRE(blob.has_value());
        CHECK(noBrain.mLayer.GetPrototypes().Load(*blob).error().mCode == ErrorCode::NotFound);
        CHECK(noBrain.mLayer.GetPrototypes().GetCount() == 0); // the root failed before registering
    }

    // Damaged bytes never get past Open; a tree with a loop or a dangling child is CorruptData.
    CHECK(BlobView::Open(std::span<const std::byte>(bytes).subspan(0, bytes.size() - 8)).error().mCode ==
          ErrorCode::CorruptData);
    Side reader;
    const auto craft = [&](const char* name4, uint32_t childIndex) {
        BlobWriter blobWriter(reader.mAllocator);
        const BlobWriter::Offset root  = blobWriter.Allocate<PrototypeBlob>();
        const BlobWriter::Offset nodes = blobWriter.Allocate<PrototypeNode>(1);
        const BlobWriter::Offset name  = blobWriter.Allocate(4, 1);
        std::memcpy(&blobWriter.At<char>(name), name4, 4);
        const BlobWriter::Offset child = blobWriter.Allocate<uint32_t>(1);
        blobWriter.At<uint32_t>(child) = childIndex;
        blobWriter.LinkSpan<PrototypeNode>(root + offsetof(PrototypeBlob, mNodes), nodes, 1);
        blobWriter.LinkSpan<char>(nodes + offsetof(PrototypeNode, mName), name, 4);
        blobWriter.LinkSpan<uint32_t>(nodes + offsetof(PrototypeNode, mChildren), child, 1);
        blobWriter.SetRoot(root);
        std::pmr::vector<std::byte> out;
        blobWriter.Finish(kPrototypeAssetKind, kPrototypeBakerVersion, reader.mWorld.ComputeSchemaHash(), out);
        return out;
    };
    const std::pmr::vector<std::byte> loop = craft("loop", 0);
    CHECK(reader.mLayer.GetPrototypes()
              .Load(*BlobView::Open(loop, PrototypeBlobExpectation(reader.mWorld)))
              .error()
              .mCode == ErrorCode::CorruptData);
    const std::pmr::vector<std::byte> dangling = craft("dang", 7);
    CHECK(reader.mLayer.GetPrototypes()
              .Load(*BlobView::Open(dangling, PrototypeBlobExpectation(reader.mWorld)))
              .error()
              .mCode == ErrorCode::CorruptData);

    // Writing refuses what the registry cannot describe.
    const std::array<ComponentValue, 1> wrongSize{
        ComponentValue{.mType = writer.mPosition, .mData = &writer, .mSize = 3}};
    const PrototypeDesc badDefault{.mName = "bad", .mMask = MaskOf({writer.mPosition}), .mDefaults = wrongSize};
    std::pmr::vector<std::byte> out;
    CHECK(WritePrototypeBlob(writer.mWorld, badDefault, writer.mAllocator, out).error().mCode ==
          ErrorCode::InvalidArgument);
    const PrototypeDesc unnamed{.mName = "", .mMask = MaskOf({writer.mPosition})};
    CHECK(WritePrototypeBlob(writer.mWorld, unnamed, writer.mAllocator, out).error().mCode ==
          ErrorCode::InvalidArgument);
    CHECK(out.empty());
}
