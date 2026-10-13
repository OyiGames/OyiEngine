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

std::string GetVersionJson(uint64_t schemaHash, uint32_t protocolVersion) {
    if (schemaHash == 0) {
        return std::format(R"({{"engine":"{}","protocol":{},"schemaHash":"0"}})", ToString(GetEngineVersion()),
                           protocolVersion);
    }
    return std::format(R"({{"engine":"{}","protocol":{},"schemaHash":"{:#018x}"}})", ToString(GetEngineVersion()),
                       protocolVersion, schemaHash);
}

} // namespace oyi::core
