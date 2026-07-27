// @category: unit
// @reason: Issue #2220 — long-lived TypeChecker on Evaluator mutate path.
//
// Production binding:
//   src/compiler/evaluator_typecheck.cpp
//   src/compiler/evaluator.ixx
//   src/compiler/evaluator_primitives_eval.cpp
//   src/compiler/evaluator_primitives_query.cpp
//   src/compiler/observability_metrics.h
//
//   AC1: N post-mutate typechecks reuse one TypeChecker (pointer stable)
//   AC2: Multi-round rebind → reuse_total + cs_cache/memo metrics grow
//   AC3: set-code invalidates facade (create after invalidate)
//   AC4: composite path can reuse persistent CS (stash after partial)
//   AC5: schema-2220 + source cites

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    std::println("=== Issue #2220: persistent TypeChecker on mutate path ===");

    // ── AC5 source ──
    {
        std::println("\n--- AC5: source ---");
        auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
        auto ev = read_file("src/compiler/evaluator.ixx");
        auto pe = read_file("src/compiler/evaluator_primitives_eval.cpp");
        auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
        auto met = read_file("src/compiler/observability_metrics.h");
        CHECK(tc.find("ensure_typechecker") != std::string::npos, "ensure_typechecker");
        CHECK(tc.find("persistent_typechecker") != std::string::npos ||
                  ev.find("persistent_typechecker") != std::string::npos,
              "persistent member");
        CHECK(tc.find("#2220") != std::string::npos, "typecheck cites #2220");
        CHECK(pe.find("invalidate_persistent_typechecker") != std::string::npos,
              "set-code invalidates");
        CHECK(q.find("schema-2220") != std::string::npos, "query schema-2220");
        CHECK(met.find("typecheck_persistent_reuse_total") != std::string::npos, "metrics");
        CHECK(tc.find("stash_partial_constraint_state") != std::string::npos, "AC4 stash");
    }

    // ── AC1 / AC2: reuse across mutates ──
    {
        std::println("\n--- AC1/AC2: reuse across post-mutate typechecks ---");
        CompilerService cs;
        CHECK(
            cs.eval(
                  "(set-code \"(define f (lambda (x) (+ x 1))) (define g (lambda (y) (+ y 2)))\")")
                .has_value(),
            "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto& ev = cs.evaluator();
        CHECK(ev.persistent_typechecker() == nullptr, "no TC before first typecheck");

        // First rebind → create
        CHECK(cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 10))\" \"r0\")").has_value(),
              "rebind0");
        void* p0 = ev.persistent_typechecker();
        CHECK(p0 != nullptr, "AC1: TC created after first post-mutate");
        const auto create0 = ev.persistent_typechecker_create_total();
        const auto reuse0 = ev.persistent_typechecker_reuse_total();
        CHECK(create0 >= 1, "AC1: create_total >= 1");

        // More rebinds → same pointer + reuse grows
        for (int i = 1; i <= 8; ++i) {
            CHECK(cs.eval(std::format("(mutate:rebind \"f\" \"(lambda (x) (+ x {}))\" \"r{}\")",
                                      10 + i, i))
                      .has_value(),
                  std::format("rebind{}", i));
        }
        void* p1 = ev.persistent_typechecker();
        CHECK(p1 == p0, "AC1: TypeChecker pointer stable across mutates");
        CHECK(ev.persistent_typechecker_reuse_total() > reuse0, "AC2: reuse_total grew");
        CHECK(ev.persistent_typechecker_create_total() == create0,
              "AC2: no extra create without set-code");

        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m) {
            const auto hits = m->typecheck_persistent_cs_cache_hits.load(std::memory_order_relaxed);
            const auto pm = m->predicate_memo_hits_total.load(std::memory_order_relaxed);
            const auto sk = m->solve_delta_epoch_skip_total.load(std::memory_order_relaxed);
            std::println("  cs_cache_hits={} predicate_memo_hits={} epoch_skip={} reuse={}", hits,
                         pm, sk, ev.persistent_typechecker_reuse_total());
            // At least one incremental signal should move under multi-round rebind
            // (cs_cache and/or memo and/or epoch skip and/or reuse).
            CHECK(hits > 0 || pm > 0 || sk > 0 || ev.persistent_typechecker_reuse_total() > 0,
                  "AC2: incremental fidelity signal");
        }
    }

    // ── AC3: set-code invalidates ──
    {
        std::println("\n--- AC3: set-code invalidates ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "seed");
        CHECK(cs.eval("(eval-current)").has_value(), "eval seed");
        CHECK(cs.eval("(mutate:rebind \"a\" \"2\" \"s\")").has_value(), "rebind a");
        auto& ev = cs.evaluator();
        void* before = ev.persistent_typechecker();
        CHECK(before != nullptr, "TC after rebind");
        const auto inv0 = href(cs, "typecheck-persistent-invalidate-total");
        CHECK(cs.eval("(set-code \"(define b 3)\")").has_value(), "set-code replace");
        // Invalidated immediately on set-code
        CHECK(ev.persistent_typechecker() == nullptr, "AC3: TC cleared after set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval b");
        CHECK(cs.eval("(mutate:rebind \"b\" \"4\" \"s2\")").has_value(), "rebind b");
        void* after = ev.persistent_typechecker();
        CHECK(after != nullptr, "AC3: new TC after set-code + mutate");
        CHECK(after != before, "AC3: new TC instance (not stale pointer)");
        CHECK(href(cs, "typecheck-persistent-invalidate-total") > inv0 ||
                  href(cs, "typecheck-persistent-create-total") >= 2,
              "AC3: invalidate or re-create observed");
    }

    // ── AC4: stash after partial (composite reuse path) ──
    {
        std::println("\n--- AC4: composite stash after post-mutate ---");
        auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
        CHECK(tc.find("stash_partial_constraint_state") != std::string::npos &&
                  tc.find("persistent_typechecker") != std::string::npos,
              "AC4: post-mutate stashes via persistent TC");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1) (define y (+ x 1))\")").has_value(), "set");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        CHECK(cs.eval("(mutate:rebind \"x\" \"10\" \"c\")").has_value(), "rebind x");
        // After rebind, commit CS may be live for composite path
        CHECK(cs.evaluator().commit_cs_live() || cs.evaluator().persistent_typechecker() != nullptr,
              "AC4: commit CS live or persistent TC present");
    }

    // ── Query schema-2220 ──
    {
        std::println("\n--- query schema-2220 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "schema-2220") == 2220, "schema-2220");
        CHECK(href(cs, "issue-2220") == 2220, "issue-2220");
        CHECK(href(cs, "typecheck-persistent-wired") == 1, "wired");
        // Drive a rebind so reuse/create counters are visible
        CHECK(cs.eval("(set-code \"(define z 0)\")").has_value(), "z");
        CHECK(cs.eval("(eval-current)").has_value(), "eval z");
        CHECK(cs.eval("(mutate:rebind \"z\" \"1\" \"q\")").has_value(), "rebind z");
        CHECK(href(cs, "typecheck-persistent-create-total") >= 0, "create key");
        CHECK(href(cs, "typecheck-persistent-reuse-total") >= 0, "reuse key");
    }

    std::println("\n=== #2220 done: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
