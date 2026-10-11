#pragma once

#include <source_location>
#include <string_view>

namespace oyi::core {

/// Single exit point for failed assertions: logs the failure (with file:line) and terminates.
/// Crash reporting (Crashpad, ADR-0018) hooks in here.
[[noreturn]] void ReportAssertionFailure(std::string_view expression, std::string_view message,
                                         std::source_location location = std::source_location::current());

} // namespace oyi::core

/// OYI_CHECK: kept in every build configuration; a failure is logged and the process terminates.
#define OYI_CHECK(expr, msg)                                                                                           \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            ::oyi::core::ReportAssertionFailure(#expr, msg);                                                           \
        }                                                                                                              \
    } while (false)

/// OYI_ASSERT: Debug builds only; the expression is not evaluated in Release builds.
#ifdef NDEBUG
#define OYI_ASSERT(expr, msg)                                                                                          \
    do {                                                                                                               \
    } while (false)
#else
#define OYI_ASSERT(expr, msg) OYI_CHECK(expr, msg)
#endif
