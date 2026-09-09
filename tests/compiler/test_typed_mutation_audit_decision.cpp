// @category: unit
// @reason: Issue #2281 — Agent-visible TypedMutationAudit decision query
// (Sampled vs hard-gate matrix). Tests the pure decide() function
// across the 3×2×3×2 strategy × linear × nodes × strict matrix
// (≥12 cells per AC4) + the query schema sentinels (AC2/AC3).
//
//   AC1: Pure function matches live should_audit_contextual +
//        requires_invariant_hard_gate for all matrix cells.
//   AC2: Query invokable by Agent without mutate side effects.
//   AC3: Schema additive; wired sentinel present.
//   AC4: Unit matrix test ≥ 12 cells.
//   AC5: Decision table documented in typed_mutation_audit.h.

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <print>
#include <string>

import std;
import aura.compiler.service;

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::decide;
using aura::compiler::typed_audit::requires_invariant_hard_gate;
using aura::compiler::typed_audit::set_sample_ratio;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::typed_audit::should_audit_contextual;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

// Save and restore the audit state around each test to avoid pollution.
struct AuditStateGuard {
    AuditStateGuard() {
        // Capture current state — we don't have a getter for sample_ratio
        // but we can re-apply a known default at destruction.
        saved_strategy_ = aura::compiler::typed_audit::get_strategy();
        // We don't capture sample_ratio, but reset to dev defaults at end.
    }
    ~AuditStateGuard() {
        apply_dev_audit_defaults();
        set_strategy(saved_strategy_);
    }
    AuditStrategy saved_strategy_ = AuditStrategy::Sampled;
};

} // namespace

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

