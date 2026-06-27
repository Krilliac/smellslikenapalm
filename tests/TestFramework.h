// tests/TestFramework.h
//
// RS2V native test framework — a self-contained, dependency-free unit test
// system for the RS2V server. No external libraries, no network.
//
// WHY THIS EXISTS
//   The suite was originally written against GoogleTest, which CMake pulled in
//   via FetchContent (a configure-time clone of github.com/google/googletest).
//   In sandboxed / offline / CI-restricted environments that clone fails, so
//   the whole suite was un-buildable. This framework replaces it outright: it
//   builds and runs anywhere a C++17 compiler is present.
//
// PUBLIC API
//     * TEST(suite, name) / TEST_F(fixture, name) — define test cases.
//     * rs2v::Test — fixture base class with virtual SetUp()/TearDown().
//     * EXPECT_* / ASSERT_* — EQ/NE/TRUE/FALSE/LT/LE/GT/GE/NEAR/FLOAT_EQ/
//       DOUBLE_EQ/STREQ/STRNE/THROW/NO_THROW/ANY_THROW.
//     * SUCCEED / FAIL / ADD_FAILURE / SKIP_TEST — all streamable with `<< msg`.
//     * RS2V_TEST_MAIN() — emits the program entry point. Supports
//       `--filter=<substr>` (run matching "Suite.Name") and `--list`.
//
//   EXPECT_* records a non-fatal failure and continues; ASSERT_* records a fatal
//   failure and returns from the current test (by textually inserting `return`,
//   so ASSERT_* may only appear in functions returning void).
//
//   Mock support (MOCK_METHOD / EXPECT_CALL / ...) lives in TestMock.h, which
//   includes this header.
//
//   For ease of migration a few well-known GoogleTest spellings remain as thin
//   aliases (e.g. GTEST_SKIP -> SKIP_TEST); the implementation underneath is
//   entirely native.

#ifndef RS2V_TEST_FRAMEWORK_H
#define RS2V_TEST_FRAMEWORK_H

#include <cstdint>
#include <cstring>
#include <cmath>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace native {

// ---------------------------------------------------------------------------
// Value stringification — uses operator<< when available, otherwise falls back
// to a sensible representation so failure messages are still useful for enums,
// pointers, and opaque types.
// ---------------------------------------------------------------------------
template <typename, typename = void>
struct is_streamable : std::false_type {};
template <typename T>
struct is_streamable<
    T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
std::string Stringify(const T& value) {
    std::ostringstream os;
    if constexpr (is_streamable<T>::value) {
        os << value;
    } else if constexpr (std::is_enum<T>::value) {
        os << "0x" << std::hex
           << static_cast<long long>(static_cast<typename std::underlying_type<T>::type>(value));
    } else {
        os << "<unprintable value (" << sizeof(T) << " bytes)>";
    }
    return os.str();
}

// Specializations to make output read like gtest's.
inline std::string Stringify(bool b) { return b ? "true" : "false"; }
inline std::string Stringify(const std::string& s) { return std::string("\"") + s + "\""; }
inline std::string Stringify(const char* s) {
    return s ? (std::string("\"") + s + "\"") : std::string("(null)");
}
inline std::string Stringify(std::nullptr_t) { return "nullptr"; }

// ---------------------------------------------------------------------------
// Result of a single assertion predicate. When it fails it carries a fully
// formatted human-readable description of what went wrong.
// ---------------------------------------------------------------------------
class AssertionResult {
public:
    static AssertionResult Pass() { return AssertionResult(true, std::string()); }
    static AssertionResult Fail(std::string message) {
        return AssertionResult(false, std::move(message));
    }
    explicit operator bool() const { return success_; }
    const std::string& message() const { return message_; }

private:
    AssertionResult(bool success, std::string message)
        : success_(success), message_(std::move(message)) {}
    bool success_;
    std::string message_;
};

// ---------------------------------------------------------------------------
// Per-test running state. A single test runs at a time on the main runner
// thread, but assertions may fire from worker threads the test spawns, so the
// failure list is mutex-guarded.
// ---------------------------------------------------------------------------
enum class FailureKind { kNonFatal, kFatal, kSkip };

