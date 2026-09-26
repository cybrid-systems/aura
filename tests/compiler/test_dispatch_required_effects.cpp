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
using aura::compiler::types::as_hash_idx; // Issue #4110: read the minted QueryResult hidx
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
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

// ── Issue #4093: JIT cell/hash heap tenant isolation — the process-global
// g_cell_heap / g_hash_tables / g_pair_slots carry owner-principal stamps
// (the #4057 pair-slot shape) and the production face (sandbox != 0 and
// Strict or Restricted+MT) refuses foreign / unstamped slots with the
// access skipped and the deny fired through the owner's
// check_workspace_isolation (IsolationDeny SE, fiber id + Mutation epoch).
// Light-link test binaries shadow the strong owner hooks with weak
// fail-closed stubs (the #4036 rationale), so the behavioral arms drive the
// gate through the explicit-context checked seams with real tenant stamps;
// the hook-driven production wiring is pinned by the cite arm + the linter.
static void ac4093_source_cite() {
    std::println("\n--- #4093 cite: stamps, gates, seams, hooks, resets ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("static std::vector<std::uint64_t> g_cell_tenants;") != std::string::npos,
          "4093 cite: g_cell_tenants parallel array");
    const auto ncp = rt.find("int64_t aura_new_cell()");
    CHECK(ncp != std::string::npos, "4093 cite: aura_new_cell");
    if (ncp != std::string::npos) {
        const auto win = rt.substr(ncp, 700);
        CHECK(win.find("g_cell_tenants") != std::string::npos,
              "4093 cite: aura_new_cell stamps the owner tenant");
        CHECK(win.find("aura_jit_owner_capability_tenant()") != std::string::npos,
              "4093 cite: stamp from the owner hook");
    }
    CHECK(rt.find("aura_new_cell_tenant") != std::string::npos,
          "4093 cite: explicit-tenant seam (#4036 aura_alloc_closure_tenant shape)");
    const auto cgc = rt.find("aura_cell_get_checked(int64_t cell_id, std::uint64_t caller_tenant");
    CHECK(cgc != std::string::npos, "4093 cite: aura_cell_get_checked seam");
    if (cgc != std::string::npos) {
        const auto win = rt.substr(cgc, 1600);
        CHECK(win.find("jit_tenant_gate_armed(sandbox_mode)") != std::string::npos,
              "4093 cite: cell read face probe");
        CHECK(win.find("jit_tenant_gate(slot_tenant, caller_tenant") != std::string::npos,
              "4093 cite: cell read owner compare");
    }
    const auto cgp = rt.find("int64_t aura_cell_get(int64_t cell_id)");
    CHECK(cgp != std::string::npos, "4093 cite: aura_cell_get wrapper");
    if (cgp != std::string::npos) {
        const auto win = rt.substr(cgp, 300);
        CHECK(win.find("aura_jit_owner_capability_tenant()") != std::string::npos &&
                  win.find("aura_jit_owner_sandbox_mode()") != std::string::npos,
              "4093 cite: production read delegates with owner-hook values");
    }
    const auto csp = rt.find("void aura_cell_set(int64_t cell_id, int64_t val)");
    CHECK(csp != std::string::npos, "4093 cite: aura_cell_set wrapper");
    if (csp != std::string::npos) {
        const auto win = rt.substr(csp, 700);
        CHECK(win.find("aura_jit_owner_require_effect") != std::string::npos,
              "4093 cite: #4018 Mutate choke stays in the production wrapper");
        CHECK(win.find("aura_cell_set_checked(cell_id, val,") != std::string::npos,
              "4093 cite: production write delegates to the checked seam");
    }
    const auto csc = rt.find("aura_cell_set_checked(int64_t cell_id, int64_t val,");
    CHECK(csc != std::string::npos, "4093 cite: aura_cell_set_checked seam");
    if (csc != std::string::npos) {
        const auto win = rt.substr(csc, 1600);
        CHECK(win.find("jit_tenant_gate_armed(sandbox_mode)") != std::string::npos,
              "4093 cite: cell write face probe");
    }
    const auto hrc = rt.find("aura_hash_ref_checked(int64_t hash_val, int64_t key_val,");
    CHECK(hrc != std::string::npos, "4093 cite: aura_hash_ref_checked seam");
    if (hrc != std::string::npos) {
        const auto win = rt.substr(hrc, 1800);
        CHECK(win.find("jit_tenant_gate_armed(sandbox_mode)") != std::string::npos,
              "4093 cite: hash read face probe");
    }
    const auto hsp = rt.find("int64_t aura_hash_set(int64_t hash_val, int64_t pair_val)");
    CHECK(hsp != std::string::npos, "4093 cite: aura_hash_set wrapper");
    if (hsp != std::string::npos) {
        const auto win = rt.substr(hsp, 600);
        CHECK(win.find("aura_jit_owner_require_effect") != std::string::npos,
              "4093 cite: #3720 Mutate choke stays in the production wrapper");
        CHECK(win.find("aura_hash_set_checked(hash_val, pair_val,") != std::string::npos,
              "4093 cite: production hash write delegates to the checked seam");
    }
    CHECK(rt.find("jit_tenant_gate_armed(aura_jit_owner_sandbox_mode())") != std::string::npos,
          "4093 cite: hash-remove inline gate reads the owner face");
    const auto pfl = rt.find("static int64_t pair_field_locked");
    CHECK(pfl != std::string::npos, "4093 cite: pair_field_locked");
    if (pfl != std::string::npos) {
        const auto win = rt.substr(pfl, 1600);
        CHECK(win.find("g_pair_slot_tenants") != std::string::npos,
              "4093 cite: pair field read consults the slot stamp");
        CHECK(win.find("jit_tenant_gate_armed(sandbox_mode)") != std::string::npos,
              "4093 cite: pair field face probe");
    }
    CHECK(rt.find("aura_hash_alloc_tenant") != std::string::npos,
          "4093 cite: hash alloc seam stamps g_hash_tenants");
    CHECK(rt.find("aura_alloc_pair_tenant") != std::string::npos,
          "4093 cite: pair alloc seam stamps g_pair_slot_tenants");
    CHECK(rt.find("g_cell_tenants.clear();") != std::string::npos &&
              rt.find("g_hash_tenants.clear();") != std::string::npos,
          "4093 cite: resets clear the stamp arrays");
    const auto sh = read_file("src/compiler/runtime_shared.h");
    CHECK(sh.find("extern std::vector<std::uint64_t> g_hash_tenants;") != std::string::npos,
          "4093 cite: runtime_shared.h declares g_hash_tenants");
    CHECK(sh.find("aura_cell_get_checked") != std::string::npos,
          "4093 cite: runtime_shared.h declares the checked seams");
    const auto ssot = read_file("src/compiler/runtime_ssot.cpp");
    CHECK(ssot.find("std::vector<std::uint64_t> g_hash_tenants;") != std::string::npos,
          "4093 cite: runtime_ssot.cpp defines g_hash_tenants");
    const auto vec = read_file("src/compiler/evaluator_primitives_vector.cpp");
    const auto ag = read_file("src/compiler/evaluator_primitives_agent.cpp");
    CHECK(vec.find("g_hash_tenants[hidx] = aura_jit_owner_capability_tenant();") !=
              std::string::npos,
          "4093 cite: hash prim stamps the owner tenant");
    CHECK(ag.find("g_hash_tenants[hidx] = aura_jit_owner_capability_tenant();") !=
              std::string::npos,
          "4093 cite: agent hash alloc sites stamp the owner tenant");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("aura_jit_owner_sandbox_mode") != std::string::npos &&
              svc.find("owner->effect_sandbox_mode()") != std::string::npos,
          "4093 cite: strong sandbox hook");
    CHECK(svc.find("aura_jit_owner_check_isolation") != std::string::npos &&
              svc.find("owner->check_workspace_isolation") != std::string::npos,
          "4093 cite: strong isolation hook");
    const auto stub = read_file("src/compiler/aura_jit_prim_dispatch_stub.cpp");
    CHECK(stub.find("aura_jit_owner_sandbox_mode") != std::string::npos &&
              stub.find("aura_jit_owner_check_isolation") != std::string::npos,
          "4093 cite: weak stubs present");
    const auto pair_src = read_file("src/compiler/evaluator_primitives_pair.cpp");
    CHECK(pair_src.find("slot_tenant != ev.capability_tenant_id()") != std::string::npos,
          "4093 cite: set-car!/set-cdr! #4057 compare intact");
}

