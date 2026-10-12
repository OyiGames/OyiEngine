#include <oyi_transform/Affine.h>
#include <oyi_transform/Components.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

namespace {

using oyi::transform::AffineConstRef;
using oyi::transform::AffineRef;
using oyi::transform::Compose;
using oyi::transform::Float3;
using oyi::transform::LocalToWorld;
using oyi::transform::Multiply;
using oyi::transform::NearlyEqual;
using oyi::transform::Rotation;
using oyi::transform::Scale;
using oyi::transform::SetIdentity;
using oyi::transform::TransformPoint;
using oyi::transform::Translation;

constexpr float kEpsilon = 1e-5f;

[[nodiscard]] bool Near(Float3 a, Float3 b) {
    return std::fabs(a.mX - b.mX) < kEpsilon && std::fabs(a.mY - b.mY) < kEpsilon && std::fabs(a.mZ - b.mZ) < kEpsilon;
}

/// Rotation of `radians` about the Y axis.
[[nodiscard]] Rotation AboutY(float radians) {
    return Rotation{.mX = 0.0f, .mY = std::sin(radians / 2.0f), .mZ = 0.0f, .mW = std::cos(radians / 2.0f)};
}

} // namespace

TEST_CASE("the default matrices are the identity", "[transform][affine]") {
    const LocalToWorld matrix;
    LocalToWorld identity;
    SetIdentity(AffineRef{identity.mM});
    CHECK(NearlyEqual(AffineConstRef{matrix.mM}, AffineConstRef{identity.mM}, 0.0f));
    CHECK(Near(TransformPoint(AffineConstRef{matrix.mM}, Float3{.mX = 1.0f, .mY = 2.0f, .mZ = 3.0f}),
               Float3{.mX = 1.0f, .mY = 2.0f, .mZ = 3.0f}));
}

TEST_CASE("Compose applies scale, then rotation, then translation", "[transform][affine]") {
    LocalToWorld matrix;
    const Translation translation{.mValue = Float3{.mX = 1.0f, .mY = 2.0f, .mZ = 3.0f}};
    const Scale scale{.mValue = Float3{.mX = 2.0f, .mY = 2.0f, .mZ = 2.0f}};
    Compose(translation, AboutY(std::numbers::pi_v<float> / 2.0f), scale, AffineRef{matrix.mM});

    // (1, 0, 0) scaled to (2, 0, 0), rotated +90 degrees about Y to (0, 0, -2), then translated.
    CHECK(Near(TransformPoint(AffineConstRef{matrix.mM}, Float3{.mX = 1.0f, .mY = 0.0f, .mZ = 0.0f}),
               Float3{.mX = 1.0f, .mY = 2.0f, .mZ = 1.0f}));
    CHECK(Near(TransformPoint(AffineConstRef{matrix.mM}, Float3{.mX = 0.0f, .mY = 1.0f, .mZ = 0.0f}),
               Float3{.mX = 1.0f, .mY = 4.0f, .mZ = 3.0f}));
    // Column 3 is the translation (column-major layout).
    CHECK(matrix.mM[9] == 1.0f);
    CHECK(matrix.mM[10] == 2.0f);
    CHECK(matrix.mM[11] == 3.0f);
}

TEST_CASE("Multiply chains a parent and a local transform", "[transform][affine]") {
    LocalToWorld parent;
    LocalToWorld local;
    LocalToWorld world;
    Compose(Translation{.mValue = Float3{.mX = 10.0f, .mY = 0.0f, .mZ = 0.0f}},
            AboutY(std::numbers::pi_v<float> / 2.0f), Scale{}, AffineRef{parent.mM});
    Compose(Translation{.mValue = Float3{.mX = 1.0f, .mY = 0.0f, .mZ = 0.0f}}, Rotation{}, Scale{},
            AffineRef{local.mM});
    Multiply(AffineConstRef{parent.mM}, AffineConstRef{local.mM}, AffineRef{world.mM});

    // The local origin sits one unit along the parent's rotated X axis, which points to -Z.
    CHECK(Near(TransformPoint(AffineConstRef{world.mM}, Float3{}), Float3{.mX = 10.0f, .mY = 0.0f, .mZ = -1.0f}));
    // Multiplying by the identity changes nothing.
    LocalToWorld identity;
    LocalToWorld same;
    Multiply(AffineConstRef{world.mM}, AffineConstRef{identity.mM}, AffineRef{same.mM});
    CHECK(NearlyEqual(AffineConstRef{same.mM}, AffineConstRef{world.mM}, kEpsilon));
    CHECK_FALSE(NearlyEqual(AffineConstRef{same.mM}, AffineConstRef{identity.mM}, kEpsilon));
}
