#pragma once

#include <cstdint>

namespace oyi::platform {

/// What the machine offers, measured once at startup (ADR-0009: workers are counted from the
/// big cores; the device profile, not this layer, turns measurements into budgets).
struct CoreTopology {
    uint32_t mLogicalCores     = 1; ///< hardware threads
    uint32_t mPhysicalCores    = 1;
    uint32_t mPerformanceCores = 1; ///< the big cores; equals mPhysicalCores on a uniform machine
    uint32_t mEfficiencyCores  = 0; ///< the little cores; zero on a uniform machine
    bool mHybrid               = false;
};

/// Asks the OS (Windows: processor relationships and efficiency classes; Apple: sysctl
/// performance levels). Falls back to the hardware thread count when a query fails.
[[nodiscard]] CoreTopology QueryCoreTopology() noexcept;

/// ADR-0009: one big core for the main thread, the other big cores plus half of the little ones
/// for workers; never fewer than one.
[[nodiscard]] uint32_t RecommendedWorkerCount(const CoreTopology& topology) noexcept;

} // namespace oyi::platform
