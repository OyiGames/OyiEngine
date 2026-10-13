#pragma once

#include <oyi_core/Allocator.h>

#include <atomic>
#include <cstddef>
#include <string_view>

namespace oyi::core {

/// General-purpose allocator on top of the platform heap. Thread-safe. This is where mimalloc
/// (ADR-0018) plugs in; engine code never calls malloc or new directly.
class SystemAllocator final : public Allocator {
public:
    struct Stats {
        size_t mLiveAllocations = 0;
        size_t mLiveBytes       = 0;
    };

    [[nodiscard]] void* Allocate(size_t size, size_t alignment) noexcept override;
    void Free(void* ptr, size_t size, size_t alignment) noexcept override;

    /// Outstanding allocations made through this instance; both are zero when nothing leaked.
    [[nodiscard]] Stats GetStats() const noexcept;

    /// "mimalloc" or "crt": the heap this build runs on (ADR-0018).
    [[nodiscard]] static std::string_view GetBackendName() noexcept;

private:
    std::atomic<size_t> mLiveAllocations = 0;
    std::atomic<size_t> mLiveBytes       = 0;
};

/// The process-wide system allocator, for code that needs an Allocator& with static lifetime.
/// Subsystems should still take an Allocator& from their creator so tests can substitute one.
[[nodiscard]] SystemAllocator& GetSystemAllocator() noexcept;

} // namespace oyi::core
