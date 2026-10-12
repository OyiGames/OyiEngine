#include <oyi_core/LinearAllocator.h>
#include <oyi_core/Memory.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace oyi::core {

LinearAllocator::LinearAllocator(std::span<std::byte> buffer) noexcept : mBuffer(buffer) {}

void* LinearAllocator::Allocate(size_t size, size_t alignment) noexcept {
    if (size == 0 || !IsValidAlignment(alignment)) {
        return nullptr;
    }
    const auto address     = reinterpret_cast<uintptr_t>(mBuffer.data() + mOffset);
    const size_t padding   = AlignUp(address, static_cast<uintptr_t>(alignment)) - address;
    const size_t remaining = mBuffer.size() - mOffset;
    if (padding > remaining || size > remaining - padding) {
        return nullptr;
    }
    mOffset += padding;
    void* const result = mBuffer.data() + mOffset;
    mOffset += size;
    return result;
}

void LinearAllocator::Free(void* /*ptr*/, size_t /*size*/, size_t /*alignment*/) noexcept {}

void LinearAllocator::Reset() noexcept {
    mOffset = 0;
}

size_t LinearAllocator::GetUsedBytes() const noexcept {
    return mOffset;
}

size_t LinearAllocator::GetCapacity() const noexcept {
    return mBuffer.size();
}

} // namespace oyi::core
