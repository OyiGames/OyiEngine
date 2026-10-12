#pragma once

#include <oyi_core/Allocator.h>

#include <cstddef>
#include <span>

namespace oyi::core {

/// Bump allocator over a buffer the caller owns. Free is a no-op; Reset reclaims everything.
/// Single-threaded by contract: one per owner, never shared across jobs.
class LinearAllocator final : public Allocator {
public:
    explicit LinearAllocator(std::span<std::byte> buffer) noexcept;

    /// Returns nullptr once the buffer is exhausted; nothing is allocated from elsewhere.
    [[nodiscard]] void* Allocate(size_t size, size_t alignment) noexcept override;
    void Free(void* ptr, size_t size, size_t alignment) noexcept override;

    /// Makes the whole buffer available again. Pointers handed out earlier become dangling.
    void Reset() noexcept;

    [[nodiscard]] size_t GetUsedBytes() const noexcept;
    [[nodiscard]] size_t GetCapacity() const noexcept;

private:
    std::span<std::byte> mBuffer;
    size_t mOffset = 0;
};

} // namespace oyi::core
