#include <oyi_core/Expected.h>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace {

using oyi::core::Error;
using oyi::core::ErrorCode;
using oyi::core::Expected;
using oyi::core::Fail;
using oyi::core::Result;
using oyi::core::ToString;

constexpr Expected<int> ParseAnswer(bool valid) {
    if (!valid) {
        return Fail(ErrorCode::InvalidArgument, "not an answer");
    }
    return 42;
}

constexpr Result CheckState(bool ready) {
    if (!ready) {
        return Fail(ErrorCode::InvalidState, "not ready");
    }
    return {};
}

} // namespace

TEST_CASE("Expected carries either a value or an Error", "[core][expected]") {
    STATIC_REQUIRE(ParseAnswer(true).has_value());
    STATIC_REQUIRE(*ParseAnswer(true) == 42);
    STATIC_REQUIRE_FALSE(ParseAnswer(false).has_value());
    STATIC_REQUIRE(ParseAnswer(false).error().mCode == ErrorCode::InvalidArgument);
    STATIC_REQUIRE(ParseAnswer(false).error().mMessage == "not an answer");
}

TEST_CASE("Result reports success without a value and errors compare by code and message", "[core][expected]") {
    STATIC_REQUIRE(CheckState(true).has_value());
    constexpr Error kNotReady{.mCode = ErrorCode::InvalidState, .mMessage = "not ready"};
    STATIC_REQUIRE(CheckState(false).error() == kNotReady);
    constexpr Error kOtherMessage{.mCode = ErrorCode::InvalidState, .mMessage = "something else"};
    STATIC_REQUIRE(CheckState(false).error() != kOtherMessage);
}

TEST_CASE("Error is small and trivially copyable so it can be returned through many layers", "[core][expected]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<Error>);
    STATIC_REQUIRE(sizeof(Error) <= 32);
    STATIC_REQUIRE(Error{}.mCode == ErrorCode::None);
}

TEST_CASE("every ErrorCode has a name", "[core][expected]") {
    STATIC_REQUIRE(ToString(ErrorCode::None) == "None");
    STATIC_REQUIRE(ToString(ErrorCode::OutOfMemory) == "OutOfMemory");
    STATIC_REQUIRE(ToString(ErrorCode::Cancelled) == "Cancelled");
    STATIC_REQUIRE(ToString(static_cast<ErrorCode>(0xFF)) == "Unknown");
}