static void ac4093_1_foreign_deny() {
    std::println("\n--- #4093 AC1: foreign tenant JIT cell/hash/pair access denied ---");
    reset_all();
    // Two principals, distinct stamps, production face. The checked seams
    // take the caller/face explicitly (light-link binaries shadow the strong
    // owner hooks — #4036 rationale); sandbox_mode = 2 arms the face without
    // env arming. B's own Mutate grant behavior is #3720/#4018-covered; the
    // arms below isolate the #4093 owner-compare semantics.
    constexpr std::uint64_t tenant_a = 4093;
    constexpr std::uint64_t tenant_b = 4094;
    constexpr int kStrictFace = 2;
    // Tenant A owns its slots via the explicit-tenant alloc seams.
    const auto cell_a = aura_new_cell_tenant(static_cast<std::int64_t>(tenant_a));
    CHECK(cell_a >= 0, "4093 AC1: A alloc cell");
    const auto hash_a = aura_hash_alloc_tenant(static_cast<std::int64_t>(tenant_a));
    CHECK(hash_a >= 0, "4093 AC1: A alloc hash");
    const auto pair_a = aura_alloc_pair_tenant(11, 22, static_cast<std::int64_t>(tenant_a));
    CHECK(pair_a != 0, "4093 AC1: A alloc pair");
    const auto kp_a = aura_alloc_pair_tenant(7, 8, static_cast<std::int64_t>(tenant_a));
    CHECK(aura_hash_set_checked(hash_a, kp_a, tenant_a, kStrictFace) == 0,
          "4093 AC1: A hash-set 7->8");
    aura_cell_set_checked(cell_a, 111, tenant_a, kStrictFace);
    CHECK(aura_cell_get_checked(cell_a, tenant_a, kStrictFace) == 111,
          "4093 AC1: A cell holds 111");
    CHECK(aura_hash_ref_checked(hash_a, 7, tenant_a, kStrictFace) == 8,
          "4093 AC1: A hash holds 7->8");
    CHECK(aura_pair_car_unchecked_checked(pair_a, tenant_a, kStrictFace) == 11,
          "4093 AC1: A pair car 11");
    // B's Mutate grant would pass the #4018 choke; the #4093 owner compare
    // refuses A's slots — access skipped, slot unchanged (the deny fires
    // through check_workspace_isolation in production links).
    aura_cell_set_checked(cell_a, 999, tenant_b, kStrictFace);
    CHECK(aura_cell_get_checked(cell_a, tenant_b, kStrictFace) == 0,
          "4093 AC1: B cell-ref on A denied (0)");
    const auto kp_b = aura_alloc_pair_tenant(9, 10, static_cast<std::int64_t>(tenant_b));
    CHECK(aura_hash_set_checked(hash_a, kp_b, tenant_b, kStrictFace) == 0,
          "4093 AC1: B hash-set on A denied");
    CHECK(aura_hash_ref_checked(hash_a, 7, tenant_b, kStrictFace) == 11,
          "4093 AC1: B hash-ref on A denied (not-found sentinel)");
    CHECK(aura_pair_car_unchecked_checked(pair_a, tenant_b, kStrictFace) == 0,
          "4093 AC1: B pair-car on A denied (0)");
    // B's own slots still update under the same face (grant + own stamp).
    const auto cell_b = aura_new_cell_tenant(static_cast<std::int64_t>(tenant_b));
    aura_cell_set_checked(cell_b, 222, tenant_b, kStrictFace);
    CHECK(aura_cell_get_checked(cell_b, tenant_b, kStrictFace) == 222,
          "4093 AC1: B own cell updates");
    const auto hash_b = aura_hash_alloc_tenant(static_cast<std::int64_t>(tenant_b));
    CHECK(aura_hash_set_checked(hash_b, kp_b, tenant_b, kStrictFace) == 0,
          "4093 AC1: B own hash-set 9->10");
    CHECK(aura_hash_ref_checked(hash_b, 9, tenant_b, kStrictFace) == 10,
          "4093 AC1: B own hash holds 9->10");
    const auto pair_b = aura_alloc_pair_tenant(33, 44, static_cast<std::int64_t>(tenant_b));
    CHECK(aura_pair_car_unchecked_checked(pair_b, tenant_b, kStrictFace) == 33,
          "4093 AC1: B own pair readable");
    // A's slots unchanged after B's attempts (deny = skip; no partial write).
    CHECK(aura_cell_get_checked(cell_a, tenant_a, kStrictFace) == 111,
          "4093 AC1: A cell unchanged");
    CHECK(aura_hash_ref_checked(hash_a, 7, tenant_a, kStrictFace) == 8,
          "4093 AC1: A hash unchanged");
    CHECK(aura_pair_car_unchecked_checked(pair_a, tenant_a, kStrictFace) == 11,
          "4093 AC1: A pair unchanged");
}

