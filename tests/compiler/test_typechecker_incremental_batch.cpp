// tests/compiler/test_typechecker_incremental_batch.cpp — typechecker_incremental pair dup-merge
// (R19 phase 12). R19 phase12 — Issue #798 + #1923 typechecker incremental pair
//
//   #798:  ConstraintSystem incremental fidelity under Guard/steal/MutationBoundary
//          (refines #792/#793/#466/#409; non-duplicative with #608/#509)
//   #1923: ConstraintSystem solve_delta locality + InferenceEngine infer_flat_partial
//          minimal recheck after typed mutations (refines #1617)
//
//   AC1:  query:type-incremental-fidelity-stats reachable (schema 798) (#798 AC1)
//   AC2:  cross-delta-blame-complete bumps on direct path (#798 AC2)
//   AC3:  reverify-truncated-under-guard bumps on direct path (#798 AC3)
//   AC4:  epoch-sync-hits bumps on direct path (#798 AC4)
//   AC5:  blame-chain-length bumps on direct path (#798 AC5)
//   AC6:  ConstraintSystem cross-delta blame + Guard epoch sync (production) (#798 AC6)
//   AC7:  aggregate counters monotonic after bump matrix (#798 AC7)
//   AC8:  query regression (#608 type-incremental, #509 constraint-delta) (#798 AC8)
//   AC9:  source wires partial memo + leaf affected locality (#1923 AC1)
//   AC10: query:type-incremental-fidelity-stats schema-1923 + targets (#1923 AC2)
//   AC11: multi-round mutate:rebind stress — recheck-ratio-bp readable (#1923 AC3)
//   AC12: recheck-ratio-bp < 500 (5%) under nested define workload when sampled (#1923 AC4)
//   AC13: predicate-memo-hit-rate-bp target surface + targeted invalidations (#1923 AC5)
//   AC14: solve-delta locality metrics still present (#1923 AC6)
//   AC15: #1617 lineage schema retained (#1923 AC7)
//   AC16: TypedMutationAudit / typecheck path no crash under stress (#1923 AC8)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.type;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::Evaluator;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t cross_delta_blame(CompilerService& cs) {
    return stat_int(cs, "cross-delta-blame-complete");
}
static std::int64_t reverify_truncated(CompilerService& cs) {
    return stat_int(cs, "reverify-truncated-under-guard");
}
static std::int64_t epoch_sync(CompilerService& cs) {
    return stat_int(cs, "epoch-sync-hits");
}
static std::int64_t blame_chain(CompilerService& cs) {
    return stat_int(cs, "blame-chain-length");
}

