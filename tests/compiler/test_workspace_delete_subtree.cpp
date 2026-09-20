// @category: unit
// @reason: Issue #2789 — workspace:delete / WorkspaceTree::delete_child
// recursively tombstones descendants (no orphan layers in list).
//
//   AC1: delete_child source cites #2789; recursive parent_layer_idx walk
//   AC2: create root→child→grandchild; delete child; grandchild is_tombstone
//   AC3: live workspace:delete parent leaves list without orphans
//   AC4: delete active descendant rebinds evaluator to root
//   AC5: this suite + linter; no docs/design/2789-*; no test_issue_2789.cpp

#include "test_harness.hpp"

#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::WorkspaceNode;
using aura::compiler::WorkspaceTree;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;
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

// Count entries in workspace:list pair spine: ((id name flags) ...)
static int list_count(CompilerService& cs) {
    auto r = cs.eval("(workspace :list)");
    if (!r)
        return -1;
    if (is_void(*r))
        return 0;
    int n = 0;
    auto cur = *r;
    auto& pairs = cs.evaluator().pairs();
    int guard = 0;
    while (is_pair(cur) && guard++ < 256) {
        ++n;
        auto idx = as_pair_idx(cur);
        if (idx >= pairs.size())
            break;
        cur = pairs[idx].cdr;
    }
    return n;
}

// True if list contains entry with given id.
static bool list_has_id(CompilerService& cs, std::int64_t want) {
    auto r = cs.eval("(workspace :list)");
    if (!r || is_void(*r))
        return false;
    auto cur = *r;
    auto& pairs = cs.evaluator().pairs();
    int guard = 0;
    while (is_pair(cur) && guard++ < 256) {
        auto idx = as_pair_idx(cur);
        if (idx >= pairs.size())
            break;
        // entry is (id . (name . flags)); car of car is id
        auto entry = pairs[idx].car;
        if (is_pair(entry)) {
            auto eidx = as_pair_idx(entry);
            if (eidx < pairs.size() && is_int(pairs[eidx].car) && as_int(pairs[eidx].car) == want)
                return true;
        }
        cur = pairs[idx].cdr;
    }
    return false;
}

} // namespace

