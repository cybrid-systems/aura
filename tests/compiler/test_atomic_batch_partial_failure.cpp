// @category: unit
// @reason: Issue #2790 — mutate:atomic-batch sub-op failure must set
// guard_ok=false (not only ok) so MutationBoundaryGuard rolls back the
// whole batch; partial prefix commits are forbidden.
//
//   AC1: source has mark_sub_op_failed / guard_ok with ok on failure path
//   AC2: first rebind + second fail → first rebind rolled back (atomicity)
//   AC3: failure returns batch-failed merr (not bare #t)
//   AC4: this suite + linter; no docs/design/2790-*; no test_issue_2790.cpp

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
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
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

static std::string atomic_batch_window(const std::string& src) {
    auto pos = src.find("mutate:atomic-batch");
    if (pos == std::string::npos)
        return {};
    // Prefer the add("mutate:atomic-batch" registration body.
    auto add_pos = src.find("add(\"mutate:atomic-batch\"");
    if (add_pos != std::string::npos)
        pos = add_pos;
    auto end = src.find("typed-mutate-atomic", pos);
    if (end == std::string::npos)
        end = pos + 8000;
    return src.substr(pos, end - pos);
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

} // namespace

int run_test_atomic_batch_partial_failure() {
    std::println("=== Issue #2790: atomic-batch partial failure rolls back ===");
    CHECK(true, "ac2790: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: mark_sub_op_failed sets guard_ok ---");
        auto src = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(!src.empty(), "AC1: mutate primitives readable");
        auto win = atomic_batch_window(src);
        CHECK(!win.empty(), "AC1: atomic-batch window");
        CHECK(win.find("Issue #2790") != std::string::npos, "AC1: cites #2790");
        CHECK(win.find("mark_sub_op_failed") != std::string::npos,
              "AC1: mark_sub_op_failed helper");
        CHECK(win.find("guard_ok = false") != std::string::npos, "AC1: guard_ok = false present");
        // Helper body sets both flags.
        auto hpos = win.find("mark_sub_op_failed");
        CHECK(hpos != std::string::npos, "AC1: helper pos");
        auto hwin = win.substr(hpos, 400);
        CHECK(hwin.find("ok = false") != std::string::npos, "AC1: helper sets ok");
        CHECK(hwin.find("guard_ok = false") != std::string::npos, "AC1: helper sets guard_ok");
        // Bool-false path uses the helper (not ok-only).
        CHECK(win.find("mark_sub_op_failed()") != std::string::npos,
              "AC1: failure path calls helper");
    }

    // ── AC2/AC3: live partial failure atomicity ──
    {
        std::println("\n--- AC2/AC3: first rebind rolled back when second fails ---");
        CompilerService cs;
        CHECK(
            cs.eval("(set-code \"(define f (lambda () 1)) (define g (lambda () 2))\")").has_value(),
            "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");

        // Second op: rebind unknown name → EvalResult unexpected → batch fail.
        auto batch = cs.eval("(mutate:atomic-batch "
                             "(list (list \"mutate:rebind\" \"f\" \"(lambda () 99)\") "
                             "      (list \"mutate:rebind\" \"no-such-2790\" \"(lambda () 0)\")))");
        CHECK(batch.has_value(), "AC3: batch returns a value");
        // Must not succeed as bare #t.
        CHECK(!(is_bool(*batch) && as_bool(*batch)), "AC3: batch not success #t");
        CHECK(is_pair(*batch), "AC3: batch-failed merr pair");
        CHECK(merr_kind(cs, *batch) == "batch-failed", "AC3: kind == batch-failed");

        // f must still be original (first rebind rolled back).
        auto f = cs.eval("(begin (eval-current) (f))");
        CHECK(f && is_int(*f) && as_int(*f) == 1, "AC2: f still 1 after partial batch rollback");
        auto g = cs.eval("(g)");
        CHECK(g && is_int(*g) && as_int(*g) == 2, "AC2: g unchanged");
    }

    // ── AC2b: happy path still commits ──
    {
        std::println("\n--- AC2b: happy path still commits both rebinds ---");
        CompilerService cs;
        CHECK(
            cs.eval("(set-code \"(define a (lambda () 3)) (define b (lambda () 4))\")").has_value(),
            "AC2b: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2b: eval");
        auto batch = cs.eval("(mutate:atomic-batch "
                             "(list (list \"mutate:rebind\" \"a\" \"(lambda () 30)\") "
                             "      (list \"mutate:rebind\" \"b\" \"(lambda () 40)\")))");
        CHECK(batch && is_bool(*batch) && as_bool(*batch), "AC2b: happy #t");
        auto a = cs.eval("(begin (eval-current) (a))");
        auto b = cs.eval("(b)");
        CHECK(a && is_int(*a) && as_int(*a) == 30, "AC2b: a rebound");
        CHECK(b && is_int(*b) && as_int(*b) == 40, "AC2b: b rebound");
    }

    std::println("\n=== #2790 atomic-batch partial failure: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_atomic_batch_partial_failure();
}
#endif
