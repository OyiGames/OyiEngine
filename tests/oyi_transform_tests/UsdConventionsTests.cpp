#include <oyi_transform/Affine.h>
#include <oyi_transform/Components.h>
#include <oyi_transform/UsdConventions.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

namespace {

using Catch::Approx;
using oyi::transform::Compose;
using oyi::transform::DegreesToRadians;
using oyi::transform::Float3;
using oyi::transform::FromUsdMatrix;
using oyi::transform::FromUsdPoint;
using oyi::transform::FromUsdQuat;
using oyi::transform::FromUsdScale;
using oyi::transform::FromUsdTranslation;
using oyi::transform::NearlyEqual;
using oyi::transform::Rotation;
using oyi::transform::Scale;
using oyi::transform::Translation;

} // namespace

TEST_CASE("angles, quaternions, points and scales take the runtime conventions", "[transform][usd]") {
    CHECK(DegreesToRadians(180.0) == Approx(3.14159265f));
    CHECK(DegreesToRadians(0.0) == 0.0f);
    const Rotation rotation = FromUsdQuat(0.5, 0.1, 0.2, 0.3); // GfQuat(real, i, j, k)
    CHECK(rotation.mX == Approx(0.1f));
    CHECK(rotation.mY == Approx(0.2f));
    CHECK(rotation.mZ == Approx(0.3f));
    CHECK(rotation.mW == Approx(0.5f));
    const Float3 point = FromUsdPoint(100.0, 200.0, -50.0, 0.01); // a centimetre stage
    CHECK(point.mX == Approx(1.0f));
    CHECK(point.mY == Approx(2.0f));
    CHECK(point.mZ == Approx(-0.5f));
    CHECK(FromUsdTranslation(3.0, 0.0, 0.0, 1.0).mValue.mX == 3.0f);
    const Scale scale = FromUsdScale(2.0, 3.0, 4.0);
    CHECK(scale.mValue.mY == 3.0f);
}

TEST_CASE("a USD matrix becomes the same affine Compose builds from its TRS", "[transform][usd]") {
    const Translation translation{.mValue = Float3{.mX = 1.0f, .mY = -2.0f, .mZ = 3.5f}};
    const Rotation rotation = FromUsdQuat(0.8660254, 0.0, 0.5, 0.0); // 60 degrees about Y
    const Scale scale{.mValue = Float3{.mX = 2.0f, .mY = 1.0f, .mZ = 0.5f}};
    std::array<float, 12> expected{};
    Compose(translation, rotation, scale, expected);

    // The same transform the way USD stores it: row vectors, so the 3x3 block is transposed and
    // the translation sits in row 3, in centimetres on a 0.01 metersPerUnit stage.
    std::array<double, 16> usd{};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 3; ++col) {
            usd[(row * 4) + col] = expected[(row * 3) + col];
        }
    }
    usd[12] = 100.0;
    usd[13] = -200.0;
    usd[14] = 350.0;
    usd[15] = 1.0;

    std::array<float, 12> converted{};
    FromUsdMatrix(usd, 0.01, converted);
    CHECK(NearlyEqual(converted, expected, 1e-5f));

    std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<float, 12> fromIdentity{};
    FromUsdMatrix(identity, 1.0, fromIdentity);
    std::array<float, 12> id{};
    oyi::transform::SetIdentity(id);
    CHECK(NearlyEqual(fromIdentity, id, 0.0f));
}
