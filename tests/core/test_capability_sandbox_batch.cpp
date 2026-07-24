// tests/core/test_capability_sandbox_batch.cpp
// R18 dup-merge — Issue #1565/#1876/#1878 (#1978 renamed): Capability Effects
// enforcement + SandboxMode + CapabilityGrant + atomic-batch tenant isolation
// combined into one batch file.
// Originals: test_capability_effects_enforcement.cpp + test_sandbox_capability_enforce.cpp +
//            test_atomic_batch_tenant.cpp. R18 ship per Anqi 13:14 #81620.

// === AC1-AC4 from test_capability_effects_enforcement.cpp (#1565) ===

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


// === AC5-AC8 from test_sandbox_capability_enforce.cpp (#1876) ===

// @reason: Issue #1876 — runtime SandboxMode + CapabilityGrant enforcement
// Issue #1876 (#1978 renamed): issue# moved from filename to header.
// on mutate/FFI paths with tenant + provenance metrics.
//
//   AC1: source cites #1876; sandbox_violations + denials_by_effect metrics
//   AC2: Strict + missing Mutate → deny + sandbox_violations++
//   AC3: allowed under sandbox records provenance
//   AC4: capability-effect-stats sandbox fields; Off mode no regression

#include "compiler/observability_metrics.h"
#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectFfi;
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
using aura::core::sandbox::g_sandbox_state;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

void reset_all() {
    reset_capability_effects_for_test();
    set_mode(SandboxMode::Off);
    g_sandbox_state().effect_checks = 0;
    g_sandbox_state().effect_denials = 0;
}

