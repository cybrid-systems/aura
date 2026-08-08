// @category: unit
// @reason: Issue #2785 — workspace:switch single bind block (COW epoch
// sync on every switch; no duplicate incomplete-refactor assign).
//
//   AC1: switch binds flat/pool + set_workspace_cow_epoch in one block
//   AC2: no second wt->active() / re-assign within the switch prim
//   AC3: live switch still returns #t and changes :current
//   AC4: COW epoch on flat matches workspace node after switch
//   AC5: this suite + linter; no docs/design/2785-*; no test_issue_2785.cpp

#include "test_harness.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.evaluator;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::WorkspaceTree;
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

int run_test_workspace_switch() {
    std::println("=== Issue #2785: workspace:switch consolidated bind ===");
    CHECK(true, "ac2785: issue stamp");

    // ── AC1/AC2: source shape ──
    {
        std::println("\n--- AC1/AC2: single bind block, no duplicate assign ---");
        auto src = read_file("src/compiler/evaluator_primitives_workspace.cpp");
        CHECK(!src.empty(), "AC1: workspace primitives readable");
        auto pos = src.find("[\"workspace:switch\"]");
        if (pos == std::string::npos)
            pos = src.find("workspace:switch");
        CHECK(pos != std::string::npos, "AC1: workspace:switch present");
        auto end = src.find("[\"workspace:current\"]", pos);
        if (end == std::string::npos)
            end = src.find("workspace:current", pos);
        auto win = src.substr(pos, end == std::string::npos ? 1200 : end - pos);
        CHECK(win.find("Issue #2785") != std::string::npos, "AC1: cites #2785");
        CHECK(win.find("set_workspace_cow_epoch") != std::string::npos,
              "AC1: set_workspace_cow_epoch in switch");
        CHECK(win.find("Issue #738") != std::string::npos, "AC1: #738 lineage retained");
        // Exactly one set of flat/pool assigns (not the old double block).
        int flat_assigns = 0;
        {
            const std::string needle = "ev.workspace_flat_ = ws->flat";
            for (size_t p = 0; (p = win.find(needle, p)) != std::string::npos; p += needle.size())
                ++flat_assigns;
        }
        CHECK(flat_assigns == 1, "AC2: exactly one workspace_flat_ assign from ws->flat");
        // No second wt->active() re-fetch (was the incomplete-refactor smell).
        int active_calls = 0;
        {
            const std::string needle = "wt->active()";
            for (size_t p = 0; (p = win.find(needle, p)) != std::string::npos; p += needle.size())
                ++active_calls;
        }
        CHECK(active_calls == 1, "AC2: exactly one wt->active() in switch body");
        // Declaration is "auto* ws = wt->active()" — that is expected.
        // Forbid a second bare reassignment (old incomplete-refactor smell).
        CHECK(win.find("ws = wt->active()") == win.rfind("ws = wt->active()"),
              "AC2: no second ws = wt->active() re-fetch");
    }

    // ── AC3: live switch behavior ──
    {
        std::println("\n--- AC3: switch returns #t and updates :current ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval-current");
        auto created = cs.eval("(workspace :create \"switch-2785\")");
        CHECK(created && is_int(*created) && as_int(*created) >= 1, "AC3: create child");
        const auto id = as_int(*created);
        auto sw = cs.eval(std::format("(workspace :switch {})", id));
        CHECK(sw && is_bool(*sw) && as_bool(*sw), "AC3: switch #t");
        auto cur = cs.eval("(workspace :current)");
        CHECK(cur && is_int(*cur) && as_int(*cur) == id, "AC3: :current == child id");
        auto sw0 = cs.eval("(workspace :switch 0)");
        CHECK(sw0 && is_bool(*sw0) && as_bool(*sw0), "AC3: switch root #t");
        auto cur0 = cs.eval("(workspace :current)");
        CHECK(cur0 && is_int(*cur0) && as_int(*cur0) == 0, "AC3: :current == 0");
    }

    // ── AC4: COW epoch synced onto flat after switch ──
    {
        std::println("\n--- AC4: flat cow epoch matches node after switch ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        auto created = cs.eval("(workspace :create \"epoch-2785\")");
        CHECK(created && is_int(*created), "AC4: create");
        const auto id = as_int(*created);
        CHECK(cs.eval(std::format("(workspace :switch {})", id)).has_value(), "AC4: switch");
        auto& ev = cs.evaluator();
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree());
        CHECK(wt != nullptr, "AC4: workspace tree");
        auto* ws = wt->active();
        CHECK(ws != nullptr && ws->flat != nullptr, "AC4: active ws + flat");
        CHECK(ws->flat->workspace_cow_epoch() == ws->cow_epoch,
              "AC4: flat epoch == node cow_epoch after switch");
        // Bump node epoch and re-switch to verify set_workspace_cow_epoch runs.
        const auto bumped = ws->cow_epoch + 7;
        ws->cow_epoch = bumped;
        CHECK(cs.eval("(workspace :switch 0)").has_value(), "AC4: switch root");
        CHECK(cs.eval(std::format("(workspace :switch {})", id)).has_value(),
              "AC4: re-switch child");
        ws = wt->active();
        CHECK(ws && ws->flat && ws->flat->workspace_cow_epoch() == bumped,
              "AC4: re-switch re-syncs bumped cow_epoch onto flat");
    }

    std::println("\n=== #2785 workspace:switch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workspace_switch();
}
#endif
