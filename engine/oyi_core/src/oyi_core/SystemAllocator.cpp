#include <oyi_core/Memory.h>
#include <oyi_core/SystemAllocator.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <string_view>

#ifndef OYI_USE_MIMALLOC
#define OYI_USE_MIMALLOC 0
#endif

#if OYI_USE_MIMALLOC
#include <mimalloc.h>
#elifdef _WIN32
#include <malloc.h> // IWYU pragma: keep
#endif

namespace oyi::core {

namespace {

// The one place in the engine that talks to the heap; everything else goes through an Allocator.
// mimalloc (ADR-0018) is the default heap; the C runtime stays for sanitizer builds and opt-outs.
void* PlatformAlignedAlloc(size_t size, size_t alignment) noexcept {
#if OYI_USE_MIMALLOC
    return mi_malloc_aligned(size, alignment); // NOLINT(misc-include-cleaner)
#elifdef _WIN32
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory, misc-include-cleaner)
    return _aligned_malloc(size, alignment);
#else
    // aligned_alloc requires an alignment of at least sizeof(void*) and a size that is a multiple
    // of it; the caller's size is still what gets tracked.
    const size_t effectiveAlignment = alignment < sizeof(void*) ? sizeof(void*) : alignment;
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
    return std::aligned_alloc(effectiveAlignment, AlignUp(size, effectiveAlignment));
#endif
}

void PlatformAlignedFree(void* ptr) noexcept {
#if OYI_USE_MIMALLOC
    mi_free(ptr); // NOLINT(misc-include-cleaner)
#elifdef _WIN32
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory, misc-include-cleaner)
    _aligned_free(ptr);
#else
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
    std::free(ptr);
#endif
}

} // namespace

void* SystemAllocator::Allocate(size_t size, size_t alignment) noexcept {
    if (size == 0 || !IsValidAlignment(alignment)) {
        return nullptr;
    }
    void* memory = PlatformAlignedAlloc(size, alignment);
    if (memory != nullptr) {
        mLiveAllocations.fetch_add(1, std::memory_order_relaxed);
        mLiveBytes.fetch_add(size, std::memory_order_relaxed);
    }
    return memory;
}

void SystemAllocator::Free(void* ptr, size_t size, size_t /*alignment*/) noexcept {
    if (ptr == nullptr) {
        return;
    }
    PlatformAlignedFree(ptr);
    mLiveAllocations.fetch_sub(1, std::memory_order_relaxed);
    mLiveBytes.fetch_sub(size, std::memory_order_relaxed);
}

SystemAllocator::Stats SystemAllocator::GetStats() const noexcept {
    return Stats{.mLiveAllocations = mLiveAllocations.load(std::memory_order_relaxed),
                 .mLiveBytes       = mLiveBytes.load(std::memory_order_relaxed)};
}

std::string_view SystemAllocator::GetBackendName() noexcept {
#if OYI_USE_MIMALLOC
    return "mimalloc";
#else
    return "crt";
#endif
}

SystemAllocator& GetSystemAllocator() noexcept {
    static SystemAllocator sInstance;
    return sInstance;
}

} // namespace oyi::core
