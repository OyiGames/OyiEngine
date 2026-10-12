#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>

namespace oyi::ecs {

/// Upper bound on registered component types per World; masks are sized for it.
inline constexpr uint32_t kMaxComponentTypes = 256;

/// Dense index of a component type inside one World's ComponentRegistry. Systems cache these at
/// registration; TypeId lookups are for tools and loading, not the frame loop.
struct ComponentTypeIndex {
    uint16_t mValue = 0xFFFFu;

    [[nodiscard]] static constexpr ComponentTypeIndex Invalid() noexcept { return ComponentTypeIndex{}; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return mValue < kMaxComponentTypes; }

    [[nodiscard]] constexpr auto operator<=>(const ComponentTypeIndex&) const noexcept = default;
};

/// Set of component types: an archetype's signature, a query's requirement, a system's access set.
class ComponentMask {
public:
    static constexpr size_t kWordCount = kMaxComponentTypes / 64;

    constexpr ComponentMask() = default;

    constexpr void Set(ComponentTypeIndex type) noexcept { mWords[WordOf(type)] |= BitOf(type); }

    constexpr void Clear(ComponentTypeIndex type) noexcept { mWords[WordOf(type)] &= ~BitOf(type); }

    [[nodiscard]] constexpr bool Test(ComponentTypeIndex type) const noexcept {
        return (mWords[WordOf(type)] & BitOf(type)) != 0;
    }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept {
        return std::ranges::all_of(mWords, [](uint64_t word) { return word == 0; });
    }

    [[nodiscard]] constexpr uint32_t Count() const noexcept {
        uint32_t count = 0;
        for (const uint64_t word : mWords) {
            count += static_cast<uint32_t>(std::popcount(word));
        }
        return count;
    }

    /// True when every type in `other` is also in this mask.
    [[nodiscard]] constexpr bool Contains(const ComponentMask& other) const noexcept {
        for (size_t i = 0; i < kWordCount; ++i) {
            if ((mWords[i] & other.mWords[i]) != other.mWords[i]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr bool Intersects(const ComponentMask& other) const noexcept {
        for (size_t i = 0; i < kWordCount; ++i) {
            if ((mWords[i] & other.mWords[i]) != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr ComponentMask With(ComponentTypeIndex type) const noexcept {
        ComponentMask copy = *this;
        copy.Set(type);
        return copy;
    }

    [[nodiscard]] constexpr ComponentMask Without(ComponentTypeIndex type) const noexcept {
        ComponentMask copy = *this;
        copy.Clear(type);
        return copy;
    }

    /// Calls `fn(ComponentTypeIndex)` for every set type in ascending order.
    template <typename Fn>
    constexpr void ForEach(Fn&& fn) const {
        for (size_t wordIndex = 0; wordIndex < kWordCount; ++wordIndex) {
            uint64_t word = mWords[wordIndex];
            while (word != 0) {
                const auto bit = static_cast<uint32_t>(std::countr_zero(word));
                fn(ComponentTypeIndex{.mValue = static_cast<uint16_t>((wordIndex * 64) + bit)});
                word &= word - 1;
            }
        }
    }

    [[nodiscard]] constexpr bool operator==(const ComponentMask&) const noexcept = default;

private:
    // uint16_t promotes to int; keep the bit arithmetic unsigned throughout.
    [[nodiscard]] static constexpr size_t WordOf(ComponentTypeIndex type) noexcept {
        return static_cast<size_t>(type.mValue) / 64u;
    }
    [[nodiscard]] static constexpr uint64_t BitOf(ComponentTypeIndex type) noexcept {
        return uint64_t{1} << (static_cast<uint32_t>(type.mValue) % 64u);
    }

    std::array<uint64_t, kWordCount> mWords{};
};

} // namespace oyi::ecs