int run_test_workspace_delete_subtree() {
    std::println("=== Issue #2789: workspace:delete recursive subtree ===");
    CHECK(true, "ac2789: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: delete_child recursive #2789 ---");
        auto ixx = read_file("src/compiler/evaluator.ixx");
        auto ws = read_file("src/compiler/evaluator_primitives_workspace.cpp");
        CHECK(!ixx.empty(), "AC1: evaluator.ixx readable");
        auto pos = ixx.find("WorkspaceTree::delete_child");
        CHECK(pos != std::string::npos, "AC1: delete_child present");
        auto win = ixx.substr(pos, 1600);
        CHECK(win.find("Issue #2789") != std::string::npos ||
                  win.find("#2789") != std::string::npos,
              "AC1: cites #2789");
        CHECK(win.find("parent_layer_idx") != std::string::npos,
              "AC1: walks parent_layer_idx children");
        CHECK(win.find("is_tombstone") != std::string::npos, "AC1: is_tombstone helper used");
        CHECK(ws.find("Issue #2789") != std::string::npos, "AC1: workspace prims cite #2789");
        CHECK(ws.find("is_under") != std::string::npos, "AC1: delete rebind uses is_under");
        CHECK(ws.find("is_tombstone") != std::string::npos, "AC1: list skips tombstones");
    }

    // ── AC2: pure tree grandchild orphan fix ──
    {
        std::println("\n--- AC2: delete parent tombstones grandchild ---");
        WorkspaceTree tree;
        WorkspaceNode root;
        root.name = "root";
        root.is_root = true;
        tree.nodes_.push_back(std::move(root));

        auto c1 = tree.create_child("child", 0, nullptr, nullptr);
        CHECK(c1 == 1, "AC2: child idx 1");
        auto c2 = tree.create_child("grandchild", c1, nullptr, nullptr);
        CHECK(c2 == 2, "AC2: grandchild idx 2");
        // Stamp cow_epoch so we can observe recursive tombstone clear
        // (delete_child zeros cow_epoch on every node it processes).
        tree.nodes_[c1].cow_epoch = 11;
        tree.nodes_[c2].cow_epoch = 22;

        CHECK(tree.is_under(c2, c1), "AC2: grandchild under child");
        CHECK(tree.is_under(c1, c1), "AC2: node under itself");
        CHECK(!tree.is_under(0, c1), "AC2: root not under child");

        CHECK(tree.delete_child(c1), "AC2: delete child (subtree)");
        CHECK(tree.nodes_[c1].cow_epoch == 0, "AC2: child cleared");
        CHECK(tree.nodes_[c2].cow_epoch == 0, "AC2: grandchild cleared (no orphan)");
        CHECK(tree.is_tombstone(c1), "AC2: child is_tombstone");
        CHECK(tree.is_tombstone(c2), "AC2: grandchild is_tombstone");
        CHECK(tree.size() == 3, "AC2: slots retained (tombstone, no erase)");
    }

    // ── AC3: live hierarchical delete + list ──
    {
        std::println("\n--- AC3: live delete parent; list has no orphans ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto c1 = cs.eval("(workspace :create \"child-2789\")");
        CHECK(c1 && is_int(*c1) && as_int(*c1) >= 1, "AC3: create child");
        const auto child = as_int(*c1);
        CHECK(cs.eval(std::format("(workspace :switch {})", child)).has_value(),
              "AC3: switch child");
        auto c2 = cs.eval("(workspace :create \"grand-2789\")");
        CHECK(c2 && is_int(*c2) && as_int(*c2) > child, "AC3: create grandchild");
        const auto grand = as_int(*c2);
        CHECK(cs.eval("(workspace :switch 0)").has_value(), "AC3: switch root");

        const auto n_before = list_count(cs);
        CHECK(n_before >= 3, "AC3: list has root+child+grand before delete");
        CHECK(list_has_id(cs, child), "AC3: list has child before");
        CHECK(list_has_id(cs, grand), "AC3: list has grand before");

        auto del = cs.eval(std::format("(workspace:delete {})", child));
        CHECK(del && is_bool(*del) && as_bool(*del), "AC3: delete child #t");

        CHECK(!list_has_id(cs, child), "AC3: list no longer has child");
        CHECK(!list_has_id(cs, grand), "AC3: list no longer has grandchild (orphan fixed)");
        CHECK(list_has_id(cs, 0), "AC3: list still has root");
        const auto n_after = list_count(cs);
        CHECK(n_after == 1, "AC3: list size 1 (only root) after subtree delete");
    }

    // ── AC4: active under deleted parent → rebind root ──
    {
        std::println("\n--- AC4: delete ancestor of active rebinds to root ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define y 2)\")").has_value(), "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        auto c1 = cs.eval("(workspace :create \"mid-2789\")");
        CHECK(c1 && is_int(*c1), "AC4: create mid");
        const auto mid = as_int(*c1);
        CHECK(cs.eval(std::format("(workspace :switch {})", mid)).has_value(), "AC4: switch mid");
        auto c2 = cs.eval("(workspace :create \"leaf-2789\")");
        CHECK(c2 && is_int(*c2), "AC4: create leaf");
        const auto leaf = as_int(*c2);
        CHECK(cs.eval(std::format("(workspace :switch {})", leaf)).has_value(), "AC4: switch leaf");
        auto cur = cs.eval("(workspace :current)");
        CHECK(cur && is_int(*cur) && as_int(*cur) == leaf, "AC4: current is leaf");

        auto del = cs.eval(std::format("(workspace:delete {})", mid));
        CHECK(del && is_bool(*del) && as_bool(*del), "AC4: delete mid subtree");
        auto cur2 = cs.eval("(workspace :current)");
        CHECK(cur2 && is_int(*cur2) && as_int(*cur2) == 0, "AC4: rebound to root 0");
        CHECK(cs.evaluator().workspace_flat() != nullptr, "AC4: evaluator flat rebound");
    }

    std::println("\n--- #3957: workspace:discard own-flat without parent is #f ---");
    {
        const auto ws_cpp = read_file("src/compiler/evaluator_primitives_workspace.cpp");
        CHECK(ws_cpp.find("Issue #3957") != std::string::npos, "3957: cite");
        auto dpos = ws_cpp.find("add(\"workspace:discard\"");
        CHECK(dpos != std::string::npos, "3957: discard present");
        auto dwin_end = ws_cpp.find("workspace :merge", dpos);
        auto dwin = ws_cpp.substr(dpos, (dwin_end == std::string::npos ? 4000 : dwin_end - dpos));
        CHECK(dwin.find("!ws.parent_flat_") != std::string::npos,
              "3957: own-flat without parent is fail-closed");
        CHECK(dwin.find("MutationBoundaryGuard::try_acquire") != std::string::npos,
              "3957: #3863 Guard retained on parent path");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define z 1)\")").has_value(), "3957: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3957: eval");
        auto d0 = cs.eval("(workspace:discard 0)");
        CHECK(d0 && is_bool(*d0) && !as_bool(*d0),
              "3957: root own-flat discard is #f not vacuous #t");
        CHECK(cs.evaluator().workspace_flat() != nullptr, "3957: root flat still live");
        CHECK(read_file("tests/issues/test_issue_3957.cpp").empty(), "3957: no invent");
        CHECK(read_file("docs/design/3957-discard-orphan.md").empty(), "3957: no docs/design");
    }

    std::println("\n=== #2789 delete subtree: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workspace_delete_subtree();
}
#endif
