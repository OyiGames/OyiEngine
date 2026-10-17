#include <oyi_asset/Blob.h>
#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/World.h>
#include <oyi_object/Prototype.h>
#include <oyi_object/PrototypeBlob.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource> // IWYU pragma: keep (libc++ sees no use, the MSVC STL needs it)
#include <vector>

namespace oyi::object {

namespace {

using asset::BlobWriter;
using Offset = BlobWriter::Offset;

/// The tree in pre-order with each node's children, so node indices are known before writing.
struct Flat {
    std::pmr::vector<const PrototypeDesc*> mNodes;
    std::pmr::vector<std::pmr::vector<uint32_t>> mChildren;

    // NOLINTNEXTLINE(misc-no-recursion)
    uint32_t Add(const PrototypeDesc& desc) {
        const auto index = static_cast<uint32_t>(mNodes.size());
        mNodes.push_back(&desc);
        mChildren.emplace_back();
        for (const PrototypeDesc* const child : desc.mChildren) {
            const uint32_t childIndex = Add(*child);
            mChildren[index].push_back(childIndex);
        }
        return index;
    }
};

[[nodiscard]] Offset NodeAt(Offset nodes, uint32_t index) noexcept {
    return nodes + static_cast<Offset>(index * sizeof(PrototypeNode));
}

} // namespace

asset::BlobExpectation PrototypeBlobExpectation(const ecs::World& world) noexcept {
    return asset::BlobExpectation{
        .mKind = kPrototypeAssetKind, .mVersion = kPrototypeBakerVersion, .mSchemaHash = world.ComputeSchemaHash()};
}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
core::Result WritePrototypeBlob(const ecs::World& world, const PrototypeDesc& desc, core::Allocator& allocator,
                                std::pmr::vector<std::byte>& out) noexcept {
    core::MemoryResource resource(allocator);
    Flat flat{.mNodes    = std::pmr::vector<const PrototypeDesc*>(&resource),
              .mChildren = std::pmr::vector<std::pmr::vector<uint32_t>>(&resource)};
    (void)flat.Add(desc);
    const ecs::ComponentRegistry& registry = world.GetComponents();
    for (const PrototypeDesc* const node : flat.mNodes) {
        if (node->mName.empty()) {
            return core::Fail(core::ErrorCode::InvalidArgument, "WritePrototypeBlob: a prototype needs a name");
        }
        for (const ComponentValue& value : node->mDefaults) {
            if (!registry.IsRegistered(value.mType) || value.mData == nullptr ||
                registry.GetInfo(value.mType).mSize != value.mSize || !node->mMask.Test(value.mType)) {
                return core::Fail(core::ErrorCode::InvalidArgument,
                                  "WritePrototypeBlob: a default does not fit a registered component of the mask");
            }
        }
        for (const ManagedBinding& binding : node->mManaged) {
            if (binding.mType == nullptr || !registry.IsRegistered(binding.mReference)) {
                return core::Fail(core::ErrorCode::InvalidArgument,
                                  "WritePrototypeBlob: a managed binding names no registered reference component");
            }
        }
    }

    BlobWriter writer(allocator);
    const Offset root  = writer.Allocate<PrototypeBlob>();
    const auto count   = static_cast<uint32_t>(flat.mNodes.size());
    const Offset nodes = writer.Allocate<PrototypeNode>(count);
    writer.LinkSpan<PrototypeNode>(root + static_cast<Offset>(offsetof(PrototypeBlob, mNodes)), nodes, count);
    std::pmr::vector<uint64_t> typeIds(&resource);
    for (uint32_t i = 0; i < count; ++i) {
        const PrototypeDesc& node = *flat.mNodes[i];
        const Offset at           = NodeAt(nodes, i);

        const Offset name = writer.Allocate(node.mName.size(), 1);
        std::memcpy(&writer.At<char>(name), node.mName.data(), node.mName.size());
        writer.LinkSpan<char>(at + static_cast<Offset>(offsetof(PrototypeNode, mName)), name,
                              static_cast<uint32_t>(node.mName.size()));

        typeIds.clear();
        node.mMask.ForEach(
            [&](ecs::ComponentTypeIndex type) { typeIds.push_back(registry.GetInfo(type).mTypeId.mValue); });
        if (!typeIds.empty()) {
            const Offset ids = writer.Allocate<uint64_t>(static_cast<uint32_t>(typeIds.size()));
            std::memcpy(&writer.At<uint64_t>(ids), typeIds.data(), typeIds.size() * sizeof(uint64_t));
            writer.LinkSpan<uint64_t>(at + static_cast<Offset>(offsetof(PrototypeNode, mComponents)), ids,
                                      static_cast<uint32_t>(typeIds.size()));
        }

        if (!node.mDefaults.empty()) {
            const auto valueCount = static_cast<uint32_t>(node.mDefaults.size());
            const Offset records  = writer.Allocate<PrototypeValueRecord>(valueCount);
            for (uint32_t v = 0; v < valueCount; ++v) {
                const ComponentValue& value = node.mDefaults[v];
                const Offset record         = records + static_cast<Offset>(v * sizeof(PrototypeValueRecord));
                const Offset bytes          = writer.Allocate(value.mSize, 1);
                std::memcpy(&writer.At<std::byte>(bytes), value.mData, value.mSize);
                writer.At<PrototypeValueRecord>(record).mTypeId = registry.GetInfo(value.mType).mTypeId.mValue;
                writer.LinkSpan<std::byte>(record + static_cast<Offset>(offsetof(PrototypeValueRecord, mBytes)), bytes,
                                           value.mSize);
            }
            writer.LinkSpan<PrototypeValueRecord>(at + static_cast<Offset>(offsetof(PrototypeNode, mDefaults)), records,
                                                  valueCount);
        }

        if (!node.mManaged.empty()) {
            const auto managedCount = static_cast<uint32_t>(node.mManaged.size());
            const Offset records    = writer.Allocate<PrototypeManagedRecord>(managedCount);
            for (uint32_t m = 0; m < managedCount; ++m) {
                const ManagedBinding& binding = node.mManaged[m];
                writer.At<PrototypeManagedRecord>(records + static_cast<Offset>(m * sizeof(PrototypeManagedRecord))) =
                    PrototypeManagedRecord{.mStateTypeId     = binding.mType->GetTypeId().mValue,
                                           .mReferenceTypeId = registry.GetInfo(binding.mReference).mTypeId.mValue,
                                           .mOffset          = binding.mOffset,
                                           .mPad             = 0};
            }
            writer.LinkSpan<PrototypeManagedRecord>(at + static_cast<Offset>(offsetof(PrototypeNode, mManaged)),
                                                    records, managedCount);
        }

        const std::pmr::vector<uint32_t>& children = flat.mChildren[i];
        if (!children.empty()) {
            const Offset indices = writer.Allocate<uint32_t>(static_cast<uint32_t>(children.size()));
            std::memcpy(&writer.At<uint32_t>(indices), children.data(), children.size() * sizeof(uint32_t));
            writer.LinkSpan<uint32_t>(at + static_cast<Offset>(offsetof(PrototypeNode, mChildren)), indices,
                                      static_cast<uint32_t>(children.size()));
        }
    }
    writer.SetRoot(root);
    writer.Finish(kPrototypeAssetKind, kPrototypeBakerVersion, world.ComputeSchemaHash(), out);
    return {};
}

} // namespace oyi::object
