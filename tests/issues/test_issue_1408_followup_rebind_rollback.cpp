// @category: integration
// @reason: Issue #1441 — try_rollback_rebind_op completes #1408 rebind variable-state rollback
//
// AC1: bind x=1, rebind x=100, rollback, eval x → 1
// AC2: nested rebind (x=1 → x=2 → x=3), rollback to x=1
// AC3: rebind + structural mutation in same batch, both roll back

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

namespace {

std::int64_t eval_int(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

bool setup_x(CompilerService& cs, std::string_view body = "1") {
    auto code = std::format("(define x {})", body);
    if (!cs.eval(std::format("(set-code \"{}\")", code)))
        return false;
    return cs.eval("(eval-current)").has_value();
}

// ── AC1 ──────────────────────────────────────────────────────────
void ac1_single_rebind_rollback() {
    std::println("\n--- AC1: rebind then rollback restores x=1 ---");
    CompilerService cs;
    CHECK(setup_x(cs, "1"), "setup (define x 1)");
    CHECK(eval_int(cs, "x") == 1, "initial x == 1");

    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "workspace flat present");
    const auto since_id = flat->next_mutation_id();

    auto r = cs.eval("(mutate:rebind \"x\" \"100\")");
    CHECK(r.has_value(), "mutate:rebind x→100 succeeds");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after rebind");
    CHECK(eval_int(cs, "x") == 100, "after rebind x == 100");

    // At least one rebind record with rollback data should exist.
    bool found_rebind = false;
    for (const auto& rec : flat->all_mutations()) {
        if (rec.mutation_id >= since_id && rec.operator_name == "rebind") {
            found_rebind = true;
            CHECK(rec.has_rollback_data, "rebind record has_rollback_data");
            CHECK(rec.status == aura::ast::MutationStatus::Committed, "rebind still Committed");
        }
    }
    CHECK(found_rebind, "rebind MutationRecord logged");

    // Capture Define body NodeId / int before rollback for AST-level check.
    aura::ast::NodeId define_id = aura::ast::NULL_NODE;
    aura::ast::NodeId body_before_rb = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
        auto v = flat->get(id);
        if (v.tag == aura::ast::NodeTag::Define && !v.children.empty()) {
            define_id = id;
            body_before_rb = v.child(0);
            break;
        }
    }
    CHECK(define_id != aura::ast::NULL_NODE, "found Define node");
    {
        auto body = flat->get(body_before_rb);
        CHECK(body.tag == aura::ast::NodeTag::LiteralInt && body.int_value == 100,
              "AST body is LiteralInt 100 before rollback");
    }

    const auto n = flat->rollback_since(since_id);
    CHECK(n >= 1, std::format("rollback_since restored >=1 records (got {})", n));

    // rebind record must be RolledBack (not left Committed).
    for (const auto& rec : flat->all_mutations()) {
        if (rec.mutation_id >= since_id && rec.operator_name == "rebind") {
            CHECK(rec.status == aura::ast::MutationStatus::RolledBack,
                  "rebind record status == RolledBack after try_rollback_rebind_op");
        }
    }

    // AST-level: Define body must point at original Int 1.
    {
        auto def_v = flat->get(define_id);
        CHECK(!def_v.children.empty(), "Define still has body after rollback");
        auto body = flat->get(def_v.child(0));
        CHECK(body.tag == aura::ast::NodeTag::LiteralInt, "body is LiteralInt after rollback");
        CHECK(body.int_value == 1,
              std::format("AST body int after rollback == 1 (got {})", body.int_value));
    }

    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after rollback");
    CHECK(eval_int(cs, "x") == 1, "after rollback x == 1");
}

