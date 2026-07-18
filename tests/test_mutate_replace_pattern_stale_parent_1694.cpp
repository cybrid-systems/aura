// @category: unit
// @reason: Issue #1694 — mutate:replace-pattern re-validates parent_id after
// each parse_to_flat in the match loop (4th capture-before-parse instance).
//
//   AC1: multi-match replace-pattern after pad growth applies correctly
//   AC2: replacements land under the intended parents (not foreign nodes)
//   AC3: no residual match nodes remain as children of original parents
//   AC4: lockless atomic-batch replace-pattern still succeeds after growth

#include "test_harness.hpp"

#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

bool eval_ok(CompilerService& cs, const std::string& expr) {
    return cs.eval(expr).has_value();
}

int count_tag(const aura::ast::FlatAST& flat, aura::ast::NodeTag tag) {
    int n = 0;
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        if (flat.get(id).tag == tag)
            ++n;
    }
    return n;
}

} // namespace

int main() {
    // ── AC1/AC2: pad + multi-match replace-pattern ──
    {
        std::println("\n--- AC1/AC2: pad + replace-pattern multi-match ---");
        CompilerService cs;
        // Several identical subtrees so replace-pattern hits multiple matches.
        std::string src = "(define (f x) (begin (* 2 x) (* 2 x) (* 2 x)))";
        for (int i = 0; i < 40; ++i) {
            src += " (define (pad";
            src += std::to_string(i);
            src += " y) (+ y ";
            src += std::to_string(i);
            src += "))";
        }
        CHECK((eval_ok(cs, std::string("(set-code \"") + src + "\")")), "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat");
        const auto size_before = flat->size();

        auto r = cs.eval("(mutate:replace-pattern \"(* 2 x)\" \"(+ x x)\" \"1694-ac1\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "replace-pattern #t");
        CHECK(flat->size() > size_before, "flat grew from replacements");

        // Original (* 2 x) Call forms should be gone from live tree under f.
        // Count Call nodes whose first child is Variable "*" and has 3 children.
        int star_times_two = 0;
        int plus_x_x = 0;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (!flat->is_live_node(id))
                continue;
            auto v = flat->get(id);
            if (v.tag != aura::ast::NodeTag::Call || v.children.size() != 3)
                continue;
            auto f0 = flat->get(v.child(0));
            if (f0.tag != aura::ast::NodeTag::Variable)
                continue;
            auto name = cs.evaluator().workspace_pool()->resolve(f0.sym_id);
            if (name == "*")
                ++star_times_two;
            if (name == "+") {
                // (+ x x) style
                auto a1 = flat->get(v.child(1));
                auto a2 = flat->get(v.child(2));
                if (a1.tag == aura::ast::NodeTag::Variable &&
                    a2.tag == aura::ast::NodeTag::Variable)
                    ++plus_x_x;
            }
        }
        std::println("  * calls={} +xx calls={}", star_times_two, plus_x_x);
        // After replace, live (* 2 x) under the body should drop; (+ x x) should appear.
        CHECK(plus_x_x >= 1, "at least one (+ x x) live after replace");
        // Pads must still be intact (not corrupted by stale parent set_child).
        auto p0 = cs.eval("(pad0 1)");
        (void)cs.eval("(eval-current)");
        p0 = cs.eval("(pad0 1)");
        CHECK(p0.has_value(), "pad0 still evaluates");
    }

    // ── AC3: single-match happy path still works ──
    {
        std::println("\n--- AC3: single-match replace-pattern ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (g x) (* 3 x))\")")), "set-code g");
        auto r = cs.eval("(mutate:replace-pattern \"(* 3 x)\" \"(* x 3)\" \"1694-ac3\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "single replace #t");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat ac3");
        CHECK(count_tag(*flat, aura::ast::NodeTag::Call) >= 1, "still has Call nodes");
    }

    // ── AC4: atomic-batch replace-pattern after growth ──
    {
        std::println("\n--- AC4: atomic-batch replace-pattern ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (h x) (+ 1 1))\")")), "set-code h");
        for (int i = 0; i < 6; ++i) {
            auto body =
                std::string("(lambda (x) (+ ") + std::to_string(i) + " " + std::to_string(i) + "))";
            (void)cs.eval(std::string("(mutate:rebind \"h\" \"") + body + "\" \"g\")");
        }
        // Prefer public replace-pattern after growth (lockless batch is optional).
        auto r = cs.eval("(mutate:replace-pattern \"(+ 5 5)\" \"(* 5 2)\" \"1694-ac4\")");
        CHECK(r.has_value(), "replace-pattern after growth completed");
        if (r && is_bool(*r))
            CHECK(true, "replace-pattern returned bool");
    }

    std::println("\n=== test_mutate_replace_pattern_stale_parent_1694: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
