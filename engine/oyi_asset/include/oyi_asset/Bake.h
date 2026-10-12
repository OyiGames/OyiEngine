#pragma once

#include <oyi_asset/Blob.h>
#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oyi::asset {

/// The baking framework (ADR-0013): authoring data (USD in the editor repository, anything the
/// tools hand over as bytes) becomes a runtime blob through a Baker that only reaches its inputs
/// through the BakeContext, so every dependency is recorded without the baker's help. Bakes are
/// deterministic: the same inputs give the same bytes (IsDeterministic proves it for a baker).

struct BakeInput {
    std::string_view mId; ///< what the asset database calls it
    std::span<const std::byte> mBytes;
};

/// What a baker may read: inputs by id, each read recorded as a dependency.
class BakeContext {
public:
    BakeContext(std::span<const BakeInput> inputs, core::Allocator& allocator) noexcept;
    BakeContext(const BakeContext&)            = delete;
    BakeContext(BakeContext&&)                 = delete;
    BakeContext& operator=(const BakeContext&) = delete;
    BakeContext& operator=(BakeContext&&)      = delete;
    ~BakeContext()                             = default;

    /// Fails with NotFound for an id that was not provided; a found input becomes a dependency.
    [[nodiscard]] core::Expected<std::span<const std::byte>> Read(std::string_view id) noexcept;
    /// Unique ids read so far, in first-read order.
    [[nodiscard]] std::span<const std::pmr::string> GetDependencies() const noexcept { return mDependencies; }
    [[nodiscard]] core::Allocator& GetScratch() const noexcept { return mAllocator; }

private:
    std::span<const BakeInput> mInputs;
    core::Allocator& mAllocator;
    core::MemoryResource mResource;
    std::pmr::vector<std::pmr::string> mDependencies;
};

/// One asset kind's conversion. Stateless by contract: everything comes from the context.
class Baker {
public:
    Baker()                        = default;
    Baker(const Baker&)            = delete;
    Baker(Baker&&)                 = delete;
    Baker& operator=(const Baker&) = delete;
    Baker& operator=(Baker&&)      = delete;
    virtual ~Baker()               = default;

    /// TypeId of the asset kind this baker produces.
    [[nodiscard]] virtual uint64_t GetKind() const noexcept = 0;
    /// Bumped whenever the output changes for the same input; stored in the blob header.
    [[nodiscard]] virtual uint32_t GetVersion() const noexcept                                 = 0;
    [[nodiscard]] virtual core::Result Bake(BakeContext& context, BlobWriter& writer) noexcept = 0;
};

struct BakeResult {
    explicit BakeResult(core::Allocator& allocator) noexcept;

    core::MemoryResource mResource;
    std::pmr::vector<std::byte> mBlob;
    std::pmr::vector<std::pmr::string> mDependencies;
};

/// Runs the baker over `inputs` and finishes the blob with the baker's kind and version and
/// `schemaHash`. The result's dependencies are what the asset database should watch.
[[nodiscard]] core::Result Bake(Baker& baker, std::span<const BakeInput> inputs, uint64_t schemaHash,
                                core::Allocator& allocator, BakeResult& out) noexcept;

/// Bakes twice and compares: the determinism contract of ADR-0013 for a baker under test.
[[nodiscard]] core::Expected<bool> IsDeterministic(Baker& baker, std::span<const BakeInput> inputs, uint64_t schemaHash,
                                                   core::Allocator& allocator) noexcept;

} // namespace oyi::asset
