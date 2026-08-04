// @category: unit
// @reason: Issue #2131 — unify GC-root pin → cascade → audit state machine
// across invalidate / soft-dirty / boundary / hot-swap / compact.
//
//   AC1: source cites #2131; GcCoordScope + PrePin/Cascade/PostAudit order
//   AC2: soft mark_define_dirty + hard invalidate open SoftDirty/Invalidate paths
//   AC3: reverse-order transition bumps phase_violations + reverse_order
//   AC4: metrics schema-2131 on query:linear-postmutate-fidelity-stats
//   AC5: compact_env_frames advances Compact path counters
//   AC6: no missing-post-audit on happy-path soft/hard invalidate

#include "test_harness.hpp"

#include "compiler/gc_coord_scope.h"
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
        "(hash-ref (engine:metrics \"query:linear-postmutate-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_gc_coord_scope() {
    std::println("=== Issue #2131: GcCoordScope pin → cascade → audit ===");

    // ── AC1: source ──
    {
        std::println("\n--- AC1: source ---");
        auto hdr = read_file("src/compiler/gc_coord_scope.h");
        auto dirty = read_file("src/compiler/service_dirty.cpp");
        auto lock = read_file("src/compiler/lock_order_audit.h");
        auto env = read_file("src/compiler/evaluator_env.cpp");
        auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(!hdr.empty() && hdr.find("Issue #2131") != std::string::npos, "gc_coord_scope.h");
        CHECK(hdr.find("PrePin") != std::string::npos && hdr.find("PostAudit") != std::string::npos,
              "phases named");
        CHECK(dirty.find("gc_coord::Scope") != std::string::npos, "dirty Scope");
        CHECK(dirty.find("Path::Invalidate") != std::string::npos ||
                  dirty.find("Path::SoftDirty") != std::string::npos,
              "soft/hard paths");
        CHECK(lock.find("#2131") != std::string::npos, "lock_order #2131");
        CHECK(env.find("Path::Compact") != std::string::npos, "compact path");
        CHECK(bound.find("Path::Boundary") != std::string::npos, "boundary path");
    }

    // ── AC2: soft + hard open scopes ──
    {
        std::println("\n--- AC2: soft + hard paths ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda (x) x)) (define g (lambda () (f 1)))\")")
                  .has_value(),
              "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        const auto open0 = cs.public_gc_coord_scopes_opened();
        const auto soft0 = cs.public_gc_coord_scopes_for_path(
            static_cast<std::uint8_t>(aura::compiler::gc_coord::Path::SoftDirty));
        const auto hard0 = cs.public_gc_coord_scopes_for_path(
            static_cast<std::uint8_t>(aura::compiler::gc_coord::Path::Invalidate));
        const auto post0 = cs.public_gc_coord_post_audit_total();
        cs.public_mark_define_dirty("f");
        CHECK(cs.public_gc_coord_scopes_opened() > open0, "scopes opened after soft");
        CHECK(cs.public_gc_coord_scopes_for_path(
                  static_cast<std::uint8_t>(aura::compiler::gc_coord::Path::SoftDirty)) > soft0,
              "soft-dirty path count");
        CHECK(cs.public_gc_coord_post_audit_total() > post0, "post-audit after soft");
        cs.public_invalidate_function("f");
        CHECK(cs.public_gc_coord_scopes_for_path(
                  static_cast<std::uint8_t>(aura::compiler::gc_coord::Path::Invalidate)) > hard0,
              "invalidate path count");
    }

    // ── AC3: reverse-order violation metrics ──
    {
        std::println("\n--- AC3: reverse-order violation ---");
        CompilerService cs;
        const auto viol0 = cs.public_gc_coord_phase_violations();
        const auto rev0 = cs.public_gc_coord_reverse_order();
        cs.public_gc_coord_set_strict(false);
        cs.public_gc_coord_force_reverse_violation();
        CHECK(cs.public_gc_coord_phase_violations() > viol0, "phase violations advanced");
        CHECK(cs.public_gc_coord_reverse_order() > rev0, "reverse_order advanced");
    }

    // ── AC5: compact path ──
    {
        std::println("\n--- AC5: compact path ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval warm");
        const auto c0 = cs.public_gc_coord_scopes_for_path(
            static_cast<std::uint8_t>(aura::compiler::gc_coord::Path::Compact));
        (void)cs.evaluator().compact_env_frames();
        CHECK(cs.public_gc_coord_scopes_for_path(
                  static_cast<std::uint8_t>(aura::compiler::gc_coord::Path::Compact)) > c0,
              "compact path advanced");
    }

    // ── AC6: happy path no missing post-audit ──
    {
        std::println("\n--- AC6: happy path completeness ---");
        const auto miss0 =
            aura::compiler::gc_coord::missing_post_audit_total.load(std::memory_order_relaxed);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define h 1)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        cs.public_mark_define_dirty("h");
        cs.public_invalidate_function("h");
        const auto miss1 =
            aura::compiler::gc_coord::missing_post_audit_total.load(std::memory_order_relaxed);
        CHECK(miss1 == miss0, "no missing post-audit on happy path");
    }

    // ── AC4: query metrics ──
    {
        std::println("\n--- AC4: schema-2131 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
        cs.public_mark_define_dirty("__none__"); // still runs Scope on soft path
        CHECK(href(cs, "schema-2131") == 2131, "schema-2131");
        CHECK(href(cs, "issue-2131") == 2131, "issue-2131");
        CHECK(href(cs, "gc-coord-wired") == 1, "gc-coord-wired");
        CHECK(href(cs, "gc-coord-scopes-opened") >= 0, "scopes-opened key");
        CHECK(href(cs, "gc-coord-post-audit-total") >= 0, "post-audit key");
        CHECK(href(cs, "gc-coord-phase-violations") >= 0, "violations key");
        CHECK(href(cs, "gc-coord-path-soft-dirty") >= 0, "soft-dirty path key");
        CHECK(href(cs, "gc-coord-path-invalidate") >= 0, "invalidate path key");
        CHECK(href(cs, "gc-coord-path-compact") >= 0, "compact path key");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_gc_coord_scope();
}
#endif
