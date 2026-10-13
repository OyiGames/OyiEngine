#pragma once

#include <oyi_transform/Affine.h>
#include <oyi_transform/Components.h>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/vector_float3.hpp>

#include <cstddef>

namespace oyi::transform {

/// GLM (ADR-0018) for scalar math in gameplay and tools; the components stay the plain POD
/// types codegen knows. Conventions match ADR-0014 on both sides: column vectors, column-major
/// matrices, quaternions as (x, y, z, w) in memory.

[[nodiscard]] inline glm::vec3 ToGlm(Float3 value) noexcept {
    return {value.mX, value.mY, value.mZ};
}

[[nodiscard]] inline Float3 FromGlm(const glm::vec3& value) noexcept {
    return Float3{.mX = value.x, .mY = value.y, .mZ = value.z};
}

[[nodiscard]] inline glm::quat ToGlm(const Rotation& rotation) noexcept {
    return {rotation.mW, rotation.mX, rotation.mY, rotation.mZ}; // glm::quat takes (w, x, y, z)
}

[[nodiscard]] inline Rotation FromGlm(const glm::quat& rotation) noexcept {
    return Rotation{.mX = rotation.x, .mY = rotation.y, .mZ = rotation.z, .mW = rotation.w};
}

/// The 3x4 affine as a 4x4 with the usual last row.
[[nodiscard]] inline glm::mat4 ToGlm(AffineConstRef affine) noexcept {
    glm::mat4 out(1.0f);
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 3; ++row) {
            out[static_cast<glm::length_t>(column)][static_cast<glm::length_t>(row)] = affine[(column * 3) + row];
        }
    }
    return out;
}

/// The upper 3x4 of a 4x4; the projective row is dropped.
inline void FromGlm(const glm::mat4& matrix, AffineRef out) noexcept {
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 3; ++row) {
            out[(column * 3) + row] = matrix[static_cast<glm::length_t>(column)][static_cast<glm::length_t>(row)];
        }
    }
}

} // namespace oyi::transform