class TestState {
public:
    void RecordFailure(const std::string& file, int line, const std::string& text, bool fatal) {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        if (fatal) fatal_ = true;
        std::ostringstream os;
        os << file << ":" << line << "\n" << text;
        messages_.push_back(os.str());
    }
    void RecordSkip(const std::string& reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        skipped_ = true;
        skip_reason_ = reason;
    }
    bool failed() const { return failed_; }
    bool fatal() const { return fatal_; }
    bool skipped() const { return skipped_; }
    const std::string& skip_reason() const { return skip_reason_; }
    const std::vector<std::string>& messages() const { return messages_; }

private:
    mutable std::mutex mutex_;
    bool failed_ = false;
    bool fatal_ = false;
    bool skipped_ = false;
    std::string skip_reason_;
    std::vector<std::string> messages_;
};

// The currently-running test's state. The runner sets this before a test and
// clears it after. Tests run one at a time, but a test may spawn worker threads
// that fire assertions, so this single process-wide pointer is shared across
// them (TestState itself is mutex-guarded). It is intentionally NOT thread_local.
inline TestState*& CurrentState() {
    static TestState* current = nullptr;
    return current;
}

// ---------------------------------------------------------------------------
// Streamed message collector. `EXPECT_x(...) << "context"` works because the
// macro ends in `AssertHelper(...) = Message() << ...` and Message swallows the
// chained `<<` operands.
// ---------------------------------------------------------------------------
class Message {
public:
    template <typename T>
    Message& operator<<(const T& value) {
        if constexpr (is_streamable<T>::value) {
            os_ << value;
        } else {
            os_ << Stringify(value);
        }
        return *this;
    }
    // Support manipulators like std::endl.
    Message& operator<<(std::ostream& (*manip)(std::ostream&)) {
        os_ << manip;
        return *this;
    }
    std::string str() const { return os_.str(); }

private:
    std::ostringstream os_;
};

// ---------------------------------------------------------------------------
// AssertHelper bridges the failure-description string (from the predicate) with
// any user-supplied streamed context, then records the result against the
// active test. operator= returns void so `return AssertHelper(...) = Message()`
// is valid inside a void test body (this is how ASSERT_* aborts the test).
// ---------------------------------------------------------------------------
class AssertHelper {
public:
    AssertHelper(FailureKind kind, const char* file, int line, std::string base)
        : kind_(kind), file_(file), line_(line), base_(std::move(base)) {}

    void operator=(const Message& streamed) const {
        std::string text = base_;
        std::string extra = streamed.str();
        if (!extra.empty()) {
            if (!text.empty()) text += "\n";
            text += extra;
        }
        TestState* state = CurrentState();
        if (kind_ == FailureKind::kSkip) {
            // The skip reason is just the user-streamed text, if any.
            if (state) state->RecordSkip(extra);
            return;
        }
        if (state) {
            state->RecordFailure(file_, line_, text, kind_ == FailureKind::kFatal);
        } else {
            // Assertion outside any test (e.g. in a helper run before the
            // runner) — print immediately so it is not silently lost.
            std::cerr << "[  ERROR ] " << file_ << ":" << line_ << "\n" << text << std::endl;
        }
    }

private:
    FailureKind kind_;
    const char* file_;
    int line_;
    std::string base_;
};

// ---------------------------------------------------------------------------
// Predicate helpers. Each returns an AssertionResult carrying a gtest-style
// failure description when it fails.
// ---------------------------------------------------------------------------
inline AssertionResult CheckBool(const char* expr, bool actual, bool expected) {
    if (actual == expected) return AssertionResult::Pass();
    std::ostringstream os;
    os << "Value of: " << expr << "\n  Actual: " << (actual ? "true" : "false")
       << "\nExpected: " << (expected ? "true" : "false");
    return AssertionResult::Fail(os.str());
}

template <typename A, typename B>
AssertionResult CheckEq(const char* ea, const char* eb, const A& a, const B& b) {
    if (a == b) return AssertionResult::Pass();
    std::ostringstream os;
    os << "Expected equality of these values:\n  " << ea << "\n    Which is: " << Stringify(a)
       << "\n  " << eb << "\n    Which is: " << Stringify(b);
    return AssertionResult::Fail(os.str());
}

template <typename A, typename B>
AssertionResult CheckNe(const char* ea, const char* eb, const A& a, const B& b) {
    if (!(a == b)) return AssertionResult::Pass();
    std::ostringstream os;
    os << "Expected: (" << ea << ") != (" << eb << "), actual: both equal "
       << Stringify(a);
    return AssertionResult::Fail(os.str());
}

