#pragma once

// Component types used by the oyi_ecs tests; run through tools/codegen like any engine module.

#include <oyi_core/Annotations.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Managed.h>

#include <cstdint>

namespace oyi::ecs_fixture {

struct OYI_COMPONENT() Position {
    float mX = 0.0f;
    float mY = 0.0f;
    float mZ = 0.0f;
};

struct OYI_COMPONENT() Velocity {
    float mX = 0.0f;
    float mY = 0.0f;
    float mZ = 0.0f;
};

struct OYI_COMPONENT() Health {
    uint16_t mCurrent = 100;
    uint16_t mMax     = 100;
    oyi::ecs::EntityId mLastAttacker{};
};

/// Sixteen floats, so a 16 KB chunk holds a few hundred of these at most.
struct OYI_COMPONENT() Transform {
    float mMatrix[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                         0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

struct OYI_COMPONENT(Tag) Frozen {};

struct OYI_COMPONENT(Chunk) ChunkBounds {
    float mMin[3] = {0.0f, 0.0f, 0.0f};
    float mMax[3] = {0.0f, 0.0f, 0.0f};
};

/// Replicated field by field (ADR-0017): two quantized floats and a counter travel, the last
/// field stays local. Predicted: a client keeps its history for rollback.
struct OYI_COMPONENT(Predicted) NetPose {
    OYI_FIELD(Replicated, Quantize = 0.01) float mX = 0.0f;
    OYI_FIELD(Replicated, Quantize = 0.01) float mY = 0.0f;
    float mLocalOnly                                = 0.0f;
    OYI_FIELD(Replicated) uint16_t mHealth          = 100;
    uint8_t mPad[2]                                 = {};
};

/// The chunk-side half of a managed Brain: a handle into the pool.
struct OYI_COMPONENT() BrainRef {
    oyi::ecs::ManagedId mObject{};
};

/// Messages are plain OYI_STRUCT records.
struct OYI_STRUCT() Damage {
    oyi::ecs::EntityId mSource{};
    float mAmount = 0.0f;
};

struct OYI_STRUCT() Ping {
    uint32_t mSerial = 0;
};

/// "Move me by this much": a system turns it into velocity.
struct OYI_STRUCT() Nudge {
    float mX = 0.0f;
};

} // namespace oyi::ecs_fixture
