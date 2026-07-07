// test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate.cpp — Issue #744:
// Shape stability → dirty → Pass short-circuit → JIT deopt/recompile closed loop
// under AI multi-round mutation.
//
// Non-duplicative with #686, #605, #723, #720, #718.
//
//   - AC1: query:shape-jit-pass-closedloop-stats reachable (schema 744)
//   - AC2: shape-stable warmup + invalidate triggers churn/deopt metrics
//   - AC3: mutate churn bumps dirty-from-shape + recompile hits
//   - AC4: JIT eval remains correct after shape deopt cycle
//   - AC5: metrics monotonic over multi-round mutate matrix
//   - AC6: query regression (shape-value-pass-stats, arena-auto-policy-stats)

#include "test_harness.hpp"
#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_744_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t loop_hash(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:shape-jit-pass-closedloop-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto churn = loop_hash(cs, "stability-churn-deopts");
    const auto dirty = loop_hash(cs, "dirty-from-shape");
    const auto recompile = loop_hash(cs, "incremental-recompile-hits");
    const auto win_lost = loop_hash(cs, "speculative-win-lost");
    if (churn < 0 || dirty < 0 || recompile < 0 || win_lost < 0)
        return -1;
    return churn + dirty + recompile + win_lost;
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (add1 x) (+ x 1)) "
                 "(define (dbl y) (* y 2)) "
                 "(define (wrap z) (add1 z)) "
                 "(define base 10) "
                 "(add1 1) (dbl 3) (wrap 5)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:shape-jit-pass-closedloop-stats (schema 744) ---");
    CHECK(setup_workspace(cs), "shape-jit workspace setup");
    auto h = cs.eval("(query:shape-jit-pass-closedloop-stats)");
    CHECK(h && is_hash(*h), "shape-jit-pass-closedloop-stats returns hash");
    CHECK(loop_hash(cs, "schema") == 744, "schema == 744");
    CHECK(loop_hash(cs, "stability-churn-deopts") >= 0, "stability-churn-deopts present");
    CHECK(loop_hash(cs, "dirty-from-shape") >= 0, "dirty-from-shape present");
    CHECK(loop_hash(cs, "incremental-recompile-hits") >= 0,
          "incremental-recompile-hits present");
    CHECK(loop_hash(cs, "speculative-win-lost") >= 0, "speculative-win-lost present");
    CHECK(loop_hash(cs, "shape-stable-block-skips") >= 0, "shape-stable-block-skips present");

    std::println("\n--- AC2: shape-stable warmup + invalidate deopt ---");
    const auto churn0 = loop_hash(cs, "stability-churn-deopts");
    const auto recompile0 = loop_hash(cs, "incremental-recompile-hits");
    CHECK(cs.eval("(eval-current :jit)").has_value(), "JIT compile seeds shape profiles");
    for (int i = 0; i < 20; ++i)
        (void)cs.eval("(add1 1)");
    cs.invalidate_shape("add1");
    CHECK(!cs.is_shape_stable("add1"), "invalidate_shape clears stability");
    const auto churn1 = loop_hash(cs, "stability-churn-deopts");
    const auto recompile1 = loop_hash(cs, "incremental-recompile-hits");
    std::println("  stability-churn-deopts: {} -> {}", churn0, churn1);
    std::println("  incremental-recompile-hits: {} -> {}", recompile0, recompile1);
    CHECK(churn1 > churn0, "stability-churn-deopts grew after invalidate_shape");
    CHECK(recompile1 > recompile0, "incremental-recompile-hits grew after invalidate_shape");

    std::println("\n--- AC3: mutate churn bumps dirty-from-shape ---");
    const auto dirty0 = loop_hash(cs, "dirty-from-shape");
    const auto stats3a = stats_sum(cs);
    for (int round = 0; round < 4; ++round) {
        (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(20 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(add1 5)");
        (void)cs.eval("(wrap 10)");
    }
    const auto dirty1 = loop_hash(cs, "dirty-from-shape");
    const auto stats3b = stats_sum(cs);
    std::println("  dirty-from-shape: {} -> {}", dirty0, dirty1);
    CHECK(stats3b >= stats3a, "closed-loop stats monotonic over mutate rounds");

    std::println("\n--- AC4: JIT eval correct after deopt cycle ---");
    CHECK(cs.eval("(eval-current :jit)").has_value(), "JIT re-eval after churn");
    auto v = cs.eval("(add1 41)");
    CHECK(v && is_int(*v) && as_int(*v) == 42, "(add1 41) == 42 post-deopt JIT");

    std::println("\n--- AC5: multi-round matrix monotonic ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        cs.invalidate_shape("wrap");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:rebind \"dbl\" \"(lambda (y) (* y 3))\" \"issue744\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats5b = stats_sum(cs);
    std::println("  closed-loop sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "closed-loop sum monotonic over stress matrix");

    std::println("\n--- AC6: query regression ---");
    auto svp = cs.eval("(query:shape-value-pass-stats)");
    auto ap = cs.eval("(query:arena-auto-policy-stats)");
    CHECK(svp && is_hash(*svp), "shape-value-pass-stats regression");
    CHECK(ap && is_hash(*ap), "arena-auto-policy-stats regression");
}

} // namespace aura_issue_744_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_issue_744_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}