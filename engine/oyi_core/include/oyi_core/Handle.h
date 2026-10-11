#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace oyi::core {

/// Strongly typed handle made of a 32-bit index and a 32-bit generation (ADR-0004).
/// `Tag` only distinguishes handle types so that an EntityId can never be assigned to a ChunkId.
/// Generations start at 1; generation 0 is reserved for the invalid handle, so `Handle{}` is
/// always invalid.
template <typename Tag>
struct Handle {
    uint32_t mIndex      = 0;
    uint32_t mGeneration = 0;

    [[nodiscard]] static constexpr Handle Invalid() noexcept { return Handle{}; }

    [[nodiscard]] constexpr bool IsValid() const noexcept { return mGeneration != 0; }

    /// Packs the handle into one u64 (generation in the high 32 bits, index in the low 32 bits)
    /// for hashing or for crossing process boundaries.
    [[nodiscard]] constexpr uint64_t ToU64() const noexcept {
        return (static_cast<uint64_t>(mGeneration) << 32u) | static_cast<uint64_t>(mIndex);
    }

    [[nodiscard]] static constexpr Handle FromU64(uint64_t packed) noexcept {
        return Handle{.mIndex      = static_cast<uint32_t>(packed & 0xFFFF'FFFFu),
                      .mGeneration = static_cast<uint32_t>(packed >> 32u)};
    }

    [[nodiscard]] constexpr auto operator<=>(const Handle&) const noexcept = default;
};

} // namespace oyi::core

template <typename Tag>
struct std::hash<oyi::core::Handle<Tag>> {
    [[nodiscard]] size_t operator()(const oyi::core::Handle<Tag>& handle) const noexcept {
        return std::hash<uint64_t>{}(handle.ToU64());
    }
};
