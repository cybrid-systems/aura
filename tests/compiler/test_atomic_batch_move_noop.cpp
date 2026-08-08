// @category: unit
// @reason: Issue #2794 — mutate:atomic-batch must not treat move-node
// already-at-destination as batch failure. #f is soft no-op; real errors
// use EvalResult unexpected. Idempotent move-node commits the batch.
//
//   AC1: atomic-batch bool-false path cites #2794; sub_op_noop_total
//   AC2: move-node same parent+index is no-op (#t, no NULL hole)
//   AC3: atomic-batch of no-op move-node commits (not batch-failed)
//   AC4: real sub-op unexpected still rolls back (rebind miss)
//   AC5: this suite + linter; no docs/design/2794-*; no test_issue_2794.cpp

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

using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
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

static std::string merr_kind(CompilerService& cs, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

// Find a non-root live child: returns (node, parent, index) or zeros.
struct ChildLoc {
    NodeId node = NULL_NODE;
    NodeId parent = NULL_NODE;
    std::uint32_t index = 0;
};

static ChildLoc find_first_child(aura::ast::FlatAST& flat) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto kids = flat.children(id);
        for (std::size_t i = 0; i < kids.size(); ++i) {
            auto c = kids[i];
            if (c == NULL_NODE || !flat.is_live_node(c))
                continue;
            return ChildLoc{c, id, static_cast<std::uint32_t>(i)};
        }
    }
    return {};
}

} // namespace

int run_test_atomic_batch_move_noop() {
    std::println("=== Issue #2794: atomic-batch move-node no-op ===");
    CHECK(true, "ac2794: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: source cites #2794 ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        auto evh = read_file("src/compiler/evaluator.ixx");
        CHECK(!mut.empty(), "AC1: mutate primitives readable");
        auto pos = mut.find("add_mutate(\"mutate:atomic-batch\"");
        if (pos == std::string::npos)
            pos = mut.find("mutate:atomic-batch");
        CHECK(pos != std::string::npos, "AC1: atomic-batch present");
        // Sub-op loop with #2794 soft no-op sits ~15KB into the body.
        auto win = mut.substr(pos, 22000);
        CHECK(win.find("Issue #2794") != std::string::npos, "AC1: atomic-batch cites #2794");
        CHECK(win.find("sub_op_noop_total") != std::string::npos, "AC1: sub_op_noop_total bump");
        // Soft no-op: continue, not mark_sub_op_failed on #f.
        CHECK(win.find("soft no-op") != std::string::npos ||
                  win.find("soft no-op signal") != std::string::npos,
              "AC1: soft no-op comment");
        CHECK(flat.find("Issue #2794") != std::string::npos, "AC1: lockless move-node cites #2794");
        CHECK(flat.find("already at the requested") != std::string::npos ||
                  flat.find("already at destination") != std::string::npos ||
                  flat.find("idempotent no-op") != std::string::npos,
              "AC1: lockless move-node no-op path");
        CHECK(evh.find("sub_op_noop_total") != std::string::npos, "AC1: domain counter");
    }

    // ── AC2/AC3: no-op move in atomic-batch commits ──
    {
        std::println("\n--- AC2/AC3: same-position move-node commits batch ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1))) (define g (lambda () 2))\")")
                  .has_value(),
              "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "AC2: workspace");
        auto loc = find_first_child(*ws);
        CHECK(loc.node != NULL_NODE && loc.parent != NULL_NODE, "AC2: found child node");

        const auto log_before = ws->mutation_log_view().size();
        const auto rollbacks_before = cs.evaluator().atomic_batch_rollbacks();
        const auto kids_before = ws->children(loc.parent).size();

        auto batch =
            cs.eval(std::format("(mutate:atomic-batch (list (list \"mutate:move-node\" {} {} {})))",
                                loc.node, loc.parent, loc.index));
        CHECK(batch.has_value(), "AC3: batch returns");
        CHECK(is_bool(*batch) && as_bool(*batch), "AC3: batch commits #t (not batch-failed)");
        CHECK(cs.evaluator().atomic_batch_rollbacks() == rollbacks_before,
              "AC3: no rollback count bump");

        // Node still at same parent+index; no NULL hole growth.
        CHECK(ws->parent_of(loc.node) == loc.parent, "AC2: parent unchanged");
        auto kids = ws->children(loc.parent);
        CHECK(kids.size() == kids_before, "AC2: child count unchanged (no NULL hole)");
        bool still_at = loc.index < kids.size() && kids[loc.index] == loc.node;
        CHECK(still_at, "AC2: still at same index");
        // No-op must not append a move-node mutation.
        CHECK(ws->mutation_log_view().size() == log_before, "AC2: no mutation log growth on no-op");
    }

    // ── AC3b: standalone move-node no-op ──
    {
        std::println("\n--- AC3b: standalone mutate:move-node no-op ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define a (lambda () 1))\")").has_value(), "AC3b: set-code");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "AC3b: workspace");
        auto loc = find_first_child(*ws);
        CHECK(loc.node != NULL_NODE, "AC3b: child");
        auto r =
            cs.eval(std::format("(mutate:move-node {} {} {})", loc.node, loc.parent, loc.index));
        CHECK(r && is_bool(*r) && as_bool(*r), "AC3b: standalone no-op returns #t");
        CHECK(ws->parent_of(loc.node) == loc.parent, "AC3b: parent unchanged");
    }

    // ── AC4: real failure still rolls back ──
    {
        std::println("\n--- AC4: unexpected sub-op still batch-failed ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        auto batch = cs.eval("(mutate:atomic-batch "
                             "(list (list \"mutate:rebind\" \"f\" \"(lambda () 99)\") "
                             "      (list \"mutate:rebind\" \"no-such-2794\" \"(lambda () 0)\")))");
        CHECK(batch.has_value(), "AC4: returns value");
        CHECK(!(is_bool(*batch) && as_bool(*batch)), "AC4: not success #t");
        CHECK(is_pair(*batch), "AC4: merr pair");
        CHECK(merr_kind(cs, *batch) == "batch-failed", "AC4: kind batch-failed");
        using aura::compiler::types::as_int;
        using aura::compiler::types::is_int;
        auto f = cs.eval("(begin (eval-current) (f))");
        CHECK(f && is_int(*f) && as_int(*f) == 1, "AC4: f still 1 after failed batch");
    }

    std::println("\n=== #2794 atomic-batch move-node no-op: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_atomic_batch_move_noop();
}
#endif