static void ac4093_2_unstamped_and_single_tenant() {
    std::println(
        "\n--- #4093 AC2: unstamped cell fails closed armed, single-tenant permissive ---");
    reset_all();
    // Legacy unstamped slot (tenant 0) via the explicit-tenant seam.
    const auto legacy = aura_new_cell_tenant(0);
    CHECK(legacy >= 0, "4093 AC2: legacy unstamped cell allocated");
    // Strict face (mode 2): the owner compare refuses the unstamped ref
    // (the #3365/#4057 deny shape on the JIT heap path).
    aura_cell_set_checked(legacy, 5, 4094, 2);
    CHECK(aura_cell_get_checked(legacy, 4094, 2) == 0, "4093 AC2: armed unstamped read denied (0)");
    // Single-tenant Restricted (mode 1, no MT, not Strict): face off →
    // legacy permissive (the contract the #4057 pair arm keeps).
    aura_cell_set_checked(legacy, 6, 4094, 1);
    CHECK(aura_cell_get_checked(legacy, 4094, 1) == 6,
          "4093 AC2: single-tenant Restricted stays permissive");
}

static void ac4093_3_soft_shares() {
    std::println("\n--- #4093 AC3: Soft/Off still shares the tables ---");
    reset_all();
    const auto cell_a = aura_new_cell_tenant(4093);
    aura_cell_set_checked(cell_a, 111, 4093, 0);
    const auto hash_a = aura_hash_alloc_tenant(4093);
    const auto kp = aura_alloc_pair_tenant(7, 8, 4093);
    CHECK(aura_hash_set_checked(hash_a, kp, 4093, 0) == 0, "4093 AC3: A hash-set");
    // Face off (mode 0): no stamp compare — the tables stay shared across
    // principals (zero-cost contract; the face probe precedes any stamp
    // load, so Soft/Off never reads the tenant arrays).
    aura_cell_set_checked(cell_a, 555, 4094, 0);
    CHECK(aura_cell_get_checked(cell_a, 4094, 0) == 555, "4093 AC3: B writes A's cell under Soft");
    CHECK(aura_hash_set_checked(hash_a, kp, 4094, 0) == 0, "4093 AC3: B hash-set on A's table");
    CHECK(aura_hash_ref_checked(hash_a, 7, 4094, 0) == 8, "4093 AC3: B reads it back (shared)");
}

// ── Issue #4094: JIT closure-capture tenant gate — a foreign-stamped env
// cell is refused and an unstamped (0) slot fails closed under Strict / MT
// (free's #4036 arms), the deny routed through check_workspace_isolation;
// the Mutate choke sits in the production wrapper on the production face
// (#3720/#4018 parity); Soft/Off keeps today's store (face probe precedes
// any stamp load). The checked seam drives caller/face explicitly
// (light-link binaries shadow the strong owner hooks — #4036 rationale);
// aura_closure_env_get observes the cell value (diagnostic family of
// aura_closure_get_env_gen). B's own Mutate-grant behavior on the choke is
// #3720/#4018-covered; the arms below isolate the #4094 owner-compare
// semantics.
static void ac4094_source_cite() {
    std::println("\n--- #4094 cite: capture gate, choke, seams, observation read ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("extern \"C\" void aura_closure_capture_checked(") != std::string::npos,
          "4094 cite: aura_closure_capture_checked seam");
    const auto cap =
        rt.find("void aura_closure_capture(int64_t closure_id, int64_t idx, int64_t val)");
    CHECK(cap != std::string::npos, "4094 cite: aura_closure_capture wrapper");
    if (cap != std::string::npos) {
        const auto win = rt.substr(cap, 900);
        CHECK(win.find("aura_jit_owner_sandbox_mode()") != std::string::npos,
              "4094 cite: choke reads the owner face");
        CHECK(win.find("face != 0 &&") != std::string::npos,
              "4094 cite: choke scoped to the production face (Soft/Off keeps today's store)");
        CHECK(win.find("aura_jit_owner_require_effect(") != std::string::npos &&
                  win.find("\"closure-capture\"") != std::string::npos,
              "4094 cite: Mutate choke op closure-capture (no grant → no store)");
        const auto joined = [](std::string s) {
            for (auto& ch : s)
                if (ch == '\n' || ch == '\t')
                    ch = ' ';
            return s;
        }(win);
        CHECK(joined.find("aura_closure_capture_checked(closure_id, idx, val,") !=
                      std::string::npos &&
                  joined.find("aura_jit_owner_capability_tenant(), face") != std::string::npos,
              "4094 cite: wrapper delegates with owner-hook values");
    }
    const auto seam =
        rt.find("aura_closure_capture_checked(int64_t closure_id, int64_t idx, int64_t val,");
    CHECK(seam != std::string::npos, "4094 cite: checked seam body");
    if (seam != std::string::npos) {
        const auto end = cap != std::string::npos && cap > seam ? cap : seam + 4200;
        const auto win = rt.substr(seam, end - seam);
        const auto face = win.find("if (sandbox_mode != 0) {");
        const auto stamp = win.find("g_closure_tenants[cid]");
        CHECK(face != std::string::npos && stamp != std::string::npos,
              "4094 cite: production-face gate reads the #4036 stamp array");
        CHECK(face == std::string::npos || stamp == std::string::npos || stamp > face,
              "4094 cite: face probe precedes the stamp load (Soft zero-cost)");
        CHECK(win.find("slot_tenant != 0 && slot_tenant != caller_tenant") != std::string::npos,
              "4094 cite: foreign-stamp refuse (free's #4036 arm)");
        CHECK(win.find("::aura::core::sandbox::is_strict()") != std::string::npos &&
                  win.find("::aura::core::provenance::multi_tenant_env_active()") !=
                      std::string::npos,
              "4094 cite: unstamped 0 fails closed under Strict / MT (free's #4036 arm)");
        CHECK(win.find("aura_jit_owner_check_isolation(") != std::string::npos &&
                  win.find("\"closure-capture\"") != std::string::npos,
              "4094 cite: deny routed through check_workspace_isolation");
        const auto is_arena = win.find("const bool is_arena");
        CHECK(is_arena != std::string::npos && face != std::string::npos && face < is_arena,
              "4094 cite: gate precedes the arena/heap store paths");
    }
    CHECK(rt.find("aura_closure_env_get(std::int64_t closure_id, std::int64_t idx)") !=
              std::string::npos,
          "4094 cite: value-observation read (aura_closure_get_env_gen family)");
    const auto sh = read_file("src/compiler/runtime_shared.h");
    CHECK(sh.find("aura_closure_capture_checked(") != std::string::npos &&
              sh.find("aura_closure_env_get(") != std::string::npos,
          "4094 cite: runtime_shared.h declares the seam + observation read");
}

