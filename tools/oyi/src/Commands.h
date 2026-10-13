#pragma once

// The subcommands of `oyi` (ADR-0016): every one takes its arguments as a span, prints either
// text or one JSON object (--json), and returns an exit code.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oyi::cli {

constexpr int kExitOk       = 0;
constexpr int kExitFailed   = 1; ///< the command ran and reported a failure
constexpr int kExitUsage    = 2;
constexpr int kExitInternal = 3;

/// Common switches every subcommand understands.
struct Options {
    bool mJson  = false;
    bool mQuiet = false;
    std::vector<std::string_view> mPositional;
    /// Every other `--name` or `--name=value`; each command decides which it accepts.
    std::vector<std::pair<std::string_view, std::string_view>> mNamed;
};

/// Splits --json / --quiet and the named switches from the positional arguments.
[[nodiscard]] bool ParseOptions(std::span<const std::string_view> args, Options& out);

/// JSON helpers: a quoted, escaped string and a failure line {"error":{"code":..,"message":..}}.
[[nodiscard]] std::string JsonString(std::string_view text);
void PrintFailure(const Options& options, std::string_view code, std::string_view message);

/// Reads a whole file; empty on failure (with `error` set).
[[nodiscard]] bool ReadFile(std::string_view path, std::vector<std::byte>& out, std::string& error);

int RunVersion(std::span<const std::string_view> args);

/// The schema hash of this build: every engine module that declares components, in dependency
/// order (what `--version --json` prints and what the RPC Welcome carries).
[[nodiscard]] unsigned long long GetBuildSchemaHash() noexcept;
int RunWorld(std::span<const std::string_view> args);
int RunAsset(std::span<const std::string_view> args);
int RunSchema(std::span<const std::string_view> args);
int RunServe(std::span<const std::string_view> args);

} // namespace oyi::cli