// Generic relational comparator, parameterized by an operator tag for nice msgs.
template <typename A, typename B, typename Op>
AssertionResult CheckRel(const char* ea, const char* eb, const A& a, const B& b,
                         const char* opname, Op op) {
    if (op(a, b)) return AssertionResult::Pass();
    std::ostringstream os;
    os << "Expected: (" << ea << ") " << opname << " (" << eb << ")\n  Actual: "
       << Stringify(a) << " vs " << Stringify(b);
    return AssertionResult::Fail(os.str());
}

// C-string comparisons (STREQ/STRNE), nullptr-safe.
inline AssertionResult CheckStrEq(const char* ea, const char* eb, const char* a, const char* b) {
    bool equal = (a == nullptr && b == nullptr) ||
                 (a != nullptr && b != nullptr && std::strcmp(a, b) == 0);
    if (equal) return AssertionResult::Pass();
    std::ostringstream os;
    os << "Expected equality of these C-strings:\n  " << ea << "\n    Which is: "
       << Stringify(a) << "\n  " << eb << "\n    Which is: " << Stringify(b);
    return AssertionResult::Fail(os.str());
}
inline AssertionResult CheckStrNe(const char* ea, const char* eb, const char* a, const char* b) {
    bool equal = (a == nullptr && b == nullptr) ||
                 (a != nullptr && b != nullptr && std::strcmp(a, b) == 0);
    if (!equal) return AssertionResult::Pass();
    std::ostringstream os;
    os << "Expected: (" << ea << ") != (" << eb << "), both are " << Stringify(a);
    return AssertionResult::Fail(os.str());
}

// Near comparison with an absolute error bound.
template <typename A, typename B, typename E>
AssertionResult CheckNear(const char* ea, const char* eb, const char* ee,
                          const A& a, const B& b, const E& abs_error) {
    double diff = std::fabs(static_cast<double>(a) - static_cast<double>(b));
    if (diff <= static_cast<double>(abs_error)) return AssertionResult::Pass();
    std::ostringstream os;
    os << "The difference between " << ea << " and " << eb << " is " << diff
       << ", which exceeds " << ee << ", where\n  " << ea << " evaluates to " << Stringify(a)
       << ",\n  " << eb << " evaluates to " << Stringify(b) << ",\n  " << ee
       << " evaluates to " << Stringify(abs_error) << ".";
    return AssertionResult::Fail(os.str());
}

// ULP-based floating point equality, mirroring gtest's 4-ULP tolerance.
template <typename Float>
class FloatingPoint {
public:
    using Bits = typename std::conditional<sizeof(Float) == 4, uint32_t, uint64_t>::type;
    explicit FloatingPoint(Float x) { std::memcpy(&bits_, &x, sizeof(x)); }

    bool AlmostEquals(const FloatingPoint& other) const {
        if (IsNan() || other.IsNan()) return false;
        return UlpDistance(bits_, other.bits_) <= kMaxUlps;
    }

private:
    static constexpr size_t kBitCount = 8 * sizeof(Float);
    static constexpr Bits kSignBit = static_cast<Bits>(1) << (kBitCount - 1);
    static constexpr size_t kMaxUlps = 4;

    bool IsNan() const {
        const Bits exp_mask = ExpMask();
        const Bits frac_mask = FracMask();
        return ((bits_ & exp_mask) == exp_mask) && ((bits_ & frac_mask) != 0);
    }
    static Bits ExpMask() {
        Bits sign = kSignBit;
        Bits frac = FracMask();
        return ~(sign | frac);
    }
    static Bits FracMask() {
        return (static_cast<Bits>(1) << (kBitCount == 32 ? 23 : 52)) - 1;
    }
    static Bits SignAndMagnitudeToBiased(Bits sam) {
        if (kSignBit & sam) return ~sam + 1;  // negative
        return kSignBit | sam;                // positive
    }
    static Bits UlpDistance(Bits a, Bits b) {
        const Bits ba = SignAndMagnitudeToBiased(a);
        const Bits bb = SignAndMagnitudeToBiased(b);
        return (ba >= bb) ? (ba - bb) : (bb - ba);
    }
    Bits bits_;
};

template <typename Float>
AssertionResult CheckFloatEq(const char* ea, const char* eb, Float a, Float b) {
    if (FloatingPoint<Float>(a).AlmostEquals(FloatingPoint<Float>(b)))
        return AssertionResult::Pass();
    std::ostringstream os;
    os << "Expected equality of these values:\n  " << ea << "\n    Which is: " << a
       << "\n  " << eb << "\n    Which is: " << b;
    return AssertionResult::Fail(os.str());
}

