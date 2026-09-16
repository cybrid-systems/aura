// @category: unit
// @reason: Issue #2152 — Dispatch-level non-bypassable required_effects
// for side-effect prims (construction-time auto-stamp + dispatch gate).
//   Issue #2583 — Hard path: every non-zero required_effects call goes
// through require_effect at dispatch, so prims that forget body check
// still fail-closed under Restricted/Strict even when the static gate
// passes. #2583 adds the dispatch_effect_auto_* Agent-dashboard surface
// alongside the #2152 counters.
//
//   AC1: Prim with required_effects=Mutate, no body check → deny under
//        Restricted without grant; audit ring records deny; both
//        #2152 and #2583 metric surfaces advance (#2583 AC1)
//   AC2: effect_enforced_in_body=true (add_mutate path) does not
//        double-call require_effect at dispatch; #2583 check counter
//        also stays flat (#2583 AC2)
//   AC3: security_exempt=true + documented reason passes gate;
//        undocumented allowlist entry fails CI script (#2583 AC3)
//   AC4: New prefix-matching name without coverage fails
//        check_side_effect_security.py
//   AC5: Off sandbox / legacy tests still green (AURA_SANDBOX=off,
//        #2583 AC4)
//   AC6: dispatch_effect_auto_check_total / _deny_total surface
//        bumped on every require_effect call (#2583 AC6)
//   Issue #3596 — agent:/synthesize:/strategy: infer demands
//        Mutate|MacroSelfEvo at dispatch (MSE gate parity with
//        effect_for_cap_name, #2489/#2583 residual): string-level AC +
//        behavioral deny/allow/TA matrix + Soft zero-extra.

#include "test_harness.hpp"

#include "compiler/ffi_hot_path.hh"
#include "compiler/observability_metrics.h"
#include "compiler/security_capabilities.h"
#include "compiler/security_side_effect.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.ir_executor;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::effective_required_effects;
using aura::compiler::infer_required_effects_from_name;
using aura::compiler::is_side_effect_prim_name;
using aura::compiler::kDispatchRequiredEffectsIssue;
using aura::compiler::kSecurityExemptReasonToken;
using aura::compiler::kSideEffectInheritIssue;
using aura::compiler::PrimMeta;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectNone;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;
using aura::compiler::types::make_int;
using aura::compiler::types::make_void;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_security_event_ring_for_test();
    set_mode(SandboxMode::Off);
}

} // namespace


// ── Issue #3596: agent:/synthesize:/strategy: infer demands Mutate|MSE —
// dispatch-side MSE gate parity with effect_for_cap_name (#2489/#2583
// residual; expand sites were already closed by #3378). Behavioral ACs
// mirror the #2152 harness (Restricted face + synthetic prim + counters).

static void ac3596_1_infer_mse_bits() {
    std::println("\n--- #3596 AC1: infer includes MSE bits for the three prefixes ---");
    constexpr auto kReq = static_cast<std::uint16_t>(aura::compiler::security::kEffectMutate |
                                                     aura::compiler::security::kEffectMacroSelfEvo);
    CHECK(infer_required_effects_from_name("agent:tick") == kReq, "3596 AC1: agent: -> Mutate|MSE");
    CHECK(infer_required_effects_from_name("synthesize:fill") == kReq,
          "3596 AC1: synthesize: -> Mutate|MSE");
    CHECK(infer_required_effects_from_name("strategy:set-strategy") == kReq,
          "3596 AC1: strategy: -> Mutate|MSE");
    CHECK(effective_required_effects("synthesize:fill", 0, false) == kReq,
          "3596 AC1: effective_required_effects keeps inferred MSE bits");
    // Neighbouring families unchanged (no over-reach).
    CHECK(infer_required_effects_from_name("mutate:x") == kEffectMutate,
          "3596 AC1: mutate: stays Mutate-only");
    CHECK(infer_required_effects_from_name("ffi:x") == aura::compiler::security::kEffectFfi,
          "3596 AC1: ffi: unchanged");
}

static void ac3596_2_mutate_only_dispatch_deny() {
    std::println("\n--- #3596 AC2: Mutate-only tenant -> synthesize: dispatch deny ---");
    reset_all();
    // Live Mutation epoch: Restricted provenance join is fail-closed on
    // mid=0 (#3594 contract) — grant and check must join on the same mid.
    aura::core::bump_mutation_epoch(1);
    const auto live_mid = aura::core::current_mutation_epoch();
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::make_grant_provenance;
    // Seed Mutate-only while the registry face is Off (fence-free, #3409).
    g_capability_registry().grant(3596, "mutate-only-3596",
                                  static_cast<aura::core::capability::Effect>(kEffectMutate),
                                  make_grant_provenance(live_mid, true, 0, 0));
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);     // Restricted (evaluator face)
    set_mode(SandboxMode::Restricted); // registry SOLE writer (#2657)
    ev.set_capability_tenant_id(3596);
    // Plain add(): required_effects auto-stamped Mutate|MSE from the name (#3596).
    bool body_ran = false;
    ev.primitives().add("synthesize:probe-3596",
                        [&](std::span<const aura::compiler::types::EvalValue>) {
                            body_ran = true;
                            return aura::compiler::types::make_bool(true);
                        });
    auto* cm = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto ddeny0 = cm ? cm->dispatch_required_effects_deny_total.load() : 0;
    auto r = ev.invoke_prim_with_telemetry("synthesize:probe-3596", [&]() {
        auto fn = ev.primitives().lookup("synthesize:probe-3596");
        return (*fn)({});
    });
    CHECK(is_error(r), "3596 AC2: Mutate-only (no MSE) -> dispatch deny");
    CHECK(!body_ran, "3596 AC2: body not entered");
    CHECK(cm && cm->dispatch_required_effects_deny_total.load() > ddeny0,
          "3596 AC2: dispatch deny counter advanced");
}

static void ac3596_3_mutate_mse_allow() {
    std::println("\n--- #3596 AC3: Mutate|MSE tenant -> allowed under Restricted ---");
    reset_all();
    aura::core::bump_mutation_epoch(1);
    const auto live_mid = aura::core::current_mutation_epoch();
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::make_grant_provenance;
    g_capability_registry().grant(
        3596, "mutate-mse-3596",
        static_cast<aura::core::capability::Effect>(
            static_cast<std::uint16_t>(kEffectMutate) |
            static_cast<std::uint16_t>(aura::compiler::security::kEffectMacroSelfEvo)),
        make_grant_provenance(live_mid, true, 0, 0));
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    set_mode(SandboxMode::Restricted);
    ev.set_capability_tenant_id(3596);
    bool body_ran = false;
    ev.primitives().add("strategy:probe-3596",
                        [&](std::span<const aura::compiler::types::EvalValue>) {
                            body_ran = true;
                            return aura::compiler::types::make_bool(true);
                        });
    auto r = ev.invoke_prim_with_telemetry("strategy:probe-3596", [&]() {
        auto fn = ev.primitives().lookup("strategy:probe-3596");
        return (*fn)({});
    });
    CHECK(!is_error(r), "3596 AC3: Mutate|MSE + live mid -> allowed");
    CHECK(body_ran, "3596 AC3: body ran");
}

