#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::asset {

/// Runtime asset blobs (ADR-0013): a small header, then a payload addressed only through
/// relative offsets, so a blob is used where it was loaded without fixing anything up. Built
/// offline by a baker, deterministic by construction (no addresses, no clocks), keyed on the
/// schema hash and the baker's version so a stale file is caught before it is trusted.

inline constexpr std::array<char, 4> kBlobMagic{'O', 'Y', 'I', 'B'};
/// 0 while the engine is 0.x: no compatibility promise for baked files; 1 with the first release.
inline constexpr uint32_t kBlobFormatVersion = 0;
/// The payload starts at this offset and every blob should be loaded with this alignment.
inline constexpr size_t kBlobAlignment = 64;

struct BlobHeader {
    std::array<char, 4> mMagic   = kBlobMagic;
    uint32_t mFormatVersion      = kBlobFormatVersion;
    uint64_t mSchemaHash         = 0; ///< the component schema the payload was laid out for
    uint64_t mKind               = 0; ///< TypeId of the asset kind
    uint32_t mVersion            = 0; ///< version of the baker that wrote it
    uint32_t mRootOffset         = 0; ///< offset of the root object inside the payload
    uint64_t mPayloadSize        = 0;
    uint64_t mContentHash        = 0; ///< FNV-1a of the payload
    std::array<uint8_t, 16> mPad = {};
};
static_assert(sizeof(BlobHeader) == kBlobAlignment, "the header is exactly one aligned block");

/// A pointer stored as a self-relative offset; zero is null.
template <typename T>
struct RelativePtr {
    int32_t mOffset = 0;

    [[nodiscard]] bool IsNull() const noexcept { return mOffset == 0; }

    [[nodiscard]] const T* Get() const noexcept {
        if (mOffset == 0) {
            return nullptr;
        }
        const auto* const base =
            reinterpret_cast<const std::byte*>(this);      // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        return reinterpret_cast<const T*>(base + mOffset); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }

    [[nodiscard]] T* Get() noexcept {
        if (mOffset == 0) {
            return nullptr;
        }
        auto* const base = reinterpret_cast<std::byte*>(this); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        return reinterpret_cast<T*>(base + mOffset);           // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }
};

/// A counted array behind a relative pointer.
template <typename T>
struct RelativeSpan {
    RelativePtr<T> mData;
    uint32_t mCount = 0;

    [[nodiscard]] std::span<const T> Get() const noexcept {
        return mData.IsNull() ? std::span<const T>{} : std::span<const T>{mData.Get(), mCount};
    }
};

/// Lays out a payload: objects are allocated by offset (the buffer may move), linked by offset,
/// and written out behind a header. Everything not written stays zero, so unused bytes do not
/// leak into the file and two bakes of the same input are byte for byte the same.
class BlobWriter {
public:
    using Offset = uint32_t;

    explicit BlobWriter(core::Allocator& allocator) noexcept;
    BlobWriter(const BlobWriter&)            = delete;
    BlobWriter(BlobWriter&&)                 = delete;
    BlobWriter& operator=(const BlobWriter&) = delete;
    BlobWriter& operator=(BlobWriter&&)      = delete;
    ~BlobWriter()                            = default;

    /// Reserves `size` zeroed bytes aligned to `alignment` (a power of two up to kBlobAlignment)
    /// and returns their payload offset.
    [[nodiscard]] Offset Allocate(size_t size, size_t alignment) noexcept;

    template <typename T>
    [[nodiscard]] Offset Allocate(uint32_t count = 1) noexcept {
        return Allocate(sizeof(T) * count, alignof(T));
    }

    /// The object at `offset`; the reference lasts until the next Allocate.
    template <typename T>
    [[nodiscard]] T& At(Offset offset) noexcept {
        return *reinterpret_cast<T*>(mPayload.data() + offset); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }

    /// Points the RelativePtr<T> stored at `pointer` to the object at `target`.
    template <typename T>
    void Link(Offset pointer, Offset target) noexcept {
        At<RelativePtr<T>>(pointer).mOffset = static_cast<int32_t>(target) - static_cast<int32_t>(pointer);
    }

    /// Points the RelativeSpan<T> at `span` to `count` objects starting at `target`.
    template <typename T>
    void LinkSpan(Offset span, Offset target, uint32_t count) noexcept {
        At<RelativeSpan<T>>(span).mData.mOffset = static_cast<int32_t>(target) - static_cast<int32_t>(span);
        At<RelativeSpan<T>>(span).mCount        = count;
    }

    void SetRoot(Offset root) noexcept { mRoot = root; }
    [[nodiscard]] size_t GetPayloadSize() const noexcept { return mPayload.size(); }

    /// Appends header and payload to `out`; the payload is padded to kBlobAlignment.
    void Finish(uint64_t kind, uint32_t version, uint64_t schemaHash, std::pmr::vector<std::byte>& out) const noexcept;

private:
    core::MemoryResource mResource;
    std::pmr::vector<std::byte> mPayload;
    Offset mRoot = 0;
};

/// What a loader expects of a blob; zero accepts anything for that field.
struct BlobExpectation {
    uint64_t mKind       = 0;
    uint32_t mVersion    = 0; ///< the baker version
    uint64_t mSchemaHash = 0;
};

/// A loaded blob, checked once: magic, format, size and content hash, and when asked, the kind,
/// the version and the schema hash it was baked for. `data` must stay alive and should be
/// kBlobAlignment-aligned (the allocators give that for free).
class BlobView {
public:
    /// Fails with CorruptData for a damaged or foreign file and InvalidArgument when the
    /// expectation is not met (a stale bake: development builds re-bake, release builds stop).
    [[nodiscard]] static core::Expected<BlobView> Open(std::span<const std::byte> data,
                                                       BlobExpectation expectation = {}) noexcept;

    [[nodiscard]] const BlobHeader& GetHeader() const noexcept { return *mHeader; }
    [[nodiscard]] std::span<const std::byte> GetPayload() const noexcept { return mPayload; }

    template <typename T>
    [[nodiscard]] const T* Root() const noexcept {
        return reinterpret_cast<const T*>(mPayload.data() +
                                          mHeader->mRootOffset); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }

private:
    BlobView(const BlobHeader* header, std::span<const std::byte> payload) noexcept
        : mHeader(header), mPayload(payload) {}

    const BlobHeader* mHeader;
    std::span<const std::byte> mPayload;
};

/// FNV-1a over bytes, the hash every blob and bake uses.
[[nodiscard]] uint64_t HashBytes(std::span<const std::byte> bytes, uint64_t seed) noexcept;

} // namespace oyi::asset