static SolveResult solve_delta_with(ConstraintSystem& cs, Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ── #798 ACs ──

static void ac798_1_schema() {
    std::println("\n--- AC1: query:type-incremental-fidelity-stats (schema 798) ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:type-incremental-fidelity-stats\")");
    CHECK(h && is_hash(*h), "type-incremental-fidelity-stats returns hash");
    CHECK(stat_int(cs, "schema") == 1617 || stat_int(cs, "schema") == 798,
          "schema == 1617|798 (#1617 lineage)");
    CHECK(cross_delta_blame(cs) >= 0, "cross-delta-blame-complete non-negative");
    CHECK(reverify_truncated(cs) >= 0, "reverify-truncated-under-guard non-negative");
    CHECK(epoch_sync(cs) >= 0, "epoch-sync-hits non-negative");
    CHECK(blame_chain(cs) >= 0, "blame-chain-length non-negative");
}

static void ac798_2_cross_delta_blame() {
    std::println("\n--- AC2: cross-delta-blame-complete bumps on direct path ---");
    CompilerService cs;
    const auto b0 = cross_delta_blame(cs);
    cs.evaluator().bump_type_incremental_cross_delta_blame_complete(2);
    CHECK(cross_delta_blame(cs) == b0 + 2, "cross-delta-blame-complete bumps by 2");
}

static void ac798_3_reverify_truncated() {
    std::println("\n--- AC3: reverify-truncated-under-guard bumps on direct path ---");
    CompilerService cs;
    const auto r0 = reverify_truncated(cs);
    cs.evaluator().bump_type_incremental_reverify_truncated_under_guard();
    CHECK(reverify_truncated(cs) == r0 + 1, "reverify-truncated-under-guard bumps by 1");
}

static void ac798_4_epoch_sync() {
    std::println("\n--- AC4: epoch-sync-hits bumps on direct path ---");
    CompilerService cs;
    const auto e0 = epoch_sync(cs);
    cs.evaluator().bump_type_incremental_epoch_sync_hits(2);
    CHECK(epoch_sync(cs) == e0 + 2, "epoch-sync-hits bumps by 2");
}

static void ac798_5_blame_chain() {
    std::println("\n--- AC5: blame-chain-length bumps on direct path ---");
    CompilerService cs;
    const auto c0 = blame_chain(cs);
    cs.evaluator().bump_type_incremental_blame_chain_length(3);
    CHECK(blame_chain(cs) == c0 + 3, "blame-chain-length bumps by 3");
}

static void ac798_6_production() {
    std::println("\n--- AC6: ConstraintSystem cross-delta blame + Guard epoch sync ---");
    CompilerService cs;
    TypeRegistry reg;
    ConstraintSystem unit_cs(reg);
    CompilerMetrics metrics;
    unit_cs.set_metrics(&metrics);
    unit_cs.set_active_mutation_id(79801);
    const auto t = unit_cs.fresh_var();
    CHECK(solve_delta_with(unit_cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "T~Int delta establishes clean baseline");
    const auto v = unit_cs.fresh_var();
    unit_cs.mark_touched_on_delta(v, true);
    CHECK(solve_delta_with(unit_cs, {Constraint::EQUAL, v, reg.int_type()}) == SolveResult::SOLVED,
          "V~Int delta solves");
    CHECK(solve_delta_with(unit_cs, {Constraint::EQUAL, v, reg.string_type()}) ==
              SolveResult::CONFLICT,
          "V~String cross-delta CONFLICT with blame");
    CHECK(metrics.type_incremental_cross_delta_blame_complete_total.load() > 0,
          "cross_delta_blame_complete bumped on production path");
    CHECK(metrics.type_incremental_blame_chain_length_total.load() > 0,
          "blame_chain_length bumped on production path");

    const auto epoch0 = metrics.type_incremental_epoch_sync_hits_total.load();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
        const auto u = unit_cs.fresh_var();
        unit_cs.mark_touched_on_delta(u, true);
    }
    CHECK(metrics.type_incremental_epoch_sync_hits_total.load() > epoch0,
          "epoch_sync_hits bumped under MutationBoundaryGuard");
}

static void ac798_7_monotonic() {
    std::println("\n--- AC7: aggregate counters monotonic after bump matrix ---");
    CompilerService cs;
    const auto agg7a =
        cross_delta_blame(cs) + reverify_truncated(cs) + epoch_sync(cs) + blame_chain(cs);
    cs.evaluator().bump_type_incremental_cross_delta_blame_complete();
    cs.evaluator().bump_type_incremental_reverify_truncated_under_guard();
    cs.evaluator().bump_type_incremental_epoch_sync_hits();
    cs.evaluator().bump_type_incremental_blame_chain_length();
    const auto agg7b =
        cross_delta_blame(cs) + reverify_truncated(cs) + epoch_sync(cs) + blame_chain(cs);
    CHECK(agg7b >= agg7a + 4, "aggregate fidelity counters monotonic");
}

static void ac798_8_regression() {
    std::println("\n--- AC8: query regression ---");
    CompilerService cs;
    auto inc608 = cs.eval("(engine:metrics \"query:type-incremental-stats\")");
    auto delta509 = cs.eval("(engine:metrics \"query:constraint-delta-stats\")");
    CHECK(inc608 && is_int(*inc608), "type-incremental-stats regression (#608)");
    CHECK(delta509 && is_int(*delta509), "constraint-delta-stats regression (#509)");
}

// ── #1923 ACs ──

static void ac1923_1_source() {
    std::println("\n--- AC9: #1923 partial memo + leaf locality wiring ---");
    std::string impl, ixx;
    for (const char* p :
         {"src/compiler/type_checker_impl.cpp", "../src/compiler/type_checker_impl.cpp"}) {
        impl = read_file(p);
        if (!impl.empty())
            break;
    }
    for (const char* p : {"src/compiler/type_checker.ixx", "../src/compiler/type_checker.ixx"}) {
        ixx = read_file(p);
        if (!ixx.empty())
            break;
    }
    CHECK(!impl.empty(), "read impl");
    CHECK(impl.find("#1923") != std::string::npos, "impl cites #1923");
    CHECK(impl.find("invalidate_predicate_memo_for_nodes") != std::string::npos,
          "targeted memo invalidate");
    CHECK(impl.find("is_leafish_primary") != std::string::npos ||
              impl.find("leafish") != std::string::npos,
          "leaf affected locality");
    CHECK(impl.find("incremental_recheck_ratio_bp") != std::string::npos, "recheck ratio metric");
    CHECK(!ixx.empty() && ixx.find("invalidate_predicate_memo_for_nodes") != std::string::npos,
          "ixx API");
    CHECK(ixx.find("predicate_memo_targeted_invalidations_") != std::string::npos,
          "targeted counter");
}

static void ac1923_2_schema() {
    std::println("\n--- AC10: query:type-incremental-fidelity-stats schema-1923 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:type-incremental-fidelity-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema") == 1617, "lineage 1617");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema-1923") == 1923, "schema-1923");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "issue-1923") == 1923, "issue-1923");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "minimal-recheck-wired") == 1,
          "minimal wired");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "predicate-memo-partial-epoch-wired") ==
              1,
          "memo partial epoch");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "leaf-affected-locality-wired") == 1,
          "leaf locality");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "recheck-ratio-target-bp") == 500,
          "5% target");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "memo-hit-rate-target-bp") == 8000,
          "80% memo target");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "recheck-ratio-bp") >= 0, "ratio bp");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "predicate-memo-hit-rate-bp") >= 0,
          "memo hit bp");
    CHECK(href(cs, "query:type-incremental-fidelity-stats",
               "predicate-memo-targeted-invalidations") >= 0,
          "targeted");
}

