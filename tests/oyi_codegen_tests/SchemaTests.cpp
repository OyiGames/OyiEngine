#include <oyi_codegen_sample/Components.gen.h>
#include <oyi_codegen_sample/Components.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace {

using namespace oyi::codegen_sample;
using oyi::core::ComponentFlags;
using oyi::core::FieldFlags;
using oyi::core::FieldKind;

TEST_CASE("generated component info matches the compiled layout", "[codegen]") {
    STATIC_REQUIRE(kPositionInfo.mSize == sizeof(Position));
    STATIC_REQUIRE(kPositionInfo.mAlign == alignof(Position));
    STATIC_REQUIRE(kHealthInfo.mSize == sizeof(Health));
    STATIC_REQUIRE(kChunkBoundsInfo.mSize == sizeof(ChunkBounds));
    STATIC_REQUIRE(kDeadInfo.mSize == sizeof(Dead));

    REQUIRE(kPositionInfo.mName == "oyi::codegen_sample::Position");
    REQUIRE(kPositionInfo.mTypeId == oyi::core::MakeTypeId("oyi::codegen_sample::Position"));
    REQUIRE(kPositionInfo.mFields.size() == 2);
    REQUIRE(kPositionInfo.mFields[0].mName == "mValue");
    REQUIRE(kPositionInfo.mFields[0].mKind == FieldKind::Struct);
    REQUIRE(kPositionInfo.mFields[0].mTypeId == kVec3Info.mTypeId);
    REQUIRE(kPositionInfo.mFields[1].mName == "mHeight");
    REQUIRE(kPositionInfo.mFields[1].mOffset == offsetof(Position, mHeight));
    REQUIRE(kPositionInfo.mFields[1].mQuantize == 0.01f);
}

TEST_CASE("the header carries one schema hash over all its records", "[codegen]") {
    STATIC_REQUIRE(kSchemaHash != 0);
    STATIC_REQUIRE(kSchemaHash != kPositionInfo.mLayoutHash);
}

TEST_CASE("component and field flags come from the annotations", "[codegen]") {
    REQUIRE(HasFlag(kPositionInfo.mFlags, ComponentFlags::Replicated));
    REQUIRE(HasFlag(kPositionInfo.mFields[0].mFlags, FieldFlags::Replicated));
    REQUIRE(HasFlag(kDeadInfo.mFlags, ComponentFlags::Tag));
    REQUIRE(kDeadInfo.mFields.empty());
    REQUIRE(HasFlag(kChunkBoundsInfo.mFlags, ComponentFlags::Chunk));
    REQUIRE_FALSE(HasFlag(kChunkBoundsInfo.mFlags, ComponentFlags::Replicated));

    // Health has one replicated field, so the component is marked replicated as a whole.
    REQUIRE(HasFlag(kHealthInfo.mFlags, ComponentFlags::Replicated));
    REQUIRE(kVec3Info.mFlags == ComponentFlags::None);
}

TEST_CASE("every field kind is classified", "[codegen]") {
    REQUIRE(kHealthInfo.mFields.size() == 10);
    REQUIRE(kHealthInfo.mFields[0].mKind == FieldKind::UInt16);
    REQUIRE(kHealthInfo.mFields[2].mKind == FieldKind::Enum);
    REQUIRE(kHealthInfo.mFields[2].mTypeId == oyi::core::MakeTypeId("oyi::codegen_sample::Team"));
    REQUIRE(kHealthInfo.mFields[3].mKind == FieldKind::Padding);
    REQUIRE(kHealthInfo.mFields[3].mCount == 3);
    REQUIRE(kHealthInfo.mFields[4].mKind == FieldKind::Struct);
    REQUIRE(kHealthInfo.mFields[4].mSize == sizeof(EntityId));
    REQUIRE(kHealthInfo.mFields[5].mKind == FieldKind::Int64);
    REQUIRE(HasFlag(kHealthInfo.mFields[5].mFlags, FieldFlags::Replicated));
    REQUIRE(HasFlag(kHealthInfo.mFields[6].mFlags, FieldFlags::Hidden));
    REQUIRE(kHealthInfo.mFields[7].mKind == FieldKind::Bool);
    REQUIRE(kHealthInfo.mFields[8].mKind == FieldKind::Padding);
    REQUIRE(kHealthInfo.mFields[8].mCount == 3);
    REQUIRE(kHealthInfo.mFields[9].mKind == FieldKind::Float64);
    REQUIRE(kHealthInfo.mFields[9].mCount == 4);
    REQUIRE(kHealthInfo.mFields[9].mSize == sizeof(double) * 4);
}

TEST_CASE("layout hash differs between components", "[codegen]") {
    REQUIRE(kPositionInfo.mLayoutHash != kHealthInfo.mLayoutHash);
    REQUIRE(kPositionInfo.mLayoutHash != 0);
}

} // namespace
