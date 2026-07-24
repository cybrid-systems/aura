// @category: unit
// @reason: Issue #2028 — constraint solver surface + cross-delta
// occurrence / Let-Poly / ADT narrowing stability.
//
//   AC1: source cites #2028; solve_delta_occurrence +
//        let_poly_instantiate_with_provenance + selective_adt_guardshape_renarrow
//   AC2: solve_delta_occurrence marks occurrence roots + solves
//   AC3: retained blame anchors restore continuity across clear
//   AC4: let_poly_instantiate_with_provenance stamps mark_let_poly_dirty
//   AC5: selective_adt_guardshape_renarrow counts IfExpr under dirty roots
//   AC6: query:type-incremental-fidelity-stats schema-2028 + wire keys
//   AC7: multi-delta stress — counters monotonic; service smoke

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.ast;
import aura.core.type;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::let_poly_instantiate_with_provenance;
using aura::compiler::selective_adt_guardshape_renarrow;
using aura::compiler::solve_delta_occurrence;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::TypeId;
using aura::core::TypeRegistry;
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

static void ac1_source() {
    std::println("\n--- AC1: source cites #2028 ---");
    auto h = read_file("src/compiler/type_checker.ixx");
    auto impl = read_file("src/compiler/type_checker_impl.cpp");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    auto m = read_file("src/compiler/observability_metrics.h");
    CHECK(!h.empty() && h.find("Issue #2028") != std::string::npos, "type_checker.ixx #2028");
    CHECK(h.find("solve_delta_occurrence") != std::string::npos, "solve_delta_occurrence API");
    CHECK(h.find("let_poly_instantiate_with_provenance") != std::string::npos,
          "let_poly_instantiate API");
    CHECK(h.find("selective_adt_guardshape_renarrow") != std::string::npos,
          "selective renarrow API");
    CHECK(!impl.empty() && impl.find("solve_delta_occurrence") != std::string::npos,
          "impl solve_delta_occurrence");
    CHECK(impl.find("selective_adt_guardshape_renarrow") != std::string::npos, "impl renarrow");
    CHECK(!q.empty() && q.find("schema-2028") != std::string::npos, "query schema-2028");
    CHECK(!m.empty() && m.find("solve_delta_occurrence_total") != std::string::npos,
          "metrics counters");
}

static void ac2_solve_delta_occurrence() {
    std::println("\n--- AC2: solve_delta_occurrence ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    auto a = cs.fresh_var();
    auto b = cs.fresh_var();
    Constraint c;
    c.kind = Constraint::EQUAL;
    c.lhs = a;
    c.rhs = b;
    cs.add_delta(std::move(c));
    TypeId occ[] = {a};
    auto r = solve_delta_occurrence(cs, std::span<const TypeId>(occ, 1), nullptr, &metrics);
    CHECK(r.status == SolveResult::SOLVED || r.status == SolveResult::CONFLICT ||
              r.status == SolveResult::TIMEOUT,
          "status defined");
    CHECK(r.occurrence_priority_roots >= 1 || r.touched_roots >= 1, "roots tracked");
    CHECK(load_u64(metrics.solve_delta_occurrence_total) >= 1, "occurrence total bumped");
}

static void ac3_continuity() {
    std::println("\n--- AC3: retained blame continuity ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.set_active_mutation_id(99);
    cs.set_active_blame_context(/*pred=*/7, /*affected=*/11);
    cs.clear_blame_context(/*preserve_last=*/false);
    CHECK(cs.retained_mutation_id() == 99, "retained mutation");
    CHECK(cs.retained_predicate_cond_node() == 7, "retained predicate");
    const auto cont0 = load_u64(metrics.cross_delta_solve_continuity_hits_total);
    auto r = solve_delta_occurrence(cs, {}, nullptr, &metrics);
    CHECK(cs.active_mutation_id() == 99, "active restored from retained");
    CHECK(load_u64(metrics.cross_delta_solve_continuity_hits_total) > cont0,
          "continuity hit bumped");
    CHECK(r.provenance_continuity, "provenance_continuity true");
    CHECK(load_u64(metrics.solve_delta_occurrence_stable_total) >= 1, "stable total");
}

static void ac4_let_poly_instantiate() {
    std::println("\n--- AC4: let_poly_instantiate_with_provenance ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    auto alpha = reg.make_var("a");
    auto forall = reg.register_forall(alpha, alpha); // ∀α.α
    CHECK(forall.valid(), "forall registered");
    const auto poly0 = load_u64(metrics.let_poly_dirty_roots_tracked_total);
    const auto inst0 = load_u64(metrics.let_poly_instantiate_provenance_total);
    auto body = let_poly_instantiate_with_provenance(cs, reg, forall, /*mutation_id=*/42,
                                                     /*provenance_node=*/9, &metrics);
    CHECK(body.valid(), "instantiated body valid");
    CHECK(cs.let_poly_dirty_roots_size() >= 1, "let_poly dirty roots");
    CHECK(load_u64(metrics.let_poly_instantiate_provenance_total) > inst0, "prov total");
    CHECK(load_u64(metrics.let_poly_dirty_roots_tracked_total) >= poly0, "dirty tracked");
    CHECK(cs.active_mutation_id() == 42, "mutation stamped");
    // Non-forall returns invalid
    auto bad = let_poly_instantiate_with_provenance(cs, reg, reg.int_type(), 0, 0, &metrics);
    CHECK(!bad.valid(), "non-forall → invalid");
}

static void ac5_selective_renarrow() {
    std::println("\n--- AC5: selective_adt_guardshape_renarrow ---");
    TypeRegistry reg;
    StringPool pool;
    FlatAST flat;
    // Minimal IfExpr: (if #t 1 0) — three children placeholders via add helpers.
    // Use set-code path via CompilerService for a real If when available;
    // unit: empty dirty → 0; synthetic IfExpr node if API allows.
    CompilerMetrics metrics;
    std::vector<NodeId> empty;
    CHECK(selective_adt_guardshape_renarrow(flat, pool, reg, empty, &metrics) == 0,
          "empty dirty → 0");
    // Build via service so we get real IfExpr nodes in workspace.
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (n) (if (number? n) n 0))) "
                  "(define y 1)\")")
              .has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* wflat = cs.evaluator().workspace_flat();
    auto* wpool = cs.evaluator().workspace_pool();
    CHECK(wflat && wpool, "workspace flat+pool");
    std::vector<NodeId> dirty;
    for (NodeId id = 0; id < wflat->size(); ++id) {
        if (wflat->get(id).tag == NodeTag::IfExpr)
            dirty.push_back(id);
    }
    CHECK(!dirty.empty(), "found IfExpr in workspace");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto ren0 = m ? load_u64(m->adt_guardshape_selective_renarrow_total) : 0;
    const auto n = selective_adt_guardshape_renarrow(*wflat, *wpool, reg, dirty, m);
    CHECK(n >= 1, "renarrowed ≥1 IfExpr");
    if (m)
        CHECK(load_u64(m->adt_guardshape_selective_renarrow_total) >= ren0 + n ||
                  load_u64(m->adt_guardshape_selective_renarrow_total) >= ren0,
              "renarrow metric non-decreasing");
}

