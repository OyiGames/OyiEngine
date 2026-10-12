#include <oyi_platform/Topology.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <thread>

#ifdef _WIN32
// NOLINTBEGIN(misc-include-cleaner)
#include <windows.h>
// NOLINTEND(misc-include-cleaner)
#include <array>
#else
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#endif

namespace oyi::platform {

namespace {

[[nodiscard]] uint32_t HardwareThreads() noexcept {
    const unsigned int count = std::thread::hardware_concurrency();
    return count == 0 ? 1u : static_cast<uint32_t>(count);
}

#ifdef _WIN32

// NOLINTBEGIN(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast)
[[nodiscard]] bool QueryWindows(CoreTopology& topology) noexcept {
    DWORD size = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size) != FALSE || size == 0) {
        return false;
    }
    // Enough for a few hundred cores; the platform layer sits below the allocators and does not
    // touch the heap.
    alignas(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) std::array<std::byte, 32768> buffer{};
    if (size > buffer.size()) {
        return false;
    }
    auto* const info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &size) == FALSE) {
        return false;
    }
    uint32_t physical  = 0;
    uint32_t bestClass = 0;
    uint32_t bestCount = 0;
    for (DWORD offset = 0; offset < size;) {
        const auto* const entry =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore) {
            ++physical;
            // The efficiency class grows with performance; uniform machines report 0 everywhere.
            const uint32_t efficiencyClass = entry->Processor.EfficiencyClass;
            if (efficiencyClass > bestClass) {
                bestClass = efficiencyClass;
                bestCount = 1;
            } else if (efficiencyClass == bestClass) {
                ++bestCount;
            }
        }
        offset += entry->Size;
    }
    if (physical == 0) {
        return false;
    }
    topology.mPhysicalCores    = physical;
    topology.mHybrid           = bestClass > 0 && bestCount < physical;
    topology.mPerformanceCores = topology.mHybrid ? bestCount : physical;
    topology.mEfficiencyCores  = physical - topology.mPerformanceCores;
    return true;
}
// NOLINTEND(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast)

#else
#ifdef __APPLE__

[[nodiscard]] uint32_t Sysctl(const char* name) noexcept {
    int32_t value = 0;
    size_t length = sizeof(value);
    if (sysctlbyname(name, &value, &length, nullptr, 0) != 0 || value <= 0) {
        return 0;
    }
    return static_cast<uint32_t>(value);
}

[[nodiscard]] bool QueryApple(CoreTopology& topology) noexcept {
    const uint32_t physical = Sysctl("hw.physicalcpu");
    if (physical == 0) {
        return false;
    }
    const uint32_t logical = Sysctl("hw.logicalcpu");
    if (logical != 0) {
        topology.mLogicalCores = logical;
    }
    topology.mPhysicalCores = physical;
    // perflevel0 is the highest performance level (the P cores on Apple silicon).
    const uint32_t performance = Sysctl("hw.perflevel0.physicalcpu");
    const uint32_t efficiency  = Sysctl("hw.perflevel1.physicalcpu");
    topology.mHybrid           = performance > 0 && efficiency > 0;
    topology.mPerformanceCores = topology.mHybrid ? performance : physical;
    topology.mEfficiencyCores  = topology.mHybrid ? std::min(efficiency, physical - performance) : 0;
    return true;
}

#endif
#endif

} // namespace

CoreTopology QueryCoreTopology() noexcept {
    CoreTopology topology;
    topology.mLogicalCores     = HardwareThreads();
    topology.mPhysicalCores    = topology.mLogicalCores;
    topology.mPerformanceCores = topology.mLogicalCores;
#ifdef _WIN32
    if (!QueryWindows(topology)) {
        topology.mPhysicalCores    = topology.mLogicalCores;
        topology.mPerformanceCores = topology.mLogicalCores;
    }
#else
#ifdef __APPLE__
    if (!QueryApple(topology)) {
        topology.mPhysicalCores    = topology.mLogicalCores;
        topology.mPerformanceCores = topology.mLogicalCores;
    }
#endif
#endif
    topology.mLogicalCores = std::max(topology.mLogicalCores, topology.mPhysicalCores);
    return topology;
}

uint32_t RecommendedWorkerCount(const CoreTopology& topology) noexcept {
    const uint32_t big    = std::max(topology.mPerformanceCores, 1u);
    const uint32_t little = topology.mHybrid ? topology.mEfficiencyCores / 2 : 0;
    const uint32_t count  = (big - 1) + little;
    return std::max(count, 1u);
}

} // namespace oyi::platform
