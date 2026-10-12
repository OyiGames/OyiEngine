#pragma once

#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace oyi::core {

/// Alignment that keeps two objects on different cache lines on every target we build for.
/// `std::hardware_destructive_interference_size` is not used: compilers disagree on its value.
inline constexpr size_t kCacheLineSize = 64;

/// Rounds `value` up to the next multiple of `alignment`, which must be a power of two.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T AlignUp(T value, T alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

/// True when `value` is a multiple of `alignment`, which must be a power of two.
template <std::unsigned_integral T>
[[nodiscard]] constexpr bool IsAligned(T value, T alignment) noexcept {
    return (value & (alignment - 1)) == 0;
}

/// Advances `ptr` to the next address that is a multiple of `alignment` (a power of two).
[[nodiscard]] inline std::byte* AlignUp(std::byte* ptr, size_t alignment) noexcept {
    const auto address = reinterpret_cast<uintptr_t>(ptr);
    return ptr + (AlignUp(address, static_cast<uintptr_t>(alignment)) - address);
}

[[nodiscard]] inline bool IsAligned(const void* ptr, size_t alignment) noexcept {
    return IsAligned(reinterpret_cast<uintptr_t>(ptr), static_cast<uintptr_t>(alignment));
}

/// Alignment values accepted by every allocator: a power of two.
[[nodiscard]] constexpr bool IsValidAlignment(size_t alignment) noexcept {
    return std::has_single_bit(alignment);
}

/// Names a batch of frame memory (ADR-0009). Everything allocated under one tag is released at once
/// with TaggedHeap::FreeTag; there is no per-pointer free. Tags are opaque to the heap: a frame
/// index, a render pass, a streaming batch.
struct MemoryTag {
    uint32_t mValue = 0;

    [[nodiscard]] static constexpr MemoryTag ForFrame(uint64_t frameIndex) noexcept {
        return MemoryTag{.mValue = static_cast<uint32_t>(frameIndex & 0xFFFF'FFFFu)};
    }

    [[nodiscard]] constexpr auto operator<=>(const MemoryTag&) const noexcept = default;
};

} // namespace oyi::core
