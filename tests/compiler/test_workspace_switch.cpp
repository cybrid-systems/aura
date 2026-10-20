// @category: unit
// @reason: Issue #2785 — workspace:switch single bind block (COW epoch
// sync on every switch; no duplicate incomplete-refactor assign).
//
//   AC1: switch binds flat/pool + set_workspace_cow_epoch in one block
//   AC2: no second wt->active() / re-assign within the switch prim
//   AC3: live switch still returns #t and changes :current
//   AC4: COW epoch on flat matches workspace node after switch
//   AC5: this suite + linter; no docs/design/2785-*; no test_issue_2785.cpp
//
//   AC7 (#4632): after ONE mutate:rebind in a child workspace the
//        per-switch cost stays flat (it used to grow with the switch count)
//   AC6 (#4630): the workspace compile of a call chain carries ONE IR
//        function per callee, not 2^depth duplicate copies, and the compile
//        cost does not double per chain level

#include "test_harness.hpp"

#include <algorithm>
#include <chrono>
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

// Issue #4332: child-layer structural mutate must not write the shared
// parent FlatAST. workspace:switch keeps has_own_flat==false and points
// workspace_flat_ at the parent; before the wrapper-level trigger_lazy_cow
// only rebind / replace-subtree cloned, so a child-layer rename-symbol
// committed onto the parent and the parent's next eval observed the child
// edit (P0 isolation face).
static void ac4332_1_child_mutate_cow_isolates_parent() {
    std::println("\n--- #4332 AC1: child-layer mutate clones before write ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define B (lambda () 7))\")").has_value(),
          "4332 AC1: set-code define on root");
    CHECK(cs.eval("(eval-current)").has_value(), "4332 AC1: eval stores root bundle");
    const auto created = cs.eval("(workspace :create \"ac4332\")");
    CHECK(created.has_value(), "4332 AC1: workspace:create");
    std::int64_t wid = -1;
    if (created && is_int(*created))
        wid = as_int(*created);
    CHECK(wid > 0, "4332 AC1: child workspace id");
    CHECK(cs.eval(std::format("(workspace :switch {})", wid)).has_value(),
          "4332 AC1: switch to child (shared flat, has_own_flat=false)");

    // Previously-uncovered structural op (not rebind / replace-subtree):
    // the wrapper-level lazy COW must clone before the body writes.
    auto mr = cs.typed_mutate("(mutate:rename-symbol \"B\" \"BC\" \"ac4332 child rename\")");
    CHECK(mr.success, "4332 AC1: child-layer rename-symbol succeeds (COW cloned)");
    CHECK(!mr.success || mr.error.empty(), "4332 AC1: no mutation error string");

    CHECK(cs.eval("(workspace :switch 0)").has_value(), "4332 AC1: switch back to root");
    // Parent must NOT observe the child edit: B still evaluates on root.
    auto b_eval = cs.eval("(B)");
    CHECK(b_eval.has_value() && is_int(*b_eval) && as_int(*b_eval) == 7,
          "4332 AC1: parent B untouched (rename stayed in the child layer)");
}

// Issue #4630: the workspace compile inlined a callee's cached IR bundle at
// EVERY call site, so a define whose body calls the next define twice
// carried 2^depth copies of the same functions. Every later step (the
// per-function coercion elimination, the source->IR map rebuild, the bundle
// copies into the IR cache) was then O(2^depth) and the workspace set-code /
// eval-current / mutate:rebind cost doubled per chain level - even though the
// call graph has only "depth" edges. The bundle now carries one copy per
// callee.
static std::string ac4630_chain_source(int depth) {
    std::string src = "(define k0 (lambda (x) (+ x 1)))\n";
    for (int j = 1; j <= depth; ++j) {
        src +=
            std::format("(define k{} (lambda (x) (+ (k{} x) (k{} (+ x 1)))))\n", j, j - 1, j - 1);
    }
    return src;
}

// The chain source goes into an Aura string literal: newlines are escaped.
static std::string ac4630_set_code_form(const std::string& src) {
    std::string escaped;
    escaped.reserve(src.size() + 16);
    for (const char c : src) {
        if (c == '\n')
            escaped += "\\n";
        else if (c == '"')
            escaped += "\\\"";
        else
            escaped += c;
    }
    return std::format("(set-code \"{}\")", escaped);
}

