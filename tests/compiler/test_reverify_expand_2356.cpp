// @category: unit
// @reason: Issue #2356 — truncated reverify one-shot expand for
// occurrence / let-poly priority roots (between bounded reverify and
// full-solve hammer).
//
//   AC1: force_reverify_limit(8) + occurrence roots → expand runs once
//   AC2: No priority roots → expand never taken (zero cost)
//   AC3: Expand never loops; expand_total increments by at most 1 per solve
//   AC4: Production TIMEOUT escalate path unchanged (source-cite)
//   AC5: Query schema-2356 additive; source-cite expand site

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

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
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

// ── AC1: occurrence roots + low limit → expand once ──
static void ac1_expand_with_occurrence() {
    std::println("\n--- AC1: force limit 8 + occurrence → expand once ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.force_reverify_limit_for_test(8);

    auto shared = cs.fresh_var();
    // Occurrence-narrow mark → occurrence_priority_roots_ non-empty.
    cs.mark_touched_on_delta(shared, /*occurrence_narrow=*/true);
    // Many clean constraints on shared → reverify candidates >> 8.
    for (int i = 0; i < 40; ++i) {
        auto o = cs.fresh_var();
        Constraint c;
        c.kind = Constraint::EQUAL;
        c.lhs = shared;
        c.rhs = o;
        cs.add(c);
    }
    // Dirty delta so solve_delta runs reverify.
    Constraint d;
    d.kind = Constraint::EQUAL;
    d.lhs = cs.fresh_var();
    d.rhs = cs.fresh_var();
    d.source_mutation_id = 1;
    cs.add_delta(d);
    cs.mark_touched_on_delta(d.lhs, false);

    const auto exp0 = load_u64(metrics.delta_reverify_expand_total);
    (void)cs.solve_delta(nullptr);
    const auto exp1 = load_u64(metrics.delta_reverify_expand_total);
    CHECK(exp1 == exp0 + 1, "AC1: expand_total increments by exactly 1");
    // Either fully resolved priority residual or still truncated with expand=1.
    CHECK(cs.last_reverify_truncated() || cs.last_reverify_unscanned() == 0 || exp1 > exp0,
          "AC1: truncated or fully scanned after expand");
    cs.force_reverify_limit_for_test(0);
}

// ── AC2: no priority roots → expand not taken ──
static void ac2_no_priority_zero_cost() {
    std::println("\n--- AC2: no occurrence/let-poly → expand never taken ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.force_reverify_limit_for_test(8);

    auto shared = cs.fresh_var();
    // Plain touched only (no occurrence_narrow, no let-poly).
    cs.mark_touched_on_delta(shared, /*occurrence_narrow=*/false);
    for (int i = 0; i < 40; ++i) {
        auto o = cs.fresh_var();
        Constraint c;
        c.kind = Constraint::EQUAL;
        c.lhs = shared;
        c.rhs = o;
        cs.add(c);
    }
    Constraint d;
    d.kind = Constraint::EQUAL;
    d.lhs = cs.fresh_var();
    d.rhs = cs.fresh_var();
    d.source_mutation_id = 2;
    cs.add_delta(d);
    cs.mark_touched_on_delta(d.lhs, false);

    const auto exp0 = load_u64(metrics.delta_reverify_expand_total);
    (void)cs.solve_delta(nullptr);
    CHECK(load_u64(metrics.delta_reverify_expand_total) == exp0,
          "AC2: expand_total unchanged without priority roots");
    CHECK(cs.last_reverify_truncated(), "AC2: still truncated under low limit");
    cs.force_reverify_limit_for_test(0);
}

// ── AC3: at most one expand per solve_delta ──
static void ac3_at_most_one_expand() {
    std::println("\n--- AC3: expand never loops (one per solve_delta) ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.force_reverify_limit_for_test(4);

    auto shared = cs.fresh_var();
    cs.mark_let_poly_dirty(shared);
    cs.mark_touched_on_delta(shared, /*occurrence_narrow=*/true);
    for (int i = 0; i < 80; ++i) {
        auto o = cs.fresh_var();
        Constraint c;
        c.kind = Constraint::EQUAL;
        c.lhs = shared;
        c.rhs = o;
        cs.add(c);
    }
    Constraint d;
    d.kind = Constraint::EQUAL;
    d.lhs = cs.fresh_var();
    d.rhs = cs.fresh_var();
    d.source_mutation_id = 3;
    cs.add_delta(d);

    const auto exp0 = load_u64(metrics.delta_reverify_expand_total);
    (void)cs.solve_delta(nullptr);
    const auto delta = load_u64(metrics.delta_reverify_expand_total) - exp0;
    CHECK(delta <= 1, "AC3: at most one expand per solve_delta");
    CHECK(delta == 1, "AC3: expand ran once with priority roots");
    cs.force_reverify_limit_for_test(0);
}

// ── AC4: TIMEOUT escalate path unchanged ──
static void ac4_timeout_escalate_source() {
    std::println("\n--- AC4: production TIMEOUT escalate path source-cite ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(tci.find("escalate_if_production") != std::string::npos ||
              etc.find("escalate_if_production") != std::string::npos,
          "AC4: escalate_if_production still present");
    CHECK(tci.find("Issue #2356") != std::string::npos, "AC4: expand cites #2356");
    CHECK(tci.find("Production TIMEOUT") != std::string::npos ||
              tci.find("escalate") != std::string::npos,
          "AC4: TIMEOUT backstop documented at expand site");
}

// ── AC5: query + source-cite ──
static void ac5_query_and_source() {
    std::println("\n--- AC5: query schema-2356 + source-cite ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(svc, "schema-2356") == 2356, "AC5: schema-2356");
    CHECK(href(svc, "issue-2356") == 2356, "AC5: issue-2356");
    CHECK(href(svc, "delta-reverify-expand-wired") == 1, "AC5: wired");
    CHECK(href(svc, "delta-reverify-expand-total") >= 0, "AC5: expand-total");
    // Lineage
    CHECK(href(svc, "schema-2146") == 2146, "AC5: schema-2146 retained");

    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(tci.find("delta_reverify_expand_total") != std::string::npos,
          "AC5: expand counter bumped in reverify");
    CHECK(tci.find("expand_limit") != std::string::npos, "AC5: expand_limit formula");
    CHECK(met.find("delta_reverify_expand_total") != std::string::npos, "AC5: metrics field");
    CHECK(q.find("delta-reverify-expand-total") != std::string::npos, "AC5: query key");
    CHECK(q.find("schema-2356") != std::string::npos, "AC5: query schema");
}

} // namespace

int main() {
    std::println("=== Issue #2356: truncated reverify one-shot expand ===");
    ac5_query_and_source();
    ac4_timeout_escalate_source();
    ac2_no_priority_zero_cost();
    ac1_expand_with_occurrence();
    ac3_at_most_one_expand();
    std::println("\n=== #2356: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
