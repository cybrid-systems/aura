// @category: unit
// @reason: Issue #1565 — Capability Effects enforcement:
// check_and_record_effect, provenance binding, Strict sandbox deny,
// mutate/FFI force path, query:capability-effect-stats, metrics.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"

#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::security::kCapMutate;
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectWrite;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::snapshot_capability_effect_stats;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

std::int64_t href_m(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void reset_all() {
    reset_capability_effects_for_test();
    set_mode(SandboxMode::Off);
}

} // namespace

int main() {
    reset_all();

    // ── AC: query:capability-effect-stats shape ──
    {
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics "query:capability-effect-stats"))");
        CHECK(h && is_hash(*h), "capability-effect-stats is hash");
        CHECK(href_m(cs, "schema") == 1565, "schema 1565");
        CHECK(href_m(cs, "active") == 1, "active");
        CHECK(href_m(cs, "phase") == 2, "phase 2");
    }

    // ── AC1: Off mode always allows, still audits ──
    {
        reset_all();
        g_capability_registry().sandbox_mode = EffectSandboxMode::Off;
        EffectProvenance prov{};
        const auto checks0 = snapshot_capability_effect_stats().checks;
        const bool ok =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 0, "test-off");
        CHECK(ok, "Off mode allows without grant");
        CHECK(snapshot_capability_effect_stats().checks == checks0 + 1, "check counted");
        CHECK(snapshot_capability_effect_stats().enforced >= 1, "enforced counted");
        CHECK(snapshot_capability_effect_stats().audits >= 1, "audit recorded");
    }

    // ── AC1: Strict mode denies without grant ──
    {
        reset_all();
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
        EffectProvenance prov{};
        const auto denied0 = snapshot_capability_effect_stats().denied;
        const bool ok =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 0, "test-strict-deny");
        CHECK(!ok, "Strict denies mutate without grant");
        CHECK(snapshot_capability_effect_stats().denied == denied0 + 1, "denied counted");
    }

    // ── AC1: Strict mode allows after grant ──
    {
        reset_all();
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
        g_capability_registry().grant(0, "mutate", Effect::Mutate);
        EffectProvenance prov{};
        const bool ok =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 0, "test-strict-ok");
        CHECK(ok, "Strict allows after Mutate grant");
    }

    // ── AC1: wildcard bypass ──
    {
        reset_all();
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
        EffectProvenance prov{};
        const bool ok = check_and_record_effect(Effect::Write, Effect::Write, prov, 0, "test-wild",
                                                /*wildcard_ok=*/true);
        CHECK(ok, "wildcard bypasses grant requirement");
    }

    // ── AC1: provenance binding mismatch ──
    {
        reset_all();
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
        EffectProvenance grant_prov{};
        grant_prov.mutation_id = 42;
        g_capability_registry().grant(1, "mutate", Effect::Mutate, grant_prov);
        EffectProvenance call_prov{};
        call_prov.mutation_id = 99; // mismatch
        const auto mm0 = snapshot_capability_effect_stats().provenance_mismatch;
        const bool ok =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, call_prov, 1, "test-prov");
        CHECK(!ok, "provenance mutation_id mismatch denies");
        CHECK(snapshot_capability_effect_stats().provenance_mismatch == mm0 + 1,
              "provenance mismatch counted");
    }

    // ── AC: full bit coverage (Write grant does not cover Write|Ffi as required Write only ok) ──
    {
        reset_all();
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
        g_capability_registry().grant(0, "io-write", Effect::Write);
        EffectProvenance prov{};
        // required = Write|Ffi, held = Write only → deny
        const bool ok = check_and_record_effect(
            Effect::Write | Effect::Ffi, Effect::Write | Effect::Ffi, prov, 0, "test-partial");
        CHECK(!ok, "partial grant does not cover multi-bit required");
        const bool ok2 =
            check_and_record_effect(Effect::Write, Effect::Write, prov, 0, "test-write-only");
        CHECK(ok2, "Write-only required allowed with Write grant");
    }

    // ── AC: Evaluator bridge + security EDSL ──
    {
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();

        // Grant while Off (grant-effect! while sandboxed requires wildcard).
        auto g = cs.eval(std::format("(security:grant-effect! \"mutate\" {})", kEffectMutate));
        CHECK(g && is_bool(*g) && as_bool(*g), "grant-effect! succeeds while Off");

        // set Strict via primitive
        auto m = cs.eval("(security:set-effect-sandbox-mode! 2)");
        CHECK(m && is_int(*m), "set-effect-sandbox-mode! returns prev mode");
        CHECK(ev.effect_sandbox_mode() == 2, "effect mode Strict");
        CHECK(ev.sandbox_mode(), "sandbox_mode_ on under Strict");

        // With Mutate grant, check-effect allows
        auto c2 = cs.eval(std::format("(security:check-effect {})", kEffectMutate));
        CHECK(c2 && is_bool(*c2) && as_bool(*c2), "check-effect allows after grant+Strict");

        // Deny path: Write without grant under Strict
        auto c1 = cs.eval(std::format("(security:check-effect {})", kEffectWrite));
        CHECK(c1 && is_bool(*c1) && !as_bool(*c1), "check-effect denies Write without grant");

        // stats mirror
        CHECK(href_m(cs, "denied") >= 1, "stats denied >= 1");
        CHECK(href_m(cs, "enforced") >= 1, "stats enforced >= 1");
        CHECK(href_m(cs, "grants") >= 1, "stats grants >= 1");
        CHECK(href_m(cs, "sandbox-mode") == 2, "stats sandbox-mode Strict");
    }

    // ── AC: mutate path force under Strict (bypass denial) ──
    {
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Enter Strict without grants — mutate:* should deny.
        ev.set_effect_sandbox_mode(2);
        // Need a workspace for mutate primitives to even be meaningful.
        (void)cs.eval("(set-code \"(define x 1)\")");

        const auto denials0 = ev.capability_denial_count();
        // Try a mutate primitive that goes through add_mutate wrapper.
        // Without grant + Strict → effect deny (and legacy sandbox cap deny).
        auto r = cs.eval("(mutate:remove-node 0)");
        // May return error or false; key is denial path was taken.
        CHECK(ev.capability_denial_count() > denials0 || (r && is_error(*r)) ||
                  snapshot_capability_effect_stats().denied >= 1,
              "mutate under Strict without grant is denied");

        // Grant mutate + wildcard to pass legacy + effect, then verify enforced path.
        ev.grant_capability(kCapWildcard);
        reset_capability_effects_for_test();
        // Re-apply Strict after reset (reset clears sandbox mode on registry).
        ev.set_effect_sandbox_mode(2);
        // Wildcard should allow effect check even under Strict.
        EffectProvenance prov{};
        const bool ok = ev.check_and_record_effect(kEffectMutate, kEffectMutate, "mutate-wild", 0,
                                                   ev.capability_tenant_id(), 0);
        CHECK(ok, "wildcard allows mutate effect under Strict");
    }

    // ── AC: Restricted only enforces when sandbox active ──
    {
        reset_all();
        g_capability_registry().sandbox_mode = EffectSandboxMode::Restricted;
        EffectProvenance prov{};
        // sandbox_active=false → allow without grant
        const bool ok1 = check_and_record_effect(Effect::Write, Effect::Write, prov, 0, "r-off",
                                                 false, /*sandbox_active=*/false);
        CHECK(ok1, "Restricted + inactive allows without grant");
        // sandbox_active=true → deny without grant
        const bool ok2 = check_and_record_effect(Effect::Write, Effect::Write, prov, 0, "r-on",
                                                 false, /*sandbox_active=*/true);
        CHECK(!ok2, "Restricted + active denies without grant");
        g_capability_registry().grant(0, "io-write", Effect::Write);
        const bool ok3 = check_and_record_effect(Effect::Write, Effect::Write, prov, 0, "r-grant",
                                                 false, /*sandbox_active=*/true);
        CHECK(ok3, "Restricted + active allows with Write grant");
    }

    // ── AC: Evaluator C++ API grant_effect_capability ──
    {
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2);
        const auto denied0 = snapshot_capability_effect_stats().denied;
        CHECK(!ev.check_and_record_effect(kEffectWrite, kEffectWrite, "pre-grant", 0, 0, 0),
              "pre-grant write denied");
        ev.grant_effect_capability(0, "io-write", kEffectWrite, 0);
        CHECK(ev.check_and_record_effect(kEffectWrite, kEffectWrite, "post-grant", 0, 0, 0),
              "post-grant write allowed");
        CHECK(snapshot_capability_effect_stats().denied > denied0, "denial recorded before grant");
    }

    reset_all();
    std::println("\n=== test_capability_effects_enforcement: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}
