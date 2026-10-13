#pragma once

#include <cstddef>
#include <string_view>

/// Profiler hooks (ADR-0018: Tracy; ADR-0009: job names are the markers). With OYI_PROFILE the
/// macros become Tracy zones, frame marks and thread names; without it they vanish. Use them as
/// statements at the start of a block.
#ifndef OYI_PROFILE
#define OYI_PROFILE 0
#endif

#if OYI_PROFILE
#include <tracy/Tracy.hpp>
#endif

namespace oyi::core {

[[nodiscard]] constexpr bool IsProfilingEnabled() noexcept {
    return OYI_PROFILE != 0;
}

namespace detail {

/// Tracy wants a terminated string; names are string_views.
inline void SetProfilerThreadName([[maybe_unused]] std::string_view name) noexcept {
#if OYI_PROFILE
    char bounded[64]   = {}; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    const size_t count = name.size() < sizeof(bounded) - 1 ? name.size() : sizeof(bounded) - 1;
    for (size_t i = 0; i < count; ++i) {
        bounded[i] = name[i];
    }
    tracy::SetThreadName(bounded);
#endif
}

} // namespace detail
} // namespace oyi::core

#if OYI_PROFILE
/// A zone named by a string literal.
#define OYI_ZONE(name) ZoneScopedN(name)
/// A zone named at run time (a job or system name).
#define OYI_ZONE_TEXT(view)                                                                                            \
    ZoneScoped;                                                                                                        \
    ZoneName((view).data(), (view).size())
#define OYI_FRAME_MARK        FrameMark
#define OYI_THREAD_NAME(view) ::oyi::core::detail::SetProfilerThreadName(view)
#else
#define OYI_ZONE(name)        static_cast<void>(0)
#define OYI_ZONE_TEXT(view)   static_cast<void>(view)
#define OYI_FRAME_MARK        static_cast<void>(0)
#define OYI_THREAD_NAME(view) static_cast<void>(view)
#endif
