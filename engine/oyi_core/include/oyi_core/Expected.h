#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace oyi::core {

/// Error categories shared by every module (ADR-0002). Callers branch on the code; the message is
/// for logs and tools and must be a string literal or other storage that outlives the error.
enum class ErrorCode : uint8_t {
    None = 0,
    InvalidArgument,
    OutOfMemory,
    OutOfRange,
    NotFound,
    AlreadyExists,
    InvalidState,
    Unsupported,
    IoFailure,
    CorruptData,
    Timeout,
    Cancelled,
};

/// Name of an error code for logs, e.g. "OutOfMemory".
[[nodiscard]] constexpr std::string_view ToString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None:
        return "None";
    case ErrorCode::InvalidArgument:
        return "InvalidArgument";
    case ErrorCode::OutOfMemory:
        return "OutOfMemory";
    case ErrorCode::OutOfRange:
        return "OutOfRange";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::AlreadyExists:
        return "AlreadyExists";
    case ErrorCode::InvalidState:
        return "InvalidState";
    case ErrorCode::Unsupported:
        return "Unsupported";
    case ErrorCode::IoFailure:
        return "IoFailure";
    case ErrorCode::CorruptData:
        return "CorruptData";
    case ErrorCode::Timeout:
        return "Timeout";
    case ErrorCode::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

/// The error half of Expected: a code plus a static message. Trivially copyable, no heap, so it is
/// cheap to return through several layers; attach context by logging at the site that has it.
struct Error {
    ErrorCode mCode = ErrorCode::None;
    std::string_view mMessage;

    [[nodiscard]] constexpr bool operator==(const Error&) const noexcept = default;
};

/// Return type of every fallible function in the engine (the core builds without exceptions).
/// Never call `.value()`; test with `has_value()` and read through `*` or `error()`.
template <typename T>
using Expected = std::expected<T, Error>;

/// Expected without a value, for functions that only succeed or fail.
using Result = std::expected<void, Error>;

/// Builds the failure branch: `return Fail(ErrorCode::NotFound, "no such entity");`.
[[nodiscard]] constexpr std::unexpected<Error> Fail(ErrorCode code, std::string_view message) noexcept {
    return std::unexpected<Error>{Error{.mCode = code, .mMessage = message}};
}

} // namespace oyi::core
