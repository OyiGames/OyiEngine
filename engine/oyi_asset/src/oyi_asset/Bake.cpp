#include <oyi_asset/Bake.h>
#include <oyi_asset/Blob.h>
#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace oyi::asset {

BakeContext::BakeContext(std::span<const BakeInput> inputs, core::Allocator& allocator) noexcept
    : mInputs(inputs), mAllocator(allocator), mResource(allocator), mDependencies(&mResource) {}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<std::span<const std::byte>> BakeContext::Read(std::string_view id) noexcept {
    for (const BakeInput& input : mInputs) {
        if (input.mId != id) {
            continue;
        }
        bool known = false;
        for (const std::pmr::string& dependency : mDependencies) {
            known = known || dependency == id;
        }
        if (!known) {
            mDependencies.emplace_back(id); // the vector's resource propagates
        }
        return input.mBytes;
    }
    return core::Fail(core::ErrorCode::NotFound, "BakeContext: no such input");
}

BakeResult::BakeResult(core::Allocator& allocator) noexcept
    : mResource(allocator), mBlob(&mResource), mDependencies(&mResource) {}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result Bake(Baker& baker, std::span<const BakeInput> inputs, uint64_t schemaHash, core::Allocator& allocator,
                  BakeResult& out) noexcept {
    BakeContext context(inputs, allocator);
    BlobWriter writer(allocator);
    const core::Result baked = baker.Bake(context, writer);
    if (!baked.has_value()) {
        return baked;
    }
    out.mBlob.clear();
    writer.Finish(baker.GetKind(), baker.GetVersion(), schemaHash, out.mBlob);
    out.mDependencies.clear();
    for (const std::pmr::string& dependency : context.GetDependencies()) {
        out.mDependencies.emplace_back(dependency);
    }
    return {};
}

core::Expected<bool> IsDeterministic(Baker& baker, std::span<const BakeInput> inputs, uint64_t schemaHash,
                                     core::Allocator& allocator) noexcept {
    BakeResult first(allocator);
    BakeResult second(allocator);
    const core::Result one = Bake(baker, inputs, schemaHash, allocator, first);
    if (!one.has_value()) {
        return core::Fail(one.error().mCode, one.error().mMessage);
    }
    const core::Result two = Bake(baker, inputs, schemaHash, allocator, second);
    if (!two.has_value()) {
        return core::Fail(two.error().mCode, two.error().mMessage);
    }
    return first.mBlob == second.mBlob && first.mDependencies == second.mDependencies;
}

} // namespace oyi::asset
