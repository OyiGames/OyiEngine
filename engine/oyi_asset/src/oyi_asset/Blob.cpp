#include <oyi_asset/Blob.h>
#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Memory.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::asset {

namespace {

constexpr uint64_t kFnvOffset = 0xCBF2'9CE4'8422'2325ull;
constexpr uint64_t kFnvPrime  = 0x0000'0100'0000'01B3ull;

} // namespace

uint64_t HashBytes(std::span<const std::byte> bytes, uint64_t seed) noexcept {
    uint64_t hash = seed;
    for (const std::byte b : bytes) {
        hash ^= static_cast<uint64_t>(static_cast<uint8_t>(b));
        hash *= kFnvPrime;
    }
    return hash;
}

BlobWriter::BlobWriter(core::Allocator& allocator) noexcept : mResource(allocator), mPayload(&mResource) {}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
BlobWriter::Offset BlobWriter::Allocate(size_t size, size_t alignment) noexcept {
    OYI_ASSERT(core::IsValidAlignment(alignment), "BlobWriter: alignment is not a power of two");
    OYI_ASSERT(alignment <= kBlobAlignment, "BlobWriter: alignment above the blob alignment");
    const size_t start = core::AlignUp(mPayload.size(), alignment);
    mPayload.resize(start + size, std::byte{0});
    return static_cast<Offset>(start);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void BlobWriter::Finish(uint64_t kind, uint32_t version, uint64_t schemaHash,
                        std::pmr::vector<std::byte>& out) const noexcept {
    const size_t paddedSize = core::AlignUp(mPayload.size(), kBlobAlignment);
    const size_t start      = out.size();
    out.resize(start + sizeof(BlobHeader) + paddedSize, std::byte{0});
    std::memcpy(out.data() + start + sizeof(BlobHeader), mPayload.data(), mPayload.size());
    const std::span<const std::byte> payload(out.data() + start + sizeof(BlobHeader), paddedSize);
    BlobHeader header;
    header.mSchemaHash  = schemaHash;
    header.mKind        = kind;
    header.mVersion     = version;
    header.mRootOffset  = mRoot;
    header.mPayloadSize = paddedSize;
    header.mContentHash = HashBytes(payload, kFnvOffset);
    std::memcpy(out.data() + start, &header, sizeof(header));
}

core::Expected<BlobView> BlobView::Open(std::span<const std::byte> data, BlobExpectation expectation) noexcept {
    if (data.size() < sizeof(BlobHeader)) {
        return core::Fail(core::ErrorCode::CorruptData, "Blob: shorter than a header");
    }
    BlobHeader header;
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.mMagic != kBlobMagic) {
        return core::Fail(core::ErrorCode::CorruptData, "Blob: not a blob");
    }
    if (header.mFormatVersion != kBlobFormatVersion) {
        return core::Fail(core::ErrorCode::CorruptData, "Blob: unsupported format version");
    }
    if (header.mPayloadSize != data.size() - sizeof(BlobHeader) || header.mPayloadSize % kBlobAlignment != 0) {
        return core::Fail(core::ErrorCode::CorruptData, "Blob: payload size does not match the data");
    }
    if (header.mRootOffset >= header.mPayloadSize && header.mPayloadSize != 0) {
        return core::Fail(core::ErrorCode::CorruptData, "Blob: root outside the payload");
    }
    const std::span<const std::byte> payload = data.subspan(sizeof(BlobHeader));
    if (HashBytes(payload, kFnvOffset) != header.mContentHash) {
        return core::Fail(core::ErrorCode::CorruptData, "Blob: content hash mismatch");
    }
    if (expectation.mKind != 0 && expectation.mKind != header.mKind) {
        return core::Fail(core::ErrorCode::InvalidArgument, "Blob: another asset kind");
    }
    if (expectation.mVersion != 0 && expectation.mVersion != header.mVersion) {
        return core::Fail(core::ErrorCode::InvalidArgument, "Blob: baked by another baker version");
    }
    if (expectation.mSchemaHash != 0 && expectation.mSchemaHash != header.mSchemaHash) {
        return core::Fail(core::ErrorCode::InvalidArgument, "Blob: baked for another schema");
    }
    const auto* const stored =
        reinterpret_cast<const BlobHeader*>(data.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    return BlobView(stored, payload);
}

} // namespace oyi::asset
