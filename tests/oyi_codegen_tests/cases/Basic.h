#pragma once

// Golden case: one of everything the generator accepts.
// Hand-aligned for readability; this is test input, not engine code.
// clang-format off

#include <oyi_core/Annotations.h>
#include <oyi_core/AssetId.h>
#include <oyi_core/Handle.h>

#include <cstdint>

namespace oyi::golden {

struct ThingTag {};
using ThingId = oyi::core::Handle<ThingTag>;

enum class Mode : std::uint16_t {
    Off = 0,
    On  = 1,
};

struct OYI_STRUCT(Description = "A width and a height") Extent {
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
    OYI_FIELD(Range = 0..1, Step = 0.05, Unit = m, Description = "Blend weight, with a \"quoted\" word") float mWeight = 0.5f;
    OYI_FIELD(ReadOnly) std::uint32_t mCache = 7;
    OYI_FIELD(Asset = Skeleton) oyi::core::AssetId mSkeleton{};
    std::int32_t  mLimits[2] = {-4, 4};
    OYI_FIELD(Range = -10..10, Unit = "m/s") std::int8_t mSigned = -3;
    std::uint8_t  mPad1[7]   = {};
};

struct OYI_COMPONENT(Transient, ReadOnly, Description = "Rebuilt every frame by a system") Scratch {
    float mValue = 0.0f;
};

struct OYI_COMPONENT(Tag, Replicated) Marker {};

} // namespace oyi::golden

// clang-format on
