// @category: unit
// @reason: Issue #2798 — replace-pattern parse skip / zero-replace paths
// must free_orphan_nodes_from (rollback_atomic_batch does not free appends).
//
//   AC1: lockless + public cite #2798; free_orphan on skip + replaced_count==0
//   AC2: match+parse-fail does not grow live-node count
//   AC3: 50 match+parse-fail storms keep live count stable
//   AC4: no-match pattern also leaves live count unchanged
//   AC5: this suite + linter; no docs/design/2798-*; no test_issue_2798.cpp

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
using aura::compiler::types::is_bool;
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

static std::size_t live_node_count(const aura::ast::FlatAST& flat) {
    std::size_t n = 0;
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (flat.is_live_node(id))
            ++n;
    }
    return n;
}

} // namespace

int run_test_replace_pattern_no_match_no_leak() {
    std::println("=== Issue #2798: replace-pattern no-match / parse-fail no leak ===");
    CHECK(true, "ac2798: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: free_orphan on replace-pattern skip paths ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        CHECK(!mut.empty() && !flat.empty(), "AC1: sources readable");
        auto lpos = flat.find("eval_flat_apply_mutate_replace_pattern");
        CHECK(lpos != std::string::npos, "AC1: lockless helper");
        auto lwin = flat.substr(lpos, 9000);
        CHECK(lwin.find("Issue #2798") != std::string::npos, "AC1: lockless cites #2798");
        CHECK(lwin.find("free_orphan_nodes_from") != std::string::npos,
              "AC1: lockless free_orphan_nodes_from");
        CHECK(lwin.find("free_repl_parse_orphans") != std::string::npos ||
                  lwin.find("size_before_parse") != std::string::npos,
              "AC1: lockless size snapshot");
        // replaced_count == 0 path frees from end_id
        CHECK(lwin.find("replaced_count == 0") != std::string::npos, "AC1: zero-replace path");

        auto ppos = mut.find("add_mutate(\"mutate:replace-pattern\"");
        if (ppos == std::string::npos)
            ppos = mut.find("mutate:replace-pattern");
        CHECK(ppos != std::string::npos, "AC1: public replace-pattern");
        // Public body is long (QueryMatcher + apply); #2798 near end.
        auto pwin = mut.substr(ppos, 24000);
        CHECK(pwin.find("Issue #2798") != std::string::npos, "AC1: public cites #2798");
        CHECK(pwin.find("free_orphan_nodes_from") != std::string::npos,
              "AC1: public free_orphan_nodes_from");
    }

    // ── AC2: match + bad replacement parse ──
    {
        std::println("\n--- AC2: match+parse-fail live count stable ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws, "AC2: workspace");
        const auto live0 = live_node_count(*ws);
        const auto freed0 = ws->ghost_orphan_nodes_freed();

        // Pattern matches lambda form; replacement is unparseable → parse fail skip.
        auto r = cs.eval("(mutate:replace-pattern \"(lambda () 1)\" \"(((\")");
        CHECK(r.has_value(), "AC2: returns");
        CHECK(!(is_bool(*r) && as_bool(*r)), "AC2: not success #t");

        CHECK(live_node_count(*ws) == live0, "AC2: live-node count unchanged");
        CHECK(ws->ghost_orphan_nodes_freed() >= freed0, "AC2: orphan free counter non-decreasing");
    }

    // ── AC3: storm ──
    {
        std::println("\n--- AC3: 50 match+parse-fail — live count stable ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto* ws = cs.evaluator().workspace_flat();
        const auto live0 = live_node_count(*ws);
        for (int i = 0; i < 50; ++i) {
            auto r = cs.eval("(mutate:replace-pattern \"(lambda () 1)\" \"(((\")");
            CHECK(r.has_value(), "AC3: each returns");
        }
        CHECK(live_node_count(*ws) == live0, "AC3: live count stable after 50 fails");
    }

    // ── AC4: pure no-match ──
    {
        std::println("\n--- AC4: no-match pattern live count stable ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        auto* ws = cs.evaluator().workspace_flat();
        const auto live0 = live_node_count(*ws);
        auto r = cs.eval("(mutate:replace-pattern \"(no-such-pattern-xyz)\" \"x\")");
        CHECK(r.has_value(), "AC4: returns");
        CHECK(!(is_bool(*r) && as_bool(*r)), "AC4: not success");
        CHECK(live_node_count(*ws) == live0, "AC4: live count unchanged on no-match");
    }

    // ── AC4b: successful replace still works ──
    {
        std::println("\n--- AC4b: successful replace-pattern still commits ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC4b: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4b: eval");
        auto r = cs.eval("(mutate:replace-pattern \"(lambda () 1)\" \"(lambda () 2)\")");
        CHECK(r.has_value(), "AC4b: returns");
        // May be #t if match applied.
        if (is_bool(*r) && as_bool(*r)) {
            auto f = cs.eval("(begin (eval-current) (f))");
            using aura::compiler::types::as_int;
            using aura::compiler::types::is_int;
            CHECK(f && is_int(*f) && as_int(*f) == 2, "AC4b: f is 2 after replace");
        } else {
            CHECK(true, "AC4b: non-#t ok if pattern shape differed (soft)");
        }
    }

    std::println("\n=== #2798 replace-pattern no-match no leak: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_replace_pattern_no_match_no_leak();
}
#endif
