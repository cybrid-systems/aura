// @category: unit
// @reason: Issue #2791 — mutate:rebind parse-error frees orphan nodes
// appended by parse_to_flat (Guard only rolls mutation log, not parse appends).
//
//   AC1: rebind parse-error path cites #2791 + free_orphan_nodes_from
//   AC2: single parse-error rebind frees live-node count back to baseline
//   AC3: 50 failed rebinds do not grow live-node count unboundedly
//   AC4: successful rebind still works after failed attempts
//   AC5: this suite + linter; no docs/design/2791-*; no test_issue_2791.cpp

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
using aura::compiler::types::is_pair;
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

int run_test_rebind_parse_failure_no_leak() {
    std::println("=== Issue #2791: rebind parse-error frees orphans ===");
    CHECK(true, "ac2791: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: free_orphan_nodes_from on rebind parse-error ---");
        auto src = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(!src.empty(), "AC1: mutate primitives readable");
        auto pos = src.find("add_mutate(\"mutate:rebind\"");
        if (pos == std::string::npos)
            pos = src.find("mutate:rebind");
        CHECK(pos != std::string::npos, "AC1: rebind present");
        // Window through set-body / next large prim (avoid full file).
        auto end = src.find("add_mutate(\"mutate:set-body\"", pos);
        if (end == std::string::npos)
            end = pos + 6000;
        auto win = src.substr(pos, end - pos);
        CHECK(win.find("Issue #2791") != std::string::npos, "AC1: cites #2791");
        CHECK(win.find("free_orphan_nodes_from") != std::string::npos,
              "AC1: free_orphan_nodes_from on rebind");
        CHECK(win.find("free_rebind_parse_orphans") != std::string::npos ||
                  win.find("size_before_parse") != std::string::npos,
              "AC1: size snapshot for free bound");
    }

    // ── AC2: live count restored after one parse-error ──
    {
        std::println("\n--- AC2: live-node count after single parse-error ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "AC2: workspace flat");
        const auto live0 = live_node_count(*flat);
        const auto size0 = flat->size();
        const auto freed0 = flat->ghost_orphan_nodes_freed();

        auto bad = cs.eval("(mutate:rebind \"f\" \"(((\")");
        CHECK(bad.has_value(), "AC2: rebind returns value");
        // Expect failure (merr or non-#t).
        CHECK(!(is_bool(*bad) && as_bool(*bad)), "AC2: parse-error not success #t");

        const auto live1 = live_node_count(*flat);
        CHECK(live1 == live0, "AC2: live-node count unchanged after parse-error rebind");
        CHECK(flat->ghost_orphan_nodes_freed() >= freed0,
              "AC2: ghost_orphan counter non-decreasing");
        // size() may stay larger (free-list reuse, no SoA shrink) but free_list
        // should absorb the partial parse so a second fail does not grow live.
        (void)size0;
    }

    // ── AC3: many failures do not grow live nodes ──
    {
        std::println("\n--- AC3: 50 parse-error rebinds — live count stable ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "AC3: flat");
        const auto live0 = live_node_count(*flat);
        const auto size0 = flat->size();

        for (int i = 0; i < 50; ++i) {
            auto r = cs.eval("(mutate:rebind \"f\" \"(((\")");
            CHECK(r.has_value(), "AC3: rebind attempt returns");
            CHECK(!(is_bool(*r) && as_bool(*r)), "AC3: each attempt fails");
        }
        const auto live1 = live_node_count(*flat);
        CHECK(live1 == live0, "AC3: live-node count stable after 50 parse-errors");
        // size may grow once on first failure then free-list reuses; allow
        // modest growth only if free_list not engaged (should not be 50x).
        const auto size1 = flat->size();
        CHECK(size1 < size0 + 500, "AC3: size not unbounded (free-list reuses orphans)");
    }

    // ── AC4: success after failures ──
    {
        std::println("\n--- AC4: successful rebind after parse-errors ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        (void)cs.eval("(mutate:rebind \"f\" \"(((\")");
        (void)cs.eval("(mutate:rebind \"f\" \"(((\")");
        auto ok = cs.eval("(mutate:rebind \"f\" \"(lambda () 42)\")");
        CHECK(ok && is_bool(*ok) && as_bool(*ok), "AC4: successful rebind after fails");
        auto v = cs.eval("(begin (eval-current) (f))");
        CHECK(v && is_int(*v) && as_int(*v) == 42, "AC4: f returns 42 after rebind");
    }

    std::println("\n=== #2791 rebind parse no-leak: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_rebind_parse_failure_no_leak();
}
#endif
