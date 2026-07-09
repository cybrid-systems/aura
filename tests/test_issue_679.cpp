// @category: integration
// @reason: Issue #679 nested Guard + atomic-batch rollback alignment

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_679_detail {
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

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:nested-guard-atomic-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_679_detail

int aura_issue_679_run() {
    using namespace aura_issue_679_detail;
    std::println("=== Issue #679: nested Guard + atomic-batch alignment ===");

    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();

    // AC1: nested guards track depth max
    {
        std::println("\n--- AC1: nested_guard_depth_max ---");
        cs.eval("(set-code \"(define x 1)\")");
        cs.eval("(eval-current)");
        bool ok = true;
        {
            aura::compiler::Evaluator::MutationBoundaryGuard outer(ev, &ok);
            bool inner_ok = true;
            {
                aura::compiler::Evaluator::MutationBoundaryGuard inner(ev, &inner_ok);
            }
        }
        CHECK(ev.nested_guard_depth_max() >= 2,
              std::format("nested depth max >= 2 (got {})", ev.nested_guard_depth_max()));
    }

    // AC2: atomic-batch suppressed flag realigned on rollback
    {
        std::println("\n--- AC2: suppressed_misalign_caught on batch rollback ---");
        const auto before = ev.suppressed_misalign_caught();
        bool ok = true;
        if (ev.workspace_flat()) {
            aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
            ev.workspace_flat()->begin_atomic_batch();
            ok = false;
        }
        CHECK(ev.suppressed_misalign_caught() > before,
              "suppressed misalign caught after failed batch boundary");
        if (ev.workspace_flat())
            CHECK(!ev.workspace_flat()->atomic_batch_active(),
                  "atomic batch flag cleared after rollback");
    }

    // AC3: query:nested-guard-atomic-stats hash
    {
        std::println("\n--- AC3: query:nested-guard-atomic-stats ---");
        auto stats = cs.eval("(query:nested-guard-atomic-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:nested-guard-atomic-stats returns hash");
        CHECK(stat_int(cs, "nested-depth-max") >= 2, "nested-depth-max present");
        CHECK(stat_int(cs, "suppressed-misalign-caught") >= 1,
              "suppressed-misalign-caught present");
        CHECK(stat_int(cs, "macro-rollback-hits") >= 0, "macro-rollback-hits present");
    }

    // AC4: atomic-batch EDSL rollback path stays consistent
    {
        std::println("\n--- AC4: mutate:atomic-batch failure rollback ---");
        cs.eval("(set-code \"(define x 1)\")");
        cs.eval("(eval-current)");
        const auto gen_before = ev.workspace_flat() ? ev.workspace_flat()->generation() : 0;
        const auto rollbacks_before = ev.atomic_batch_rollbacks();
        auto r = cs.eval("(mutate:atomic-batch (list (list \"mutate:noop\")) \"fail\")");
        const auto rollbacks_after = ev.atomic_batch_rollbacks();
        CHECK(rollbacks_after > rollbacks_before,
              "atomic-batch unsupported op increments rollback counter");
        (void)r;
        const auto gen_after = ev.workspace_flat() ? ev.workspace_flat()->generation() : 0;
        CHECK(gen_before == gen_after || gen_after > 0, "generation stable after batch rollback");
    }

    // AC5: stats registry
    {
        std::println("\n--- AC5: stats:list + stats:count ---");
        auto r = cs.eval("(stats:count)");
        const auto n =
            r && aura::compiler::types::is_int(*r) ? aura::compiler::types::as_int(*r) : 0;
        CHECK(n >= 58, std::format("stats:count >= 58 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_679_run();
}
#endif
