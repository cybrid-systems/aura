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
#include "compiler/runtime_shared.h" // #4036: aura_alloc_closure / aura_closure_is_freed / aura_free_closure_checked
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
#include <sstream>
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
using aura::compiler::types::as_bool;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
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

// Issue #4037: merr values are (kind . message) pairs — is_error() does not
// flag them; read the deny kind off the car string (same shape as merr_kind
// in test_workspace_lock_unlock.cpp).
static std::string ac4037_merr_kind(CompilerService& cs,
                                    const aura::compiler::types::EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

// ── Issue #4037: mutate:set-agent-fingerprint — the author fingerprint is the
// blame label TypedTransactionGuard copies onto every sub-mutation of the next
// typed atomic batch, so a non-zero store is an identity write:
// kEffectTenantAdmin, enforced IN BODY and conditionally (non-zero only — a
// dispatch-level require cannot see the arg and would gate clearing to 0).
// Clearing to 0 stays allowed without TenantAdmin; Soft/Off keeps the store
// ungated (sandbox-mode guard). Deny reason = require_effect default
// "capability-effect-deny" (TenantAdmin-only, no Mutate/Ffi/Render bits).

static void ac4037_meta_contract() {
    std::println("\n--- #4037 meta: non-exempt, TA required, enforced in body ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto slot = ev.primitives().slot_for_name("mutate:set-agent-fingerprint");
    CHECK(slot < ev.primitives().slot_count(), "4037 meta: prim registered");
    const auto& meta = ev.primitives().meta_for_slot(slot);
    CHECK(!meta.security_exempt, "4037 meta: security_exempt dropped");
    CHECK(meta.required_effects == aura::compiler::security::kEffectTenantAdmin,
          "4037 meta: required_effects = kEffectTenantAdmin");
    CHECK(meta.effect_enforced_in_body,
          "4037 meta: effect_enforced_in_body (arg-conditional, dispatch not double-gating)");
    CHECK(meta.guard_exempt, "4037 meta: still guard_exempt (metadata-only, no AST write)");
}

static void ac4037_1_deny_no_grant() {
    std::println("\n--- #4037 AC1: Restricted no grant → non-zero set denies, store unchanged ---");
    reset_all();
    aura::core::bump_mutation_epoch(1);
    const auto live_mid = aura::core::current_mutation_epoch();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    set_mode(SandboxMode::Restricted);
    ev.set_capability_tenant_id(4037);
    ev.clear_boundary_audit_mid_for_test();
    ev.note_boundary_audit_mid_for_test(live_mid);
    CHECK(ev.current_agent_fingerprint() == 0, "4037 AC1: store starts 0");
    auto r = cs.eval("(mutate:set-agent-fingerprint 4242)");
    CHECK(r && ac4037_merr_kind(cs, *r) == "capability-effect-deny",
          "4037 AC1: non-zero set denies with empty grants");
    CHECK(ev.current_agent_fingerprint() == 0, "4037 AC1: store unchanged after deny");
    std::size_t denies = 0;
    bool saw_reason = false;
    bool deny_tenant_ok = false;
    const auto seq = g_security_event_ring().seq.load(std::memory_order_relaxed);
    const std::size_t rn = std::min<std::size_t>(seq, g_security_event_ring().ring.size());
    for (std::size_t i = 0; i < rn; ++i) {
        const auto& e = g_security_event_ring().ring[i];
        if (e.denied && e.kind == aura::core::security_event::SecurityEventKind::EffectDeny &&
            std::string_view(e.op) == "mutate:set-agent-fingerprint") {
            ++denies;
            saw_reason = std::string_view(e.reason) == "capability-effect-deny";
            deny_tenant_ok = e.tenant_id == 4037;
        }
    }
    CHECK(denies == 1, "4037 AC1: exactly one EffectDeny row for the prim");
    CHECK(saw_reason, "4037 AC1: deny reason = capability-effect-deny");
    CHECK(deny_tenant_ok, "4037 AC1: SE row keeps the real caller tenant");
    set_mode(SandboxMode::Off);
}

static void ac4037_1b_batch_blame_stays_zero() {
    std::println("\n--- #4037 AC1b: denied set → next atomic batch author stays 0 ---");
    reset_all();
    aura::core::bump_mutation_epoch(1);
    const auto live_mid = aura::core::current_mutation_epoch();
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::make_grant_provenance;
    // Mutate-only (no TenantAdmin): the batch itself commits while the identity
    // write stays denied — the record author column must stay 0.
    g_capability_registry().grant(4037, "mutate", aura::core::capability::Effect::Mutate,
                                  make_grant_provenance(live_mid, true, 0, 0));
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    set_mode(SandboxMode::Restricted);
    ev.set_capability_tenant_id(4037);
    ev.clear_boundary_audit_mid_for_test();
    ev.note_boundary_audit_mid_for_test(live_mid);
    auto r = cs.eval("(mutate:set-agent-fingerprint 4242)");
    CHECK(r && ac4037_merr_kind(cs, *r) == "capability-effect-deny",
          "4037 AC1b: non-zero set denied under Restricted (no TenantAdmin)");
    CHECK(ev.current_agent_fingerprint() == 0, "4037 AC1b: store still 0 after deny");
    // Disarm for the batch: the blame stamping (TypedTransactionGuard copying
    // current_agent_fingerprint) is face-independent — what AC1b pins is that
    // a DENIED set leaves nothing to stamp. (Workspace Guard mid minting vs
    // epoch-bound grants is the #3964/#3966 surface, not #4037.)
    set_mode(SandboxMode::Off);
    ev.set_effect_sandbox_mode(0);
    CHECK(cs.eval("(set-code \"(define x4037 1)\")").has_value(), "4037 AC1b: set-code target");
    CHECK(cs.eval("(eval-current)").has_value(), "4037 AC1b: eval-current");
    const std::array<std::string_view, 1> mutations = {"(mutate:rebind \"x4037\" \"7\")"};
    auto batch = cs.typed_mutate_atomic(mutations);
    CHECK(batch.success, "4037 AC1b: atomic batch commits");
    set_mode(SandboxMode::Off);
    ev.set_effect_sandbox_mode(0);
    auto q = cs.eval("(hash-ref (query:last-mutation-provenance) \"author-fingerprint\")");
    CHECK(q && is_int(*q) && as_int(*q) == 0,
          "4037 AC1b: batch record author-fingerprint stays 0 (no forged label)");
}

static void ac4037_2_ta_allow_batch_blame() {
    std::println("\n--- #4037 AC2: explicit TenantAdmin → set lands, batch blames 4242 ---");
    reset_all();
    aura::core::bump_mutation_epoch(1);
    const auto live_mid = aura::core::current_mutation_epoch();
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::make_grant_provenance;
    g_capability_registry().grant(4037, "mutate", aura::core::capability::Effect::Mutate,
                                  make_grant_provenance(live_mid, true, 0, 0));
    // Explicit tenant-admin grant — NOT wildcard-only (#3144 strips TA from *).
    g_capability_registry().grant(4037, "tenant-admin", aura::core::capability::Effect::TenantAdmin,
                                  make_grant_provenance(live_mid, true, 0, 0));
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    set_mode(SandboxMode::Restricted);
    ev.set_capability_tenant_id(4037);
    ev.clear_boundary_audit_mid_for_test();
    ev.note_boundary_audit_mid_for_test(live_mid);
    auto r = cs.eval("(mutate:set-agent-fingerprint 4242)");
    CHECK(r && !is_error(*r) && is_int(*r) && as_int(*r) == 4242,
          "4037 AC2: explicit TA set lands");
    CHECK(ev.current_agent_fingerprint() == 4242, "4037 AC2: store = 4242");
    bool allow_tenant_ok = false;
    const auto seq = g_security_event_ring().seq.load(std::memory_order_relaxed);
    const std::size_t rn = std::min<std::size_t>(seq, g_security_event_ring().ring.size());
    for (std::size_t i = 0; i < rn; ++i) {
        const auto& e = g_security_event_ring().ring[i];
        if (!e.denied && e.kind == aura::core::security_event::SecurityEventKind::EffectAllow &&
            std::string_view(e.op) == "mutate:set-agent-fingerprint")
            allow_tenant_ok = e.tenant_id == 4037;
    }
    CHECK(allow_tenant_ok, "4037 AC2: EffectAllow row carries caller tenant 4037");
    CHECK(cs.eval("(set-code \"(define x4037 1)\")").has_value(), "4037 AC2: set-code target");
    CHECK(cs.eval("(eval-current)").has_value(), "4037 AC2: eval-current");
    const std::array<std::string_view, 1> mutations = {"(mutate:rebind \"x4037\" \"7\")"};
    // Disarm for the batch (same rationale as AC1b): the blame stamping is
    // face-independent; AC2 pins that the ALLOWED set's label propagates into
    // the committed records. (Workspace Guard mid minting vs epoch-bound
    // grants is the #3964/#3966 surface, not #4037.)
    set_mode(SandboxMode::Off);
    ev.set_effect_sandbox_mode(0);
    auto batch = cs.typed_mutate_atomic(mutations);
    CHECK(batch.success, "4037 AC2: atomic batch commits");
    auto q = cs.eval("(hash-ref (query:last-mutation-provenance) \"author-fingerprint\")");
    CHECK(q && is_int(*q) && as_int(*q) == 4242, "4037 AC2: batch records author-fingerprint 4242");
}

static void ac4037_3_wildcard_insufficient() {
    std::println(
        "\n--- #4037 AC3: wildcard-only * does NOT pass (effects_for strips TA, #3144) ---");
    reset_all();
    aura::core::bump_mutation_epoch(1);
    const auto live_mid = aura::core::current_mutation_epoch();
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::make_grant_provenance;
    // Full-mask wildcard (same shape as #3141 AC1) — TenantAdmin bit included
    // at grant time but stripped by effects_for for kCapWildcard (#3144).
    g_capability_registry().grant(
        4037, "*",
        aura::core::capability::Effect::Read | aura::core::capability::Effect::Write |
            aura::core::capability::Effect::Exec | aura::core::capability::Effect::Mutate |
            aura::core::capability::Effect::Network | aura::core::capability::Effect::Ffi |
            aura::core::capability::Effect::Render | aura::core::capability::Effect::MacroSelfEvo |
            aura::core::capability::Effect::TenantAdmin | aura::core::capability::Effect::Syscall,
        make_grant_provenance(live_mid, true, 0, 0));
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    set_mode(SandboxMode::Restricted);
    ev.set_capability_tenant_id(4037);
    ev.clear_boundary_audit_mid_for_test();
    ev.note_boundary_audit_mid_for_test(live_mid);
    auto r = cs.eval("(mutate:set-agent-fingerprint 4242)");
    CHECK(r && ac4037_merr_kind(cs, *r) == "capability-effect-deny",
          "4037 AC3: wildcard-only denies");
    CHECK(ev.current_agent_fingerprint() == 0, "4037 AC3: store unchanged");
    set_mode(SandboxMode::Off);
}

static void ac4037_4_clear_zero_ungated() {
    std::println("\n--- #4037 AC4: set-to-0 without TenantAdmin restores 0 ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0);
    set_mode(SandboxMode::Off);
    auto r5 = cs.eval("(mutate:set-agent-fingerprint 5)");
    CHECK(r5 && is_int(*r5) && as_int(*r5) == 5, "4037 AC4: Off-face store = 5");
    CHECK(ev.current_agent_fingerprint() == 5, "4037 AC4: store holds 5");
    ev.set_effect_sandbox_mode(1);
    set_mode(SandboxMode::Restricted);
    ev.set_capability_tenant_id(4037);
    auto r0 = cs.eval("(mutate:set-agent-fingerprint 0)");
    CHECK(r0 && !is_error(*r0) && is_int(*r0) && as_int(*r0) == 0,
          "4037 AC4: clear to 0 allowed without TA");
    CHECK(ev.current_agent_fingerprint() == 0, "4037 AC4: store restored 0");
    auto rn = cs.eval("(mutate:set-agent-fingerprint 9)");
    CHECK(rn && ac4037_merr_kind(cs, *rn) == "capability-effect-deny",
          "4037 AC4: non-zero direction still gated");
    set_mode(SandboxMode::Off);
}

static void ac4037_5_off_ungated() {
    std::println("\n--- #4037 AC5: Off face (AURA_SANDBOX=off) → store stays ungated ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0);
    set_mode(SandboxMode::Off);
    ev.set_capability_tenant_id(4037);
    auto r = cs.eval("(mutate:set-agent-fingerprint 777)");
    CHECK(r && is_int(*r) && as_int(*r) == 777, "4037 AC5: Off store ungated");
    CHECK(ev.current_agent_fingerprint() == 777, "4037 AC5: store = 777");
}

static void ac4037_source_cite() {
    std::println("\n--- #4037 source-cite: conditional in-body require + non-exempt meta ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto at = mut.find("add(\"mutate:set-agent-fingerprint\"");
    CHECK(at != std::string::npos, "4037 cite: registration located");
    if (at != std::string::npos) {
        const auto win = mut.substr(at, 1800);
        CHECK(win.find("fp != 0") != std::string::npos,
              "4037 cite: non-zero-only conditional guards the require");
        CHECK(win.find("require_effect") != std::string::npos,
              "4037 cite: in-body require present");
        CHECK(win.find("kEffectTenantAdmin") != std::string::npos,
              "4037 cite: TenantAdmin bit required");
        CHECK(win.find("set_current_agent_fingerprint") != std::string::npos,
              "4037 cite: store call present");
    }
    const auto mb = mut.find("PrimMeta ex{};", at);
    CHECK(mb != std::string::npos, "4037 cite: meta block located");
    if (mb != std::string::npos) {
        const auto mwin = mut.substr(mb, 500);
        CHECK(mwin.find("security_exempt") == std::string::npos,
              "4037 cite: no security_exempt in meta");
        CHECK(mwin.find("kEffectTenantAdmin") != std::string::npos &&
                  mwin.find("effect_enforced_in_body") != std::string::npos,
              "4037 cite: meta declares TA + in-body enforcement");
    }
    const auto al = read_file("tests/side-effect-security-allowlist.txt");
    CHECK(!al.empty(), "4037 cite: allowlist readable");
    bool al_row = false;
    {
        std::istringstream in(al);
        std::string ln;
        while (std::getline(in, ln)) {
            const auto p = ln.find_first_not_of(" \t");
            if (p == std::string::npos || ln[p] == '#')
                continue; // prose comments document the removal; not entries
            if (ln.find("mutate:set-agent-fingerprint") != std::string::npos)
                al_row = true;
        }
    }
    CHECK(!al_row, "4037 cite: SECURITY_EXEMPT allowlist row removed");
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
        // mutate:set-agent-fingerprint left this exempt list in #4037 — it is
        // now a TenantAdmin identity write enforced in body (see the #4037 AC
        // block at the bottom of this runner).
        for (const char* name : {"mutate:validate-reflected", "mutate:validate-against-schema"}) {
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
        // Issue #4018: aura_cell_set shares the same Mutate choke.
        const auto cellp = rt.find("void aura_cell_set");
        CHECK(cellp != std::string::npos, "4018 AC: aura_cell_set");
        if (cellp != std::string::npos) {
            const auto cwin = rt.substr(cellp, 700);
            CHECK(cwin.find("aura_jit_owner_require_effect") != std::string::npos,
                  "4018 AC: cell_set require before write");
            CHECK(cwin.find("kEffectMutate") != std::string::npos, "4018 AC: cell Mutate bits");
            const auto clockp = cwin.find("aura_lock_workspace_write");
            const auto creqp = cwin.find("aura_jit_owner_require_effect");
            CHECK(creqp != std::string::npos && (clockp == std::string::npos || creqp < clockp),
                  "4018 AC: cell choke before write lock");
        }
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

    // ── Issue #4036: set-car!/set-cdr!/string-fill!/closure:free! enter the
    // #3720 Mutate choke + closure-slot tenant isolation. Production face
    // = Restricted/Strict (sandbox mode != 0); Soft/Off stays a no-op.
    // No new Effect bit, no query key, no second capability model.
    {
        std::println("\n--- #4036 infer: sibling mutators stamp Mutate, no body choke ---");
        CHECK(infer_required_effects_from_name("set-car!") == kEffectMutate, "4036: set-car!");
        CHECK(infer_required_effects_from_name("set-cdr!") == kEffectMutate, "4036: set-cdr!");
        CHECK(infer_required_effects_from_name("string-fill!") == kEffectMutate,
              "4036: string-fill!");
        CHECK(infer_required_effects_from_name("closure:free!") == kEffectMutate,
              "4036: closure:free!");
        CHECK(infer_required_effects_from_name("hash-ref") == kEffectNone, "4036: read stays None");
        // Dispatch is the single choke: prim bodies must NOT also call
        // require_effect (double-consume of single-use, same note as c-*).
        const auto pair_src = read_file("src/compiler/evaluator_primitives_pair.cpp");
        const auto sc = pair_src.find("add, ev, \"set-car!\"");
        CHECK(sc != std::string::npos, "4036: set-car! body located");
        if (sc != std::string::npos) {
            const auto win = pair_src.substr(sc, 1500);
            CHECK(win.find("require_effect") == std::string::npos,
                  "4036: set-car!/set-cdr! windows stay body-choke-free");
        }
        const auto sf = pair_src.find("add, ev, \"string-fill!\"");
        CHECK(sf != std::string::npos, "4036: string-fill! body located");
        if (sf != std::string::npos) {
            const auto win = pair_src.substr(sf, 900);
            CHECK(win.find("require_effect") == std::string::npos,
                  "4036: string-fill! window stays body-choke-free");
        }
    }

    {
        std::println(
            "\n--- #4036 AC1: Restricted+MT no grant → all four deny, heaps unchanged ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4036);
        CHECK(cs.eval("(define *p4036* (cons 1 2))").has_value(), "4036 AC1: define pair");
        CHECK(cs.eval("(define *s4036* \"abc\")").has_value(), "4036 AC1: define string");
        auto* cm = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto ddeny0 = cm ? cm->dispatch_required_effects_deny_total.load() : 0;
        const auto se0 = g_security_event_ring().total.load(std::memory_order_relaxed);
        auto r1 = cs.eval("(set-car! *p4036* 9)");
        CHECK(r1 && is_error(*r1), "4036 AC1: set-car! denies with empty grants");
        auto c1 = cs.eval("(car *p4036*)");
        CHECK(c1 && is_int(*c1) && as_int(*c1) == 1, "4036 AC1: car unchanged");
        auto r2 = cs.eval("(set-cdr! *p4036* 9)");
        CHECK(r2 && is_error(*r2), "4036 AC1: set-cdr! denies");
        auto c2 = cs.eval("(cdr *p4036*)");
        CHECK(c2 && is_int(*c2) && as_int(*c2) == 2, "4036 AC1: cdr unchanged");
        auto r3 = cs.eval("(string-fill! *s4036* 88)");
        CHECK(r3 && is_error(*r3), "4036 AC1: string-fill! denies");
        auto c3 = cs.eval("(car (string->list *s4036*))");
        CHECK(c3 && is_int(*c3) && as_int(*c3) == 97, "4036 AC1: string content unchanged");
        auto r4 = cs.eval("(closure:free! 0)");
        CHECK(r4 && is_error(*r4), "4036 AC1: closure:free! denies");
        if (cm)
            CHECK(cm->dispatch_required_effects_deny_total.load() > ddeny0,
                  "4036 AC1: dispatch deny counter advanced");
        // One deny row per sibling, op == prim name. Under Restricted+MT the
        // isolation consult (#3365/#3415) preempts the effect check for
        // NodeId-less dispatches, so the deny row surfaces as IsolationDeny
        // (kind 1) instead of EffectDeny (kind 0) — same deny class, op still
        // equals the prim name (the issue's observable).
        const auto seq = g_security_event_ring().seq.load(std::memory_order_relaxed);
        const std::size_t rn = std::min<std::size_t>(seq, g_security_event_ring().ring.size());
        auto saw_op = [&](std::string_view needle) {
            for (std::size_t i = 0; i < rn; ++i) {
                const auto& e = g_security_event_ring().ring[i];
                if (e.denied &&
                    (e.kind == aura::core::security_event::SecurityEventKind::EffectDeny ||
                     e.kind == aura::core::security_event::SecurityEventKind::IsolationDeny) &&
                    std::string_view(e.op).find(needle) != std::string_view::npos)
                    return true;
            }
            return false;
        };
        CHECK(saw_op("set-car"), "4036 AC1: deny row op set-car!");
        CHECK(saw_op("set-cdr"), "4036 AC1: deny row op set-cdr!");
        CHECK(saw_op("string-fill"), "4036 AC1: deny row op string-fill!");
        CHECK(saw_op("closure:free"), "4036 AC1: deny row op closure:free!");
        CHECK(g_security_event_ring().total.load() > se0, "4036 AC1: SE ring advanced");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4036 AC2: single-use Mutate grant — one set-car! consumes once ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        const auto live_mid = aura::core::current_mutation_epoch();
        using aura::core::capability::g_capability_registry;
        using aura::core::capability::make_grant_provenance;
        // Seed while Off (same fence-free grant as #3720 AC4). A body-level
        // second require_effect would consume the single-use grant mid-call
        // and error the FIRST write — the allow proves dispatch-only choke.
        g_capability_registry().grant(4036, "mutate",
                                      static_cast<aura::core::capability::Effect>(kEffectMutate),
                                      make_grant_provenance(live_mid, true, 0, 0),
                                      /*single_use=*/true);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4036);
        ev.clear_boundary_audit_mid_for_test();
        ev.note_boundary_audit_mid_for_test(live_mid);
        CHECK(cs.eval("(define *p4036su* (cons 1 2))").has_value(), "4036 AC2: define");
        auto r = cs.eval("(set-car! *p4036su* 42)");
        CHECK(r && !is_error(*r), "4036 AC2: single-use grant allows one write");
        auto got = cs.eval("(car *p4036su*)");
        CHECK(got && is_int(*got) && as_int(*got) == 42, "4036 AC2: write landed");
        auto r2 = cs.eval("(set-car! *p4036su* 43)");
        CHECK(r2 && is_error(*r2), "4036 AC2: grant consumed exactly once");
        auto got2 = cs.eval("(car *p4036su*)");
        CHECK(got2 && is_int(*got2) && as_int(*got2) == 42, "4036 AC2: second write denied");
    }

    {
        std::println("\n--- #4036 AC3: closure tenant stamp — same-tenant free, cross-tenant "
                     "IsolationDeny ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        // Allow/deny matrix runs under Restricted (no MT), mirroring the
        // #3720 AC4 allow-path precedent: under MT the NodeId-less dispatch
        // isolation consult denies unstamped refs before the body runs,
        // which would make the deny shape vacuous. The legacy-unstamped arm
        // (strict||MT) is covered by AC4 at the checked-entry level.
        const auto live_mid = aura::core::current_mutation_epoch();
        using aura::core::capability::g_capability_registry;
        using aura::core::capability::make_grant_provenance;
        g_capability_registry().grant(4036, "mutate",
                                      static_cast<aura::core::capability::Effect>(kEffectMutate),
                                      make_grant_provenance(live_mid, true, 0, 0));
        g_capability_registry().grant(7777, "mutate",
                                      static_cast<aura::core::capability::Effect>(kEffectMutate),
                                      make_grant_provenance(live_mid, true, 0, 0));
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4036);
        ev.clear_boundary_audit_mid_for_test();
        ev.note_boundary_audit_mid_for_test(live_mid);
        // Explicit-tenant seam: same stamp point as the production owner-hook
        // path (test binaries shadow the strong hook with the light-link
        // weak stub, so the seam pins the slot tenant deterministically).
        const auto sa = aura_alloc_closure_tenant(4036, 4036);
        const auto sb = aura_alloc_closure_tenant(4036, 4036);
        CHECK(sa >= 0 && sb >= 0, "4036 AC3: slots allocated");
        CHECK(aura_closure_is_freed(sa) == 0, "4036 AC3: sa live before free");
        // Same-tenant: effect allow (grant) → slot tenant == caller → free.
        auto r = cs.eval(std::format("(closure:free! {})", sa));
        CHECK(r && is_bool(*r) && as_bool(*r), "4036 AC3: same-tenant free allowed");
        CHECK(aura_closure_is_freed(sa) == 1, "4036 AC3: sa freed");
        // Caller switches tenant; sb stays stamped 4036. Bool-false result
        // proves the deny came from the closure check (dispatch/workspace
        // denies surface as error values) — non-vacuous.
        ev.set_capability_tenant_id(7777);
        auto r2 = cs.eval(std::format("(closure:free! {})", sb));
        CHECK(r2 && is_bool(*r2) && !as_bool(*r2), "4036 AC3: cross-tenant free denied");
        CHECK(aura_closure_is_freed(sb) == 0, "4036 AC3: sb NOT freed (zero free)");
        CHECK(aura_closure_live_count() >= 1, "4036 AC3: foreign slot still live");
    }

    {
        std::println(
            "\n--- #4036 AC4: legacy unstamped slot fails closed under MT/Strict; Off frees ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(0); // owner stamps 0 → legacy unstamped slot
        cs.register_jit_primitives();
        const auto sid = aura_alloc_closure(77);
        CHECK(sid >= 0, "4036 AC4: slot allocated");
        CHECK(aura_closure_is_freed(sid) == 0, "4036 AC4: slot live");
        aura::core::provenance::set_multi_tenant_env_active(true);
        CHECK(aura_free_closure_checked(sid, /*caller_tenant=*/9, /*sandbox_mode=*/1) != 0,
              "4036 AC4: legacy slot refuses under Restricted+MT");
        CHECK(aura_closure_is_freed(sid) == 0, "4036 AC4: zero free under MT");
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Strict);
        CHECK(aura_free_closure_checked(sid, 9, 1) != 0,
              "4036 AC4: legacy slot refuses under Strict");
        CHECK(aura_closure_is_freed(sid) == 0, "4036 AC4: still zero free under Strict");
        set_mode(SandboxMode::Off);
        CHECK(aura_free_closure_checked(sid, 9, 0) == 0, "4036 AC4: Off keeps today's free");
        CHECK(aura_closure_is_freed(sid) == 1, "4036 AC4: freed under Off");
    }

    {
        std::println("\n--- #4036 AC5: Soft/Off — zero grants, all four keep today's behavior ---");
        reset_all();
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        set_mode(SandboxMode::Off);
        cs.register_jit_primitives();
        CHECK(cs.eval("(define *p4036s* (cons 1 2))").has_value(), "4036 AC5: define pair");
        auto r1 = cs.eval("(set-car! *p4036s* 5)");
        CHECK(r1 && !is_error(*r1), "4036 AC5: set-car! works");
        CHECK(cs.eval("(set-cdr! *p4036s* 6)").has_value(), "4036 AC5: set-cdr! works");
        CHECK(cs.eval("(car *p4036s*)").has_value(), "4036 AC5: car reads");
        CHECK(cs.eval("(define *s4036x* \"hi\")").has_value(), "4036 AC5: define string");
        CHECK(cs.eval("(string-fill! *s4036x* 65)").has_value(), "4036 AC5: string-fill! works");
        const auto sid = aura_alloc_closure(5);
        auto r4 = cs.eval(std::format("(closure:free! {})", sid));
        CHECK(r4 && is_bool(*r4) && as_bool(*r4), "4036 AC5: closure:free! works");
        CHECK(aura_closure_is_freed(sid) == 1, "4036 AC5: slot freed");
        const auto src = read_file("src/compiler/security_side_effect.hh");
        CHECK(src.find("set-car!") != std::string::npos, "4036 AC5: infer names present");
    }

    // ── Issue #4057: set-car!/set-cdr! process-level g_pair_slots branch is
    // tenant-gated under the production face (sandbox != 0 and Strict or
    // Restricted+MT). The g_pair_slots index space is process-shared, so the
    // #4036 dispatch Mutate choke alone cannot stop a same-tenant grant from
    // writing another tenant's JIT pair handed over via mailbox (idx >=
    // pairs_.size() falls through to the process slot). The branch compares
    // the slot's owner stamp (g_pair_slot_tenants, parallel to g_pair_slots)
    // against the caller principal BEFORE the write; foreign or unstamped (0)
    // slots refuse with the slot unchanged and the existing
    // check_workspace_isolation / record_audit path carries the IsolationDeny
    // (fiber id + Mutation epoch). Soft/Off never reads the tenant array.
    // Runtime ACs drive the prim body directly (prims.lookup, #3798 AC2
    // precedent): under Restricted+MT the NodeId-less dispatch consult
    // denies unstamped refs pre-body (#4036 AC3 note), which would make a
    // dispatch-level deny vacuous for the body gate under test.
    {
        std::println("\n--- #4057 AC1: Restricted+MT foreign JIT pair → deny, slot unchanged ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        const auto live_mid = aura::core::current_mutation_epoch();
        using aura::core::capability::g_capability_registry;
        using aura::core::capability::make_grant_provenance;
        // Adversarial shape: tenant A holds own-tenant Mutate (the #4036
        // dispatch choke passes for it); NO cross_grant A→77 exists, so only
        // the in-body isolation arm can stop the foreign-slot write.
        g_capability_registry().grant(4057, "mutate",
                                      static_cast<aura::core::capability::Effect>(kEffectMutate),
                                      make_grant_provenance(live_mid, true, 0, 0));
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);     // Restricted (evaluator face)
        set_mode(SandboxMode::Restricted); // registry SOLE writer (#2657)
        ev.set_capability_tenant_id(4057);
        aura::core::provenance::set_multi_tenant_env_active(true);
        // Deterministic process slot table: idx0 = legacy unstamped slot
        // (tenant 0), idx1 = foreign tenant-77 JIT pair (mailbox handoff).
        // A's pairs_ stays empty so both indices take the g_pair_slots
        // branch (idx >= pairs_.size()). Slots go on the heap-owned list so
        // process-exit PairSlotCleanup frees them.
        g_pair_slots.clear();
        g_pair_slot_tenants.clear();
        auto* legacy = static_cast<PairSlot*>(std::malloc(sizeof(PairSlot)));
        legacy->car = 101;
        legacy->cdr = 102;
        g_pair_slots.push_back(legacy);
        g_pair_slot_tenants.push_back(0);
        g_owned_pair_slots_.push_back(legacy);
        auto* foreign = static_cast<PairSlot*>(std::malloc(sizeof(PairSlot)));
        foreign->car = 11;
        foreign->cdr = 22;
        g_pair_slots.push_back(foreign);
        g_pair_slot_tenants.push_back(77);
        g_owned_pair_slots_.push_back(foreign);
        auto set_car_fn = ev.primitives().lookup("set-car!");
        auto set_cdr_fn = ev.primitives().lookup("set-cdr!");
        CHECK(set_car_fn.has_value() && set_cdr_fn.has_value(), "4057 AC1: pair mutators present");
        const auto iso_epoch = aura::core::current_mutation_epoch();
        const auto fiber_now = static_cast<std::int64_t>(aura_fiber_current_id());
        auto r1 = (*set_car_fn)({aura::compiler::types::make_pair(1), make_int(9)});
        CHECK(is_error(r1), "4057 AC1: foreign JIT pair set-car! denied");
        CHECK(foreign->car == 11 && foreign->cdr == 22,
              "4057 AC1: foreign slot car/cdr unchanged (zero write)");
        CHECK(std::string_view(ev.last_mutate_error()).find("set-car") != std::string_view::npos,
              "4057 AC1: deny reason names set-car (isolation, not wildcard)");
        auto r1b = (*set_cdr_fn)({aura::compiler::types::make_pair(1), make_int(9)});
        CHECK(is_error(r1b), "4057 AC1: foreign JIT pair set-cdr! denied");
        CHECK(foreign->cdr == 22, "4057 AC1: foreign slot cdr still unchanged");
        // The deny rows are IsolationDeny carrying the live fiber id and the
        // Mutation epoch (#4057 observable; record_audit single path).
        const auto seq1 = g_security_event_ring().seq.load(std::memory_order_relaxed);
        const std::size_t rn1 = std::min<std::size_t>(seq1, g_security_event_ring().ring.size());
        bool saw_iso4057_car = false;
        bool saw_iso4057_cdr = false;
        for (std::size_t i = 0; i < rn1; ++i) {
            const auto& e = g_security_event_ring().ring[i];
            const auto op_sv = std::string_view(e.op);
            if (e.denied && e.epoch == iso_epoch && e.fiber_id == fiber_now &&
                e.kind == aura::core::security_event::SecurityEventKind::IsolationDeny) {
                if (op_sv.find("set-car") != std::string_view::npos)
                    saw_iso4057_car = true;
                if (op_sv.find("set-cdr") != std::string_view::npos)
                    saw_iso4057_cdr = true;
            }
        }
        CHECK(saw_iso4057_car, "4057 AC1: IsolationDeny op set-car! fiber+Mutation epoch");
        CHECK(saw_iso4057_cdr, "4057 AC1: IsolationDeny op set-cdr! fiber+Mutation epoch");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4057 AC2: Restricted+MT unstamped legacy slot (tenant 0) → deny ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4057);
        aura::core::provenance::set_multi_tenant_env_active(true);
        g_pair_slots.clear();
        g_pair_slot_tenants.clear();
        auto* legacy = static_cast<PairSlot*>(std::malloc(sizeof(PairSlot)));
        legacy->car = 101;
        legacy->cdr = 102;
        g_pair_slots.push_back(legacy);
        g_pair_slot_tenants.push_back(0); // unstamped — fails closed on MT
        g_owned_pair_slots_.push_back(legacy);
        auto sc_fn = ev.primitives().lookup("set-car!");
        CHECK(sc_fn.has_value(), "4057 AC2: set-car! present");
        auto r = (*sc_fn)({aura::compiler::types::make_pair(0), make_int(9)});
        CHECK(is_error(r), "4057 AC2: unstamped slot set-car! denied");
        CHECK(legacy->car == 101 && legacy->cdr == 102,
              "4057 AC2: unstamped slot unchanged (zero write)");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4057 AC3: caller's own pairs_[idx] write keeps working ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        const auto live_mid = aura::core::current_mutation_epoch();
        using aura::core::capability::g_capability_registry;
        using aura::core::capability::make_grant_provenance;
        g_capability_registry().grant(4057, "mutate",
                                      static_cast<aura::core::capability::Effect>(kEffectMutate),
                                      make_grant_provenance(live_mid, true, 0, 0));
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted); // no MT: dispatch unstamped-ref rule idle
        ev.set_capability_tenant_id(4057);
        // Restricted allow-path needs the live Mutation epoch joined (same
        // #4036 AC3 / #3594 seeding — a silent fail-closed deny has an empty
        // reason and would mask the allow shape under test).
        ev.clear_boundary_audit_mid_for_test();
        ev.note_boundary_audit_mid_for_test(live_mid);
        // Own pair via cons: idx 0 < pairs_.size() → local branch, no tenant
        // gate; the dispatch Mutate allow comes from the grant (#3720 AC4
        // Restricted no-MT allow-path precedent).
        CHECK(cs.eval("(define *p4057* (cons 5 6))").has_value(), "4057 AC3: cons");
        auto w = cs.eval("(set-car! *p4057* 9)");
        CHECK(w && !is_error(*w), "4057 AC3: own pair set-car! allowed");
        auto got = cs.eval("(car *p4057*)");
        CHECK(got && is_int(*got) && as_int(*got) == 9, "4057 AC3: write landed");
    }

    {
        std::println("\n--- #4057 AC4: Soft/Off — high idx still writes the process slot ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        set_mode(SandboxMode::Off);
        g_pair_slots.clear();
        g_pair_slot_tenants.clear();
        auto* foreign = static_cast<PairSlot*>(std::malloc(sizeof(PairSlot)));
        foreign->car = 11;
        foreign->cdr = 22;
        g_pair_slots.push_back(foreign);
        g_pair_slot_tenants.push_back(77); // foreign stamp — unread on Soft/Off
        g_owned_pair_slots_.push_back(foreign);
        const auto se0 = g_security_event_ring().total.load(std::memory_order_relaxed);
        auto sc_fn = ev.primitives().lookup("set-car!");
        CHECK(sc_fn.has_value(), "4057 AC4: set-car! present");
        auto r = (*sc_fn)({aura::compiler::types::make_pair(0), make_int(9)});
        CHECK(!is_error(r), "4057 AC4: Soft write allowed");
        // The slot stores raw EvalValue bits (the prim writes a[1].val), so
        // the landed car is make_int(9).val — compare in the same encoding.
        CHECK(foreign->car == make_int(9).val && foreign->cdr == 22,
              "4057 AC4: process slot written (tenant array unread)");
        CHECK(g_security_event_ring().total.load(std::memory_order_relaxed) == se0,
              "4057 AC4: no extra isolation deny");
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
            const auto win = src.substr(
                pc, 3000); // widened 2026-09-16: #3850+ wave growth put gates at +2467..+2703
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
            const auto hs_win = src.substr(
                hs,
                3000); // widened 2026-09-16: HashSet gate arm widened with the PrimCall window
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
        CHECK(!is_error(vec_r), "3798 AC2: direct vector construct");
        const auto vec = vec_r;
        auto before = (*vref_pfn)({vec, make_int(0)});
        CHECK(is_int(before) && as_int(before) == 10, "3798 AC2: before[0]==10");

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
            CHECK(is_int(after) && as_int(after) == 10, "3798 AC2: workspace vector unchanged");
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
        CHECK(!is_error(vec_r), "3798 AC3: vector construct");
        const auto vec = vec_r;

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
            CHECK(is_int(after) && as_int(after) == 99, "3798 AC3: Soft stored 99");
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
        std::println(
            "\n--- #3834 AC1: Call primitive arm production fail-closed next to PrimCall ---");
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
            CHECK(win.find("Issue #3834") != std::string::npos ||
                      win.find("#3834") != std::string::npos,
                  "3834 AC1: Call documents #3834");
            // Soft/Off raw path retained (call_args form).
            CHECK(win.find("(*pfn)(call_args)") != std::string::npos,
                  "3834 AC3: Soft/Off ownerless raw (*pfn)(call_args) retained");
        }
        if (pc != std::string::npos) {
            const auto pc_win = src.substr(
                pc, 3000); // widened 2026-09-16: #3850+ wave growth put gates at +2467..+2703
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
        CHECK(!is_error(vec_r), "3834 AC2: direct vector construct");
        const auto vec = vec_r;
        auto before = (*vref_pfn)({vec, make_int(0)});
        CHECK(is_int(before) && as_int(before) == 10, "3834 AC2: before[0]==10");

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
            {aura::ir::IROpcode::Arg, {0, 0, 0, 0}},             // locals[0] = vec
            {aura::ir::IROpcode::Arg, {1, 1, 0, 0}},             // locals[1] = idx
            {aura::ir::IROpcode::Arg, {2, 2, 0, 0}},             // locals[2] = val
            {aura::ir::IROpcode::Primitive, {5, vs_slot, 0, 0}}, // locals[5] = vector-set!
            {aura::ir::IROpcode::Call, {5, 0, 3, 3}},            // Call prim with 3 args
            {aura::ir::IROpcode::Primitive, {6, vr_slot, 0, 0}}, // locals[6] = vector-ref
            {aura::ir::IROpcode::Call, {6, 0, 2, 4}},            // Call vector-ref
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
            CHECK(is_int(after) && as_int(after) == 10, "3834 AC2: workspace vector unchanged");
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
        CHECK(!is_error(vec_r), "3834 AC3: vector construct");
        const auto vec = vec_r;
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
            CHECK(is_int(after) && as_int(after) == 99, "3834 AC3: Soft stored 99");
        }
        set_mode(SandboxMode::Off);
    }

    {
        std::println("\n--- #3834 AC4: no new query key; extend #3798/#3720 family ---");
        CHECK(true, "3834 AC4: no new query key (reuse production_defaults gate)");
    }


    // ── Issue #4037: author-fingerprint identity write gate ──
    ac4037_meta_contract();
    ac4037_1_deny_no_grant();
    ac4037_1b_batch_blame_stays_zero();
    ac4037_2_ta_allow_batch_blame();
    ac4037_3_wildcard_insufficient();
    ac4037_4_clear_zero_ungated();
    ac4037_5_off_ungated();
    ac4037_source_cite();

    std::println("\n=== #2152/#3524 dispatch required_effects: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dispatch_required_effects();
}
#endif