// k0(x) = x+1, k_n(x) = k_{n-1}(x) + k_{n-1}(x+1) => k_n(x) = 2^n * x + b_n.
static long long ac4630_chain_value(int depth, long long x) {
    long long a = 1;
    long long b = 1;
    for (int n = 1; n <= depth; ++n) {
        const long long na = a * 2;
        const long long nb = b * 2 + a;
        a = na;
        b = nb;
    }
    return a * x + b;
}

static void ac4630_workspace_compile_is_not_exponential() {
    std::println("\n--- AC6 (#4630): the workspace compile carries one copy per callee ---");
    constexpr int kDepth = 14;
    CompilerService cs;
    CHECK(cs.eval(ac4630_set_code_form(ac4630_chain_source(kDepth))).has_value(),
          "4630 AC6: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4630 AC6: eval-current");
    const auto* entry = cs.get_define_v2(std::format("k{}", kDepth));
    CHECK(entry != nullptr, "4630 AC6: the top define has an IR cache entry");
    if (entry != nullptr) {
        const auto functions = entry->irs.size();
        std::println("4630 AC6: IR bundle functions = {}", functions);
        // One function per define in the chain (plus room for the entry), NOT
        // 2^14 = 16384 copies of the same callee body.
        CHECK(functions <= static_cast<std::size_t>(kDepth) + 2,
              "4630 AC6: the IR bundle must carry one function per callee, not 2^depth copies");
    }
    // The chain still evaluates to the same values.
    {
        constexpr int kEvalDepth = 5;
        CompilerService ev;
        CHECK(ev.eval(ac4630_set_code_form(ac4630_chain_source(kEvalDepth))).has_value(),
              "4630 AC6: set-code (value check)");
        CHECK(ev.eval("(eval-current)").has_value(), "4630 AC6: eval-current (value check)");
        for (int n = 0; n <= kEvalDepth; ++n) {
            const auto res = ev.eval(std::format("(k{} 10)", n));
            CHECK(res.has_value(), std::format("4630 AC6: k{} evaluates", n));
            if (!res.has_value() || !is_int(*res))
                continue;
            CHECK(as_int(*res) == ac4630_chain_value(n, 10),
                  std::format("4630 AC6: k{}(10) == {}", n, ac4630_chain_value(n, 10)));
        }
    }
    // The compile cost does not double per chain level.
    {
        auto compile_ms = [](int depth) -> double {
            double best = 1e18;
            for (int attempt = 0; attempt < 3; ++attempt) {
                CompilerService svc;
                const auto t0 = std::chrono::steady_clock::now();
                if (!svc.eval(ac4630_set_code_form(ac4630_chain_source(depth))).has_value())
                    return 1e18;
                if (!svc.eval("(eval-current)").has_value())
                    return 1e18;
                const auto t1 = std::chrono::steady_clock::now();
                const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                best = std::min(best, ms);
            }
            return best;
        };
        const double shallow = compile_ms(8);
        const double deep = compile_ms(14);
        std::println("4630 AC6: depth 8 = {:.2} ms, depth 14 = {:.2} ms", shallow, deep);
        CHECK(shallow > 0.0, "4630 AC6: the shallow compile was measured");
        // 2^6 = 64x for the unfixed path; the fixed path is flat. 10x leaves
        // room for a loaded machine in both directions.
        CHECK(deep < shallow * 10.0,
              "4630 AC6: the depth-14 compile must not cost 2^6 times the depth-8 compile");
    }
}
// Issue #4632: ONE mutate:rebind in a child workspace made every later
// workspace :switch re-scan the whole top environment. A switch into a
// rebound workspace re-binds that workspace's defines, and every re-bind
// appends to the env's binding vector, so the env grew with the number of
// switches while the compiler service re-walked all of it on every switch.
// A multi-tenant switch loop therefore became quadratic.
static std::string ac4632_switch_loop_script(int trips) {
    return std::format(R"(
(set-code "(define ttl (lambda (cat) 0))\n(define admit (lambda (cat src) #t))")
(eval-current)
(define w (workspace :create "c1"))
(workspace :switch w)
(eval-current)
(mutate:rebind "ttl" "(lambda (cat) 7)" "r")
(eval-current)
(workspace :switch 0)
(eval-current)
(define t0 (current-time-ms))
(let loop ((i 0)) (if (< i {}) (begin (workspace :switch w) (workspace :switch 0) (loop (+ i 1)))))
(define t1 (current-time-ms))
(display (list "trips" {} "ms" (- t1 t0)))
(newline)
)",
                 trips, trips);
}

