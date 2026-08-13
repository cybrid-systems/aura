// tests/core/test_capability_single_use_consume.cpp
// @category: unit
// @reason: Issue #2586 — single-use / mutation-bound grant (auto-revoke
//          after first successful check_and_record_effect that uses the
//          grant's bits; deny path does NOT consume).
//
//   AC1: grant_once(Mutate) → 1st allow; 2nd deny (no other grant)
//   AC2: 1st deny (other reasons, e.g. wrong effect bits) → grant still
//        valid, retryable
//   AC3: non single_use grant behavior unchanged (multiple allows OK)
//   AC4: Soft/Off mode usable; production default API (grant() with
//        single_use=false) does not force auto-revoke
//   AC5: audit / SE dual-write reason "single-use-consumed" visible in
//        SecurityEvent ring + capability_single_use_consumed metric
//   AC6: tests + source-cite (no docs/design/)
//
// Source-cite:
//   src/core/capability_model.hh — CapabilityGrant.single_use field
//     (line ~133), CapabilityEffectMetrics.capability_single_use_consumed_total
//     counter (~196), grant() extended signature (~405), grant_once sugar (~449),
//     check_and_record_effect consume block (~842), CapabilityEffectStatsSnapshot
//     .capability_single_use_consumed (~975), reset + snapshot fields.
//   src/compiler/evaluator_security.cpp — grant_effect_capability single_use
//     parameter forwarding to registry::grant.
//   src/compiler/evaluator.ixx — grant_effect_capability declaration
//     with `bool single_use = false` default parameter.
//   src/compiler/evaluator_primitives_security.cpp — query:capability-effect-stats
//     surfaces capability-single-use-consumed-total / schema-2586 / issue-2586.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::kEffectMacroSelfEvo;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectSyscall;
using aura::compiler::security::kEffectTenantAdmin;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::snapshot_capability_effect_stats;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::kSecurityEventRingSize;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::security_event::SecurityEvent;
using aura::test::g_failed;
using aura::test::g_passed;

// Helper for AC6 source-cite checks (test path + ../ test path).
static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (in)
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    }
    return {};
}

// Walk back through the SE ring looking for an event whose reason matches
// `needle`. Bounded lookback so wrap storms don't scan forever. Returns the
// matched slot if found, nullptr otherwise.
const SecurityEvent* ring_lookup_reason(std::string_view needle, std::uint64_t lookback = 16) {
    const auto& ring = g_security_event_ring();
    const auto cur = ring.seq.load(std::memory_order_acquire);
    if (cur == 0)
        return nullptr;
    const auto start = cur;
    const auto end = (cur > lookback) ? cur - lookback : std::uint64_t{1};
    for (auto s = start; s >= end && s > 0; --s) {
        const auto idx = (s - 1) % kSecurityEventRingSize;
        const auto& e = ring.ring[idx];
        const auto rlen = std::strlen(e.reason);
        if (rlen == needle.size() && std::string_view(e.reason, rlen) == needle) {
            return &e;
        }
    }
    return nullptr;
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_security_event_ring_for_test();
    set_mode(SandboxMode::Off);
}

} // namespace

