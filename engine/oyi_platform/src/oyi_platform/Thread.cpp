#include <oyi_platform/Thread.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#ifdef _WIN32
// NOLINTBEGIN(misc-include-cleaner)
#include <windows.h>
// NOLINTEND(misc-include-cleaner)
#else
#include <pthread.h>
#endif

namespace oyi::platform {

#ifdef _WIN32

// NOLINTBEGIN(misc-include-cleaner,bugprone-suspicious-stringview-data-usage)
void SetCurrentThreadName(std::string_view name) noexcept {
    std::array<wchar_t, 128> wide{};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(std::min<size_t>(name.size(), 127)), wide.data(),
                            static_cast<int>(wide.size() - 1));
    wide[static_cast<size_t>(std::max(length, 0))] = L'\0';
    (void)SetThreadDescription(GetCurrentThread(), wide.data());
}

uint32_t GetCurrentThreadName(std::span<char> buffer) noexcept {
    PWSTR description = nullptr;
    if (FAILED(GetThreadDescription(GetCurrentThread(), &description)) || description == nullptr) {
        return 0;
    }
    const int written = WideCharToMultiByte(CP_UTF8, 0, description, -1, buffer.data(), static_cast<int>(buffer.size()),
                                            nullptr, nullptr);
    LocalFree(description);
    // written counts the terminator; a buffer that is too small yields 0.
    return written > 0 ? static_cast<uint32_t>(written - 1) : 0;
}

uint64_t CurrentThreadId() noexcept {
    return static_cast<uint64_t>(::GetCurrentThreadId());
}
// NOLINTEND(misc-include-cleaner,bugprone-suspicious-stringview-data-usage)

#else

void SetCurrentThreadName(std::string_view name) noexcept {
    std::array<char, 64> bounded{}; // Apple allows 63 bytes plus the terminator
    const size_t count = std::min(name.size(), bounded.size() - 1);
    std::memcpy(bounded.data(), name.data(), count);
    bounded[count] = '\0';
#ifdef __APPLE__
    (void)pthread_setname_np(bounded.data());
#else
    (void)pthread_setname_np(pthread_self(), bounded.data());
#endif
}

uint32_t GetCurrentThreadName(std::span<char> buffer) noexcept {
    std::array<char, 64> bounded{};
    if (pthread_getname_np(pthread_self(), bounded.data(), bounded.size()) != 0) {
        return 0;
    }
    const size_t length = std::min(std::strlen(bounded.data()), buffer.size());
    std::memcpy(buffer.data(), bounded.data(), length);
    return static_cast<uint32_t>(length);
}

uint64_t CurrentThreadId() noexcept {
#ifdef __APPLE__
    uint64_t id = 0;
    (void)pthread_threadid_np(nullptr, &id);
    return id;
#else
    return static_cast<uint64_t>(pthread_self());
#endif
}

#endif

} // namespace oyi::platform