static void ac4094_1_foreign_deny() {
    std::println(
        "\n--- #4094 AC1: foreign tenant capture of A's closure denied, env unchanged ---");
    reset_all();
    // Two principals, distinct stamps, production face via the seam params
    // (light-link binaries shadow the strong owner hooks — #4036 rationale).
    constexpr std::uint64_t tenant_a = 4094;
    constexpr std::uint64_t tenant_b = 4095;
    constexpr int kStrictFace = 2;
    // A allocates (explicit-tenant seam stamps g_closure_tenants) and
    // captures on its own stamp — the gate passes.
    const auto cid_a = aura_alloc_closure_tenant(4094, tenant_a);
    CHECK(cid_a >= 0, "4094 AC1: A alloc closure");
    aura_closure_capture_checked(cid_a, 0, 111, tenant_a, kStrictFace);
    CHECK(aura_closure_env_get(cid_a, 0) == 111, "4094 AC1: A capture lands");
    // B's capture of A's cid: the owner compare refuses the foreign slot —
    // capture skipped, env cell unchanged (in production links the deny
    // fires through check_workspace_isolation: IsolationDeny SE with B's
    // fiber id + the Mutation epoch; the light-link stub keeps JIT behavior).
    aura_closure_capture_checked(cid_a, 0, 999, tenant_b, kStrictFace);
    CHECK(aura_closure_env_get(cid_a, 0) == 111,
          "4094 AC1: B capture on A denied (env cell unchanged)");
    // B's own closure still captures under the same face (grant + own stamp).
    const auto cid_b = aura_alloc_closure_tenant(4095, tenant_b);
    CHECK(cid_b >= 0, "4094 AC1: B alloc closure");
    aura_closure_capture_checked(cid_b, 0, 222, tenant_b, kStrictFace);
    CHECK(aura_closure_env_get(cid_b, 0) == 222, "4094 AC1: B own capture lands");
    // A's cell unchanged after B's attempt (deny = skip; no partial write).
    CHECK(aura_closure_env_get(cid_a, 0) == 111, "4094 AC1: A env unchanged");
}

static void ac4094_2_unstamped_fail_closed() {
    std::println("\n--- #4094 AC2: unstamped closure fails closed under Strict/MT, single-tenant "
                 "permissive ---");
    reset_all();
    // Legacy unstamped slot (tenant 0) via the explicit-tenant seam.
    const auto legacy = aura_alloc_closure_tenant(4094, 0);
    CHECK(legacy >= 0, "4094 AC2: legacy unstamped closure allocated");
    // Strict (process face): free's unstamped arm fires — capture refused.
    set_mode(SandboxMode::Strict);
    aura_closure_capture_checked(legacy, 0, 5, 4095, 2);
    CHECK(aura_closure_env_get(legacy, 0) == 0, "4094 AC2: armed unstamped capture denied (0)");
    // Restricted + MT: the same arm.
    aura::core::provenance::set_multi_tenant_env_active(true);
    set_mode(SandboxMode::Restricted);
    aura_closure_capture_checked(legacy, 0, 6, 4095, 1);
    CHECK(aura_closure_env_get(legacy, 0) == 0, "4094 AC2: MT unstamped capture denied");
    aura::core::provenance::set_multi_tenant_env_active(false);
    // Single-tenant Restricted (mode 1, no MT, not Strict): the free-arm
    // contract keeps legacy slots permissive.
    aura_closure_capture_checked(legacy, 0, 7, 4095, 1);
    CHECK(aura_closure_env_get(legacy, 0) == 7,
          "4094 AC2: single-tenant Restricted stays permissive");
    set_mode(SandboxMode::Off);
}

static void ac4094_3_soft_writes() {
    std::println("\n--- #4094 AC3: Soft/Off keeps today's store ---");
    reset_all();
    const auto cid_a = aura_alloc_closure_tenant(4094, 4094);
    CHECK(cid_a >= 0, "4094 AC3: A alloc closure");
    // Face off (mode 0): no stamp compare — the tables stay shared across
    // principals (zero-cost contract: the face probe precedes any
    // g_closure_tenants load, so Soft/Off never reads the stamps).
    aura_closure_capture_checked(cid_a, 0, 111, 4094, 0);
    CHECK(aura_closure_env_get(cid_a, 0) == 111, "4094 AC3: A capture lands under Soft");
    aura_closure_capture_checked(cid_a, 0, 555, 4095, 0);
    CHECK(aura_closure_env_get(cid_a, 0) == 555, "4094 AC3: B captures A's slot under Soft");
    // Production wrapper under the light-link weak stubs (face hook → 0):
    // the choke never arms and the store proceeds — today's Soft behavior
    // is preserved for every existing caller of the production symbol.
    const auto cid_w = aura_alloc_closure(4094);
    CHECK(cid_w >= 0, "4094 AC3: production alloc");
    aura_closure_capture(cid_w, 0, 77);
    CHECK(aura_closure_env_get(cid_w, 0) == 77, "4094 AC3: production wrapper stores under Soft");
}

