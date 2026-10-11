#include <oyi_core/Assert.h>

#include <cstdio>
#include <cstdlib>
#include <format>

namespace oyi::core {

void ReportAssertionFailure(std::string_view expression, std::string_view message, std::source_location location) {
    // Until the logging module exists this writes straight to stderr. The field names already match
    // the structured (JSON Lines) log format so tooling does not have to change later.
    const std::string line =
        std::format(R"({{"level":"fatal","kind":"assert","expr":"{}","msg":"{}","file":"{}","line":{}}})", expression,
                    message, location.file_name(), location.line());
    std::fputs(line.c_str(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::abort();
}

} // namespace oyi::core
