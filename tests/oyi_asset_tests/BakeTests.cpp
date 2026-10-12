#include <oyi_asset/Bake.h>
#include <oyi_asset/Blob.h>
#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace {

using oyi::asset::Bake;
using oyi::asset::BakeContext;
using oyi::asset::BakeInput;
using oyi::asset::Baker;
using oyi::asset::BakeResult;
using oyi::asset::BlobExpectation;
using oyi::asset::BlobView;
using oyi::asset::BlobWriter;
using oyi::asset::IsDeterministic;
using oyi::asset::RelativeSpan;
using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;

[[nodiscard]] std::span<const std::byte> Bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
            text.size()}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

/// Bakes "a.txt" + "b.txt" into one text blob; reads "c.txt" only when a flag says so.
struct Concat {
    RelativeSpan<char> mText;
    uint32_t mSources = 0;
};

class ConcatBaker final : public Baker {
public:
    explicit ConcatBaker(bool readOptional) noexcept : mReadOptional(readOptional) {}

    [[nodiscard]] uint64_t GetKind() const noexcept override { return 0x5EED; }
    [[nodiscard]] uint32_t GetVersion() const noexcept override { return 2; }

    [[nodiscard]] oyi::core::Result Bake(BakeContext& context, BlobWriter& writer) noexcept override {
        const auto a = context.Read("a.txt");
        const auto b = context.Read("b.txt");
        if (!a.has_value() || !b.has_value()) {
            return oyi::core::Fail(ErrorCode::NotFound, "ConcatBaker: inputs missing");
        }
        std::span<const std::byte> c;
        if (mReadOptional) {
            const auto optional = context.Read("c.txt");
            if (optional.has_value()) {
                c = *optional;
            }
            const auto again = context.Read("a.txt"); // reading twice records once
            if (!again.has_value()) {
                return oyi::core::Fail(ErrorCode::NotFound, "ConcatBaker: a vanished");
            }
        }
        const BlobWriter::Offset root = writer.Allocate<Concat>();
        const auto size               = static_cast<uint32_t>(a->size() + b->size() + c.size());
        const BlobWriter::Offset text = writer.Allocate(size, 1);
        std::memcpy(&writer.At<char>(text), a->data(), a->size());
        std::memcpy(&writer.At<char>(text + static_cast<uint32_t>(a->size())), b->data(), b->size());
        std::memcpy(&writer.At<char>(text + static_cast<uint32_t>(a->size() + b->size())), c.data(), c.size());
        writer.LinkSpan<char>(root, text, size);
        writer.At<Concat>(root).mSources = c.empty() ? 2u : 3u;
        writer.SetRoot(root);
        return {};
    }

private:
    bool mReadOptional;
};

/// Breaks the contract on purpose: the output depends on how often it ran.
class CountingBaker final : public Baker {
public:
    [[nodiscard]] uint64_t GetKind() const noexcept override { return 1; }
    [[nodiscard]] uint32_t GetVersion() const noexcept override { return 1; }
    [[nodiscard]] oyi::core::Result Bake(BakeContext& /*context*/, BlobWriter& writer) noexcept override {
        const BlobWriter::Offset root = writer.Allocate<uint32_t>();
        writer.At<uint32_t>(root)     = ++mRuns;
        writer.SetRoot(root);
        return {};
    }

private:
    uint32_t mRuns = 0;
};

} // namespace

TEST_CASE("a bake records exactly the inputs it read and produces a checked blob", "[asset][bake]") {
    SystemAllocator allocator;
    const std::array<BakeInput, 3> inputs{BakeInput{.mId = "a.txt", .mBytes = Bytes("hello ")},
                                          BakeInput{.mId = "b.txt", .mBytes = Bytes("world")},
                                          BakeInput{.mId = "c.txt", .mBytes = Bytes("!")}};
    ConcatBaker plain(false);
    BakeResult result(allocator);
    REQUIRE(Bake(plain, inputs, 0x77, allocator, result).has_value());
    REQUIRE(result.mDependencies.size() == 2);
    CHECK(result.mDependencies[0] == "a.txt");
    CHECK(result.mDependencies[1] == "b.txt");
    const auto view =
        BlobView::Open(result.mBlob, BlobExpectation{.mKind = 0x5EED, .mVersion = 2, .mSchemaHash = 0x77});
    REQUIRE(view.has_value());
    const auto* const concat = view->Root<Concat>();
    CHECK(std::string_view(concat->mText.Get().data(), concat->mText.Get().size()) == "hello world");
    CHECK(concat->mSources == 2);

    ConcatBaker greedy(true);
    REQUIRE(Bake(greedy, inputs, 0x77, allocator, result).has_value());
    REQUIRE(result.mDependencies.size() == 3);
    CHECK(result.mDependencies[2] == "c.txt");
    CHECK(BlobView::Open(result.mBlob)->Root<Concat>()->mSources == 3);
}

TEST_CASE("a missing input fails the bake and the determinism check tells bakers apart", "[asset][bake]") {
    SystemAllocator allocator;
    const std::array<BakeInput, 1> inputs{BakeInput{.mId = "a.txt", .mBytes = Bytes("only a")}};
    ConcatBaker plain(false);
    BakeResult result(allocator);
    CHECK(Bake(plain, inputs, 0, allocator, result).error().mCode == ErrorCode::NotFound);

    const std::array<BakeInput, 2> both{BakeInput{.mId = "a.txt", .mBytes = Bytes("a")},
                                        BakeInput{.mId = "b.txt", .mBytes = Bytes("b")}};
    CHECK(*IsDeterministic(plain, both, 1, allocator));
    CountingBaker counting;
    CHECK_FALSE(*IsDeterministic(counting, both, 1, allocator));
    CHECK(IsDeterministic(plain, inputs, 1, allocator).error().mCode == ErrorCode::NotFound);
}

TEST_CASE("the context hands out inputs by id and nothing else", "[asset][bake]") {
    SystemAllocator allocator;
    const std::array<BakeInput, 1> inputs{BakeInput{.mId = "x", .mBytes = Bytes("xyz")}};
    BakeContext context(inputs, allocator);
    CHECK(context.GetDependencies().empty());
    CHECK(context.Read("missing").error().mCode == ErrorCode::NotFound);
    CHECK(context.GetDependencies().empty());
    REQUIRE(context.Read("x").has_value());
    CHECK(context.Read("x")->size() == 3);
    CHECK(context.GetDependencies().size() == 1);
    CHECK(&context.GetScratch() == &allocator);
}
