#include <oyi_core/Version.h>
#include <oyi_transform/Components.gen.h> // IWYU pragma: keep

#include <Commands.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oyi::cli {

bool ParseOptions(std::span<const std::string_view> args, Options& out) {
    for (const std::string_view arg : args) {
        if (arg == "--json") {
            out.mJson = true;
        } else if (arg == "--quiet") {
            out.mQuiet = true;
        } else if (arg.starts_with("--")) {
            return false;
        } else {
            out.mPositional.push_back(arg);
        }
    }
    return true;
}

std::string JsonString(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

void PrintFailure(const Options& options, std::string_view code, std::string_view message) {
    if (options.mJson) {
        std::fputs(
            std::format(R"({{"error":{{"code":{},"message":{}}}}})", JsonString(code), JsonString(message)).c_str(),
            stdout);
        std::fputc('\n', stdout);
        return;
    }
    if (!options.mQuiet) {
        std::fputs(std::format("oyi: {}: {}\n", code, message).c_str(), stderr);
    }
}

bool ReadFile(std::string_view path, std::vector<std::byte>& out, std::string& error) {
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file.is_open()) {
        error = std::format("cannot open {}", path);
        return false;
    }
    std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    out.resize(chars.size());
    for (size_t i = 0; i < chars.size(); ++i) {
        out[i] = static_cast<std::byte>(chars[i]);
    }
    return true;
}

int RunVersion(std::span<const std::string_view> args) {
    Options options;
    if (!ParseOptions(args, options) || !options.mPositional.empty()) {
        std::fputs("usage: oyi --version [--json]\n", stdout);
        return kExitUsage;
    }
    // The schema hash of this build: every engine module that declares components, in order.
    const uint64_t schemaHash = oyi::transform::kSchemaHash;
    const std::string text =
        options.mJson ? core::GetVersionJson(schemaHash)
                      : std::format("{} (schema {:#018x})", core::ToString(core::GetEngineVersion()), schemaHash);
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    return kExitOk;
}

} // namespace oyi::cli
