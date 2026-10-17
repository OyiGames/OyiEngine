#pragma once

#include <oyi_asset/Blob.h>
#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/World.h>
#include <oyi_object/Prototype.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource> // IWYU pragma: keep (libc++ sees no use, the MSVC STL needs it)
#include <vector>

namespace oyi::object {

/// The prototype asset (ADR-0024, RFC-0001 step 6): a PrototypeDesc tree laid out as a blob.
/// Types are named by TypeId so the file does not depend on a world's registration order; the
/// loader resolves them against the world it loads into. Default values are the component bytes
/// for the schema the blob was baked for (the header's schema hash, checked at Open).
inline constexpr uint64_t kPrototypeAssetKind    = core::MakeTypeId("oyi::object::PrototypeBlob").mValue;
inline constexpr uint32_t kPrototypeBakerVersion = 1;

/// A default value: the component and its bytes.
struct PrototypeValueRecord {
    uint64_t mTypeId = 0;
    asset::RelativeSpan<std::byte> mBytes;
};

/// A managed binding: the state block's TypeId names the managed type (ManagedTypeRegistry), the
/// reference component holds the ManagedId at `mOffset`.
struct PrototypeManagedRecord {
    uint64_t mStateTypeId     = 0;
    uint64_t mReferenceTypeId = 0;
    uint32_t mOffset          = 0;
    uint32_t mPad             = 0;
};

/// One prototype of the tree; children are indices into PrototypeBlob::mNodes.
struct PrototypeNode {
    asset::RelativeSpan<char> mName;
    asset::RelativeSpan<uint64_t> mComponents; ///< component TypeIds (ObjectInfo is implied)
    asset::RelativeSpan<PrototypeValueRecord> mDefaults;
    asset::RelativeSpan<PrototypeManagedRecord> mManaged;
    asset::RelativeSpan<uint32_t> mChildren;
};

/// The root object: node 0 is the root prototype.
struct PrototypeBlob {
    asset::RelativeSpan<PrototypeNode> mNodes;
};

/// Lays `desc` and its children out as a prototype blob for `world`'s schema and appends header
/// and payload to `out`. Two writes of the same input are byte for byte the same. Fails with
/// InvalidArgument for a default or binding the world's registry cannot describe.
[[nodiscard]] core::Result WritePrototypeBlob(const ecs::World& world, const PrototypeDesc& desc,
                                              core::Allocator& allocator, std::pmr::vector<std::byte>& out) noexcept;

/// What BlobView::Open should expect of a prototype blob for `world`.
[[nodiscard]] asset::BlobExpectation PrototypeBlobExpectation(const ecs::World& world) noexcept;

} // namespace oyi::object