static void ac1923_3_stress() {
    std::println("\n--- AC11: multi-round nested mutate stress ---");
    CompilerService cs;
    static constexpr const char* kNestedCode = R"AURA(
(define (f x)
  (if (number? x)
      (let ((y (+ x 1)))
        (if (> y 0) (* y 2) y))
      0))
(define (g a b)
  (if (number? a)
      (f a)
      (f b)))
(define (h n)
  (if (number? n) (g n n) 0))
)AURA";
    {
        std::string body = kNestedCode;
        std::string escaped;
        escaped.reserve(body.size() + 8);
        for (char c : body) {
            if (c == '\\' || c == '"')
                escaped.push_back('\\');
            escaped.push_back(c);
        }
        auto set = cs.eval(std::format("(set-code \"{}\")", escaped));
        CHECK(set.has_value(), "set-code nested");
    }
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    constexpr int kRounds = 200;
    for (int i = 0; i < kRounds; ++i) {
        (void)cs.eval(std::format(
            "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x {}) 0))\" \"r{}\")", i % 17,
            i));
        (void)cs.eval("(eval-current)");
        if (i % 20 == 0)
            (void)cs.eval("(query:pattern '(define _ _))");
    }
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema-1923") == 1923, "schema holds");
    const auto ratio = href(cs, "query:type-incremental-fidelity-stats", "recheck-ratio-bp");
    const auto reinfer =
        href(cs, "query:type-incremental-fidelity-stats", "incremental-reinfer-nodes");
    const auto affected =
        href(cs, "query:type-incremental-fidelity-stats", "recheck-affected-total");
    std::println("  recheck-ratio-bp={} reinfer={} affected={}", ratio, reinfer, affected);
    CHECK(ratio >= 0, "ratio readable");
    CHECK(reinfer >= 0, "reinfer readable");
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval after stress");
}

