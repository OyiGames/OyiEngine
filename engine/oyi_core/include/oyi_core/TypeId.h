#pragma once

#include <compare>
#include <cstdint>
#include <string_view>

namespace oyi::core {

/// 64-bit FNV-1a over the bytes of `text`. Shared with tools/codegen, which computes the same
/// value in Python; generated headers static_assert that both agree.
[[nodiscard]] constexpr uint64_t Fnv1a64(std::string_view text) noexcept {
    uint64_t hash = 0xCBF2'9CE4'8422'2325ull;
    for (const char c : text) {
        hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
        hash *= 0x0000'0100'0000'01B3ull;
    }
    return hash;
}

/// Stable identifier of a reflected type: the FNV-1a hash of its fully qualified name
/// (ADR-0011). Zero is reserved for "no type".
struct TypeId {
    uint64_t mValue = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return mValue != 0; }

    [[nodiscard]] constexpr auto operator<=>(const TypeId&) const noexcept = default;
};

[[nodiscard]] constexpr TypeId MakeTypeId(std::string_view qualifiedName) noexcept {
    return TypeId{.mValue = Fnv1a64(qualifiedName)};
}

} // namespace oyi::core
