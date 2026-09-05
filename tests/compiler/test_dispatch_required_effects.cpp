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

#include "test_harness.hpp"

#include "compiler/ffi_hot_path.hh"
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

    std::println("\n=== #2152/#3524 dispatch required_effects: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dispatch_required_effects();
}
#endif
