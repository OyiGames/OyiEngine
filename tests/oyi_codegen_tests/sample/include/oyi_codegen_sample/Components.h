#pragma once

// Fixture module for tools/codegen: exercises every field kind the generator supports.
// Not part of the engine; see tests/oyi_codegen_tests.

#include <oyi_core/Annotations.h>
#include <oyi_core/AssetId.h>
#include <oyi_core/Handle.h>

#include <cstdint>

namespace oyi::codegen_sample {

struct EntityTag {};
using EntityId = oyi::core::Handle<EntityTag>;

enum class Team : uint8_t {
    Neutral = 0,
    Red     = 1,
    Blue    = 2,
};

struct OYI_STRUCT() Vec3 {
    float mX = 0.0f;
    float mY = 0.0f;
    float mZ = 0.0f;
};

struct OYI_COMPONENT(Replicated) Position {
    Vec3 mValue{};
    OYI_FIELD(Quantize = 0.01) float mHeight = 0.0f;
};

struct OYI_COMPONENT() Health {
    uint16_t mCurrent = 100;
    uint16_t mMax     = 100;
    Team mTeam        = Team::Neutral;
    uint8_t mPad[3]   = {};
    EntityId mOwner{};
    OYI_FIELD(Replicated) int64_t mSeed  = 0;
    OYI_FIELD(Hidden) uint32_t mInternal = 0;
    bool mAlive                          = true;
    uint8_t mPad2[3]                     = {};
    double mWeights[4]                   = {};
};

struct OYI_COMPONENT(Tag) Dead {};

/// A message record (ADR-0024): listed under kModule.mMessages, not mStructs.
struct OYI_STRUCT(Message) Poke {
    float mStrength = 1.0f;
};

struct OYI_COMPONENT(Chunk) ChunkBounds {
    Vec3 mMin{};
    Vec3 mMax{};
};

/// Tool-facing options (ADR-0024): never replicated, saved as defaults, described for inspectors.
struct OYI_COMPONENT(Transient, Description = "Rebuilt every frame") Scratch {
    OYI_FIELD(Range = 0..1, Step = 0.1, Unit = m, Description = "Blend weight") float mBlend = 0.5f;
    OYI_FIELD(ReadOnly) uint32_t mCache                                                      = 0;
    OYI_FIELD(Asset = Mesh) oyi::core::AssetId mMesh {};
};

} // namespace oyi::codegen_sample
