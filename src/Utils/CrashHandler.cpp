// src/Utils/CrashHandler.cpp
//
// Implementation of the minimal headless crash handler. See CrashHandler.h.
//
// Everything is emitted to BOTH Logger::Error(...) AND fprintf(stderr, ...) +
// fflush, because mid-crash the logger may be unusable (corrupt heap, held
// locks, etc.) — stderr is the more reliable channel, and the logger gives us
// the on-disk log when it still works.

#include "Utils/CrashHandler.h"

#include "Utils/Logger.h"
#include "Utils/StackTrace.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <typeinfo>

#ifdef _WIN32
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <unistd.h>
#endif

namespace rs2v {
namespace {

constexpr const char* kBanner = "===== RS2V CRASH =====";

std::atomic<bool> g_installed{false};

// Emit a line to BOTH stderr and the logger. stderr first (most reliable),
// flushed immediately so it survives an imminent abort.
void EmitLine(const std::string& line) {
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
    // Logger uses printf-style formatting; pass through a literal "%s" so any
    // '%' in the message is not misinterpreted as a format specifier.
    Logger::Error("%s", line.c_str());
}

void EmitBanner() {
    EmitLine("");
    EmitLine(kBanner);
}

// Capture and emit a stack trace (skip the crash-handler frames themselves).
void EmitStackTrace() {
    StackTrace trace = StackTrace::Capture(/*skipFrames=*/2);
    EmitLine("Stack trace:");
    const std::string text = trace.ToString();
    if (text.empty()) {
        EmitLine("  <no frames captured (symbols unavailable on this platform)>");
        return;
    }
    // Emit per line so both channels stay line-oriented.
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        EmitLine(text.substr(start, nl - start));
        start = nl + 1;
    }
}

// ---------------------------------------------------------------------------
// std::terminate handler — recover what() by rethrowing the in-flight exception
// ---------------------------------------------------------------------------
void TerminateHandler() {
    EmitBanner();
    EmitLine("Cause: std::terminate (uncaught exception or noexcept violation)");

    // Rethrow inside the handler to recover the exception type + message.
    if (std::current_exception()) {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            EmitLine(std::string("Exception type   : ") + typeid(e).name());
            EmitLine(std::string("Exception message: ") + e.what());
        } catch (...) {
            EmitLine("Exception        : non-std exception (unknown type)");
        }
    } else {
        EmitLine("Exception        : (none in flight — direct std::terminate call)");
    }

