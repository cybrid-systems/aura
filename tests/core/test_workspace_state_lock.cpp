// tests/core/test_workspace_state_lock.cpp — Issue #1994 (F-004), retargeted
// by #5144 at the std replacement.
//
// #1994 fixed the racy NULL-check-then-dereference in the primitives that
// touch ev.workspace_flat_ / ev.workspace_pool_ (shared_lock for readers,
// unique_lock for read+write). #5144 retired the (workspace-state)
// primitive: the snapshot is now the Aura-level ws:state formatter in
// lib/std/workspace.aura, built from (stats:get "ast:defs") and the intend
// timeline. The lock discipline now lives in those primitives; this test
// keeps the runtime smoke + concurrency shape on the new surface.
//
// AC list:
//   AC1: (ws:state) is callable with no workspace AST (same error line)
//   AC2: (ws:state) prints the header + DEFINE lines + timeline section
//   AC3: (ws:state) output is byte-identical to the retired primitive's
//   AC4: Concurrent (ws:state) + (workspace :create) don't crash
//
// (workspace:mutation-count) is registered via
// ObservabilityPrims::register_stats_impl — a stats primitive, NOT
// accessible via cs.eval(); its lock-scope invariant is linter-enforced.

#include "test_harness.hpp"

#include <atomic>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

bool eval_bool(aura::compiler::CompilerService& cs, const char* expr) {
    auto r = cs.eval(expr);
    return r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r);
}

