#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace oyi::platform {

/// Names the calling thread for debuggers and profilers. Names longer than the OS allows are
/// truncated (Apple keeps 63 bytes).
void SetCurrentThreadName(std::string_view name) noexcept;

/// Copies the calling thread's name into `buffer` (no terminator added) and returns the length;
/// zero when the OS has no name for it or the query is unsupported.
[[nodiscard]] uint32_t GetCurrentThreadName(std::span<char> buffer) noexcept;

/// The OS identifier of the calling thread: stable for its lifetime, never zero.
[[nodiscard]] uint64_t CurrentThreadId() noexcept;

} // namespace oyi::platform