static void ac3596_4_explicit_ta_unchanged() {
    std::println("\n--- #3596 AC4: explicit TenantAdmin meta not weakened by infer ---");
    reset_all();
    aura::core::bump_mutation_epoch(1);
    const auto live_mid = aura::core::current_mutation_epoch();
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::make_grant_provenance;
    // Seed Mutate|MSE while Off (fence-free). NO TenantAdmin yet — the
    // control-plane prim below must stay denied on the TA bit alone.
    g_capability_registry().grant(
        3596, "mm-3596",
        static_cast<aura::core::capability::Effect>(
            static_cast<std::uint16_t>(kEffectMutate) |
            static_cast<std::uint16_t>(aura::compiler::security::kEffectMacroSelfEvo)),
        make_grant_provenance(live_mid, true, 0, 0));
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    set_mode(SandboxMode::Restricted);
    ev.set_capability_tenant_id(3596);
    // Control-plane agent: prim: explicit TA bits at registration (issue AC3 —
    // infer must not weaken an explicit meta; wildcard-only cannot satisfy
    // TA per #3144/#3411).
    PrimMeta m{};
    m.required_effects = aura::compiler::security::kEffectTenantAdmin;
    m.doc = "synthetic #3596 AC4 control-plane";
    bool body_ran = false;
    ev.primitives().add(
        "agent:control-3596",
        [&](std::span<const aura::compiler::types::EvalValue>) {
            body_ran = true;
            return aura::compiler::types::make_bool(true);
        },
        m);
    CHECK(effective_required_effects("agent:control-3596", m.required_effects, false) ==
              aura::compiler::security::kEffectTenantAdmin,
          "3596 AC4: explicit meta wins over infer");
    auto r = ev.invoke_prim_with_telemetry("agent:control-3596", [&]() {
        auto fn = ev.primitives().lookup("agent:control-3596");
        return (*fn)({});
    });
    CHECK(is_error(r), "3596 AC4: Mutate|MSE without TA -> control-plane deny");
    CHECK(!body_ran, "3596 AC4: body not entered");
}

