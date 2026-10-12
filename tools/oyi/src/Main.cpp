// oyi: the single command-line entry point (ADR-0016). Subcommands live next to the modules they
// operate on; this file only routes.
#include <Commands.h>

#include <cstdio>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

namespace {

void PrintUsage() {
    std::fputs("usage: oyi --version [--json]\n"
               "       oyi world  inspect <file.oyiw> [--json]\n"
               "       oyi asset  inspect <file.oyib> [--json]\n"
               "       oyi schema validate <header.h> [--json]\n",
               stdout);
}

int Run(std::span<const std::string_view> args) {
    if (args.empty()) {
        PrintUsage();
        return oyi::cli::kExitUsage;
    }
    const auto command = args[0];
    const auto rest    = args.subspan(1);
    if (command == "--version") {
        return oyi::cli::RunVersion(rest);
    }
    if (command == "world") {
        return oyi::cli::RunWorld(rest);
    }
    if (command == "asset") {
        return oyi::cli::RunAsset(rest);
    }
    if (command == "schema") {
        return oyi::cli::RunSchema(rest);
    }
    PrintUsage();
    return oyi::cli::kExitUsage;
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
        std::fputs("oyi: internal error: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return oyi::cli::kExitInternal;
    }
}
