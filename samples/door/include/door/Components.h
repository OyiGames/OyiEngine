#pragma once

// The door's reflected types: a POD component the system animates, a message that opens it, the
// state block of its managed brain and the chunk-side handle to that brain.

#include <oyi_core/Annotations.h>
#include <oyi_ecs/ManagedObject.h>

#include <cstdint>

namespace door {

struct OYI_COMPONENT(Description = "A hinged door") DoorState {
    OYI_FIELD(Range = 0..90, Unit = deg) float mAngle = 0.0f;
    OYI_FIELD(Unit = "deg/s") float mSpeed            = 90.0f;
    uint8_t mOpen                                     = 0;
    uint8_t mPad[3]                                   = {};
};

struct OYI_STRUCT(Message) OpenRequest {
    uint8_t mOpen   = 1;
    uint8_t mPad[3] = {};
};

/// What the brain keeps across world files: how often its door opened.
struct OYI_STRUCT() DoorBrainState {
    uint32_t mOpened = 0;
};

struct OYI_COMPONENT() DoorBrainRef {
    oyi::ecs::ManagedId mObject{};
};

} // namespace door
