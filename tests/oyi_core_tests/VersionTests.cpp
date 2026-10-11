#include <oyi_core/Version.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("engine version is 0.1.0", "[core][version]") {
    const auto version = oyi::core::GetEngineVersion();
    CHECK(version == oyi::core::Version{.mMajor = 0, .mMinor = 1, .mPatch = 0});
    CHECK(oyi::core::ToString(version) == "0.1.0");
}

TEST_CASE("versions compare by major, minor, patch", "[core][version]") {
    using oyi::core::Version;
    STATIC_CHECK(Version{.mMajor = 0, .mMinor = 2, .mPatch = 0} > Version{.mMajor = 0, .mMinor = 1, .mPatch = 9});
    STATIC_CHECK(Version{.mMajor = 1, .mMinor = 0, .mPatch = 0} > Version{.mMajor = 0, .mMinor = 9, .mPatch = 9});
}

TEST_CASE("version json has the four agreed fields", "[core][version]") {
    const std::string json = oyi::core::GetVersionJson();
    CHECK(json.contains("\"engine\":\"0.1.0\""));
    CHECK(json.contains("\"protocol\":"));
    CHECK(json.contains("\"schemaHash\":"));
}
