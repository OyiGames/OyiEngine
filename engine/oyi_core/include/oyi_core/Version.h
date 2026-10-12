#pragma once

#include <cstdint>
#include <string>
#include <string_view>

static_assert(__cplusplus >= 202302L, "OyiEngine requires C++23. Set the standard in the preset or toolchain file.");

namespace oyi::core {

/// Engine SDK semantic version. Four version numbers exist in the project and each has one job
/// (ADR-0020): this is the engine SDK version only. Protocol version, schema hash and data-format
/// versions are owned by their respective modules.
struct Version {
    uint32_t mMajor = 0;
    uint32_t mMinor = 0;
    uint32_t mPatch = 0;

    [[nodiscard]] constexpr auto operator<=>(const Version&) const = default;
};

/// Returns the version of this engine build.
[[nodiscard]] Version GetEngineVersion() noexcept;

/// Formats a version as "major.minor.patch".
[[nodiscard]] std::string ToString(const Version& version);

/// JSON line used by `oyi --version --json`: {"engine":"0.1.0","protocol":0,"schemaHash":"0x..."}.
/// `schemaHash` is the build's component schema (the tool combines the modules'), "0" when
/// unknown; `protocolVersion` is the wire protocol of oyi_rpc, 0 when the build has none.
[[nodiscard]] std::string GetVersionJson(uint64_t schemaHash = 0, uint32_t protocolVersion = 0);

/// Compile-time engine name for static assertions and compatibility checks.
inline constexpr std::string_view kEngineName = "OyiEngine";

} // namespace oyi::core