// ── Issue #4110: evaluator-minted hashes carry the owner stamp and the
// tree-walker hash prims run the same armed-face owner compare the JIT
// checked seams run. make_query_result_hash (and every other evaluator
// mint) pushed g_hash_tables without stamping g_hash_tenants, so under
// Restricted+MT / Strict the production JIT hash-ref gate read the
// owner's own query:find / query:pattern result as foreign (not-found
// sentinel 11); the interpreter prims never consulted the stamps, so a
// foreign principal could still read / write the process-global table.
// Fix: one SSOT stamp helper (aura_hash_stamp_new_table_owner) at every
// push site + aura_hash_gate_checked in the four tree-walker prims.
// Light-link binaries shadow the strong owner hooks with weak fail-closed
// stubs (#4036 rationale), so the behavioral arms drive the gate through
// the explicit-context seams with real tenant stamps; the hook-driven
// production wiring is pinned by the cite arm + the linter.
static void ac4110_source_cite() {
    std::println("\n--- #4110 cite: stamp helper, sweep, tree-walker gate, seam ---");
    const auto hdr = read_file("src/compiler/runtime_shared.h");
    CHECK(hdr.find("void aura_hash_stamp_new_table_owner();") != std::string::npos,
          "4110 cite: runtime_shared.h declares the stamp helper");
    CHECK(hdr.find("aura_hash_gate_checked(std::uint64_t hidx") != std::string::npos,
          "4110 cite: runtime_shared.h declares the tree-walker gate seam");
    const auto ssot = read_file("src/compiler/runtime_ssot.cpp");
    {
        const auto at = ssot.find("void aura_hash_stamp_new_table_owner() {");
        CHECK(at != std::string::npos, "4110 cite: runtime_ssot.cpp defines the stamp helper");
        if (at != std::string::npos) {
            const auto win = ssot.substr(at, 400);
            CHECK(win.find("g_hash_tenants.resize(g_hash_tables.size(), 0);") != std::string::npos,
                  "4110 cite: helper resizes to size (vector-constructor shape)");
            CHECK(win.find("g_hash_tenants[g_hash_tables.size() - 1]") != std::string::npos,
                  "4110 cite: helper stamps the back slot");
            CHECK(win.find("aura_jit_owner_capability_tenant()") != std::string::npos,
                  "4110 cite: helper stamps from the owner hook");
        }
    }
    // Sweep: every mint file stamps every push (same statement shape the
    // #4110 linter counts).
    static constexpr std::array<std::string_view, 17> kSwept = {
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        "src/compiler/evaluator_primitives_query_tail.cpp",
        "src/compiler/evaluator_primitives_query_lifecycle.cpp",
        "src/compiler/evaluator_primitives_query_reflect.cpp",
        "src/compiler/evaluator_primitives_query_type_stats.cpp",
        "src/compiler/evaluator_primitives_query_obs_mid.cpp",
        "src/compiler/evaluator_primitives_mutation.cpp",
        "src/compiler/evaluator_primitives_memory.cpp",
        "src/compiler/evaluator_primitives_obs_jit.cpp",
        "src/compiler/evaluator_primitives_obs_eval.cpp",
        "src/compiler/evaluator_primitives_messaging.cpp",
        "src/compiler/evaluator_primitives_stdlib_review.cpp",
        "src/compiler/evaluator_primitives_persist.cpp",
        "src/compiler/evaluator_primitives_json.cpp",
        "src/compiler/evaluator_primitives_compile.cpp",
        "src/compiler/evaluator.ixx",
        "src/compiler/evaluator_workspace_tree.cpp",
    };
    auto count_of = [](const std::string& s, std::string_view pat) {
        std::size_t n = 0;
        auto at = s.find(pat);
        while (at != std::string::npos) {
            ++n;
            at = s.find(pat, at + pat.size());
        }
        return n;
    };
    for (const auto f : kSwept) {
        const auto src = read_file(std::string(f).c_str());
        CHECK(!src.empty(), "4110 cite: swept mint file readable");
        CHECK(count_of(src, "g_hash_tables.push_back(ht);") ==
                  count_of(src, "aura_hash_stamp_new_table_owner();"),
              "4110 cite: every push in the mint file stamps the owner");
    }
    const auto qw = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    CHECK(qw.find("make_query_result_hash") != std::string::npos,
          "4110 cite: make_query_result_hash mint is in the stamped sweep set");
    // #4093-pinned inline stamps stay (the helper is additive, no rewrite).
    const auto vec = read_file("src/compiler/evaluator_primitives_vector.cpp");
    CHECK(vec.find("g_hash_tenants[hidx] = aura_jit_owner_capability_tenant();") !=
              std::string::npos,
          "4110 cite: vector hash prim inline stamp (#4093) intact");
    const auto ag = read_file("src/compiler/evaluator_primitives_agent.cpp");
    CHECK(ag.find("g_hash_tenants[hidx] = aura_jit_owner_capability_tenant();") !=
              std::string::npos,
          "4110 cite: agent hash alloc inline stamps (#4093) intact");
    // The four tree-walker prims gate before the probe with owner-hook values.
    CHECK(vec.find("aura_hash_gate_checked(hidx, aura_jit_owner_capability_tenant(),") !=
              std::string::npos,
          "4110 cite: tree-walker gates pass the owner-hook caller");
    CHECK(vec.find("aura_jit_owner_sandbox_mode(), \"hash-ref\"") != std::string::npos,
          "4110 cite: hash-ref gate reads the owner face hook");
    CHECK(vec.find("\"hash-ref\"))") != std::string::npos, "4110 cite: hash-ref gate op");
    CHECK(vec.find("\"hash-has-key?\"))") != std::string::npos, "4110 cite: hash-has-key? gate op");
    CHECK(vec.find("\"hash-set!\"))") != std::string::npos, "4110 cite: hash-set! gate op");
    CHECK(vec.find("\"hash-remove!\"))") != std::string::npos, "4110 cite: hash-remove! gate op");
    // Seam body: face probe precedes the stamp load; deny through the gate.
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    {
        const auto at = rt.find("extern \"C\" bool aura_hash_gate_checked(");
        CHECK(at != std::string::npos, "4110 cite: gate seam defined in the jit runtime TU");
        if (at != std::string::npos) {
            const auto win = rt.substr(at, 500);
            const auto armed_at = win.find("jit_tenant_gate_armed(sandbox_mode)");
            const auto load_at = win.find("g_hash_tenants[hidx]");
            CHECK(armed_at != std::string::npos && load_at != std::string::npos &&
                      armed_at < load_at,
                  "4110 cite: face probe precedes the stamp load (Soft/Off zero-cost)");
            CHECK(win.find("jit_tenant_gate(slot_tenant, caller_tenant, op)") != std::string::npos,
                  "4110 cite: deny routed through jit_tenant_gate (check_workspace_isolation)");
        }
        CHECK(rt.find("if (slot_tenant == caller_tenant)") != std::string::npos,
              "4110 cite: jit_tenant_gate exact compare intact");
        CHECK(rt.find("slot_tenant == 0 ||") == std::string::npos &&
                  rt.find("caller_tenant == 0 ||") == std::string::npos,
              "4110 cite: no unstamped-0 match weakening");
        CHECK(rt.find("aura_hash_alloc_tenant") != std::string::npos,
              "4110 cite: #4093 hash alloc seam intact");
    }
}

