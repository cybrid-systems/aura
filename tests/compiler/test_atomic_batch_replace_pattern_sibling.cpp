// @category: unit
// @reason: Issue #2802 — replace-pattern must not allocate pattern flat/pool
// on shared Evaluator::temp_arena_ (sibling atomic-batch sub-ops can reuse
// that memory). Per-call local ASTArena + isolation metric.
//
//   AC1: public + lockless cite #2802; local ASTArena; no temp_arena_ create
//   AC2: dual replace-pattern in one atomic-batch both apply correctly
//   AC3: sequential public multi-call isolation metric bumps
//   AC4: storm (many batch dual replace-patterns) still correct
//   AC5: this suite + linter; no docs/design/2802-*; no test_issue_2802.cpp

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
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

// Window around replace-pattern must not allocate pattern via temp_arena_.
static bool window_uses_local_pat_arena(std::string_view win) {
    if (win.find("ASTArena pat_arena") == std::string_view::npos &&
        win.find("pat_arena(/*initial_size=") == std::string_view::npos)
        return false;
    // Forbid allocating pattern pool/flat from shared temp_arena_.
    if (win.find("temp_arena_->create") != std::string_view::npos)
        return false;
    if (win.find("temp_arena_->allocator") != std::string_view::npos)
        return false;
    return win.find("create<aura::ast::StringPool>") != std::string_view::npos ||
           win.find("create<StringPool>") != std::string_view::npos ||
           win.find("pat_pool") != std::string_view::npos;
}

} // namespace

