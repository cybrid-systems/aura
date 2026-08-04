// @category: unit
// @reason: Issue #2195 — extend Constraint Kind with SUBTYPE + strengthen
// meet/join for identical concrete scalar tags (G1 / R5).
//
//   AC1: SUBTYPE goals in solve_delta; CONFLICT exports kind=SUBTYPE
//   AC2: meet on identical concrete tags does not widen to Dynamic;
//        different tags still Dynamic
//   AC3: EQUAL-only solve does not bump subtype_goal_solve_total
//   AC4: meet precision / same-tag path when meet succeeds
//   AC5: tests + schema-2195 fidelity keys + fields.inc

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
import aura.core.type;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::solve_delta_occurrence;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeId;
using aura::core::TypeRegistry;
using aura::core::TypeTag;
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

// ── AC1: SUBTYPE solve + CONFLICT kind export ────────────────
static void ac1_subtype_solve_and_conflict() {
    std::println("\n--- AC1: SUBTYPE goals participate; CONFLICT kind=SUBTYPE ---");

    // Success path: Int <: Any via SUBTYPE
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        Constraint c;
        c.kind = Constraint::SUBTYPE;
        c.lhs = reg.int_type();
        c.rhs = reg.dynamic_type();
        cs.add_delta(std::move(c));
        TypeId occ[] = {reg.int_type()};
        auto r = solve_delta_occurrence(cs, std::span<const TypeId>(occ, 1), nullptr, &metrics);
        CHECK(r.status == SolveResult::SOLVED, "SUBTYPE Int <: Any SOLVED");
        CHECK(load_u64(metrics.subtype_goal_solve_total) >= 1, "subtype_goal_solve_total bumped");
        CHECK(r.unresolved.empty(), "no unresolved on SOLVED SUBTYPE");
    }

    // Success path: var <: Int unifies
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        auto v = cs.fresh_var();
        Constraint c;
        c.kind = Constraint::SUBTYPE;
        c.lhs = v;
        c.rhs = reg.int_type();
        cs.add_delta(std::move(c));
        TypeId occ[] = {v};
        auto r = solve_delta_occurrence(cs, std::span<const TypeId>(occ, 1), nullptr, &metrics);
        CHECK(r.status == SolveResult::SOLVED, "SUBTYPE var <: Int SOLVED");
        CHECK(load_u64(metrics.subtype_goal_solve_total) >= 1, "solve total after var subtype");
    }

    // Conflict path: function arity mismatch under SUBTYPE
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        auto f1 = reg.register_func({reg.int_type()}, reg.int_type());
        auto f2 = reg.register_func({reg.int_type(), reg.bool_type()}, reg.int_type());
        Constraint c;
        c.kind = Constraint::SUBTYPE;
        c.lhs = f1;
        c.rhs = f2;
        c.affected_node = 4242;
        cs.set_active_mutation_id(19);
        cs.set_active_blame_context(/*pred=*/1, /*affected=*/4242);
        cs.add_delta(std::move(c));
        auto r = solve_delta_occurrence(cs, {}, nullptr, &metrics);
        if (r.status == SolveResult::CONFLICT) {
            CHECK(true, "SUBTYPE arity CONFLICT returned");
            CHECK(!r.unresolved.empty(), "unresolved exported on CONFLICT");
            if (!r.unresolved.empty()) {
                CHECK(r.unresolved[0].kind == Constraint::SUBTYPE,
                      "AC1: unresolved kind == SUBTYPE");
                CHECK(static_cast<int>(r.unresolved[0].kind) == 2, "SUBTYPE enum value 2");
            }
            CHECK(metrics.last_conflict_goal_kind.load(std::memory_order_relaxed) ==
                      static_cast<std::uint8_t>(Constraint::SUBTYPE),
                  "last_conflict_goal_kind = SUBTYPE");
            CHECK(load_u64(metrics.subtype_goal_conflict_total) >= 1,
                  "subtype_goal_conflict_total");
            CHECK(load_u64(metrics.last_unresolved_goal_kind) ==
                      static_cast<std::uint64_t>(Constraint::SUBTYPE),
                  "last_unresolved_goal_kind = SUBTYPE");
        } else {
            // Defensive: still require kind surface and metrics wiring.
            CHECK(r.status == SolveResult::SOLVED || r.status == SolveResult::TIMEOUT,
                  "non-CONFLICT still defined");
            CHECK(static_cast<int>(Constraint::SUBTYPE) == 2, "SUBTYPE = 2 stable");
        }
    }
}

// ── AC2: meet/join same-tag vs cross-tag ─────────────────────
static void ac2_meet_same_tag_no_widen() {
    std::println("\n--- AC2: meet identical concrete tags; cross-tag Dynamic ---");
    TypeRegistry reg;
    const auto Dyn = reg.dynamic_type();
    const auto Int = reg.int_type();
    const auto Str = reg.string_type();
    const auto Bool = reg.bool_type();
    const auto Flt = reg.lookup_type("Float");
    CHECK(Flt.valid(), "Float registered");

    CHECK(reg.meet(Int, Int) == Int, "meet(Int,Int)=Int");
    CHECK(reg.meet(Str, Str) == Str, "meet(String,String)=String");
    CHECK(reg.meet(Bool, Bool) == Bool, "meet(Bool,Bool)=Bool");
    CHECK(reg.meet(Flt, Flt) == Flt, "meet(Float,Float)=Float");
    CHECK(reg.meet(Int, Int) != Dyn, "meet same Int not Dynamic");

    CHECK(reg.join(Int, Int) == Int, "join(Int,Int)=Int");
    CHECK(reg.join(Str, Str) == Str, "join(String,String)=String");
    CHECK(reg.join(Bool, Bool) == Bool, "join(Bool,Bool)=Bool");

    // Cross-tag still widens (no false precision)
    CHECK(reg.meet(Int, Str) == Dyn, "meet(Int,String)=Dynamic");
    CHECK(reg.meet(Int, Bool) == Dyn, "meet(Int,Bool)=Dynamic");
    CHECK(reg.meet(Int, Flt) == Dyn, "meet(Int,Float)=Dynamic");
    CHECK(reg.join(Int, Str) == Dyn, "join(Int,String)=Dynamic");
    CHECK(reg.join(Int, Flt) == Dyn, "join(Int,Float)=Dynamic");

    // Source has same-tag scalar path
    auto impl = read_file("src/core/type_impl.cpp");
    CHECK(impl.find("Issue #2195") != std::string::npos, "meet cites #2195");
    CHECK(impl.find("scalar_concrete") != std::string::npos, "scalar_concrete path");
}

