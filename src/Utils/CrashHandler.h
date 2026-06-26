// src/Utils/CrashHandler.h
//
// Minimal crash handler for the headless RS2V dedicated server.
//
// Goal: when the server dies — an uncaught C++ exception, a Windows SEH fault
// (access violation, stack overflow, ...), or a POSIX fatal signal (SIGSEGV,
// SIGABRT, SIGFPE, SIGILL) — print a clear banner, the exception/signal
// details, and a symbolized stack trace to BOTH the logger and stderr, then
// terminate. This replaces the silent "exit code 3" with actionable
// diagnostics.
//
// This is deliberately stripped down vs. a full crash reporter: NO minidump,
// NO upload, NO screenshots, NO consent dialogs. Just diagnostics to the log
// and the console.
//
// Install it as the VERY FIRST thing in main(), before any other init.

#pragma once

namespace rs2v {

// Install process-wide crash handlers:
//   * std::set_terminate          (uncaught C++ exceptions)
//   * Windows: SetUnhandledExceptionFilter (SEH faults)
//   * POSIX:   sigaction for SIGSEGV / SIGABRT / SIGFPE / SIGILL
// Idempotent: calling more than once is harmless (only the first call installs).
void InstallCrashHandler();

}  // namespace rs2v