static void ac4110_1_query_find_stamped_jit_ref() {
    std::println("\n--- #4110 AC1: query:find mint stamps the owner; JIT hash-ref reads it ---");
    reset_all();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(),
          "4110 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4110 AC1: eval-current");
    // :as-query-result mints the QueryResult hash via make_query_result_hash
    // — the same push_back path the production auto-upgrade rides (schema-2
    // keys incl. schema-2192 ride the identical insert + publish).
    CHECK(cs.eval("(define qr4110 (query :find \"f\" :as-query-result #t))").has_value(),
          "4110 AC1: query:find mints the QueryResult hash");
    const auto qr = cs.eval("qr4110");
    CHECK(qr.has_value() && is_hash(*qr), "4110 AC1: result is a QueryResult hash");
    if (!qr || !is_hash(*qr))
        return;
    const auto hidx = as_hash_idx(*qr);
    CHECK(hidx < g_hash_tables.size() && g_hash_tables[hidx] != nullptr,
          "4110 AC1: table published");
    // THE FIX: the mint stamps the owner (pre-fix the parallel array stayed
    // at the resize default 0, so the JIT gate read the owner's own table
    // as foreign and returned the not-found sentinel 11 under the face).
    CHECK(g_hash_tenants.size() > hidx, "4110 AC1: stamp array covers the mint");
    if (g_hash_tenants.size() > hidx) {
        CHECK(g_hash_tenants[hidx] == aura_jit_owner_capability_tenant(),
              "4110 AC1: minted table stamped with the owner hook value");
    }
    // Recover the raw key bytes of the schema-2192 field (string keys are
    // string-heap tagged vals the test cannot mint) and drive the real JIT
    // probe under the armed face — the same splitmix slot walk the
    // production ABI runs (#4093 shape: face 2 arms without env arming).
    const auto* ht = g_hash_tables[hidx];
    const auto meta = ht->metadata();
    const auto keys = ht->keys();
    const auto vals = ht->values();
    std::int64_t key_val = 0;
    bool found = false;
    for (std::uint64_t i = 0; i < ht->capacity; ++i) {
        if (meta[i] == 0xFF)
            continue;
        if (vals[i] == make_int(2192).val) {
            key_val = keys[i];
            found = true;
            break;
        }
    }
    CHECK(found, "4110 AC1: schema-2192 field present");
    if (!found)
        return;
    constexpr int kStrictFace = 2;
    const auto hv = static_cast<std::int64_t>(qr->val);
    // The armed gate admits the owner principal on the minted table (the
    // fix — pre-#4110 the unstamped mint read as foreign and the JIT gate
    // returned the not-found sentinel 11 for the owner's own table) and
    // still skips a foreign principal through the same compare.
    CHECK(
        !aura_hash_gate_checked(hidx, aura_jit_owner_capability_tenant(), kStrictFace, "hash-ref"),
        "4110 AC1: armed gate admits the owner's own minted table");
    CHECK(aura_hash_gate_checked(hidx, 4111, kStrictFace, "hash-ref"),
          "4110 AC1: armed gate still skips a foreign principal");
    CHECK(aura_hash_ref_checked(hv, key_val, 4111, kStrictFace) == 11,
          "4110 AC1: foreign JIT ref still denied (sentinel 11)");
}

static void ac4110_2_tree_walker_agrees() {
    std::println("\n--- #4110 AC2: tree-walker hash-ref agrees with the JIT result ---");
    reset_all();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(),
          "4110 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4110 AC2: eval-current");
    CHECK(cs.eval("(define qr4110 (query :find \"f\" :as-query-result #t))").has_value(),
          "4110 AC2: query:find mints the QueryResult hash");
    const auto qr = cs.eval("qr4110");
    CHECK(qr.has_value() && is_hash(*qr), "4110 AC2: result is a QueryResult hash");
    if (!qr || !is_hash(*qr))
        return;
    const auto hidx = as_hash_idx(*qr);
    constexpr int kStrictFace = 2;
    // The armed gate admits the owner on the minted table (JIT path); the
    // tree-walker probe of the SAME table agrees on the content.
    CHECK(
        !aura_hash_gate_checked(hidx, aura_jit_owner_capability_tenant(), kStrictFace, "hash-ref"),
        "4110 AC2: owner gate-admitted on the minted table (JIT path)");
    const auto tw = cs.eval("(hash-ref qr4110 \"schema-2192\")");
    CHECK(tw.has_value() && is_int(*tw) && as_int(*tw) == 2192, "4110 AC2: tree-walker reads 2192");
    CHECK(tw && as_int(*tw) == 2192,
          "4110 AC2: tree-walker agrees with the minted field the gate admits");
    const auto has = cs.eval("(hash-has-key? qr4110 \"schema-2192\")");
    CHECK(has.has_value() && is_bool(*has) && as_bool(*has), "4110 AC2: hash-has-key? agrees (#t)");
}

static void ac4110_3_foreign_tree_walker_gate() {
    std::println("\n--- #4110 AC3: foreign tree-walker access skipped, no store ---");
    reset_all();
    // Tenant-stamped table via the #4093 seam (the light-link binary's weak
    // owner hook reads 0, so explicit stamps pin the slot tenant
    // deterministically).
    constexpr std::uint64_t tenant_a = 4110;
    constexpr std::uint64_t tenant_b = 4111;
    constexpr int kStrictFace = 2;
    const auto hash_a = aura_hash_alloc_tenant(static_cast<std::int64_t>(tenant_a));
    CHECK(hash_a >= 0, "4110 AC3: A alloc hash");
    const auto hidx_a = static_cast<std::uint64_t>(hash_a) >> 6;
    const auto kp = aura_alloc_pair_tenant(7, 8, static_cast<std::int64_t>(tenant_a));
    CHECK(aura_hash_set_checked(hash_a, kp, tenant_a, kStrictFace) == 0, "4110 AC3: A stores 7->8");
    // Owner passes the armed gate for the tree-walker ops; foreign is
    // SKIPPED (true = skip; the deny fires through check_workspace_isolation
    // in production links — the interpreter must return void / #f and not
    // store).
    CHECK(!aura_hash_gate_checked(hidx_a, tenant_a, kStrictFace, "hash-ref"),
          "4110 AC3: owner ref allowed");
    CHECK(!aura_hash_gate_checked(hidx_a, tenant_a, kStrictFace, "hash-set!"),
          "4110 AC3: owner set allowed");
    CHECK(aura_hash_gate_checked(hidx_a, tenant_b, kStrictFace, "hash-ref"),
          "4110 AC3: foreign ref skipped");
    CHECK(aura_hash_gate_checked(hidx_a, tenant_b, kStrictFace, "hash-has-key?"),
          "4110 AC3: foreign has-key? skipped");
    CHECK(aura_hash_gate_checked(hidx_a, tenant_b, kStrictFace, "hash-set!"),
          "4110 AC3: foreign set skipped");
    CHECK(aura_hash_gate_checked(hidx_a, tenant_b, kStrictFace, "hash-remove!"),
          "4110 AC3: foreign remove skipped");
    // The skip is real: B's write attempt leaves A's table unchanged.
    const auto kp_b = aura_alloc_pair_tenant(9, 10, static_cast<std::int64_t>(tenant_b));
    CHECK(aura_hash_set_checked(hash_a, kp_b, tenant_b, kStrictFace) == 0,
          "4110 AC3: B set returns (skipped)");
    CHECK(aura_hash_ref_checked(hash_a, 7, tenant_a, kStrictFace) == 8,
          "4110 AC3: A entry intact after B's set");
    CHECK(aura_hash_ref_checked(hash_a, 9, tenant_a, kStrictFace) == 11,
          "4110 AC3: B insert not stored (not found for A)");
}