std::int64_t href(CompilerService& cs, std::string_view key) {
    // #1876 folds sandbox-status into query:capability-effect-stats (freeze).
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1876 source surface ---");
        auto sec = read_first(
            {"src/compiler/evaluator_security.cpp", "../src/compiler/evaluator_security.cpp"});
        auto prim = read_first({"src/compiler/evaluator_primitives_security.cpp",
                                "../src/compiler/evaluator_primitives_security.cpp"});
        auto hdr = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        CHECK(!sec.empty() && sec.find("#1876") != std::string::npos, "security.cpp cites #1876");
        CHECK(sec.find("sandbox_violations_total") != std::string::npos, "violations bump");
        CHECK(sec.find("capability_denials_by_effect") != std::string::npos, "denials_by_effect");
        CHECK(sec.find("record_mutation") != std::string::npos, "provenance record on allow");
        CHECK(!prim.empty() && prim.find("sandbox-status-schema") != std::string::npos,
              "sandbox-status fields in capability-effect-stats");
        CHECK(prim.find("#1876") != std::string::npos, "prims cite #1876");
        CHECK(!hdr.empty() && hdr.find("sandbox_violations_total") != std::string::npos,
              "metrics field");
        CHECK(hdr.find("capability_denials_by_effect") != std::string::npos,
              "denials_by_effect metric");
    }

    // ── AC2: Strict missing Mutate → deny + metrics ──
    {
        std::println("\n--- AC2: Strict + missing Mutate → deny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->sandbox_violations_total.store(0);
            m->capability_denials_by_effect.store(0);
            m->capability_denial_mutate_total.store(0);
            m->sandbox_provenance_records_total.store(0);
        }
        ev.set_effect_sandbox_mode(2); // Strict
        CHECK(ev.effect_sandbox_mode() == 2, "Strict mode");
        CHECK(ev.sandbox_mode(), "sandbox_mode_ on");

        const auto den0 = ev.capability_denial_count();
        const auto v0 = static_cast<CompilerMetrics*>(ev.compiler_metrics())
                            ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                  ->sandbox_violations_total.load()
                            : 0;
        CHECK(!ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac2-mutate", 0, 0, 0),
              "Strict without grant denies Mutate");
        CHECK(ev.capability_denial_count() > den0, "capability_denial_count bumped");
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m && m->sandbox_violations_total.load() > v0, "sandbox_violations_total bumped");
        CHECK(m->capability_denials_by_effect.load() & kEffectMutate,
              "denials_by_effect has Mutate bit");
        CHECK(m->capability_denial_mutate_total.load() >= 1, "denial_mutate count");
        CHECK(g_sandbox_state().effect_denials >= 1, "g_sandbox effect_denials");
        std::println("  violations={} by_effect={:#x} mutate_denials={}",
                     m->sandbox_violations_total.load(), m->capability_denials_by_effect.load(),
                     m->capability_denial_mutate_total.load());
    }

    // ── AC3: allow under sandbox records provenance ──
    {
        std::println("\n--- AC3: allow under sandbox → provenance ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m)
            m->sandbox_provenance_records_total.store(0);
        ev.grant_capability(kCapWildcard);
        ev.set_effect_sandbox_mode(2);
        // Wildcard allows; sandbox still active → provenance record.
        CHECK(ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac3-allow", 42,
                                         ev.capability_tenant_id(), 0),
              "wildcard allows Mutate under Strict");
        CHECK(m && m->sandbox_provenance_records_total.load() >= 1,
              "provenance record under sandbox allow");
        // Ffi deny without grant (clear registry grants but keep wildcard for legacy)
        reset_capability_effects_for_test();
        ev.set_effect_sandbox_mode(2);
        // Without wildcard string cap, Ffi denied:
        // Re-grant only mutate effect, not ffi, and no wildcard.
        // (set_effect_sandbox_mode sets sandbox_mode_ true)
        // Drop wildcard by starting fresh service for ffi deny path.
    }
    {
        reset_all();
        CompilerService cs2;
        auto& ev2 = cs2.evaluator();
        auto* m2 = static_cast<CompilerMetrics*>(ev2.compiler_metrics());
        if (m2) {
            m2->capability_denial_ffi_total.store(0);
            m2->capability_denials_by_effect.store(0);
        }
        ev2.set_effect_sandbox_mode(2);
        CHECK(!ev2.check_and_record_effect(kEffectFfi, kEffectFfi, "ac3-ffi", 0, 0, 0),
              "Strict denies Ffi without grant");
        CHECK(m2 && m2->capability_denial_ffi_total.load() >= 1, "ffi denial counted");
        CHECK(m2->capability_denials_by_effect.load() & kEffectFfi, "by_effect has Ffi bit");
    }

    // ── AC4: capability-effect-stats sandbox fields + Off mode ──
    {
        std::println("\n--- AC4: capability-effect-stats sandbox fields + Off mode ---");
        reset_all();
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics "query:capability-effect-stats"))");
        CHECK(h && is_hash(*h), "capability-effect-stats is hash");
        CHECK(href(cs, "schema") == 1565, "schema 1565 preserved");
        CHECK(href(cs, "sandbox-status-schema") == 1876, "sandbox-status-schema 1876");
        CHECK(href(cs, "sandbox-mode") == 0, "mode Off initially");

        // Off mode: check allows without grant, no violation.
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m)
            m->sandbox_violations_total.store(0);
        CHECK(ev.check_and_record_effect(kEffectWrite, kEffectWrite, "ac4-off", 0, 0, 0),
              "Off allows without grant");
        // Restricted inactive allows:
        g_capability_registry().sandbox_mode = EffectSandboxMode::Restricted;
        EffectProvenance prov{};
        CHECK(check_and_record_effect(Effect::Write, Effect::Write, prov, 0, "off-r", false, false),
              "Restricted+inactive allows");
        // After Strict path, status shows counters
        ev.set_effect_sandbox_mode(2);
        (void)ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac4-strict", 0, 0, 0);
        auto h2 = cs.eval(R"((engine:metrics "query:capability-effect-stats"))");
        CHECK(h2 && is_hash(*h2), "stats still hash");
        CHECK(href(cs, "sandbox-strict") == 1, "sandbox-strict flag set");
        CHECK(href(cs, "sandbox-mode") == 2, "sandbox-mode Strict");
        CHECK(href(cs, "sandbox-violations") >= 0, "violations field present");
        std::println("  mode={} strict={} violations={} denials={}", href(cs, "sandbox-mode"),
                     href(cs, "sandbox-strict"), href(cs, "sandbox-violations"),
                     href(cs, "effect-denials"));
    }

    reset_all();
    std::println("\n=== test_sandbox_capability_enforce_1876: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}


// === AC9-AC13 from test_atomic_batch_tenant.cpp (#1878) ===

// @reason: Issue #1878 — atomic-batch strong atomicity + multi-tenant
// Issue #1878/#1900 (#1978 renamed): issue# moved from filename to header.
// isolation under Strict sandbox (sibling to #1900 dispatch coverage).
//
//   AC1: source cites #1878; strong atomicity docs; weak metric surface
//   AC2: atomic-batch:stats / stats-hash expose atomicity + tenant fields
//   AC3: successful batch under strong mode bumps strong commits; weak=0
//   AC4: Strict + :tenant-target foreign → deny + tenant-isolation-denials
//   AC5: unsupported op still clear error (no silent full success)
//   AC6: single-tenant batch under Strict still succeeds (no regression)

#include "compiler/observability_metrics.h"
#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"
#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectMutate;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    set_mode(SandboxMode::Off);
}

