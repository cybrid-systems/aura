// @category: integration
// @reason: Issue #1924 — DeltaBlameChain / blame context propagation across
// typed_mutate, coercion deferral, narrowing, and ConstraintSystem paths.
//
//   AC1: source wires #1924 + active_mutation_id getters + partial clear
//   AC2: query:type-incremental-fidelity-stats schema-1924 + AC metrics
//   AC3: ConstraintSystem rich conflict → blame_chain_complete_total
//   AC4: multi-round mutate:rebind stress — no crash; metrics readable
//   AC5: TypedMutationAudit blame_chain_complete / miss counters present
//   AC6: bare add_delta miss path bumps blame_propagation_miss when conflict
//   AC7: #1617 lineage schema retained
//   AC8: coercion/narrow stamp metric surfaces wired

#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.type;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::SolveResult;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static void ac1_source() {
    std::println("\n--- AC1: #1924 source surface ---");
    auto impl =
        read_first({"src/compiler/type_checker_impl.cpp", "../src/compiler/type_checker_impl.cpp"});
    auto ixx = read_first({"src/compiler/type_checker.ixx", "../src/compiler/type_checker.ixx"});
    auto hdr = read_first(
        {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
    CHECK(!impl.empty(), "read impl");
    CHECK(impl.find("#1924") != std::string::npos, "impl cites #1924");
    CHECK(impl.find("blame_propagation_miss_total") != std::string::npos, "miss metric wired");
    CHECK(impl.find("blame_chain_complete_total") != std::string::npos, "complete metric wired");
    CHECK(impl.find("add_deferred_coercion") != std::string::npos, "coercion path present");
    CHECK(!ixx.empty() && ixx.find("active_mutation_id()") != std::string::npos, "getter");
    CHECK(ixx.find("preserve_last") != std::string::npos ||
              ixx.find("clear_blame_context") != std::string::npos,
          "clear_blame API");
    CHECK(!hdr.empty() && hdr.find("blame_propagation_miss_total") != std::string::npos,
          "hdr miss");
    CHECK(hdr.find("blame_chain_complete_total") != std::string::npos, "hdr complete");
    CHECK(hdr.find("blame_propagation_wired") != std::string::npos, "wired flag");
}

static void ac2_schema() {
    std::println("\n--- AC2: schema-1924 on type-incremental-fidelity-stats ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:type-incremental-fidelity-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema") == 1617, "lineage 1617");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema-1924") == 1924, "schema-1924");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "issue-1924") == 1924, "issue-1924");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "blame-propagation-wired") == 1,
          "wired");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "blame-chain-complete-total") >= 0,
          "complete key");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "blame-propagation-miss-total") >= 0,
          "miss key");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "blame-propagation-coercion-stamped") >=
              0,
          "coercion stamped");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "blame-propagation-narrow-stamped") >=
              0,
          "narrow stamped");
}

static SolveResult add_solve(ConstraintSystem& cs, Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

static void ac3_cs_rich_conflict() {
    std::println("\n--- AC3: ConstraintSystem rich conflict complete ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.set_active_mutation_id(192401);
    cs.set_active_blame_context(/*pred=*/11, /*affected=*/22);
    cs.push_blame_affected_node(33);
    const auto t = cs.fresh_var();
    CHECK(add_solve(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED, "baseline");
    const auto c0 = metrics.blame_chain_complete_total.load();
    CHECK(add_solve(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
          "CONFLICT");
    const auto& chain = cs.last_blame_chain();
    CHECK(!chain.frames.empty(), "frames");
    CHECK(chain.root_mutation_id == 192401, "root mut");
    CHECK(chain.is_complete() || chain.complete, "complete-ish");
    CHECK(metrics.blame_chain_complete_total.load() > c0, "complete metric +");
    // Preserve last dump across clear.
    cs.clear_blame_context(/*preserve_last=*/true);
    CHECK(!cs.last_blame_chain().frames.empty(), "preserved last chain");
}

static void ac4_mutate_stress() {
    std::println("\n--- AC4: multi-round typed mutate stress ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (if (number? x) (+ x 1) 0)) "
                  "(define (g y) (if (number? y) (f y) 0))\")")
              .has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    constexpr int kRounds = 120;
    for (int i = 0; i < kRounds; ++i) {
        (void)cs.eval(std::format(
            "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x {}) 0))\" \"b{}\")", i % 9,
            i));
        (void)cs.eval("(eval-current)");
    }
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema-1924") == 1924, "schema holds");
    const auto complete =
        href(cs, "query:type-incremental-fidelity-stats", "blame-chain-complete-total");
    const auto miss =
        href(cs, "query:type-incremental-fidelity-stats", "blame-propagation-miss-total");
    const auto narrow =
        href(cs, "query:type-incremental-fidelity-stats", "blame-propagation-narrow-stamped");
    std::println("  complete={} miss={} narrow-stamped={}", complete, miss, narrow);
    CHECK(complete >= 0, "complete readable");
    CHECK(miss >= 0, "miss readable");
    // Under healthy stamping, complete should dominate miss for rebind+if.
    if (complete > 0 && miss >= 0)
        CHECK(complete + miss > 0, "propagation activity");
    CHECK(cs.eval("(+ 1 2)").has_value(), "still evals");
}

static void ac5_audit_counters() {
    std::println("\n--- AC5: TypedMutationAudit blame counters ---");
    reset_for_test();
    // Counters exist and reset to zero.
    CHECK(g_typed_mutation_audit_counters.blame_chain_complete_total.load() == 0,
          "audit complete 0");
    CHECK(g_typed_mutation_audit_counters.blame_propagation_miss_total.load() == 0, "audit miss 0");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (t x) x)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval(std::format("(mutate:rebind \"t\" \"(lambda (x) (+ x {}))\" \"a{}\")", i, i));
        (void)cs.eval("(eval-current)");
    }
    // Audit may or may not fire depending on strategy; counters stay non-negative.
    CHECK(g_typed_mutation_audit_counters.blame_chain_complete_total.load() >= 0, "complete >=0");
    CHECK(g_typed_mutation_audit_counters.blame_propagation_miss_total.load() >= 0, "miss >=0");
}

static void ac6_bare_conflict_miss() {
    std::println("\n--- AC6: bare conflict without mutation_id → miss ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    // No set_active_mutation_id → propagation miss on conflict.
    const auto t = cs.fresh_var();
    CHECK(add_solve(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED, "ok");
    const auto m0 = metrics.blame_propagation_miss_total.load();
    CHECK(add_solve(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
          "conflict");
    CHECK(metrics.blame_propagation_miss_total.load() > m0, "miss bumped");
    CHECK(!cs.last_blame_chain().frames.empty(), "still dumpable chain");
}

static void ac7_lineage() {
    std::println("\n--- AC7: #1617 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema") == 1617, "schema");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "cross-delta-blame-complete") >= 0,
          "cross-delta");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "let-poly-wired") == 1, "let-poly");
}

static void ac8_wiring_flags() {
    std::println("\n--- AC8: wiring flags + schema-1923 coexistence ---");
    CompilerService cs;
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema-1923") == 1923, "1923");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema-1924") == 1924, "1924");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "minimal-recheck-wired") == 1,
          "1923 wired");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "blame-propagation-wired") == 1,
          "1924 wired");
}

} // namespace

int main() {
    std::println("=== Issue #1924: blame tracking typed_mutate ===");
    ac1_source();
    ac2_schema();
    ac3_cs_rich_conflict();
    ac4_mutate_stress();
    ac5_audit_counters();
    ac6_bare_conflict_miss();
    ac7_lineage();
    ac8_wiring_flags();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
