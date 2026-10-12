#pragma once

#include <oyi_transform/Affine.h>
#include <oyi_transform/Components.h>

#include <span>

namespace oyi::transform {

/// ADR-0014: the authoring conventions of USD (row vectors, row-major 4x4 with the translation
/// in row 3, degrees, GfQuat with the real part first, metersPerUnit) become the runtime ones
/// (column vectors, column-major 3x4, radians, (x, y, z, w), metres) in exactly one place: the
/// baker. Nothing at runtime converts.

[[nodiscard]] constexpr float DegreesToRadians(double degrees) noexcept {
    return static_cast<float>(degrees * 0.017453292519943295);
}

/// A GfQuat (real, imaginary i j k) as the runtime (x, y, z, w).
[[nodiscard]] Rotation FromUsdQuat(double real, double i, double j, double k) noexcept;

/// A point or translation in stage units to metres.
[[nodiscard]] Float3 FromUsdPoint(double x, double y, double z, double metersPerUnit) noexcept;
[[nodiscard]] Translation FromUsdTranslation(double x, double y, double z, double metersPerUnit) noexcept;

/// Scale factors carry no unit.
[[nodiscard]] Scale FromUsdScale(double x, double y, double z) noexcept;

/// A row-major 4x4 USD matrix (m[row * 4 + col], translation in row 3) to the column-major 3x4
/// affine of LocalToWorld; the translation is scaled to metres, the projective column is ignored.
void FromUsdMatrix(std::span<const double, 16> usd, double metersPerUnit, AffineRef out) noexcept;

} // namespace oyi::transform
