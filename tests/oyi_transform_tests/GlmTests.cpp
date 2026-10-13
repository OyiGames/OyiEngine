#include <oyi_transform/Affine.h>
#include <oyi_transform/Components.h>
#include <oyi_transform/Glm.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/ext/vector_float4.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp> // mat4_cast

#include <array>
#include <cstddef>

namespace {

using Catch::Approx;
using oyi::transform::Compose;
using oyi::transform::Float3;
using oyi::transform::FromGlm;
using oyi::transform::Rotation;
using oyi::transform::Scale;
using oyi::transform::ToGlm;
using oyi::transform::TransformPoint;
using oyi::transform::Translation;

} // namespace

TEST_CASE("vectors and quaternions cross to GLM and back unchanged", "[transform][glm]") {
    const Float3 point{.mX = 1.0f, .mY = -2.0f, .mZ = 3.0f};
    const glm::vec3 vector = ToGlm(point);
    CHECK(vector.y == -2.0f);
    const Float3 back = FromGlm(vector);
    CHECK(back.mX == 1.0f);
    CHECK(back.mZ == 3.0f);

    const Rotation rotation{.mX = 0.1f, .mY = 0.2f, .mZ = 0.3f, .mW = 0.9f};
    const glm::quat quaternion = ToGlm(rotation);
    CHECK(quaternion.w == 0.9f);
    CHECK(quaternion.x == 0.1f);
    const Rotation restored = FromGlm(quaternion);
    CHECK(restored.mZ == 0.3f);
    CHECK(restored.mW == 0.9f);
}

TEST_CASE("Compose agrees with GLM's translate, rotate and scale", "[transform][glm]") {
    const Translation translation{.mValue = Float3{.mX = 2.0f, .mY = 3.0f, .mZ = -1.0f}};
    const glm::quat spin    = glm::angleAxis(0.7f, glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
    const Rotation rotation = FromGlm(spin);
    const Scale scale{.mValue = Float3{.mX = 2.0f, .mY = 0.5f, .mZ = 1.5f}};

    std::array<float, 12> ours{};
    Compose(translation, rotation, scale, ours);
    const glm::mat4 theirs = glm::translate(glm::mat4(1.0f), ToGlm(translation.mValue)) * glm::mat4_cast(spin) *
                             glm::scale(glm::mat4(1.0f), ToGlm(scale.mValue));
    const glm::mat4 lifted = ToGlm(ours);
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            CHECK(lifted[column][row] == Approx(theirs[column][row]).margin(1e-5f));
        }
    }
    // Points agree too, and the 4x4 comes back as the same 3x4.
    const Float3 p{.mX = 0.5f, .mY = -0.25f, .mZ = 4.0f};
    const Float3 oursPoint   = TransformPoint(ours, p);
    const glm::vec4 theirsPt = theirs * glm::vec4(ToGlm(p), 1.0f);
    CHECK(oursPoint.mX == Approx(theirsPt.x).margin(1e-5f));
    CHECK(oursPoint.mY == Approx(theirsPt.y).margin(1e-5f));
    CHECK(oursPoint.mZ == Approx(theirsPt.z).margin(1e-5f));
    std::array<float, 12> roundTrip{};
    FromGlm(theirs, roundTrip);
    for (size_t i = 0; i < 12; ++i) {
        CHECK(roundTrip[i] == Approx(ours[i]).margin(1e-5f));
    }
}