static void ac4632_switch_cost_stays_flat() {
    std::println("\n--- AC7 (#4632): the per-switch cost stays flat after one rebind ---");
    // The rebound child still evaluates its defines after many switches.
    {
        CompilerService cs;
        CHECK(cs.eval(ac4632_switch_loop_script(50)).has_value(), "4632 AC7: the switch loop runs");
        const auto created = cs.eval("(workspace :create \"ac4632\")");
        CHECK(created.has_value(), "4632 AC7: workspace :create");
        if (!created.has_value() || !is_int(*created))
            return;
        const auto id = as_int(*created);
        CHECK(cs.eval(std::format("(workspace :switch {})", id)).has_value(), "4632 AC7: switch");
        CHECK(cs.eval("(eval-current)").has_value(), "4632 AC7: eval-current");
        CHECK(cs.eval("(mutate:rebind \"ttl\" \"(lambda (cat) 7)\" \"r\")").has_value(),
              "4632 AC7: mutate:rebind");
        CHECK(cs.eval("(eval-current)").has_value(), "4632 AC7: eval-current after rebind");
        for (int i = 0; i < 20; ++i) {
            CHECK(cs.eval(std::format("(workspace :switch {})", id)).has_value(),
                  "4632 AC7: switch back to the child");
            CHECK(cs.eval("(workspace :switch 0)").has_value(), "4632 AC7: switch to the root");
        }
        CHECK(cs.eval(std::format("(workspace :switch {})", id)).has_value(),
              "4632 AC7: switch to the child");
        const auto res = cs.eval("(ttl \"cat\")");
        CHECK(res.has_value(), "4632 AC7: the rebound define evaluates");
        if (res.has_value() && is_int(*res))
            CHECK(as_int(*res) == 7, "4632 AC7: the rebound define returns the rebound body");
    }
    // The per-switch cost does not grow with the number of switches.
    {
        constexpr int kSmall = 500;
        constexpr int kLarge = 4000;
        double small_ms = 1e18;
        double large_ms = 1e18;
        for (int attempt = 0; attempt < 3; ++attempt) {
            for (const int trips : {kSmall, kLarge}) {
                CompilerService cs;
                const auto t0 = std::chrono::steady_clock::now();
                const auto res = cs.eval(ac4632_switch_loop_script(trips));
                const auto t1 = std::chrono::steady_clock::now();
                if (!res.has_value())
                    continue;
                const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                if (trips == kSmall)
                    small_ms = std::min(small_ms, ms);
                else
                    large_ms = std::min(large_ms, ms);
            }
        }
        const double small_per_trip = small_ms / static_cast<double>(kSmall);
        const double large_per_trip = large_ms / static_cast<double>(kLarge);
        std::println("4632 AC7: {} trips -> {:.4} ms/trip, {} trips -> {:.4} ms/trip", kSmall,
                     small_per_trip, kLarge, large_per_trip);
        CHECK(small_per_trip > 0.0, "4632 AC7: the small loop was measured");
        // The unfixed path re-scanned the whole env per switch, so the
        // per-trip cost grew with the number of switches (quadratic
        // overall). The fixed path keeps it flat. 2.5x leaves room for a
        // loaded machine.
        CHECK(large_per_trip < small_per_trip * 2.5,
              "4632 AC7: the per-switch cost must not grow with the number of switches");
    }
}

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

    std::println("\n=== Issue #4332: child-layer mutate COW isolation ===");
    ac4332_1_child_mutate_cow_isolates_parent();

    std::println("\n=== Issue #4630: workspace compile call-chain memoization ===");
    ac4630_workspace_compile_is_not_exponential();

    std::println("\n=== Issue #4632: workspace switch cost after one rebind ===");
    ac4632_switch_cost_stays_flat();

    std::println("\n=== #2785 workspace:switch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workspace_switch();
}
#endif
