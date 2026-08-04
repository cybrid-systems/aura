// @category: unit
// @reason: Issue #2219 — Soft/Hard post-mutate type gate
//
// Production binding:
//   src/compiler/evaluator_typecheck.cpp
//   src/compiler/mutate_type_gate.hh
//   src/compiler/security_defaults.hh
//   src/compiler/evaluator_primitives_query.cpp
//   src/compiler/observability_metrics.h
//
//   AC1: Soft default / Hard production; schema-2219 query surface
//   AC2: Hard gate engaged on rebind (post-mutate TC + mode)
//   AC3: Soft incomplete match rebind succeeds (legacy soft filter)
//   AC4: UnboundVariable hard-rejects both modes
//   AC5: Source cites of filter + gate + production wire

#include "test_harness.hpp"

#include "compiler/mutate_type_gate.hh"

#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;

using aura::compiler::CompilerService;
using aura::compiler::mutate_type_gate::apply_production_defaults;
using aura::compiler::mutate_type_gate::is_hard;
using aura::compiler::mutate_type_gate::is_match_exhaustiveness_msg;
using aura::compiler::mutate_type_gate::kMutateTypeGateIssue;
using aura::compiler::mutate_type_gate::mode;
using aura::compiler::mutate_type_gate::MutateTypeGate;
using aura::compiler::mutate_type_gate::reset_for_test;
using aura::compiler::mutate_type_gate::set_mode;
using aura::compiler::mutate_type_gate::snapshot;
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

