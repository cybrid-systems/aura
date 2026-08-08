// @category: unit
// @reason: Issue #2788 — workspace:rollback-to name→id resolve under one
// exclusive lock; typed merr for not-found vs concurrent-delete / pair drift.
//
//   AC1: source cites #2788; WorkspaceUniqueIfNeeded; make_merr kinds
//   AC2: name resolve checks sources_ size before restore (no silent #f)
//   AC3: live named rollback still returns #t (compat with #737)
//   AC4: missing name returns merr kind "not-found" (not bare #f)
//   AC5: vector pair drift (names longer) → "concurrent-delete"
//   AC6: this suite + linter; no docs/design/2788-*; no test_issue_2788.cpp

#include "test_harness.hpp"

#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

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

static std::string rollback_to_window(const std::string& src) {
    auto pos = src.find("workspace:rollback-to");
    if (pos == std::string::npos)
        return {};
    auto end = src.find("workspace:rollback-latest", pos);
    if (end == std::string::npos)
        end = pos + 3500;
    return src.substr(pos, end - pos);
}

// make_merr → pair (kind-string . (msg-string . ...))
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

} // namespace

int run_test_workspace_rollback_to() {
    std::println("=== Issue #2788: workspace:rollback-to typed resolve ===");
    CHECK(true, "ac2788: issue stamp");

    // ── AC1/AC2: source shape ──
    {
        std::println("\n--- AC1/AC2: lock + typed merr + sources_ check ---");
        auto src = read_file("src/compiler/evaluator_primitives_workspace.cpp");
        CHECK(!src.empty(), "AC1: workspace primitives readable");
        auto win = rollback_to_window(src);
        CHECK(!win.empty(), "AC1: rollback-to present");
        CHECK(win.find("Issue #2788") != std::string::npos, "AC1: cites #2788");
        CHECK(win.find("WorkspaceUniqueIfNeeded") != std::string::npos,
              "AC1: exclusive resolve lock");
        CHECK(win.find("make_merr") != std::string::npos, "AC1: typed make_merr");
        CHECK(win.find("not-found") != std::string::npos, "AC1: not-found kind");
        CHECK(win.find("concurrent-delete") != std::string::npos, "AC1: concurrent-delete kind");
        // Name hit must validate sources_ before restore (pair drift).
        CHECK(win.find("sources_n") != std::string::npos ||
                  win.find("snapshot_sources_.size()") != std::string::npos,
              "AC2: sources_ size observed with names_");
        CHECK(win.find("i >= sources_n") != std::string::npos, "AC2: name hit checks sources_n");
        // Restore after lock release (no deadlock with Guard).
        CHECK(win.find("release workspace exclusive before restore") != std::string::npos ||
                  win.find("// release") != std::string::npos,
              "AC2: restore after lock scope");
    }

    // ── AC3: live named rollback still #t ──
    {
        std::println("\n--- AC3: live named rollback returns #t ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto snap = cs.eval("(workspace:snapshot \"pre-2788\")");
        CHECK(snap.has_value(), "AC3: snapshot");
        (void)cs.eval("(set-code \"(define x 99)\")");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: re-eval after edit");
        auto restored = cs.eval("(workspace:rollback-to \"pre-2788\")");
        CHECK(restored && is_bool(*restored) && as_bool(*restored),
              "AC3: rollback-to by name returns #t");
    }

    // ── AC4: missing name → not-found merr (not silent #f) ──
    {
        std::println("\n--- AC4: missing name is typed not-found ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define y 2)\")").has_value(), "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        (void)cs.eval("(workspace:snapshot \"exists-2788\")");
        auto miss = cs.eval("(workspace:rollback-to \"no-such-snap-2788\")");
        CHECK(miss.has_value(), "AC4: returns a value");
        CHECK(!(is_bool(*miss) && !as_bool(*miss)), "AC4: not silent bare #f");
        CHECK(is_pair(*miss), "AC4: merr pair");
        CHECK(merr_kind(cs, *miss) == "not-found", "AC4: kind == not-found");
    }

    // ── AC5: concurrent-delete path is wired (source + out-of-range id) ──
    {
        std::println("\n--- AC5: concurrent-delete wiring + out-of-range id ---");
        // snapshot_names_/sources_ are private; live pair-drift injection is
        // not available. Verify the drift branch exists in source and that
        // an out-of-range numeric id is typed not-found (not silent #f).
        auto src = read_file("src/compiler/evaluator_primitives_workspace.cpp");
        auto win = rollback_to_window(src);
        CHECK(win.find("concurrent-delete") != std::string::npos,
              "AC5: concurrent-delete kind string in prim");
        CHECK(win.find("vector pair drift") != std::string::npos ||
                  win.find("snapshot_sources_ shorter") != std::string::npos,
              "AC5: pair-drift error text present");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define z 3)\")").has_value(), "AC5: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC5: eval");
        auto bad = cs.eval("(workspace:rollback-to 999999)");
        CHECK(bad.has_value(), "AC5: out-of-range id returns value");
        CHECK(!(is_bool(*bad) && !as_bool(*bad)), "AC5: not silent bare #f");
        CHECK(is_pair(*bad) && merr_kind(cs, *bad) == "not-found",
              "AC5: out-of-range id → not-found merr");
    }

    std::println("\n=== #2788 rollback-to: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workspace_rollback_to();
}
#endif
