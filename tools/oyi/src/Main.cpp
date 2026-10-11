// oyi: the single command-line entry point (ADR-0016). Only --version exists for now; subcommands
// are added together with the modules they operate on.
#include <oyi_core/Version.h>

#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk    = 0;
constexpr int kExitUsage = 2;

void PrintUsage() {
    std::fputs("usage: oyi --version [--json]\n", stdout);
}

int RunVersion(std::span<const std::string_view> args) {
    bool json = false;
    for (const auto arg : args) {
        if (arg == "--json") {
            json = true;
        } else {
            PrintUsage();
            return kExitUsage;
        }
    }
    const auto text = json ? oyi::core::GetVersionJson() : oyi::core::ToString(oyi::core::GetEngineVersion());
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    return kExitOk;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return kExitUsage;
    }
    const std::string_view command = argv[1];
    // Remaining arguments are handed to the subcommand as a span so that no subcommand has to
    // re-parse argc/argv.
    std::vector<std::string_view> rest;
    rest.reserve(static_cast<size_t>(argc));
    for (int i = 2; i < argc; ++i) {
        rest.emplace_back(argv[i]);
    }
    if (command == "--version") {
        return RunVersion(rest);
    }
    PrintUsage();
    return kExitUsage;
}
