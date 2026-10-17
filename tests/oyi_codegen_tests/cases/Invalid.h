#pragma once

// Golden case: every rule the validator enforces, one violation each (see expected/Invalid.errors.txt).
// Hand-aligned; the expected diagnostics carry line and column numbers, so keep clang-format away.
// clang-format off

#include <oyi_core/Annotations.h>

#include <cstdint>

namespace oyi::golden {

struct Plain {
    int mValue = 0;
};

class OYI_COMPONENT() NotAStruct {
public:
    std::uint32_t mValue = 0;
};

struct OYI_COMPONENT(Flying) BadOptions : Plain {
    int            mPlainInt = 0;
    std::uint64_t* mPointer  = nullptr;
    Plain          mPlain{};
    std::uint32_t  mUninitialized;
    std::uint8_t   mBits : 3 = 0;
    OYI_FIELD(Quantize = 0.5) std::uint8_t mQuantizedInt = 0;
    OYI_FIELD(Loud) float mUnknownOption = 0.0f;
    std::uint16_t mPadX = 0;
    std::uint8_t  mPad[4][4] = {};
    virtual void Update();
    BadOptions();
private:
    std::uint8_t mSecret = 0;
};

enum Legacy { kLegacyA = 0 };

struct OYI_COMPONENT() BadEnum {
    Legacy mLegacy = kLegacyA;
    std::uint8_t mPad[4] = {};
};

struct OYI_COMPONENT() ImplicitPadding {
    std::uint8_t  mSmall = 0;
    std::uint32_t mWide  = 0;
    std::uint16_t mTail  = 0;
};

struct OYI_COMPONENT(Tag) NotEmpty {
    std::uint8_t mValue = 0;
};

struct OYI_COMPONENT() Nested {
    struct Inner {
        std::uint8_t mValue = 0;
    };
    std::uint8_t mValue = 0;
};

struct OYI_COMPONENT(Transient, Replicated) BadTransient {
    float mValue = 0.0f;
};

struct OYI_COMPONENT(Description = Unquoted) BadDescription {
    float mValue = 0.0f;
};

struct OYI_COMPONENT() BadToolOptions {
    OYI_FIELD(Asset = Mesh) std::uint64_t mNotAnAsset = 0;
    OYI_FIELD(Range = 0..1) bool mRangeOnBool = false;
    std::uint8_t mPad[3] = {};
    OYI_FIELD(Range = 5..1) float mInverted = 0.0f;
    OYI_FIELD(Step = -1) float mNegativeStep = 0.0f;
    OYI_FIELD(Description = plain) float mBareDescription = 0.0f;
};

} // namespace oyi::golden

// clang-format on