    EmitStackTrace();
    EmitLine("===== END RS2V CRASH =====");
    std::fflush(stderr);
    std::abort();
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Windows SEH unhandled-exception filter
// ---------------------------------------------------------------------------

// MSVC encodes a thrown C++ exception as this proprietary SEH code ('msc'|0xE0).
// Such an exception reaches the unhandled-exception filter (not set_terminate)
// when it escapes main on Windows.
constexpr DWORD kMsvcCppException = 0xE06D7363;

const char* SehCodeName(DWORD code) {
    switch (code) {
        case kMsvcCppException:               return "MSVC C++ EXCEPTION";
        case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:          return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION:      return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        default:                              return "UNKNOWN_EXCEPTION";
    }
}

// Best-effort recovery of an uncaught C++ exception's what() from inside the
// SEH filter. On Windows an uncaught C++ exception reaches the unhandled-
// exception filter (the C++ runtime has NOT yet populated
// std::current_exception()), so we cannot rethrow. Instead the thrown object
// pointer is carried in the exception record: for the MSVC C++ exception the
// raised object address is ExceptionInformation[1]. If it derives from
// std::exception we can read what(). This is wrapped in __try/__except so a bad
// pointer can never make the crash handler itself fault.
#if defined(_MSC_VER)
// Raw, no-C++-unwinding probe of the thrown object (C4509/C2712 forbid __try in
// a function with objects needing unwinding, so this helper holds only PODs).
// __try/__except is MSVC-only; MinGW/GCC has no SEH __try, and its C++ runtime
// uses a different exception-record layout, so the probe is MSVC-gated.
// Copies what()/typeid name into caller-provided plain buffers under SEH
// protection. Returns true on success.
bool ProbeCppException(void* obj, char* typeBuf, size_t typeCap,
                       char* msgBuf, size_t msgCap) {
    __try {
        const std::exception* ex = static_cast<const std::exception*>(obj);
        const char* tn = typeid(*ex).name();
        const char* wt = ex->what();
        std::strncpy(typeBuf, tn ? tn : "?", typeCap - 1);
        typeBuf[typeCap - 1] = '\0';
        std::strncpy(msgBuf, wt ? wt : "?", msgCap - 1);
        msgBuf[msgCap - 1] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif  // _MSC_VER

void EmitCppExceptionDetails(const EXCEPTION_RECORD* rec) {
#if defined(_MSC_VER)
    if (!rec || rec->NumberParameters < 2) {
        EmitLine("Exception        : C++ exception (object unavailable)");
        return;
    }
    void* obj = reinterpret_cast<void*>(rec->ExceptionInformation[1]);
    if (!obj) {
        EmitLine("Exception        : C++ exception (null object)");
        return;
    }
    char typeBuf[256] = {};
    char msgBuf[512] = {};
    if (ProbeCppException(obj, typeBuf, sizeof(typeBuf), msgBuf, sizeof(msgBuf))) {
        EmitLine(std::string("Exception type   : ") + typeBuf);
        EmitLine(std::string("Exception message: ") + msgBuf);
    } else {
        EmitLine("Exception        : C++ exception (not derived from std::exception, "
                 "or message unreadable)");
    }
#else
    // MinGW/GCC: no SEH __try and a different exception-record layout. The
    // symbolized stack trace below still pinpoints the throw site; the message
    // is not safely recoverable from the SEH record here.
    (void)rec;
    EmitLine("Exception        : C++ exception (message not recoverable on this "
             "toolchain; see stack trace for throw site)");
#endif
}

LONG WINAPI SehFilter(EXCEPTION_POINTERS* ep) {
    EmitBanner();

    const bool isCpp = ep && ep->ExceptionRecord &&
                       ep->ExceptionRecord->ExceptionCode == kMsvcCppException;
    EmitLine(isCpp ? "Cause: unhandled C++ exception"
                   : "Cause: unhandled Windows (SEH) exception");

    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
        const DWORD code = rec->ExceptionCode;

        char buf[256];
        std::snprintf(buf, sizeof(buf), "Exception code   : 0x%08lX (%s)",
                      static_cast<unsigned long>(code), SehCodeName(code));
        EmitLine(buf);

        std::snprintf(buf, sizeof(buf), "Faulting address : 0x%p",
                      rec->ExceptionAddress);
        EmitLine(buf);

        if (isCpp) {
            EmitCppExceptionDetails(rec);
        }

        if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            const ULONG_PTR rw   = rec->ExceptionInformation[0];
            const ULONG_PTR addr = rec->ExceptionInformation[1];
            const char* op = (rw == 0) ? "READ" : (rw == 1) ? "WRITE"
                            : (rw == 8) ? "DEP/EXECUTE" : "UNKNOWN";
            std::snprintf(buf, sizeof(buf), "Access violation : %s at 0x%p", op,
                          reinterpret_cast<void*>(addr));
            EmitLine(buf);
        }
    } else {
        EmitLine("Exception record : (unavailable)");
    }

    // CaptureStackBackTrace-based trace. The exception CONTEXT is the most
    // accurate origin, but CaptureStackBackTrace already gives a usable,
    // symbolized stack from inside the filter and is dramatically simpler /
    // more robust than a manual StackWalk64 (no special-casing per arch).
    EmitStackTrace();
    EmitLine("===== END RS2V CRASH =====");
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif  // _WIN32

#if defined(__unix__) || defined(__APPLE__)
// ---------------------------------------------------------------------------
// POSIX fatal-signal handler
// ---------------------------------------------------------------------------
volatile sig_atomic_t g_inSignalHandler = 0;

const char* SignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGFPE:  return "SIGFPE (floating-point/integer arithmetic)";
        case SIGILL:  return "SIGILL (illegal instruction)";
#ifdef SIGBUS
        case SIGBUS:  return "SIGBUS (bus error)";
#endif
        default:      return "UNKNOWN SIGNAL";
    }
}

void SignalHandler(int sig) {
    // Guard against a crash inside the handler: re-raise with the default
    // disposition if we re-enter.
    if (g_inSignalHandler) {
        std::signal(sig, SIG_DFL);
        std::raise(sig);
        return;
    }
    g_inSignalHandler = 1;

    EmitBanner();
    EmitLine(std::string("Cause: fatal signal ") + std::to_string(sig) + " - " +
             SignalName(sig));

    // Best-effort symbolized trace (not strictly async-signal-safe, but the
    // process is about to die anyway — matches common crash-reporter practice).
    EmitStackTrace();
    EmitLine("===== END RS2V CRASH =====");
    std::fflush(stderr);

    // Restore default handler and re-raise to get the normal termination /
    // core dump.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif  // POSIX

}  // namespace

void InstallCrashHandler() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        return;  // Already installed.
    }

    std::set_terminate(&TerminateHandler);

#ifdef _WIN32
    SetUnhandledExceptionFilter(&SehFilter);
#elif defined(__unix__) || defined(__APPLE__)
    std::signal(SIGSEGV, &SignalHandler);
    std::signal(SIGABRT, &SignalHandler);
    std::signal(SIGFPE,  &SignalHandler);
    std::signal(SIGILL,  &SignalHandler);
#ifdef SIGBUS
    std::signal(SIGBUS,  &SignalHandler);
#endif
#endif

    Logger::Info("[CrashHandler] Crash handler installed (terminate + "
#ifdef _WIN32
                 "SEH filter"
#elif defined(__unix__) || defined(__APPLE__)
                 "POSIX signal handlers"
#else
                 "no-op on this platform"
#endif
                 ")");
}

}  // namespace rs2v
