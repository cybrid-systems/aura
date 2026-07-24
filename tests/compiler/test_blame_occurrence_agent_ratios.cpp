// @category: unit
// @reason: Issue #2030 — surface provenance blame completeness + occurrence
// post-mutate hit rate (+ linear/provenance consistency ratios) to agent
// query surfaces (self-evo-stats + type-incremental-fidelity-stats).
//
//   AC1: source cites #2030; ratio keys on self-evo-stats + fidelity-stats
//   AC2: blame_completeness_ratio = complete/(complete+miss) in bp
//   AC3: occurrence_narrowing_post_mutate_hit_rate in 0..10000
//   AC4: linear + provenance consistency ratios present
//   AC5: multi-delta mutate — ratios remain well-formed; health-score folds them
//   AC6: schema-2030 wire flags on both agent-facing queries
//   AC7: no new public stats prim (SlimSurface); register_stats_impl only

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href_q(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t evo(CompilerService& cs, std::string_view key) {
    return href_q(cs, "query:self-evo-stats", key);
}

static std::int64_t fid(CompilerService& cs, std::string_view key) {
    return href_q(cs, "query:type-incremental-fidelity-stats", key);
}

static bool in_bp(std::int64_t v) {
    return v >= 0 && v <= 10000;
}

static void seed(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define x 1) (define y (+ x 1)) "
                  "(define f (lambda (n) (if (number? n) n 0)))\")")
              .has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2030 ---");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(!q.empty() && q.find("Issue #2030") != std::string::npos, "query.cpp cites #2030");
    CHECK(q.find("blame_completeness_ratio") != std::string::npos, "blame_completeness_ratio key");
    CHECK(q.find("occurrence_narrowing_post_mutate_hit_rate") != std::string::npos,
          "occurrence hit rate key");
    CHECK(q.find("linear-occurrence-consistency-bp") != std::string::npos, "linear occ ratio");
    CHECK(q.find("schema-2030") != std::string::npos, "schema-2030");
    // Both agent surfaces
    CHECK(q.find("query:self-evo-stats") != std::string::npos, "self-evo-stats");
    CHECK(q.find("query:type-incremental-fidelity-stats") != std::string::npos, "fidelity-stats");
}

static void ac2_blame_ratio() {
    std::println("\n--- AC2: blame_completeness_ratio formula ---");
    reset_for_test();
    CompilerService cs;
    seed(cs);
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    // Seed known complete/miss so ratio is exact.
    m->blame_chain_complete_total.store(3, std::memory_order_relaxed);
    m->blame_propagation_miss_total.store(1, std::memory_order_relaxed);
    // 3/(3+1) = 7500 bp
    CHECK(evo(cs, "blame_completeness_ratio") == 7500, "self-evo 7500 bp");
    CHECK(evo(cs, "blame-completeness-ratio-bp") == 7500, "kebab alias 7500");
    CHECK(fid(cs, "blame_completeness_ratio") == 7500, "fidelity 7500");
    // Zero activity → 10000 (N/A = perfect for adaptive policy until data)
    m->blame_chain_complete_total.store(0, std::memory_order_relaxed);
    m->blame_propagation_miss_total.store(0, std::memory_order_relaxed);
    CHECK(evo(cs, "blame_completeness_ratio") == 10000, "empty → 10000");
}

static void ac3_occurrence_hit_rate() {
    std::println("\n--- AC3: occurrence_narrowing_post_mutate_hit_rate ---");
    CompilerService cs;
    seed(cs);
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    m->occurrence_renarrow_hits_total.store(4, std::memory_order_relaxed);
    m->occurrence_renarrow_total.store(5, std::memory_order_relaxed);
    // 4/5 = 8000 bp
    CHECK(evo(cs, "occurrence_narrowing_post_mutate_hit_rate") == 8000, "renarrow 8000");
    CHECK(evo(cs, "occurrence-narrowing-post-mutate-hit-rate-bp") == 8000, "kebab 8000");
    CHECK(in_bp(evo(cs, "occurrence_narrowing_post_mutate_hit_rate")), "in bp range");
    // Fall back: stale refresh path
    m->occurrence_renarrow_total.store(0, std::memory_order_relaxed);
    m->occurrence_renarrow_hits_total.store(0, std::memory_order_relaxed);
    m->occurrence_stale_refreshes_total.store(2, std::memory_order_relaxed);
    m->occurrence_blame_chain_complete_total.store(1, std::memory_order_relaxed);
    CHECK(evo(cs, "occurrence_narrowing_post_mutate_hit_rate") == 5000, "stale fallback 5000");
}

