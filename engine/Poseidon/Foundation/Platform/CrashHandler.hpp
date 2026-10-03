#pragma once

namespace Poseidon::Foundation
{
// Install fatal crash handlers so a native crash leaves an actionable report.
//
// Windows: installs SetUnhandledExceptionFilter, logs a dbghelp stack trace, and writes
// crash-<pid>.dmp next to the executable.
//
// Linux: installs fatal-signal handlers so a crash leaves an offline-symbolizable trace instead
// of a bare "Segmentation fault". On SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS the handler writes a
// crash report (signal + fault address + build id + version, a backtrace, the raw return
// addresses, and /proc/self/maps) to crashDir, enables a core dump, then re-raises to the
// default handler so the OS still produces a core. The report's addresses + module load bases +
// build id are enough to symbolize against the matching release binary with llvm-symbolizer /
// addr2line (the Linux analogue of the Windows .dmp -> PDB workflow).
//
// crashDir: Linux report directory (current directory if empty/null). Ignored on Windows.
void InstallCrashHandler(const char* crashDir);

//! DIAG-001: called by the Windows crash filter after the minidump and the stack trace are written, with
//! the exception code, the faulting address, the minidump path and the first frames as one line of text
//! ("module!symbol+0x.. | ..."). Used by the --diag event log to record a "crash" event and close its files.
//! nullptr turns it off. A no-op on Linux (the signal handler is async-signal-safe and stays that way).
using CrashNotifyFn = void (*)(unsigned code, const void* addr, const char* dump, const char* stack);
void SetCrashNotify(CrashNotifyFn fn);
} // namespace Poseidon::Foundation