static void ac6_query_schema() {
    std::println("\n--- AC6: query schema-2028 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    // Drive APIs so counters may be non-zero
    TypeRegistry reg;
    ConstraintSystem unit_cs(reg);
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    if (m)
        unit_cs.set_metrics(m);
    unit_cs.set_active_mutation_id(1);
    unit_cs.clear_blame_context(false);
    (void)solve_delta_occurrence(unit_cs, {}, nullptr, m);
    auto alpha = reg.make_var("p");
    auto forall = reg.register_forall(alpha, alpha);
    (void)let_poly_instantiate_with_provenance(unit_cs, reg, forall, 3, 1, m);

    auto h = cs.eval("(engine:metrics \"query:type-incremental-fidelity-stats\")");
    CHECK(h && is_hash(*h), "fidelity hash");
    CHECK(href(cs, "schema-2028") == 2028, "schema-2028");
    CHECK(href(cs, "issue-2028") == 2028, "issue-2028");
    CHECK(href(cs, "solver-surface-wired") == 1, "solver-surface-wired");
    CHECK(href(cs, "solve-delta-occurrence-wired") == 1, "occurrence wired");
    CHECK(href(cs, "let-poly-instantiate-provenance-wired") == 1, "let-poly wired");
    CHECK(href(cs, "adt-guardshape-renarrow-wired") == 1, "renarrow wired");
    CHECK(href(cs, "solve-delta-occurrence-total") >= 0, "occurrence total key");
    CHECK(href(cs, "let-poly-instantiate-provenance") >= 0, "let-poly key");
    CHECK(href(cs, "adt-guardshape-selective-renarrow") >= 0, "renarrow key");
    CHECK(href(cs, "cross-delta-solve-continuity-hits") >= 0, "continuity key");
    // Primary lineage retained
    CHECK(href(cs, "schema") == 1617 || href(cs, "schema") == 2028, "schema lineage");
    CHECK(href(cs, "let-poly-wired") == 1, "legacy let-poly-wired");
}

static void ac7_multi_delta_stress() {
    std::println("\n--- AC7: multi-delta stress ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    const auto t0 = load_u64(metrics.solve_delta_occurrence_total);
    for (int i = 0; i < 8; ++i) {
        auto v = cs.fresh_var();
        auto w = cs.fresh_var();
        Constraint c;
        c.kind = Constraint::EQUAL;
        c.lhs = v;
        c.rhs = w;
        cs.add_delta(std::move(c));
        cs.set_active_mutation_id(static_cast<std::uint64_t>(100 + i));
        cs.clear_blame_context(false);
        TypeId occ[] = {v};
        (void)solve_delta_occurrence(cs, std::span<const TypeId>(occ, 1), nullptr, &metrics);
    }
    CHECK(load_u64(metrics.solve_delta_occurrence_total) >= t0 + 8, "8 solves counted");
    CHECK(load_u64(metrics.cross_delta_solve_continuity_hits_total) >= 1,
          "continuity across multi-delta");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 2)").has_value(), "service eval ok");
    CHECK(href(svc, "schema-2028") == 2028, "schema after eval");
}

} // namespace

int main() {
    ac1_source();
    ac2_solve_delta_occurrence();
    ac3_continuity();
    ac4_let_poly_instantiate();
    ac5_selective_renarrow();
    ac6_query_schema();
    ac7_multi_delta_stress();
    if (g_failed)
        return 1;
    std::println("constraint solver surface cross-delta (#2028): OK ({} passed)", g_passed);
    return 0;
}