// ---------------------------------------------------------------------------
// The Test base class. Fixtures derive from this; ::testing::Test aliases it.
// ---------------------------------------------------------------------------
class Test {
public:
    virtual ~Test() = default;
    virtual void SetUp() {}
    virtual void TearDown() {}
    virtual void TestBody() = 0;
};

// ---------------------------------------------------------------------------
// Registry of all tests, populated at static-init time by TestRegistrar.
// ---------------------------------------------------------------------------
struct TestInfo {
    const char* suite;
    const char* name;
    const char* file;
    int line;
    std::function<Test*()> factory;
};

class Registry {
public:
    static Registry& Instance() {
        static Registry instance;
        return instance;
    }
    void Add(const TestInfo& info) { tests_.push_back(info); }
    const std::vector<TestInfo>& tests() const { return tests_; }

private:
    std::vector<TestInfo> tests_;
};

class TestRegistrar {
public:
    TestRegistrar(const char* suite, const char* name, const char* file, int line,
                  std::function<Test*()> factory) {
        Registry::Instance().Add(TestInfo{suite, name, file, line, std::move(factory)});
    }
};

// ---------------------------------------------------------------------------
// The runner. Executes every registered test (optionally filtered by a
// substring of "Suite.Name"), capturing failures, and prints a concise report.
// Returns 0 if all selected tests pass (or are skipped), 1 if any fail.
// ---------------------------------------------------------------------------
inline int RunRegisteredTests(const std::string& filter) {
    const auto& all = Registry::Instance().tests();
    const char* kGreen = "\033[0;32m";
    const char* kRed = "\033[0;31m";
    const char* kYellow = "\033[0;33m";
    const char* kReset = "\033[0m";
#ifdef _WIN32
    // Avoid ANSI on plain Windows consoles.
    kGreen = kRed = kYellow = kReset = "";
#endif

    // Select tests matching the filter (empty filter = run everything).
    std::vector<const TestInfo*> tests;
    for (const auto& info : all) {
        std::string full = std::string(info.suite) + "." + info.name;
        if (filter.empty() || full.find(filter) != std::string::npos) {
            tests.push_back(&info);
        }
    }

    std::cout << kGreen << "[==========]" << kReset << " Running " << tests.size()
              << " test" << (tests.size() == 1 ? "" : "s") << ".\n";

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    std::vector<std::string> failed_names;

    for (const auto* infop : tests) {
        const TestInfo& info = *infop;
        std::string full = std::string(info.suite) + "." + info.name;
        std::cout << kGreen << "[ RUN      ]" << kReset << " " << full << std::endl;

        TestState state;
        CurrentState() = &state;

        Test* test = nullptr;
        try {
            test = info.factory();
        } catch (const std::exception& e) {
            state.RecordFailure(info.file, info.line,
                                std::string("Exception constructing fixture: ") + e.what(), true);
        } catch (...) {
            state.RecordFailure(info.file, info.line,
                                "Unknown exception constructing fixture", true);
        }

        if (test) {
            try {
                test->SetUp();
                if (!state.fatal() && !state.skipped()) {
                    test->TestBody();
                }
            } catch (const std::exception& e) {
                state.RecordFailure(info.file, info.line,
                                    std::string("Uncaught exception: ") + e.what(), true);
            } catch (...) {
                state.RecordFailure(info.file, info.line, "Uncaught unknown exception", true);
            }
            try {
                test->TearDown();
            } catch (const std::exception& e) {
                state.RecordFailure(info.file, info.line,
                                    std::string("Uncaught exception in TearDown: ") + e.what(),
                                    true);
            } catch (...) {
                state.RecordFailure(info.file, info.line,
                                    "Uncaught unknown exception in TearDown", true);
            }
            delete test;
        }

        CurrentState() = nullptr;

        for (const auto& msg : state.messages()) {
            std::cout << msg << std::endl;
        }

        if (state.failed()) {
            ++failed;
            failed_names.push_back(full);
            std::cout << kRed << "[  FAILED  ]" << kReset << " " << full << std::endl;
        } else if (state.skipped()) {
            ++skipped;
            std::cout << kYellow << "[  SKIPPED ]" << kReset << " " << full;
            if (!state.skip_reason().empty()) std::cout << " (" << state.skip_reason() << ")";
            std::cout << std::endl;
        } else {
            ++passed;
            std::cout << kGreen << "[       OK ]" << kReset << " " << full << std::endl;
        }
    }

    std::cout << kGreen << "[==========]" << kReset << " " << tests.size() << " test"
              << (tests.size() == 1 ? "" : "s") << " ran.\n";
    std::cout << kGreen << "[  PASSED  ]" << kReset << " " << passed << " test"
              << (passed == 1 ? "" : "s") << ".\n";
    if (skipped > 0) {
        std::cout << kYellow << "[  SKIPPED ]" << kReset << " " << skipped << " test"
                  << (skipped == 1 ? "" : "s") << ".\n";
    }
    if (failed > 0) {
        std::cout << kRed << "[  FAILED  ]" << kReset << " " << failed << " test"
                  << (failed == 1 ? "" : "s") << ", listed below:\n";
        for (const auto& name : failed_names) {
            std::cout << kRed << "[  FAILED  ]" << kReset << " " << name << "\n";
        }
    }
    return failed > 0 ? 1 : 0;
}