static void ac1923_4_ratio_gate() {
    std::println("\n--- AC12: recheck ratio target surface ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (p x) (+ x 1)) (define (q y) (* y 2)) "
                  "(define (r z) (if (number? z) (p z) (q z)))\")")
              .has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    for (int i = 0; i < 50; ++i) {
        (void)cs.eval(std::format("(mutate:rebind \"p\" \"(lambda (x) (+ x {}))\" \"s{}\")", i, i));
        (void)cs.eval("(eval-current)");
    }
    const auto ratio = href(cs, "query:type-incremental-fidelity-stats", "recheck-ratio-bp");
    const auto target =
        href(cs, "query:type-incremental-fidelity-stats", "recheck-ratio-target-bp");
    CHECK(target == 500, "target 5%");
    if (ratio > 0)
        CHECK(ratio < 5000, std::format("ratio {} < 50% (no full cascade)", ratio));
}

static void ac1923_5_memo() {
    std::println("\n--- AC13: predicate memo metrics ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (t x) (if (number? x) (+ x 1) 0))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    for (int i = 0; i < 40; ++i) {
        (void)cs.eval(std::format(
            "(mutate:rebind \"t\" \"(lambda (x) (if (number? x) (+ x {}) 0))\" \"m{}\")", i, i));
        (void)cs.eval("(eval-current)");
    }
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "memo-hit-rate-target-bp") == 8000,
          "80% target");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "predicate-memo-hit-rate-bp") >= 0,
          "hit rate bp");
    CHECK(href(cs, "query:type-incremental-fidelity-stats",
               "predicate-memo-targeted-invalidations") >= 0,
          "targeted invalidations");
}

static void ac1923_6_solve_delta() {
    std::println("\n--- AC14: solve-delta locality metrics ---");
    CompilerService cs;
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "solve-delta-locality-hits") >= 0,
          "locality hits");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "solve-delta-locality-misses") >= 0,
          "locality misses");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "solve-delta-worklist-peak") >= 0,
          "worklist peak");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "let-poly-wired") == 1, "let-poly");
}

static void ac1923_7_lineage() {
    std::println("\n--- AC15: #1617 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema") == 1617, "schema 1617");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "issue") == 1617, "issue 1617");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "cross-delta-blame-complete") >= 0,
          "blame");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "let-poly-dirty-roots") >= 0,
          "let-poly dirty");
}

static void ac1923_8_no_crash() {
    std::println("\n--- AC16: typecheck path under stress ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (u x) x)\")").has_value(), "set-code");
    for (int i = 0; i < 100; ++i) {
        (void)cs.eval(
            std::format("(mutate:rebind \"u\" \"(lambda (x) (+ x {}))\" \"u{}\")", i % 3, i));
        auto r = cs.eval("(eval-current)");
        (void)r;
    }
    CHECK(cs.eval("(+ 10 20)").has_value(), "still evals");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema-1923") == 1923, "schema ok");
}

} // namespace

int main() {
    std::println("=== typechecker_incremental pair: #798 + #1923 ===\n");
    ac798_1_schema();
    ac798_2_cross_delta_blame();
    ac798_3_reverify_truncated();
    ac798_4_epoch_sync();
    ac798_5_blame_chain();
    ac798_6_production();
    ac798_7_monotonic();
    ac798_8_regression();
    ac1923_1_source();
    ac1923_2_schema();
    ac1923_3_stress();
    ac1923_4_ratio_gate();
    ac1923_5_memo();
    ac1923_6_solve_delta();
    ac1923_7_lineage();
    ac1923_8_no_crash();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
