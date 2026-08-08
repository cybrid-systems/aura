// @category: unit
// @reason: Issue #2784 — workspace:sync-from must rebind the *actual*
// source body, never a hardcoded identity lambda.
//
//   AC1: sync-from source body is unparsed from source workspace define
//   AC2: after sync, my-fn applies as (* x 2) not identity (5 → 10)
//   AC3: no hardcoded "(lambda (x) x)" in sync-from path
//   AC4: fail closed on missing symbol / invalid source
//   AC5: this suite + linter; no docs/design/2784-*; no test_issue_2784.cpp

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

} // namespace

int run_test_workspace_sync_from() {
    std::println("=== Issue #2784: workspace:sync-from actual body ===");
    CHECK(true, "ac2784: issue stamp");

    // ── AC3/AC5: source-cite — no hardcoded identity in sync-from ──
    {
        std::println("\n--- AC3: no hardcoded identity lambda in sync-from ---");
        auto src = read_file("src/compiler/evaluator_primitives_workspace.cpp");
        CHECK(!src.empty(), "AC3: workspace primitives readable");
        CHECK(src.find("workspace:sync-from") != std::string::npos, "AC3: sync-from present");
        CHECK(src.find("Issue #2784") != std::string::npos, "AC3: cites #2784");
        CHECK(src.find("unparse_node") != std::string::npos, "AC3: unparse_node for body");
        // The identity stub must not appear as the rebind code source.
        // Allow the string only if it is in a comment about the bug.
        auto pos = src.find("workspace:sync-from");
        CHECK(pos != std::string::npos, "AC3: sync-from site");
        // Window covering the sync-from prim body (until discard prim).
        auto win_end = src.find("workspace:discard", pos);
        auto win = src.substr(pos, win_end == std::string::npos ? 4000 : win_end - pos);
        CHECK(win.find("std::string(\"(lambda (x) x)\")") == std::string::npos &&
                  win.find("\"(lambda (x) x)\"") == std::string::npos,
              "AC3: no hardcoded (lambda (x) x) as rebind code");
        CHECK(win.find("ev.workspace_flat_->root = saved_root") == std::string::npos,
              "AC3: no discard-parse fallback that restored root");
    }

    // ── AC1/AC2: live cross-workspace sync with non-identity body ──
    {
        std::println("\n--- AC1/AC2: sync-from multiplies, not identity ---");
        CompilerService cs;
        // Root workspace: define my-fn as double.
        CHECK(cs.eval("(set-code \"(define my-fn (lambda (x) (* x 2)))\")").has_value(),
              "AC1: set-code root");
        CHECK(cs.eval("(eval-current)").has_value(), "AC1: eval-current root");
        auto r0 = cs.eval("(my-fn 5)");
        CHECK(r0 && is_int(*r0) && as_int(*r0) == 10, "AC1: root my-fn doubles");

        // Child workspace starts as COW of root (has my-fn).
        auto created = cs.eval("(workspace :create \"ws-sync-target\")");
        CHECK(created && is_int(*created) && as_int(*created) >= 1, "AC1: create child ws");
        const auto child_id = as_int(*created);

        // Mutate root to a different body so child (COW) still has *2 until sync?
        // Better setup: child has identity, root has *2, sync-from root into child.
        // After create, switch to child and rebind my-fn to identity (simulate wrong state).
        CHECK(cs.eval(std::format("(workspace :switch {})", child_id)).has_value(),
              "AC1: switch to child");
        CHECK(cs.eval("(mutate:rebind \"my-fn\" \"(lambda (x) x)\" \"stub-id\")").has_value(),
              "AC1: child temporarily identity");
        auto r_id = cs.eval("(my-fn 5)");
        // May need eval-current after rebind in some configurations.
        (void)cs.eval("(eval-current)");
        r_id = cs.eval("(my-fn 5)");
        CHECK(r_id && is_int(*r_id) && as_int(*r_id) == 5,
              "AC1: child my-fn is identity before sync");

        // Root (id 0) still has *2 — switch back and verify, then sync into child.
        CHECK(cs.eval("(workspace :switch 0)").has_value(), "AC1: switch root");
        (void)cs.eval("(eval-current)");
        auto r_root = cs.eval("(my-fn 5)");
        CHECK(r_root && is_int(*r_root) && as_int(*r_root) == 10, "AC1: root still doubles");

        CHECK(cs.eval(std::format("(workspace :switch {})", child_id)).has_value(),
              "AC1: switch child for sync");
        auto ok = cs.eval("(workspace:sync-from 0 \"my-fn\")");
        CHECK(ok && is_bool(*ok) && as_bool(*ok), "AC2: sync-from returns #t");
        (void)cs.eval("(eval-current)");
        auto r_sync = cs.eval("(my-fn 5)");
        CHECK(r_sync && is_int(*r_sync) && as_int(*r_sync) == 10,
              "AC2: after sync-from, my-fn doubles (not identity)");
    }

    // ── AC4: fail closed ──
    {
        std::println("\n--- AC4: fail closed on missing symbol ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        auto created = cs.eval("(workspace :create \"ws-empty-sym\")");
        CHECK(created && is_int(*created), "AC4: create");
        CHECK(cs.eval(std::format("(workspace :switch {})", as_int(*created))).has_value(),
              "AC4: switch");
        auto miss = cs.eval("(workspace:sync-from 0 \"no-such-symbol\")");
        CHECK(miss && is_bool(*miss) && !as_bool(*miss), "AC4: missing symbol → #f");
        auto bad = cs.eval("(workspace:sync-from 99 \"a\")");
        CHECK(bad && is_bool(*bad) && !as_bool(*bad), "AC4: invalid source id → #f");
    }

    std::println("\n=== #2784 workspace:sync-from: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workspace_sync_from();
}
#endif
