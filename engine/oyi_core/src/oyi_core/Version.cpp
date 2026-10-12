#include <oyi_core/Version.h>

#include <cstdint>
#include <format>
#include <string>

namespace oyi::core {

Version GetEngineVersion() noexcept {
    return Version{.mMajor = 0, .mMinor = 1, .mPatch = 0};
}

std::string ToString(const Version& version) {
    return std::format("{}.{}.{}", version.mMajor, version.mMinor, version.mPatch);
}

std::string GetVersionJson(uint64_t schemaHash) {
    // The protocol version is wired in once the protocol module exists; the field is emitted now
    // so that clients never have to change their parser.
    if (schemaHash == 0) {
        return std::format(R"({{"engine":"{}","protocol":0,"schemaHash":"0"}})", ToString(GetEngineVersion()));
    }
    return std::format(R"({{"engine":"{}","protocol":0,"schemaHash":"{:#018x}"}})", ToString(GetEngineVersion()),
                       schemaHash);
}

} // namespace oyi::core