// Convenience: run everything.
inline int RunAllTests() { return RunRegisteredTests(std::string()); }

// Parse a minimal command line: `--filter=<substr>` (alias `--gtest_filter=`,
// kept only so old invocations keep working) selects tests by a substring of
// "Suite.Name"; `--list` prints the registered tests and exits.
inline int RunAllTests(int argc, char** argv) {
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";
        const char* kF = "--filter=";
        const char* kG = "--gtest_filter=";
        if (arg.rfind(kF, 0) == 0) {
            filter = arg.substr(std::strlen(kF));
        } else if (arg.rfind(kG, 0) == 0) {
            filter = arg.substr(std::strlen(kG));
        } else if (arg == "--list" || arg == "--list_tests") {
            for (const auto& info : Registry::Instance().tests()) {
                std::cout << info.suite << "." << info.name << "\n";
            }
            return 0;
        }
    }
    return RunRegisteredTests(filter);
}

}  // namespace native

// ---------------------------------------------------------------------------
// Public native test API. `rs2v::Test` is the fixture base; test sources derive
// their fixtures from it. RS2V_TEST_MAIN() emits a standard entry point.
// ---------------------------------------------------------------------------
namespace rs2v {
using Test = ::native::Test;
}  // namespace rs2v

#define RS2V_TEST_MAIN()                                   \
    int main(int argc, char** argv) {                      \
        return ::native::RunAllTests(argc, argv);          \
    }

// ---------------------------------------------------------------------------
// Core assertion plumbing.
//
// The `switch (0) case 0: default:` prefix is the classic "ambiguous else
// blocker": it makes the macro a single statement that safely consumes a
// trailing `<< "msg"` and never captures a following `else`.
// ---------------------------------------------------------------------------
#define RS2V_AMBIGUOUS_ELSE_BLOCKER_ switch (0) case 0: default:

#define RS2V_TEST_ASSERT_(assertion_result, kind)                                   \
    RS2V_AMBIGUOUS_ELSE_BLOCKER_                                                     \
    if (const ::native::AssertionResult rs2v_ar_ = (assertion_result)) {            \
        ;                                                                           \
    } else                                                                          \
        ::native::AssertHelper((kind), __FILE__, __LINE__, rs2v_ar_.message()) =    \
            ::native::Message()

// Non-fatal (EXPECT_*) records and continues. Fatal (ASSERT_*) records and the
// `return` exits the current (void) test function.
#define RS2V_EXPECT_(ar) RS2V_TEST_ASSERT_((ar), ::native::FailureKind::kNonFatal)
#define RS2V_ASSERT_(ar)                                                             \
    RS2V_AMBIGUOUS_ELSE_BLOCKER_                                                     \
    if (const ::native::AssertionResult rs2v_ar_ = (ar)) {                          \
        ;                                                                           \
    } else                                                                          \
        return ::native::AssertHelper(::native::FailureKind::kFatal, __FILE__,      \
                                      __LINE__, rs2v_ar_.message()) =               \
                   ::native::Message()

