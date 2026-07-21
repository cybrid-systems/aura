// @category: unit
// @reason: Issue #1876 — runtime SandboxMode + CapabilityGrant enforcement
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
