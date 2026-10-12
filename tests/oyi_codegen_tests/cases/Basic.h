#pragma once

// Golden case: one of everything the generator accepts.
// Hand-aligned for readability; this is test input, not engine code.
// clang-format off

#include <oyi_core/Annotations.h>
#include <oyi_core/Handle.h>

#include <cstdint>

namespace oyi::golden {

struct ThingTag {};
using ThingId = oyi::core::Handle<ThingTag>;

enum class Mode : std::uint16_t {
    Off = 0,
    On  = 1,
};

struct OYI_STRUCT() Extent {
    std::int32_t mWidth  = 0;
    std::int32_t mHeight = 0;
};

struct OYI_COMPONENT() Everything {
    bool          mFlag      = false;
    std::int8_t   mTiny      = -1;
    std::uint8_t  mPad0[4]   = {};
    Mode          mMode      = Mode::Off;
    std::uint32_t mCount     = 0;
    float         mRatio     = 1.0f;
    std::int64_t  mBig       = 0;
    double        mPrecise   = 0.0;
    OYI_FIELD(Replicated, Quantize = 0.5) float mSpeed = 0.0f;
    Extent        mExtent{};
    ThingId       mOther{};
    std::uint16_t mSamples[6] = {};
};

struct OYI_COMPONENT(Tag, Replicated) Marker {};

} // namespace oyi::golden

// clang-format on