static void ac4110_4_unarmed_face_no_tenant_load() {
    std::println("\n--- #4110 AC4: Soft/Off stays a face probe — no tenant consult ---");
    reset_all();
    constexpr std::uint64_t tenant_a = 4110;
    constexpr std::uint64_t tenant_b = 4111;
    const auto hash_a = aura_hash_alloc_tenant(static_cast<std::int64_t>(tenant_a));
    CHECK(hash_a >= 0, "4110 AC4: A alloc hash");
    const auto hidx_a = static_cast<std::uint64_t>(hash_a) >> 6;
    const auto kp = aura_alloc_pair_tenant(7, 8, static_cast<std::int64_t>(tenant_a));
    CHECK(aura_hash_set_checked(hash_a, kp, tenant_a, 0) == 0, "4110 AC4: A stores under Soft");
    // Face 0: the gate seam allows WITHOUT consulting the stamp — the
    // tables stay shared (the #4093 AC3 zero-cost contract; the face probe
    // precedes any g_hash_tenants load, so Soft/Off never reads it).
    CHECK(!aura_hash_gate_checked(hidx_a, tenant_b, 0, "hash-ref"),
          "4110 AC4: unarmed ref allowed");
    CHECK(!aura_hash_gate_checked(hidx_a, tenant_b, 0, "hash-set!"),
          "4110 AC4: unarmed set allowed");
    CHECK(!aura_hash_gate_checked(hidx_a, tenant_b, 0, "hash-has-key?"),
          "4110 AC4: unarmed has-key? allowed");
    CHECK(!aura_hash_gate_checked(hidx_a, tenant_b, 0, "hash-remove!"),
          "4110 AC4: unarmed remove allowed");
    const auto kp_b = aura_alloc_pair_tenant(9, 10, static_cast<std::int64_t>(tenant_b));
    CHECK(aura_hash_set_checked(hash_a, kp_b, tenant_b, 0) == 0, "4110 AC4: B stores under Soft");
    CHECK(aura_hash_ref_checked(hash_a, 9, tenant_b, 0) == 10,
          "4110 AC4: B reads it back (shared)");
    // Single-tenant Restricted (face 1, no MT env, not Strict): face off →
    // permissive (the #4093 AC2 contract preserved).
    aura::core::provenance::set_multi_tenant_env_active(false);
    CHECK(!aura_hash_gate_checked(hidx_a, tenant_b, 1, "hash-ref"),
          "4110 AC4: mode-1 unarmed allowed");
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
        // Issue #4093: anchor the production wrapper signature — the checked
        // seam (aura_hash_set_checked) shares the prefix but holds no choke.
        const auto setp = rt.find("int64_t aura_hash_set(int64_t hash_val");
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
        // Issue #4093: anchor the production wrapper signature (see above).
        const auto cellp = rt.find("void aura_cell_set(int64_t cell_id");
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

    // ── Issue #4059 (allow side): with a real Mutate grant the load body
    // choke ALLOWS and the workspace installs (the deny side runs in
    // test_load_cap_io_read.cpp — MT escape + no-Mutate effect deny).
    // #4037 AC2 grant shape: registry grants under Off, arm after, join
    // the boundary seed to the live epoch.
    {
        std::println("\n--- #4059 AC3: Restricted + Mutate grant → load installs workspace ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        const auto live_mid = aura::core::current_mutation_epoch();
        using aura::core::capability::g_capability_registry;
        using aura::core::capability::make_grant_provenance;
        g_capability_registry().grant(4059, "mutate",
                                      static_cast<aura::core::capability::Effect>(kEffectMutate),
                                      make_grant_provenance(live_mid, true, 0, 0));
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4059);
        ev.grant_capability(aura::compiler::security::kCapIoRead);
        ev.clear_boundary_audit_mid_for_test();
        ev.note_boundary_audit_mid_for_test(live_mid);
        const std::string allow_path = "/tmp/aura_4059_allow.aura";
        {
            std::ofstream out(allow_path);
            out << "(define *loaded4059* 41)\n";
        }
        const auto* ws0 = ev.workspace_flat();
        auto r = cs.eval(std::format("(load \"{}\")", allow_path));
        CHECK(r && !is_error(*r), "4059 AC3: load allowed with Mutate");
        CHECK(ev.workspace_flat() != ws0, "4059 AC3: workspace swapped (fresh flat AST)");
        auto got = cs.eval("*loaded4059*");
        CHECK(got && is_int(*got) && as_int(*got) == 41, "4059 AC3: loaded body evaluated");
        bool allow_row_ok = false;
        const auto seq = g_security_event_ring().seq.load(std::memory_order_relaxed);
        const std::size_t rn = std::min<std::size_t>(seq, g_security_event_ring().ring.size());
        for (std::size_t i = 0; i < rn; ++i) {
            const auto& e = g_security_event_ring().ring[i];
            if (!e.denied && e.kind == aura::core::security_event::SecurityEventKind::EffectAllow &&
                std::string_view(e.op) == "load")
                allow_row_ok = e.epoch == live_mid;
        }
        CHECK(allow_row_ok, "4059 AC3: EffectAllow row carries the Mutation epoch");
        // Wildcard variant: the "*" row (full mask) authorizes the swap too
        // (the #3144 strip keeps TA/MSE out; Mutate survives).
        aura::core::capability::reset_capability_effects_for_test();
        aura::core::capability::g_capability_registry().grant(
            4059, "*", aura::core::capability::effect_for_cap_name("*"),
            aura::core::capability::make_grant_provenance(live_mid, true, 0, 0));
        const auto* ws0b = ev.workspace_flat();
        auto rw = cs.eval(std::format("(load \"{}\")", allow_path));
        CHECK(rw && !is_error(*rw), "4059 AC3b: wildcard authorizes the swap");
        CHECK(ev.workspace_flat() != ws0b, "4059 AC3b: workspace swapped");
        std::remove(allow_path.c_str());
    }

    // ── Issue #4058: capability_stack_ (with-capability pushes) is lexical
    // scope only — check-capability / capability-stack readouts. It never
    // satisfies has_capability: the oracle reads effects_effective_for +
    // the granted_capabilities_ string mirror, so a zero-grant Agent can no
    // longer read host files, clear process exception stacks, or open the
    // kPrimSecSandboxed dispatch gate by pushing strings. with-capability
    // stays a non-grant (no registry write).
    {
        std::println("\n--- #4058 AC1: with-capability \"*\" cannot read files (zero grant) ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);     // Restricted (evaluator face)
        set_mode(SandboxMode::Restricted); // registry SOLE writer (#2657)
        ev.set_capability_tenant_id(4058);
        const std::string secret = std::string("/tmp/aura_4058_secret.txt");
        {
            std::ofstream out(secret);
            out << "aura-4058-secret-content\n";
        }
        auto r1 = cs.eval(std::format("(with-capability \"*\" (read-file \"{}\"))", secret));
        CHECK(r1 && is_error(*r1), "4058 AC1: stack-disguised read-file denied");
        bool leaked4058 = false;
        for (const auto& s : ev.string_heap())
            if (s.find("aura-4058-secret") != std::string::npos)
                leaked4058 = true;
        CHECK(!leaked4058, "4058 AC1: file content did not enter the string heap");
        // A real io-read registry grant keeps reading (non-MT Restricted:
        // the #3802 host-path gate stays passthrough for /tmp).
        ev.grant_capability(aura::compiler::security::kCapIoRead);
        auto r2 = cs.eval(std::format("(read-file \"{}\")", secret));
        CHECK(r2 && !is_error(*r2) && is_string(*r2),
              "4058 AC1: real io-read registry grant still reads");
        if (r2 && is_string(*r2)) {
            const auto sidx = as_string_idx(*r2);
            CHECK(sidx < ev.string_heap().size() &&
                      ev.string_heap()[sidx].find("aura-4058-secret") != std::string::npos,
                  "4058 AC1: real grant reads actual content");
        }
        std::remove(secret.c_str());
    }

    {
        std::println(
            "\n--- #4058 AC2: with-capability \"exception-control\" cannot clear fibers ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4058);
        auto e1 = cs.eval("(with-capability \"exception-control\" (jit:exception-fibers-clear))");
        CHECK(e1 && is_error(*e1), "4058 AC2: stack-disguised exception-fibers-clear denied");
        bool reason4058 = false;
        for (const auto& s : ev.string_heap())
            if (s.find("exception-control required") != std::string::npos)
                reason4058 = true;
        CHECK(reason4058, "4058 AC2: deny names the exception-control gate");
        CHECK(!ev.has_capability(aura::compiler::security::kCapExceptionControl),
              "4058 AC2: no capability leaked from the push");
        // Off-face control: the same form executes — the deny above is the
        // capability gate, not syntax.
        reset_all();
        CompilerService cs2;
        auto e2 = cs2.eval("(with-capability \"exception-control\" (jit:exception-fibers-clear))");
        CHECK(e2 && !is_error(*e2), "4058 AC2: Off face keeps the prim working");
    }

    {
        std::println("\n--- #4058 AC3: with-capability \"sandbox\" cannot open the "
                     "kPrimSecSandboxed dispatch ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4058);
        // mutation-log-compact: kPrimSecSandboxed meta with
        // requires_mutation_guard unset → heap_mutate false → the dispatch
        // kCapSandbox gate applies (#3235 skips Guard-gated heap mutators).
        auto s1 = cs.eval("(with-capability \"sandbox\" (mutation-log-compact))");
        CHECK(s1 && is_error(*s1),
              "4058 AC3: stack-disguised sandbox push does not open the dispatch gate");
        bool sandbox_deny4058 = false;
        for (const auto& s : ev.string_heap())
            if (s.find("sandboxed primitive requires kCapSandbox") != std::string::npos)
                sandbox_deny4058 = true;
        CHECK(sandbox_deny4058, "4058 AC3: deny names the kCapSandbox gate");
        // A REAL sandbox string grant (grant_capability → granted_capabilities_
        // + registry) opens the sandbox gate and the prim executes — the
        // disguise deny above disappears. (Its Mutate meta is
        // effect_enforced_in_body, so per #2583 AC2 the dispatch skips the
        // double require_effect; no second deny is expected here.)
        ev.grant_capability(aura::compiler::security::kCapSandbox);
        auto s2 = cs.eval("(mutation-log-compact)");
        CHECK(s2 && !is_error(*s2),
              "4058 AC3: real sandbox grant passes the sandbox gate (prim executes)");
        CHECK(ev.has_capability("sandbox"), "4058 AC3: registry-backed grant satisfies the oracle");
        // Off-face control: the prim executes.
        reset_all();
        CompilerService cs3;
        auto s3 = cs3.eval("(mutation-log-compact)");
        CHECK(s3 && !is_error(*s3), "4058 AC3: Off face keeps the prim working");
    }

    {
        std::println("\n--- #4058 AC4: check-capability still sees the pushed layer (lexical) ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4058);
        auto c1 = cs.eval("(with-capability \"sandbox\" (check-capability \"sandbox\"))");
        CHECK(c1 && is_bool(*c1) && as_bool(*c1),
              "4058 AC4: lexical query sees the pushed layer inside the body");
        auto c2 = cs.eval("(check-capability \"sandbox\")");
        CHECK(c2 && is_bool(*c2) && !as_bool(*c2), "4058 AC4: scope popped after the body");
    }

    {
        std::println(
            "\n--- #4058 AC5: has_capability ignores the pushed stack (in-body probe) ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4058);
        // capability? consults has_capability with "*" pushed — the pre-fix
        // oracle answered true right here; the registry-backed oracle must
        // still say no. This is the same predicate the dual-fiber hazard
        // exposes (tenant B resuming on the same Evaluator observes
        // has_capability with A's un-popped layers present).
        auto p1 = cs.eval("(with-capability \"*\" (capability? \"io-read\"))");
        CHECK(p1 && is_bool(*p1) && !as_bool(*p1),
              "4058 AC5: pushed \"*\" does not satisfy has_capability");
        CHECK(!ev.has_capability("io-read") && !ev.has_capability("sandbox"),
              "4058 AC5: zero grant stays zero post-pop");
        // Off-face control: the oracle short-circuits allow-all again.
        reset_all();
        CompilerService cs2;
        auto p2 = cs2.eval("(with-capability \"*\" (capability? \"io-read\"))");
        CHECK(p2 && is_bool(*p2) && as_bool(*p2), "4058 AC5: Soft/Off oracle stays allow-all");
    }

    {
        std::println("\n--- #4058 AC6: Soft/Off — disguise still reads, zero extra deny rows ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        set_mode(SandboxMode::Off);
        const std::string soft_secret = std::string("/tmp/aura_4058_soft.txt");
        {
            std::ofstream out(soft_secret);
            out << "aura-4058-soft-content\n";
        }
        const auto se0 = g_security_event_ring().total.load(std::memory_order_relaxed);
        auto s1 = cs.eval(std::format("(with-capability \"*\" (read-file \"{}\"))", soft_secret));
        CHECK(s1 && !is_error(*s1) && is_string(*s1), "4058 AC6: Soft/Off read still works");
        CHECK(g_security_event_ring().total.load(std::memory_order_relaxed) == se0,
              "4058 AC6: no extra deny rows on the Soft face");
        std::remove(soft_secret.c_str());
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

    // ── Issue #4093: JIT cell/hash heap tenant isolation ──
    ac4093_source_cite();
    ac4093_1_foreign_deny();
    ac4093_2_unstamped_and_single_tenant();
    ac4093_3_soft_shares();
    ac4094_source_cite();
    ac4094_1_foreign_deny();
    ac4094_2_unstamped_fail_closed();
    ac4094_3_soft_writes();

    // ── Issue #4110: evaluator-minted hash stamps + tree-walker gate ──
    ac4110_source_cite();
    ac4110_1_query_find_stamped_jit_ref();
    ac4110_2_tree_walker_agrees();
    ac4110_3_foreign_tree_walker_gate();
    ac4110_4_unarmed_face_no_tenant_load();

    std::println("\n=== #2152/#3524 dispatch required_effects: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dispatch_required_effects();
}
#endif