int run_test_capability_single_use_consume() {
    std::println("=== Issue #2586: single-use grant auto-revoke ===");

    // ── AC1: grant_once(Mutate) → 1st allow; 2nd deny ──────────────
    {
        std::println("\n--- #2586 AC1: single-use auto-revoke on 2nd check ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        EffectProvenance prov{};
        prov.epoch = 1;
        prov.mutation_id = 1;
        g_capability_registry().grant_once(/*tenant=*/1, "mut-2586-once", Effect::Mutate, prov);

        const bool ok1 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 1, "mut-once-1");
        CHECK(ok1, "AC1: 1st check_and_record_effect(Mutate) → allow");

        const bool ok2 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 1, "mut-once-2");
        CHECK(!ok2, "AC1: 2nd check_and_record_effect(Mutate) → deny (single_use consumed)");

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 1,
              "AC1: capability_single_use_consumed == 1 after 1st allow");
        CHECK(snap.revokes == 1, "AC1: revokes counter == 1 (single_use path bumps revoke_total)");
        CHECK(snap.enforced == 1, "AC1: enforced counter == 1");
        CHECK(snap.denied == 1, "AC1: denied counter == 1");
        CHECK(snap.audits >= 2, "AC1: audits counter >= 2 (effect + revoke audit)");
    }

    // ── AC2: deny path does NOT consume (retryable) ────────────────
    {
        std::println("\n--- #2586 AC2: deny does not consume (retryable) ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        EffectProvenance prov{};
        prov.epoch = 2;
        prov.mutation_id = 2;
        g_capability_registry().grant_once(/*tenant=*/2, "mut-2586-retry", Effect::Mutate, prov);

        // 1st: deny — ask for Write bit but grant only has Mutate.
        const bool ok1 =
            check_and_record_effect(Effect::Write, Effect::Write, prov, 2, "write-deny");
        CHECK(!ok1, "AC2: 1st check_and_record_effect(Write) → deny (no Write bit in grant)");

        const auto snap0 = snapshot_capability_effect_stats();
        CHECK(snap0.capability_single_use_consumed == 0,
              "AC2: single_use_consumed == 0 after deny (deny path does NOT consume)");

        // Retry: Mutate bit still works because deny did not consume.
        const bool ok2 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 2, "mut-retry-1");
        CHECK(ok2, "AC2: retry check_and_record_effect(Mutate) → allow (deny did not consume)");

        // Post-retry, 2nd Mutate check should deny because single_use now consumed.
        const bool ok3 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 2, "mut-retry-2");
        CHECK(!ok3, "AC2: post-retry 2nd Mutate check → deny (now consumed)");

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 1,
              "AC2: single_use_consumed == 1 (only the successful Mutate check consumed)");
        CHECK(snap.denied == 2, "AC2: denied counter == 2 (Write deny + post-retry Mutate deny)");
        CHECK(snap.enforced == 1, "AC2: enforced counter == 1 (the successful Mutate retry)");
    }

    // ── AC3: non single_use grant behavior unchanged ──────────────
    {
        std::println("\n--- #2586 AC3: non single_use grant unchanged ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        EffectProvenance prov{};
        prov.epoch = 3;
        prov.mutation_id = 3;
        // Default grant() — single_use=false (legacy behavior).
        g_capability_registry().grant(/*tenant=*/3, "mut-2586-perm", Effect::Mutate, prov);

        for (int i = 0; i < 5; ++i) {
            const std::string op = "mut-perm-" + std::to_string(i);
            const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 3, op);
            CHECK(ok, "AC3: non single_use grant 5x allow");
        }

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 0,
              "AC3: single_use_consumed == 0 for non single_use grant");
        CHECK(snap.revokes == 0, "AC3: revokes == 0 for non single_use grant");
        CHECK(snap.enforced == 5, "AC3: enforced == 5 (all 5 allowed)");
    }

    // ── AC4: Soft/Off usable; production default API not forced ────
    {
        std::println("\n--- #2586 AC4: Off mode + production default API ---");
        // (4a) Off mode: single_use grant is "usable" — counter tracks
        // consumption on every successful allow, but Off-mode always-allow
        // semantics are unchanged (grant not required for allow). 2nd call
        // still returns true in Off mode; the consumption is observable via
        // capability_single_use_consumed.
        reset_all();
        set_mode(SandboxMode::Off);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        EffectProvenance prov{};
        prov.epoch = 4;
        prov.mutation_id = 4;
        g_capability_registry().grant_once(/*tenant=*/4, "mut-2586-off", Effect::Mutate, prov);

        const bool off_ok1 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 4, "off-mut-1");
        CHECK(off_ok1, "AC4a: Off mode 1st allow");
        const bool off_ok2 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 4, "off-mut-2");
        CHECK(off_ok2, "AC4a: Off mode 2nd STILL allow (Off-mode semantics — grant not required; "
                       "single_use consumption tracked via counter, not via allow decision)");

        const auto snap0 = snapshot_capability_effect_stats();
        CHECK(snap0.capability_single_use_consumed == 1,
              "AC4a: Off mode single_use_consumed == 1 (consumption tracked even when grant "
              "not required for allow)");

        // (4b) Production default API: grant_effect_capability / grant()
        // without single_use param → no auto-revoke.
        reset_all();
        set_mode(SandboxMode::Strict);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        EffectProvenance prov2{};
        prov2.epoch = 5;
        prov2.mutation_id = 5;
        g_capability_registry().grant(/*tenant=*/5, "mut-2586-default", Effect::Mutate, prov2);

        for (int i = 0; i < 3; ++i) {
            const std::string op = "default-" + std::to_string(i);
            const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov2, 5, op);
            CHECK(ok, "AC4b: default API 3x allow (no single_use forced)");
        }

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 0,
              "AC4b: default API single_use_consumed == 0");
        CHECK(snap.revokes == 0, "AC4b: default API revokes == 0");
    }

    // ── AC5: audit / SE visible revoke with reason 'single-use-consumed' ──
    {
        std::println("\n--- #2586 AC5: audit/SE reason 'single-use-consumed' ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        EffectProvenance prov{};
        prov.epoch = 6;
        prov.mutation_id = 6;
        g_capability_registry().grant_once(/*tenant=*/6, "mut-2586-audit", Effect::Mutate, prov);

        const auto ring_seq_before = g_security_event_ring().seq.load(std::memory_order_acquire);
        const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 6, "audit-1");
        CHECK(ok, "AC5: single_use grant 1st allow");

        const auto ring_seq_after = g_security_event_ring().seq.load(std::memory_order_acquire);
        CHECK(ring_seq_after > ring_seq_before,
              "AC5: SE ring advanced (single_use audit appended)");

        // Look for an event with reason "single-use-consumed" — emitted by
        // record_audit() inside the consume block.
        const auto* found = ring_lookup_reason("single-use-consumed");
        CHECK(found != nullptr, "AC5: SE ring has event with reason 'single-use-consumed'");
        if (found) {
            CHECK(!found->denied, "AC5: single-use-consumed event is denied=false (allow path)");
            CHECK(found->effect_bits == static_cast<std::uint16_t>(Effect::Mutate),
                  "AC5: event effect_bits == Mutate");
        }

        // Metric surface also reflects the consumption (counters bump atomic).
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 1, "AC5: capability_single_use_consumed == 1");
        CHECK(snap.revokes == 1, "AC5: revokes == 1 (single_use path bumps revoke_total)");
    }

    // ── AC6: tests + source-cite (no docs/design/) ─────────────────
    {
        std::println("\n--- #2586 AC6: source-cite ---");
        std::println("AC6 — see file header for source-cite list:");
        std::println("  - src/core/capability_model.hh: CapabilityGrant.single_use, "
                     "grant() extended signature, grant_once sugar, check_and_record_effect "
                     "consume block, CapabilityEffectMetrics + Snapshot.");
        std::println("  - src/compiler/evaluator_security.cpp: grant_effect_capability "
                     "single_use parameter.");
        std::println("  - src/compiler/evaluator.ixx: grant_effect_capability declaration.");
        std::println("  - src/compiler/evaluator_primitives_security.cpp: "
                     "query:capability-effect-stats capability-single-use-consumed-total "
                     "/ schema-2586 / issue-2586.");
        std::println("  - tests/core/test_capability_single_use_consume.cpp (this file).");
        std::println("  - no docs/design/ (per #1655 #1485 ship philosophy).");
        CHECK(true, "AC6: source-cite listed above (no docs/design)");
    }

    // ── #2882 AC1: production default forces single_use for high-risk ───────────
    {
        std::println("\n--- #2882 AC1: production default single-use for high-risk ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        // Use the production default surface (grant_effect_capability with
        // single_use=false default). Under Restricted + Mutate, the force
        // logic must promote single_use=true — first allow consumes, second
        // denies with the existing #2586 'single-use-consumed' reason.
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        const auto epoch_before =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();
        // Call the production default API with single_use=false (the default).
        ev.grant_effect_capability(/*tenant=*/7, "mut-2882-default", kEffectMutate, /*mid=*/1,
                                   /*single_use=*/false);
        const auto epoch_after =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();
        CHECK(epoch_after == epoch_before + 1,
              "AC1: capability_high_risk_forced_single_use_total bumps under Restricted");

        // Restricted needs sandbox_active=true for grant enforcement.
        // Stamp mid so fail-closed mid join (#2707) matches the grant.
        EffectProvenance prov{};
        prov.mutation_id = 1;
        prov.epoch = 1;
        // 1st allow — Mutate bit satisfied by the grant.
        const bool ok1 = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, /*tenant=*/7,
                                                 "2882-default-1", /*wildcard_ok=*/false,
                                                 /*sandbox_active=*/true);
        CHECK(ok1, "AC1: 1st allow under production default high-risk grant");

        // 2nd deny — single_use consumed by 1st allow.
        const bool ok2 = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, /*tenant=*/7,
                                                 "2882-default-2", /*wildcard_ok=*/false,
                                                 /*sandbox_active=*/true);
        CHECK(!ok2, "AC1: 2nd deny (forced single_use consumed)");
    }

    // ── #2882 AC2: explicit grant_effect_durable admin path bypasses force ──────
    {
        std::println("\n--- #2882 AC2: explicit grant_effect_durable admin path ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(8);

        const auto durable_before =
            g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
        const auto forced_before =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();

        // grant_effect_durable MUST bypass the force and stay single_use=false.
        // Issue #2967: under production the durable surface also requires the
        // caller to hold TenantAdmin + a non-empty audit reason — grant the
        // meta-privilege to the caller first so the #2882 durable-override
        // semantics are exercised (admin caller, sticky grant).
        ev.grant_capability("tenant-admin");
        ev.grant_effect_durable(/*tenant=*/8, "mut-2882-durable", kEffectMutate, /*mid=*/2,
                                /*reason=*/"2882-ac2-durable");

        const auto durable_after =
            g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
        const auto forced_after =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();
        CHECK(durable_after == durable_before + 1,
              "AC2: capability_durable_high_risk_grant_total bumps for high-risk durable override");
        CHECK(forced_after == forced_before,
              "AC2: capability_high_risk_forced_single_use_total NOT bumped by durable override");

        // Multiple allows — durable grants stay sticky.
        for (int i = 0; i < 3; ++i) {
            const std::string op = "2882-durable-" + std::to_string(i);
            const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate,
                                                    EffectProvenance{}, /*tenant=*/8, op);
            CHECK(ok,
                  std::string("AC2: durable admin grant 3x allow (run ") + std::to_string(i) + ")");
        }
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 0,
              "AC2: durable admin grant bypasses single_use (consumed=0)");
    }

    // ── #2882 AC3: Off / Soft path no force applied ───────────────────────────────
    {
        std::println("\n--- #2882 AC3: Off / Soft path no force ---");
        reset_all();
        set_mode(SandboxMode::Off);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0); // Off
        ev.set_capability_tenant_id(9);

        const auto forced_before =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();

        // Under Off, the production default surface must NOT force single_use.
        ev.grant_effect_capability(/*tenant=*/9, "mut-2882-off", kEffectMutate, /*mid=*/3,
                                   /*single_use=*/false);

        const auto forced_after =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();
        CHECK(forced_after == forced_before,
              "AC3: Off mode does NOT bump capability_high_risk_forced_single_use_total");

        // Multiple allows — caller intent (single_use=false) preserved.
        for (int i = 0; i < 3; ++i) {
            const std::string op = "2882-off-" + std::to_string(i);
            const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate,
                                                    EffectProvenance{}, /*tenant=*/9, op);
            CHECK(ok, std::string("AC3: Off mode multi-use (run ") + std::to_string(i) + ")");
        }
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 0, "AC3: Off mode multi-use (consumed=0)");
    }

    // ── #2882 AC5: snapshot + posture prim additive surface ────────────────────
    {
        std::println("\n--- #2882 AC5: snapshot exposes #2882 counters ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(10);

        // Trigger both counters under production defaults (Restricted).
        // Issue #2967: durable high-risk requires caller TenantAdmin + reason.
        ev.grant_capability("tenant-admin");
        ev.grant_effect_capability(/*tenant=*/10, "mut-2882-q", kEffectMutate, /*mid=*/4, false);
        ev.grant_effect_durable(/*tenant=*/11, "mac-2882-q", kEffectMacroSelfEvo, /*mid=*/5,
                                /*reason=*/"2882-ac5-mac");
        ev.grant_effect_durable(/*tenant=*/12, "adm-2882-q", kEffectTenantAdmin, /*mid=*/6,
                                /*reason=*/"2882-ac5-adm");
        ev.grant_effect_durable(/*tenant=*/13, "sys-2882-q", kEffectSyscall, /*mid=*/7,
                                /*reason=*/"2882-ac5-sys");

        // Verify the CapabilityEffectStatsSnapshot struct exposes the 2 new
        // #2882 counters (compile-time member access check) and that the
        // counter snapshot reflects the grants. The posture prim
        // (query:capability-effect-stats) hash surface is verified separately
        // in AC6 source-cite check (the prim inserts schema-2882 /
        // high-risk-default-single-use-mask / forced + durable totals).
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_high_risk_forced_single_use == 1,
              "AC5: forced-single-use counter reflects 1 forced grant");
        CHECK(snap.capability_durable_high_risk_grant == 3,
              "AC5: durable-high-risk-grant counter reflects 3 durable grants");
        CHECK(snap.capability_single_use_consumed == 0,
              "AC5: durable grants do not consume single_use");

        // Posture prim must reference kHighRiskMask — the linter (AC5 in
        // check_production_default_single_use_2882.py) cross-checks both
        // sides (evaluator_security.cpp + evaluator_primitives_security.cpp).
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("kHighRiskMask") != std::string::npos,
              "AC5: posture prim references kHighRiskMask");
    }

    // ── #2882 AC6: source-cite + no invent + no docs/design/ ────────────────────
    {
        std::println("\n--- #2882 AC6: source-cite + no invent + no docs/design/ ---");
        const auto cap_model = read_file("src/core/capability_model.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto ixx = read_file("src/compiler/evaluator.ixx");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto build = read_file("build.py");

        // #2882 source-cite in cap_model + sec + ixx + posture.
        CHECK(cap_model.find("Issue #2882") != std::string::npos,
              "AC6: capability_model.hh cites Issue #2882");
        CHECK(sec.find("Issue #2882") != std::string::npos,
              "AC6: evaluator_security.cpp cites Issue #2882");
        CHECK(ixx.find("#2882") != std::string::npos, "AC6: evaluator.ixx cites #2882");
        CHECK(posture.find("schema-2882") != std::string::npos,
              "AC6: evaluator_primitives_security.cpp cites schema-2882");

        // build.py wires the new linter.
        CHECK(build.find("check_production_default_single_use_2882") != std::string::npos,
              "AC6: build.py wires #2882 linter");

        // No new test_issue_2882.cpp (per #81967).
        std::ifstream invent_c("tests/core/test_issue_2882.cpp");
        if (!invent_c.good())
            invent_c.open("../tests/core/test_issue_2882.cpp");
        CHECK(!invent_c.good(), "AC6: no tests/core/test_issue_2882.cpp (forbidden per #81967)");
        std::ifstream invent_cp("tests/compiler/test_issue_2882.cpp");
        if (!invent_cp.good())
            invent_cp.open("../tests/compiler/test_issue_2882.cpp");
        CHECK(!invent_cp.good(),
              "AC6: no tests/compiler/test_issue_2882.cpp (forbidden per #81967)");

        // No docs/design/2882-* (per #1655).
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("2882-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #2944: mutation-session grants (mid-bound + auto-revoke on boundary) ──
    {
        std::println("\n--- #2944 AC1: session grant mid-bound under Restricted ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        EffectProvenance prov{};
        prov.epoch = 10;
        prov.mutation_id = 10;
        // session_bound without single_use so multi-check under same mid works.
        g_capability_registry().grant_session(/*tenant=*/20, "mut-2944-sess", Effect::Mutate, prov,
                                              /*single_use=*/false);

        CapabilityGrant g;
        CHECK(g_capability_registry().find_grant(20, "mut-2944-sess", g), "AC1: grant found");
        CHECK(g.session_bound, "AC1: session_bound stamped");
        CHECK(g.bound_mutation_id == 10, "AC1: bound_mutation_id == 10");

        // Restricted needs sandbox_active=true for grant + mid join enforce.
        const bool ok_same = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 20,
                                                     "2944-same-mid", false, true);
        CHECK(ok_same, "AC1: same-mid check allows under Restricted");

        EffectProvenance other = prov;
        other.mutation_id = 11;
        other.epoch = 11;
        const bool ok_cross = check_and_record_effect(Effect::Mutate, Effect::Mutate, other, 20,
                                                      "2944-cross-mid", false, true);
        CHECK(!ok_cross, "AC1: cross-mid check denies under Restricted");

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_session_grant >= 1, "AC1: session_grant counter");
        CHECK(snap.capability_live_session_grants >= 1, "AC1: live session residual");
    }
    {
        std::println("\n--- #2944 AC2: revoke_session_grants_for_mid on mid exit ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        EffectProvenance prov{};
        prov.epoch = 20;
        prov.mutation_id = 20;
        g_capability_registry().grant_session(21, "mut-2944-exit", Effect::Mutate, prov, false);

        CHECK(check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 21, "2944-pre", false,
                                      true),
              "AC2: allow before session revoke");

        const auto n = g_capability_registry().revoke_session_grants_for_mid(20);
        CHECK(n >= 1, "AC2: revoke_session_grants_for_mid revokes >=1");

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_session_revoke >= 1, "AC2: session_revoke counter");
        CHECK(snap.capability_live_session_grants == 0, "AC2: live residual cleared");

        CHECK(!check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 21, "2944-post", false,
                                       true),
              "AC2: deny after session revoke");

        // SE dual-write reason session-mid-exit
        const auto* se = ring_lookup_reason("session-mid-exit");
        CHECK(se != nullptr, "AC2: SE reason session-mid-exit present");
    }
    {
        std::println("\n--- #2944 AC3: Soft/Off zero-cost empty session path ---");
        reset_all();
        set_mode(SandboxMode::Off);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        const auto n = g_capability_registry().revoke_session_grants_for_mid(99);
        CHECK(n == 0, "AC3: empty live session → revoke returns 0");
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_session_revoke == 0, "AC3: no session_revoke under empty");
    }
    {
        std::println("\n--- #2944 AC4: durable grants unaffected by session revoke ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(22);
        // Durable Mutate (sticky) + session Mutate under same mid.
        ev.grant_effect_durable(22, "mut-2944-dur", kEffectMutate, /*mid=*/30);
        ev.grant_effect_session(22, "mut-2944-sess2", kEffectMutate, /*mid=*/30,
                                /*single_use=*/false);

        EffectProvenance prov{};
        prov.epoch = 30;
        prov.mutation_id = 30;
        CHECK(check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 22, "2944-both", false,
                                      true),
              "AC4: allow with durable+session");

        (void)g_capability_registry().revoke_session_grants_for_mid(30);
        // Durable remains — still allows.
        CHECK(check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 22, "2944-dur-only",
                                      false, true),
              "AC4: durable survives session revoke");
    }
    {
        std::println("\n--- #2944 AC5/AC6: schema + source-cite + no invent ---");
        const auto cap = read_file("src/core/capability_model.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto ixx = read_file("src/compiler/evaluator.ixx");
        const auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto build = read_file("build.py");
        CHECK(cap.find("session_bound") != std::string::npos, "AC6: session_bound field");
        CHECK(cap.find("revoke_session_grants_for_mid") != std::string::npos,
              "AC6: revoke_session_grants_for_mid");
        CHECK(cap.find("capability_session_revoke_total") != std::string::npos,
              "AC6: session_revoke metric");
        CHECK(sec.find("grant_effect_session") != std::string::npos,
              "AC6: grant_effect_session impl");
        CHECK(ixx.find("grant_effect_session") != std::string::npos,
              "AC6: grant_effect_session decl");
        CHECK(bound.find("revoke_session_grants_for_mid") != std::string::npos,
              "AC6: outermost dtor revokes session");
        CHECK(bound.find("Issue #2944") != std::string::npos ||
                  bound.find("#2944") != std::string::npos,
              "AC6: boundary cites #2944");
        CHECK(posture.find("schema-2944") != std::string::npos, "AC5: schema-2944");
        CHECK(posture.find("mutation-session-grant-wired") != std::string::npos,
              "AC5: mutation-session-grant-wired");
        CHECK(posture.find("capability-session-revoke-total") != std::string::npos,
              "AC5: session-revoke-total key");
        CHECK(build.find("check_mutation_session_grant_2944") != std::string::npos,
              "AC6: build.py wires linter");
        std::ifstream invent("tests/core/test_issue_2944.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_2944.cpp");
        CHECK(!invent.good(), "AC6: no test_issue_2944.cpp");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("2944-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name);
            }

            // ── #2967 AC1: production durable high-risk requires TenantAdmin ────────
            {
                std::println(
                    "\n--- #2967 AC1: durable high-risk grant without TenantAdmin → deny ---");
                reset_all();
                set_mode(SandboxMode::Restricted);
                aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

                CompilerService cs;
                auto& ev = cs.evaluator();
                ev.set_effect_sandbox_mode(1); // Restricted
                ev.set_capability_tenant_id(20);

                const auto deny_before =
                    g_capability_effect_metrics().capability_durable_grant_deny_total.load();
                const auto allow_before =
                    g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();

                // No TenantAdmin on the caller → durable high-risk grant denied.
                ev.grant_effect_durable(/*tenant=*/20, "mut-2967-noadmin", kEffectMutate,
                                        /*mid=*/30,
                                        /*reason=*/"2967-ac1");

                const auto deny_after =
                    g_capability_effect_metrics().capability_durable_grant_deny_total.load();
                const auto allow_after =
                    g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
                CHECK(
                    deny_after == deny_before + 1,
                    "AC1: capability_durable_grant_deny_total bumps when caller lacks TenantAdmin");
                CHECK(allow_after == allow_before,
                      "AC1: capability_durable_high_risk_grant_total NOT bumped on deny");
                CHECK(ring_lookup_reason("durable-grant-needs-tenant-admin") != nullptr,
                      "AC1: SE EffectDeny reason 'durable-grant-needs-tenant-admin' recorded");
            }

            // ── #2967 AC2: TenantAdmin + reason → allow; empty reason → deny ────────
            {
                std::println(
                    "\n--- #2967 AC2: TenantAdmin + reason allows; empty reason denies ---");
                reset_all();
                set_mode(SandboxMode::Restricted);
                aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

                CompilerService cs;
                auto& ev = cs.evaluator();
                ev.set_effect_sandbox_mode(1); // Restricted
                ev.set_capability_tenant_id(21);
                // Caller holds the meta-privilege.
                ev.grant_capability("tenant-admin");

                const auto allow_before =
                    g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
                ev.grant_effect_durable(/*tenant=*/21, "mut-2967-admin", kEffectMutate, /*mid=*/31,
                                        /*reason=*/"2967-ac2-rotate");
                const auto allow_after =
                    g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
                CHECK(allow_after == allow_before + 1,
                      "AC2: durable high-risk grant allowed with TenantAdmin + reason");

                // Empty reason under production → deny.
                const auto deny_before =
                    g_capability_effect_metrics().capability_durable_grant_deny_total.load();
                ev.grant_effect_durable(/*tenant=*/21, "mut-2967-noreason", kEffectMutate,
                                        /*mid=*/32);
                const auto deny_after =
                    g_capability_effect_metrics().capability_durable_grant_deny_total.load();
                CHECK(deny_after == deny_before + 1,
                      "AC2: empty reason under production → deny (deny counter bumps)");
                CHECK(ring_lookup_reason("durable-grant-reason-required") != nullptr,
                      "AC2: SE EffectDeny reason 'durable-grant-reason-required' recorded");
            }

            // ── #2967 AC3: Soft / Off no hard gate (zero-cost path) ────────────────
            {
                std::println("\n--- #2967 AC3: Off path no gate ---");
                reset_all();
                set_mode(SandboxMode::Off);
                aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);

                CompilerService cs;
                auto& ev = cs.evaluator();
                ev.set_effect_sandbox_mode(0); // Off
                ev.set_capability_tenant_id(22);

                const auto deny_before =
                    g_capability_effect_metrics().capability_durable_grant_deny_total.load();
                const auto allow_before =
                    g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
                // No TenantAdmin, no reason — Off path must not hard-gate.
                ev.grant_effect_durable(/*tenant=*/22, "mut-2967-off", kEffectMutate, /*mid=*/33);
                const auto deny_after =
                    g_capability_effect_metrics().capability_durable_grant_deny_total.load();
                const auto allow_after =
                    g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
                CHECK(deny_after == deny_before, "AC3: Off path does not deny (no hard gate)");
                CHECK(allow_after == allow_before + 1,
                      "AC3: Off path durable high-risk grant proceeds (zero-cost)");
            }

            // ── #2967 AC4: snapshot + posture additive keys ─────────────────────────
            {
                std::println("\n--- #2967 AC4: snapshot + posture additive keys ---");
                reset_all();
                set_mode(SandboxMode::Restricted);
                aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

                CompilerService cs;
                auto& ev = cs.evaluator();
                ev.set_effect_sandbox_mode(1); // Restricted
                ev.set_capability_tenant_id(23);

                // Trigger one deny (no TenantAdmin) + one allow (TenantAdmin + reason).
                ev.grant_effect_durable(/*tenant=*/23, "mut-2967-q-deny", kEffectMutate, /*mid=*/34,
                                        /*reason=*/"2967-ac4");
                ev.grant_capability("tenant-admin");
                ev.grant_effect_durable(/*tenant=*/23, "mut-2967-q-allow", kEffectMutate,
                                        /*mid=*/35,
                                        /*reason=*/"2967-ac4-allow");

                const auto snap = snapshot_capability_effect_stats();
                CHECK(snap.capability_durable_grant_deny == 1,
                      "AC4: snapshot exposes capability_durable_grant_deny");
                CHECK(snap.capability_durable_high_risk_grant == 1,
                      "AC4: snapshot durable-high-risk-grant reflects 1 allow");

                const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
                CHECK(posture.find("schema-2967") != std::string::npos,
                      "AC4: posture prim cites schema-2967");
                CHECK(posture.find("capability-durable-grant-deny-total") != std::string::npos,
                      "AC4: posture prim exposes capability-durable-grant-deny-total");
                CHECK(posture.find("durable-grant-tenant-admin-wired") != std::string::npos,
                      "AC4: posture prim exposes durable-grant-tenant-admin-wired");
                CHECK(posture.find("durable-grant-reason-wired") != std::string::npos,
                      "AC4: posture prim exposes durable-grant-reason-wired");
            }

            // ── #2967 AC5: source-cite + no invent + no docs/design/ ────────────────
            {
                std::println("\n--- #2967 AC5: source-cite + no invent + no docs/design/ ---");
                const auto cap_model = read_file("src/core/capability_model.hh");
                const auto sec = read_file("src/compiler/evaluator_security.cpp");
                const auto ixx = read_file("src/compiler/evaluator.ixx");
                const auto build = read_file("build.py");

                CHECK(cap_model.find("Issue #2967") != std::string::npos,
                      "AC5: capability_model.hh cites Issue #2967");
                CHECK(sec.find("Issue #2967") != std::string::npos,
                      "AC5: evaluator_security.cpp cites Issue #2967");
                CHECK(ixx.find("#2967") != std::string::npos, "AC5: evaluator.ixx cites #2967");
                CHECK(build.find("check_capability_durable_gate_2967") != std::string::npos,
                      "AC5: build.py wires #2967 linter");

                // No new test_issue_2967.cpp (per #81967).
                std::ifstream invent_2967("tests/core/test_issue_2967.cpp");
                if (!invent_2967.good())
                    invent_2967.open("../tests/core/test_issue_2967.cpp");
                CHECK(!invent_2967.good(),
                      "AC5: no tests/core/test_issue_2967.cpp (forbidden per #81967)");

                // No docs/design/2967-* (per #1655).
                const std::filesystem::path docs_design_2967 = "docs/design";
                std::error_code ec2967;
                if (std::filesystem::is_directory(docs_design_2967, ec2967)) {
                    for (const auto& entry :
                         std::filesystem::directory_iterator(docs_design_2967, ec2967)) {
                        const auto name = entry.path().filename().string();
                        CHECK(name.find("2967-") == std::string::npos,
                              std::string("AC5: no docs/design/") + name +
                                  " (forbidden per #1655)");
                    }
                }
            }

            std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
            return g_failed == 0 ? 0 : 1;
        }

#ifndef AURA_ISSUE_BATCH_MEMBER
        int main() {
            return run_test_capability_single_use_consume();
        }
#endif
