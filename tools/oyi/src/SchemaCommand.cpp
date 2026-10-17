// `oyi schema validate`: the C++ front of tools/codegen's linter (ADR-0011, ADR-0016). The
// generator stays Python; this command finds it, the interpreter, libclang and the compiler
// arguments the build was configured with, and forwards.
#include <Commands.h>

#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

#ifndef OYI_CODEGEN_DIR
#define OYI_CODEGEN_DIR ""
#endif
#ifndef OYI_PYTHON
#define OYI_PYTHON "python3"
#endif
#ifndef OYI_LIBCLANG
#define OYI_LIBCLANG ""
#endif
#ifndef OYI_CODEGEN_ARGS_FILE
#define OYI_CODEGEN_ARGS_FILE ""
#endif

namespace oyi::cli {

namespace {

[[nodiscard]] std::string Setting(const char* variable, std::string_view fallback) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* const value = std::getenv(variable);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] std::string Quote(std::string_view text) {
    std::string out = "\"";
    out += text;
    out += '"';
    return out;
}

} // namespace

int RunSchema(std::span<const std::string_view> args) {
    Options options;
    const bool parsed = ParseOptions(args, options) && options.mNamed.empty();
    if (parsed && options.mPositional.size() == 1 && options.mPositional[0] == "dump") {
        // The merged schema of this build; the same text with or without --json.
        if (!options.mQuiet) {
            const std::string_view json = GetBuildSchemaJson();
            std::fwrite(json.data(), 1, json.size(), stdout);
        }
        return kExitOk;
    }
    if (!parsed || options.mPositional.size() != 2 || options.mPositional[0] != "validate") {
        std::fputs("usage: oyi schema validate <header.h> [--json]\n"
                   "       oyi schema dump [--json]\n",
                   stdout);
        return kExitUsage;
    }
    const std::string codegenDir = Setting("OYI_CODEGEN_DIR", OYI_CODEGEN_DIR);
    const std::string python     = Setting("OYI_PYTHON", OYI_PYTHON);
    const std::string libclang   = Setting("OYI_LIBCLANG", OYI_LIBCLANG);
    const std::string argsFile   = Setting("OYI_CODEGEN_ARGS_FILE", OYI_CODEGEN_ARGS_FILE);
    if (codegenDir.empty()) {
        PrintFailure(options, "InvalidState", "tools/codegen not found; set OYI_CODEGEN_DIR");
        return kExitFailed;
    }
    std::string command = std::format("cd /d {} && ", Quote(codegenDir));
#ifndef _WIN32
    command = std::format("cd {} && ", Quote(codegenDir));
#endif
    command += Quote(python) + " -m oyi_codegen validate " + Quote(options.mPositional[1]);
    if (options.mJson) {
        command += " --json";
    }
    if (!libclang.empty()) {
        command += " --libclang " + Quote(libclang);
    }
    // One compiler argument per line, written by CMake at configure time.
    if (!argsFile.empty()) {
        std::ifstream file(argsFile);
        std::string line;
        bool first = true;
        while (std::getline(file, line)) {
            if (line.empty()) {
                continue;
            }
            command += first ? " -- " : " ";
            command += Quote(line);
            first = false;
        }
    }
#ifdef _WIN32
    command = Quote(command); // cmd.exe strips one pair of outer quotes
#endif
    std::fflush(stdout);
    // NOLINTNEXTLINE(concurrency-mt-unsafe,cert-env33-c,bugprone-command-processor)
    const int status = std::system(command.c_str());
    if (status == 0) {
        return kExitOk;
    }
    if (!options.mJson && !options.mQuiet) {
        std::fputs("oyi: schema validation failed\n", stderr);
    }
    return kExitFailed;
}

} // namespace oyi::cli
