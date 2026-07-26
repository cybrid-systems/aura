// @category: unit
// @reason: Issue #2152 — Dispatch-level non-bypassable required_effects
// for side-effect prims (construction-time auto-stamp + dispatch gate).
//
//   AC1: Prim with required_effects=Mutate, no body check → deny under
//        Restricted without grant; audit ring records deny
//   AC2: effect_enforced_in_body=true (add_mutate path) does not
//        double-call require_effect at dispatch
//   AC3: security_exempt=true + documented reason passes gate;
//        undocumented allowlist entry fails CI script
//   AC4: New prefix-matching name without coverage fails
//        check_side_effect_security.py
//   AC5: Off sandbox / legacy tests still green (AURA_SANDBOX=off)

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "compiler/security_capabilities.h"
#include "compiler/security_side_effect.hh"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <cstdlib>
#include <cstdint>
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
using aura::compiler::effective_required_effects;
using aura::compiler::infer_required_effects_from_name;
using aura::compiler::is_side_effect_prim_name;
using aura::compiler::kDispatchRequiredEffectsIssue;
using aura::compiler::kSecurityExemptReasonToken;
using aura::compiler::kSideEffectInheritIssue;
using aura::compiler::PrimMeta;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectNone;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
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

int main() {
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
        }
        // Query surface
        CHECK(href(cs, "schema-2152") == 2152, "schema-2152");
        CHECK(href(cs, "dispatch-required-effects-wired") == 1, "wired marker");
        CHECK(href(cs, "dispatch-required-effects-deny") >= 1, "query deny count");
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
        const auto script = read_file("scripts/check_side_effect_security.py");
        CHECK(script.find("2152") != std::string::npos, "gate cites #2152");
        CHECK(script.find("SECURITY_EXEMPT") != std::string::npos, "gate checks exempt reason");
        // Live allowlist is clean.
        const int rc =
            std::system("python3 scripts/check_side_effect_security.py --strict >/dev/null 2>&1");
        const int rc2 =
            std::system("cd .. 2>/dev/null; python3 scripts/check_side_effect_security.py --strict "
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
        const int rc =
            std::system("python3 scripts/check_side_effect_security.py --strict >/dev/null 2>&1");
        std::remove(tmp.c_str());
        // Also try from parent if cwd is build/
        const int rc_clean =
            std::system("python3 scripts/check_side_effect_security.py --strict >/dev/null 2>&1");
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

    std::println("\n=== #2152 dispatch required_effects: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}
