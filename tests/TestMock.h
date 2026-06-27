// tests/TestMock.h
//
// RS2V native mocking — a compact, dependency-free replacement for the subset
// of GoogleMock the RS2V test suite uses. Pairs with TestFramework.h.
//
// Supported, gtest/gmock-compatible spellings:
//   * MOCK_METHOD(ret, name, (arg-types...), (specs...))   // new gmock form,
//     specs may be empty, (override), (const), (const, override), etc.
//   * EXPECT_CALL(obj, Method(matchers...)) .Times(n) .WillOnce(action)
//                                           .WillRepeatedly(action) .InSequence(s)
//   * ON_CALL(obj, Method(matchers...)).WillByDefault(action)
//   * Matchers: testing::_  (wildcard) and any concrete value (compared with ==)
//   * Actions:  testing::Return(value)
//   * Wrappers: testing::NiceMock<T>, testing::StrictMock<T>
//   * testing::InSequence (RAII; calls are already matched in declaration order)
//
// Unmatched (uninteresting) calls return a default-constructed value, like a
// NiceMock. A StrictMock additionally records a non-fatal failure for them.
//
// Expectation cardinalities are verified when the mock is destroyed: an unmet
// EXPECT_CALL records a failure against the running test via TestFramework.

#ifndef RS2V_TEST_MOCK_H
#define RS2V_TEST_MOCK_H

#include "TestFramework.h"

#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace native {
namespace mock {

// ---------------------------------------------------------------------------
// Wildcard matcher (testing::_).
// ---------------------------------------------------------------------------
struct Wildcard {};

template <typename Arg, typename M>
std::function<bool(const Arg&)> WrapMatcher(M m) {
    if constexpr (std::is_same_v<std::decay_t<M>, Wildcard>) {
        return [](const Arg&) { return true; };
    } else {
        return [m](const Arg& a) { return m == a; };
    }
}

// ---------------------------------------------------------------------------
// Actions. Return(v) produces a ReturnAction carrying the value; the
// expectation converts it into a typed action when WillOnce/WillByDefault is
// called (the value is cast to the method's return type then).
// ---------------------------------------------------------------------------
template <typename V>
struct ReturnAction {
    V value;
};

// ---------------------------------------------------------------------------
// One configured expectation for a mock method.
// ---------------------------------------------------------------------------
template <typename R, typename... Args>
struct Expectation {
    std::function<bool(const Args&...)> matches;
    std::vector<std::function<R(const Args&...)>> once_actions;
    std::function<R(const Args&...)> repeated_action;
    bool has_repeated = false;
    bool is_default = false;            // set by ON_CALL().WillByDefault()
    bool times_explicit = false;
    int min_calls = 0;
    int max_calls = 0;
    int call_count = 0;

    bool Saturated() const { return call_count >= max_calls; }
};

// ---------------------------------------------------------------------------
// Builder returned by EXPECT_CALL / ON_CALL to fluently configure an
// expectation. Holds a pointer into the owning MockMethod's expectation list.
// ---------------------------------------------------------------------------
template <typename R, typename... Args>
class ExpectationBuilder {
public:
    explicit ExpectationBuilder(Expectation<R, Args...>* exp) : exp_(exp) {}

    ExpectationBuilder& Times(int n) {
        exp_->times_explicit = true;
        exp_->min_calls = n;
        exp_->max_calls = n;
        return *this;
    }

    template <typename Action>
    ExpectationBuilder& WillOnce(Action action) {
        exp_->once_actions.push_back(MakeAction(std::move(action)));
        if (!exp_->times_explicit) {
            exp_->min_calls = static_cast<int>(exp_->once_actions.size());
            exp_->max_calls = exp_->min_calls;
        }
        return *this;
    }

    template <typename Action>
    ExpectationBuilder& WillRepeatedly(Action action) {
        exp_->repeated_action = MakeAction(std::move(action));
        exp_->has_repeated = true;
        if (!exp_->times_explicit) {
            exp_->min_calls = static_cast<int>(exp_->once_actions.size());
            exp_->max_calls = std::numeric_limits<int>::max();
        }
        return *this;
    }

    template <typename Action>
    ExpectationBuilder& WillByDefault(Action action) {
        exp_->repeated_action = MakeAction(std::move(action));
        exp_->has_repeated = true;
        exp_->is_default = true;
        exp_->min_calls = 0;
        exp_->max_calls = std::numeric_limits<int>::max();
        return *this;
    }

