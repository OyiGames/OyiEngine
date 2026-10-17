#include <oyi_core/Version.h>
#include <oyi_object/Components.gen.h>  // IWYU pragma: keep
#include <oyi_physics/Components.gen.h> // IWYU pragma: keep
#include <oyi_rpc/Rpc.h>
#include <oyi_transform/Components.gen.h> // IWYU pragma: keep

#include <Commands.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <initializer_list>
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
            const std::string_view named = arg.substr(2);
            const size_t equals          = named.find('=');
            out.mNamed.emplace_back(named.substr(0, equals),
                                    equals == std::string_view::npos ? std::string_view{} : named.substr(equals + 1));
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

unsigned long long GetBuildSchemaHash() noexcept {
    // Boost-style combination: the order of the modules is part of the hash.
    unsigned long long hash = 0;
    for (const unsigned long long part :
         {oyi::transform::kSchemaHash, oyi::object::kSchemaHash, oyi::physics::kSchemaHash}) {
        hash ^= part + 0x9E37'79B9'7F4A'7C15ull + (hash << 6u) + (hash >> 2u);
    }
    return hash;
}

int RunVersion(std::span<const std::string_view> args) {
    Options options;
    if (!ParseOptions(args, options) || !options.mPositional.empty() || !options.mNamed.empty()) {
        std::fputs("usage: oyi --version [--json]\n", stdout);
        return kExitUsage;
    }
    const uint64_t schemaHash = GetBuildSchemaHash();
    const std::string text =
        options.mJson ? core::GetVersionJson(schemaHash, rpc::kProtocolVersion)
                      : std::format("{} (schema {:#018x}, protocol {})", core::ToString(core::GetEngineVersion()),
                                    schemaHash, rpc::kProtocolVersion);
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    return kExitOk;
}

} // namespace oyi::cli