// ---- Boolean -------------------------------------------------------------
#define EXPECT_TRUE(cond)  RS2V_EXPECT_(::native::CheckBool(#cond, static_cast<bool>(cond), true))
#define EXPECT_FALSE(cond) RS2V_EXPECT_(::native::CheckBool(#cond, static_cast<bool>(cond), false))
#define ASSERT_TRUE(cond)  RS2V_ASSERT_(::native::CheckBool(#cond, static_cast<bool>(cond), true))
#define ASSERT_FALSE(cond) RS2V_ASSERT_(::native::CheckBool(#cond, static_cast<bool>(cond), false))

// ---- Equality / inequality ----------------------------------------------
#define EXPECT_EQ(a, b) RS2V_EXPECT_(::native::CheckEq(#a, #b, (a), (b)))
#define EXPECT_NE(a, b) RS2V_EXPECT_(::native::CheckNe(#a, #b, (a), (b)))
#define ASSERT_EQ(a, b) RS2V_ASSERT_(::native::CheckEq(#a, #b, (a), (b)))
#define ASSERT_NE(a, b) RS2V_ASSERT_(::native::CheckNe(#a, #b, (a), (b)))

// ---- Relational ----------------------------------------------------------
#define EXPECT_LT(a, b) RS2V_EXPECT_(::native::CheckRel(#a, #b, (a), (b), "<",  [](const auto& x, const auto& y){ return x <  y; }))
#define EXPECT_LE(a, b) RS2V_EXPECT_(::native::CheckRel(#a, #b, (a), (b), "<=", [](const auto& x, const auto& y){ return x <= y; }))
#define EXPECT_GT(a, b) RS2V_EXPECT_(::native::CheckRel(#a, #b, (a), (b), ">",  [](const auto& x, const auto& y){ return x >  y; }))
#define EXPECT_GE(a, b) RS2V_EXPECT_(::native::CheckRel(#a, #b, (a), (b), ">=", [](const auto& x, const auto& y){ return x >= y; }))
#define ASSERT_LT(a, b) RS2V_ASSERT_(::native::CheckRel(#a, #b, (a), (b), "<",  [](const auto& x, const auto& y){ return x <  y; }))
#define ASSERT_LE(a, b) RS2V_ASSERT_(::native::CheckRel(#a, #b, (a), (b), "<=", [](const auto& x, const auto& y){ return x <= y; }))
#define ASSERT_GT(a, b) RS2V_ASSERT_(::native::CheckRel(#a, #b, (a), (b), ">",  [](const auto& x, const auto& y){ return x >  y; }))
#define ASSERT_GE(a, b) RS2V_ASSERT_(::native::CheckRel(#a, #b, (a), (b), ">=", [](const auto& x, const auto& y){ return x >= y; }))

// ---- Floating point ------------------------------------------------------
#define EXPECT_NEAR(a, b, abs_err) RS2V_EXPECT_(::native::CheckNear(#a, #b, #abs_err, (a), (b), (abs_err)))
#define ASSERT_NEAR(a, b, abs_err) RS2V_ASSERT_(::native::CheckNear(#a, #b, #abs_err, (a), (b), (abs_err)))
#define EXPECT_FLOAT_EQ(a, b)  RS2V_EXPECT_(::native::CheckFloatEq<float>(#a, #b, (a), (b)))
#define EXPECT_DOUBLE_EQ(a, b) RS2V_EXPECT_(::native::CheckFloatEq<double>(#a, #b, (a), (b)))
#define ASSERT_FLOAT_EQ(a, b)  RS2V_ASSERT_(::native::CheckFloatEq<float>(#a, #b, (a), (b)))
#define ASSERT_DOUBLE_EQ(a, b) RS2V_ASSERT_(::native::CheckFloatEq<double>(#a, #b, (a), (b)))

// ---- C-strings -----------------------------------------------------------
#define EXPECT_STREQ(a, b) RS2V_EXPECT_(::native::CheckStrEq(#a, #b, (a), (b)))
#define EXPECT_STRNE(a, b) RS2V_EXPECT_(::native::CheckStrNe(#a, #b, (a), (b)))
#define ASSERT_STREQ(a, b) RS2V_ASSERT_(::native::CheckStrEq(#a, #b, (a), (b)))
#define ASSERT_STRNE(a, b) RS2V_ASSERT_(::native::CheckStrNe(#a, #b, (a), (b)))