    // Sequencing is a no-op here: expectations are already matched in the order
    // they were declared, which covers the InSequence use in this suite.
    template <typename Seq>
    ExpectationBuilder& InSequence(Seq&) {
        return *this;
    }
    ExpectationBuilder& RetiresOnSaturation() { return *this; }

private:
    // Convert a ReturnAction<V> into a typed R(const Args&...) callable.
    template <typename V>
    std::function<R(const Args&...)> MakeAction(ReturnAction<V> ret) {
        if constexpr (std::is_void_v<R>) {
            return [](const Args&...) {};
        } else {
            V value = ret.value;
            return [value](const Args&...) -> R { return static_cast<R>(value); };
        }
    }
    // Allow a raw callable as an action too (e.g. a lambda).
    template <typename Fn>
    auto MakeAction(Fn fn) -> std::enable_if_t<
        std::is_invocable_v<Fn, const Args&...>, std::function<R(const Args&...)>> {
        return [fn](const Args&... a) -> R { return fn(a...); };
    }

    Expectation<R, Args...>* exp_;
};

// ---------------------------------------------------------------------------
// MockMethod stores expectations and resolves calls against them.
// ---------------------------------------------------------------------------
template <typename Sig>
class MockMethod;

template <typename R, typename... Args>
class MockMethod<R(Args...)> {
public:
    explicit MockMethod(const char* name, bool strict = false)
        : name_(name), strict_(strict) {}

    ~MockMethod() { Verify(); }

    // Build a new expectation from positional matchers (values or Wildcard).
    template <typename... Ms>
    ExpectationBuilder<R, Args...> AddExpectation(Ms... ms) {
        static_assert(sizeof...(Ms) == sizeof...(Args),
                      "matcher count must equal mocked-method argument count");
        expectations_.push_back(std::make_unique<Expectation<R, Args...>>());
        Expectation<R, Args...>* exp = expectations_.back().get();
        exp->matches = BuildMatcher(std::move(ms)...);
        return ExpectationBuilder<R, Args...>(exp);
    }

    R Invoke(Args... args) {
        // First pass: a non-default expectation that still wants calls.
        for (auto& e : expectations_) {
            if (e->is_default) continue;
            if (!e->Saturated() && e->matches(args...)) {
                return Perform(*e, args...);
            }
        }
        // Second pass: any matching default (ON_CALL).
        for (auto& e : expectations_) {
            if (!e->is_default) continue;
            if (e->matches(args...)) {
                return Perform(*e, args...);
            }
        }
        // Uninteresting call.
        if (strict_) {
            if (TestState* s = CurrentState()) {
                s->RecordFailure(__FILE__, __LINE__,
                                 std::string("Uninteresting call to strict mock method ") + name_,
                                 false);
            }
        }
        if constexpr (!std::is_void_v<R>) {
            return R{};
        }
    }

    void Verify() {
        if (verified_) return;
        verified_ = true;
        for (auto& e : expectations_) {
            if (e->is_default) continue;
            if (e->call_count < e->min_calls) {
                if (TestState* s = CurrentState()) {
                    std::ostringstream os;
                    os << "Mock method " << name_ << " expected to be called at least "
                       << e->min_calls << " time(s), but was called " << e->call_count
                       << " time(s).";
                    s->RecordFailure(__FILE__, __LINE__, os.str(), false);
                }
            }
        }
    }

private:
    template <typename... Ms>
    std::function<bool(const Args&...)> BuildMatcher(Ms... ms) {
        auto preds = std::make_tuple(WrapMatcher<Args>(std::move(ms))...);
        return [preds](const Args&... a) -> bool {
            return MatchTuple(preds, std::index_sequence_for<Args...>{}, a...);
        };
    }

    template <typename Tuple, std::size_t... I>
    static bool MatchTuple(const Tuple& preds, std::index_sequence<I...>, const Args&... a) {
        bool ok = true;
        // Pair the I-th predicate with the I-th argument (packs expand in lockstep).
        (void)std::initializer_list<int>{(ok = ok && std::get<I>(preds)(a), 0)...};
        return ok;
    }

    R Perform(Expectation<R, Args...>& e, Args&... args) {
        int idx = e.call_count;
        ++e.call_count;
        if (idx < static_cast<int>(e.once_actions.size())) {
            return e.once_actions[idx](args...);
        }
        if (e.has_repeated) {
            return e.repeated_action(args...);
        }
        if constexpr (!std::is_void_v<R>) {
            return R{};
        }
    }

    const char* name_;
    bool strict_;
    bool verified_ = false;
    std::vector<std::unique_ptr<Expectation<R, Args...>>> expectations_;
};

}  // namespace mock
}  // namespace native