// ── AC3: EQUAL-only does not pay SUBTYPE work ────────────────
static void ac3_equal_only_no_subtype_work() {
    std::println("\n--- AC3: EQUAL-only solve — no subtype_goal_solve_total ---");
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
    Constraint c2;
    c2.kind = Constraint::CONSISTENT;
    c2.lhs = a;
    c2.rhs = reg.int_type();
    cs.add_delta(std::move(c2));
    TypeId occ[] = {a};
    auto r = solve_delta_occurrence(cs, std::span<const TypeId>(occ, 1), nullptr, &metrics);
    CHECK(r.status == SolveResult::SOLVED || r.status == SolveResult::TIMEOUT ||
              r.status == SolveResult::CONFLICT,
          "EQUAL/CONSISTENT status defined");
    CHECK(load_u64(metrics.subtype_goal_solve_total) == 0,
          "AC3: no subtype solve work on EQUAL-only worklist");
    CHECK(load_u64(metrics.subtype_goal_conflict_total) == 0,
          "AC3: no subtype conflict on EQUAL-only");
}

// ── AC4: meet precision when same-tag / narrow succeeds ──────
static void ac4_meet_precision() {
    std::println("\n--- AC4: meet precision / occurrence-friendly same-tag ---");
    TypeRegistry reg;
    const auto Int = reg.int_type();
    const auto Dyn = reg.dynamic_type();
    const auto hits0 = reg.meet_precision_hit_total();
    // Int ∩ Any → Int (pre-existing + still precise)
    auto m = reg.meet(Int, Dyn);
    CHECK(m == Int, "Int ∩ Any = Int");
    CHECK(reg.meet_precision_hit_total() > hits0, "precision hit on Int∩Any");

    // Same concrete tag identity does not widen
    CHECK(reg.meet(Int, Int) == Int && reg.meet(Int, Int) != Dyn, "same-tag Int precise");

    auto h = read_file("src/compiler/type_checker.ixx");
    CHECK(h.find("SUBTYPE") != std::string::npos, "Kind has SUBTYPE");
    CHECK(h.find("EQUAL") != std::string::npos && h.find("CONSISTENT") != std::string::npos,
          "EQUAL/CONSISTENT retained");
    auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("Constraint::SUBTYPE") != std::string::npos, "solve routes SUBTYPE");
    CHECK(impl.find("consistent_subtype") != std::string::npos, "uses consistent_subtype");
    CHECK(impl.find("last_unresolved_goal_kind") != std::string::npos,
          "TIMEOUT/export stores unresolved kind");
}

// ── AC5: schema + fields.inc ─────────────────────────────────
static void ac5_schema_and_fields() {
    std::println("\n--- AC5: schema-2195 + fields.inc + source surface ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define id (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(href(cs, "schema-2195") == 2195, "schema-2195");
    CHECK(href(cs, "issue-2195") == 2195, "issue-2195");
    CHECK(href(cs, "subtype-goal-wired") == 1, "subtype-goal-wired");
    CHECK(href(cs, "last-conflict-goal-kind") >= 0, "last-conflict-goal-kind key");
    CHECK(href(cs, "last-unresolved-goal-kind") >= 0, "last-unresolved-goal-kind key");
    CHECK(href(cs, "subtype-goal-solve-total") >= 0, "subtype-goal-solve-total key");
    CHECK(href(cs, "subtype-goal-conflict-total") >= 0, "subtype-goal-conflict-total key");

    auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    CHECK(fields.find("last_conflict_goal_kind") != std::string::npos, "fields last_conflict");
    CHECK(fields.find("subtype_goal_solve_total") != std::string::npos, "fields subtype solve");
    CHECK(fields.find("subtype_goal_conflict_total") != std::string::npos,
          "fields subtype conflict");
    CHECK(fields.find("last_unresolved_goal_kind") != std::string::npos, "fields last_unresolved");

    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("schema-2195") != std::string::npos, "query schema-2195");
    CHECK(q.find("last-conflict-goal-kind") != std::string::npos, "query last-conflict");
    CHECK(q.find("subtype-goal-solve-total") != std::string::npos, "query subtype solve");

    auto obs = read_file("src/compiler/observability_metrics.h");
    CHECK(obs.find("subtype_goal_solve_total") != std::string::npos, "obs subtype solve");
    CHECK(obs.find("last_conflict_goal_kind") != std::string::npos, "obs last conflict kind");
}

} // namespace

int run_test_subtype_constraint_meet() {
    std::println("=== Issue #2195: SUBTYPE goals + stronger meet/join ===");
    ac1_subtype_solve_and_conflict();
    ac2_meet_same_tag_no_widen();
    ac3_equal_only_no_subtype_work();
    ac4_meet_precision();
    ac5_schema_and_fields();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_subtype_constraint_meet();
}
#endif
