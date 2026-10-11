#include <oyi_core/Version.h>

#include <format>

namespace oyi::core {

Version GetEngineVersion() noexcept {
    return Version{.mMajor = 0, .mMinor = 1, .mPatch = 0};
}

std::string ToString(const Version& version) {
    return std::format("{}.{}.{}", version.mMajor, version.mMinor, version.mPatch);
}

std::string GetVersionJson() {
    // The protocol version and schema hash are wired in once their modules exist. The fields are
    // emitted now so that clients never have to change their parser.
    return std::format(R"({{"engine":"{}","protocol":0,"schemaHash":"0"}})", ToString(GetEngineVersion()));
}

} // namespace oyi::core