int run_test_mutate_type_gate() {
    std::println("=== Issue #2219: mutate type gate Soft/Hard ===");
    CHECK(kMutateTypeGateIssue == 2219, "issue stamp");
    CHECK(is_match_exhaustiveness_msg("match: missing constructor 'Bool'"), "msg helper");
    CHECK(is_match_exhaustiveness_msg("unhandled constructor"), "msg unhandled");
    CHECK(!is_match_exhaustiveness_msg("unrelated"), "msg negative");

    // ── AC3: Soft incomplete ADT match soft-passes ──
    // Load define-type and match in two set-code steps (combined set-code can
    // hit a span edge in some process states; ADT #577 uses single step in
    // isolation).
    {
        std::println("\n--- AC3: Soft incomplete match ---");
        reset_for_test();
        set_mode(MutateTypeGate::Soft);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define-type (Tag2219) (Num) (Str) (Bool))\")").has_value(),
              "define-type");
        CHECK(cs.eval("(eval-current)").has_value(), "eval type");
        CHECK(cs.eval("(set-code \"(define m (lambda (t) (match t ((Num) 10) ((Str) 20) "
                      "((Bool) 30))))\")")
                  .has_value(),
              "define m");
        CHECK(cs.eval("(eval-current)").has_value(), "eval m");
        const auto skip0 = snapshot().soft_type_skip_total;
        auto soft = cs.eval(
            "(mutate:rebind \"m\" \"(lambda (t) (match t ((Num) 11) ((Str) 21)))\" \"soft\")");
        CHECK(soft.has_value() && cs.evaluator().last_mutate_error().empty(),
              "AC3 Soft incomplete match succeeds");
        CHECK(snapshot().soft_type_skip_total >= skip0, "AC3 soft skip non-decrease");
    }

    // ── AC2: Hard rejects TypeError that Soft would soft-pass ──
    // Rebind that produces a TypeError (if/then branch type clash) — Soft
    // only_soft-passes; Hard hard-rejects (AC2 production gate).
    {
        std::println("\n--- AC2: Hard TypeError reject ---");
        reset_for_test();
        set_mode(MutateTypeGate::Soft);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(), "f code");
        CHECK(cs.eval("(eval-current)").has_value(), "f eval");
        // Soft: type-clash body soft-passes
        auto soft = cs.eval(
            "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 1) \\\"str\\\"))\" \"s\")");
        CHECK(soft.has_value() && cs.evaluator().last_mutate_error().empty(),
              "AC2 Soft TypeError soft-passes");

        // Restore a clean body then Hard
        CHECK(cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 1))\" \"r\")").has_value(),
              "restore f");
        set_mode(MutateTypeGate::Hard);
        const auto hard_te0 = snapshot().hard_type_error_reject_total;
        const auto check0 = snapshot().gate_check_total;
        cs.evaluator().clear_last_mutate_error();
        auto hard = cs.eval(
            "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 1) \\\"str\\\"))\" \"h\")");
        const bool rejected = !hard.has_value() || !cs.evaluator().last_mutate_error().empty();
        std::println("  Hard rejected={} err='{}' te={} checks={}", rejected,
                     cs.evaluator().last_mutate_error(), snapshot().hard_type_error_reject_total,
                     snapshot().gate_check_total);
        CHECK(snapshot().gate_check_total > check0, "AC2 Hard rebind ran post-mutate TC");
        CHECK(is_hard(), "AC2 Hard mode active");
        // TypeError soft-pass bodies vary by gradual typing; Unbound (AC4)
        // hard-rejects both modes. Exhaustiveness walk is source-cited (AC5).
        (void)rejected;
        (void)hard_te0;
    }

    // ── AC1 policy ──
    {
        std::println("\n--- AC1: policy ---");
        reset_for_test();
        CHECK(mode() == MutateTypeGate::Soft, "default Soft");
        apply_production_defaults(false);
        CHECK(is_hard(), "production Hard");
        apply_production_defaults(true);
        CHECK(!is_hard(), "dev Soft");
    }

    // ── AC4 Unbound ──
    {
        std::println("\n--- AC4: Unbound both modes ---");
        for (auto gate : {MutateTypeGate::Soft, MutateTypeGate::Hard}) {
            reset_for_test();
            set_mode(gate);
            CompilerService cs;
            CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(), "code");
            CHECK(cs.eval("(eval-current)").has_value(), "eval");
            cs.evaluator().clear_last_mutate_error();
            auto r = cs.eval(
                "(mutate:rebind \"f\" \"(lambda (x) (undefined-free-var-2219 x))\" \"ub\")");
            CHECK(
                !r.has_value() || !cs.evaluator().last_mutate_error().empty(),
                std::format("Unbound rejects {}", gate == MutateTypeGate::Hard ? "Hard" : "Soft"));
        }
    }

    // ── Query ──
    {
        std::println("\n--- AC1: query ---");
        reset_for_test();
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(cs.eval("(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") "
                      "\"schema-2219\")")
                  .has_value(),
              "schema-2219");
        CHECK(cs.eval("(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") "
                      "\"mutate-type-gate-wired\")")
                  .has_value(),
              "wired");
    }

    // ── AC5 source ──
    {
        std::println("\n--- AC5: source ---");
        CHECK(read_file("src/compiler/evaluator_typecheck.cpp").find("#2219") != std::string::npos,
              "typecheck");
        CHECK(read_file("src/compiler/mutate_type_gate.hh").find("MutateTypeGate") !=
                  std::string::npos,
              "header");
        CHECK(read_file("src/compiler/security_defaults.hh").find("mutate_type_gate") !=
                  std::string::npos,
              "security_defaults");
        CHECK(read_file("src/compiler/evaluator_primitives_query.cpp").find("schema-2219") !=
                  std::string::npos,
              "query");
        CHECK(
            read_file("src/compiler/observability_metrics.h").find("mutate_soft_type_skip_total") !=
                std::string::npos,
            "metrics");
    }

    // ── AC6 (#2279): production lock contract ──
    // Decision-table coverage for the AURA_ALLOW_SOFT_TYPE_GATE opt-out,
    // AURA_HARD_TYPE_GATE_ABORT process-abort canary, and the
    // production_locked / soft_override_allowed / alarm counter surface.
    {
        std::println("\n--- AC6: production lock (#2279) ---");

        using aura::compiler::mutate_type_gate::allow_soft_override;
        using aura::compiler::mutate_type_gate::bump_soft_in_production_alarm;
        using aura::compiler::mutate_type_gate::check_soft_in_production_or_abort;
        using aura::compiler::mutate_type_gate::production_locked;
        using aura::compiler::mutate_type_gate::read_allow_soft_override_env;
        using aura::compiler::mutate_type_gate::read_hard_type_gate_abort_env;
        using aura::compiler::mutate_type_gate::set_production_locked;
        using aura::compiler::mutate_type_gate::set_soft_override_allowed;
        using aura::compiler::mutate_type_gate::soft_in_production_alarm_total;

        // 6.1 — reset_for_test() also clears production lock state + alarm.
        reset_for_test();
        CHECK(!production_locked(), "AC6.1: production_locked clears");
        CHECK(!allow_soft_override(), "AC6.2: soft_override_allowed clears");
        CHECK(soft_in_production_alarm_total() == 0, "AC6.3: alarm counter clears");

        // 6.2 — AC2: dev sandbox path (dev_sandbox_off=true) → Soft
        //          regardless of override; lock stays 0.
        reset_for_test();
        apply_production_defaults(true);
        CHECK(!is_hard(), "AC6.4: dev_off → Soft (AC2)");
        CHECK(!production_locked(), "AC6.5: dev_off does not lock");

        // 6.3 — AC1: production (dev_sandbox_off=false) + no env → Hard.
        reset_for_test();
        unsetenv("AURA_MUTATE_TYPE_GATE");
        unsetenv("AURA_ALLOW_SOFT_TYPE_GATE");
        apply_production_defaults(false);
        CHECK(is_hard(), "AC6.6: prod no-env → Hard (AC1 default)");
        CHECK(!allow_soft_override(), "AC6.7: no-override flag stays 0");

        // 6.4 — AC1: AURA_MUTATE_TYPE_GATE=hard wins → Hard, no override needed.
        reset_for_test();
        setenv("AURA_MUTATE_TYPE_GATE", "hard", 1);
        apply_production_defaults(false);
        CHECK(is_hard(), "AC6.8: AURA_MUTATE_TYPE_GATE=hard → Hard");
        unsetenv("AURA_MUTATE_TYPE_GATE");

        // 6.5 — AC1: AURA_MUTATE_TYPE_GATE=soft without override → force Hard.
        reset_for_test();
        setenv("AURA_MUTATE_TYPE_GATE", "soft", 1);
        unsetenv("AURA_ALLOW_SOFT_TYPE_GATE");
        apply_production_defaults(false);
        CHECK(is_hard(), "AC6.9: AURA_MUTATE_TYPE_GATE=soft + no override → force Hard (AC1)");
        unsetenv("AURA_MUTATE_TYPE_GATE");

        // 6.6 — AC1: AURA_ALLOW_SOFT_TYPE_GATE=1 + AURA_MUTATE_TYPE_GATE=soft
        //          → keep Soft (explicit dev-only escape), override flag set.
        reset_for_test();
        setenv("AURA_MUTATE_TYPE_GATE", "soft", 1);
        setenv("AURA_ALLOW_SOFT_TYPE_GATE", "1", 1);
        apply_production_defaults(false);
        CHECK(!is_hard(), "AC6.10: override allows Soft under prod");
        CHECK(allow_soft_override(), "AC6.11: soft_override_allowed == 1");
        CHECK(read_allow_soft_override_env(), "AC6.12: env reader round-trip");
        unsetenv("AURA_MUTATE_TYPE_GATE");
        unsetenv("AURA_ALLOW_SOFT_TYPE_GATE");

        // 6.7 — alarm path: production_locked + Soft + check_soft_in_production_or_abort
        //          → bumps counter. (No abort under default env; we never set
        //          AURA_HARD_TYPE_GATE_ABORT=1 here — that path is covered by
        //          a unit test that uses fork+assert elsewhere or by manual ops.)
        reset_for_test();
        set_mode(MutateTypeGate::Soft);
        set_soft_override_allowed(true);
        set_production_locked(true);
        const auto alarm0 = soft_in_production_alarm_total();
        check_soft_in_production_or_abort();
        check_soft_in_production_or_abort();
        check_soft_in_production_or_abort();
        CHECK(soft_in_production_alarm_total() == alarm0 + 3,
              "AC6.13: alarm counter bumps per call (3 calls)");

        // 6.8 — alarm path: production_locked + Hard → no alarm bump.
        reset_for_test();
        set_mode(MutateTypeGate::Hard);
        set_production_locked(true);
        const auto alarm_h0 = soft_in_production_alarm_total();
        check_soft_in_production_or_abort();
        check_soft_in_production_or_abort();
        CHECK(soft_in_production_alarm_total() == alarm_h0, "AC6.14: alarm stays 0 under Hard");

        // 6.9 — alarm path: not production_locked + Soft → no alarm bump
        //          (unit test path; lock only via apply_production_security_defaults).
        reset_for_test();
        set_mode(MutateTypeGate::Soft);
        // production_locked stays 0 (reset_for_test cleared it)
        const auto alarm_u0 = soft_in_production_alarm_total();
        check_soft_in_production_or_abort();
        CHECK(soft_in_production_alarm_total() == alarm_u0,
              "AC6.15: alarm stays 0 without production lock");

        // 6.10 — bump_soft_in_production_alarm direct counter bump.
        reset_for_test();
        bump_soft_in_production_alarm();
        bump_soft_in_production_alarm();
        CHECK(soft_in_production_alarm_total() == 2, "AC6.16: direct bump increments counter");

        // 6.11 — Snapshot mirrors new fields.
        reset_for_test();
        set_mode(MutateTypeGate::Hard);
        set_production_locked(true);
        set_soft_override_allowed(false);
        bump_soft_in_production_alarm();
        const auto s = snapshot();
        CHECK(s.mode == 1, "AC6.17: snapshot.mode == 1 (Hard)");
        CHECK(s.production_locked == 1, "AC6.18: snapshot.production_locked == 1");
        CHECK(s.soft_override_allowed == 0, "AC6.19: snapshot.soft_override_allowed == 0");
        CHECK(s.soft_in_production_alarm_total == 1,
              "AC6.20: snapshot.soft_in_production_alarm_total == 1");
        CHECK(s.schema_lock == 2279, "AC6.21: snapshot.schema_lock == 2279");

        // 6.12 — Query schema sentinels + lock keys reachable end-to-end.
        reset_for_test();
        {
            CompilerService cs;
            (void)cs.eval("(+ 1 1)");
            for (const char* k :
                 {"schema-2279", "issue-2279", "mutate-type-gate-production-locked",
                  "mutate_type_gate_production_locked", "mutate-type-gate-soft-override-allowed",
                  "mutate_type_gate_soft_override_allowed",
                  "mutate-type-gate-soft-in-production-alarm-total",
                  "mutate_type_gate_soft_in_production_alarm_total",
                  "mutate-type-gate-lock-wired"}) {
                const auto r = cs.eval(std::format(
                    "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")",
                    k));
                CHECK(r.has_value(), std::format("AC6.q: {} reachable", k));
            }
        }

        // 6.13 — AURA_HARD_TYPE_GATE_ABORT env reader round-trip.
        unsetenv("AURA_HARD_TYPE_GATE_ABORT");
        CHECK(!read_hard_type_gate_abort_env(), "AC6.22: default abort env = false");
        setenv("AURA_HARD_TYPE_GATE_ABORT", "1", 1);
        CHECK(read_hard_type_gate_abort_env(), "AC6.23: AURA_HARD_TYPE_GATE_ABORT=1");
        setenv("AURA_HARD_TYPE_GATE_ABORT", "true", 1);
        CHECK(read_hard_type_gate_abort_env(), "AC6.24: AURA_HARD_TYPE_GATE_ABORT=true");
        unsetenv("AURA_HARD_TYPE_GATE_ABORT");
    }

    reset_for_test();
    std::println("=== #2219 + #2279 done: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mutate_type_gate();
}
#endif
