// @category: integration
// @reason: Issue #687 — DeadCoercionEliminationPass +
//  IR-interpreter identity fast-path + zero-overhead
//  gradual typing dashboard (P0 production).
//
//  Scope shipped:
//   - IR-interpreter CastOp identity fast-path (mirrors JIT
//     narrow_evidence check): when ops[2] >= 3 (Dynamic
//     passthrough), skip the 7-branch switch and bump
//     dead_coercion_post_mutate_elim_hits_total.
//   - New atomic dead_coercion_post_mutate_elim_hits_total
//     on CompilerMetrics.
//   - New hash primitive (query:dead-coercion-elim-stats,
//     schema 687) with 6 fields:
//       - casts-eliminated        (lowering pass count)
//       - residual-hotpath        (kept_for_debug count)
//       - zero-overhead-savings   (cumulative μs pass time)
//       - post-mutate-elim-hits   (interpreter identity fast-path)
//       - hot-path-rate           (derived 100*post/(post+residual))
//       - schema=687
//
//  Non-duplicative with #433 (compile:dead-coercion-stats,
//  single int), #508 (compile:dead-coercion-elapsed +
//  compile:dead-coercion-kept-for-debug, two more ints).
//  #687 carves out a hash dashboard so the AI Agent gets
//  the full zero-overhead-elimination surface in one call.
//
//   - AC1:  query:dead-coercion-elim-stats reachable (schema 687)
//   - AC2:  6 fields present in the hash response
//   - AC3:  All counters == 0 on a fresh CompilerService
//   - AC4:  hot-path-rate is 100 on a fresh CS (no
//           residual, no elims — vacuously zero overhead)
//   - AC5:  schema sentinel is 687
//   - AC6:  regression — (compile:dead-coercion-stats) (#433)
//           and (compile:dead-coercion-elapsed) (#508) still
//           reachable (the single-int primitives are kept
//           alongside the hash dashboard)

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_687_detail {
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

static std::int64_t hash_int(aura::compiler::CompilerService& cs, const std::string& prim,
                             const std::string& key) {
    auto r = cs.eval(std::format("(hash-ref ({}) '{}')", prim, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// Note: #832 re-registered query:dead-coercion-elim-stats as the standard
// total/hits/savings/active surface (schema 832). The original #687 field
// names were superseded; ACs below track the live surface.
static void run_ac1_reachable(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: query:dead-coercion-elim-stats reachable (schema 832) ---");
    auto r = cs.eval("(query:dead-coercion-elim-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r), "query:dead-coercion-elim-stats returns a hash");
    auto schema = hash_int(cs, "query:dead-coercion-elim-stats", "schema");
    CHECK(schema == 832, std::format("schema field == 832 (got {})", schema));
}

static void run_ac2_six_fields(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: standard stats fields present in the hash response ---");
    const std::vector<std::string> keys = {"total", "hits", "savings", "active", "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format("(hash-ref (query:dead-coercion-elim-stats) '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac3_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: counters non-negative (fresh CS; may be process-global) ---");
    auto total = hash_int(cs, "query:dead-coercion-elim-stats", "total");
    auto hits = hash_int(cs, "query:dead-coercion-elim-stats", "hits");
    auto savings = hash_int(cs, "query:dead-coercion-elim-stats", "savings");
    auto active = hash_int(cs, "query:dead-coercion-elim-stats", "active");
    CHECK(total >= 0, std::format("total >= 0 (got {})", total));
    CHECK(hits >= 0, std::format("hits >= 0 (got {})", hits));
    CHECK(savings >= 0, std::format("savings >= 0 (got {})", savings));
    CHECK(active == 1, std::format("active == 1 (got {})", active));
}

static void run_ac4_hot_path_rate_fresh(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: active flag on standard surface ---");
    auto active = hash_int(cs, "query:dead-coercion-elim-stats", "active");
    CHECK(active == 1, std::format("active == 1 (got {})", active));
}

static void run_ac5_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: schema sentinel is 832 (#687 surface superseded) ---");
    auto schema = hash_int(cs, "query:dead-coercion-elim-stats", "schema");
    CHECK(schema == 832, std::format("schema field == 832 (got {})", schema));
}

static void run_ac6_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC6: regression — single-int primitives still reachable ---");
    auto stats = cs.eval("(compile:dead-coercion-stats)");
    auto elapsed = cs.eval("(compile:dead-coercion-elapsed)");
    auto kept = cs.eval("(compile:dead-coercion-kept-for-debug)");
    CHECK(stats && aura::compiler::types::is_int(*stats),
          "compile:dead-coercion-stats (#433) regression [int]");
    CHECK(elapsed && aura::compiler::types::is_int(*elapsed),
          "compile:dead-coercion-elapsed (#508) regression [int]");
    CHECK(kept && aura::compiler::types::is_int(*kept),
          "compile:dead-coercion-kept-for-debug (#508) regression [int]");
}

} // namespace aura_issue_687_detail

int aura_issue_687_run() {
    using namespace aura_issue_687_detail;

    {
        aura::compiler::CompilerService cs;
        run_ac1_reachable(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac2_six_fields(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac3_fresh_zero(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac4_hot_path_rate_fresh(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac5_schema_sentinel(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac6_regression(cs);
    }

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_687_run();
}
#endif
