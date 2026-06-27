// tests/StackTraceTests.cpp
//
// Tests for the cross-platform stack-trace capture used by the crash handler.
//
// The abort-y crash handlers (std::terminate / SEH filter / signal handlers)
// cannot be unit-tested in-process, but the stack CAPTURE that feeds them can.
//
// What we assert:
//   * Capturing from a known function returns a non-empty frame list.
//   * ToString() on a captured trace is non-empty and is one frame per line.
//   * Each captured frame carries a non-zero instruction address (the weak
//     invariant that holds even when no PDB/symbols are present in the test
//     environment).
//   * STRONGER invariant, asserted only when symbol resolution is actually
//     available in this build/run: the name of the enclosing function appears
//     in the formatted output. If symbols are unavailable we skip that check
//     (see HasAnyResolvedSymbol) rather than fail — a symbol-less CI runner
//     must still go green.

#include <gtest/gtest.h>

#include "Utils/StackTrace.h"

#include <string>

namespace {

// Returns true if the APPLICATION's own symbols resolved — i.e. at least one
// frame in the same module as the capture site (frame 0 is our own code) carries
// a function name. Used to gate the "function name appears in output" assertion.
//
// Gating on ANY resolved name is wrong: on Windows the system DLLs
// (kernel32/ntdll) always resolve their EXPORTED names even in a Release build
// with no application PDB, and on POSIX libc frames resolve via dladdr — so an
// "any frame resolved" gate is true even when the app's own frames are
// unsymbolized, making the strong assertion run against an "<unknown>" app frame
// and fail. Scoping to the capture site's module keeps a symbol-less runner on
// the skip path (green) while still asserting in a symbol-resolving build.
bool HasAppSymbols(const rs2v::StackTrace& trace) {
    const auto& frames = trace.GetFrames();
    if (frames.empty()) return false;
    const std::string& appModule = frames.front().moduleName;
    for (const auto& f : frames) {
        if (!f.functionName.empty() && f.moduleName == appModule) {
            return true;
        }
    }
    return false;
}

// A deliberately-named function so we can look for it in the symbolized output.
// NOINLINE so the frame is not optimized away into its caller.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
rs2v::StackTrace
DistinctlyNamedCaptureFunction() {
    rs2v::StackTrace trace = rs2v::StackTrace::Capture();
    // Touch the result so the compiler cannot elide the call entirely.
    volatile int sink = trace.GetFrameCount();
    (void)sink;
    return trace;
}

}  // namespace

TEST(StackTraceTests, CaptureReturnsNonEmptyFrameList) {
    rs2v::StackTrace trace = rs2v::StackTrace::Capture();
    EXPECT_GT(trace.GetFrameCount(), 0) << "Expected at least one captured frame";
    EXPECT_FALSE(trace.GetFrames().empty());
}

TEST(StackTraceTests, ToStringIsNonEmpty) {
    rs2v::StackTrace trace = rs2v::StackTrace::Capture();
    std::string text = trace.ToString();
    EXPECT_FALSE(text.empty()) << "Formatted stack trace should not be empty";
    // One frame per line: there should be at least as many newlines as frames.
    size_t newlines = 0;
    for (char c : text) {
        if (c == '\n') ++newlines;
    }
    EXPECT_GE(newlines, static_cast<size_t>(trace.GetFrameCount()));
}

TEST(StackTraceTests, AtLeastOneFrameHasNonZeroAddress) {
    // Weak invariant that must hold even with no symbols available: the raw
    // instruction pointers captured by the OS backtrace are real addresses.
    rs2v::StackTrace trace = rs2v::StackTrace::Capture();
    bool anyNonZero = false;
    for (const auto& f : trace.GetFrames()) {
        if (f.address != 0) {
            anyNonZero = true;
            break;
        }
    }
    EXPECT_TRUE(anyNonZero) << "Expected at least one frame with a non-zero address";
}

TEST(StackTraceTests, FunctionNameAppearsWhenSymbolsAvailable) {
    rs2v::StackTrace trace = DistinctlyNamedCaptureFunction();

    if (!HasAppSymbols(trace)) {
        GTEST_SKIP() << "Application symbol resolution unavailable in this "
                        "build/environment (no PDB / stripped binary; system-DLL "
                        "exports don't count); skipping name assertion.";
    }

    // In a symbol-resolving build, the enclosing function should show up
    // somewhere in the formatted trace.
    std::string text = trace.ToString();
    EXPECT_NE(text.find("DistinctlyNamedCaptureFunction"), std::string::npos)
        << "Expected the capturing function name in the symbolized trace:\n"
        << text;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