std::int64_t stats_href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (stats:get \"atomic-batch:stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t qhref(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:atomic-batch-stats-hash\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

bool setup_ws(CompilerService& cs) {
    return cs.eval("(set-code \"(define f (lambda (x) (* x 2))) (define g 1)\")").has_value();
}

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1878 source surface ---");
        auto mut = read_first({"src/compiler/evaluator_primitives_mutate.cpp",
                               "../src/compiler/evaluator_primitives_mutate.cpp"});
        auto obs = read_first({"src/compiler/evaluator_primitives_obs_eval.cpp",
                               "../src/compiler/evaluator_primitives_obs_eval.cpp"});
        auto q = read_first({"src/compiler/evaluator_primitives_mutation.cpp",
                             "../src/compiler/evaluator_primitives_mutation.cpp"});
        auto hdr = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        CHECK(!mut.empty() && mut.find("#1878") != std::string::npos, "mutate cites #1878");
        CHECK(mut.find("STRONG") != std::string::npos || mut.find("strong") != std::string::npos,
              "strong atomicity documented");
        CHECK(mut.find(":tenant-target") != std::string::npos, ":tenant-target keyword");
        CHECK(mut.find("tenant-isolation-denied") != std::string::npos, "tenant deny path");
        CHECK(mut.find("bump_atomic_batch_strong_atomicity_commit") != std::string::npos,
              "strong commit bump");
        CHECK(!obs.empty() && obs.find("#1878") != std::string::npos, "stats cites #1878");
        CHECK(obs.find("weak-atomicity-used") != std::string::npos, "weak metric in stats");
        CHECK(obs.find("atomicity-mode") != std::string::npos, "atomicity-mode in stats");
        CHECK(!q.empty() && q.find("schema-1878") != std::string::npos, "stats-hash schema-1878");
        CHECK(!hdr.empty() && hdr.find("atomic_batch_weak_atomicity_used") != std::string::npos,
              "weak metric field");
        CHECK(hdr.find("atomic_batch_tenant_isolation_denials") != std::string::npos,
              "tenant denials field");
    }

    // ── AC2: stats surfaces ──
    {
        std::println("\n--- AC2: atomic-batch stats #1878 fields ---");
        reset_all();
        CompilerService cs;
        auto h = cs.eval(R"((stats:get "atomic-batch:stats"))");
        CHECK(h && is_hash(*h), "atomic-batch:stats hash");
        CHECK(stats_href(cs, "atomicity-mode") == 1, "atomicity-mode strong=1");
        CHECK(stats_href(cs, "weak-atomicity-used") == 0, "weak-atomicity-used starts 0");
        CHECK(stats_href(cs, "schema-1878") == 1878, "schema-1878");
        CHECK(stats_href(cs, "tenant-isolation-denials") >= 0, "tenant-isolation-denials field");
        auto qh = cs.eval(R"((engine:metrics "query:atomic-batch-stats-hash"))");
        CHECK(qh && is_hash(*qh), "query:atomic-batch-stats-hash hash");
        CHECK(qhref(cs, "atomicity-mode") == 1, "hash atomicity-mode");
        CHECK(qhref(cs, "schema-1878") == 1878, "hash schema-1878");
        CHECK(qhref(cs, "weak-atomicity-used") == 0, "hash weak=0");
    }

    // ── AC3: strong commit path ──
    {
        std::println("\n--- AC3: strong atomicity commit ---");
        reset_all();
        CompilerService cs;
        CHECK(setup_ws(cs), "workspace");
        auto& ev = cs.evaluator();
        const auto strong0 = ev.atomic_batch_strong_atomicity_commits_total();
        const auto weak0 = ev.atomic_batch_weak_atomicity_used_total();
        const auto inter0 = ev.atomic_batch_interleaved_prevented_total();
        auto r = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:rebind\" \"f\" \"(lambda (x) (* x 3))\")) \"ac3\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "batch succeeds");
        CHECK(ev.atomic_batch_strong_atomicity_commits_total() > strong0, "strong commits bumped");
        CHECK(ev.atomic_batch_weak_atomicity_used_total() == weak0, "weak still 0");
        CHECK(ev.atomic_batch_interleaved_prevented_total() > inter0, "interleaved-prevented");
        CHECK(stats_href(cs, "strong-atomicity-commits") >= 1, "stats strong commits");
        CHECK(stats_href(cs, "weak-atomicity-used") == 0, "stats weak 0");
    }

    // ── AC4: Strict + foreign :tenant-target deny ──
    {
        std::println("\n--- AC4: Strict cross-tenant batch deny ---");
        reset_all();
        CompilerService cs;
        CHECK(setup_ws(cs), "workspace");
        auto& ev = cs.evaluator();
        // Principal tenant 7, Strict sandbox, no cross-tenant grant.
        ev.set_tenant_principal(7, "bob");
        ev.set_effect_sandbox_mode(2);
        // Grant wildcard so capability effect model allows Mutate, but
        // isolation still blocks foreign tenant-target.
        ev.grant_capability(kCapWildcard);
        const auto den0 = ev.atomic_batch_tenant_isolation_denials_total();
        auto r = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:rebind\" \"f\" \"(lambda (x) (* x 9))\")) "
                         "\"ac4\" :tenant-target 8)");
        // Should fail (error pair or non-#t).
        const bool denied = !r || is_error(*r) || (is_bool(*r) && !as_bool(*r)) || !is_bool(*r);
        CHECK(denied, "foreign :tenant-target denied under Strict");
        CHECK(ev.atomic_batch_tenant_isolation_denials_total() > den0,
              "tenant-isolation-denials bumped");
        CHECK(stats_href(cs, "tenant-isolation-denials") >= 1, "stats denials");
        std::println("  denials={}", ev.atomic_batch_tenant_isolation_denials_total());

        // Same tenant target allowed under Strict.
        auto ok = cs.eval("(mutate:atomic-batch "
                          "(list (list \"mutate:rebind\" \"f\" \"(lambda (x) (* x 5))\")) "
                          "\"ac4-self\" :tenant-target 7)");
        CHECK(ok && is_bool(*ok) && as_bool(*ok), "same-tenant :tenant-target allowed");
    }

    // ── AC5: unsupported op clear error ──
    {
        std::println("\n--- AC5: unsupported op clear error ---");
        reset_all();
        CompilerService cs;
        CHECK(setup_ws(cs), "workspace");
        const auto u0 = stats_href(cs, "unsupported-op-total");
        auto r = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:not-a-real-op\" \"x\" \"1\")) \"ac5\")");
        const bool failed = !r || !is_bool(*r) || !as_bool(*r);
        CHECK(failed, "unsupported op does not succeed as #t");
        CHECK(stats_href(cs, "unsupported-op-total") > u0 || failed,
              "unsupported metric or hard error");
    }

    // ── AC6: single-tenant Strict no regression ──
    {
        std::println("\n--- AC6: single-tenant Strict batch ok ---");
        reset_all();
        CompilerService cs;
        CHECK(setup_ws(cs), "workspace");
        auto& ev = cs.evaluator();
        ev.grant_capability(kCapWildcard);
        ev.set_tenant_principal(1, "solo");
        ev.set_effect_sandbox_mode(2);
        auto r = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:rebind\" \"g\" \"42\")) \"ac6\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "Strict single-tenant batch succeeds");
        CHECK(ev.atomic_batch_weak_atomicity_used_total() == 0, "still strong (weak=0)");
    }

    reset_all();
    std::println("\n=== test_atomic_batch_tenant_1878: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