// ── AC2 ──────────────────────────────────────────────────────────
void ac2_nested_rebind_rollback() {
    std::println("\n--- AC2: nested rebind x=1→2→3, rollback to 1 ---");
    CompilerService cs;
    CHECK(setup_x(cs, "1"), "setup (define x 1)");
    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "workspace flat");
    const auto since_id = flat->next_mutation_id();

    CHECK(cs.eval("(mutate:rebind \"x\" \"2\")").has_value(), "rebind → 2");
    CHECK(cs.eval("(mutate:rebind \"x\" \"3\")").has_value(), "rebind → 3");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    CHECK(eval_int(cs, "x") == 3, "after nested rebinds x == 3");

    const auto n = flat->rollback_since(since_id);
    CHECK(n >= 2, std::format("rollback_since undid >=2 (got {})", n));
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after nested rollback");
    CHECK(eval_int(cs, "x") == 1, "after nested rollback x == 1");
}

// ── AC3 ──────────────────────────────────────────────────────────
void ac3_rebind_plus_structural_batch() {
    std::println("\n--- AC3: rebind + structural mutation batch both roll back ---");
    CompilerService cs;
    // Two defines so we can rebind x and also do a structural op via another rebind/y path.
    CHECK(cs.eval("(set-code \"(define x 1) (define y 2)\")").has_value(), "set-code x,y");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    CHECK(eval_int(cs, "x") == 1, "x==1");
    CHECK(eval_int(cs, "y") == 2, "y==2");

    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "workspace flat");
    const auto since_id = flat->next_mutation_id();

    // Rebind x and y in one "batch" window (same since_id), plus a
    // structural-only op via mutate:replace-value on a literal if available —
    // two rebinds already produce rebind + structural-set-child pairs each.
    CHECK(cs.eval("(mutate:rebind \"x\" \"10\")").has_value(), "rebind x→10");
    CHECK(cs.eval("(mutate:rebind \"y\" \"20\")").has_value(), "rebind y→20");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after batch");
    CHECK(eval_int(cs, "x") == 10, "x==10 before rollback");
    CHECK(eval_int(cs, "y") == 20, "y==20 before rollback");

    // Count rebind + structural records in the batch.
    std::size_t rebind_n = 0, structural_n = 0;
    for (const auto& rec : flat->all_mutations()) {
        if (rec.mutation_id < since_id)
            continue;
        if (rec.operator_name == "rebind")
            ++rebind_n;
        if (rec.operator_name.starts_with("structural-"))
            ++structural_n;
    }
    CHECK(rebind_n >= 2, std::format("batch has >=2 rebind records (got {})", rebind_n));
    CHECK(structural_n >= 2,
          std::format("batch has >=2 structural records (got {})", structural_n));

    // Direct try_rollback_record on one rebind (y) — isolated unit check.
    aura::ast::MutationRecord* y_rebind = nullptr;
    for (auto it = flat->all_mutations().rbegin(); it != flat->all_mutations().rend(); ++it) {
        if (it->operator_name == "rebind" && it->status == aura::ast::MutationStatus::Committed) {
            y_rebind = &(*it);
            break;
        }
    }
    CHECK(y_rebind != nullptr, "found a committed rebind record");
    if (y_rebind) {
        auto tr = flat->try_rollback_record(*y_rebind);
        CHECK(tr.has_value(), "try_rollback_record(rebind) succeeds (#1441)");
        CHECK(y_rebind->status == aura::ast::MutationStatus::RolledBack,
              "direct try_rollback_record marks RolledBack");
    }

    // Roll back the entire batch (remaining Committed records + already-rolled y).
    (void)flat->rollback_since(since_id);
    CHECK(cs.eval("(eval-current)").has_value(), "eval after full batch rollback");
    CHECK(eval_int(cs, "x") == 1, "batch rollback x==1");
    CHECK(eval_int(cs, "y") == 2, "batch rollback y==2");
}

} // namespace

int main() {
    std::println("=== Issue #1441: try_rollback_rebind_op (#1408 follow-up) ===");
    ac1_single_rebind_rollback();
    ac2_nested_rebind_rollback();
    ac3_rebind_plus_structural_batch();
    if (::aura::test::g_failed) {
        std::println(std::cerr, "FAIL ({} failed, {} passed)", ::aura::test::g_failed,
                     ::aura::test::g_passed);
        return 1;
    }
    std::println("OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