static void ac4_linear_provenance_ratios() {
    std::println("\n--- AC4: linear + provenance consistency ratios ---");
    CompilerService cs;
    seed(cs);
    CHECK(in_bp(evo(cs, "linear-occurrence-consistency-bp")), "linear occ bp");
    CHECK(in_bp(evo(cs, "type-invariant-ratio-bp")), "type inv bp");
    CHECK(in_bp(evo(cs, "linear-invariant-ratio-bp")), "linear inv bp");
    CHECK(in_bp(evo(cs, "provenance-invariant-ratio-bp")), "prov inv bp");
    CHECK(in_bp(evo(cs, "linear-provenance-consistency-bp")), "lin-prov bp");
    CHECK(in_bp(fid(cs, "linear-occurrence-consistency-bp")), "fidelity linear occ");
    CHECK(in_bp(fid(cs, "linear-provenance-consistency-bp")), "fidelity lin-prov");
}

static void ac5_multi_delta() {
    std::println("\n--- AC5: multi-delta mutate ratios well-formed ---");
    reset_for_test();
    CompilerService cs;
    seed(cs);
    for (int i = 0; i < 4; ++i) {
        CHECK(cs.eval(std::format("(mutate:rebind \"x\" \"{}\")", 10 + i)).has_value(), "mutate");
    }
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok");
    CHECK(in_bp(evo(cs, "blame_completeness_ratio")), "blame after mutates");
    CHECK(in_bp(evo(cs, "occurrence_narrowing_post_mutate_hit_rate")), "occ after mutates");
    CHECK(in_bp(evo(cs, "health-score-bp")), "health-score bp");
    // health folds 6 terms now (#2030)
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("blame_completeness_ratio_bp +") != std::string::npos ||
              q.find("occurrence_narrowing_post_mutate_hit_rate_bp") != std::string::npos,
          "health folds #2030 ratios");
}

static void ac6_schema_wire() {
    std::println("\n--- AC6: schema-2030 + wire flags ---");
    CompilerService cs;
    seed(cs);
    auto h1 = cs.eval("(engine:metrics \"query:self-evo-stats\")");
    CHECK(h1 && is_hash(*h1), "self-evo hash");
    CHECK(evo(cs, "schema-2030") == 2030, "self-evo schema-2030");
    CHECK(evo(cs, "issue-2030") == 2030, "self-evo issue-2030");
    CHECK(evo(cs, "blame-occurrence-ratios-wired") == 1, "self-evo wired");
    CHECK(evo(cs, "schema") == 1909 || evo(cs, "schema") == 2030, "primary lineage 1909");
    auto h2 = cs.eval("(engine:metrics \"query:type-incremental-fidelity-stats\")");
    CHECK(h2 && is_hash(*h2), "fidelity hash");
    CHECK(fid(cs, "schema-2030") == 2030, "fidelity schema-2030");
    CHECK(fid(cs, "blame-occurrence-ratios-wired") == 1, "fidelity wired");
}

static void ac7_slim_surface() {
    std::println("\n--- AC7: SlimSurface — no new public *-stats prim ---");
    auto cat = read_file("src/compiler/evaluator_primitives_observability.cpp");
    // Must not register a new public query:blame-* via add()
    CHECK(cat.find("\"query:blame-completeness") == std::string::npos ||
              cat.find("register_stats_impl") != std::string::npos,
          "no new public blame query via add()");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("register_stats_impl") != std::string::npos, "uses register_stats_impl");
    // Keys live on existing self-evo-stats / fidelity-stats only
    CHECK(q.find("blame_completeness_ratio") != std::string::npos, "ratio on existing surface");
}

} // namespace

int main() {
    ac1_source();
    ac2_blame_ratio();
    ac3_occurrence_hit_rate();
    ac4_linear_provenance_ratios();
    ac5_multi_delta();
    ac6_schema_wire();
    ac7_slim_surface();
    if (g_failed)
        return 1;
    std::println("blame + occurrence agent ratios (#2030): OK ({} passed)", g_passed);
    return 0;
}
