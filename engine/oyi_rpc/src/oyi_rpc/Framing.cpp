#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_rpc/Rpc.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

namespace oyi::rpc {

namespace {

[[nodiscard]] uint32_t ReadLength(const std::byte* at) noexcept {
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8u) | (static_cast<uint32_t>(at[2]) << 16u) |
           (static_cast<uint32_t>(at[3]) << 24u);
}

} // namespace

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
void AppendFrame(std::pmr::vector<std::byte>& out, std::span<const std::byte> payload) noexcept {
    const auto length = static_cast<uint32_t>(payload.size());
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((length >> shift) & 0xFFu));
    }
    out.insert(out.end(), payload.begin(), payload.end());
}

FrameReader::FrameReader(core::Allocator& allocator) noexcept : mResource(allocator), mBuffer(&mResource) {}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result FrameReader::Push(std::span<const std::byte> bytes) noexcept {
    mBuffer.insert(mBuffer.end(), bytes.begin(), bytes.end());
    if (mBuffer.size() >= 4 && ReadLength(mBuffer.data()) > kMaxFrameSize) {
        mBuffer.clear();
        return core::Fail(core::ErrorCode::CorruptData, "FrameReader: frame larger than the protocol allows");
    }
    return {};
}

bool FrameReader::HasFrame() const noexcept {
    return mBuffer.size() >= 4 && mBuffer.size() - 4 >= ReadLength(mBuffer.data());
}

std::span<const std::byte> FrameReader::GetFrame() const noexcept {
    if (!HasFrame()) {
        return {};
    }
    return {mBuffer.data() + 4, ReadLength(mBuffer.data())};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void FrameReader::PopFrame() noexcept {
    if (!HasFrame()) {
        return;
    }
    const size_t consumed = 4 + ReadLength(mBuffer.data());
    mBuffer.erase(mBuffer.begin(), mBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
}

void FrameReader::Clear() noexcept {
    mBuffer.clear();
}

Reply::Reply(core::Allocator& allocator) noexcept
    : mResource(allocator), mErrorCode(&mResource), mMessage(&mResource), mPayload(&mResource) {}

// NOLINTNEXTLINE(bugprone-exception-escape)
void Reply::Fail(std::string_view code, std::string_view message) noexcept {
    mOk = false;
    mErrorCode.assign(code);
    mMessage.assign(message);
    mPayload.clear();
}

StreamFrame::StreamFrame(core::Allocator& allocator) noexcept
    : mResource(allocator), mStream(&mResource), mPayload(&mResource) {}

} // namespace oyi::rpc
