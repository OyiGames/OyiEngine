#pragma once

#include <oyi_transform/Components.h>

#include <span>

namespace oyi::transform {

/// Helpers over the column-major 3x4 layout of LocalToWorld / LocalToParent (ADR-0014: column
/// vectors, M * v). Element (row r, column c) lives at index c * 3 + r.
using AffineRef      = std::span<float, 12>;
using AffineConstRef = std::span<const float, 12>;

void SetIdentity(AffineRef out) noexcept;

/// out = T * R * S: scale first, then rotate, then translate.
void Compose(const Translation& translation, const Rotation& rotation, const Scale& scale, AffineRef out) noexcept;

/// out = parent * local. `out` may alias neither input.
void Multiply(AffineConstRef parent, AffineConstRef local, AffineRef out) noexcept;

[[nodiscard]] Float3 TransformPoint(AffineConstRef matrix, Float3 point) noexcept;

[[nodiscard]] bool NearlyEqual(AffineConstRef a, AffineConstRef b, float epsilon) noexcept;

} // namespace oyi::transform