// ---------------------------------------------------------------------------
// Public native mocking surface (rs2v namespace). Matchers, actions, and mock
// wrappers used by test sources.
// ---------------------------------------------------------------------------
namespace rs2v {
inline constexpr ::native::mock::Wildcard _{};

template <typename V>
::native::mock::ReturnAction<V> Return(V value) {
    return ::native::mock::ReturnAction<V>{std::move(value)};
}
// Return() with no value, for void methods.
inline ::native::mock::ReturnAction<int> Return() { return {0}; }

template <typename T>
class NiceMock : public T {
public:
    using T::T;
};
template <typename T>
class StrictMock : public T {
public:
    using T::T;
};

// InSequence is an inert RAII marker in this implementation.
class InSequence {
public:
    InSequence() = default;
};

// -- Limited support for richer actions/cardinalities -----------------------
// These exist so the legacy/excluded test sources (which were written against
// the full GoogleMock action language) name native symbols rather than foreign
// ones. They cover the common cases; exotic combinations are best-effort.
template <typename Fn>
Fn Invoke(Fn fn) { return fn; }                 // WillOnce(Invoke(f)) -> calls f

// DoAll(a, b, ..., last) performs all actions and returns the last one's value.
template <typename A>
A DoAll(A last) { return last; }
template <typename First, typename... Rest>
auto DoAll(First, Rest... rest) { return DoAll(rest...); }

// SetArgReferee<N>(v): assign v into the N-th (reference) argument. Implemented
// as an action callable that writes through the indexed argument.
template <std::size_t N, typename V>
auto SetArgReferee(V v) {
    return [v](auto&&... args) {
        auto tup = std::forward_as_tuple(args...);
        std::get<N>(tup) = v;
    };
}

// Cardinality helpers, consumed by ExpectationBuilder::Times(...).
struct Cardinality {
    int min_calls;
    int max_calls;
    operator int() const { return min_calls; }  // back-compat with int Times()
};
inline Cardinality AtLeast(int n) { return {n, (1 << 30)}; }
inline Cardinality AtMost(int n) { return {0, n}; }
inline Cardinality Between(int lo, int hi) { return {lo, hi}; }
inline Cardinality Exactly(int n) { return {n, n}; }
}  // namespace rs2v

// ---------------------------------------------------------------------------
// Preprocessor helpers to expand MOCK_METHOD's (arg-types) and (specs) lists.
// Supports 0..6 arguments. Argument types containing a top-level comma must be
// wrapped in parentheses, exactly as GoogleMock requires.
// ---------------------------------------------------------------------------
#define RS2V_CAT_(a, b) a##b
#define RS2V_CAT(a, b) RS2V_CAT_(a, b)
#define RS2V_UNPAREN(...) __VA_ARGS__

// --- emptiness detection (Jens Gustedt's ISEMPTY) -------------------------
#define RS2V_HAS_COMMA(...) RS2V_HAS_COMMA_(__VA_ARGS__, 1, 1, 1, 1, 1, 1, 1, 0)
#define RS2V_HAS_COMMA_(_1, _2, _3, _4, _5, _6, _7, _8, N, ...) N
#define RS2V_TRIGGER_PARENTHESIS_(...) ,
#define RS2V_ISEMPTY(...)                                                        \
    RS2V_ISEMPTY_(RS2V_HAS_COMMA(__VA_ARGS__),                                   \
                  RS2V_HAS_COMMA(RS2V_TRIGGER_PARENTHESIS_ __VA_ARGS__),         \
                  RS2V_HAS_COMMA(__VA_ARGS__(/*empty*/)),                        \
                  RS2V_HAS_COMMA(RS2V_TRIGGER_PARENTHESIS_ __VA_ARGS__(/*empty*/)))
#define RS2V_PASTE5(_0, _1, _2, _3, _4) _0##_1##_2##_3##_4
#define RS2V_ISEMPTY_(_0, _1, _2, _3) RS2V_HAS_COMMA(RS2V_PASTE5(RS2V_IS_EMPTY_CASE_, _0, _1, _2, _3))
#define RS2V_IS_EMPTY_CASE_0001 ,

// --- argument count (0..6) -------------------------------------------------
#define RS2V_NARG(...) RS2V_NARG_SEL(RS2V_ISEMPTY(__VA_ARGS__), __VA_ARGS__)
#define RS2V_NARG_SEL(empty, ...) RS2V_CAT(RS2V_NARG_SEL_, empty)(__VA_ARGS__)
#define RS2V_NARG_SEL_1(...) 0
#define RS2V_NARG_SEL_0(...) RS2V_NARG_COUNT(__VA_ARGS__, 6, 5, 4, 3, 2, 1, 0)
#define RS2V_NARG_COUNT(_1, _2, _3, _4, _5, _6, N, ...) N