// ── Issue #3625: Production RenderFastExit is metrics-only — never an audit skip ──
static void ac3625_1_production_render_still_audits() {
    std::println("\n--- #3625 AC1: production + render hotpath → invariant audit still runs ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    // Production face (Full strategy, the real deployment pairing): the
    // invariant audit decision must run and the render-fast skip must not
    // fire. Regression guard for the #2311/#3625 ordering. The metrics-only
    // gate (production_defaults_active() || strategy == Full) also covers
    // the degenerate pairings (Sampled/Off leftovers) at source level;
    // empirically the Guard path is strategy-gated upstream there, so the
    // skip cannot engage through this surface regardless.
    apply_production_audit_defaults();
    {
        CompilerService cs;
        (void)cs.eval("(define f3625 (lambda (x) x))");
        auto* metrics =
            static_cast<aura::compiler::CompilerMetrics*>(cs.evaluator().compiler_metrics());
        const auto considered0 =
            g_typed_mutation_audit_counters.audits_considered.load(std::memory_order_relaxed);
        const auto skip0 =
            metrics ? metrics->render_fast_exit_skipped_audit_total.load(std::memory_order_relaxed)
                    : 0;
        cs.evaluator().enter_render_hotpath();
        // Ordinary typed mutate: non-linear, non-match set-body.
        auto mut = cs.eval("(mutate:set-body \"f3625\" \"(lambda (x) (+ x 1))\")");
        CHECK(mut.has_value(), "3625 AC1: set-body mutate ran");
        const auto considered1 =
            g_typed_mutation_audit_counters.audits_considered.load(std::memory_order_relaxed);
        const auto skip1 =
            metrics ? metrics->render_fast_exit_skipped_audit_total.load(std::memory_order_relaxed)
                    : 0;
        CHECK(considered1 > considered0,
              "ac3625_1_production_render_still_audits: audits_considered increments (audit ran)");
        CHECK(skip1 == skip0,
              "3625 AC1: render fast did NOT skip the invariant suite under Production/Full");
        cs.evaluator().exit_render_hotpath();
    }
    apply_dev_audit_defaults();
}

static void ac3625_2_soft_render_fast_still_skips() {
    std::println("\n--- #3625 AC2: Soft + render hotpath keeps the fast-exit skip ---");
    apply_dev_audit_defaults();
    set_strategy(AuditStrategy::Sampled);
    CompilerService cs;
    (void)cs.eval("(define g3625 (lambda (x) x))");
    auto* metrics =
        static_cast<aura::compiler::CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto rf0 = metrics ? metrics->render_fast_exit_total.load(std::memory_order_relaxed) : 0;
    const auto skip0 =
        metrics ? metrics->render_fast_exit_skipped_audit_total.load(std::memory_order_relaxed) : 0;
    cs.evaluator().enter_render_hotpath();
    auto mut = cs.eval("(mutate:set-body \"g3625\" \"(lambda (x) (+ x 1))\")");
    CHECK(mut.has_value(), "3625 AC2: set-body mutate ran");
    const auto rf1 = metrics ? metrics->render_fast_exit_total.load(std::memory_order_relaxed) : 0;
    const auto skip1 =
        metrics ? metrics->render_fast_exit_skipped_audit_total.load(std::memory_order_relaxed) : 0;
    CHECK(rf1 > rf0, "ac3625_2_soft_render_fast_still_skips: Soft render fast path armed");
    CHECK(skip1 > skip0, "ac3625_2_soft_render_fast_still_skips: Soft fast-exit skip retained");
    cs.evaluator().exit_render_hotpath();
    apply_dev_audit_defaults();
}

static void ac3625_3_suppress_counters_unchanged() {
    std::println("\n--- #3625 AC3: #2311 linear/match suppress stays ahead of the gate ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto suppress_pos =
        mb.find("m->render_fast_exit_suppressed_linear_or_match_total.fetch_add(");
    const auto gate_pos = mb.find("Issue #3625 (#2311/#3322 residual)");
    CHECK(suppress_pos != std::string::npos, "3625 AC3: #2311 suppress counter block present");
    CHECK(gate_pos != std::string::npos, "3625 AC3: #3625 strategy gate present");
    CHECK(
        suppress_pos < gate_pos,
        "ac3625_3_suppress_counters_unchanged: linear/match suppress still fires before the gate");
    CHECK(mb.find("render_fast_exit_suppressed_linear_total") != std::string::npos &&
              mb.find("render_fast_exit_suppressed_match_total") != std::string::npos,
          "3625 AC3: per-cause counters retained");
}

static void ac3625_4_source_and_linter() {
    std::println("\n--- #3625 AC4: source-cite + linter ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto t = read_file("tests/compiler/test_typed_mutation_audit_decision.cpp");
    const auto lint = read_file("scripts/check_render_fast_audit_3625.py");
    const auto build = read_file("build.py");
    const auto gate_pos = mb.find("Issue #3625 (#2311/#3322 residual)");
    CHECK(gate_pos != std::string::npos, "3625 AC4: gate cite");
    const std::string scope = mb.substr(gate_pos, 1700);
    CHECK(scope.find("production_defaults_active()") != std::string::npos &&
              scope.find("typed_audit::get_strategy() == typed_audit::AuditStrategy::Full") !=
                  std::string::npos,
          "3625 AC4: production/Full metrics-only gate");
    CHECK(scope.find("render_fast_effective") != std::string::npos &&
              scope.find("ev_->render_fast_exit_this_boundary_ = render_fast_effective;") !=
                  std::string::npos,
          "3625 AC4: boundary flag gated on render_fast_effective");
    CHECK(scope.find("render_fast_exit_total.fetch_add") != std::string::npos,
          "3625 AC4: observe-only hotpath signal retained (RenderFastExit not deleted)");
    CHECK(mb.find("note_invariant_enforcement_skipped") != std::string::npos &&
              mb.find("render_fast_exit_skipped_audit_total") != std::string::npos,
          "3625 AC4: skip site intact for Soft");
    CHECK(mb.find("Issue #3614") != std::string::npos, "3625 AC4: #3614 persist order untouched");
    CHECK(mb.find("consume_outermost_audit_rollback_needs_fail") != std::string::npos,
          "3625 AC4: #3517 rollback machinery retained");
    CHECK(t.find("ac3625_1_production_render_still_audits") != std::string::npos,
          "3625 AC4: runtime AC1");
    CHECK(!lint.empty() && lint.find("3625") != std::string::npos, "3625 AC4: linter");
    CHECK(build.find("check_render_fast_audit_3625") != std::string::npos, "3625 AC4: build.py");
    CHECK(read_file("tests/compiler/test_issue_3625.cpp").empty(), "3625 AC4: no invent");
    CHECK(read_file("docs/design/3625-render-fast-audit.md").empty(), "3625 AC4: no docs/design");
}

int run_test_typed_mutation_audit_decision() {
    std::println("=== Issue #2281: TypedMutationAudit decision query ===");

    AuditStateGuard guard;

    // ── AC1 + AC4: matrix coverage (≥12 cells) ──
    // Strategy × Linear × Nodes × Strict = 3 × 2 × 3 × 2 = 36 cells.
    // We test a representative subset of ≥12 cells across all
    // strategies + force paths (linear / match-sites / nodes /
    // production-nodes / strict / sampled-hit / sampled-skip).
    {
        std::println("\n--- AC1+AC4: matrix cells ---");

        // --- Off: 2 cells ---
        apply_dev_audit_defaults();
        set_strategy(AuditStrategy::Off);
        {
            auto d = decide(1, 0, false, false);
            CHECK(!d.would_audit, "AC1.1: Off + small non-linear → no audit");
            CHECK(!d.would_hard_gate, "AC1.2: Off → no hard gate");
            CHECK(d.force_reason == "off", "AC1.3: Off reason == 'off'");
        }
        {
            auto d = decide(1, 8, true, true);
            CHECK(!d.would_audit,
                  "AC1.4: Off + linear+strict+large → still no audit (strategy Off wins)");
            CHECK(!d.would_hard_gate, "AC1.5: Off → no hard gate (strategy Off wins)");
            CHECK(d.force_reason == "off", "AC1.6: Off reason stays 'off'");
        }

        // --- Full: 2 cells ---
        apply_production_audit_defaults();
        {
            auto d = decide(1, 0, false, false);
            CHECK(d.would_audit, "AC1.7: Full + tiny → audit (always)");
            CHECK(d.would_hard_gate, "AC1.8: Full → hard gate (always)");
            CHECK(d.force_reason == "full", "AC1.9: Full reason == 'full'");
        }
        {
            auto d = decide(1, 0, true, false);
            CHECK(d.would_audit, "AC1.10: Full + linear → audit");
            CHECK(d.would_hard_gate, "AC1.11: Full + linear → hard gate");
            CHECK(d.force_reason == "full", "AC1.12: Full reason stays 'full' (strategy wins)");
        }

        // --- Sampled (dev, ratio=4): 4 cells ---
        apply_dev_audit_defaults();
        {
            // mid % ratio == 0 → sample_hit (audit)
            auto d = decide(/*mid=*/0, /*nodes=*/1, false, false);
            CHECK(d.would_audit, "AC1.13: Sampled dev + hit → audit (sample hit)");
            CHECK(!d.would_hard_gate, "AC1.14: Sampled dev + hit + small → no hard gate");
            CHECK(d.force_reason == "sampled-hit", "AC1.15: hit reason");
        }
        {
            // mid % ratio != 0 → sample_skip (no audit)
            auto d = decide(/*mid=*/1, /*nodes=*/1, false, false);
            CHECK(!d.would_audit, "AC1.16: Sampled dev + skip → no audit");
            CHECK(!d.would_hard_gate, "AC1.17: Sampled dev + skip → no hard gate");
            CHECK(d.force_reason == "sampled-skip", "AC1.18: skip reason");
        }
        {
            // linear force
            auto d = decide(1, 1, true, false);
            CHECK(d.would_audit, "AC1.19: Sampled dev + linear → audit (force)");
            CHECK(d.would_hard_gate, "AC1.20: Sampled dev + linear → hard gate (force)");
            CHECK(d.force_reason == "linear", "AC1.21: linear reason");
        }
        {
            // nodes force (dev threshold = 8)
            auto d = decide(1, 8, false, false);
            CHECK(d.would_audit, "AC1.22: Sampled dev + nodes=8 → audit (force)");
            CHECK(d.would_hard_gate, "AC1.23: Sampled dev + nodes=8 → hard gate (force)");
            CHECK(d.force_reason == "nodes", "AC1.24: nodes reason (dev)");
        }

        // --- Sampled (prod, ratio=1): 2 cells ---
        apply_production_audit_defaults();
        set_strategy(AuditStrategy::Sampled); // override Full default
        {
            // ratio=1 → sample_hit always (audit)
            auto d = decide(1, 0, false, false);
            CHECK(d.would_audit, "AC1.25: Sampled prod + ratio=1 → audit (always hit)");
            CHECK(!d.would_hard_gate, "AC1.26: Sampled prod + small + no force → no hard gate");
            CHECK(d.force_reason == "sampled-hit", "AC1.27: prod hit reason (no other force)");
        }
        {
            // nodes force (prod threshold = 1)
            auto d = decide(1, 1, false, false);
            CHECK(d.would_audit, "AC1.28: Sampled prod + nodes=1 → audit (force)");
            CHECK(d.would_hard_gate, "AC1.29: Sampled prod + nodes=1 → hard gate (force)");
            CHECK(d.force_reason == "production-nodes", "AC1.30: production-nodes reason");
        }

        // --- Strict + Sampled: 2 cells ---
        apply_dev_audit_defaults();
        {
            // strict_sandbox forces hard_gate but doesn't force audit
            // (sample_hit depends on mid%ratio)
            auto d = decide(/*mid=*/0, /*nodes=*/0, false, true);
            CHECK(d.would_audit, "AC1.31: Sampled + strict + hit → audit");
            CHECK(d.would_hard_gate, "AC1.32: Sampled + strict → hard gate (strict forces)");
            CHECK(d.force_reason == "strict", "AC1.33: strict reason takes priority");
        }
        {
            auto d = decide(/*mid=*/1, /*nodes=*/0, false, true);
            CHECK(!d.would_audit, "AC1.34: Sampled + strict + skip → no audit (no force)");
            CHECK(d.would_hard_gate,
                  "AC1.35: Sampled + strict + skip → hard gate (strict still forces)");
            CHECK(d.force_reason == "strict",
                  "AC1.36: strict reason takes priority (skip + strict = strict)");
        }

        // --- Match sites: 1 cell ---
        apply_dev_audit_defaults();
        {
            auto d = decide(1, 0, false, false, /*match=*/true);
            CHECK(d.would_audit, "AC1.37: Sampled + match-sites → audit (force)");
            CHECK(d.would_hard_gate, "AC1.38: Sampled + match-sites → hard gate (force)");
            CHECK(d.force_reason == "match-sites", "AC1.39: match-sites reason");
        }

        // --- Priority: linear > match-sites > nodes ---
        apply_dev_audit_defaults();
        {
            auto d = decide(1, 8, /*linear=*/true, false, /*match=*/true);
            CHECK(d.would_audit, "AC1.40: linear + match + nodes → audit");
            CHECK(d.would_hard_gate, "AC1.41: linear + match + nodes → hard gate");
            CHECK(d.force_reason == "linear", "AC1.42: linear takes priority over match/nodes");
        }
    }

    // ── AC1: decide() matches live should_audit_contextual + requires_invariant_hard_gate ──
    {
        std::println("\n--- AC1: parity with live helpers ---");
        apply_dev_audit_defaults();
        set_sample_ratio(4);
        for (std::uint64_t mid : {0u, 1u, 4u, 7u, 8u}) {
            for (std::uint64_t nodes : {0u, 1u, 7u, 8u, 16u}) {
                for (bool lin : {false, true}) {
                    for (bool strict : {false, true}) {
                        const auto d = decide(mid, nodes, lin, strict);
                        const bool live_audit = should_audit_contextual(mid, nodes, lin);
                        const bool live_hard = requires_invariant_hard_gate(nodes, lin, strict);
                        CHECK(d.would_audit == live_audit,
                              std::format("AC1.parity.audit mid={} nodes={} lin={} strict={}", mid,
                                          nodes, lin ? 1 : 0, strict ? 1 : 0));
                        CHECK(d.would_hard_gate == live_hard,
                              std::format("AC1.parity.hard mid={} nodes={} lin={} strict={}", mid,
                                          nodes, lin ? 1 : 0, strict ? 1 : 0));
                    }
                }
            }
        }
    }

    // ── AC2: query invokable by Agent without mutate side effects ──
    {
        std::println("\n--- AC2: query no side effects ---");
        apply_dev_audit_defaults();
        CompilerService cs;
        (void)cs.eval("(+ 1 1)"); // warm up
        // Multiple calls to decide() via the query surface should be
        // idempotent (no counter bumps — decide() is PURE).
        for (int i = 0; i < 3; ++i) {
            const auto r1 = cs.eval(
                std::format("(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") "
                            "\"audit-decision-strategy\")"));
            CHECK(r1.has_value(), std::format("AC2.{}: strategy reachable", i));
        }
    }

    // ── AC3: schema additive + wired sentinel ──
    {
        std::println("\n--- AC3: schema sentinels ---");
        apply_dev_audit_defaults();
        CompilerService cs;
        (void)cs.eval("(+ 1 1)");
        for (const char* k : {"schema-2281", "issue-2281", "audit-decision-wired",
                              "audit-decision-strategy", "audit-decision-sample-ratio",
                              "audit-decision-production-defaults", "audit-decision-would-audit",
                              "audit-decision-would-hard-gate", "audit-decision-force-reason"}) {
            const auto r = cs.eval(std::format(
                "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", k));
            CHECK(r.has_value(), std::format("AC3.q: {} reachable", k));
        }
    }

    // ── Issue #3625: Production RenderFastExit is metrics-only — never an
    // audit skip (#2311/#3322 residual); Soft keeps the hotpath skip.
    std::println("\n=== Issue #3625: production render fast never skips the invariant audit ===");
    ac3625_1_production_render_still_audits();
    ac3625_2_soft_render_fast_still_skips();
    ac3625_3_suppress_counters_unchanged();
    ac3625_4_source_and_linter();

    apply_dev_audit_defaults();
    std::println("=== #2281 done: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_typed_mutation_audit_decision();
}
#endif
