#pragma once

#include <oyi_core/Handle.h>

namespace oyi::ecs {

struct EntityTag {};

/// Runtime handle of an entity (ADR-0004): 32-bit index plus 32-bit generation, valid only within
/// the World that created it. Persistent identity (save games, networking) is a separate stable id.
using EntityId = core::Handle<EntityTag>;

} // namespace oyi::ecs
