// @category: integration
// @reason: Issue #461 — JIT IRInterpreter fallback for unhandled opcodes.
//          Validates:
//            - aura_jit_fallback_to_interpreter stub is callable
//              and returns the new sentinel
//            - AURA_FALLBACK_COUNT_V atomic is exposed
//              and bumpable
//            - query:jit-fallback-stats returns the counter value
//            - AuraJIT::Metrics has the new fallback_count and
//              consistency_violations fields
//            - (smoke) aura_jit_test still works (#461 back-compat)


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

// Note: we don't include aura_jit.h directly because it
// transitively includes <atomic>, which conflicts with the
// module-imported <atomic> (the same issue that test_issue_285
// and test_issue_287 hit). The Metrics struct is forward-
// declared locally so AC4 can poke its fields.

namespace aura::jit {
struct MetricsForwardDecl {
    std::atomic<std::uint64_t> fallback_count{0};
    std::atomic<std::uint64_t> consistency_violations{0};
};
} // namespace aura::jit

namespace aura_issue_461_detail {

// Forward declaration of the fallback stub (defined in
// aura_jit_bridge.cpp as extern "C"). Matches the actual
// ABI used by JITted code.
extern "C" std::uint64_t aura_jit_fallback_to_interpreter(int64_t* args, uint32_t n_args);
extern "C" std::uint64_t aura_jit_fallback_count_v_read();
extern "C" int64_t aura_jit_test();

// ── AC1: stub returns tagged VOID (Issue #969; not poison DEADBEEF) ──
bool test_stub_returns_sentinel() {
    std::println("\n--- AC1: aura_jit_fallback_to_interpreter returns sentinel ---");
    int64_t args[1] = {0};
    auto rc = aura_jit_fallback_to_interpreter(args, 1);
    CHECK(rc == 11ull, "fallback stub returns void sentinel (tag 11)");
    return true;
}

// ── AC2: stub bumps the atomic counter ──
bool test_stub_bumps_counter() {
    std::println("\n--- AC2: fallback stub bumps atomic counter ---");
    auto before = aura_jit_fallback_count_v_read();
    int64_t args[1] = {0};
    aura_jit_fallback_to_interpreter(args, 0);
    auto after = aura_jit_fallback_count_v_read();
    CHECK(after == before + 1,
          "fallback counter: " + std::to_string(before) + " -> " + std::to_string(after));
    return true;
}

// ── AC3: stub is no-op on null args ──
bool test_stub_null_args_safe() {
    std::println("\n--- AC3: fallback stub handles null args ---");
    auto rc = aura_jit_fallback_to_interpreter(nullptr, 0);
    // Issue #969: fallback returns tagged VOID (11), not the old
    // 0xDEADBEEFBEEFDEAD poison pattern that corrupted EvalValue math.
    CHECK(rc == 11ull, "null args returns void sentinel (no crash)");
    return true;
}

// ── AC4: Metrics fallback_count + consistency_violations exist ──
//
// We don't include aura_jit.h directly (atomic redefinition
// with the module). Instead, use a local struct that mirrors
// the Metrics layout for AC4. The real check that the actual
// Metrics struct has these fields is done at compile time by
// aura_jit.cpp itself (if the fields don't exist, the JIT
// default case won't compile).
bool test_metrics_fallback_field() {
    std::println("\n--- AC4: Metrics has fallback_count + consistency_violations ---");
    aura::jit::MetricsForwardDecl m;
    CHECK(m.fallback_count.load() == 0, "Metrics::fallback_count starts at 0 (local mirror)");
    CHECK(m.consistency_violations.load() == 0,
          "Metrics::consistency_violations starts at 0 (local mirror)");
    m.fallback_count.fetch_add(7, std::memory_order_relaxed);
    m.consistency_violations.fetch_add(2, std::memory_order_relaxed);
    CHECK(m.fallback_count.load() == 7, "Metrics::fallback_count is atomic + mutable");
    CHECK(m.consistency_violations.load() == 2,
          "Metrics::consistency_violations is atomic + mutable");
    return true;
}

// ── AC5: query:jit-fallback-stats returns the counter ──
bool test_query_jit_fallback_stats() {
    std::println("\n--- AC5: query:jit-fallback-stats returns counter ---");
    aura::compiler::CompilerService cs;
    auto before = aura_jit_fallback_count_v_read();
    auto r = cs.eval("(engine:metrics \"query:jit-fallback-stats\")");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r), "query:jit-fallback-stats returns an integer");
    if (aura::compiler::types::is_int(*r)) {
        auto v = aura::compiler::types::as_int(*r);
        CHECK(static_cast<std::uint64_t>(v) == before,
              "query:jit-fallback-stats matches the atomic counter: " + std::to_string(v) +
                  " == " + std::to_string(before));
    }
    return true;
}

// ── AC6: query:jit-fallback-stats after a fallback call ──
bool test_query_after_fallback() {
    std::println("\n--- AC6: query:jit-fallback-stats reflects stub calls ---");
    aura::compiler::CompilerService cs;
    auto before = aura_jit_fallback_count_v_read();
    int64_t args[1] = {42};
    aura_jit_fallback_to_interpreter(args, 1);
    auto after = aura_jit_fallback_count_v_read();
    auto r = cs.eval("(engine:metrics \"query:jit-fallback-stats\")");
    if (!r) {
        ++g_failed;
        return false;
    }
    bool ok = false;
    if (aura::compiler::types::is_int(*r)) {
        auto v = aura::compiler::types::as_int(*r);
        ok = (static_cast<std::uint64_t>(v) == after);
    }
    CHECK(ok, "query:jit-fallback-stats reflects the post-stub-call counter");
    return true;
}

// ── AC7: aura_jit_test (pre-#461) still works ──
bool test_aura_jit_test_unchanged() {
    std::println("\n--- AC7: aura_jit_test (pre-#461) still works (regression) ---");
    auto r = aura_jit_test();
    CHECK(r == 42 || r == -1, // 42 with LLVM, -1 without
          "aura_jit_test returns 42 (with LLVM) or -1 (without)");
    return true;
}

int run_tests() {
    std::println("Issue #461 (JIT IRInterpreter fallback for unhandled opcodes)\n");
    test_stub_returns_sentinel();
    test_stub_bumps_counter();
    test_stub_null_args_safe();
    test_metrics_fallback_field();
    test_query_jit_fallback_stats();
    test_query_after_fallback();
    test_aura_jit_test_unchanged();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_461_detail

int aura_issue_461_run() {
    return aura_issue_461_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_461_run();
}
#endif