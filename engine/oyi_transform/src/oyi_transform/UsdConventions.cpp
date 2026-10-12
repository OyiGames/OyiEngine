#include <oyi_transform/Affine.h>
#include <oyi_transform/Components.h>
#include <oyi_transform/UsdConventions.h>

#include <cstddef>
#include <span>

namespace oyi::transform {

Rotation FromUsdQuat(double real, double i, double j, double k) noexcept {
    return Rotation{.mX = static_cast<float>(i),
                    .mY = static_cast<float>(j),
                    .mZ = static_cast<float>(k),
                    .mW = static_cast<float>(real)};
}

Float3 FromUsdPoint(double x, double y, double z, double metersPerUnit) noexcept {
    return Float3{.mX = static_cast<float>(x * metersPerUnit),
                  .mY = static_cast<float>(y * metersPerUnit),
                  .mZ = static_cast<float>(z * metersPerUnit)};
}

Translation FromUsdTranslation(double x, double y, double z, double metersPerUnit) noexcept {
    return Translation{.mValue = FromUsdPoint(x, y, z, metersPerUnit)};
}

Scale FromUsdScale(double x, double y, double z) noexcept {
    return Scale{.mValue =
                     Float3{.mX = static_cast<float>(x), .mY = static_cast<float>(y), .mZ = static_cast<float>(z)}};
}

void FromUsdMatrix(std::span<const double, 16> usd, double metersPerUnit, AffineRef out) noexcept {
    // Row vectors (v * M) are column vectors (M * v) of the transposed matrix, so the 3x3 block
    // transposes; USD keeps the translation in row 3.
    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 3; ++col) {
            out[(col * 3) + row] = static_cast<float>(usd[(col * 4) + row]);
        }
        out[9 + row] = static_cast<float>(usd[12 + row] * metersPerUnit);
    }
}

} // namespace oyi::transform
