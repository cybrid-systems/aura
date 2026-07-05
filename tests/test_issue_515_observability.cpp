// @category: integration
// @reason: Issue #515 — consolidated-p0-production-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_515_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:consolidated-p0-production-stats) '{}')", key));
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

} // namespace aura_issue_515_detail

int main() {
    using namespace aura_issue_515_detail;

    std::println("=== Issue #515: consolidated-p0-production-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    // AC1: query:consolidated-p0-production-stats returns hash
    {
        std::println("\n--- AC1: query:consolidated-p0-production-stats ---");
        auto stats = cs.eval("(query:consolidated-p0-production-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:consolidated-p0-production-stats returns hash");
        CHECK(hash_int(cs, "checkpoint-save") >= 0, "checkpoint-save present");
        CHECK(hash_int(cs, "eda-coverage-feedback") >= 0, "eda-coverage-feedback present");
        CHECK(hash_int(cs, "soa-passes-skipped") >= 0, "soa-passes-skipped present");
        CHECK(hash_int(cs, "memory-envframe-refresh") >= 0, "memory-envframe-refresh present");
        CHECK(hash_int(cs, "orchestration-steal-attempts") >= 0,
              "orchestration-steal-attempts present");
        CHECK(hash_int(cs, "consolidated-p0-production-total") >= 0,
              "consolidated-p0-production-total present");
        CHECK(hash_int(cs, "consolidated-p0-production-recommendation") >= 0,
              "consolidated-p0-production-recommendation present");
    }

    const auto save_before = hash_int(cs, "checkpoint-save");
    const auto coverage_before = hash_int(cs, "eda-coverage-feedback");
    const auto total_before = hash_int(cs, "consolidated-p0-production-total");

    // AC2: panic-checkpoint bumps persistence pillar
    {
        std::println("\n--- AC2: panic-checkpoint persistence pillar ---");
        auto r = cs.eval("(panic-checkpoint)");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "panic-checkpoint returns #t");
        const auto save_after = hash_int(cs, "checkpoint-save");
        CHECK(save_after > save_before,
              std::format("checkpoint-save bumped ({} -> {})", save_before, save_after));
    }

    // AC3: EDA coverage feedback bumps EDA pillar
    {
        std::println("\n--- AC3: EDA coverage feedback pillar ---");
        auto cov = cs.eval("(verify:parse-coverage-feedback \"0 hole_a\n1 hole_b\n\")");
        CHECK(cov && aura::compiler::types::is_int(*cov),
              "verify:parse-coverage-feedback returns int");
        const auto coverage_after = hash_int(cs, "eda-coverage-feedback");
        CHECK(coverage_after > coverage_before,
              std::format("eda-coverage-feedback bumped ({} -> {})", coverage_before,
                          coverage_after));
    }

    // AC4: mutate cycle + orchestration regression
    {
        std::println("\n--- AC4: mutate + orchestration regression ---");
        (void)cs.eval("(mutate:rebind \"x\" \"42\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:request-gc-safepoint 15)");
        const auto total_after = hash_int(cs, "consolidated-p0-production-total");
        CHECK(total_after >= total_before,
              std::format("consolidated-p0-production-total monotonic ({} -> {})", total_before,
                          total_after));
        auto wss = cs.eval("(query:workspace-snapshot-stats)");
        CHECK(wss && aura::compiler::types::is_hash(*wss),
              "query:workspace-snapshot-stats hash regression");
        auto ros = cs.eval("(query:runtime-orchestration-stats)");
        CHECK(ros && aura::compiler::types::is_hash(*ros),
              "query:runtime-orchestration-stats hash regression");
    }

    // AC5: legacy int-sum meta trackers regression
    {
        std::println("\n--- AC5: meta tracker regression ---");
        auto cpp = cs.eval("(query:consolidated-production-priority-stats)");
        CHECK(cpp && aura::compiler::types::is_int(*cpp),
              "query:consolidated-production-priority-stats int regression");
        auto roadmap = cs.eval("(query:production-roadmap-stats)");
        CHECK(roadmap && aura::compiler::types::is_int(*roadmap),
              "query:production-roadmap-stats int regression");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 113,
              "stats:count >= 113");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}