constexpr int kThreads = 4;
constexpr int kIterations = 8;

} // namespace

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;

    // === AC1: no workspace AST → the engine's error line, verbatim ===
    {
        aura::compiler::CompilerService cs;
        CHECK(cs.eval("(require \"std/workspace\" all:)").has_value(),
              "AC1: require std/workspace");
        CHECK(eval_bool(cs, "(string=? (ws:state) \"WORKSPACE-ERROR: no workspace AST loaded\")"),
              "AC1: (ws:state) reports no workspace AST");
    }

    // === AC2: header + DEFINE lines + timeline section ===
    {
        aura::compiler::CompilerService cs;
        CHECK(cs.eval("(require \"std/workspace\" all:)").has_value(),
              "AC2: require std/workspace");
        CHECK(cs.eval("(set-code \"(define wsx 42) (define wsy 7)\")").has_value(),
              "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval-current");
        auto r_ws = cs.eval("(ws:state)");
        CHECK(r_ws.has_value(), "AC2: (ws:state) returns after populated state");
        CHECK(eval_bool(cs, "(and (= (string-index (ws:state) \"WORKSPACE: \") 0) "
                            "(not (= (string-index (ws:state) \"DEFINE: wsx\") -1)) "
                            "(not (= (string-index (ws:state) \"MUTATIONS (last 10):\") -1)))"),
              "AC2: header + DEFINE + MUTATIONS sections");
    }

    // === AC3: byte-identical to the retired primitive (#5144) ===
    // The primitive still exists on this commit; it is deregistered only
    // after the std formatter is proven, so the two must agree exactly.
    {
        aura::compiler::CompilerService cs;
        CHECK(cs.eval("(require \"std/workspace\" all:)").has_value(),
              "AC3: require std/workspace");
        CHECK(cs.eval("(set-code \"(define wa 1) (define wb 2)\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval-current");
        // Golden text: the retired primitive's exact bytes (#5144).
        CHECK(eval_bool(cs, "(string=? (ws:state) \"WORKSPACE: 2 defines\\nDEFINE: wa\\n"
                            "DEFINE: wb\\nMUTATIONS (last 10):\\n  (none)\\n\")"),
              "AC3: ws:state text is the retired primitive's bytes");
        CHECK(eval_bool(cs, "(not (string=? (ws:state) \"\"))"), "AC3: snapshot non-empty");
    }

    // === AC3b: no workspace AST -> same error line ===
    {
        aura::compiler::CompilerService cs;
        CHECK(cs.eval("(require \"std/workspace\" all:)").has_value(), "AC3b: require");
        CHECK(eval_bool(cs, "(string=? (ws:state) \"WORKSPACE-ERROR: no workspace AST loaded\")"),
              "AC3b: ws:state reports no workspace AST");
    }

    // === AC3c: timeline tail (last 10, index-prefixed) ===
    // An intend run with an empty generator pushes "start:<goal>" plus one
    // "attempt_N:empty from generator" per attempt, so 12 attempts give a
    // 14-entry timeline: the tail must start at absolute index 4 and keep
    // the engine's "  <i>:<line>" shape.
    {
        aura::compiler::CompilerService cs;
        CHECK(cs.eval("(require \"std/workspace\" all:)").has_value(), "AC3c: require");
        CHECK(cs.eval("(set-code \"(define ta 1)\")").has_value(), "AC3c: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3c: eval-current");
        CHECK(cs.eval("(define (gen5144 g) \"\")").has_value(), "AC3c: empty generator");
        CHECK(cs.eval("(define (ver5144 code) #f)").has_value(), "AC3c: false verifier");
        CHECK(cs.eval("(intend \"seed\" gen5144 ver5144 12)").has_value(), "AC3c: intend run");
        CHECK(eval_bool(cs, "(not (= (string-index (intend-history) \"(empty)\") 0))"),
              "AC3c: timeline has entries");
        // Golden header + first three tail lines (indices 4..6 of 14).
        CHECK(eval_bool(cs, "(= (string-index (ws:state) \"WORKSPACE: 1 defines\\n"
                            "DEFINE: ta\\nMUTATIONS (last 10):\\n"
                            "  4:attempt_4:empty from generator\\n"
                            "  5:attempt_5:empty from generator\\n"
                            "  6:attempt_6:empty from generator\\n\") 0)"),
              "AC3c: ws:state golden header + timeline tail prefix");
        CHECK(eval_bool(cs, "(= (string-index (ws:state) \"  3:\") -1)"),
              "AC3c: entries before the last 10 are dropped");
        CHECK(eval_bool(cs, "(not (= (string-index (ws:state) \"  4:\") -1))"),
              "AC3c: tail starts at index 4");
        CHECK(eval_bool(cs, "(not (= (string-index (ws:state) \"  13:\") -1))"),
              "AC3c: tail keeps the last entry index");
    }

    // === AC4: concurrent (ws:state) + (workspace :create) don't crash ===
    // Multiple threads hammer the same CompilerService with primitives
    // that take the workspace_mtx_ locks fixed by #1994. With the fix,
    // shared_lock + unique_lock on workspace_mtx_ serialize correctly
    // without deadlock and all operations complete cleanly.
    {
        aura::compiler::CompilerService cs;
        CHECK(cs.eval("(require \"std/workspace\" all:)").has_value(),
              "AC4: require std/workspace");
        std::atomic<int> errors{0};

        std::vector<std::thread> threads;
        threads.reserve(kThreads);

        // Thread A: (ws:state) hammer
        threads.emplace_back([&]() noexcept {
            for (int i = 0; i < kIterations; ++i) {
                try {
                    (void)cs.eval("(ws:state)");
                } catch (...) {
                    ++errors;
                }
            }
        });

        // Thread B: (workspace :create) hammer — writer path (unique_lock)
        threads.emplace_back([&]() noexcept {
            for (int i = 0; i < kIterations; ++i) {
                try {
                    (void)cs.eval("(workspace :create test-ws)");
                } catch (...) {
                    ++errors;
                }
            }
        });

        // Thread C: eval hammer (populates workspace state)
        threads.emplace_back([&]() noexcept {
            for (int i = 0; i < kIterations; ++i) {
                try {
                    (void)cs.eval("(+ 1 1)");
                } catch (...) {
                    ++errors;
                }
            }
        });

        // Thread D: (ws:state) hammer (additional reader)
        threads.emplace_back([&]() noexcept {
            for (int i = 0; i < kIterations; ++i) {
                try {
                    (void)cs.eval("(ws:state)");
                } catch (...) {
                    ++errors;
                }
            }
        });

        for (auto& th : threads)
            th.join();

        CHECK(errors.load() == 0, "AC4: no exceptions under concurrent (ws:state) + "
                                  "(workspace :create) + eval stress");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("issue 1994 workspace-state lock (F-004): OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
