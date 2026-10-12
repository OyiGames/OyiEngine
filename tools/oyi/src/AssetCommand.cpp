#include <oyi_asset/Blob.h>
#include <oyi_core/Expected.h>

#include <Commands.h>

#include <cstddef>
#include <cstdio>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oyi::cli {

int RunAsset(std::span<const std::string_view> args) {
    Options options;
    if (!ParseOptions(args, options) || options.mPositional.size() != 2 || options.mPositional[0] != "inspect") {
        std::fputs("usage: oyi asset inspect <file.oyib> [--json]\n", stdout);
        return kExitUsage;
    }
    std::vector<std::byte> bytes;
    std::string error;
    if (!ReadFile(options.mPositional[1], bytes, error)) {
        PrintFailure(options, "IoFailure", error);
        return kExitFailed;
    }
    const auto view = asset::BlobView::Open(bytes);
    if (!view.has_value()) {
        PrintFailure(options, core::ToString(view.error().mCode), view.error().mMessage);
        return kExitFailed;
    }
    const asset::BlobHeader& header = view->GetHeader();
    if (options.mJson) {
        std::fputs(
            std::format(
                R"({{"formatVersion":{},"kind":"{:#018x}","version":{},"schemaHash":"{:#018x}","rootOffset":{},"payloadSize":{},"contentHash":"{:#018x}"}})",
                header.mFormatVersion, header.mKind, header.mVersion, header.mSchemaHash, header.mRootOffset,
                header.mPayloadSize, header.mContentHash)
                .c_str(),
            stdout);
        std::fputc('\n', stdout);
    } else if (!options.mQuiet) {
        std::fputs(std::format("blob: format {}, kind {:#018x}, baker version {}, schema {:#018x}\n"
                               "payload: {} bytes, root at {}, content hash {:#018x} (verified)\n",
                               header.mFormatVersion, header.mKind, header.mVersion, header.mSchemaHash,
                               header.mPayloadSize, header.mRootOffset, header.mContentHash)
                       .c_str(),
                   stdout);
    }
    return kExitOk;
}

} // namespace oyi::cli