int run_test_atomic_batch_replace_pattern_sibling() {
    std::println("=== Issue #2802: atomic-batch replace-pattern sibling isolation ===");
    CHECK(true, "ac2802: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: local pat_arena, not temp_arena_ ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(!mut.empty() && !flat.empty() && !ast.empty(), "AC1: sources readable");

        auto ppos = mut.find("add_mutate(\"mutate:replace-pattern\"");
        if (ppos == std::string::npos)
            ppos = mut.find("mutate:replace-pattern");
        CHECK(ppos != std::string::npos, "AC1: public replace-pattern");
        // Pattern alloc is early in the body; #2802 comment nearby.
        auto pwin = mut.substr(ppos, 12000);
        CHECK(pwin.find("Issue #2802") != std::string::npos, "AC1: public cites #2802");
        CHECK(window_uses_local_pat_arena(pwin), "AC1: public local pat_arena, no temp_arena_");
        CHECK(pwin.find("note_replace_pattern_temp_arena_corruption_prevented") !=
                  std::string::npos,
              "AC1: public notes isolation metric");

        auto lpos = flat.find("eval_flat_apply_mutate_replace_pattern");
        CHECK(lpos != std::string::npos, "AC1: lockless helper");
        // Phase-1 arena + phase-2 nest-safe batch (~8KB+).
        auto lwin = flat.substr(lpos, 10000);
        CHECK(lwin.find("Issue #2802") != std::string::npos, "AC1: lockless cites #2802");
        CHECK(window_uses_local_pat_arena(lwin), "AC1: lockless local pat_arena, no temp_arena_");
        CHECK(lwin.find("note_replace_pattern_temp_arena_corruption_prevented") !=
                  std::string::npos,
              "AC1: lockless notes isolation metric");
        // Nested batch: do not commit/rollback outer atomic-batch mid-sub-op.
        CHECK(lwin.find("atomic_batch_active") != std::string::npos ||
                  lwin.find("nested_outer_batch") != std::string::npos,
              "AC1: lockless nest-safe batch (atomic_batch_active)");

        CHECK(ast.find("replace_pattern_temp_arena_corruption_prevented") != std::string::npos,
              "AC1: FlatAST metric");
        CHECK(ast.find("Issue #2802") != std::string::npos, "AC1: ast cites #2802");
    }

    // ── AC2: dual replace-pattern atomic-batch ──
    {
        std::println("\n--- AC2: two replace-pattern sub-ops in one batch ---");
        CompilerService cs;
        // Lockless match_sub is structural (does not compare LiteralInt values),
        // so use a *sequential* rewrite chain both sub-ops can apply:
        //   (lambda () 1) → (lambda () 10) → (lambda () 20)
        // Two sibling sub-ops each allocate their own pat_arena; both must
        // succeed without corrupting the other pattern flat/pool.
        CHECK(cs.eval("(set-code \"(begin "
                      "(define a (lambda () 1)) "
                      "(define b (lambda () 1)))\")")
                  .has_value(),
              "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws, "AC2: workspace");
        const auto iso0 = ws->replace_pattern_temp_arena_corruption_prevented_total();

        auto r =
            cs.eval("(mutate:atomic-batch "
                    "(list "
                    "  (list \"mutate:replace-pattern\" \"(lambda () 1)\" \"(lambda () 10)\") "
                    "  (list \"mutate:replace-pattern\" \"(lambda () 10)\" \"(lambda () 20)\")))");
        CHECK(r.has_value(), "AC2: batch returns");
        CHECK(is_bool(*r) && as_bool(*r), "AC2: batch success #t");
        CHECK(ws->replace_pattern_temp_arena_corruption_prevented_total() >= iso0 + 2,
              "AC2: isolation metric +>=2 (one per sub-op)");

        auto va = cs.eval("(begin (eval-current) (a))");
        auto vb = cs.eval("(begin (eval-current) (b))");
        // Both define bodies rewritten by the chain → 20.
        CHECK(va && is_int(*va) && as_int(*va) == 20, "AC2: a is 20 after dual chain");
        CHECK(vb && is_int(*vb) && as_int(*vb) == 20, "AC2: b is 20 after dual chain");
    }

    // ── AC3: sequential public calls also isolate ──
    {
        std::println("\n--- AC3: sequential public replace-pattern metric ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(begin "
                      "(define x (lambda () 3)) "
                      "(define y (lambda () 4)))\")")
                  .has_value(),
              "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto* ws = cs.evaluator().workspace_flat();
        const auto iso0 = ws->replace_pattern_temp_arena_corruption_prevented_total();

        auto r1 = cs.eval("(mutate:replace-pattern \"(lambda () 3)\" \"(lambda () 30)\")");
        auto r2 = cs.eval("(mutate:replace-pattern \"(lambda () 4)\" \"(lambda () 40)\")");
        CHECK(r1.has_value() && is_bool(*r1) && as_bool(*r1), "AC3: first #t");
        CHECK(r2.has_value() && is_bool(*r2) && as_bool(*r2), "AC3: second #t");
        CHECK(ws->replace_pattern_temp_arena_corruption_prevented_total() >= iso0 + 2,
              "AC3: metric +>=2");
        auto vx = cs.eval("(begin (eval-current) (x))");
        auto vy = cs.eval("(begin (eval-current) (y))");
        CHECK(vx && is_int(*vx) && as_int(*vx) == 30, "AC3: x is 30");
        CHECK(vy && is_int(*vy) && as_int(*vy) == 40, "AC3: y is 40");
    }

    // ── AC4: storm ──
    {
        std::println("\n--- AC4: 20 dual-sub-op batches remain correct ---");
        CompilerService cs;
        for (int i = 0; i < 20; ++i) {
            // Sequential rewrite chain (lockless is structural; see AC2).
            CHECK(cs.eval("(set-code \"(begin "
                          "(define p (lambda () 5)) "
                          "(define q (lambda () 5)))\")")
                      .has_value(),
                  "AC4: reset code");
            CHECK(cs.eval("(eval-current)").has_value(), "AC4: re-eval");
            auto r = cs.eval(
                "(mutate:atomic-batch "
                "(list "
                "  (list \"mutate:replace-pattern\" \"(lambda () 5)\" \"(lambda () 50)\") "
                "  (list \"mutate:replace-pattern\" \"(lambda () 50)\" \"(lambda () 60)\")))");
            CHECK(r.has_value() && is_bool(*r) && as_bool(*r), "AC4: batch ok");
            auto vp = cs.eval("(begin (eval-current) (p))");
            auto vq = cs.eval("(begin (eval-current) (q))");
            CHECK(vp && is_int(*vp) && as_int(*vp) == 60, "AC4: p is 60");
            CHECK(vq && is_int(*vq) && as_int(*vq) == 60, "AC4: q is 60");
        }
    }

    // ── Issue #3664: lockless match_sub is a local recursive struct, not
    // a type-erased callable. Wildcard + nested match still apply. Soft unchanged.
    {
        std::println("\n--- #3664 AC1: replace-pattern arm has no std::function ---");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        auto lpos = flat.find("eval_flat_apply_mutate_replace_pattern");
        CHECK(lpos != std::string::npos, "3664 AC1: lockless helper");
        auto nxt = flat.find("eval_flat_apply_mutate_replace_subtree", lpos);
        auto lwin = nxt != std::string::npos && nxt > lpos ? flat.substr(lpos, nxt - lpos)
                                                           : flat.substr(lpos, 14000);
        CHECK(lwin.find("Issue #3664") != std::string::npos, "3664 AC1: cites #3664");
        CHECK(lwin.find("struct MatchSub") != std::string::npos, "3664 AC1: local MatchSub");
        CHECK(lwin.find("operator()") != std::string::npos, "3664 AC1: recursive operator()");
        CHECK(lwin.find("std::function") == std::string::npos,
              "3664 AC1: no std::function in replace-pattern arm");
        CHECK(lwin.find("(*this)") != std::string::npos, "3664 AC1: recurse via *this");
    }
    {
        std::println("\n--- #3664 AC2: #3042 PureWrap still bans std::function dirty pred ---");
        const auto pw =
            read_file("scripts/coverage/checks/check_pure_wrap_no_std_function_3042.py");
        CHECK(pw.find("Issue #3042") != std::string::npos, "3664 AC2: #3042 linter present");
        CHECK(pw.find("std::function") != std::string::npos, "3664 AC2: #3042 still bans");
        const auto ev = read_file("src/compiler/evaluator_eval_flat.cpp");
        CHECK(ev.find("std::function<bool(aura::ast::NodeId)> has_define") != std::string::npos,
              "3664 AC2: while has_define cold std::function may remain");
        CHECK(ev.find("std::function<std::string(aura::ast::NodeId)> node_source") !=
                  std::string::npos,
              "3664 AC2: DefineModule serializer cold std::function may remain");
    }
    {
        std::println("\n--- #3664 AC3: Soft/Off unchanged; no new query key ---");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        auto lpos = flat.find("eval_flat_apply_mutate_replace_pattern");
        auto nxt = flat.find("eval_flat_apply_mutate_replace_subtree", lpos);
        auto lwin = nxt != std::string::npos && nxt > lpos ? flat.substr(lpos, nxt - lpos)
                                                           : flat.substr(lpos, 14000);
        CHECK(lwin.find("schema-3664") == std::string::npos, "3664 AC3: no schema-3664");
        CHECK(lwin.find("production_defaults_active") == std::string::npos,
              "3664 AC3: matcher is not production-gated");
        const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(mut.find("schema-3664") == std::string::npos, "3664 AC3: no new query key");
    }
    {
        std::println("\n--- #3664 AC4: wildcard + nested lockless replace ---");
        {
            CompilerService cs;
            CHECK(cs.eval("(set-code \"(define f (lambda () (+ 1 2)))\")").has_value(),
                  "3664 AC4: set-code wildcard");
            CHECK(cs.eval("(eval-current)").has_value(), "3664 AC4: eval wildcard");
            auto r = cs.eval("(mutate:atomic-batch "
                             "(list "
                             "  (list \"mutate:replace-pattern\" \"(+ ... ...)\" \"(* 3 4)\")))");
            CHECK(r.has_value() && is_bool(*r) && as_bool(*r), "3664 AC4: wildcard batch #t");
            auto vf = cs.eval("(begin (eval-current) (f))");
            CHECK(vf && is_int(*vf) && as_int(*vf) == 12,
                  "3664 AC4: f is 12 after (+ ...) → (* 3 4)");
        }
        {
            CompilerService cs;
            CHECK(cs.eval("(set-code \"(define g (lambda () 1))\")").has_value(),
                  "3664 AC4: set-code nested");
            CHECK(cs.eval("(eval-current)").has_value(), "3664 AC4: eval nested");
            auto r = cs.eval(
                "(mutate:atomic-batch "
                "(list "
                "  (list \"mutate:replace-pattern\" \"(lambda () ...)\" \"(lambda () 9)\")))");
            CHECK(r.has_value() && is_bool(*r) && as_bool(*r), "3664 AC4: nested batch #t");
            auto vg = cs.eval("(begin (eval-current) (g))");
            CHECK(vg && is_int(*vg) && as_int(*vg) == 9, "3664 AC4: g is 9 after (lambda () ...)");
        }
        CHECK(read_file("tests/compiler/test_issue_3664.cpp").empty() &&
                  read_file("tests/issues/test_issue_3664.cpp").empty(),
              "3664 AC4: no test_issue_3664.cpp");
        CHECK(read_file("docs/design/3664-replace-pattern-match-sub.md").empty(),
              "3664 AC4: no docs/design/");
        const auto build = read_file("build.py");
        CHECK(build.find("check_replace_pattern_match_sub_no_std_function_3664") !=
                  std::string::npos,
              "3664 AC4: build.py wires linter");
        const auto p3663 = build.find("check_linear_move_elision_abort_live_3663");
        const auto p3664 = build.find("check_replace_pattern_match_sub_no_std_function_3664");
        CHECK(p3663 != std::string::npos && p3664 != std::string::npos && p3663 < p3664,
              "3664 AC4: linter AFTER #3663");
    }

    std::println("\n=== #2802 atomic-batch replace-pattern sibling: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_atomic_batch_replace_pattern_sibling();
}
#endif
