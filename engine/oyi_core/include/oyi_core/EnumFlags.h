#pragma once

#include <type_traits>

/// Defines bitwise operators and HasFlag() for an `enum class` used as a flag set.
/// Use at namespace scope right after the enum definition. `E` is a type, so it cannot be
/// parenthesized the way bugprone-macro-parentheses wants.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define OYI_ENUM_FLAGS(E)                                                                                              \
    [[nodiscard]] constexpr E operator|(E lhs, E rhs) noexcept {                                                       \
        return static_cast<E>(static_cast<std::underlying_type_t<E>>(lhs) |                                            \
                              static_cast<std::underlying_type_t<E>>(rhs));                                            \
    }                                                                                                                  \
    [[nodiscard]] constexpr E operator&(E lhs, E rhs) noexcept {                                                       \
        return static_cast<E>(static_cast<std::underlying_type_t<E>>(lhs) &                                            \
                              static_cast<std::underlying_type_t<E>>(rhs));                                            \
    }                                                                                                                  \
    [[nodiscard]] constexpr E operator^(E lhs, E rhs) noexcept {                                                       \
        return static_cast<E>(static_cast<std::underlying_type_t<E>>(lhs) ^                                            \
                              static_cast<std::underlying_type_t<E>>(rhs));                                            \
    }                                                                                                                  \
    [[nodiscard]] constexpr E operator~(E value) noexcept {                                                            \
        return static_cast<E>(~static_cast<std::underlying_type_t<E>>(value));                                         \
    }                                                                                                                  \
    constexpr E& operator|=(E& lhs, E rhs) noexcept {                                                                  \
        return lhs = lhs | rhs;                                                                                        \
    }                                                                                                                  \
    constexpr E& operator&=(E& lhs, E rhs) noexcept {                                                                  \
        return lhs = lhs & rhs;                                                                                        \
    }                                                                                                                  \
    constexpr E& operator^=(E& lhs, E rhs) noexcept {                                                                  \
        return lhs = lhs ^ rhs;                                                                                        \
    }                                                                                                                  \
    [[nodiscard]] constexpr bool HasFlag(E value, E flag) noexcept {                                                   \
        return (value & flag) == flag;                                                                                 \
    }                                                                                                                  \
    static_assert(std::is_enum_v<E>, "OYI_ENUM_FLAGS requires an enum type")
// NOLINTEND(bugprone-macro-parentheses)
