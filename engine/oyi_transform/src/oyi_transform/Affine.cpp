#include <oyi_transform/Affine.h>
#include <oyi_transform/Components.h>

#include <cmath>
#include <cstddef>
#include <span>

namespace oyi::transform {

namespace {

[[nodiscard]] constexpr size_t At(size_t row, size_t column) noexcept {
    return (column * 3) + row;
}

} // namespace

void SetIdentity(AffineRef out) noexcept {
    for (size_t i = 0; i < 12; ++i) {
        out[i] = 0.0f;
    }
    out[At(0, 0)] = 1.0f;
    out[At(1, 1)] = 1.0f;
    out[At(2, 2)] = 1.0f;
}

void Compose(const Translation& translation, const Rotation& rotation, const Scale& scale, AffineRef out) noexcept {
    const float x  = rotation.mX;
    const float y  = rotation.mY;
    const float z  = rotation.mZ;
    const float w  = rotation.mW;
    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;

    // Rotation matrix from the unit quaternion, each column scaled by the matching scale factor.
    out[At(0, 0)] = (1.0f - (2.0f * (yy + zz))) * scale.mValue.mX;
    out[At(1, 0)] = (2.0f * (xy + wz)) * scale.mValue.mX;
    out[At(2, 0)] = (2.0f * (xz - wy)) * scale.mValue.mX;

    out[At(0, 1)] = (2.0f * (xy - wz)) * scale.mValue.mY;
    out[At(1, 1)] = (1.0f - (2.0f * (xx + zz))) * scale.mValue.mY;
    out[At(2, 1)] = (2.0f * (yz + wx)) * scale.mValue.mY;

    out[At(0, 2)] = (2.0f * (xz + wy)) * scale.mValue.mZ;
    out[At(1, 2)] = (2.0f * (yz - wx)) * scale.mValue.mZ;
    out[At(2, 2)] = (1.0f - (2.0f * (xx + yy))) * scale.mValue.mZ;

    out[At(0, 3)] = translation.mValue.mX;
    out[At(1, 3)] = translation.mValue.mY;
    out[At(2, 3)] = translation.mValue.mZ;
}

void Multiply(AffineConstRef parent, AffineConstRef local, AffineRef out) noexcept {
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 3; ++row) {
            float sum = 0.0f;
            for (size_t k = 0; k < 3; ++k) {
                sum += parent[At(row, k)] * local[At(k, column)];
            }
            if (column == 3) {
                sum += parent[At(row, 3)];
            }
            out[At(row, column)] = sum;
        }
    }
}

Float3 TransformPoint(AffineConstRef matrix, Float3 point) noexcept {
    return Float3{
        .mX = (matrix[At(0, 0)] * point.mX) + (matrix[At(0, 1)] * point.mY) + (matrix[At(0, 2)] * point.mZ) +
              matrix[At(0, 3)],
        .mY = (matrix[At(1, 0)] * point.mX) + (matrix[At(1, 1)] * point.mY) + (matrix[At(1, 2)] * point.mZ) +
              matrix[At(1, 3)],
        .mZ = (matrix[At(2, 0)] * point.mX) + (matrix[At(2, 1)] * point.mY) + (matrix[At(2, 2)] * point.mZ) +
              matrix[At(2, 3)],
    };
}

bool NearlyEqual(AffineConstRef a, AffineConstRef b, float epsilon) noexcept {
    for (size_t i = 0; i < 12; ++i) {
        if (std::fabs(a[i] - b[i]) > epsilon) {
            return false;
        }
    }
    return true;
}

} // namespace oyi::transform
