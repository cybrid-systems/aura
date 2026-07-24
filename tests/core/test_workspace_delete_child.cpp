// @category: unit
// @reason: Issue #1770 — WorkspaceTree::delete_child must null node
// Issue #1770 (#1978 renamed): issue# moved from filename to header.
// fields before delete so a throwing dtor cannot leave dangling pointers.
//
//   AC1: source cites #1770; nulls before delete owned_flat/pool
//   AC2: delete root (idx 0) rejected
//   AC3: create_child + delete_child leaves node fields null / has_own_flat false
//   AC4: delete_child OOB returns false

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::WorkspaceNode;
using aura::compiler::WorkspaceTree;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: delete_child nulls before delete ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1770") != std::string::npos, "cites #1770");
        auto pos = ixx.find("WorkspaceTree::delete_child");
        CHECK(pos != std::string::npos, "delete_child present");
        auto win = ixx.substr(pos, 900);
        CHECK(win.find("owned_flat") != std::string::npos, "detaches owned_flat");
        CHECK(win.find("owned_pool") != std::string::npos, "detaches owned_pool");
        auto null_flat = win.find("n.flat = nullptr");
        auto del_flat = win.find("delete owned_flat");
        CHECK(null_flat != std::string::npos && del_flat != std::string::npos,
              "null and delete present");
        CHECK(null_flat < del_flat, "null before delete owned_flat");
        CHECK(win.find("n.has_own_flat = false") != std::string::npos, "clears has_own_flat");
    }

    // ── AC2: root reject ──
    {
        std::println("\n--- AC2: delete_child(0) rejected ---");
        WorkspaceTree tree;
        WorkspaceNode root;
        root.name = "root";
        root.is_root = true;
        tree.nodes_.push_back(std::move(root));
        CHECK(!tree.delete_child(0), "cannot delete root idx 0");
        CHECK(tree.size() == 1, "root remains");
    }

    // ── AC3: create + delete child ──
    {
        std::println("\n--- AC3: create_child + delete_child clears fields ---");
        WorkspaceTree tree;
        WorkspaceNode root;
        root.name = "root";
        root.is_root = true;
        tree.nodes_.push_back(std::move(root));

        auto idx = tree.create_child("child", 0, nullptr, nullptr);
        CHECK(idx == 1, "child idx 1");
        CHECK(tree.delete_child(idx), "delete_child ok");
        CHECK(tree.nodes_[idx].flat == nullptr, "flat null");
        CHECK(tree.nodes_[idx].pool == nullptr, "pool null");
        CHECK(tree.nodes_[idx].parent_flat_ == nullptr, "parent_flat null");
        CHECK(tree.nodes_[idx].parent_pool_ == nullptr, "parent_pool null");
        CHECK(!tree.nodes_[idx].has_own_flat, "has_own_flat false");
        CHECK(tree.nodes_[idx].cow_epoch == 0, "cow_epoch 0");
        CHECK(tree.nodes_[idx].generation == 0, "generation 0");
    }

    // ── AC4: OOB ──
    {
        std::println("\n--- AC4: OOB delete_child ---");
        WorkspaceTree tree;
        WorkspaceNode root;
        root.is_root = true;
        tree.nodes_.push_back(std::move(root));
        CHECK(!tree.delete_child(99), "OOB false");
        CHECK(!tree.delete_child(1), "missing child false");
    }

    std::println("\n=== test_workspace_delete_child_1770: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
