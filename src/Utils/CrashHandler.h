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

#include <utility>

namespace rs2v {

// Install process-wide crash handlers:
//   * std::set_terminate          (uncaught C++ exceptions)
//   * Windows: SetUnhandledExceptionFilter (SEH faults)
//   * POSIX:   sigaction for SIGSEGV / SIGABRT / SIGFPE / SIGILL
// Idempotent: calling more than once is harmless (only the first call installs).
void InstallCrashHandler();

// ---------------------------------------------------------------------------
// Non-fatal exception handling
// ---------------------------------------------------------------------------
// The crash handler above turns an *uncaught* exception (one that reaches
// std::terminate) into a fatal, diagnosed abort. The helpers below are the
// complement: they let a caller CATCH an exception at a subsystem boundary, log
// it with the same diagnostics, and KEEP RUNNING — for the many errors that are
// recoverable (one bad packet, one bad command, a single misbehaving tick)
// rather than reasons to take the whole server down.

// Log the exception currently being handled (call from inside a catch block) as
// a NON-FATAL event: a distinct banner, the type/message, the context label, and
// a best-effort stack trace — then RETURN (no abort). Safe to call with no
// exception in flight. noexcept: reporting must never itself throw.
void ReportNonFatalException(const char* context) noexcept;

// Total number of non-fatal exceptions reported so far (process-wide). Useful
// for surfacing "the server recovered from N errors" in status/telemetry.
unsigned long long NonFatalExceptionCount() noexcept;

// Run `fn`, swallowing and reporting any exception as non-fatal. Returns true if
// `fn` completed without throwing, false if an exception was caught + reported.
// Use at boundaries where a failure should be logged but not propagate to
// std::terminate (game tick, per-packet processing, command dispatch, etc.).
template <typename Fn>
bool Guard(const char* context, Fn&& fn) noexcept {
    try {
        std::forward<Fn>(fn)();
        return true;
    } catch (...) {
        ReportNonFatalException(context);
        return false;
    }
}

}  // namespace rs2v
