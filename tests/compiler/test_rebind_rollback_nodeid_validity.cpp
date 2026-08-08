// @category: unit
// @reason: Issue #2795 — mutate:rebind must capture old body NodeId AFTER
// parse_to_flat + resolve_define_after_parse; Guard rollback must not
// reattach a free/OOB stale NodeId (UAF / logical corruption).
//
//   AC1: rebind source cites #2795; old_value after parse + live check
//   AC2: try_rollback_rebind_op rejects free-slot old_child + metric
//   AC3: live rebind + boundary abort restores original body NodeId
//   AC4: this suite + linter; no docs/design/2795-*; no test_issue_2795.cpp

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core;
import aura.core.ast;

namespace {

using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
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

static NodeId find_define(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                          std::string_view name) {
    auto sym = pool.intern(std::string(name));
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto v = flat.get(id);
        if (v.tag == NodeTag::Define && v.sym_id == sym)
            return id;
    }
    return NULL_NODE;
}

} // namespace

int run_test_rebind_rollback_nodeid_validity() {
    std::println("=== Issue #2795: rebind rollback NodeId validity ===");
    CHECK(true, "ac2795: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: capture after parse + #2795 citations ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto flat_src = read_file("src/compiler/evaluator_eval_flat.cpp");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(!mut.empty(), "AC1: mutate readable");
        auto pos = mut.find("add_mutate(\"mutate:rebind\"");
        if (pos == std::string::npos)
            pos = mut.find("mutate:rebind");
        CHECK(pos != std::string::npos, "AC1: rebind present");
        // Existing path is long (hygiene + add-path); window to body capture.
        auto win = mut.substr(pos, 20000);
        CHECK(win.find("Issue #2795") != std::string::npos, "AC1: rebind cites #2795");
        CHECK(win.find("old_value_node") != std::string::npos, "AC1: old_value_node");
        // Capture must be after parse_to_flat in the same window.
        auto parse_pos = win.find("parse_to_flat");
        auto capture_pos = win.find("old_value_node");
        CHECK(parse_pos != std::string::npos && capture_pos != std::string::npos,
              "AC1: parse + capture present");
        CHECK(capture_pos > parse_pos, "AC1: old_value_node after parse_to_flat in source");
        CHECK(win.find("resolve_define_after_parse") != std::string::npos,
              "AC1: resolve_define_after_parse");
        auto resolve_pos = win.find("resolve_define_after_parse");
        CHECK(resolve_pos < capture_pos, "AC1: resolve before capture");
        CHECK(win.find("is_live_node") != std::string::npos ||
                  win.find("is_free_slot") != std::string::npos,
              "AC1: live/free check on old body");
        CHECK(flat_src.find("Issue #2795") != std::string::npos, "AC1: batch rebind cites #2795");
        CHECK(ast.find("Issue #2795") != std::string::npos, "AC1: try_rollback cites #2795");
        CHECK(ast.find("rebind_rollback_stale_nodeid_prevented") != std::string::npos,
              "AC1: stale-prevented counter");
    }

    // ── AC2: try_rollback rejects free-slot old_child ──
    {
        std::println("\n--- AC2: stale free-slot old_child rejected ---");
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto body = flat.add_literal(static_cast<std::int64_t>(1));
        auto def = flat.add_define(pool.intern("x"), body);
        flat.root = def;
        // Fake a free slot that is NOT the real body: append then free as orphan.
        auto ghost = flat.add_literal(static_cast<std::int64_t>(99));
        (void)flat.free_orphan_nodes_from(ghost);
        CHECK(flat.is_free_slot(ghost), "AC2: ghost is free");

        const auto torn0 = flat.rebind_rollback_stale_nodeid_prevented_total();
        aura::ast::MutationRecord rec{};
        rec.mutation_id = 1;
        rec.target_node = def;
        rec.operator_name = "rebind";
        rec.status = aura::ast::MutationStatus::Committed;
        rec.has_rollback_data = true;
        rec.field_offset = 0;
        rec.old_value = static_cast<std::uint64_t>(ghost); // stale free id
        rec.new_value = static_cast<std::uint64_t>(body);
        auto r = flat.try_rollback_record(rec);
        CHECK(!r.has_value(), "AC2: try_rollback fails on free old_child");
        CHECK(flat.rebind_rollback_stale_nodeid_prevented_total() > torn0, "AC2: metric bumped");
        // Define body unchanged (still original body, not free ghost).
        auto kids = flat.children(def);
        CHECK(!kids.empty() && kids[0] == body, "AC2: define body still original");
    }

    // ── AC3: live rebind + boundary abort restores body ──
    {
        std::println("\n--- AC3: Guard abort restores original body NodeId ---");
        Evaluator ev;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto body = flat.add_literal(static_cast<std::int64_t>(10));
        auto def = flat.add_define(pool.intern("acc"), body);
        flat.root = def;
        ev.set_workspace_flat(&flat);
        ev.set_workspace_pool(&pool);

        const auto old_body = flat.children(def)[0];
        CHECK(old_body == body, "AC3: setup body");

        // Simulate rebind: parse append + capture post-resolve + set_child, then abort.
        const auto size_before = static_cast<std::size_t>(flat.size());
        ev.enter_mutation_boundary();
        auto new_body = flat.add_literal(static_cast<std::int64_t>(42));
        // Re-resolve define in pre-parse range (size_before still covers def).
        auto resolved = flat.resolve_define_after_parse(pool.intern("acc"), def, size_before);
        CHECK(resolved == def, "AC3: define still at preferred id");
        auto old_v = flat.get(resolved);
        auto old_value_node = old_v.children.empty() ? NULL_NODE : old_v.child(0);
        CHECK(old_value_node == old_body, "AC3: post-parse body id still original");
        CHECK(flat.is_live_node(old_value_node), "AC3: old body live");
        (void)flat.add_mutation_with_rollback(
            resolved, "rebind", "acc", "acc", "ac2795", aura::ast::MutationStatus::Committed, 0,
            static_cast<std::uint64_t>(old_value_node), static_cast<std::uint64_t>(new_body), true);
        flat.set_child(resolved, 0, new_body);
        CHECK(flat.children(resolved)[0] == new_body, "AC3: mid-boundary new body");
        ev.exit_mutation_boundary(false);

        CHECK(flat.children(resolved)[0] == old_body, "AC3: body restored to original NodeId");
        CHECK(flat.get(old_body).int_value == 10, "AC3: original literal still 10");
        CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::RolledBack,
              "AC3: log RolledBack");
    }

    // ── AC3b: EDSL happy path still works ──
    {
        std::println("\n--- AC3b: EDSL rebind still commits ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC3b: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3b: eval");
        auto r = cs.eval("(mutate:rebind \"f\" \"(lambda () 7)\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "AC3b: rebind #t");
        auto f = cs.eval("(begin (eval-current) (f))");
        CHECK(f && is_int(*f) && as_int(*f) == 7, "AC3b: f is 7");
    }

    std::println("\n=== #2795 rebind rollback NodeId validity: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_rebind_rollback_nodeid_validity();
}
#endif