// ---- Exceptions ----------------------------------------------------------
namespace native {
template <typename Ex, typename Fn>
AssertionResult CheckThrow(Fn&& fn, const char* stmt, const char* extype) {
    try {
        fn();
    } catch (const Ex&) {
        return AssertionResult::Pass();
    } catch (...) {
        return AssertionResult::Fail(std::string("Expected: ") + stmt + " throws " + extype +
                                     ".\n  Actual: it throws a different type.");
    }
    return AssertionResult::Fail(std::string("Expected: ") + stmt + " throws " + extype +
                                 ".\n  Actual: it throws nothing.");
}
template <typename Fn>
AssertionResult CheckNoThrow(Fn&& fn, const char* stmt) {
    try {
        fn();
    } catch (const std::exception& e) {
        return AssertionResult::Fail(std::string("Expected: ") + stmt +
                                     " doesn't throw.\n  Actual: it throws: " + e.what());
    } catch (...) {
        return AssertionResult::Fail(std::string("Expected: ") + stmt +
                                     " doesn't throw.\n  Actual: it throws an unknown type.");
    }
    return AssertionResult::Pass();
}
template <typename Fn>
AssertionResult CheckAnyThrow(Fn&& fn, const char* stmt) {
    try {
        fn();
    } catch (...) {
        return AssertionResult::Pass();
    }
    return AssertionResult::Fail(std::string("Expected: ") + stmt +
                                 " throws an exception.\n  Actual: it throws nothing.");
}
}  // namespace native

#define EXPECT_THROW(stmt, etype) RS2V_EXPECT_(::native::CheckThrow<etype>([&]() { stmt; }, #stmt, #etype))
#define ASSERT_THROW(stmt, etype) RS2V_ASSERT_(::native::CheckThrow<etype>([&]() { stmt; }, #stmt, #etype))
#define EXPECT_NO_THROW(stmt)     RS2V_EXPECT_(::native::CheckNoThrow([&]() { stmt; }, #stmt))
#define ASSERT_NO_THROW(stmt)     RS2V_ASSERT_(::native::CheckNoThrow([&]() { stmt; }, #stmt))
#define EXPECT_ANY_THROW(stmt)    RS2V_EXPECT_(::native::CheckAnyThrow([&]() { stmt; }, #stmt))
#define ASSERT_ANY_THROW(stmt)    RS2V_ASSERT_(::native::CheckAnyThrow([&]() { stmt; }, #stmt))

// ---- Unconditional outcomes ---------------------------------------------
#define SUCCEED()      RS2V_EXPECT_(::native::AssertionResult::Pass())
#define ADD_FAILURE()                                                                \
    ::native::AssertHelper(::native::FailureKind::kNonFatal, __FILE__, __LINE__,      \
                           "Failed") = ::native::Message()
#define FAIL()                                                                       \
    return ::native::AssertHelper(::native::FailureKind::kFatal, __FILE__, __LINE__,  \
                                  "Failed") = ::native::Message()
// SKIP_TEST() marks the current test skipped and returns. GTEST_SKIP is kept as
// a one-line alias only because it is such a widely-recognized spelling.
#define SKIP_TEST()                                                                  \
    return ::native::AssertHelper(::native::FailureKind::kSkip, __FILE__, __LINE__,   \
                                  "Skipped") = ::native::Message()
#define GTEST_SKIP() SKIP_TEST()

// ---------------------------------------------------------------------------
// Test definition macros.
// ---------------------------------------------------------------------------
#define RS2V_TEST_CLASS_NAME_(suite, name) suite##_##name##_Test

#define RS2V_DEFINE_TEST_(suite, name, base)                                         \
    class RS2V_TEST_CLASS_NAME_(suite, name) : public base {                         \
    public:                                                                          \
        RS2V_TEST_CLASS_NAME_(suite, name)() = default;                              \
        void TestBody() override;                                                    \
                                                                                     \
    private:                                                                         \
        static const ::native::TestRegistrar registrar_;                             \
    };                                                                               \
    const ::native::TestRegistrar RS2V_TEST_CLASS_NAME_(suite, name)::registrar_(    \
        #suite, #name, __FILE__, __LINE__,                                           \
        []() -> ::native::Test* { return new RS2V_TEST_CLASS_NAME_(suite, name)(); });\
    void RS2V_TEST_CLASS_NAME_(suite, name)::TestBody()

#define TEST(suite, name)    RS2V_DEFINE_TEST_(suite, name, ::native::Test)
#define TEST_F(fixture, name) RS2V_DEFINE_TEST_(fixture, name, fixture)

#endif  // RS2V_TEST_FRAMEWORK_H
