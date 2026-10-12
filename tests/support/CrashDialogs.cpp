// Linked into every test executable: a failed OYI_CHECK, an escaped exception or a CRT assertion
// must end the process so that ctest reports it, never block on a dialog box.

#ifdef _WIN32

// The CRT and Win32 headers confuse include-cleaner; everything used here comes from these two.
// NOLINTBEGIN(misc-include-cleaner)
#include <crtdbg.h>
#include <windows.h>

#include <cstdlib>

namespace {

struct NoDialogs {
    NoDialogs() noexcept {
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    }
};

const NoDialogs kNoDialogs;

} // namespace
// NOLINTEND(misc-include-cleaner)

#else

namespace oyi::tests {

/// Nothing to configure here; the symbol keeps the translation unit non-empty.
int gCrashDialogsConfigured = 0;

} // namespace oyi::tests

#endif
