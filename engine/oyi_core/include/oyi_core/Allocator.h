#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace oyi::core {

/// Interface every allocator in the engine implements. Allocation never throws and never
/// terminates: a request that cannot be served returns nullptr and the caller decides. Containers
/// take an Allocator& (or a MemoryResource built on one) instead of using global new.
class Allocator {
public:
    Allocator()                            = default;
    Allocator(const Allocator&)            = delete;
    Allocator(Allocator&&)                 = delete;
    Allocator& operator=(const Allocator&) = delete;
    Allocator& operator=(Allocator&&)      = delete;
    virtual ~Allocator()                   = default;

    /// Returns `size` bytes aligned to `alignment` (a power of two), or nullptr when the request
    /// cannot be served. A size of zero or an invalid alignment yields nullptr.
    [[nodiscard]] virtual void* Allocate(size_t size, size_t alignment) noexcept = 0;

    /// Releases memory obtained from Allocate with the same size and alignment. nullptr is ignored.
    /// Frame allocators implement this as a no-op: their memory is reclaimed by tag.
    virtual void Free(void* ptr, size_t size, size_t alignment) noexcept = 0;

    /// Allocates and constructs one T; nullptr when out of memory.
    template <typename T, typename... Args>
    [[nodiscard]] T* New(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        void* memory = Allocate(sizeof(T), alignof(T));
        if (memory == nullptr) {
            return nullptr;
        }
        return std::construct_at(static_cast<T*>(memory), std::forward<Args>(args)...);
    }

    /// Destroys an object from New and releases its memory. nullptr is ignored.
    template <typename T>
    void Delete(T* object) noexcept {
        if (object == nullptr) {
            return;
        }
        std::destroy_at(object);
        Free(object, sizeof(T), alignof(T));
    }
};

} // namespace oyi::core
