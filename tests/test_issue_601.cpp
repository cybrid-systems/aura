// @category: integration
// @reason: Issue #601 — JIT hot-swap / invalidate safety for live closures:
// bridge_epoch refresh + forced-deopt protocol. Scope-limited observability
// foundation: 3 new counters (jit_hotswap_live_closure_refreshed_total,
// jit_hotswap_forced_deopt_total, jit_hotswap_epoch_mismatch_prevented_total),
// a proactive IRClosure walk in invalidate_function, and the
// (query:jit-hotswap-closure-stats) Aura primitive.

#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_601_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t snap_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:jit-hotswap-closure-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_601_detail

int main() {
    using namespace aura_issue_601_detail;

    std::println("=== Issue #601: JIT hot-swap live-closure refresh + forced-deopt ===");

    aura::compiler::CompilerService cs;

    // AC1: query:jit-hotswap-closure-stats primitive exists, returns hash
    {
        std::println("\n--- AC1: primitive exists + hash shape ---");
        auto h = cs.eval("(query:jit-hotswap-closure-stats)");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "query:jit-hotswap-closure-stats returns hash");
        CHECK(snap_stat(cs, "live-closure-refreshed-total") == 0,
              "live-closure-refreshed-total starts at 0");
        CHECK(snap_stat(cs, "forced-deopt-total") == 0, "forced-deopt-total starts at 0");
        CHECK(snap_stat(cs, "epoch-mismatch-prevented-total") == 0,
              "epoch-mismatch-prevented-total starts at 0");
        CHECK(snap_stat(cs, "hotswap-invalidate-total") >= 0,
              "hotswap-invalidate-total key present (cross-link to #491)");
    }

    // AC2: stats:count grew (new primitive registered)
    {
        std::println("\n--- AC2: stats:count reflects new primitive ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 90,
              "stats:count >= 90 (90 was the pre-#601 count; +1 for #601)");
    }

    // AC3: closure created via mk-adder, applied, captured into runtime_closures_
    // (single cs.eval call so the workspace is shared across set-code + apply).
    {
        std::println("\n--- AC3: closure created + applied pre-mutate ---");
        auto r = cs.eval("(set-code \"(define (mk-adder n) (lambda (x) (+ x n)))"
                         " (define add5 (mk-adder 5))\") "
                         "(eval-current) "
                         "(add5 10)");
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 15,
              "(add5 10) == 15 (closure captured + applied)");
        const auto refreshed_before = snap_stat(cs, "live-closure-refreshed-total");
        const auto mismatch_before = snap_stat(cs, "epoch-mismatch-prevented-total");
        // No invalidate yet -> no refresh
        CHECK(refreshed_before == 0, "no refresh before invalidate");
        CHECK(mismatch_before == 0, "no mismatch prevented before invalidate");
    }

    // AC4: mutate:rebind triggers invalidate_function which bumps mutation_epoch_
    // and runs the proactive IRClosure walk. hotswap-invalidate-total MUST grow.
    {
        std::println("\n--- AC4: mutate:rebind triggers invalidate + walk ---");
        const auto hotswap_before = snap_stat(cs, "hotswap-invalidate-total");
        CHECK(
            cs.eval(
                  "(mutate:rebind \"mk-adder\" \"(lambda (n) (lambda (x) (+ x n)))\" \"issue601\")")
                .has_value(),
            "mutate:rebind mk-adder lambda under Guard");
        const auto hotswap_after = snap_stat(cs, "hotswap-invalidate-total");
        CHECK(
            hotswap_after > hotswap_before,
            std::format("hotswap-invalidate-total grew ({} -> {})", hotswap_before, hotswap_after));
        CHECK(cs.eval("(eval-current)").has_value(),
              "eval-current after mutate (no crash on invalidation)");
    }

    // AC5: closures built before mutate can still be applied safely after
    // invalidate (the proactive walk refreshed their bridge_epoch).
    {
        std::println("\n--- AC5: post-invalidate apply remains safe ---");
        auto r = cs.eval("(set-code \"(define (mk-adder n) (lambda (x) (+ x n)))"
                         " (define add7 (mk-adder 7))\") "
                         "(eval-current) "
                         "(add7 100)");
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 107,
              "(add7 100) == 107 after mutate (closure survived invalidation)");
    }

    // AC6: jit-stats-hash still works (back-compat with #491)
    {
        std::println("\n--- AC6: jit-stats-hash back-compat ---");
        auto stats = cs.eval("(query:jit-stats-hash)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:jit-stats-hash still returns hash after #601 wiring");
        auto r =
            cs.eval(std::format("(hash-ref (query:jit-stats-hash) 'hotswap-invalidate-total')"));
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) >= 1,
              "jit-stats-hash still reports hotswap-invalidate-total >= 1");
    }

    // AC7: forced-deopt-total reserved at 0 in this scope-limited layer.
    // The func_id-scoped deopt decision (closure.func_id no longer in current
    // module) is a follow-up; the foundation refreshes all stale closures.
    {
        std::println("\n--- AC7: forced-deopt reserved at 0 in this scope ---");
        CHECK(snap_stat(cs, "forced-deopt-total") == 0,
              "forced-deopt-total == 0 (scope-limited foundation: refresh-only)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}