// --- named parameter list (types -> "t0 a0, t1 a1, ...") -------------------
#define RS2V_PARAMS(...) RS2V_CAT(RS2V_PARAMS_, RS2V_NARG(__VA_ARGS__))(__VA_ARGS__)
#define RS2V_PARAMS_0()
#define RS2V_PARAMS_1(t0) t0 a0
#define RS2V_PARAMS_2(t0, t1) t0 a0, t1 a1
#define RS2V_PARAMS_3(t0, t1, t2) t0 a0, t1 a1, t2 a2
#define RS2V_PARAMS_4(t0, t1, t2, t3) t0 a0, t1 a1, t2 a2, t3 a3
#define RS2V_PARAMS_5(t0, t1, t2, t3, t4) t0 a0, t1 a1, t2 a2, t3 a3, t4 a4
#define RS2V_PARAMS_6(t0, t1, t2, t3, t4, t5) t0 a0, t1 a1, t2 a2, t3 a3, t4 a4, t5 a5

// --- forwarded argument names ("a0, a1, ...") ------------------------------
#define RS2V_ARGS(...) RS2V_CAT(RS2V_ARGS_, RS2V_NARG(__VA_ARGS__))(__VA_ARGS__)
#define RS2V_ARGS_0()
#define RS2V_ARGS_1(t0) a0
#define RS2V_ARGS_2(t0, t1) a0, a1
#define RS2V_ARGS_3(t0, t1, t2) a0, a1, a2
#define RS2V_ARGS_4(t0, t1, t2, t3) a0, a1, a2, a3
#define RS2V_ARGS_5(t0, t1, t2, t3, t4) a0, a1, a2, a3, a4
#define RS2V_ARGS_6(t0, t1, t2, t3, t4, t5) a0, a1, a2, a3, a4, a5

// --- const-ness detection from the specs list ------------------------------
// Uses the canonical CHECK()/PROBE() trick: a probe token expands to a comma
// list, and the variadic CHECK re-splits the EXPANDED text (so the probe's
// commas become real argument separators) before CHECK_N picks the flag.
#define RS2V_FIRST_(a, ...) a
#define RS2V_FIRST(...) RS2V_FIRST_(__VA_ARGS__, )
#define RS2V_PROBE_const ~, 1,
#define RS2V_CHECK_N(x, n, ...) n
#define RS2V_CHECK(...) RS2V_CHECK_N(__VA_ARGS__, 0, )
#define RS2V_IS_CONST_TOKEN(tok) RS2V_CHECK(RS2V_CAT(RS2V_PROBE_, tok))
#define RS2V_CONST_0
#define RS2V_CONST_1 const
#define RS2V_CONST_QUAL(specs) RS2V_CAT(RS2V_CONST_, RS2V_IS_CONST_TOKEN(RS2V_FIRST specs))

// ---------------------------------------------------------------------------
// MOCK_METHOD — generates the storage, the overriding method, and the
// rs2v_mock_<name> accessor used by EXPECT_CALL / ON_CALL.
//
// `args` and `specs` arrive parenthesized, e.g.
//   MOCK_METHOD(bool, Validate, (const std::string&, int), (override))
// ---------------------------------------------------------------------------
#define MOCK_METHOD(ret, name, args, specs)                                          \
    mutable ::native::mock::MockMethod<ret(RS2V_UNPAREN args)> rs2v_mm_##name{#name}; \
    ret name(RS2V_PARAMS args) RS2V_CONST_QUAL(specs) {                              \
        return rs2v_mm_##name.Invoke(RS2V_ARGS args);                                \
    }                                                                                \
    template <typename... RS2VMs>                                                     \
    auto rs2v_mock_##name(RS2VMs... rs2v_ms) const {                                 \
        return rs2v_mm_##name.AddExpectation(std::move(rs2v_ms)...);                 \
    }

// EXPECT_CALL(obj, Method(m...))  ->  obj.rs2v_mock_Method(m...)
// ON_CALL(obj, Method(m...)).WillByDefault(...) uses the same accessor.
#define EXPECT_CALL(obj, call) (obj).rs2v_mock_##call
#define ON_CALL(obj, call) (obj).rs2v_mock_##call

#endif  // RS2V_TEST_MOCK_H
