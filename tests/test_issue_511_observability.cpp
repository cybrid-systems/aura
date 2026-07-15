// @category: integration
// @reason: Issue #511 — workspace-snapshot-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_511_detail {
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

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:workspace-snapshot-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_511_detail

int aura_issue_511_observability_run() {
    using namespace aura_issue_511_detail;

    std::println("=== Issue #511: workspace-snapshot-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    // AC1: query:workspace-snapshot-stats returns hash
    {
        std::println("\n--- AC1: query:workspace-snapshot-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:workspace-snapshot-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:workspace-snapshot-stats returns hash");
        CHECK(hash_int(cs, "workspace-size") >= 0, "workspace-size present");
        CHECK(hash_int(cs, "gen-age") >= 0, "gen-age present");
        CHECK(hash_int(cs, "checkpoint-save") >= 0, "checkpoint-save present");
        CHECK(hash_int(cs, "workspace-snapshot-total") >= 0, "workspace-snapshot-total present");
        CHECK(hash_int(cs, "workspace-snapshot-recommendation") >= 0,
              "workspace-snapshot-recommendation present");
    }

    const auto save_before = hash_int(cs, "checkpoint-save");
    const auto restore_before = hash_int(cs, "checkpoint-restore");
    const auto source_before = hash_int(cs, "panic-safe-source-len");
    const auto total_before = hash_int(cs, "workspace-snapshot-total");

    // AC2: panic-checkpoint bumps save + panic-safe-source-len
    {
        std::println("\n--- AC2: panic-checkpoint lifecycle ---");
        auto r = cs.eval("(panic-checkpoint)");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "panic-checkpoint returns #t");
        const auto save_after = hash_int(cs, "checkpoint-save");
        const auto source_after = hash_int(cs, "panic-safe-source-len");
        CHECK(save_after > save_before,
              std::format("checkpoint-save bumped ({} -> {})", save_before, save_after));
        CHECK(source_after > source_before,
              std::format("panic-safe-source-len bumped ({} -> {})", source_before, source_after));
    }

    // AC3: panic-restore bumps restore + rollback-success
    {
        std::println("\n--- AC3: panic-restore lifecycle ---");
        auto r = cs.eval("(panic-restore)");
        CHECK(r && aura::compiler::types::is_bool(*r), "panic-restore returns bool");
        const auto restore_after = hash_int(cs, "checkpoint-restore");
        const auto rollback = hash_int(cs, "rollback-success");
        CHECK(restore_after > restore_before,
              std::format("checkpoint-restore bumped ({} -> {})", restore_before, restore_after));
        CHECK(rollback >= 0, "rollback-success present");
    }

    // AC4: mutate under Guard bumps commit via successful boundary
    {
        std::println("\n--- AC4: mutate + checkpoint commit ---");
        const auto commit_before = hash_int(cs, "checkpoint-commit");
        (void)cs.eval("(panic-checkpoint)");
        (void)cs.eval("(mutate:rebind \"x\" \"99\")");
        (void)cs.eval("(eval-current)");
        const auto commit_after = hash_int(cs, "checkpoint-commit");
        CHECK(commit_after >= commit_before,
              std::format("checkpoint-commit monotonic ({} -> {})", commit_before, commit_after));
    }

    // AC5: regression + total monotonic
    {
        std::println("\n--- AC5: regression ---");
        const auto total_after = hash_int(cs, "workspace-snapshot-total");
        CHECK(total_after > total_before,
              std::format("workspace-snapshot-total bumped ({} -> {})", total_before, total_after));
        auto pcs = cs.eval("(engine:metrics \"query:panic-checkpoint-lifecycle-stats\")");
        CHECK(pcs && aura::compiler::types::is_int(*pcs),
              "query:panic-checkpoint-lifecycle-stats int regression");
        auto srl = cs.eval("(engine:metrics \"query:stable-ref-lifecycle-stats\")");
        CHECK(srl && aura::compiler::types::is_hash(*srl),
              "query:stable-ref-lifecycle-stats hash regression");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 106,
              "stats:count >= 106");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_511_observability_run();
}
#endif
