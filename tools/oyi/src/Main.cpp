// oyi: the single command-line entry point (ADR-0016). Only --version exists for now; subcommands
// are added together with the modules they operate on.
#include <oyi_core/Version.h>

#include <cstdio>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk       = 0;
constexpr int kExitUsage    = 2;
constexpr int kExitInternal = 3;

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

int Run(std::span<const std::string_view> args) {
    if (args.empty()) {
        PrintUsage();
        return kExitUsage;
    }
    // Remaining arguments are handed to the subcommand as a span so that no subcommand has to
    // re-parse argc/argv.
    const auto command = args[0];
    const auto rest    = args.subspan(1);
    if (command == "--version") {
        return RunVersion(rest);
    }
    PrintUsage();
    return kExitUsage;
}

} // namespace

int main(int argc, char** argv) {
    // The tool links the standard library with exceptions enabled; anything escaping a subcommand is
    // reported here instead of terminating silently.
    try {
        std::vector<std::string_view> args;
        args.reserve(static_cast<size_t>(argc));
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
        return Run(args);
    } catch (const std::exception& error) {
        // Only non-throwing calls here, so nothing can escape main.
        std::fputs("oyi: internal error: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return kExitInternal;
    }
}
