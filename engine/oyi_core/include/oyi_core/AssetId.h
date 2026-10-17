#pragma once

#include <oyi_core/TypeId.h>

#include <cstdint>
#include <string_view>

namespace oyi::core {

/// A reference to a baked asset by stable identity (ADR-0013, ADR-0024): the FNV-1a hash of the
/// asset's canonical path. Components may hold one; `OYI_FIELD(Asset = <Kind>)` says which kind
/// of blob it must resolve to. Zero is "no asset".
struct AssetId {
    uint64_t mValue = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return mValue != 0; }
    [[nodiscard]] constexpr bool operator==(const AssetId&) const noexcept = default;
};

[[nodiscard]] constexpr AssetId MakeAssetId(std::string_view canonicalPath) noexcept {
    return AssetId{.mValue = Fnv1a64(canonicalPath)};
}

} // namespace oyi::core