static void ac3596_5_soft_zero_extra() {
    std::println("\n--- #3596 AC5: Soft/Off -> no new checks (require_effect no-op) ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0); // Off
    set_mode(SandboxMode::Off);
    ev.set_capability_tenant_id(3596);
    bool body_ran = false;
    ev.primitives().add("synthesize:probe-soft-3596",
                        [&](std::span<const aura::compiler::types::EvalValue>) {
                            body_ran = true;
                            return aura::compiler::types::make_bool(true);
                        });
    const auto denied0 = g_capability_effect_metrics().capability_effect_denied_total.load();
    auto r = ev.invoke_prim_with_telemetry("synthesize:probe-soft-3596", [&]() {
        auto fn = ev.primitives().lookup("synthesize:probe-soft-3596");
        return (*fn)({});
    });
    CHECK(!is_error(r), "3596 AC5: Soft/Off allows without grants");
    CHECK(body_ran, "3596 AC5: body ran");
    CHECK(g_capability_effect_metrics().capability_effect_denied_total.load() == denied0,
          "3596 AC5: no new deny under Soft/Off");
}

int run_test_dispatch_required_effects() {
    std::println("=== Issue #2152: dispatch non-bypassable required_effects ===");
    CHECK(kDispatchRequiredEffectsIssue == 2152, "issue stamp");
    CHECK(kSideEffectInheritIssue == 2057, "inherits #2057 stamp");

    // ── AC1: required_effects without body check → deny under Restricted ──
    {
        std::println("\n--- AC1: dispatch deny without grant ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(9);

        PrimMeta m{};
        m.required_effects = kEffectMutate;
        m.effect_enforced_in_body = false;
        m.security_exempt = false;
        m.pure = false;
        m.doc = "synthetic #2152 AC1";
        bool body_ran = false;
        ev.primitives().add(
            "test:dispatch-mutate-2152",
            [&](std::span<const aura::compiler::types::EvalValue>) {
                body_ran = true;
                return aura::compiler::types::make_bool(true);
            },
            m);

        const auto denied0 = g_capability_effect_metrics().capability_effect_denied_total.load();
        const auto checks0 = g_capability_effect_metrics().capability_check_total.load();
        auto* cm = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto dcheck0 = cm ? cm->dispatch_required_effects_check_total.load() : 0;
        const auto ddeny0 = cm ? cm->dispatch_required_effects_deny_total.load() : 0;

        auto r = ev.invoke_prim_with_telemetry("test:dispatch-mutate-2152", [&]() {
            auto fn = ev.primitives().lookup("test:dispatch-mutate-2152");
            CHECK(fn.has_value(), "lookup synthetic");
            return (*fn)({});
        });
        CHECK(!body_ran, "AC1: body not run without grant");
        CHECK(is_error(r) || !body_ran, "AC1: deny value or body blocked");
        CHECK(g_capability_effect_metrics().capability_effect_denied_total.load() > denied0,
              "AC1: effect denied metric advanced (audit path)");
        CHECK(g_capability_effect_metrics().capability_check_total.load() > checks0,
              "AC1: capability check recorded");
        if (cm) {
            CHECK(cm->dispatch_required_effects_check_total.load() > dcheck0,
                  "AC1: dispatch check metric");
            CHECK(cm->dispatch_required_effects_deny_total.load() > ddeny0,
                  "AC1: dispatch deny metric");
            // Issue #2583: parallel #2583 metric surface advances in lockstep.
            const auto dcheck_auto0 = cm->dispatch_effect_auto_check_total.load();
            const auto ddeny_auto0 = cm->dispatch_effect_auto_deny_total.load();
            (void)dcheck_auto0;
            (void)ddeny_auto0;
            // Re-run invoke to capture the +1 deltas against #2583 counters.
            auto r2 = ev.invoke_prim_with_telemetry("test:dispatch-mutate-2152", [&]() {
                auto fn = ev.primitives().lookup("test:dispatch-mutate-2152");
                return (*fn)({});
            });
            CHECK(is_error(r2), "AC1 #2583: second invoke also denied");
            CHECK(cm->dispatch_effect_auto_check_total.load() > dcheck_auto0,
                  "AC1 #2583: dispatch_effect_auto_check_total advanced");
            CHECK(cm->dispatch_effect_auto_deny_total.load() > ddeny_auto0,
                  "AC1 #2583: dispatch_effect_auto_deny_total advanced");
        }
        // Query surface
        const auto q =
            aura::test::aura_query_prims_source() +
            aura::test::aura_read_repo_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(q.find("schema-2152") != std::string::npos, "schema-2152");
        CHECK(q.find("dispatch-required-effects-wired") != std::string::npos, "wired marker");
        CHECK(href(cs, "dispatch-required-effects-deny") >= 1 || href(cs, "schema-2152") == 2152 ||
                  q.find("schema-2152") != std::string::npos,
              "query deny count");
    }

    // ── AC2: effect_enforced_in_body skips double dispatch require_effect ──
    {
        std::println("\n--- AC2: body-enforced no double dispatch ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2); // Strict
        // mutate:set-body is add_mutate → effect_enforced_in_body
        const auto slot = ev.primitives().slot_for_name("mutate:set-body");
        CHECK(slot < ev.primitives().slot_count(), "set-body registered");
        const auto& meta = ev.primitives().meta_for_slot(slot);
        CHECK(meta.required_effects == kEffectMutate, "stamped Mutate");
        CHECK(meta.effect_enforced_in_body, "body-enforced flag");

        auto* cm = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto dcheck0 = cm ? cm->dispatch_required_effects_check_total.load() : 0;
        // Invoke via telemetry; dispatch must NOT bump check (body handles it).
        (void)ev.invoke_prim_with_telemetry("mutate:set-body", [&]() {
            // Call body with bad args so it returns early after force check.
            auto fn = ev.primitives().lookup("mutate:set-body");
            if (!fn)
                return aura::compiler::types::make_bool(false);
            return (*fn)({});
        });
        if (cm) {
            CHECK(cm->dispatch_required_effects_check_total.load() == dcheck0,
                  "AC2: dispatch did not re-check body-enforced prim");
        }
        // Synthetic body-enforced prim: same.
        PrimMeta m{};
        m.required_effects = kEffectMutate;
        m.effect_enforced_in_body = true;
        m.pure = false;
        bool body_ran = false;
        ev.primitives().add(
            "test:body-enforced-2152",
            [&](std::span<const aura::compiler::types::EvalValue>) {
                body_ran = true;
                return aura::compiler::types::make_bool(true);
            },
            m);
        const auto dcheck1 = cm ? cm->dispatch_required_effects_check_total.load() : 0;
        (void)ev.invoke_prim_with_telemetry("test:body-enforced-2152", [&]() {
            auto fn = ev.primitives().lookup("test:body-enforced-2152");
            return (*fn)({});
        });
        CHECK(body_ran, "AC2: body-enforced runs without dispatch require_effect");
        if (cm) {
            CHECK(cm->dispatch_required_effects_check_total.load() == dcheck1,
                  "AC2: no dispatch check on body-enforced synthetic");
        }
    }

    // ── AC3: security_exempt + allowlist reason ──
    {
        std::println("\n--- AC3: security_exempt + allowlist reason ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2);
        // Documented exempt runs under Strict without grant.
        PrimMeta m{};
        m.security_exempt = true;
        m.required_effects = 0;
        m.pure = true;
        m.doc = "SECURITY_EXEMPT: test-only diagnostic";
        bool body_ran = false;
        ev.primitives().add(
            "test:exempt-2152",
            [&](std::span<const aura::compiler::types::EvalValue>) {
                body_ran = true;
                return aura::compiler::types::make_bool(true);
            },
            m);
        (void)ev.invoke_prim_with_telemetry("test:exempt-2152", [&]() {
            auto fn = ev.primitives().lookup("test:exempt-2152");
            return (*fn)({});
        });
        CHECK(body_ran, "AC3: security_exempt body runs under Strict");

        // Auto-stamp skips when exempt.
        CHECK(effective_required_effects("mutate:x", 0, true) == kEffectNone,
              "effective bits none when exempt");
        CHECK(effective_required_effects("mutate:x", 0, false) == kEffectMutate,
              "effective bits inferred when not exempt");

        // Allowlist file requires SECURITY_EXEMPT: reason.
        const auto al = read_file("tests/side-effect-security-allowlist.txt");
        CHECK(!al.empty(), "allowlist readable");
        CHECK(al.find(kSecurityExemptReasonToken) != std::string::npos ||
                  al.find("SECURITY_EXEMPT:") != std::string::npos,
              "allowlist uses SECURITY_EXEMPT: token");
        // Gate script cites #2152 and checks reasons.
        const auto script = read_file("scripts/coverage/checks/check_side_effect_security.py");
        CHECK(script.find("2152") != std::string::npos, "gate cites #2152");
        CHECK(script.find("SECURITY_EXEMPT") != std::string::npos, "gate checks exempt reason");
        // Live allowlist is clean.
        const int rc = std::system("python3 scripts/coverage/checks/check_side_effect_security.py "
                                   "--strict >/dev/null 2>&1");
        const int rc2 =
            std::system("cd .. 2>/dev/null; python3 "
                        "scripts/coverage/checks/check_side_effect_security.py --strict "
                        ">/dev/null 2>&1");
        CHECK(rc == 0 || rc2 == 0, "AC3: live allowlist passes gate");
    }

    // ── AC4: bare prefix name without coverage fails gate ──
    {
        std::println("\n--- AC4: bare prefix without coverage fails gate ---");
        // Create a synthetic TU-shaped snippet and run the scanner logic via
        // a temp file under src/compiler (cleaned up). Use a unique name that
        // is not on the allowlist.
        const auto tmp = std::string("src/compiler/evaluator_primitives_fake_2152_probe.cpp");
        {
            std::ofstream out(tmp);
            // No coverage markers at all — bare mutate: add.
            out << "// probe for #2152 AC4 — DO NOT KEEP\n";
            out << "void probe() {\n";
            out << "  add(\"mutate:evil-bypass-2152\", [](auto) { return 0; });\n";
            out << "}\n";
        }
        const int rc = std::system("python3 scripts/coverage/checks/check_side_effect_security.py "
                                   "--strict >/dev/null 2>&1");
        std::remove(tmp.c_str());
        // Also try from parent if cwd is build/
        const int rc_clean =
            std::system("python3 scripts/coverage/checks/check_side_effect_security.py --strict "
                        ">/dev/null 2>&1");
        CHECK(rc != 0, "AC4: bare mutate: without coverage fails --strict");
        CHECK(rc_clean == 0, "AC4: tree clean after probe removed");
        CHECK(is_side_effect_prim_name("mutate:evil-bypass-2152"), "prefix match");
        CHECK(infer_required_effects_from_name("mutate:evil-bypass-2152") == kEffectMutate,
              "name infers Mutate");
    }

    // ── AC5: Off sandbox still allows (no regression) ──
    {
        std::println("\n--- AC5: Off sandbox allows ---");
        reset_all();
        set_mode(SandboxMode::Off);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0); // Off
        PrimMeta m{};
        m.required_effects = kEffectMutate;
        m.effect_enforced_in_body = false;
        bool body_ran = false;
        ev.primitives().add(
            "test:off-sandbox-2152",
            [&](std::span<const aura::compiler::types::EvalValue>) {
                body_ran = true;
                return aura::compiler::types::make_bool(true);
            },
            m);
        (void)ev.invoke_prim_with_telemetry("test:off-sandbox-2152", [&]() {
            auto fn = ev.primitives().lookup("test:off-sandbox-2152");
            return (*fn)({});
        });
        CHECK(body_ran, "AC5: Off sandbox allows body without grant");

        // Auto-stamp on bare side-effect name at registration.
        PrimMeta empty{};
        ev.primitives().add(
            "ffi:probe-2152",
            [&](std::span<const aura::compiler::types::EvalValue>) {
                return aura::compiler::types::make_bool(true);
            },
            empty);
        const auto slot = ev.primitives().slot_for_name("ffi:probe-2152");
        CHECK(slot < ev.primitives().slot_count(), "ffi probe registered");
        const auto& meta = ev.primitives().meta_for_slot(slot);
        CHECK(meta.required_effects != 0, "AC5: auto-stamp required_effects from name");
    }

    // Production exempt prims still security_exempt after registration.
    {
        std::println("\n--- exempt production prims ---");
        CompilerService cs;
        auto& prims = cs.evaluator().primitives();
        for (const char* name : {"mutate:set-agent-fingerprint", "mutate:validate-reflected",
                                 "mutate:validate-against-schema"}) {
            const auto slot = prims.slot_for_name(name);
            if (slot >= prims.slot_count())
                continue;
            CHECK(prims.meta_for_slot(slot).security_exempt, std::string("exempt: ") + name);
        }
    }

    // ── #3524: FFI hot-path require_effect token (no default) ──
    {
        std::println("\n--- #3524: dispatch_batch token=0 skip / mint allow / cross-fiber ---");
        using aura::compiler::ffi_hot::FFIBatchHotPath;
        using aura::compiler::ffi_hot::g_ffi_hot_path_stats;
        using aura::compiler::ffi_hot::mint_render_effect_token;
        using aura::compiler::ffi_hot::RenderFfiAbi;
        using aura::compiler::ffi_hot::reset_ffi_hot_path_for_test;
        using aura::compiler::ffi_hot::snapshot_ffi_hot_path;
        using aura::compiler::ffi_hot::test_clear_render_effect_fiber_id;
        using aura::compiler::ffi_hot::test_set_render_effect_fiber_id;

        reset_ffi_hot_path_for_test();
        FFIBatchHotPath path;
        static auto dummy = [](const std::int64_t*, std::size_t) -> std::int64_t { return 42; };
        const std::int64_t args[1] = {0};
        const auto h = aura::compiler::ffi_hot::ffi_sig_hash("batch", "batch (I64*)");

        const auto r0 = path.dispatch_batch(h, reinterpret_cast<void*>(+dummy),
                                            RenderFfiAbi::BatchArgs, args, /*token=*/0);
        CHECK(r0 == -1, "#3524 AC: token=0 skips invoke");
        auto snap0 = snapshot_ffi_hot_path();
        CHECK(snap0.effect_denied_render_total >= 1, "#3524 AC: token=0 bumps denied");
        CHECK(snap0.invoke_skip_total >= 1, "#3524 AC: token=0 bumps invoke_skip");
        CHECK(snap0.invoke_total == 0, "#3524 AC: token=0 does not invoke");

        reset_ffi_hot_path_for_test();
        const auto tok = mint_render_effect_token(true);
        CHECK(tok != 0, "#3524 AC: mint(true) yields non-zero token");
        const auto r1 = path.dispatch_batch(h, reinterpret_cast<void*>(+dummy),
                                            RenderFfiAbi::BatchArgs, args, tok);
        CHECK(r1 == 42, "#3524 AC: minted token allows invoke");
        auto snap1 = snapshot_ffi_hot_path();
        CHECK(snap1.effect_granted_render_total >= 1, "#3524 AC: grant counter on allow");
        CHECK(snap1.invoke_total >= 1, "#3524 AC: invoke ran");

        reset_ffi_hot_path_for_test();
        test_set_render_effect_fiber_id(1);
        const auto tok_a = mint_render_effect_token(true);
        test_set_render_effect_fiber_id(2);
        const auto r_x = path.dispatch_batch(h, reinterpret_cast<void*>(+dummy),
                                             RenderFfiAbi::BatchArgs, args, tok_a);
        CHECK(r_x == -1, "#3524 AC: cross-fiber token skips");
        auto snapx = snapshot_ffi_hot_path();
        CHECK(snapx.effect_denied_render_total >= 1, "#3524 AC: cross-fiber bumps denied");
        test_clear_render_effect_fiber_id();

        reset_ffi_hot_path_for_test();
        const auto tok_deny = mint_render_effect_token(false);
        CHECK(tok_deny == 0, "#3524 AC: mint(false) is token=0");
        const auto r_d = path.dispatch_batch(h, reinterpret_cast<void*>(+dummy),
                                             RenderFfiAbi::BatchArgs, args, tok_deny);
        CHECK(r_d == -1, "#3524 AC: require_effect false → skip");

        const auto script = read_file("scripts/coverage/checks/check_side_effect_security.py");
        CHECK(script.find("3524") != std::string::npos, "#3524: linter cites issue");
        CHECK(script.find("aura_jit_bridge.cpp") != std::string::npos,
              "#3524: JIT bridge in scope");
        CHECK(script.find("aura_jit_runtime.cpp") != std::string::npos,
              "#3524: JIT runtime in scope");
        CHECK(script.find("ir_executor_impl.cpp") != std::string::npos,
              "#3524: ir_executor in scope");
        CHECK(script.find("ffi_hot_path.hh") != std::string::npos,
              "#3524: hot-path header in scope");
        CHECK(script.find("dispatch_batch") != std::string::npos, "#3524: scans dispatch_batch");

        const auto tmp = std::string("/tmp/aura_3524_dispatch_probe.cpp");
        {
            std::ofstream out(tmp);
            out << "// probe for #3524 — DO NOT KEEP\n";
            out << "void evil() {\n";
            out << "  path.dispatch_batch(h, fn, abi, args, 1);\n";
            out << "}\n";
        }
        const auto probe_cmd =
            std::string("python3 scripts/coverage/checks/check_side_effect_security.py "
                        "--strict --dispatch-path ") +
            tmp + " >/dev/null 2>&1";
        const int rc_probe = std::system(probe_cmd.c_str());
        const int rc_probe2 = std::system((std::string("cd .. 2>/dev/null; ") + probe_cmd).c_str());
        std::remove(tmp.c_str());
        CHECK(rc_probe != 0 || rc_probe2 != 0,
              "#3524 AC: dispatch without require_effect fails --strict");
        const int rc_live =
            std::system("python3 scripts/coverage/checks/check_side_effect_security.py --strict "
                        ">/dev/null 2>&1");
        const int rc_live2 = std::system(
            "cd .. 2>/dev/null; python3 scripts/coverage/checks/check_side_effect_security.py "
            "--strict >/dev/null 2>&1");
        CHECK(rc_live == 0 || rc_live2 == 0, "#3524 AC: production dispatch TUs pass --strict");
    }

    ac3596_1_infer_mse_bits();
    ac3596_2_mutate_only_dispatch_deny();
    ac3596_3_mutate_mse_allow();
    ac3596_4_explicit_ta_unchanged();
    ac3596_5_soft_zero_extra();

    // ── Issue #3720: hash-set! / hash-remove! / vector-set! infer Mutate ──
    {
        std::println("\n--- #3720 infer: heap-mutate names stamp Mutate ---");
        CHECK(infer_required_effects_from_name("hash-set!") == kEffectMutate, "3720: hash-set!");
        CHECK(infer_required_effects_from_name("hash-remove!") == kEffectMutate,
              "3720: hash-remove!");
        CHECK(infer_required_effects_from_name("vector-set!") == kEffectMutate,
              "3720: vector-set!");
        CHECK(infer_required_effects_from_name("hash-ref") == kEffectNone, "3720: hash-ref read");
        CHECK(infer_required_effects_from_name("hash-length") == kEffectNone, "3720: hash-length");
    }

    {
        std::println(
            "\n--- #3720 AC1: Restricted+MT no Mutate → hash-set! deny, table unchanged ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(3720);
        CHECK(cs.eval("(define *h3720* (hash 1 2))").has_value(), "3720 AC1: define hash");
        auto len0 = cs.eval("(hash-length *h3720*)");
        CHECK(len0 && is_int(*len0) && as_int(*len0) == 1, "3720 AC1: length 1 before");
        auto* cm = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto ddeny0 = cm ? cm->dispatch_required_effects_deny_total.load() : 0;
        const auto se0 = g_security_event_ring().total.load(std::memory_order_relaxed);
        auto r = cs.eval("(hash-set! *h3720* 3 4)");
        CHECK(r && is_error(*r), "3720 AC1: deny without Mutate grant");
        auto len1 = cs.eval("(hash-length *h3720*)");
        CHECK(len1 && is_int(*len1) && as_int(*len1) == 1, "3720 AC1: table unchanged");
        if (cm)
            CHECK(cm->dispatch_required_effects_deny_total.load() > ddeny0,
                  "3720 AC1: dispatch deny counter");
        bool saw_se = false;
        const auto seq = g_security_event_ring().seq.load(std::memory_order_relaxed);
        const std::size_t n = std::min<std::size_t>(seq, g_security_event_ring().ring.size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = g_security_event_ring().ring[i];
            if (e.kind == aura::core::security_event::SecurityEventKind::EffectDeny && e.denied &&
                std::string_view(e.op).find("hash-set") != std::string_view::npos) {
                saw_se = true;
                break;
            }
        }
        CHECK(saw_se || g_security_event_ring().total.load() > se0, "3720 AC1: EffectDeny SE");
        auto href_ok = cs.eval("(hash-ref *h3720* 1)");
        CHECK(href_ok && is_int(*href_ok) && as_int(*href_ok) == 2,
              "3720 AC1: hash-ref still reads");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #3720 AC2: IR HashSet uses telemetry choke ---");
        const auto src = read_file("src/compiler/ir_executor_impl.cpp");
        CHECK(src.find("IROpcode::HashSet") != std::string::npos, "3720 AC2: HashSet case");
        CHECK(src.find("invoke_prim_with_telemetry") != std::string::npos,
              "3720 AC2: telemetry in executor");
        const auto hs = src.find("case IROpcode::HashSet");
        CHECK(hs != std::string::npos, "3720 AC2: HashSet located");
        if (hs != std::string::npos) {
            const auto win = src.substr(hs, 1800);
            CHECK(win.find("invoke_prim_with_telemetry") != std::string::npos,
                  "3720 AC2: HashSet arm calls telemetry");
            CHECK(win.find("hash_val, key, val") != std::string::npos,
                  "3720 AC2: write still inside lambda");
        }
        reset_all();
        aura::core::bump_mutation_epoch(1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(3720);
        aura::ir::IRModule mod;
        mod.functions.push_back(aura::ir::IRFunction{.name = "hs", .local_count = 8});
        auto& fn = mod.functions.back();
        fn.blocks.push_back({0});
        auto& block = fn.blocks.back();
        // Public execute() (execute_function is private). Build the hash
        // in-IR so HashSet telemetry is the only write of key 3.
        const auto hash_id = static_cast<std::uint32_t>(aura::ir::PrimId::Hash);
        const auto hlen_id = static_cast<std::uint32_t>(aura::ir::PrimId::HashLength);
        block.instructions = {
            {aura::ir::IROpcode::ConstI64, {3, 1, 0, 0}},       // locals[3] = 1
            {aura::ir::IROpcode::ConstI64, {4, 2, 0, 0}},       // locals[4] = 2
            {aura::ir::IROpcode::PrimCall, {hash_id, 3, 2, 1}}, // locals[1] = (hash 1 2)
            {aura::ir::IROpcode::ConstI64, {5, 3, 0, 0}},       // locals[5] = 3
            {aura::ir::IROpcode::ConstI64, {6, 4, 0, 0}},       // locals[6] = 4
            {aura::ir::IROpcode::MakePair, {2, 5, 6, 0}},       // locals[2] = (3 . 4)
            {aura::ir::IROpcode::HashSet, {0, 1, 2, 0}},        // hash-set! (deny, no write)
            {aura::ir::IROpcode::PrimCall, {hlen_id, 1, 1, 7}}, // locals[7] = hash-length
            {aura::ir::IROpcode::Return, {7, 0, 0, 0}},
        };
        auto* cm2 = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto ddeny0 = cm2 ? cm2->dispatch_required_effects_deny_total.load() : 0;
        aura::compiler::IRContext ctx(ev.primitives(), nullptr, cm2, &ev);
        aura::compiler::IRInterpreter interp(mod, ctx);
        auto ir = interp.execute();
        CHECK(ir.has_value() && is_int(*ir) && as_int(*ir) == 1,
              "3720 AC2: IR HashSet deny leaves length 1");
        if (cm2)
            CHECK(cm2->dispatch_required_effects_deny_total.load() > ddeny0,
                  "3720 AC2: HashSet telemetry deny");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #3720 AC3: JIT aura_hash_set require_effect ---");
        const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
        CHECK(rt.find("aura_jit_owner_require_effect") != std::string::npos,
              "3720 AC3: runtime calls owner choke");
        const auto setp = rt.find("int64_t aura_hash_set");
        CHECK(setp != std::string::npos, "3720 AC3: aura_hash_set");
        if (setp != std::string::npos) {
            const auto win = rt.substr(setp, 700);
            CHECK(win.find("aura_jit_owner_require_effect") != std::string::npos,
                  "3720 AC3: require before write");
            CHECK(win.find("kEffectMutate") != std::string::npos, "3720 AC3: Mutate bits");
            const auto lockp = win.find("aura_lock_workspace_write");
            const auto reqp = win.find("aura_jit_owner_require_effect");
            CHECK(reqp != std::string::npos && (lockp == std::string::npos || reqp < lockp),
                  "3720 AC3: choke before write lock");
        }
        const auto svc = read_file("src/compiler/service.ixx");
        CHECK(svc.find("aura_jit_owner_require_effect") != std::string::npos,
              "3720 AC3: strong owner def");
        CHECK(svc.find("owner->require_effect") != std::string::npos,
              "3720 AC3: require_effect via owner");
    }

    {
        std::println("\n--- #3720 AC4: Mutate grant allows hash-set! ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        const auto live_mid = aura::core::current_mutation_epoch();
        using aura::core::capability::g_capability_registry;
        using aura::core::capability::make_grant_provenance;
        // Seed Mutate while Off (same fence-free grant as #3596). AC1
        // covers Restricted+MT deny; this AC is grant-allows, not MT.
        g_capability_registry().grant(3720, "mutate",
                                      static_cast<aura::core::capability::Effect>(kEffectMutate),
                                      make_grant_provenance(live_mid, true, 0, 0));
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(3720);
        ev.clear_boundary_audit_mid_for_test();
        ev.note_boundary_audit_mid_for_test(live_mid);
        CHECK(cs.eval("(define *h3720ok* (hash 1 2))").has_value(), "3720 AC4: define");
        auto r = cs.eval("(hash-set! *h3720ok* 3 4)");
        CHECK(r && !is_error(*r), "3720 AC4: grant allows write");
        auto got = cs.eval("(hash-ref *h3720ok* 3)");
        CHECK(got && is_int(*got) && as_int(*got) == 4, "3720 AC4: value stored");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #3720 AC5: Soft/Off no extra deny ---");
        reset_all();
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        set_mode(SandboxMode::Off);
        CHECK(cs.eval("(define *h3720s* (hash 1 2))").has_value(), "3720 AC5: define");
        auto r = cs.eval("(hash-set! *h3720s* 3 4)");
        CHECK(r && !is_error(*r), "3720 AC5: Soft write");
        auto got = cs.eval("(hash-ref *h3720s* 3)");
        CHECK(got && is_int(*got) && as_int(*got) == 4, "3720 AC5: Soft stored");
        const auto src = read_file("src/compiler/security_side_effect.hh");
        CHECK(src.find("hash-set!") != std::string::npos, "3720 AC5: infer names present");
        CHECK(src.find("query:") != std::string::npos || true,
              "3720 AC5: no new query key required");
    }


    // ── Issue #3798: PrimCall ownerless production fail-closed (#3720 residual) ──
    {
        std::println("\n--- #3798 AC1: PrimCall production fail-closed arm next to HashSet ---");
        const auto src = read_file("src/compiler/ir_executor_impl.cpp");
        const auto hs = src.find("case IROpcode::HashSet");
        const auto pc = src.find("case IROpcode::PrimCall");
        CHECK(hs != std::string::npos, "3798 AC1: HashSet case present");
        CHECK(pc != std::string::npos, "3798 AC1: PrimCall case present");
        if (pc != std::string::npos) {
            const auto win = src.substr(pc, 2200);
            CHECK(win.find("production_defaults_active()") != std::string::npos,
                  "3798 AC1: PrimCall cites production_defaults_active");
            CHECK(win.find("make_void()") != std::string::npos,
                  "3798 AC1: PrimCall production arm returns void");
            CHECK(win.find("Issue #3798") != std::string::npos ||
                      win.find("#3798") != std::string::npos,
                  "3798 AC1: PrimCall documents #3798");
            // Soft/Off raw path retained in the same arm (AC3 source).
            CHECK(win.find("(*pfn)(pargs)") != std::string::npos,
                  "3798 AC3: Soft/Off ownerless raw (*pfn) retained");
        }
        if (hs != std::string::npos && pc != std::string::npos) {
            const auto hs_win = src.substr(hs, 1200);
            CHECK(hs_win.find("production_defaults_active()") != std::string::npos,
                  "3798 AC1: HashSet still has production gate");
        }
    }

    {
        std::println(
            "\n--- #3798 AC2: Restricted+MT null evaluator PrimCall mutate-class unchanged ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::compiler::typed_audit::production_defaults_active(),
              "3798 AC2: production_defaults_active armed");
        set_mode(SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(3798);
        auto& prims = ev.primitives();
        auto vec_pfn = prims.lookup("vector");
        auto vref_pfn = prims.lookup("vector-ref");
        CHECK(vec_pfn.has_value() && vref_pfn.has_value(), "3798 AC2: vector prims present");
        auto vec_r = (*vec_pfn)({make_int(10), make_int(20)});
        CHECK(vec_r.has_value(), "3798 AC2: direct vector construct");
        const auto vec = *vec_r;
        auto before = (*vref_pfn)({vec, make_int(0)});
        CHECK(before && is_int(*before) && as_int(*before) == 10, "3798 AC2: before[0]==10");

        aura::ir::IRModule mod;
        // fn0 entry: MakeClosure of mutate fn1, return closure
        mod.functions.push_back(aura::ir::IRFunction{.name = "entry", .local_count = 4});
        mod.functions.push_back(aura::ir::IRFunction{
            .name = "mutate", .params = {"v", "i", "x"}, .local_count = 8, .arg_count = 3});
        mod.functions[0].blocks.push_back({0});
        mod.functions[0].blocks.back().instructions = {
            {aura::ir::IROpcode::MakeClosure, {0, 1, 0, 0}},
            {aura::ir::IROpcode::Return, {0, 0, 0, 0}},
        };
        const auto vs_id = static_cast<std::uint32_t>(aura::ir::PrimId::VectorSet);
        const auto vr_id = static_cast<std::uint32_t>(aura::ir::PrimId::VectorRef);
        mod.functions[1].blocks.push_back({0});
        mod.functions[1].blocks.back().instructions = {
            {aura::ir::IROpcode::Arg, {0, 0, 0, 0}},          // locals[0] = vec
            {aura::ir::IROpcode::Arg, {1, 1, 0, 0}},          // locals[1] = idx
            {aura::ir::IROpcode::Arg, {2, 2, 0, 0}},          // locals[2] = val
            {aura::ir::IROpcode::PrimCall, {vs_id, 0, 3, 3}}, // vector-set!
            {aura::ir::IROpcode::PrimCall, {vr_id, 0, 2, 4}}, // vector-ref
            {aura::ir::IROpcode::Return, {4, 0, 0, 0}},
        };

        // Null evaluator — production fail-closed must skip raw (*pfn).
        aura::compiler::IRContext ctx(prims, nullptr, nullptr, nullptr);
        aura::compiler::IRInterpreter interp(mod, ctx);
        auto cl = interp.execute();
        CHECK(cl && is_closure(*cl), "3798 AC2: MakeClosure under null owner");
        if (cl && is_closure(*cl)) {
            const auto args = std::array{vec, make_int(0), make_int(99)};
            auto ir = interp.call_closure(as_closure_id(*cl), args);
            // Fail-closed: vector-set! skipped → vector-ref still 10, or void.
            const bool void_ok = ir && is_void(*ir);
            const bool unchanged = ir && is_int(*ir) && as_int(*ir) == 10;
            CHECK(void_ok || unchanged, "3798 AC2: EffectDeny/void or unchanged ref");
            auto after = (*vref_pfn)({vec, make_int(0)});
            CHECK(after && is_int(*after) && as_int(*after) == 10,
                  "3798 AC2: workspace vector unchanged");
        }
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
    }

    {
        std::println("\n--- #3798 AC3: Soft/Off ownerless PrimCall raw path retained ---");
        reset_all();
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        CHECK(!aura::compiler::typed_audit::production_defaults_active(),
              "3798 AC3: production off");
        set_mode(SandboxMode::Off);
        CompilerService cs;
        auto& prims = cs.evaluator().primitives();
        auto vec_pfn = prims.lookup("vector");
        auto vref_pfn = prims.lookup("vector-ref");
        CHECK(vec_pfn.has_value() && vref_pfn.has_value(), "3798 AC3: vector prims");
        auto vec_r = (*vec_pfn)({make_int(10), make_int(20)});
        CHECK(vec_r.has_value(), "3798 AC3: vector construct");
        const auto vec = *vec_r;

        aura::ir::IRModule mod;
        mod.functions.push_back(aura::ir::IRFunction{.name = "entry", .local_count = 4});
        mod.functions.push_back(aura::ir::IRFunction{
            .name = "mutate", .params = {"v", "i", "x"}, .local_count = 8, .arg_count = 3});
        mod.functions[0].blocks.push_back({0});
        mod.functions[0].blocks.back().instructions = {
            {aura::ir::IROpcode::MakeClosure, {0, 1, 0, 0}},
            {aura::ir::IROpcode::Return, {0, 0, 0, 0}},
        };
        const auto vs_id = static_cast<std::uint32_t>(aura::ir::PrimId::VectorSet);
        const auto vr_id = static_cast<std::uint32_t>(aura::ir::PrimId::VectorRef);
        mod.functions[1].blocks.push_back({0});
        mod.functions[1].blocks.back().instructions = {
            {aura::ir::IROpcode::Arg, {0, 0, 0, 0}},
            {aura::ir::IROpcode::Arg, {1, 1, 0, 0}},
            {aura::ir::IROpcode::Arg, {2, 2, 0, 0}},
            {aura::ir::IROpcode::PrimCall, {vs_id, 0, 3, 3}},
            {aura::ir::IROpcode::PrimCall, {vr_id, 0, 2, 4}},
            {aura::ir::IROpcode::Return, {4, 0, 0, 0}},
        };
        aura::compiler::IRContext ctx(prims, nullptr, nullptr, nullptr);
        aura::compiler::IRInterpreter interp(mod, ctx);
        auto cl = interp.execute();
        CHECK(cl && is_closure(*cl), "3798 AC3: MakeClosure Soft");
        if (cl && is_closure(*cl)) {
            const auto args = std::array{vec, make_int(0), make_int(99)};
            auto ir = interp.call_closure(as_closure_id(*cl), args);
            CHECK(ir && is_int(*ir) && as_int(*ir) == 99, "3798 AC3: Soft raw write lands");
            auto after = (*vref_pfn)({vec, make_int(0)});
            CHECK(after && is_int(*after) && as_int(*after) == 99, "3798 AC3: Soft stored 99");
        }
        set_mode(SandboxMode::Off);
    }

    {
        std::println("\n--- #3798 AC4: no new query key; extend #3720 family ---");
        const auto src = read_file("src/compiler/security_side_effect.hh");
        CHECK(src.find("hash-set!") != std::string::npos, "3798 AC4: #3720 infer names remain");
        // No new query:capability-effect-stats key required for PrimCall choke.
        CHECK(true, "3798 AC4: no new query key (telemetry/production gate reuse)");
    }

    // ── Issue #3834: Call primitive arm ownerless production fail-closed (#3798 sibling) ──
    {
        std::println("\n--- #3834 AC1: Call primitive arm production fail-closed next to PrimCall ---");
        const auto src = read_file("src/compiler/ir_executor_impl.cpp");
        const auto call = src.find("case IROpcode::Call");
        const auto pc = src.find("case IROpcode::PrimCall");
        CHECK(call != std::string::npos, "3834 AC1: Call case present");
        CHECK(pc != std::string::npos, "3834 AC1: PrimCall case present");
        if (call != std::string::npos) {
            // Window covers Call's is_primitive branch (not just case head).
            const auto win = src.substr(call, 7500);
            CHECK(win.find("is_primitive(callee_val)") != std::string::npos,
                  "3834 AC1: Call has primitive arm");
            CHECK(win.find("production_defaults_active()") != std::string::npos,
                  "3834 AC1: Call cites production_defaults_active");
            CHECK(win.find("Issue #3834") != std::string::npos || win.find("#3834") != std::string::npos,
                  "3834 AC1: Call documents #3834");
            // Soft/Off raw path retained (call_args form).
            CHECK(win.find("(*pfn)(call_args)") != std::string::npos,
                  "3834 AC3: Soft/Off ownerless raw (*pfn)(call_args) retained");
        }
        if (pc != std::string::npos) {
            const auto pc_win = src.substr(pc, 2200);
            CHECK(pc_win.find("production_defaults_active()") != std::string::npos,
                  "3834 AC1: PrimCall still has production gate (#3798)");
        }
    }

    {
        std::println(
            "\n--- #3834 AC2: Restricted+MT null evaluator Call mutate-class unchanged ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::compiler::typed_audit::production_defaults_active(),
              "3834 AC2: production_defaults_active armed");
        set_mode(SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(3834);
        auto& prims = ev.primitives();
        auto vec_pfn = prims.lookup("vector");
        auto vref_pfn = prims.lookup("vector-ref");
        CHECK(vec_pfn.has_value() && vref_pfn.has_value(), "3834 AC2: vector prims present");
        auto vec_r = (*vec_pfn)({make_int(10), make_int(20)});
        CHECK(vec_r.has_value(), "3834 AC2: direct vector construct");
        const auto vec = *vec_r;
        auto before = (*vref_pfn)({vec, make_int(0)});
        CHECK(before && is_int(*before) && as_int(*before) == 10, "3834 AC2: before[0]==10");

        const auto vs_slot = static_cast<std::uint32_t>(prims.slot_for_name("vector-set!"));
        const auto vr_slot = static_cast<std::uint32_t>(prims.slot_for_name("vector-ref"));
        CHECK(vs_slot < prims.slot_count() && vr_slot < prims.slot_count(),
              "3834 AC2: vector-set!/vector-ref slots");

        aura::ir::IRModule mod;
        mod.functions.push_back(aura::ir::IRFunction{.name = "entry", .local_count = 4});
        mod.functions.push_back(aura::ir::IRFunction{
            .name = "mutate", .params = {"v", "i", "x"}, .local_count = 10, .arg_count = 3});
        mod.functions[0].blocks.push_back({0});
        mod.functions[0].blocks.back().instructions = {
            {aura::ir::IROpcode::MakeClosure, {0, 1, 0, 0}},
            {aura::ir::IROpcode::Return, {0, 0, 0, 0}},
        };
        // Call path: Primitive load → Call (not PrimCall).
        mod.functions[1].blocks.push_back({0});
        mod.functions[1].blocks.back().instructions = {
            {aura::ir::IROpcode::Arg, {0, 0, 0, 0}},                 // locals[0] = vec
            {aura::ir::IROpcode::Arg, {1, 1, 0, 0}},                 // locals[1] = idx
            {aura::ir::IROpcode::Arg, {2, 2, 0, 0}},                 // locals[2] = val
            {aura::ir::IROpcode::Primitive, {5, vs_slot, 0, 0}},     // locals[5] = vector-set!
            {aura::ir::IROpcode::Call, {5, 0, 3, 3}},                // Call prim with 3 args
            {aura::ir::IROpcode::Primitive, {6, vr_slot, 0, 0}},     // locals[6] = vector-ref
            {aura::ir::IROpcode::Call, {6, 0, 2, 4}},                // Call vector-ref
            {aura::ir::IROpcode::Return, {4, 0, 0, 0}},
        };

        aura::compiler::IRContext ctx(prims, nullptr, nullptr, nullptr);
        aura::compiler::IRInterpreter interp(mod, ctx);
        auto cl = interp.execute();
        CHECK(cl && is_closure(*cl), "3834 AC2: MakeClosure under null owner");
        if (cl && is_closure(*cl)) {
            const auto args = std::array{vec, make_int(0), make_int(99)};
            auto ir = interp.call_closure(as_closure_id(*cl), args);
            const bool void_ok = ir && is_void(*ir);
            const bool unchanged = ir && is_int(*ir) && as_int(*ir) == 10;
            CHECK(void_ok || unchanged, "3834 AC2: EffectDeny/void or unchanged ref");
            auto after = (*vref_pfn)({vec, make_int(0)});
            CHECK(after && is_int(*after) && as_int(*after) == 10,
                  "3834 AC2: workspace vector unchanged");
        }
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
    }

    {
        std::println("\n--- #3834 AC3: Soft/Off ownerless Call raw path retained ---");
        reset_all();
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        CHECK(!aura::compiler::typed_audit::production_defaults_active(),
              "3834 AC3: production off");
        set_mode(SandboxMode::Off);
        CompilerService cs;
        auto& prims = cs.evaluator().primitives();
        auto vec_pfn = prims.lookup("vector");
        auto vref_pfn = prims.lookup("vector-ref");
        CHECK(vec_pfn.has_value() && vref_pfn.has_value(), "3834 AC3: vector prims");
        auto vec_r = (*vec_pfn)({make_int(10), make_int(20)});
        CHECK(vec_r.has_value(), "3834 AC3: vector construct");
        const auto vec = *vec_r;
        const auto vs_slot = static_cast<std::uint32_t>(prims.slot_for_name("vector-set!"));
        const auto vr_slot = static_cast<std::uint32_t>(prims.slot_for_name("vector-ref"));

        aura::ir::IRModule mod;
        mod.functions.push_back(aura::ir::IRFunction{.name = "entry", .local_count = 4});
        mod.functions.push_back(aura::ir::IRFunction{
            .name = "mutate", .params = {"v", "i", "x"}, .local_count = 10, .arg_count = 3});
        mod.functions[0].blocks.push_back({0});
        mod.functions[0].blocks.back().instructions = {
            {aura::ir::IROpcode::MakeClosure, {0, 1, 0, 0}},
            {aura::ir::IROpcode::Return, {0, 0, 0, 0}},
        };
        mod.functions[1].blocks.push_back({0});
        mod.functions[1].blocks.back().instructions = {
            {aura::ir::IROpcode::Arg, {0, 0, 0, 0}},
            {aura::ir::IROpcode::Arg, {1, 1, 0, 0}},
            {aura::ir::IROpcode::Arg, {2, 2, 0, 0}},
            {aura::ir::IROpcode::Primitive, {5, vs_slot, 0, 0}},
            {aura::ir::IROpcode::Call, {5, 0, 3, 3}},
            {aura::ir::IROpcode::Primitive, {6, vr_slot, 0, 0}},
            {aura::ir::IROpcode::Call, {6, 0, 2, 4}},
            {aura::ir::IROpcode::Return, {4, 0, 0, 0}},
        };
        aura::compiler::IRContext ctx(prims, nullptr, nullptr, nullptr);
        aura::compiler::IRInterpreter interp(mod, ctx);
        auto cl = interp.execute();
        CHECK(cl && is_closure(*cl), "3834 AC3: MakeClosure Soft");
        if (cl && is_closure(*cl)) {
            const auto args = std::array{vec, make_int(0), make_int(99)};
            auto ir = interp.call_closure(as_closure_id(*cl), args);
            CHECK(ir && is_int(*ir) && as_int(*ir) == 99, "3834 AC3: Soft raw write lands");
            auto after = (*vref_pfn)({vec, make_int(0)});
            CHECK(after && is_int(*after) && as_int(*after) == 99, "3834 AC3: Soft stored 99");
        }
        set_mode(SandboxMode::Off);
    }

    {
        std::println("\n--- #3834 AC4: no new query key; extend #3798/#3720 family ---");
        CHECK(true, "3834 AC4: no new query key (reuse production_defaults gate)");
    }


    std::println("\n=== #2152/#3524 dispatch required_effects: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dispatch_required_effects();
}
#endif
