// test_compiler_root_epoch_gc_safety_post_invalidate.cpp — Issue #599:
// Automatic epoch/version root management + GC synergy for live
// IRClosure/EnvFrame post-invalidate_function (Prompt6 memory safety).
//
// Non-duplicative with #531 (closure-env-safety-stats),
// #598 (linear-ownership-runtime-stats), #682 (GC root coordination).
//
//   - AC1:  query:compiler-root-stats reachable (schema 599)
//   - AC2:  invalidate path bumps root-refresh-count
//   - AC3:  stale-closure-detected counter observable
//   - AC4:  env-version-mismatch counter observable
//   - AC5:  dangling-prevented counter observable
//   - AC6:  multi-round invalidate+mutate — stats monotonic
//   - AC7:  query regression (closure-env-safety, linear-runtime, gc-heap)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_599_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (engine:metrics \"query:compiler-root-stats\") \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto stale = hash_int(cs, "stale-closure-detected");
    const auto mismatch = hash_int(cs, "env-version-mismatch");
    const auto refresh = hash_int(cs, "root-refresh-count");
    const auto dangling = hash_int(cs, "dangling-prevented");
    if (stale < 0 || mismatch < 0 || refresh < 0 || dangling < 0)
        return -1;
    return stale + mismatch + refresh + dangling;
}

static bool setup_closure_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (f x) x) (define a 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:compiler-root-stats (schema 599) ---");
    CHECK(setup_closure_workspace(cs), "closure workspace setup");
    auto h = cs.eval("(engine:metrics \"query:compiler-root-stats\")");
    CHECK(h && is_hash(*h), "compiler-root-stats returns hash");
    CHECK(hash_int(cs, "schema") == 599, "schema == 599");
    const auto s0 = stats_sum(cs);
    std::println("  compiler-root-stats sum = {}", s0);
    CHECK(s0 >= 0, "compiler-root-stats non-negative");

    std::println("\n--- AC2: invalidate bumps root-refresh-count ---");
    const auto refresh0 = hash_int(cs, "root-refresh-count");
    for (int i = 0; i < 3; ++i) {
        (void)cs.eval("(f 42)");
    }
    (void)cs.eval("(set-code \"(define (f x) (+ x 1)) (define a 1)\")");
    (void)cs.eval("(eval-current)");
    const auto refresh1 = hash_int(cs, "root-refresh-count");
    std::println("  root-refresh-count: {} -> {}", refresh0, refresh1);
    CHECK(refresh1 > refresh0, "root-refresh-count bumped after function redefine/invalidate");

    std::println("\n--- AC3: stale-closure-detected observable ---");
    cs.evaluator().bump_compiler_root_stale_closure_detected();
    const auto stale = hash_int(cs, "stale-closure-detected");
    CHECK(stale > 0, "stale-closure-detected counter observable");

    std::println("\n--- AC4: env-version-mismatch observable ---");
    cs.evaluator().bump_envframe_version_mismatch_in_walk();
    const auto mismatch = hash_int(cs, "env-version-mismatch");
    CHECK(mismatch > 0, "env-version-mismatch counter observable");

    std::println("\n--- AC5: dangling-prevented observable ---");
    cs.evaluator().bump_compiler_root_dangling_prevented();
    const auto dangling = hash_int(cs, "dangling-prevented");
    CHECK(dangling > 0, "dangling-prevented counter observable");

    std::println("\n--- AC6: multi-round invalidate+mutate monotonic ---");
    const auto stats6a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"a\" \"" + std::to_string(100 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(f " + std::to_string(round) + ")");
    }
    const auto stats6b = stats_sum(cs);
    std::println("  compiler-root sum: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "compiler-root stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto ces = cs.eval("(engine:metrics \"query:closure-env-safety-stats\")");
    auto los = cs.eval("(engine:metrics \"query:linear-ownership-runtime-stats\")");
    auto gc = cs.eval("(gc-heap)");
    CHECK(ces && is_hash(*ces), "closure-env-safety-stats regression");
    CHECK(los && is_int(*los), "linear-ownership-runtime-stats regression");
    CHECK(gc.has_value(), "(gc-heap) regression after invalidate matrix");
}

} // namespace aura_599_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_599_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}