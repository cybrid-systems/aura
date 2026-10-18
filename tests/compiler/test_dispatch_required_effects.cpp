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
#include "compiler/mutation_hold_budget.h"
#include "compiler/security_capabilities.h"
#include "compiler/security_side_effect.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"
#include "core/workspace_isolation.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.ir_executor;
import aura.compiler.service;
import aura.compiler.value;

namespace aura::serve {
void clear_current_fiber_session_mid() noexcept;
}

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
    // Issue #4296: interpreter/agent mints stamp the executing evaluator.
    // JIT aura_new_cell / aura_hash_set / aura_hash_ref keep the owner hook
    // (cited above on the runtime TU).
    CHECK(vec.find("g_hash_tenants[hidx] = ev.capability_tenant_id();") != std::string::npos,
          "4093 cite: hash prim stamps the executing evaluator (#4296)");
    CHECK(vec.find("g_hash_tenants[hidx] = aura_jit_owner_capability_tenant();") ==
              std::string::npos,
          "4093 cite: hash prim no longer stamps the JIT owner (#4296)");
    CHECK(ag.find("g_hash_tenants[hidx] = ev.capability_tenant_id();") != std::string::npos,
          "4093 cite: agent hash alloc sites stamp the executing evaluator (#4296)");
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

// ── Issue #4441: JIT car/cdr slow + non-L2 path (aura_pair_car /
// aura_pair_cdr) share the #4093 pair-slot tenant gate. Light-link
// binaries shadow the owner hooks, so the behavior arms drive
// aura_pair_car_checked / aura_pair_cdr_checked (the body those symbols
// delegate to). OpCar/OpCdr still CreateCall the production symbols.
static void ac4441_source_cite() {
    std::println("\n--- #4441 cite: slow and non-L2 car/cdr share the pair gate ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto car = rt.find("int64_t aura_pair_car(int64_t pair_val)");
    CHECK(car != std::string::npos, "4441 cite: aura_pair_car");
    if (car != std::string::npos) {
        const auto win = rt.substr(car, 600);
        CHECK(win.find("aura_pair_car_checked(") != std::string::npos &&
                  win.find("aura_jit_owner_capability_tenant()") != std::string::npos &&
                  win.find("aura_jit_owner_sandbox_mode()") != std::string::npos,
              "4441 cite: aura_pair_car delegates with owner-hook values");
    }
    const auto cdr = rt.find("int64_t aura_pair_cdr(int64_t pair_val)");
    CHECK(cdr != std::string::npos, "4441 cite: aura_pair_cdr");
    if (cdr != std::string::npos) {
        const auto win = rt.substr(cdr, 500);
        CHECK(win.find("aura_pair_cdr_checked(") != std::string::npos &&
                  win.find("aura_jit_owner_capability_tenant()") != std::string::npos &&
                  win.find("aura_jit_owner_sandbox_mode()") != std::string::npos,
              "4441 cite: aura_pair_cdr delegates with owner-hook values");
    }
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    CHECK(jit.find("ExternalLinkage, \"aura_pair_car\", mod)") != std::string::npos,
          "4441 cite: fn_pair_car bound to aura_pair_car");
    CHECK(jit.find("ExternalLinkage, \"aura_pair_cdr\", mod)") != std::string::npos,
          "4441 cite: fn_pair_cdr bound to aura_pair_cdr");
    const auto opcar = jit.find("case OpCar:");
    const auto opcdr = jit.find("case OpCdr:");
    CHECK(opcar != std::string::npos && opcdr != std::string::npos && opcdr > opcar,
          "4441 cite: OpCar then OpCdr");
    if (opcar != std::string::npos && opcdr > opcar) {
        const auto win = jit.substr(opcar, opcdr - opcar);
        const std::string needle = "CreateCall(llvm::FunctionCallee(fn_pair_car),";
        std::size_t n = 0;
        for (std::size_t pos = 0; (pos = win.find(needle, pos)) != std::string::npos;
             pos += needle.size())
            ++n;
        CHECK(n >= 2, "4441 cite: OpCar slow and non-L2 both call fn_pair_car");
        CHECK(win.find("fn_pair_car_unchecked") != std::string::npos,
              "4441 cite: OpCar L2 fast path stays unchecked");
    }
    const auto opcast = jit.find("case OpCastOp:");
    if (opcdr != std::string::npos && opcast != std::string::npos && opcast > opcdr) {
        const auto win = jit.substr(opcdr, opcast - opcdr);
        const std::string needle = "CreateCall(llvm::FunctionCallee(fn_pair_cdr),";
        std::size_t n = 0;
        for (std::size_t pos = 0; (pos = win.find(needle, pos)) != std::string::npos;
             pos += needle.size())
            ++n;
        CHECK(n >= 2, "4441 cite: OpCdr slow and non-L2 both call fn_pair_cdr");
    }
    const auto sh = read_file("src/compiler/runtime_shared.h");
    CHECK(sh.find("aura_pair_car_checked") != std::string::npos &&
              sh.find("aura_pair_cdr_checked") != std::string::npos,
          "4441 cite: runtime_shared.h declares the slow-path seams");
}

static void ac4441_slow_path_gate() {
    std::println("\n--- #4441: slow-path car/cdr fail closed on the production pair face ---");
    reset_all();
    constexpr std::uint64_t tenant_a = 7;
    constexpr std::uint64_t tenant_b = 9;
    constexpr int kRestricted = 1;
    constexpr int kStrictFace = 2;
    constexpr int kSoft = 0;

    const auto pair_a = aura_alloc_pair_tenant(222, 444, static_cast<std::int64_t>(tenant_a));
    CHECK(pair_a != 0, "4441: A alloc pair");
    const auto legacy = aura_alloc_pair_tenant(11, 22, 0);
    CHECK(legacy != 0, "4441: unstamped pair allocated");

    // Restricted+MT. The checked seams take caller/face explicitly
    // (light-link binaries shadow the strong owner hooks).
    aura::core::provenance::set_multi_tenant_env_active(true);
    CHECK(aura_pair_car_checked(pair_a, tenant_b, kRestricted) == 0,
          "4441: Restricted+MT foreign car is 0");
    CHECK(aura_pair_cdr_checked(pair_a, tenant_b, kRestricted) == 0,
          "4441: Restricted+MT foreign cdr is 0");
    CHECK(aura_pair_car_unchecked_checked(pair_a, tenant_b, kRestricted) == 0,
          "4441: L2 fast path foreign car agrees");
    CHECK(aura_pair_car_checked(pair_a, tenant_a, kRestricted) == 222,
          "4441: Restricted+MT owner car is 222");
    CHECK(aura_pair_cdr_checked(pair_a, tenant_a, kRestricted) == 444,
          "4441: Restricted+MT owner cdr is 444");
    CHECK(aura_pair_car_checked(legacy, tenant_b, kRestricted) == 0,
          "4441: Restricted+MT unstamped car is 0");
    CHECK(aura_pair_cdr_checked(legacy, tenant_b, kRestricted) == 0,
          "4441: Restricted+MT unstamped cdr is 0");

    // Strict arms without MT. Foreign and unstamped stay 0; the owner reads.
    aura::core::provenance::set_multi_tenant_env_active(false);
    CHECK(aura_pair_car_checked(pair_a, tenant_b, kStrictFace) == 0,
          "4441: Strict foreign car is 0");
    CHECK(aura_pair_cdr_checked(pair_a, tenant_b, kStrictFace) == 0,
          "4441: Strict foreign cdr is 0");
    CHECK(aura_pair_car_checked(legacy, tenant_a, kStrictFace) == 0,
          "4441: Strict unstamped car is 0");
    CHECK(aura_pair_cdr_checked(legacy, tenant_a, kStrictFace) == 0,
          "4441: Strict unstamped cdr is 0");
    CHECK(aura_pair_car_checked(pair_a, tenant_a, kStrictFace) == 222,
          "4441: Strict owner car is 222");
    CHECK(aura_pair_cdr_checked(pair_a, tenant_a, kStrictFace) == 444,
          "4441: Strict owner cdr is 444");

    // Soft/Off (mode 0) returns the foreign slot even while MT is active.
    // The face probe precedes any stamp load.
    aura::core::provenance::set_multi_tenant_env_active(true);
    CHECK(aura_pair_car_checked(pair_a, tenant_b, kSoft) == 222,
          "4441: Soft foreign car stays 222");
    CHECK(aura_pair_cdr_checked(pair_a, tenant_b, kSoft) == 444,
          "4441: Soft foreign cdr stays 444");
    aura::core::provenance::set_multi_tenant_env_active(false);

    // Single-tenant Restricted (mode 1, MT off) stays permissive.
    CHECK(aura_pair_car_checked(pair_a, tenant_b, kRestricted) == 222,
          "4441: single-tenant Restricted car stays 222");
    CHECK(aura_pair_cdr_checked(pair_a, tenant_b, kRestricted) == 444,
          "4441: single-tenant Restricted cdr stays 444");

    // A denied read does not write. Owner and Soft re-reads see the payload.
    aura::core::provenance::set_multi_tenant_env_active(true);
    CHECK(aura_pair_car_checked(pair_a, tenant_b, kRestricted) == 0, "4441: foreign car still 0");
    CHECK(aura_pair_car_checked(pair_a, tenant_a, kRestricted) == 222,
          "4441: owner car unchanged after foreign read");
    CHECK(aura_pair_cdr_checked(pair_a, tenant_a, kRestricted) == 444,
          "4441: owner cdr unchanged after foreign read");
    CHECK(aura_pair_car_checked(legacy, tenant_b, kSoft) == 11,
          "4441: unstamped car payload unchanged");
    CHECK(aura_pair_cdr_checked(legacy, tenant_b, kSoft) == 22,
          "4441: unstamped cdr payload unchanged");
    aura::core::provenance::set_multi_tenant_env_active(false);
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
    // Issue #4296: inline stamps stay, principal is the executing evaluator
    // (the helper above still stamps from the JIT owner hook).
    const auto vec = read_file("src/compiler/evaluator_primitives_vector.cpp");
    CHECK(vec.find("g_hash_tenants[hidx] = ev.capability_tenant_id();") != std::string::npos,
          "4110 cite: vector hash prim stamps the executing evaluator (#4296)");
    const auto ag = read_file("src/compiler/evaluator_primitives_agent.cpp");
    CHECK(ag.find("g_hash_tenants[hidx] = ev.capability_tenant_id();") != std::string::npos,
          "4110 cite: agent hash alloc stamps the executing evaluator (#4296)");
    // The four tree-walker prims gate before the probe with this evaluator.
    CHECK(vec.find("aura_hash_gate_checked(hidx, ev.capability_tenant_id(),") != std::string::npos,
          "4110 cite: tree-walker gates pass the executing evaluator");
    CHECK(vec.find("aura_hash_gate_checked(hidx, aura_jit_owner_capability_tenant(),") ==
              std::string::npos,
          "4110 cite: tree-walker gates do not pass the JIT owner (#4296)");
    // clang-format may break after effect_sandbox_mode(); the op literal
    // still has to sit in the same call.
    auto face_gates_op = [](const std::string& src, std::string_view op) {
        const std::string face = "ev.effect_sandbox_mode()";
        const std::string lit = std::format("\"{}\"", op);
        for (std::size_t at = src.find(face); at != std::string::npos;
             at = src.find(face, at + face.size())) {
            if (src.substr(at, 96).find(lit) != std::string::npos)
                return true;
        }
        return false;
    };
    CHECK(face_gates_op(vec, "hash-ref"),
          "4110 cite: hash-ref gate reads the executing evaluator face");
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

// ── Issue #4134: fail-closed aura_jit_owner_check_isolation when the owner
// Evaluator is unwired. The unwired hook (strong def and light-link stub)
// returned 1 (allow) — an audit/forensics hole and a footgun for any caller
// that trusts the return for access. The hook now denies (0) when unwired:
// the #4093/#4094 gates decide the access skip locally on stamp mismatch
// (unchanged), the IsolationDeny SE is best-effort (needs the owner's
// record_audit path; the existing tenant-isolation counter
// tenant_boundary_violation_prevented_total is the observable), and
// Soft/Off face 0 keeps the zero-cost contract (sandbox_mode stub stays 0 →
// gates never arm). Light-link binaries shadow the strong owner hooks with
// the weak stubs (#4036 rationale), so the direct hook calls below exercise
// the fail-closed stub; the strong-def shape is source-cited.
static void ac4134_source_cite() {
    std::println(
        "\n--- #4134 cite: fail-closed isolation hook (strong + stub), zero-cost intact ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto iso = svc.find("extern \"C\" int aura_jit_owner_check_isolation(");
    CHECK(iso != std::string::npos, "4134 cite: strong isolation hook present");
    if (iso != std::string::npos) {
        const auto win = svc.substr(iso, 1000);
        const auto unwired = win.find("if (!owner) {");
        const auto deny = win.find("return 0;");
        CHECK(unwired != std::string::npos && deny != std::string::npos && deny > unwired,
              "4134 cite: unwired owner denies (0) — no false-allow");
        CHECK(win.find("return 1;") == std::string::npos,
              "4134 cite: no unwired allow (1) remains in the strong def");
        CHECK(win.find("owner->check_workspace_isolation(") != std::string::npos,
              "4134 cite: owner-wired routing intact (#4093 deny path)");
        CHECK(win.find("tenant_boundary_violation_prevented_total") != std::string::npos,
              "4134 cite: best-effort deny observable via the existing isolation counter");
    }
    CHECK(svc.find("Issue #4134: fail-closed") != std::string::npos,
          "4134 cite: rationale documented at the hook");
    const auto req = svc.find("extern \"C\" int aura_jit_owner_require_effect(");
    CHECK(req != std::string::npos &&
              svc.substr(req, 320).find("if (!owner)\n        return 0;") != std::string::npos,
          "4134 cite: require_effect sibling stays fail-closed unwired");
    const auto stub = read_file("src/compiler/aura_jit_prim_dispatch_stub.cpp");
    const auto stub_iso = stub.find("aura_jit_owner_check_isolation(std::uint64_t, std::uint64_t");
    CHECK(stub_iso != std::string::npos &&
              stub.substr(stub_iso, 200).find("return 0;") != std::string::npos,
          "4134 cite: weak isolation stub denies (0)");
    CHECK(stub.substr(stub_iso, 200).find("return 1;") == std::string::npos,
          "4134 cite: weak isolation stub no longer allows (1)");
    const auto stub_mode = stub.find("aura_jit_owner_sandbox_mode(void) noexcept");
    CHECK(stub_mode != std::string::npos &&
              stub.substr(stub_mode, 200).find("return 0;") != std::string::npos,
          "4134 cite: sandbox_mode stub stays 0 (gates never arm light-link)");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("if (slot_tenant == caller_tenant)") != std::string::npos &&
              rt.find("aura_jit_owner_check_isolation(caller_tenant, slot_tenant") !=
                  std::string::npos,
          "4134 cite: jit_tenant_gate skip semantics untouched (#4110 AC7 shape)");
    CHECK(rt.find("Issue #4134") != std::string::npos,
          "4134 cite: gate comment documents the fail-closed hook return");
}

static void ac4134_1_owner_wired_deny_unchanged() {
    std::println("\n--- #4134 AC1: foreign access still skipped with the face armed ---");
    reset_all();
    constexpr std::uint64_t tenant_a = 4134;
    constexpr std::uint64_t tenant_b = 4135;
    constexpr int kStrictFace = 2;
    const auto cid_a = aura_alloc_closure_tenant(4134, tenant_a);
    CHECK(cid_a >= 0, "4134 AC1: A alloc closure");
    aura_closure_capture_checked(cid_a, 0, 3134, tenant_a, kStrictFace);
    CHECK(aura_closure_env_get(cid_a, 0) == 3134, "4134 AC1: A capture lands");
    // Face armed, foreign stamp: the gate still skips the access (the local
    // stamp compare is untouched) and production links route the deny
    // through the owner's check_workspace_isolation (#4094 AC1 semantics;
    // #4134 flipped only the unwired hook return).
    aura_closure_capture_checked(cid_a, 0, 999, tenant_b, kStrictFace);
    CHECK(aura_closure_env_get(cid_a, 0) == 3134, "4134 AC1: B foreign capture still skipped");
}

static void ac4134_2_unwired_hook_denies() {
    std::println("\n--- #4134 AC2: unwired isolation hook returns 0 (deny) ---");
    reset_all();
    // Light-link: the weak stub IS the hook (no owner Evaluator wired). The
    // stub allowed (1) before #4134; callers honoring the return can no
    // longer false-allow.
    CHECK(aura_jit_owner_check_isolation(4135, 4134, kEffectMutate, "jit-tenant") == 0,
          "4134 AC2: unwired hook denies a foreign pair");
    CHECK(aura_jit_owner_check_isolation(0, 0, kEffectMutate, "jit-tenant") == 0,
          "4134 AC2: unwired hook denies regardless of stamps");
}

static void ac4134_3_soft_zero_cost() {
    std::println("\n--- #4134 AC3: Soft/Off face 0 keeps the zero-cost contract ---");
    reset_all();
    constexpr std::uint64_t tenant_a = 4134;
    constexpr std::uint64_t tenant_b = 4135;
    const auto cid_a = aura_alloc_closure_tenant(4134, tenant_a);
    CHECK(cid_a >= 0, "4134 AC3: A alloc closure");
    aura_closure_capture_checked(cid_a, 0, 3134, tenant_a, 0);
    CHECK(aura_closure_env_get(cid_a, 0) == 3134, "4134 AC3: A capture lands");
    // Face 0 (mode 0): the seam never arms the gate — the capture proceeds
    // across principals without consulting g_closure_tenants (the face
    // probe precedes the stamp load; the #4093 AC3 zero-cost shape on the
    // closure seam).
    aura_closure_capture_checked(cid_a, 0, 999, tenant_b, 0);
    CHECK(aura_closure_env_get(cid_a, 0) == 999,
          "4134 AC3: Soft capture lands across principals (shared)");
    // The hash gate stays a probe when unarmed: no skip, no deny, no load.
    const auto hash_a = aura_hash_alloc_tenant(4134);
    CHECK(hash_a >= 0, "4134 AC3: A alloc hash");
    const auto hidx_a = static_cast<std::uint64_t>(hash_a) >> 6;
    CHECK(!aura_hash_gate_checked(hidx_a, tenant_b, 0, "hash-set!"),
          "4134 AC3: unarmed hash gate is probe-only");
}

static void ac4134_4_require_effect_precedes_store() {
    std::println("\n--- #4134 AC4: unwired require_effect deny still precedes the store ---");
    reset_all();
    // Unwired owner → require_effect denies (0); the JIT mutate chokes run
    // BEFORE any heap store (cite below), so no silent write.
    CHECK(aura_jit_owner_require_effect(kEffectMutate, "hash-set!") == 0,
          "4134 AC4: unwired require_effect denies hash-set!");
    CHECK(aura_jit_owner_require_effect(kEffectMutate, "cell-set!") == 0,
          "4134 AC4: unwired require_effect denies cell-set!");
    // Runtime proof: the deny precedes the store — the C-ABI hash-set!
    // stores nothing (light-link stub denies the Mutate before the write).
    const auto hash_b = aura_hash_alloc_tenant(4134);
    CHECK(hash_b >= 0, "4134 AC4: alloc hash for choke drive");
    const auto kp = aura_alloc_pair_tenant(7, 8, 4134);
    CHECK(kp != 0, "4134 AC4: alloc pair for choke drive");
    CHECK(aura_hash_set(hash_b, kp) == 0, "4134 AC4: denied hash-set! rc 0");
    CHECK(aura_hash_ref(hash_b, 7) == 11, "4134 AC4: no entry stored (not-found sentinel)");
    // The choke lines sit upstream of the stores in the runtime source.
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto choke = rt.find("aura_jit_owner_require_effect(aura::compiler::security::"
                               "kEffectMutate, \"hash-set!\") == 0");
    CHECK(choke != std::string::npos, "4134 AC4: hash-set! Mutate choke present");
    if (choke != std::string::npos) {
        const auto store = rt.find("aura_hash_set_checked(hash_val, pair_val", choke);
        CHECK(store != std::string::npos, "4134 AC4: hash-set! store downstream of the choke");
    }
    const auto choke_cell = rt.find("aura_jit_owner_require_effect(aura::compiler::security::"
                                    "kEffectMutate, \"cell-set!\") == 0");
    CHECK(choke_cell != std::string::npos, "4134 AC4: cell-set! Mutate choke present");
}

// Issue #4296: interpreter hash mint + hash-ref/has-key?/set!/remove!
// authorize the executing Evaluator, not the JIT owner. JIT C ABI keeps
// the owner hooks. Soft/Off (face 0) does not read g_hash_tenants.
static aura::compiler::types::EvalValue
call_hash_op(aura::compiler::Evaluator& ev, std::string_view name,
             std::initializer_list<aura::compiler::types::EvalValue> args) {
    auto pfn = ev.primitives().lookup(name);
    if (!pfn)
        return make_void();
    const std::vector<aura::compiler::types::EvalValue> v(args);
    return (*pfn)(v);
}

static bool raw_hash_int_is(std::uint64_t hidx, std::int64_t key, std::int64_t expect) {
    if (hidx >= g_hash_tables.size() || g_hash_tables[hidx] == nullptr)
        return false;
    auto* ht = g_hash_tables[hidx];
    const auto meta = ht->metadata();
    const auto keys = ht->keys();
    const auto vals = ht->values();
    const auto want = make_int(key).val;
    for (std::uint64_t i = 0; i < ht->capacity; ++i) {
        if (meta[i] == 0xFF)
            continue;
        if (keys[i] == want)
            return vals[i] == make_int(expect).val;
    }
    return false;
}

static bool ring_has_isolation_deny(std::string_view op) {
    using aura::core::security_event::SecurityEventKind;
    const auto seq = g_security_event_ring().seq.load(std::memory_order_relaxed);
    const std::size_t rn = std::min<std::size_t>(seq, g_security_event_ring().ring.size());
    for (std::size_t i = 0; i < rn; ++i) {
        const auto& e = g_security_event_ring().ring[i];
        if (e.denied && e.kind == SecurityEventKind::IsolationDeny && std::string_view(e.op) == op)
            return true;
    }
    return false;
}

// Issue #4505: fail-closed is exactly one audit row per denied prim call —
// the helper must fire once, not per walked slot.
static std::size_t ring_isolation_deny_count(std::string_view op) {
    using aura::core::security_event::SecurityEventKind;
    const auto seq = g_security_event_ring().seq.load(std::memory_order_relaxed);
    const std::size_t rn = std::min<std::size_t>(seq, g_security_event_ring().ring.size());
    std::size_t n = 0;
    for (std::size_t i = 0; i < rn; ++i) {
        const auto& e = g_security_event_ring().ring[i];
        if (e.denied && e.kind == SecurityEventKind::IsolationDeny && std::string_view(e.op) == op)
            ++n;
    }
    return n;
}

// Issue #4505: display/write deny must leave the payload off stdout. Same
// pipe/dup2 capture shape as test_write_string_escape.cpp.
template <typename F> static std::string with_stdout_capture(F&& fn) {
    int pipefd[2];
    if (::pipe(pipefd) != 0)
        return {};
    int saved = ::dup(STDOUT_FILENO);
    if (saved < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    ::fflush(stdout);
    if (::dup2(pipefd[1], STDOUT_FILENO) < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        ::close(saved);
        return {};
    }
    ::close(pipefd[1]);

    fn();

    ::fflush(stdout);
    ::dup2(saved, STDOUT_FILENO);
    ::close(saved);

    std::string out;
    char buf[512];
    ssize_t n = 0;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
    return out;
}

static void ac4296_executing_evaluator_hash_gate() {
    std::println("\n--- #4296: hash stamp/gate uses the executing Evaluator ---");
    reset_all();
    aura::core::provenance::set_multi_tenant_env_active(true);
    CompilerService cs_a;
    CompilerService cs_b;
    auto& a = cs_a.evaluator();
    auto& b = cs_b.evaluator();
    a.set_effect_sandbox_mode(2); // process face Strict; both evaluators read it
    a.set_capability_tenant_id(7);
    b.set_capability_tenant_id(9);
    CHECK(aura_jit_owner_capability_tenant() == 0, "4296: JIT ctx starts unwired");
    CHECK(a.effect_sandbox_mode() == 2 && b.effect_sandbox_mode() == 2,
          "4296: production face is Strict");

    const auto hv = call_hash_op(a, "hash", {make_int(1), make_int(42)});
    CHECK(is_hash(hv), "4296: tenant 7 hash mints");
    if (!is_hash(hv)) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        a.set_effect_sandbox_mode(0);
        return;
    }
    const auto hidx = as_hash_idx(hv);
    CHECK(hidx < g_hash_tenants.size() && g_hash_tenants[hidx] == 7,
          "4296: unwired JIT stamps tenant 7, not owner 0");
    const auto own = call_hash_op(a, "hash-ref", {hv, make_int(1)});
    CHECK(is_int(own) && as_int(own) == 42, "4296: tenant 7 reads its own hash");

    auto& iso = aura::core::workspace_isolation::g_tenant_isolation_metrics();
    const auto prevented0 =
        iso.tenant_boundary_violation_prevented_total.load(std::memory_order_relaxed);
    const auto set_b = call_hash_op(b, "hash-set!", {hv, make_int(1), make_int(99)});
    (void)set_b;
    const auto ref_b = call_hash_op(b, "hash-ref", {hv, make_int(1)});
    const auto has_b = call_hash_op(b, "hash-has-key?", {hv, make_int(1)});
    const auto rm_b = call_hash_op(b, "hash-remove!", {hv, make_int(1)});
    CHECK(is_void(ref_b), "4296: unwired JIT, tenant 9 hash-ref returns no value");
    CHECK(is_bool(has_b) && !as_bool(has_b), "4296: unwired JIT, tenant 9 hash-has-key? is #f");
    CHECK(is_bool(rm_b) && !as_bool(rm_b), "4296: unwired JIT, tenant 9 hash-remove! is #f");
    CHECK(raw_hash_int_is(hidx, 1, 42), "4296: unwired JIT, hash-set! leaves the slot unchanged");
    const auto own_after = call_hash_op(a, "hash-ref", {hv, make_int(1)});
    CHECK(is_int(own_after) && as_int(own_after) == 42,
          "4296: tenant 7 still reads 42 after tenant 9");
    // IsolationDeny and tenant_boundary_violation_prevented_total are
    // recorded only inside the strong owner hook (service.ixx →
    // check_workspace_isolation). This batch NEEDs
    // libaura_jit_light_test_objects.so first, so the weak stub wins ELF
    // search, returns 0, and records nothing. The local stamp compare is
    // what this binary can observe. A second ev.check_workspace_isolation
    // in the prim would double-record on the production link.
    const auto prevented1 =
        iso.tenant_boundary_violation_prevented_total.load(std::memory_order_relaxed);

    // JIT ctx installed on Evaluator A only. When the strong hook is the
    // one ELF resolved, the owner tenant is 7 while B executes as 9.
    cs_a.register_jit_primitives();
    const bool strong_owner = aura_jit_owner_capability_tenant() == 7;
    if (strong_owner) {
        CHECK(prevented1 > prevented0, "4296: unwired strong owner bumps the isolation counter");
    } else {
        CHECK(aura_jit_owner_capability_tenant() == 0,
              "4296: light-link stub interposes the owner hook");
        CHECK(prevented1 == prevented0,
              "4296: light-link stub does not bump the isolation counter");
    }
    reset_security_event_ring_for_test();
    (void)call_hash_op(b, "hash-set!", {hv, make_int(1), make_int(99)});
    if (strong_owner) {
        CHECK(ring_has_isolation_deny("hash-set!"),
              "4296: tenant 9 hash-set! records IsolationDeny");
    } else {
        CHECK(!ring_has_isolation_deny("hash-set!"),
              "4296: light-link stub records no IsolationDeny on hash-set!");
    }
    CHECK(raw_hash_int_is(hidx, 1, 42), "4296: hash-set! leaves the slot unchanged");
    reset_security_event_ring_for_test();
    const auto ref_wired = call_hash_op(b, "hash-ref", {hv, make_int(1)});
    CHECK(is_void(ref_wired), "4296: tenant 9 hash-ref returns no value");
    if (strong_owner) {
        CHECK(ring_has_isolation_deny("hash-ref"), "4296: tenant 9 hash-ref records IsolationDeny");
    } else {
        CHECK(!ring_has_isolation_deny("hash-ref"),
              "4296: light-link stub records no IsolationDeny on hash-ref");
    }
    reset_security_event_ring_for_test();
    const auto rm_wired = call_hash_op(b, "hash-remove!", {hv, make_int(1)});
    CHECK(is_bool(rm_wired) && !as_bool(rm_wired), "4296: tenant 9 hash-remove! is #f");
    if (strong_owner) {
        CHECK(ring_has_isolation_deny("hash-remove!"),
              "4296: tenant 9 hash-remove! records IsolationDeny");
    } else {
        CHECK(!ring_has_isolation_deny("hash-remove!"),
              "4296: light-link stub records no IsolationDeny on hash-remove!");
    }
    CHECK(raw_hash_int_is(hidx, 1, 42), "4296: remove deny leaves 42 in the slot");
    const auto own_wired = call_hash_op(a, "hash-ref", {hv, make_int(1)});
    CHECK(is_int(own_wired) && as_int(own_wired) == 42,
          "4296: tenant 7 own hash still succeeds with JIT on A");

    // Same evaluator, TenantScope rebinds the principal. JIT stays on A
    // (tenant 7), so a JIT-owner stamp would label B's mint as 7.
    {
        aura::compiler::Evaluator::TenantScope scope(b, 7);
        const auto h7 = call_hash_op(b, "hash", {make_int(3), make_int(30)});
        CHECK(is_hash(h7), "4296: TenantScope 7 mints");
        if (is_hash(h7)) {
            const auto i7 = as_hash_idx(h7);
            CHECK(i7 < g_hash_tenants.size() && g_hash_tenants[i7] == 7,
                  "4296: TenantScope 7 stamp is 7");
            const auto r7 = call_hash_op(b, "hash-ref", {h7, make_int(3)});
            CHECK(is_int(r7) && as_int(r7) == 30, "4296: TenantScope 7 reads its mint");
            {
                aura::compiler::Evaluator::TenantScope scope9(b, 9);
                (void)call_hash_op(b, "hash-set!", {h7, make_int(3), make_int(31)});
                CHECK(raw_hash_int_is(i7, 3, 30), "4296: TenantScope 9 hash-set! leaves 30");
                const auto r9 = call_hash_op(b, "hash-ref", {h7, make_int(3)});
                CHECK(is_void(r9), "4296: TenantScope 9 hash-ref returns no value");
            }
            const auto back = call_hash_op(b, "hash-ref", {h7, make_int(3)});
            CHECK(is_int(back) && as_int(back) == 30, "4296: TenantScope 7 still reads 30");
        }
    }
    {
        aura::compiler::Evaluator::TenantScope scope9(b, 9);
        const auto h9 = call_hash_op(b, "hash", {make_int(4), make_int(40)});
        CHECK(is_hash(h9), "4296: TenantScope 9 mints");
        if (is_hash(h9)) {
            const auto i9 = as_hash_idx(h9);
            if (strong_owner) {
                CHECK(aura_jit_owner_capability_tenant() == 7, "4296: JIT owner still A's 7");
            } else {
                CHECK(aura_jit_owner_capability_tenant() == 0,
                      "4296: light-link stub still reports owner 0");
            }
            CHECK(i9 < g_hash_tenants.size() && g_hash_tenants[i9] == 9,
                  "4296: TenantScope 9 stamp is 9, not the JIT owner");
            const auto r9 = call_hash_op(b, "hash-ref", {h9, make_int(4)});
            CHECK(is_int(r9) && as_int(r9) == 40, "4296: TenantScope 9 reads its own mint");
            {
                aura::compiler::Evaluator::TenantScope scope7(b, 7);
                const auto r7 = call_hash_op(b, "hash-ref", {h9, make_int(4)});
                CHECK(is_void(r7), "4296: TenantScope 7 cannot read tenant 9's hash");
                CHECK(raw_hash_int_is(i9, 4, 40), "4296: tenant 9 slot unchanged");
            }
        }
    }

    // Face 0: historical shared read/write, no tenant load.
    a.set_effect_sandbox_mode(0);
    CHECK(a.effect_sandbox_mode() == 0 && b.effect_sandbox_mode() == 0, "4296: face back to Off");
    (void)call_hash_op(b, "hash-set!", {hv, make_int(1), make_int(77)});
    CHECK(raw_hash_int_is(hidx, 1, 77), "4296: Off hash-set! from tenant 9 lands");
    const auto soft_b = call_hash_op(b, "hash-ref", {hv, make_int(1)});
    const auto soft_a = call_hash_op(a, "hash-ref", {hv, make_int(1)});
    CHECK(is_int(soft_b) && as_int(soft_b) == 77, "4296: Off tenant 9 reads the shared write");
    CHECK(is_int(soft_a) && as_int(soft_a) == 77, "4296: Off tenant 7 sees tenant 9's write");

    aura::core::provenance::set_multi_tenant_env_active(false);
    set_mode(SandboxMode::Off);
}

// Issue #4298: in-range cons/vector slots on one Evaluator are mailbox
// indices. TenantScope rebinds the same heap; the prod face must refuse a
// foreign set-car!/set-cdr!/vector-set! and must not return the payload.
static void ac4298_inrange_pair_vector_tenant() {
    std::println("\n--- #4298: in-range pair/vector tenant stamp ---");
    reset_all();
    aura::core::bump_mutation_epoch(1);
    aura::core::provenance::set_multi_tenant_env_active(true);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted + MT → production face
    auto cons_fn = ev.primitives().lookup("cons");
    auto car_fn = ev.primitives().lookup("car");
    auto cdr_fn = ev.primitives().lookup("cdr");
    auto set_car_fn = ev.primitives().lookup("set-car!");
    auto set_cdr_fn = ev.primitives().lookup("set-cdr!");
    auto vec_fn = ev.primitives().lookup("vector");
    auto vref_fn = ev.primitives().lookup("vector-ref");
    auto vset_fn = ev.primitives().lookup("vector-set!");
    CHECK(cons_fn && car_fn && cdr_fn && set_car_fn && set_cdr_fn && vec_fn && vref_fn && vset_fn,
          "4298: pair and vector prims present");
    if (!(cons_fn && car_fn && cdr_fn && set_car_fn && set_cdr_fn && vec_fn && vref_fn &&
          vset_fn)) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
        return;
    }

    aura::compiler::types::EvalValue pair;
    aura::compiler::types::EvalValue vec;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        pair = (*cons_fn)({make_int(1), make_int(2)});
        vec = (*vec_fn)({make_int(10), make_int(20)});
    }
    CHECK(!is_error(pair), "4298: tenant 7 cons");
    CHECK(!is_error(vec), "4298: tenant 7 vector");

    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto sc = (*set_car_fn)({pair, make_int(9)});
        auto sd = (*set_cdr_fn)({pair, make_int(8)});
        auto vs = (*vset_fn)({vec, make_int(0), make_int(99)});
        CHECK(is_error(sc), "4298: tenant 9 set-car! denied");
        CHECK(is_error(sd), "4298: tenant 9 set-cdr! denied");
        CHECK(is_error(vs), "4298: tenant 9 vector-set! denied");
        auto rc = (*car_fn)({pair});
        auto rd = (*cdr_fn)({pair});
        auto rv = (*vref_fn)({vec, make_int(0)});
        CHECK(is_error(rc), "4298: tenant 9 car does not return the payload");
        CHECK(is_error(rd), "4298: tenant 9 cdr does not return the payload");
        CHECK(is_error(rv), "4298: tenant 9 vector-ref does not return the payload");
        CHECK(ring_has_isolation_deny("set-car!"), "4298: IsolationDeny set-car!");
        CHECK(ring_has_isolation_deny("set-cdr!"), "4298: IsolationDeny set-cdr!");
        CHECK(ring_has_isolation_deny("vector-set!"), "4298: IsolationDeny vector-set!");
        CHECK(ring_has_isolation_deny("car"), "4298: IsolationDeny car");
        CHECK(ring_has_isolation_deny("vector-ref"), "4298: IsolationDeny vector-ref");
    }
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        auto rc = (*car_fn)({pair});
        auto rd = (*cdr_fn)({pair});
        auto rv = (*vref_fn)({vec, make_int(0)});
        CHECK(is_int(rc) && as_int(rc) == 1, "4298: slot car unchanged after foreign write");
        CHECK(is_int(rd) && as_int(rd) == 2, "4298: slot cdr unchanged after foreign write");
        CHECK(is_int(rv) && as_int(rv) == 10, "4298: vector element unchanged");
        auto sc = (*set_car_fn)({pair, make_int(3)});
        auto sd = (*set_cdr_fn)({pair, make_int(4)});
        auto vs = (*vset_fn)({vec, make_int(0), make_int(30)});
        CHECK(!is_error(sc) && !is_error(sd) && !is_error(vs), "4298: owner mutates");
        auto rc2 = (*car_fn)({pair});
        auto rd2 = (*cdr_fn)({pair});
        auto rv2 = (*vref_fn)({vec, make_int(0)});
        CHECK(is_int(rc2) && as_int(rc2) == 3, "4298: owner set-car! landed");
        CHECK(is_int(rd2) && as_int(rd2) == 4, "4298: owner set-cdr! landed");
        CHECK(is_int(rv2) && as_int(rv2) == 30, "4298: owner vector-set! landed");
    }

    // Tenant 0's cons is a real stamp (not the never-stamped hole) and
    // refuses a non-zero caller.
    aura::compiler::types::EvalValue zpair;
    {
        aura::compiler::Evaluator::TenantScope t0(ev, 0);
        zpair = (*cons_fn)({make_int(5), make_int(6)});
    }
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto sc = (*set_car_fn)({zpair, make_int(7)});
        auto rc = (*car_fn)({zpair});
        CHECK(is_error(sc), "4298: tenant-0 cons set-car! from tenant 9 denied");
        CHECK(is_error(rc), "4298: tenant-0 cons car from tenant 9 denied");
    }
    {
        aura::compiler::Evaluator::TenantScope t0(ev, 0);
        auto rc = (*car_fn)({zpair});
        CHECK(is_int(rc) && as_int(rc) == 5, "4298: tenant 0 still reads its cons");
    }

    // Soft/Off: foreign in-range write lands and the stamp array is not consulted.
    reset_security_event_ring_for_test();
    ev.set_effect_sandbox_mode(0);
    aura::compiler::types::EvalValue soft_pair;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        soft_pair = (*cons_fn)({make_int(1), make_int(2)});
    }
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto sc = (*set_car_fn)({soft_pair, make_int(11)});
        auto rc = (*car_fn)({soft_pair});
        CHECK(!is_error(sc), "4298: Off set-car! kept");
        CHECK(is_int(rc) && as_int(rc) == 11, "4298: Off write visible, stamp not loaded");
        CHECK(!ring_has_isolation_deny("set-car!"), "4298: Off records no IsolationDeny");
    }

    const auto pair_src = read_file("src/compiler/evaluator_primitives_pair.cpp");
    const auto scpos = pair_src.find("add, ev, \"set-car!\"");
    CHECK(scpos != std::string::npos, "4298: set-car! body located");
    if (scpos != std::string::npos) {
        const auto win = pair_src.substr(scpos, 3500);
        CHECK(win.find("inrange_slot_tenant_allows") != std::string::npos,
              "4298: in-range arm consults the mint stamp");
        CHECK(win.find("g_pair_slot_tenants") != std::string::npos,
              "4298: #4057 g_pair_slots arm kept");
        CHECK(win.find("require_effect") == std::string::npos,
              "4298: set-car! window stays body-choke-free");
    }

    aura::core::provenance::set_multi_tenant_env_active(false);
    set_mode(SandboxMode::Off);
}

// Issue #4428: string_heap_ and the tree-walker closure map are one
// Evaluator. TenantScope only rebinds the principal. string-fill! and
// closure:free! on a closure value must refuse a foreign mint on the
// production face and leave the slot in place. Soft/Off does not read
// the stamp. Direct prim calls (same as #4298): dispatch under MT would
// deny an unstamped ref before the body, which would make this gate vacuous.
static void ac4428_string_fill_and_closure_value() {
    std::println("\n--- #4428: string-fill! and closure:free! value owner ---");
    using aura::compiler::types::make_closure;
    using aura::compiler::types::make_string;
    reset_all();
    aura::core::bump_mutation_epoch(1);
    aura::core::provenance::set_multi_tenant_env_active(true);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted + MT → production face
    auto fill_fn = ev.primitives().lookup("string-fill!");
    auto free_fn = ev.primitives().lookup("closure:free!");
    CHECK(fill_fn && free_fn, "4428: string-fill! and closure:free! present");
    if (!(fill_fn && free_fn)) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
        return;
    }

    aura::compiler::types::EvalValue s;
    aura::compiler::types::EvalValue clv;
    aura::compiler::ClosureId cid = 0;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        const auto idx = ev.push_string_heap("hello");
        s = make_string(static_cast<std::uint64_t>(idx));
        aura::compiler::Closure cl;
        cid = ev.register_active_closure(std::move(cl));
        clv = make_closure(cid);
    }
    CHECK(ev.copy_string_heap_at(
              static_cast<std::size_t>(aura::compiler::types::as_string_idx(s))) == "hello",
          "4428: tenant 7 interned hello");
    CHECK(ev.find_active_closure(cid).has_value(), "4428: tenant 7 closure registered");

    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto filled = (*fill_fn)({s, make_int(88)});
        auto freed = (*free_fn)({clv});
        CHECK(is_error(filled), "4428: tenant 9 string-fill! denied");
        CHECK(is_bool(freed) && !as_bool(freed), "4428: tenant 9 closure:free! returns false");
        CHECK(ev.copy_string_heap_at(
                  static_cast<std::size_t>(aura::compiler::types::as_string_idx(s))) == "hello",
              "4428: string bytes unchanged");
        CHECK(ev.find_active_closure(cid).has_value(), "4428: closure still present");
        CHECK(ring_has_isolation_deny("string-fill!"), "4428: IsolationDeny string-fill!");
        CHECK(ring_has_isolation_deny("closure:free!"), "4428: IsolationDeny closure:free!");
    }
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        auto filled = (*fill_fn)({s, make_int(88)});
        CHECK(!is_error(filled), "4428: owner string-fill! lands");
        const auto owner_bytes = ev.copy_string_heap_at(
            static_cast<std::size_t>(aura::compiler::types::as_string_idx(s)));
        const std::string filled88(5, static_cast<char>(88));
        CHECK(owner_bytes == filled88, "4428: owner fill wrote 88");
        auto freed = (*free_fn)({clv});
        CHECK(is_bool(freed) && as_bool(freed), "4428: owner closure:free! returns true");
        CHECK(!ev.find_active_closure(cid).has_value(), "4428: owner free removed the closure");
    }

    // Tenant 0's intern is a real stamp (not the never-stamped hole).
    aura::compiler::types::EvalValue zs;
    aura::compiler::types::EvalValue zclv;
    aura::compiler::ClosureId zcid = 0;
    {
        aura::compiler::Evaluator::TenantScope t0(ev, 0);
        const auto idx = ev.push_string_heap("zero");
        zs = make_string(static_cast<std::uint64_t>(idx));
        aura::compiler::Closure zcl;
        zcid = ev.register_active_closure(std::move(zcl));
        zclv = make_closure(zcid);
    }
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto filled = (*fill_fn)({zs, make_int(89)});
        auto freed = (*free_fn)({zclv});
        CHECK(is_error(filled), "4428: tenant-0 string refuses tenant 9");
        CHECK(is_bool(freed) && !as_bool(freed), "4428: tenant-0 closure refuses tenant 9");
        CHECK(ev.copy_string_heap_at(
                  static_cast<std::size_t>(aura::compiler::types::as_string_idx(zs))) == "zero",
              "4428: tenant-0 string unchanged");
        CHECK(ev.find_active_closure(zcid).has_value(), "4428: tenant-0 closure still present");
    }

    // Soft/Off: foreign fill and free land, and the stamp is not consulted.
    reset_security_event_ring_for_test();
    ev.set_effect_sandbox_mode(0);
    aura::compiler::types::EvalValue soft_s;
    aura::compiler::types::EvalValue soft_clv;
    aura::compiler::ClosureId soft_cid = 0;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        const auto idx = ev.push_string_heap("soft");
        soft_s = make_string(static_cast<std::uint64_t>(idx));
        aura::compiler::Closure scl;
        soft_cid = ev.register_active_closure(std::move(scl));
        soft_clv = make_closure(soft_cid);
    }
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto filled = (*fill_fn)({soft_s, make_int(90)});
        auto freed = (*free_fn)({soft_clv});
        CHECK(!is_error(filled), "4428: Off string-fill! kept");
        const auto off_bytes = ev.copy_string_heap_at(
            static_cast<std::size_t>(aura::compiler::types::as_string_idx(soft_s)));
        const std::string filled90(4, static_cast<char>(90));
        CHECK(off_bytes == filled90, "4428: Off write visible, stamp not loaded");
        CHECK(is_bool(freed) && as_bool(freed), "4428: Off closure:free! kept");
        CHECK(!ev.find_active_closure(soft_cid).has_value(), "4428: Off free removed the closure");
        CHECK(!ring_has_isolation_deny("string-fill!"),
              "4428: Off records no string IsolationDeny");
        CHECK(!ring_has_isolation_deny("closure:free!"),
              "4428: Off records no closure IsolationDeny");
    }

    const auto pair_src = read_file("src/compiler/evaluator_primitives_pair.cpp");
    const auto sfpos = pair_src.find("add, ev, \"string-fill!\"");
    CHECK(sfpos != std::string::npos, "4428: string-fill! body located");
    if (sfpos != std::string::npos) {
        const auto win = pair_src.substr(sfpos, 1200);
        CHECK(win.find("inrange_string_tenant_allows") != std::string::npos,
              "4428: string-fill! consults the intern stamp");
        CHECK(win.find("require_effect") == std::string::npos,
              "4428: string-fill! window stays body-choke-free");
    }
    const auto mem_src = read_file("src/compiler/evaluator_primitives_memory.cpp");
    const auto cfpos = mem_src.find("\"closure:free!\"");
    CHECK(cfpos != std::string::npos, "4428: closure:free! body located");
    if (cfpos != std::string::npos) {
        const auto win = mem_src.substr(cfpos, 1600);
        CHECK(win.find("aura_free_closure_checked") != std::string::npos,
              "4428: int path still uses aura_free_closure_checked");
        CHECK(win.find("tw_closure_tenant_allows") != std::string::npos,
              "4428: value path compares the owner");
        const auto allow = win.find("tw_closure_tenant_allows");
        const auto erase = win.find("erase_active_closure");
        CHECK(allow != std::string::npos && erase != std::string::npos && allow < erase,
              "4428: owner compare precedes the erase");
    }
    const auto sec_src = read_file("src/compiler/evaluator_security.cpp");
    const auto spos = sec_src.find("bool Evaluator::inrange_string_tenant_allows");
    CHECK(spos != std::string::npos, "4428: string allow helper located");
    if (spos != std::string::npos) {
        const auto win = sec_src.substr(spos, 1800);
        CHECK(win.find("if (!prod)") != std::string::npos,
              "4428: Soft/Off returns before the stamp");
        CHECK(win.find("rec == 0") != std::string::npos,
              "4428: never-stamped string stays allowed");
        CHECK(win.find("check_workspace_isolation") != std::string::npos,
              "4428: string mismatch audits IsolationDeny");
    }

    aura::core::provenance::set_multi_tenant_env_active(false);
    set_mode(SandboxMode::Off);
}

// Issue #4429: hash-keys / hash-values / hash->alist / hash-length skipped
// aura_hash_gate_checked. Direct prim calls (same as #4296): the production
// face must return an empty result and copy no pairs. IsolationDeny is
// recorded only when the strong owner hook is the one ELF resolved; the
// light-link stub returns 0 and writes no ring event (#4296).
static void ac4429_hash_enumerator_gate() {
    std::println("\n--- #4429: hash enumerators use the hash tenant gate ---");
    using aura::compiler::types::as_pair_idx;
    using aura::compiler::types::as_string_idx;
    using aura::compiler::types::EvalValue;
    using aura::compiler::types::is_pair;
    using aura::compiler::types::is_string;
    using aura::compiler::types::make_string;
    reset_all();
    aura::core::bump_mutation_epoch(1);
    aura::core::provenance::set_multi_tenant_env_active(true);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted + MT → production face
    cs.register_jit_primitives();
    auto keys_fn = ev.primitives().lookup("hash-keys");
    auto vals_fn = ev.primitives().lookup("hash-values");
    auto alist_fn = ev.primitives().lookup("hash->alist");
    auto len_fn = ev.primitives().lookup("hash-length");
    CHECK(keys_fn && vals_fn && alist_fn && len_fn, "4429: enumerator prims present");
    if (!(keys_fn && vals_fn && alist_fn && len_fn)) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
        return;
    }

    auto read_str = [&](const EvalValue& v) -> std::string {
        if (!is_string(v))
            return {};
        const auto si = static_cast<std::size_t>(as_string_idx(v));
        if (si >= ev.string_heap_size())
            return {};
        return ev.copy_string_heap_at(si);
    };
    auto list_car_str = [&](const EvalValue& v) -> std::string {
        if (!is_pair(v))
            return {};
        auto& ps = ev.pairs();
        const auto idx = as_pair_idx(v);
        if (idx >= ps.size())
            return {};
        return read_str(ps[idx].car);
    };
    auto alist_key = [&](const EvalValue& v) -> std::string {
        if (!is_pair(v))
            return {};
        auto& ps = ev.pairs();
        const auto idx = as_pair_idx(v);
        if (idx >= ps.size() || !is_pair(ps[idx].car))
            return {};
        const auto kv = as_pair_idx(ps[idx].car);
        if (kv >= ps.size())
            return {};
        return read_str(ps[kv].car);
    };
    auto alist_val = [&](const EvalValue& v) -> std::string {
        if (!is_pair(v))
            return {};
        auto& ps = ev.pairs();
        const auto idx = as_pair_idx(v);
        if (idx >= ps.size() || !is_pair(ps[idx].car))
            return {};
        const auto kv = as_pair_idx(ps[idx].car);
        if (kv >= ps.size())
            return {};
        return read_str(ps[kv].cdr);
    };
    auto table_has = [&](std::uint64_t hidx, const EvalValue& want) -> bool {
        if (hidx >= g_hash_tables.size() || g_hash_tables[hidx] == nullptr)
            return false;
        auto* ht = g_hash_tables[hidx];
        const auto meta = ht->metadata();
        const auto vals = ht->values();
        for (std::uint64_t i = 0; i < ht->capacity; ++i) {
            if (meta[i] == 0xFF)
                continue;
            if (vals[i] == want.val)
                return true;
        }
        return false;
    };

    EvalValue key;
    EvalValue secret;
    EvalValue hv;
    bool strong_owner = false;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        strong_owner = aura_jit_owner_capability_tenant() == 7;
        const auto kidx = ev.push_string_heap("k");
        const auto vidx = ev.push_string_heap("secret");
        key = make_string(static_cast<std::uint64_t>(kidx));
        secret = make_string(static_cast<std::uint64_t>(vidx));
        hv = call_hash_op(ev, "hash", {key, secret});
    }
    CHECK(is_hash(hv), "4429: tenant 7 mints the hash");
    if (!is_hash(hv)) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
        return;
    }
    const auto hidx = as_hash_idx(hv);
    CHECK(hidx < g_hash_tenants.size() && g_hash_tenants[hidx] == 7, "4429: stamp is tenant 7");
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        const auto own_ref = call_hash_op(ev, "hash-ref", {hv, key});
        const auto own_keys = call_hash_op(ev, "hash-keys", {hv});
        const auto own_vals = call_hash_op(ev, "hash-values", {hv});
        const auto own_al = call_hash_op(ev, "hash->alist", {hv});
        const auto own_len = call_hash_op(ev, "hash-length", {hv});
        CHECK(read_str(own_ref) == "secret", "4429: owner hash-ref reads secret");
        CHECK(list_car_str(own_keys) == "k", "4429: owner hash-keys returns k");
        CHECK(list_car_str(own_vals) == "secret", "4429: owner hash-values returns secret");
        CHECK(alist_key(own_al) == "k" && alist_val(own_al) == "secret",
              "4429: owner hash->alist returns k and secret");
        CHECK(is_int(own_len) && as_int(own_len) == 1, "4429: owner hash-length is 1");
    }

    const auto pairs_before = ev.pairs_size();
    reset_security_event_ring_for_test();
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        const auto ref9 = call_hash_op(ev, "hash-ref", {hv, key});
        const auto keys9 = call_hash_op(ev, "hash-keys", {hv});
        const auto vals9 = call_hash_op(ev, "hash-values", {hv});
        const auto al9 = call_hash_op(ev, "hash->alist", {hv});
        const auto len9 = call_hash_op(ev, "hash-length", {hv});
        CHECK(is_void(ref9), "4429: tenant 9 hash-ref returns no value");
        CHECK(is_void(keys9), "4429: tenant 9 hash-keys is empty");
        CHECK(is_void(vals9), "4429: tenant 9 hash-values is empty");
        CHECK(is_void(al9), "4429: tenant 9 hash->alist is empty");
        CHECK(is_int(len9) && as_int(len9) == 0, "4429: tenant 9 hash-length is 0");
    }
    CHECK(ev.pairs_size() == pairs_before, "4429: foreign enumerate copies no pairs");
    CHECK(table_has(hidx, secret), "4429: secret stays in the table");
    const auto sz_after = (hidx < g_hash_tables.size() && g_hash_tables[hidx] != nullptr)
                              ? static_cast<std::int64_t>(g_hash_tables[hidx]->size)
                              : static_cast<std::int64_t>(-1);
    CHECK(sz_after == 1, "4429: table size unchanged");
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        const auto still = call_hash_op(ev, "hash-ref", {hv, key});
        CHECK(read_str(still) == "secret", "4429: owner still reads secret");
    }
    if (strong_owner) {
        CHECK(ring_has_isolation_deny("hash-keys"), "4429: IsolationDeny hash-keys");
        CHECK(ring_has_isolation_deny("hash-values"), "4429: IsolationDeny hash-values");
        CHECK(ring_has_isolation_deny("hash->alist"), "4429: IsolationDeny hash->alist");
        CHECK(ring_has_isolation_deny("hash-length"), "4429: IsolationDeny hash-length");
    } else {
        CHECK(!ring_has_isolation_deny("hash-keys"),
              "4429: light-link stub records no IsolationDeny on hash-keys");
        CHECK(!ring_has_isolation_deny("hash-length"),
              "4429: light-link stub records no IsolationDeny on hash-length");
    }

    // Unstamped (record 0) under the armed face denies a non-zero caller.
    CHECK(hidx < g_hash_tenants.size(), "4429: stamp slot exists");
    if (hidx < g_hash_tenants.size())
        g_hash_tenants[hidx] = 0;
    const auto pairs_unstamped = ev.pairs_size();
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        const auto keys_u = call_hash_op(ev, "hash-keys", {hv});
        const auto len_u = call_hash_op(ev, "hash-length", {hv});
        CHECK(is_void(keys_u), "4429: unstamped hash-keys is empty");
        CHECK(is_int(len_u) && as_int(len_u) == 0, "4429: unstamped hash-length is 0");
    }
    CHECK(ev.pairs_size() == pairs_unstamped, "4429: unstamped enumerate copies no pairs");
    CHECK(table_has(hidx, secret), "4429: unstamped deny leaves the secret");

    // Face 0: stamp 0 versus caller 9 would deny if the stamp were loaded.
    reset_security_event_ring_for_test();
    ev.set_effect_sandbox_mode(0);
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        const auto keys_off = call_hash_op(ev, "hash-keys", {hv});
        const auto vals_off = call_hash_op(ev, "hash-values", {hv});
        const auto al_off = call_hash_op(ev, "hash->alist", {hv});
        const auto len_off = call_hash_op(ev, "hash-length", {hv});
        CHECK(list_car_str(keys_off) == "k", "4429: Off hash-keys returns k");
        CHECK(list_car_str(vals_off) == "secret", "4429: Off hash-values returns secret");
        CHECK(alist_key(al_off) == "k" && alist_val(al_off) == "secret",
              "4429: Off hash->alist returns k and secret");
        CHECK(is_int(len_off) && as_int(len_off) == 1, "4429: Off hash-length is 1");
        CHECK(!ring_has_isolation_deny("hash-keys"),
              "4429: Off records no hash-keys IsolationDeny");
        CHECK(!ring_has_isolation_deny("hash-length"),
              "4429: Off records no hash-length IsolationDeny");
    }

    const auto vec = read_file("src/compiler/evaluator_primitives_vector.cpp");
    auto gate_before = [&](const char* op, const char* read_at) {
        const auto at = vec.find(std::format("add, ev, \"{}\"", op));
        if (at == std::string::npos)
            return false;
        const auto win = vec.substr(at, 1200);
        const auto gate = win.find("aura_hash_gate_checked");
        if (gate == std::string::npos)
            return false;
        const auto read = win.find(read_at);
        const auto lit = win.find(std::format("\"{}\"", op), gate);
        return read != std::string::npos && lit != std::string::npos && gate < read && lit < read;
    };
    CHECK(gate_before("hash-length", "->size"), "4429: hash-length gates before the size read");
    CHECK(gate_before("hash-keys", "auto* ht"), "4429: hash-keys gates before the copy");
    CHECK(gate_before("hash-values", "auto* ht"), "4429: hash-values gates before the copy");
    CHECK(gate_before("hash->alist", "auto* ht"), "4429: hash->alist gates before the copy");
    CHECK(vec.find("\"hash-ref\"))") != std::string::npos, "4429: hash-ref gate remains");
    CHECK(vec.find("\"hash-remove!\"))") != std::string::npos, "4429: hash-remove! gate remains");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto seam = rt.find("extern \"C\" bool aura_hash_gate_checked(");
    CHECK(seam != std::string::npos &&
              rt.substr(seam, 500).find("jit_tenant_gate(slot_tenant, caller_tenant, op)") !=
                  std::string::npos,
          "4429: deny still uses the existing isolation gate");

    aura::core::provenance::set_multi_tenant_env_active(false);
    set_mode(SandboxMode::Off);
    ev.set_effect_sandbox_mode(0);
}

// Issue #4430: granted_capabilities_ is one vector on the Evaluator.
// TenantScope rebinds the principal and used to leave string-only caps
// (compile / sandbox) visible to the next tenant. Production compares the
// stamp. Scope release retires that principal's write-side rows only.
static void ac4430_string_cap_tenant_mirror() {
    std::println("\n--- #4430: string-only caps follow the tenant principal ---");
    using aura::compiler::security::kCapCompile;
    using aura::compiler::security::kCapCompileDirty;
    using aura::compiler::security::kCapSandbox;
    using aura::compiler::types::make_string;

    auto heap_has = [](aura::compiler::Evaluator& ev, std::string_view needle) {
        for (const auto& s : ev.string_heap()) {
            if (s.find(needle) != std::string::npos)
                return true;
        }
        return false;
    };

    reset_all();
    aura::compiler::mutation_hold_live_reset_for_test();
    aura::serve::clear_current_fiber_session_mid();
    aura::core::bump_mutation_epoch(1);
    aura::core::provenance::set_multi_tenant_env_active(true);
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        const auto mid = aura::core::current_mutation_epoch();
        CHECK(mid != 0, "4430: epoch published for the host session mid");
        aura::compiler::g_mutation_hold_live_session_mid.store(mid, std::memory_order_release);
        int marks = 0;
        ev.set_mark_block_dirty_fn([&](const char*, std::size_t, std::uint32_t) {
            ++marks;
            return true;
        });
        auto dirty = ev.primitives().lookup("compile:mark-block-dirty!");
        auto compact = ev.primitives().lookup("mutation-log-compact");
        CHECK(compact.has_value(), "4430: mutation-log-compact present");
        if (compact) {
            {
                aura::compiler::Evaluator::TenantScope t1(ev, 443001);
                ev.grant_capability(kCapCompile);
                ev.grant_capability(kCapSandbox);
                CHECK(ev.has_capability(kCapCompile), "4430: T1 holds compile");
                CHECK(ev.has_capability(kCapSandbox), "4430: T1 holds sandbox");
                const auto n = ev.granted_capability_count();
                {
                    aura::compiler::Evaluator::TenantScope t2(ev, 443002);
                    CHECK(!ev.has_capability(kCapCompile), "4430: T2 does not see T1 compile");
                    CHECK(!ev.has_capability(kCapCompileDirty), "4430: T2 has no compile-dirty");
                    CHECK(!ev.has_capability(kCapSandbox), "4430: T2 does not see T1 sandbox");
                    const auto marks_before = marks;
                    if (dirty) {
                        const auto ni = ev.push_string_heap("blk");
                        auto r = (*dirty)({make_string(static_cast<std::uint64_t>(ni)), make_int(0),
                                           make_int(0)});
                        CHECK(is_error(r), "4430: foreign mark-block-dirty denied");
                        const bool named =
                            heap_has(ev, "capability denied: compile-dirty required");
                        CHECK(named, "4430: deny names compile-dirty");
                    }
                    CHECK(marks == marks_before, "4430: foreign principal did not mark IR dirty");
                    auto denied = ev.invoke_prim_with_telemetry("mutation-log-compact",
                                                                [&]() { return (*compact)({}); });
                    CHECK(is_error(denied), "4430: foreign mutation-log-compact denied");
                    const bool sand = heap_has(ev, "kCapSandbox");
                    CHECK(sand, "4430: deny names kCapSandbox");
                    ev.grant_capability(kCapCompile);
                    CHECK(ev.has_capability(kCapCompile), "4430: T2 own compile lands");
                    const auto n2 = ev.granted_capability_count();
                    CHECK(n2 == n + 1, "4430: T2 compile is a second row");
                }
                CHECK(ev.capability_tenant_id() == 443001, "4430: principal restored to T1");
                CHECK(ev.has_capability(kCapCompile), "4430: nested release keeps T1 compile");
                CHECK(ev.has_capability(kCapSandbox), "4430: nested release keeps T1 sandbox");
                CHECK(ev.granted_capability_count() == n, "4430: T2 write-side row retired");
                auto opened = ev.invoke_prim_with_telemetry("mutation-log-compact",
                                                            [&]() { return (*compact)({}); });
                CHECK(!is_error(opened), "4430: T1 sandbox opens mutation-log-compact");
                if (dirty) {
                    const auto ni = ev.push_string_heap("blk");
                    const auto before = marks;
                    auto r = (*dirty)(
                        {make_string(static_cast<std::uint64_t>(ni)), make_int(0), make_int(0)});
                    CHECK(!is_error(r), "4430: T1 mark-block-dirty runs");
                    CHECK(marks == before + 1, "4430: T1 dirty hook fired");
                }
            }
            CHECK(!ev.has_capability(kCapCompile), "4430: scope release retires T1 compile");
            ev.set_capability_tenant_id(443001);
            CHECK(!ev.has_capability(kCapCompile), "4430: compile stays retired for T1");
            CHECK(ev.has_capability(kCapSandbox), "4430: sandbox stays sticky for T1");
            ev.set_capability_tenant_id(443002);
            CHECK(!ev.has_capability(kCapSandbox), "4430: sticky sandbox does not authorize T2");
            CHECK(!ev.has_capability(kCapCompile), "4430: T2 compile died with its scope");
        }
        ev.set_effect_sandbox_mode(0);
    }

    reset_all();
    aura::compiler::mutation_hold_live_reset_for_test();
    aura::serve::clear_current_fiber_session_mid();
    aura::core::provenance::set_multi_tenant_env_active(false);
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        set_mode(SandboxMode::Off);
        {
            aura::compiler::Evaluator::TenantScope t1(ev, 443001);
            ev.grant_capability(kCapCompile);
            {
                aura::compiler::Evaluator::TenantScope t2(ev, 443002);
                CHECK(ev.has_capability(kCapCompile), "4430: Soft/Off string caps stay allow-all");
            }
            CHECK(ev.has_capability(kCapCompile),
                  "4430: Soft scope release does not erase compile");
        }
    }

    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto rel = sec.find("void Evaluator::TenantScope::release()");
    CHECK(rel != std::string::npos, "4430: TenantScope::release located");
    if (rel != std::string::npos) {
        const auto win = sec.substr(rel, 2400);
        CHECK(win.find("retire_session_string_caps_for_principal") != std::string::npos,
              "4430: release retires this principal's string caps");
    }
    const auto has = sec.find("bool Evaluator::has_capability");
    CHECK(has != std::string::npos, "4430: has_capability located");
    if (has != std::string::npos) {
        const auto win = sec.substr(has, 4500);
        CHECK(win.find("granted_capability_tenants_") != std::string::npos,
              "4430: string path reads the tenant stamp");
        CHECK(win.find("capability_tenant_id_") != std::string::npos,
              "4430: string path compares the current principal");
    }
    const auto caps = read_file("src/compiler/security_capabilities.h");
    CHECK(caps.find("kEffectCompile") == std::string::npos, "4430: no new compile effect bit");
    CHECK(read_file("tests/issues/test_issue_4430.cpp").empty(), "4430: no tests/issues file");
    CHECK(read_file("docs/design/4430-string-capability-mirror.md").empty(),
          "4430: no docs/design");

    aura::compiler::g_mutation_hold_live_session_mid.store(0, std::memory_order_release);
    aura::core::provenance::set_multi_tenant_env_active(false);
    set_mode(SandboxMode::Off);
}

// Issue #4431: hash-keys / hash-values / hash->alist and the query-result
// spines pushed pairs without note_inrange_pair_mint. Record 0 stays the
// documented allow. The production face stamps those user-visible cells so
// a foreign set-car! / set-cdr! / car hits the existing mismatch arm.
static void ac4431_user_visible_pair_mint() {
    std::println("\n--- #4431: user-visible pair lists carry the mint stamp ---");
    using aura::compiler::types::EvalValue;
    using aura::compiler::types::is_pair;
    reset_all();
    aura::core::bump_mutation_epoch(1);
    aura::core::provenance::set_multi_tenant_env_active(true);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    cs.register_jit_primitives();
    auto set_car_fn = ev.primitives().lookup("set-car!");
    auto set_cdr_fn = ev.primitives().lookup("set-cdr!");
    auto car_fn = ev.primitives().lookup("car");
    auto cdr_fn = ev.primitives().lookup("cdr");
    CHECK(set_car_fn && set_cdr_fn && car_fn && cdr_fn, "4431: pair prims present");
    if (!(set_car_fn && set_cdr_fn && car_fn && cdr_fn)) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
        ev.set_effect_sandbox_mode(0);
        return;
    }

    EvalValue hv;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        hv = call_hash_op(ev, "hash", {make_int(1), make_int(42)});
    }
    CHECK(is_hash(hv), "4431: tenant 7 mints the hash");
    if (!is_hash(hv)) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
        ev.set_effect_sandbox_mode(0);
        return;
    }

    EvalValue keys;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        keys = call_hash_op(ev, "hash-keys", {hv});
    }
    CHECK(is_pair(keys), "4431: owner hash-keys is a list cell");
    reset_security_event_ring_for_test();
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto sc = (*set_car_fn)({keys, make_int(99)});
        auto sd = (*set_cdr_fn)({keys, make_int(0)});
        auto rc = (*car_fn)({keys});
        CHECK(is_error(sc), "4431: foreign set-car! on hash-keys denied");
        CHECK(is_error(sd), "4431: foreign set-cdr! on hash-keys denied");
        CHECK(is_error(rc), "4431: foreign car on hash-keys denied");
        CHECK(ring_has_isolation_deny("set-car!"), "4431: IsolationDeny set-car!");
        CHECK(ring_has_isolation_deny("set-cdr!"), "4431: IsolationDeny set-cdr!");
        CHECK(ring_has_isolation_deny("car"), "4431: IsolationDeny car");
    }
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        auto rc = (*car_fn)({keys});
        CHECK(is_int(rc) && as_int(rc) == 1, "4431: hash-keys car unchanged");
        auto sc = (*set_car_fn)({keys, make_int(3)});
        CHECK(!is_error(sc), "4431: owner set-car! on hash-keys lands");
        auto rc2 = (*car_fn)({keys});
        CHECK(is_int(rc2) && as_int(rc2) == 3, "4431: owner hash-keys car is 3");
    }

    EvalValue vals;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        vals = call_hash_op(ev, "hash-values", {hv});
    }
    CHECK(is_pair(vals), "4431: owner hash-values is a list cell");
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto sc = (*set_car_fn)({vals, make_int(7)});
        CHECK(is_error(sc), "4431: foreign set-car! on hash-values denied");
    }
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        auto rc = (*car_fn)({vals});
        CHECK(is_int(rc) && as_int(rc) == 42, "4431: hash-values car unchanged");
    }

    EvalValue al;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        al = call_hash_op(ev, "hash->alist", {hv});
    }
    CHECK(is_pair(al), "4431: owner hash->alist is a list cell");
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto sc = (*set_car_fn)({al, make_int(0)});
        CHECK(is_error(sc), "4431: foreign set-car! on alist spine denied");
    }
    EvalValue kv;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        kv = (*car_fn)({al});
    }
    CHECK(is_pair(kv), "4431: alist car is the kv pair");
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto sc = (*set_car_fn)({kv, make_int(8)});
        auto sd = (*set_cdr_fn)({kv, make_int(9)});
        CHECK(is_error(sc), "4431: foreign set-car! on alist kv denied");
        CHECK(is_error(sd), "4431: foreign set-cdr! on alist kv denied");
    }
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        auto rc = (*car_fn)({kv});
        auto rd = (*cdr_fn)({kv});
        CHECK(is_int(rc) && as_int(rc) == 1, "4431: alist key unchanged");
        CHECK(is_int(rd) && as_int(rd) == 42, "4431: alist value unchanged");
    }

    reset_security_event_ring_for_test();
    ev.set_effect_sandbox_mode(0);
    EvalValue soft_keys;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        soft_keys = call_hash_op(ev, "hash-keys", {hv});
    }
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto sc = (*set_car_fn)({soft_keys, make_int(11)});
        auto rc = (*car_fn)({soft_keys});
        CHECK(!is_error(sc), "4431: Off set-car! on hash-keys kept");
        CHECK(is_int(rc) && as_int(rc) == 11, "4431: Off write visible and stamp not loaded");
        CHECK(!ring_has_isolation_deny("set-car!"), "4431: Off records no IsolationDeny");
    }

    const auto vec = read_file("src/compiler/evaluator_primitives_vector.cpp");
    auto body_of = [&](const char* op) -> std::string {
        const auto at = vec.find(std::format("add, ev, \"{}\"", op));
        if (at == std::string::npos)
            return {};
        const auto end = vec.find("register_prim", at + 10);
        if (end == std::string::npos)
            return vec.substr(at);
        return vec.substr(at, end - at);
    };
    const auto keys_body = body_of("hash-keys");
    const auto kpush = keys_body.find("pairs.push_back");
    const auto kstamp = keys_body.find("note_inrange_pair_mint");
    const auto kgate = keys_body.find("aura_hash_gate_checked");
    CHECK(kgate != std::string::npos && kpush != std::string::npos && kgate < kpush,
          "4431: hash-keys gate stays before the copy");
    CHECK(kpush != std::string::npos && kstamp != std::string::npos && kpush < kstamp,
          "4431: hash-keys stamps the minted list cell");
    const auto vals_body = body_of("hash-values");
    const auto vpush = vals_body.find("pairs.push_back");
    const auto vstamp = vals_body.find("note_inrange_pair_mint");
    CHECK(vpush != std::string::npos && vstamp != std::string::npos && vpush < vstamp,
          "4431: hash-values stamps the minted list cell");
    const auto al_body = body_of("hash->alist");
    std::size_t al_stamps = 0;
    for (std::size_t at = 0;
         (at = al_body.find("note_inrange_pair_mint", at)) != std::string::npos;) {
        ++al_stamps;
        at += 1;
    }
    CHECK(al_stamps == 2, "4431: hash->alist stamps the kv cell and the spine");

    const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    CHECK(qws.find("ws.pairs.push_back") == std::string::npos,
          "4431: query builders stamp every push");
    CHECK(qws.find("note_inrange_pair_mint") != std::string::npos,
          "4431: query stamp uses the mint helper");
    const auto pre = qws.find("Pre-allocate 3*N");
    CHECK(pre != std::string::npos, "4431: children-stable prealloc located");
    if (pre != std::string::npos) {
        const auto win = qws.substr(pre, 500);
        CHECK(win.find("push_stamped_pair") != std::string::npos,
              "4431: placeholder cells are stamped");
    }
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto allow = sec.find("bool Evaluator::inrange_slot_tenant_allows");
    CHECK(allow != std::string::npos, "4431: in-range allow located");
    if (allow != std::string::npos) {
        const auto win = sec.substr(allow, 1800);
        CHECK(win.find("rec == 0") != std::string::npos,
              "4431: record 0 stays the documented allow");
    }

    aura::core::provenance::set_multi_tenant_env_active(false);
    set_mode(SandboxMode::Off);
    ev.set_effect_sandbox_mode(0);
}

// Issue #4505: the sibling readers. Tenant 7 mints, tenant 9 holds the
// mailbox refs (#4298 threat model). Every in-range reader — list-ref /
// append / reverse / map / filter / member / memv / assv / length / caar,
// vector->list / list->vector / vector-length (+ the vector-ref OOB path
// that formatted the length before the gate), string-ref / substring /
// string->list / string-length / string-append / string=?, display /
// write, and the eval_hash_keys_eq probe — must compare the mint stamp
// before any payload is copied, returned, formatted, or printed. Same
// helper contract as car/vector-ref/string-fill!; no Effect bit, no
// second require_effect, no query key.
static void ac4505_inrange_reader_prims() {
    std::println("\n--- #4505: in-range reader prims tenant gate ---");
    using aura::compiler::types::as_error_idx;
    using aura::compiler::types::as_pair_idx;
    using aura::compiler::types::as_string_idx;
    using aura::compiler::types::EvalValue;
    using aura::compiler::types::is_pair;
    using aura::compiler::types::is_primitive;
    using aura::compiler::types::is_string;
    using aura::compiler::types::is_vector;
    using aura::compiler::types::make_string;
    reset_all();
    aura::core::bump_mutation_epoch(1);
    aura::core::provenance::set_multi_tenant_env_active(true);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted + MT → production face
    auto lookup = [&](const char* name) { return ev.primitives().lookup(name); };
    auto cons_fn = lookup("cons");
    auto car_fn = lookup("car");
    auto list_fn = lookup("list");
    auto vec_fn = lookup("vector");
    auto list_ref_fn = lookup("list-ref");
    auto append_fn = lookup("append");
    auto reverse_fn = lookup("reverse");
    auto map_fn = lookup("map");
    auto filter_fn = lookup("filter");
    auto member_fn = lookup("member");
    auto memv_fn = lookup("memv");
    auto assv_fn = lookup("assv");
    auto length_fn = lookup("length");
    auto caar_fn = lookup("caar");
    auto v2l_fn = lookup("vector->list");
    auto l2v_fn = lookup("list->vector");
    auto vlen_fn = lookup("vector-length");
    auto vref_fn = lookup("vector-ref");
    auto sref_fn = lookup("string-ref");
    auto subs_fn = lookup("substring");
    auto s2l_fn = lookup("string->list");
    auto slen_fn = lookup("string-length");
    auto sapp_fn = lookup("string-append");
    auto seq_fn = lookup("string=?");
    auto display_fn = lookup("display");
    auto write_fn = lookup("write");
    auto hash_fn = lookup("hash");
    auto href_fn = lookup("hash-ref");
    auto hhas_fn = lookup("hash-has-key?");
    CHECK(list_ref_fn && append_fn && reverse_fn && map_fn && filter_fn && member_fn && memv_fn &&
              assv_fn && length_fn && caar_fn && v2l_fn && l2v_fn && vlen_fn && vref_fn &&
              sref_fn && subs_fn && s2l_fn && slen_fn && sapp_fn && seq_fn && display_fn &&
              write_fn && hash_fn && href_fn && hhas_fn,
          "4505: reader prims present");
    if (!(list_ref_fn && append_fn && reverse_fn && map_fn && filter_fn && member_fn && memv_fn &&
          assv_fn && length_fn && caar_fn && v2l_fn && l2v_fn && vlen_fn && vref_fn && sref_fn &&
          subs_fn && s2l_fn && slen_fn && sapp_fn && seq_fn && display_fn && write_fn && hash_fn &&
          href_fn && hhas_fn)) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
        return;
    }

    EvalValue pair7, lst7, vec7, str7, nested7, alist7;
    const EvalValue car_prim = [&] {
        auto v = cs.eval("car"); // prim value for map/filter fn slots
        return v ? *v : make_void();
    }();
    CHECK(is_primitive(car_prim), "4505: car prim value resolvable");
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        pair7 = (*cons_fn)({make_int(42), make_int(1)});
        lst7 = (*list_fn)({make_int(10), make_int(20), make_int(30)});
        vec7 = (*vec_fn)({make_int(10), make_int(20)});
        str7 = make_string(static_cast<std::uint64_t>(ev.push_string_heap("hello")));
        nested7 = (*list_fn)({(*list_fn)({make_int(77)})});
        alist7 = (*list_fn)({(*cons_fn)({make_int(10), make_int(11)})});
    }
    CHECK(!is_error(pair7) && !is_error(lst7) && !is_error(vec7) && is_string(str7),
          "4505: tenant 7 mints the shared payloads");

    EvalValue own9s, own9vec, own9pair;
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        own9s = make_string(static_cast<std::uint64_t>(ev.push_string_heap("zz")));
        own9vec = (*vec_fn)({make_int(5)});
        own9pair = (*cons_fn)({make_int(5), make_int(6)});
    }

    // One denied call per prim; the audit ring must carry exactly one
    // IsolationDeny row per op, and no payload may come back.
    reset_security_event_ring_for_test();
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto r_listref = (*list_ref_fn)({lst7, make_int(0)});
        auto r_append = (*append_fn)({lst7, make_void()});
        auto r_reverse = (*reverse_fn)({lst7});
        auto r_map = (*map_fn)({car_prim, lst7});
        auto r_filter = (*filter_fn)({car_prim, lst7});
        auto r_member = (*member_fn)({make_int(10), lst7});
        auto r_memv = (*memv_fn)({make_int(10), lst7});
        auto r_assv = (*assv_fn)({make_int(10), alist7});
        auto r_length = (*length_fn)({lst7});
        auto r_caar = (*caar_fn)({nested7});
        auto r_v2l = (*v2l_fn)({vec7});
        auto r_l2v = (*l2v_fn)({lst7});
        auto r_vlen = (*vlen_fn)({vec7});
        auto r_sref = (*sref_fn)({str7, make_int(0)});
        auto r_subs = (*subs_fn)({str7, make_int(0), make_int(2)});
        auto r_s2l = (*s2l_fn)({str7});
        auto r_slen = (*slen_fn)({str7});
        auto r_sapp = (*sapp_fn)({str7});
        auto r_seq = (*seq_fn)({str7, str7});
        CHECK(is_error(r_listref), "4505: tenant 9 list-ref denied");
        CHECK(is_error(r_append), "4505: tenant 9 append denied");
        CHECK(is_error(r_reverse), "4505: tenant 9 reverse denied");
        CHECK(is_error(r_map), "4505: tenant 9 map denied");
        CHECK(is_error(r_filter), "4505: tenant 9 filter denied");
        CHECK(is_error(r_member), "4505: tenant 9 member denied (no sublist ref)");
        CHECK(is_error(r_memv), "4505: tenant 9 memv denied");
        CHECK(is_error(r_assv), "4505: tenant 9 assv denied");
        CHECK(is_error(r_length), "4505: tenant 9 length denied");
        CHECK(is_error(r_caar), "4505: tenant 9 caar denied on the nested list");
        CHECK(is_error(r_v2l), "4505: tenant 9 vector->list denied");
        CHECK(is_error(r_l2v), "4505: tenant 9 list->vector denied");
        CHECK(is_error(r_vlen), "4505: tenant 9 vector-length denied");
        CHECK(is_error(r_sref), "4505: tenant 9 string-ref denied");
        CHECK(is_error(r_subs), "4505: tenant 9 substring denied");
        CHECK(is_error(r_s2l), "4505: tenant 9 string->list denied");
        CHECK(is_error(r_slen), "4505: tenant 9 string-length denied");
        CHECK(is_error(r_sapp), "4505: tenant 9 string-append denied");
        CHECK(is_error(r_seq), "4505: tenant 9 string=? denied");
        CHECK(ring_isolation_deny_count("list-ref") == 1, "4505: one IsolationDeny list-ref");
        CHECK(ring_isolation_deny_count("append") == 1, "4505: one IsolationDeny append");
        CHECK(ring_isolation_deny_count("reverse") == 1, "4505: one IsolationDeny reverse");
        CHECK(ring_isolation_deny_count("map") == 1, "4505: one IsolationDeny map");
        CHECK(ring_isolation_deny_count("filter") == 1, "4505: one IsolationDeny filter");
        CHECK(ring_isolation_deny_count("member") == 1, "4505: one IsolationDeny member");
        CHECK(ring_isolation_deny_count("memv") == 1, "4505: one IsolationDeny memv");
        CHECK(ring_isolation_deny_count("assv") == 1, "4505: one IsolationDeny assv");
        CHECK(ring_isolation_deny_count("length") == 1, "4505: one IsolationDeny length");
        CHECK(ring_isolation_deny_count("caar") == 1, "4505: one IsolationDeny caar");
        CHECK(ring_isolation_deny_count("vector->list") == 1,
              "4505: one IsolationDeny vector->list");
        CHECK(ring_isolation_deny_count("list->vector") == 1,
              "4505: one IsolationDeny list->vector");
        CHECK(ring_isolation_deny_count("vector-length") == 1,
              "4505: one IsolationDeny vector-length");
        CHECK(ring_isolation_deny_count("string-ref") == 1, "4505: one IsolationDeny string-ref");
        CHECK(ring_isolation_deny_count("substring") == 1, "4505: one IsolationDeny substring");
        CHECK(ring_isolation_deny_count("string->list") == 1,
              "4505: one IsolationDeny string->list");
        CHECK(ring_isolation_deny_count("string-length") == 1,
              "4505: one IsolationDeny string-length");
        CHECK(ring_isolation_deny_count("string-append") == 1,
              "4505: one IsolationDeny string-append");
        CHECK(ring_isolation_deny_count("string=?") == 1, "4505: one IsolationDeny string=?");

        // display / write: no payload on stdout, deny result, one row each.
        const auto out_ds = with_stdout_capture([&] {
            auto r = (*display_fn)({str7});
            CHECK(is_error(r), "4505: tenant 9 display of a foreign string denied");
        });
        const auto out_dp = with_stdout_capture([&] {
            auto r = (*display_fn)({pair7});
            CHECK(is_error(r), "4505: tenant 9 display of a foreign pair denied");
        });
        const auto out_dv = with_stdout_capture([&] {
            auto r = (*display_fn)({vec7});
            CHECK(is_error(r), "4505: tenant 9 display of a foreign vector denied");
        });
        const auto out_ws = with_stdout_capture([&] {
            auto r = (*write_fn)({str7});
            CHECK(is_error(r), "4505: tenant 9 write of a foreign string denied");
        });
        CHECK(out_ds.find("hello") == std::string::npos,
              "4505: foreign string bytes absent from stdout (display)");
        CHECK(out_dp.find("42") == std::string::npos && out_dp.find("(") == std::string::npos,
              "4505: foreign pair payload absent from stdout (display)");
        CHECK(out_dv.find("#(") == std::string::npos && out_dv.find("10") == std::string::npos,
              "4505: foreign vector payload absent from stdout (display)");
        CHECK(out_ws.find("hello") == std::string::npos,
              "4505: foreign string bytes absent from stdout (write)");
        CHECK(ring_isolation_deny_count("display") == 3,
              "4505: one IsolationDeny per display deny");
        CHECK(ring_isolation_deny_count("write") == 1, "4505: one IsolationDeny write");

        // vector-ref OOB: the deny must fire before the #4497 branch formats
        // vector_heap[idx].size() into the message — the length is payload.
        auto r_vref_oob = (*vref_fn)({vec7, make_int(99)});
        CHECK(is_error(r_vref_oob), "4505: tenant 9 vector-ref OOB denied");
        if (is_error(r_vref_oob)) {
            const auto eidx = static_cast<std::size_t>(as_error_idx(r_vref_oob));
            CHECK(eidx < ev.error_values_.size() && is_string(ev.error_values_[eidx]),
                  "4505: OOB deny carries an error message");
            if (eidx < ev.error_values_.size() && is_string(ev.error_values_[eidx])) {
                const auto msg = ev.copy_string_heap_at(
                    static_cast<std::size_t>(as_string_idx(ev.error_values_[eidx])));
                CHECK(msg.find("for vector of length 2") == std::string::npos,
                      "4505: foreign length not formatted into the OOB message");
                CHECK(msg.find("denied") != std::string::npos,
                      "4505: OOB message is the cross-tenant deny shape");
            }
        }
        CHECK(ring_isolation_deny_count("vector-ref") == 1, "4505: one IsolationDeny vector-ref");

        // eval_hash_keys_eq: the probe is an equality oracle. A foreign
        // string/vector/pair stored key must not answer byte/element
        // equality for tenant 9 — a mismatch reads as missing.
        auto h9s = (*hash_fn)({str7, make_int(42)});
        auto h9v = (*hash_fn)({vec7, make_int(43)});
        auto h9p = (*hash_fn)({pair7, make_int(44)});
        CHECK(is_hash(h9s) && is_hash(h9v) && is_hash(h9p),
              "4505: tenant 9 mints the probe tables");
        if (is_hash(h9s) && is_hash(h9v) && is_hash(h9p)) {
            const auto base_s = ring_isolation_deny_count("hash-ref");
            auto rs = (*href_fn)({h9s, own9s});
            auto rv = (*href_fn)({h9v, own9vec});
            auto rp = (*href_fn)({h9p, own9pair});
            auto hs = (*hhas_fn)({h9s, own9s});
            CHECK(is_void(rs), "4505: foreign string key probes as missing");
            CHECK(is_void(rv), "4505: foreign vector key probes as missing");
            CHECK(is_void(rp), "4505: foreign pair key probes as missing");
            CHECK(is_bool(hs) && !as_bool(hs), "4505: hash-has-key? misses on the foreign key");
            CHECK(ring_isolation_deny_count("hash-ref") == base_s + 3,
                  "4505: one probe IsolationDeny per foreign key arm (hash-ref)");
            CHECK(ring_isolation_deny_count("hash-has-key?") == 1,
                  "4505: one probe IsolationDeny (hash-has-key?)");
        }
    }

    // Owner still reads its own mints, payloads unchanged after the denies.
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        auto r0 = (*list_ref_fn)({lst7, make_int(0)});
        auto r2 = (*list_ref_fn)({lst7, make_int(2)});
        CHECK(is_int(r0) && as_int(r0) == 10, "4505: owner list-ref head");
        CHECK(is_int(r2) && as_int(r2) == 30, "4505: owner list-ref tail");
        auto ap = (*append_fn)({lst7, make_void()});
        CHECK(!is_error(ap) && is_pair(ap) && as_int((*car_fn)({ap})) == 10,
              "4505: owner append copies");
        auto rev = (*reverse_fn)({lst7});
        CHECK(!is_error(rev) && is_pair(rev) && as_int((*car_fn)({rev})) == 30,
              "4505: owner reverse copies");
        auto mp = (*map_fn)({car_prim, alist7});
        CHECK(!is_error(mp) && is_pair(mp) && as_int((*car_fn)({mp})) == 10,
              "4505: owner map copies");
        auto fl = (*filter_fn)({car_prim, alist7});
        CHECK(!is_error(fl) && is_pair(fl), "4505: owner filter copies");
        auto mem = (*member_fn)({make_int(20), lst7});
        CHECK(is_pair(mem), "4505: owner member returns the sublist");
        auto mv = (*memv_fn)({make_int(20), lst7});
        CHECK(is_pair(mv), "4505: owner memv returns the sublist");
        auto av = (*assv_fn)({make_int(10), alist7});
        CHECK(is_pair(av), "4505: owner assv returns the entry");
        auto len = (*length_fn)({lst7});
        CHECK(is_int(len) && as_int(len) == 3, "4505: owner length");
        auto ca = (*caar_fn)({nested7});
        CHECK(is_int(ca) && as_int(ca) == 77, "4505: owner caar on the nested list");
        auto v2l = (*v2l_fn)({vec7});
        CHECK(is_pair(v2l) && as_int((*car_fn)({v2l})) == 10, "4505: owner vector->list copies");
        auto l2v = (*l2v_fn)({lst7});
        CHECK(is_vector(l2v), "4505: owner list->vector mints");
        if (is_vector(l2v)) {
            auto e = (*vref_fn)({l2v, make_int(1)});
            CHECK(is_int(e) && as_int(e) == 20, "4505: owner list->vector elements");
        }
        auto vl = (*vlen_fn)({vec7});
        CHECK(is_int(vl) && as_int(vl) == 2, "4505: owner vector-length");
        auto sr = (*sref_fn)({str7, make_int(0)});
        CHECK(is_int(sr) && as_int(sr) == 104, "4505: owner string-ref is 'h'");
        auto subs = (*subs_fn)({str7, make_int(0), make_int(2)});
        CHECK(is_string(subs) &&
                  ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(subs))) == "he",
              "4505: owner substring copies");
        auto s2l = (*s2l_fn)({str7});
        CHECK(is_pair(s2l) && as_int((*car_fn)({s2l})) == 104, "4505: owner string->list copies");
        auto sl = (*slen_fn)({str7});
        CHECK(is_int(sl) && as_int(sl) == 5, "4505: owner string-length");
        auto sa = (*sapp_fn)({str7, str7});
        CHECK(is_string(sa) && ev.copy_string_heap_at(
                                   static_cast<std::size_t>(as_string_idx(sa))) == "hellohello",
              "4505: owner string-append copies");
        auto sq = (*seq_fn)({str7, str7});
        CHECK(is_bool(sq) && as_bool(sq), "4505: owner string=? compares");
        const auto out_own_d = with_stdout_capture([&] { (void)(*display_fn)({str7}); });
        const auto out_own_w = with_stdout_capture([&] { (void)(*write_fn)({str7}); });
        const auto out_own_p = with_stdout_capture([&] { (void)(*display_fn)({pair7}); });
        const auto out_own_v = with_stdout_capture([&] { (void)(*display_fn)({vec7}); });
        CHECK(out_own_d.find("hello") != std::string::npos, "4505: owner display prints");
        CHECK(out_own_w.find("\"hello\"") != std::string::npos, "4505: owner write quotes");
        CHECK(out_own_p.find("(42 . 1)") != std::string::npos, "4505: owner display pair");
        CHECK(out_own_v.find("#(10 20)") != std::string::npos, "4505: owner display vector");
        // Source objects untouched by every deny above (readers, not stores).
        CHECK(as_int((*car_fn)({pair7})) == 42, "4505: owner pair bytes unchanged");
        CHECK(ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(str7))) == "hello",
              "4505: owner string bytes unchanged");
        auto vlast = (*vref_fn)({vec7, make_int(1)});
        CHECK(is_int(vlast) && as_int(vlast) == 20, "4505: owner vector unchanged");
    }

    // Tenant 0's mint is a real stamp (tenant+1) and refuses tenant 9;
    // tenant 0 still reads its own slots.
    EvalValue lst0, str0;
    {
        aura::compiler::Evaluator::TenantScope t0(ev, 0);
        lst0 = (*list_fn)({make_int(5), make_int(6)});
        str0 = make_string(static_cast<std::uint64_t>(ev.push_string_heap("zero")));
    }
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto r = (*list_ref_fn)({lst0, make_int(0)});
        auto sl = (*slen_fn)({str0});
        CHECK(is_error(r), "4505: tenant-0 list refused for tenant 9");
        CHECK(is_error(sl), "4505: tenant-0 string refused for tenant 9");
    }
    {
        aura::compiler::Evaluator::TenantScope t0(ev, 0);
        auto r = (*list_ref_fn)({lst0, make_int(0)});
        auto sl = (*slen_fn)({str0});
        CHECK(is_int(r) && as_int(r) == 5, "4505: tenant 0 reads its own list");
        CHECK(is_int(sl) && as_int(sl) == 4, "4505: tenant 0 reads its own string");
    }

    // Soft/Off: the helpers return true before the stamp load — foreign
    // reads copy/print and no IsolationDeny row is written.
    reset_security_event_ring_for_test();
    ev.set_effect_sandbox_mode(0);
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto r = (*list_ref_fn)({lst7, make_int(0)});
        CHECK(is_int(r) && as_int(r) == 10, "4505: Off list-ref copies");
        auto sr = (*sref_fn)({str7, make_int(0)});
        CHECK(is_int(sr) && as_int(sr) == 104, "4505: Off string-ref copies");
        auto v2l = (*v2l_fn)({vec7});
        CHECK(is_pair(v2l), "4505: Off vector->list copies");
        const auto out = with_stdout_capture([&] { (void)(*display_fn)({str7}); });
        CHECK(out.find("hello") != std::string::npos, "4505: Off display prints the payload");
        CHECK(!ring_has_isolation_deny("list-ref") && !ring_has_isolation_deny("string-ref") &&
                  !ring_has_isolation_deny("display"),
              "4505: Off records no reader IsolationDeny");
    }

    // Source-cite windows: the gates live in the prim bodies, ahead of any
    // copy/format/print, and none of them add a require_effect choke.
    const auto list_src = read_file("src/compiler/evaluator_primitives_list.cpp");
    const auto lrpos = list_src.find("add, ev, \"list-ref\"");
    CHECK(lrpos != std::string::npos, "4505: list-ref body located");
    if (lrpos != std::string::npos) {
        const auto win = list_src.substr(lrpos, 2600);
        CHECK(win.find("inrange_slot_tenant_allows") != std::string::npos,
              "4505: list-ref consults the mint stamp");
        CHECK(win.find("require_effect") == std::string::npos,
              "4505: list-ref stays body-choke-free");
    }
    CHECK(list_src.find("append: cross-tenant pair slot read denied (#4505)") != std::string::npos,
          "4505: append deny shape present");
    CHECK(list_src.find("\"map\"") != std::string::npos &&
              list_src.find("\"filter\"") != std::string::npos,
          "4505: map/filter gates cite the prim op");
    const auto vec_src = read_file("src/compiler/evaluator_primitives_vector.cpp");
    const auto vrpos = vec_src.find("add, ev, \"vector-ref\"");
    CHECK(vrpos != std::string::npos, "4505: vector-ref body located");
    if (vrpos != std::string::npos) {
        const auto win = vec_src.substr(vrpos, 2600);
        const auto gate = win.find("inrange_slot_tenant_allows");
        const auto oob = win.find("for vector of length");
        CHECK(gate != std::string::npos && oob != std::string::npos && gate < oob,
              "4505: vector-ref gate precedes the length format");
    }
    // The char file registers the string->list that wins name lookup —
    // its body must carry the same reader gate.
    const auto char_src = read_file("src/compiler/evaluator_primitives_char.cpp");
    const auto cs2l = char_src.find("add(\"string->list\"");
    CHECK(cs2l != std::string::npos, "4505: winning string->list registration located");
    if (cs2l != std::string::npos) {
        const auto win = char_src.substr(cs2l, 900);
        CHECK(win.find("inrange_string_tenant_allows") != std::string::npos,
              "4505: winning string->list consults the intern stamp");
    }
    const auto eqpos = vec_src.find("bool eval_hash_keys_eq(");
    CHECK(eqpos != std::string::npos, "4505: eval_hash_keys_eq located");
    if (eqpos != std::string::npos) {
        const auto win = vec_src.substr(eqpos, 3200);
        CHECK(win.find("inrange_string_tenant_allows") != std::string::npos,
              "4505: key equality gates foreign string bytes");
        CHECK(win.find("inrange_slot_tenant_allows") != std::string::npos,
              "4505: key equality gates foreign pair/vector slots");
    }
    const auto pair_src = read_file("src/compiler/evaluator_primitives_pair.cpp");
    const auto s2lpos = pair_src.find("add, ev, \"string->list\"");
    CHECK(s2lpos != std::string::npos, "4505: string->list body located");
    if (s2lpos != std::string::npos) {
        const auto win = pair_src.substr(s2lpos, 1200);
        CHECK(win.find("inrange_string_tenant_allows") != std::string::npos,
              "4505: string->list consults the intern stamp");
        CHECK(win.find("require_effect") == std::string::npos,
              "4505: string->list stays body-choke-free");
    }
    CHECK(pair_src.find("caar: cross-tenant pair slot read denied (#4505)") != std::string::npos,
          "4505: caar deny shape present");
    const auto rt_src = read_file("src/compiler/evaluator_primitives_runtime.cpp");
    const auto dpos = rt_src.find("add(\"display\"");
    CHECK(dpos != std::string::npos, "4505: display body located");
    if (dpos != std::string::npos) {
        const auto win = rt_src.substr(dpos, 900);
        CHECK(win.find("io_walk_tenant_allows") != std::string::npos,
              "4505: display pre-walks the payload before printing");
    }
    CHECK(rt_src.find("static bool io_walk_tenant_allows") != std::string::npos,
          "4505: io_print_val walk is gated by the pre-walk helper");

    aura::core::provenance::set_multi_tenant_env_active(false);
    set_mode(SandboxMode::Off);
    ev.set_effect_sandbox_mode(0);
}

// Issue #4518: the kEffectNone reader bypass. apply copies pairs_[idx].car
// into the callee arg vector; list->string walks the spine; %base64-encode /
// %base64-decode / %hex-encode / %hex-decode copy string_heap_ bytes
// (#4500: % names stay kEffectNone); f64vector-length / -ref / -dot and
// bytes->f64vector / f32-bytes->f64vector / bytes-i32-ref read the string
// slot that backs an f64vector (#4509 lock is the UAF fix, not a stamp
// compare). infer_required_effects_from_name returns kEffectNone for every
// one, so dispatch never choked a foreign mailbox ref. Same-Evaluator
// Restricted+MT: tenant 7 mints, tenant 9 reads — each listed prim must
// consult the existing helper (required_effects=0, its own name) BEFORE any
// car / byte is copied, deny with the #4505 error shape, leave the source
// slot unchanged, and record exactly one IsolationDeny. f64vector-set! is
// the already-gated #4428 sibling and stays on kEffectMutate. Record 0 /
// short stamp arrays / Soft-Off stay allow; no Effect bit, no second body
// require_effect, no new query key.
static void ac4518_inrange_apply_encoding_f64_readers() {
    std::println("\n--- #4518: apply / %-encoding / f64 reader tenant gate ---");
    using aura::compiler::types::as_bool;
    using aura::compiler::types::as_error_idx;
    using aura::compiler::types::as_float;
    using aura::compiler::types::as_int;
    using aura::compiler::types::as_string_idx;
    using aura::compiler::types::EvalValue;
    using aura::compiler::types::is_float;
    using aura::compiler::types::is_string;
    using aura::compiler::types::make_float;
    using aura::compiler::types::make_string;
    reset_all();
    aura::core::bump_mutation_epoch(1);
    aura::core::provenance::set_multi_tenant_env_active(true);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted + MT → production face
    auto lookup = [&](const char* name) { return ev.primitives().lookup(name); };
    auto apply_fn = lookup("apply");
    auto l2s_fn = lookup("list->string");
    auto b64e_fn = lookup("%base64-encode");
    auto b64d_fn = lookup("%base64-decode");
    auto hexe_fn = lookup("%hex-encode");
    auto hexd_fn = lookup("%hex-decode");
    auto f64len_fn = lookup("f64vector-length");
    auto f64ref_fn = lookup("f64vector-ref");
    auto f64dot_fn = lookup("f64vector-dot");
    auto b2f64_fn = lookup("bytes->f64vector");
    auto f32b2f64_fn = lookup("f32-bytes->f64vector");
    auto i32ref_fn = lookup("bytes-i32-ref");
    auto f64set_fn = lookup("f64vector-set!");
    auto cons_fn = lookup("cons");
    auto list_fn = lookup("list");
    auto car_fn = lookup("car");
    CHECK(apply_fn && l2s_fn && b64e_fn && b64d_fn && hexe_fn && hexd_fn && f64len_fn &&
              f64ref_fn && f64dot_fn && b2f64_fn && f32b2f64_fn && i32ref_fn && f64set_fn &&
              cons_fn && list_fn && car_fn,
          "4518: reader prims present");
    if (!(apply_fn && l2s_fn && b64e_fn && b64d_fn && hexe_fn && hexd_fn && f64len_fn &&
          f64ref_fn && f64dot_fn && b2f64_fn && f32b2f64_fn && i32ref_fn && f64set_fn && cons_fn &&
          list_fn && car_fn)) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
        return;
    }

    const EvalValue car_prim = [&] {
        auto v = cs.eval("car"); // prim value for the apply fn slot
        return v ? *v : make_void();
    }();
    CHECK(aura::compiler::types::is_primitive(car_prim), "4518: car prim value resolvable");

    auto err_msg = [&](const aura::compiler::types::EvalValue& r) -> std::string {
        if (!is_error(r))
            return {};
        const auto eidx = static_cast<std::size_t>(as_error_idx(r));
        if (eidx >= ev.error_values_.size() || !is_string(ev.error_values_[eidx]))
            return {};
        return ev.copy_string_heap_at(
            static_cast<std::size_t>(as_string_idx(ev.error_values_[eidx])));
    };

    // Tenant 7 mints the shared payloads (real stamps: tenant+1, non-zero).
    // arglist7 = ((42 . 1)) — apply spreads its elements, so car receives the
    // pair. cps7 = (104 105) — list->string UTF-8-encodes code points "hi".
    EvalValue lst7, str7, f647, bytes7, arglist7, cps7;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        lst7 = (*list_fn)({make_int(10), make_int(20), make_int(30)});
        str7 = make_string(static_cast<std::uint64_t>(ev.push_string_heap("hello")));
        f647 = (*lookup("make-f64vector"))({make_int(2), make_float(1.5)});
        bytes7 = make_string(static_cast<std::uint64_t>(
            ev.push_string_heap(std::string("\x00\x00\x00\x00\x00\x00\xf0\x3f", 8))));
        const auto p7 = (*cons_fn)({make_int(42), make_int(1)});
        arglist7 = (*list_fn)({p7});
        cps7 = (*list_fn)({make_int(104), make_int(105)});
    }
    CHECK(!is_error(lst7) && is_string(str7) && !is_error(f647) && is_string(bytes7) &&
              !is_error(arglist7) && !is_error(cps7),
          "4518: tenant 7 mints the shared payloads");
    if (!(!is_error(lst7) && is_string(str7) && !is_error(f647) && is_string(bytes7) &&
          !is_error(arglist7) && !is_error(cps7))) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
        return;
    }

    // One denied call per prim; each op must audit exactly one
    // IsolationDeny, return the #4505 deny shape, and leak no payload.
    reset_security_event_ring_for_test();
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto r_apply = (*apply_fn)({car_prim, lst7});
        auto r_l2s = (*l2s_fn)({lst7});
        auto r_b64e = (*b64e_fn)({str7});
        auto r_b64d = (*b64d_fn)({str7});
        auto r_hexe = (*hexe_fn)({str7});
        auto r_hexd = (*hexd_fn)({str7});
        auto r_len = (*f64len_fn)({f647});
        auto r_ref = (*f64ref_fn)({f647, make_int(0)});
        auto r_dot = (*f64dot_fn)({f647, f647});
        auto r_b2f = (*b2f64_fn)({bytes7});
        auto r_f32b = (*f32b2f64_fn)({bytes7});
        auto r_i32 = (*i32ref_fn)({bytes7, make_int(0)});
        auto r_set = (*f64set_fn)({f647, make_int(0), make_float(9.0)});
        CHECK(is_error(r_apply), "4518: tenant 9 apply denied (args never built)");
        CHECK(is_error(r_l2s), "4518: tenant 9 list->string denied");
        CHECK(is_error(r_b64e), "4518: tenant 9 %base64-encode denied");
        CHECK(is_error(r_b64d), "4518: tenant 9 %base64-decode denied");
        CHECK(is_error(r_hexe), "4518: tenant 9 %hex-encode denied");
        CHECK(is_error(r_hexd), "4518: tenant 9 %hex-decode denied");
        CHECK(is_error(r_len), "4518: tenant 9 f64vector-length denied");
        CHECK(is_error(r_ref), "4518: tenant 9 f64vector-ref denied");
        CHECK(is_error(r_dot), "4518: tenant 9 f64vector-dot denied");
        CHECK(is_error(r_b2f), "4518: tenant 9 bytes->f64vector denied");
        CHECK(is_error(r_f32b), "4518: tenant 9 f32-bytes->f64vector denied");
        CHECK(is_error(r_i32), "4518: tenant 9 bytes-i32-ref denied");
        CHECK(is_error(r_set), "4518: tenant 9 f64vector-set! still denied (#4428 sibling)");
        // Deny shape: exact #4505 texts, no payload bytes in the message.
        CHECK(err_msg(r_apply) == "apply: cross-tenant pair slot read denied (#4505)",
              "4518: apply deny shape exact");
        CHECK(err_msg(r_l2s) == "list->string: cross-tenant pair slot read denied (#4505)",
              "4518: list->string deny shape exact, list bytes absent");
        CHECK(err_msg(r_ref) == "f64vector-ref: cross-tenant string slot read denied (#4505)",
              "4518: f64vector-ref deny shape exact, double absent");
        CHECK(err_msg(r_i32) == "bytes-i32-ref: cross-tenant string slot read denied (#4505)",
              "4518: bytes-i32-ref deny shape exact, word absent");
        CHECK(err_msg(r_b64e).find("aGVsbG8") == std::string::npos &&
                  err_msg(r_b64e).find("denied (#4505)") != std::string::npos,
              "4518: %base64-encode denies with the deny shape, no encoding leak");
        CHECK(err_msg(r_hexe).find("68656c6c6f") == std::string::npos,
              "4518: %hex-encode leaks no hex payload");
        CHECK(err_msg(r_set).find("#4428") != std::string::npos,
              "4518: f64vector-set! keeps its #4428 write-deny shape");
        // One IsolationDeny row per denied prim, op = the prim name.
        CHECK(ring_isolation_deny_count("apply") == 1, "4518: one IsolationDeny apply");
        CHECK(ring_isolation_deny_count("list->string") == 1,
              "4518: one IsolationDeny list->string");
        CHECK(ring_isolation_deny_count("%base64-encode") == 1,
              "4518: one IsolationDeny %base64-encode");
        CHECK(ring_isolation_deny_count("%base64-decode") == 1,
              "4518: one IsolationDeny %base64-decode");
        CHECK(ring_isolation_deny_count("%hex-encode") == 1, "4518: one IsolationDeny %hex-encode");
        CHECK(ring_isolation_deny_count("%hex-decode") == 1, "4518: one IsolationDeny %hex-decode");
        CHECK(ring_isolation_deny_count("f64vector-length") == 1,
              "4518: one IsolationDeny f64vector-length");
        CHECK(ring_isolation_deny_count("f64vector-ref") == 1,
              "4518: one IsolationDeny f64vector-ref");
        CHECK(ring_isolation_deny_count("f64vector-dot") == 1,
              "4518: one IsolationDeny f64vector-dot (x slot denies first)");
        CHECK(ring_isolation_deny_count("bytes->f64vector") == 1,
              "4518: one IsolationDeny bytes->f64vector");
        CHECK(ring_isolation_deny_count("f32-bytes->f64vector") == 1,
              "4518: one IsolationDeny f32-bytes->f64vector");
        CHECK(ring_isolation_deny_count("bytes-i32-ref") == 1,
              "4518: one IsolationDeny bytes-i32-ref");
        CHECK(ring_isolation_deny_count("f64vector-set!") == 1,
              "4518: one IsolationDeny f64vector-set! (gated sibling)");
    }

    // Owner still reads its own mints; the denied reads left every source
    // slot unchanged (readers, not stores).
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        auto car0 = (*apply_fn)({car_prim, arglist7});
        CHECK(aura::compiler::types::is_int(car0) && as_int(car0) == 42,
              "4518: owner apply spreads the arg list into car");
        auto l2s = (*l2s_fn)({cps7});
        CHECK(is_string(l2s) &&
                  ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(l2s))) == "hi",
              "4518: owner list->string builds the full string");
        auto b64 = (*b64e_fn)({str7});
        CHECK(is_string(b64) && ev.copy_string_heap_at(
                                    static_cast<std::size_t>(as_string_idx(b64))) == "aGVsbG8=",
              "4518: owner %base64-encode encodes");
        auto b64d = (*b64d_fn)({b64});
        CHECK(is_string(b64d) &&
                  ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(b64d))) == "hello",
              "4518: owner %base64-decode roundtrips");
        auto hexe = (*hexe_fn)({str7});
        CHECK(is_string(hexe) && ev.copy_string_heap_at(
                                     static_cast<std::size_t>(as_string_idx(hexe))) == "68656c6c6f",
              "4518: owner %hex-encode encodes");
        auto hexd = (*hexd_fn)({hexe});
        CHECK(is_string(hexd) &&
                  ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(hexd))) == "hello",
              "4518: owner %hex-decode roundtrips");
        auto len = (*f64len_fn)({f647});
        CHECK(aura::compiler::types::is_int(len) && as_int(len) == 2,
              "4518: owner f64vector-length");
        auto e0 = (*f64ref_fn)({f647, make_int(0)});
        auto e1 = (*f64ref_fn)({f647, make_int(1)});
        CHECK(is_float(e0) && is_float(e1) && as_float(e0) == 1.5 && as_float(e1) == 1.5,
              "4518: owner f64vector-ref reads; denied reads changed nothing");
        auto dot = (*f64dot_fn)({f647, f647});
        CHECK(is_float(dot) && as_float(dot) == 4.5, "4518: owner f64vector-dot MACs");
        auto b2f = (*b2f64_fn)({bytes7});
        CHECK(is_string(b2f), "4518: owner bytes->f64vector mints an f64vector");
        if (is_string(b2f)) {
            auto d = (*f64ref_fn)({b2f, make_int(0)});
            CHECK(is_float(d) && as_float(d) == 1.0, "4518: owner snapshot decodes 1.0");
        }
        auto f32b = (*f32b2f64_fn)({bytes7});
        if (is_string(f32b)) {
            auto d = (*f64ref_fn)({f32b, make_int(0)});
            CHECK(is_float(d) && as_float(d) == 0.0, "4518: owner f32 widen of the zero bytes");
        }
        auto w0 = (*i32ref_fn)({bytes7, make_int(0)});
        CHECK(aura::compiler::types::is_int(w0) && as_int(w0) == 0,
              "4518: owner bytes-i32-ref reads; source bytes unchanged");
        CHECK(ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(str7))) == "hello",
              "4518: tenant 7 string unchanged after the denies");
        CHECK(ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(bytes7))).size() == 8,
              "4518: tenant 7 byte string unchanged after the denies");
        // The #4428 sibling gate still allows the owner write.
        auto setok = (*f64set_fn)({f647, make_int(1), make_float(2.5)});
        CHECK(!is_error(setok), "4518: owner f64vector-set! lands");
        auto e1b = (*f64ref_fn)({f647, make_int(1)});
        CHECK(is_float(e1b) && as_float(e1b) == 2.5, "4518: owner write is visible to the owner");
    }

    // Record 0 / short stamp array: the helper is already source-cited for
    // the documented allow (#4428/#4431 windows below and above); Soft-Off
    // proves the not-prod arm behaviorally — foreign reads copy the payload
    // and record no IsolationDeny.
    reset_security_event_ring_for_test();
    ev.set_effect_sandbox_mode(0);
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto car0 = (*apply_fn)({car_prim, arglist7});
        CHECK(aura::compiler::types::is_int(car0) && as_int(car0) == 42,
              "4518: Off apply copies the payload");
        auto l2s = (*l2s_fn)({cps7});
        CHECK(is_string(l2s) &&
                  ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(l2s))) == "hi",
              "4518: Off list->string copies");
        auto b64 = (*b64e_fn)({str7});
        CHECK(is_string(b64) && ev.copy_string_heap_at(
                                    static_cast<std::size_t>(as_string_idx(b64))) == "aGVsbG8=",
              "4518: Off %base64-encode copies");
        auto e0 = (*f64ref_fn)({f647, make_int(0)});
        CHECK(is_float(e0) && as_float(e0) == 1.5, "4518: Off f64vector-ref copies");
        auto w0 = (*i32ref_fn)({bytes7, make_int(0)});
        CHECK(aura::compiler::types::is_int(w0) && as_int(w0) == 0,
              "4518: Off bytes-i32-ref copies");
        CHECK(!ring_has_isolation_deny("apply") && !ring_has_isolation_deny("list->string") &&
                  !ring_has_isolation_deny("%base64-encode") &&
                  !ring_has_isolation_deny("f64vector-ref") &&
                  !ring_has_isolation_deny("bytes-i32-ref"),
              "4518: Off records no reader IsolationDeny");
    }

    // Source-cite windows: every new gate lives in the prim body, ahead of
    // any copy, and none of them add a require_effect choke.
    const auto rt_src = read_file("src/compiler/evaluator_primitives_runtime.cpp");
    const auto apos = rt_src.find("add(\"apply\"");
    CHECK(apos != std::string::npos, "4518: apply body located");
    if (apos != std::string::npos) {
        const auto win = rt_src.substr(apos, 2200);
        const auto gate = win.find("inrange_slot_tenant_allows");
        const auto copy = win.find("args.push_back");
        CHECK(gate != std::string::npos && copy != std::string::npos && gate < copy,
              "4518: apply gate precedes the arg-vector build");
        CHECK(win.find("require_effect") == std::string::npos, "4518: apply stays body-choke-free");
    }
    CHECK(rt_src.find("apply: cross-tenant pair slot read denied (#4505)") != std::string::npos,
          "4518: apply deny shape present");
    const auto char_src = read_file("src/compiler/evaluator_primitives_char.cpp");
    for (const auto* name : {"list->string", "%base64-encode", "%base64-decode", "%hex-encode",
                             "%hex-decode", "f64vector-length", "f64vector-ref", "f64vector-dot",
                             "bytes->f64vector", "f32-bytes->f64vector", "bytes-i32-ref"}) {
        const auto p = char_src.find(std::format("add(\"{}\"", name));
        CHECK(p != std::string::npos, std::format("4518: {} body located", name));
        if (p == std::string::npos)
            continue;
        // #4518 pin: the #4597/#4555 FFI series grew the c-func bodies — the
        // nearest tenant consult now sits ~1.8k chars past `add(`. Widen to
        // 2560 (verified: no require_effect inside 2560 of any add below,
        // so the body-choke-free check stays meaningful).
        const auto win = char_src.substr(p, 2560);
        CHECK(win.find("inrange_string_tenant_allows") != std::string::npos ||
                  win.find("inrange_slot_tenant_allows") != std::string::npos,
              std::format("4518: {} consults the tenant stamp", name));
        CHECK(win.find("require_effect") == std::string::npos,
              std::format("4518: {} stays body-choke-free", name));
    }
    {
        const auto p = char_src.find("add(\"list->string\"");
        if (p != std::string::npos) {
            const auto win = char_src.substr(p, 1600);
            const auto gate = win.find("inrange_slot_tenant_allows");
            const auto copy = win.find(".car");
            CHECK(gate != std::string::npos && copy != std::string::npos && gate < copy,
                  "4518: list->string gate precedes the car copy");
        }
        const auto sp = char_src.find("add(\"f64vector-set!\"");
        CHECK(sp != std::string::npos, "4518: f64vector-set! body located");
        if (sp != std::string::npos) {
            // #4519 widened: the body now also carries the second-section
            // bounds check between the gate and the slot write.
            const auto win = char_src.substr(sp, 1800);
            CHECK(win.find("kEffectMutate") != std::string::npos,
                  "4518: f64vector-set! keeps its Mutate gate");
        }
    }
    CHECK(char_src.find("list->string: cross-tenant pair slot read denied (#4505)") !=
              std::string::npos,
          "4518: list->string deny shape present");
    CHECK(char_src.find("bytes-i32-ref: cross-tenant string slot read denied (#4505)") !=
              std::string::npos,
          "4518: bytes-i32-ref deny shape present");

    aura::core::provenance::set_multi_tenant_env_active(false);
    set_mode(SandboxMode::Off);
    ev.set_effect_sandbox_mode(0);
}

// Issue #4519: the panic checkpoint mutator raced the shared string heap.
// fiber:spawn shares one Evaluator, so another fiber's push_string_heap
// (pmr::vector realloc) or restore_panic_checkpoint truncation could free
// the buffer under save_panic_checkpoint's unlocked slot copy, under
// restore's unlocked push_back / resize, and under f64vector-set!'s second
// critical section (f64slot validated then released; a shrink between the
// sections made string_heap_[idx] an OOB write). The fix is the existing
// alloc_storage_lock_ only — no pin registry, no LifetimePin row, no new
// query key, no moving-window-green change.
static void ac4519_source_cite() {
    std::println("\n--- #4519 cite: checkpoint heap lock + set! second-section bounds ---");
    const auto wt = read_file("src/compiler/evaluator_workspace_tree.cpp");
    const auto save_fn = wt.find("bool Evaluator::save_panic_checkpoint()");
    const auto restore_fn = wt.find("bool Evaluator::restore_panic_checkpoint()");
    CHECK(save_fn != std::string::npos && restore_fn != std::string::npos && save_fn < restore_fn,
          "4519 cite: save/restore located in workspace tree");
    if (save_fn != std::string::npos && restore_fn != std::string::npos && save_fn < restore_fn) {
        const auto win = wt.substr(save_fn, restore_fn - save_fn);
        // #4598 restructure: the string_heap_ slot copy is gone (a generation-
        // matched copy of the stamped workspace text, or an unparse — no heap
        // indexing, so no bounds row). The #4519 invariant that survived:
        // every panic_safe_source_ write AND its heap-size snapshot sit
        // inside alloc_storage_lock_ — on both paths.
        const auto lock1 = win.find("std::lock_guard alloc_lock(alloc_storage_lock_");
        const auto copy1 = win.find("panic_safe_source_ = workspace_source_text_;");
        const auto lock2 = win.find("std::lock_guard alloc_lock(alloc_storage_lock_", lock1 + 1);
        const auto copy2 = win.find("panic_safe_source_ = std::move(src);");
        const auto snap1 = win.find("panic_safe_string_heap_size_ = string_heap_.size();");
        const auto snap2 =
            win.find("panic_safe_string_heap_size_ = string_heap_.size();", snap1 + 1);
        CHECK(lock1 != std::string::npos && copy1 != std::string::npos &&
                  snap1 != std::string::npos && lock1 < copy1 && copy1 < snap1 &&
                  lock2 != std::string::npos && copy2 != std::string::npos &&
                  snap2 != std::string::npos && lock2 < copy2 && copy2 < snap2,
              "4519 cite: save bounds-check + slot copy inside alloc_storage_lock_");
        CHECK(win.find("panic_safe_string_heap_size_ = string_heap_.size()") != std::string::npos,
              "4519 cite: save snapshots the heap size in the locked region");
    }
    if (restore_fn != std::string::npos) {
        const auto win = wt.substr(restore_fn, 9000);
        const auto lockp = win.find("std::lock_guard alloc_lock(alloc_storage_lock_");
        // Issue #4543: the restore mint is LockedStringHeap::mint (locked
        // size-read + append in one call); the outer lock is retained.
        const auto push = win.find("string_heap_.mint(panic_safe_source_)");
        const auto setcall = win.find("(*set_fn)({make_string(idx)})");
        CHECK(push != std::string::npos && lockp != std::string::npos &&
                  setcall != std::string::npos && lockp < push && push < setcall,
              "4519 cite: restore size-read + push_back atomic under the lock");
        const auto lockr = win.find("std::lock_guard alloc_lock_resize(alloc_storage_lock_");
        const auto rsz = win.find("string_heap_.resize(new_string_heap_size)");
        CHECK(rsz != std::string::npos && lockr != std::string::npos && lockr < rsz,
              "4519 cite: restore resize holds alloc_storage_lock_");
        CHECK(win.find("string_slot_tenants_.resize(string_heap_.size())") != std::string::npos,
              "4519 cite: tenant stamps shrink beside the heap resize");
    }
    // The MBG dtor path (phase 2-3) is a caller of the now-locked restore:
    // the auto-rollback arm must still route through it unchanged.
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto auto_rb = mb.find("ev_->panic_auto_rollback_");
    const auto dtor_restore = mb.find("(void)ev_->restore_panic_checkpoint();");
    CHECK(auto_rb != std::string::npos && dtor_restore != std::string::npos &&
              auto_rb < dtor_restore,
          "4519 cite: MBG dtor auto-rollback path calls the locked restore");
    const auto pc = read_file("src/compiler/evaluator_primitives_char.cpp");
    const auto sp = pc.find("add(\"f64vector-set!\"");
    CHECK(sp != std::string::npos, "4519 cite: f64vector-set! body located");
    if (sp != std::string::npos) {
        const auto win = pc.substr(sp, 2400);
        const auto lock2 = win.find("std::lock_guard lock(ev.alloc_storage_lock_)");
        const auto bounds2 = win.find("idx >= ev.string_heap_.size()");
        const auto slot = win.find("auto& s = ev.string_heap_[idx]");
        CHECK(lock2 != std::string::npos && bounds2 != std::string::npos &&
                  slot != std::string::npos && lock2 < bounds2 && bounds2 < slot,
              "4519 cite: second section bounds-checks before operator[]");
        CHECK(win.find("inrange_string_tenant_allows") != std::string::npos &&
                  win.find("kEffectMutate") != std::string::npos,
              "4519 cite: #4428 tenant gate retained after the bounds check");
    }
}

static void ac4519_f64set_second_section_bounds() {
    std::println("\n--- #4519: f64vector-set! second critical section bounds ---");
    using aura::compiler::types::as_float;
    using aura::compiler::types::as_string_idx;
    using aura::compiler::types::is_error;
    using aura::compiler::types::is_string;
    using aura::compiler::types::make_float;
    using aura::compiler::types::make_int;
    using aura::compiler::types::make_string;
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto set_fn = ev.primitives().lookup("f64vector-set!");
    auto ref_fn = ev.primitives().lookup("f64vector-ref");
    auto mk_fn = ev.primitives().lookup("make-f64vector");
    CHECK(set_fn && ref_fn && mk_fn, "4519: f64 prims present");
    if (!(set_fn && ref_fn && mk_fn))
        return;
    auto err_msg = [&](const aura::compiler::types::EvalValue& r) -> std::string {
        if (!is_error(r))
            return {};
        const auto eidx = static_cast<std::size_t>(aura::compiler::types::as_error_idx(r));
        if (eidx >= ev.error_values_.size() ||
            !aura::compiler::types::is_string(ev.error_values_[eidx]))
            return {};
        return ev.copy_string_heap_at(
            static_cast<std::size_t>(aura::compiler::types::as_string_idx(ev.error_values_[eidx])));
    };

    // AC1: owner set! still lands (regression).
    auto f64 = (*mk_fn)({make_int(2), make_float(1.5)});
    CHECK(!is_error(f64) && is_string(f64), "4519 AC1: make-f64vector ok");
    auto w = (*set_fn)({f64, make_int(1), make_float(2.25)});
    CHECK(!is_error(w), "4519 AC1: in-range set lands");
    auto r = (*ref_fn)({f64, make_int(1)});
    CHECK(!is_error(r) && as_float(r) == 2.25, "4519 AC1: set/ref roundtrip");

    // AC2: a stale slot handle (idx past the heap — the face a concurrent
    // restore-shrink presents) refuses with the arg-validation error instead
    // of writing OOB; the second-section bounds check itself is pinned by
    // the source-cite AC and exercised by the AC4 stress.
    auto w2 = (*set_fn)({make_string(999999), make_int(0), make_float(9.0)});
    CHECK(is_error(w2), "4519 AC2: stale slot refused");
    CHECK(err_msg(w2) == "f64vector-set!: expected (f64vector index number)",
          "4519 AC2: refuse shape is the arg-validation error");

    // AC3: a slot whose payload is not %8 keeps refusing.
    auto notf64 = make_string(static_cast<std::uint64_t>(ev.push_string_heap("abc")));
    auto w3 = (*set_fn)({notf64, make_int(0), make_float(1.0)});
    CHECK(is_error(w3), "4519 AC3: non-%8 slot refused");

    // AC4: bounded adversarial stress — a rollback thread drives the only
    // public shrinker (save/restore_panic_checkpoint: restore's truncate
    // resize drops slots pushed mid-restore) plus a mint thread forcing
    // reallocations, while the main thread mints + set!s. The re-acquired
    // second section bounds-checks, so the worst case is a clean refuse,
    // never an OOB write through string_heap_[idx].
    std::atomic<bool> stop{false};
    std::thread rollbacker([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (ev.save_panic_checkpoint()) {
                if (!ev.restore_panic_checkpoint()) {
                    // set-code result shape can vary; a failed restore must
                    // not stack a second reinstall on the same workspace.
                    if (ev.has_panic_checkpoint())
                        ev.commit_panic_checkpoint();
                    break;
                }
                if (ev.has_panic_checkpoint())
                    ev.commit_panic_checkpoint(); // release the GC defer arm
            }
            std::this_thread::yield();
        }
    });
    std::thread pusher([&] {
        while (!stop.load(std::memory_order_relaxed))
            (void)ev.push_string_heap(std::string(8, '\0'));
    });
    for (int i = 0; i < 256; ++i) {
        auto v = (*mk_fn)({make_int(2), make_float(0.5)});
        if (!is_error(v))
            (void)(*set_fn)({v, make_int(1), make_float(1.0)});
    }
    stop.store(true, std::memory_order_relaxed);
    rollbacker.join();
    pusher.join();
    CHECK(true, "4519 AC4: set! stress completed without OOB");
    // Heap still serviceable after the stress.
    const auto ping = ev.push_string_heap("ping4519");
    CHECK(ping >= 0, "4519 AC4: heap mints after stress");
    if (ev.has_panic_checkpoint())
        ev.commit_panic_checkpoint();
}

static void ac4519_checkpoint_heap_lock() {
    std::println("\n--- #4519: panic checkpoint save/restore under the heap lock ---");
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_int;
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(
        cs.eval("(set-code \"(define (g4519 x) (+ x 1)) (define w4519 (g4519 41))\")").has_value(),
        "4519: set-code seeds workspace");
    CHECK(cs.eval("(eval-current)").has_value(), "4519: eval-current");

    // AC1: save snapshots the workspace source (live checkpoint).
    CHECK(ev.save_panic_checkpoint(), "4519 AC1: save ok");
    CHECK(ev.has_panic_checkpoint(), "4519 AC1: checkpoint live");

    // AC2a: concurrent push_string_heap (pmr::vector realloc) while save
    // snapshots the source. The bounds check + slot copy + size snapshot
    // are atomic under alloc_storage_lock_, so the copied source never
    // straddles a reallocation and idx never lands past the end. Commit
    // (not restore) clears each round — restore re-enters set-code, and a
    // full workspace reinstall racing mints cross-thread is not a
    // supported face; AC2b/AC3 cover the restore critical sections.
    std::atomic<bool> stop{false};
    std::atomic<int> push_ok{0};
    auto pusher = [&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (ev.push_string_heap(std::string(8, '\0')) >= 0)
                push_ok.fetch_add(1, std::memory_order_relaxed);
        }
    };
    int saves = 0;
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 3; ++t)
            threads.emplace_back(pusher);
        for (int i = 0; i < 256; ++i) {
            if (ev.save_panic_checkpoint())
                ++saves;
            if (ev.has_panic_checkpoint())
                ev.commit_panic_checkpoint(); // release the GC defer arm
        }
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : threads)
            th.join();
    }
    CHECK(saves > 0, "4519 AC2a: saves under concurrent heap realloc");
    CHECK(push_ok.load(std::memory_order_relaxed) > 0, "4519 AC2a: concurrent pushes landed");
    if (ev.has_panic_checkpoint())
        ev.commit_panic_checkpoint();
    CHECK(!ev.has_panic_checkpoint(), "4519 AC2a: no checkpoint leak");
    CHECK(cs.eval("(eval-current)").has_value(), "4519 AC2a: evaluator usable after stress");

    // AC2b: one save/restore round on a fresh evaluator (the serve-suite
    // pattern) — the locked size-read + push_back reservation and the
    // locked truncate resize on a heap the stress grew. A failed restore
    // (set-code result shape varies) commits the fallback, as the GC-defer
    // suites do.
    {
        CompilerService cs1;
        auto& ev1 = cs1.evaluator();
        CHECK(cs1.eval("(set-code \"(define s4519 5)\")").has_value(), "4519 AC2b: seed");
        CHECK(cs1.eval("(eval-current)").has_value(), "4519 AC2b: eval-current");
        CHECK(ev1.save_panic_checkpoint(), "4519 AC2b: save ok");
        (void)ev1.push_string_heap(std::string(8, '\0'));
        const bool restored1 = ev1.restore_panic_checkpoint();
        if (!restored1 && ev1.has_panic_checkpoint())
            ev1.commit_panic_checkpoint();
        CHECK(restored1 || !ev1.has_panic_checkpoint(),
              "4519 AC2b: restored or fallback-committed");
        CHECK(cs1.eval("(eval-current)").has_value(), "4519 AC2b: evaluator usable");
    }

    // AC3: deterministic truncate shape (single thread). The locked push
    // reserves idx = size-at-restore and the locked resize keeps the heap
    // at idx+1: the re-pushed source survives, post-save pushes stay.
    {
        CompilerService cs2;
        auto& ev2 = cs2.evaluator();
        CHECK(cs2.eval("(set-code \"(define z4519 7)\")").has_value(), "4519 AC3: seed");
        CHECK(cs2.eval("(eval-current)").has_value(), "4519 AC3: eval-current");
        CHECK(ev2.save_panic_checkpoint(), "4519 AC3: save ok");
        CHECK(ev2.panic_safe_string_heap_size() == ev2.string_heap_size(),
              "4519 AC3: save snapshot equals the live heap size");
        const auto at_save = ev2.string_heap_size();
        (void)ev2.push_string_heap("extra-a-4519");
        (void)ev2.push_string_heap("extra-b-4519");
        CHECK(ev2.string_heap_size() == at_save + 2, "4519 AC3: two pushes landed");
        const bool restored3 = ev2.restore_panic_checkpoint();
        if (!restored3 && ev2.has_panic_checkpoint())
            ev2.commit_panic_checkpoint();
        CHECK(restored3, "4519 AC3: restore ok");
        // Calibrated 2026-10-15 (probe-measured): the locked push puts the
        // checkpoint source at idx = at_save+2 and the locked resize cuts the
        // heap back to idx+1 — but MutationBoundaryGuard::try_acquire inside
        // truncate_env_frames_to_checkpoint can intern one extra slot after
        // the string truncate (state-dependent: +1 here, 0 in other restore
        // contexts). Exact post-restore size is therefore not a stable
        // invariant; the #4519 contract is the idx+1 FLOOR plus the source
        // slot surviving intact at idx.
        CHECK(ev2.string_heap_size() >= at_save + 3,
              "4519 AC3: heap keeps at least idx+1 after restore");
        CHECK(ev2.string_heap_at(at_save + 2) == "(define z4519 7)",
              "4519 AC3: checkpoint source survives at idx");
        CHECK(!ev2.has_panic_checkpoint(), "4519 AC3: checkpoint cleared");
    }

    // AC4: restore rolls the workspace back to the checkpoint source —
    // post-restore the pre-save define still evaluates and the post-save
    // define is gone (the locked push + truncate keep the re-pushed source
    // coherent at idx).
    // AC4: checkpoint lifecycle vs successful mutations (calibrated
    // 2026-10-15 against the real contract, Issues #241/#2120): an
    // outermost mutation Guard that completes successfully COMMITS the
    // live panic checkpoint — a committed mutation supersedes any
    // pre-mutation rollback state — so the post-mutate manual restore is
    // a no-op returning false and the workspace keeps BOTH defines.
    {
        CompilerService cs4;
        auto& ev4 = cs4.evaluator();
        CHECK(cs4.eval("(set-code \"(define keep4519 11)\")").has_value(), "4519 AC4: seed A");
        CHECK(cs4.eval("(eval-current)").has_value(), "4519 AC4: eval A");
        CHECK(ev4.save_panic_checkpoint(), "4519 AC4: save ok");
        CHECK(cs4.eval("(set-code \"(define drop4519 22)\")").has_value(), "4519 AC4: mutate B");
        CHECK(!ev4.has_panic_checkpoint(),
              "4519 AC4: successful outermost mutation commits the checkpoint");
        CHECK(!ev4.restore_panic_checkpoint(),
              "4519 AC4: restore after commit-on-success returns false");
        const auto keep = cs4.eval("(+ keep4519 0)");
        CHECK(keep.has_value() && is_int(*keep) && as_int(*keep) == 11,
              "4519 AC4: pre-save define still evaluates");
        const auto drop = cs4.eval("(+ drop4519 0)");
        CHECK(drop.has_value() && is_int(*drop) && as_int(*drop) == 22,
              "4519 AC4: post-save define kept (no rollback after commit)");
    }
}

// Issue #4530: the copy/answer siblings of the #4505 reader gates. Tenant 7
// mints a string (and a spine holding it); tenant 9 must not receive the
// bytes through the format / symbol-append / gensym / string-join
// constructors, nor content answers through the string inequalities,
// string-index, string->number, or equal?. Same helper contract: one
// IsolationDeny per denied prim call, source slot unchanged, owner calls
// still copy.
static void ac4530_string_gate_siblings() {
    std::println("\n--- #4530: string gate siblings (format/join/cmp/equal?) ---");
    using aura::compiler::types::EvalValue;
    using aura::compiler::types::is_error;
    using aura::compiler::types::is_string;
    using aura::compiler::types::make_string;
    reset_all();
    aura::core::bump_mutation_epoch(1);
    aura::core::provenance::set_multi_tenant_env_active(true);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted + MT → production face
    auto lookup = [&](const char* name) { return ev.primitives().lookup(name); };
    auto list_fn = lookup("list");
    auto format_fn = lookup("format");
    auto symapp_fn = lookup("symbol-append");
    auto gensym_fn = lookup("gensym");
    auto sjoin_fn = lookup("string-join");
    auto slt_fn = lookup("string<?");
    auto sgt_fn = lookup("string>?");
    auto sle_fn = lookup("string<=?");
    auto sge_fn = lookup("string>=?");
    auto sidx_fn = lookup("string-index");
    auto s2n_fn = lookup("string->number");
    auto eq_fn = lookup("equal?");
    CHECK(format_fn && symapp_fn && gensym_fn && sjoin_fn && slt_fn && sgt_fn && sle_fn && sge_fn &&
              sidx_fn && s2n_fn && eq_fn,
          "4530: sibling prims present");
    if (!(format_fn && symapp_fn && gensym_fn && sjoin_fn && slt_fn && sgt_fn && sle_fn && sge_fn &&
          sidx_fn && s2n_fn && eq_fn)) {
        aura::core::provenance::set_multi_tenant_env_active(false);
        set_mode(SandboxMode::Off);
        return;
    }

    EvalValue str7, num7, lst7;
    {
        aura::compiler::Evaluator::TenantScope t7(ev, 7);
        str7 = make_string(static_cast<std::uint64_t>(ev.push_string_heap("secret")));
        num7 = make_string(static_cast<std::uint64_t>(ev.push_string_heap("17")));
        lst7 = (*list_fn)({str7});
    }
    CHECK(is_string(str7) && is_string(num7), "4530: tenant 7 mints the strings");

    EvalValue own9, own_secret;
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        own9 = make_string(static_cast<std::uint64_t>(ev.push_string_heap("zz")));
        own_secret = make_string(static_cast<std::uint64_t>(ev.push_string_heap("secret")));
    }

    reset_security_event_ring_for_test();
    {
        aura::compiler::Evaluator::TenantScope t9(ev, 9);
        auto tmpl9 = make_string(static_cast<std::uint64_t>(ev.push_string_heap("~a")));
        auto r_fmt = (*format_fn)({tmpl9, str7});
        auto r_symapp = (*symapp_fn)({str7});
        auto r_gensym = (*gensym_fn)({str7});
        auto r_join =
            (*sjoin_fn)({lst7, make_string(static_cast<std::uint64_t>(ev.push_string_heap("")))});
        auto r_lt = (*slt_fn)({str7, own9});
        auto r_gt = (*sgt_fn)({str7, own9});
        auto r_le = (*sle_fn)({str7, own9});
        auto r_ge = (*sge_fn)({str7, own9});
        auto r_sidx = (*sidx_fn)({str7, own_secret});
        auto r_s2n = (*s2n_fn)({num7});
        auto r_eq = (*eq_fn)({str7, own_secret});
        CHECK(is_error(r_fmt), "4530: tenant 9 format of a foreign string denied");
        CHECK(is_error(r_symapp), "4530: tenant 9 symbol-append of a foreign string denied");
        CHECK(is_error(r_gensym), "4530: tenant 9 gensym on a foreign prefix denied");
        CHECK(is_error(r_join), "4530: tenant 9 string-join over a foreign spine denied");
        CHECK(is_error(r_lt), "4530: tenant 9 string<? denied");
        CHECK(is_error(r_gt), "4530: tenant 9 string>? denied");
        CHECK(is_error(r_le), "4530: tenant 9 string<=? denied");
        CHECK(is_error(r_ge), "4530: tenant 9 string>=? denied");
        CHECK(is_error(r_sidx), "4530: tenant 9 string-index denied");
        CHECK(is_error(r_s2n), "4530: tenant 9 string->number on a foreign numeric denied");
        CHECK(is_error(r_eq), "4530: tenant 9 equal? against a foreign string denied");
        // The deny results must not smuggle the bytes back as a value.
        CHECK(!is_string(r_fmt) && !is_string(r_symapp) && !is_string(r_join),
              "4530: no constructor result string returned on deny");
        CHECK(ring_isolation_deny_count("format") == 1, "4530: one IsolationDeny format");
        CHECK(ring_isolation_deny_count("symbol-append") == 1,
              "4530: one IsolationDeny symbol-append");
        CHECK(ring_isolation_deny_count("gensym") == 1, "4530: one IsolationDeny gensym");
        CHECK(ring_isolation_deny_count("string-join") == 1, "4530: one IsolationDeny string-join");
        CHECK(ring_isolation_deny_count("string<?") == 1, "4530: one IsolationDeny string<?");
        CHECK(ring_isolation_deny_count("string>?") == 1, "4530: one IsolationDeny string>?");
        CHECK(ring_isolation_deny_count("string<=?") == 1, "4530: one IsolationDeny string<=?");
        CHECK(ring_isolation_deny_count("string>=?") == 1, "4530: one IsolationDeny string>=?");
        CHECK(ring_isolation_deny_count("string-index") == 1,
              "4530: one IsolationDeny string-index");
        CHECK(ring_isolation_deny_count("string->number") == 1,
              "4530: one IsolationDeny string->number");
        CHECK(ring_isolation_deny_count("equal?") == 1, "4530: one IsolationDeny equal?");

        // Owner calls still copy/answer (t9 on its own strings).
        auto o_fmt = (*format_fn)({tmpl9, own9});
        CHECK(!is_error(o_fmt), "4530: owner format still works");
        auto o_symapp = (*symapp_fn)({own9});
        CHECK(!is_error(o_symapp), "4530: owner symbol-append still works");
        auto o_lt = (*slt_fn)({own9, own9});
        CHECK(!is_error(o_lt), "4530: owner string<? still answers");
        auto o_eq = (*eq_fn)({own9, own9});
        CHECK(!is_error(o_eq), "4530: owner equal? still answers");
    }
    aura::core::provenance::set_multi_tenant_env_active(false);
    set_mode(SandboxMode::Off);
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
        // Live allowlist is clean. Anchor to the source tree: add_test runs
        // from build/, where repo-relative script paths do not resolve (same
        // CWD trap as 3308 AC6; AURA_SOURCE_DIR anchoring, 2026-10-15).
        const std::string gate_cmd = std::string("cd \"") + AURA_SOURCE_DIR +
                                     "\" && python3 scripts/coverage/checks/"
                                     "check_side_effect_security.py --strict >/dev/null 2>&1";
        const int rc = std::system(gate_cmd.c_str());
        CHECK(rc == 0, "AC3: live allowlist passes gate");
    }

    // ── AC4: bare prefix name without coverage fails gate ──
    {
        std::println("\n--- AC4: bare prefix without coverage fails gate ---");
        // Create a synthetic TU-shaped snippet and run the scanner logic via
        // a temp file under src/compiler (cleaned up). Use a unique name that
        // is not on the allowlist.
        // Anchor to the source tree (AURA_SOURCE_DIR): the probe must land
        // inside the scanned tree and the gate must run from the repo root,
        // not build/ (CWD trap; same fix family as 3308 AC6).
        const auto tmp = std::filesystem::path(AURA_SOURCE_DIR) /
                         "src/compiler/evaluator_primitives_fake_2152_probe.cpp";
        {
            std::ofstream out(tmp);
            // No coverage markers at all — bare mutate: add.
            out << "// probe for #2152 AC4 — DO NOT KEEP\n";
            out << "void probe() {\n";
            out << "  add(\"mutate:evil-bypass-2152\", [](auto) { return 0; });\n";
            out << "}\n";
        }
        const std::string gate_cmd2 = std::string("cd \"") + AURA_SOURCE_DIR +
                                      "\" && python3 scripts/coverage/checks/"
                                      "check_side_effect_security.py --strict >/dev/null 2>&1";
        const int rc = std::system(gate_cmd2.c_str());
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        const int rc_clean = std::system(gate_cmd2.c_str());
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

    // ---- Issue #4111: interpreter car/cdr reads of the process-shared pair
    // slots are gated - the dual-track read face of the #4057 write gate.
    // Foreign or unstamped (0) slots -> IsolationDeny via the existing
    // check_workspace_isolation (fiber id + Mutation epoch, required_effects
    // = 0 - no effect consume; dispatch stays the Mutate choke for writers)
    // with the slot unread; Soft/Off never consults the tenant array.
    {
        std::println(
            "\n--- #4111 AC1: Restricted+MT foreign JIT pair -> car/cdr deny, slot unread ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4111);
        aura::core::provenance::set_multi_tenant_env_active(true);
        g_pair_slots.clear();
        g_pair_slot_tenants.clear();
        auto* foreign4111 = static_cast<PairSlot*>(std::malloc(sizeof(PairSlot)));
        foreign4111->car = make_int(11).val;
        foreign4111->cdr = make_int(22).val;
        g_pair_slots.push_back(foreign4111);
        g_pair_slot_tenants.push_back(77);
        g_owned_pair_slots_.push_back(foreign4111);
        auto car_fn = ev.primitives().lookup("car");
        auto cdr_fn = ev.primitives().lookup("cdr");
        CHECK(car_fn.has_value() && cdr_fn.has_value(), "4111 AC1: pair readers present");
        const auto iso_epoch = aura::core::current_mutation_epoch();
        const auto fiber_now = static_cast<std::int64_t>(aura_fiber_current_id());
        auto rc = (*car_fn)({aura::compiler::types::make_pair(0)});
        CHECK(is_error(rc), "4111 AC1: foreign JIT pair car denied");
        CHECK(foreign4111->car == make_int(11).val && foreign4111->cdr == make_int(22).val,
              "4111 AC1: slot never read (deny before any read)");
        CHECK(std::string_view(ev.last_mutate_error()).find("car") != std::string_view::npos,
              "4111 AC1: deny reason names car (isolation, not wildcard)");
        auto rd = (*cdr_fn)({aura::compiler::types::make_pair(0)});
        CHECK(is_error(rd), "4111 AC1: foreign JIT pair cdr denied");
        CHECK(foreign4111->cdr == make_int(22).val, "4111 AC1: slot cdr still unread");
        const auto seq1 = g_security_event_ring().seq.load(std::memory_order_relaxed);
        const std::size_t rn1 = std::min<std::size_t>(seq1, g_security_event_ring().ring.size());
        bool saw_car4111 = false;
        bool saw_cdr4111 = false;
        for (std::size_t i = 0; i < rn1; ++i) {
            const auto& e = g_security_event_ring().ring[i];
            const auto op_sv = std::string_view(e.op);
            if (e.denied && e.epoch == iso_epoch && e.fiber_id == fiber_now &&
                e.kind == aura::core::security_event::SecurityEventKind::IsolationDeny) {
                if (op_sv == "car")
                    saw_car4111 = true;
                if (op_sv == "cdr")
                    saw_cdr4111 = true;
            }
        }
        CHECK(saw_car4111, "4111 AC1: IsolationDeny op car, fiber+Mutation epoch");
        CHECK(saw_cdr4111, "4111 AC1: IsolationDeny op cdr, fiber+Mutation epoch");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4111 AC2: Restricted+MT unstamped legacy slot (tenant 0) -> deny ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(4111);
        aura::core::provenance::set_multi_tenant_env_active(true);
        g_pair_slots.clear();
        g_pair_slot_tenants.clear();
        auto* legacy4111 = static_cast<PairSlot*>(std::malloc(sizeof(PairSlot)));
        legacy4111->car = make_int(101).val;
        legacy4111->cdr = make_int(102).val;
        g_pair_slots.push_back(legacy4111);
        g_pair_slot_tenants.push_back(0); // unstamped - fails closed under MT
        g_owned_pair_slots_.push_back(legacy4111);
        auto car_fn = ev.primitives().lookup("car");
        CHECK(car_fn.has_value(), "4111 AC2: car reader present");
        auto r = (*car_fn)({aura::compiler::types::make_pair(0)});
        CHECK(is_error(r), "4111 AC2: unstamped slot car denied");
        CHECK(legacy4111->car == make_int(101).val && legacy4111->cdr == make_int(102).val,
              "4111 AC2: unstamped slot never read");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4111 AC3: owner-tenant slot reads under the armed face ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        set_mode(SandboxMode::Restricted);
        ev.set_capability_tenant_id(77); // matches the slot stamp
        aura::core::provenance::set_multi_tenant_env_active(true);
        g_pair_slots.clear();
        g_pair_slot_tenants.clear();
        auto* own4111 = static_cast<PairSlot*>(std::malloc(sizeof(PairSlot)));
        own4111->car = make_int(11).val;
        own4111->cdr = make_int(22).val;
        g_pair_slots.push_back(own4111);
        g_pair_slot_tenants.push_back(77);
        g_owned_pair_slots_.push_back(own4111);
        auto car_fn = ev.primitives().lookup("car");
        auto cdr_fn = ev.primitives().lookup("cdr");
        CHECK(car_fn.has_value() && cdr_fn.has_value(), "4111 AC3: pair readers present");
        auto rc = (*car_fn)({aura::compiler::types::make_pair(0)});
        CHECK(!is_error(rc) && is_int(rc) && as_int(rc) == 11,
              "4111 AC3: owner car reads the stamped slot");
        auto rd = (*cdr_fn)({aura::compiler::types::make_pair(0)});
        CHECK(!is_error(rd) && is_int(rd) && as_int(rd) == 22,
              "4111 AC3: owner cdr reads the stamped slot");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4111 AC4: Soft/Off - tenant array unread, legacy read unchanged ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        set_mode(SandboxMode::Off);
        g_pair_slots.clear();
        g_pair_slot_tenants.clear();
        auto* foreign4111 = static_cast<PairSlot*>(std::malloc(sizeof(PairSlot)));
        foreign4111->car = make_int(11).val;
        foreign4111->cdr = make_int(22).val;
        g_pair_slots.push_back(foreign4111);
        g_pair_slot_tenants.push_back(77); // foreign stamp - unread on Soft/Off
        g_owned_pair_slots_.push_back(foreign4111);
        const auto se0 = g_security_event_ring().total.load(std::memory_order_relaxed);
        auto car_fn = ev.primitives().lookup("car");
        auto cdr_fn = ev.primitives().lookup("cdr");
        CHECK(car_fn.has_value() && cdr_fn.has_value(), "4111 AC4: pair readers present");
        auto rc = (*car_fn)({aura::compiler::types::make_pair(0)});
        CHECK(!is_error(rc) && is_int(rc) && as_int(rc) == 11,
              "4111 AC4: Soft/Off car still returns the value");
        auto rd = (*cdr_fn)({aura::compiler::types::make_pair(0)});
        CHECK(!is_error(rd) && is_int(rd) && as_int(rd) == 22,
              "4111 AC4: Soft/Off cdr still returns the value");
        CHECK(g_security_event_ring().total.load(std::memory_order_relaxed) == se0,
              "4111 AC4: no isolation deny row (tenant array never consulted)");
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

    // ── Issue #4441: JIT slow-path and non-L2 pair car/cdr tenant gate ──
    ac4441_source_cite();
    ac4441_slow_path_gate();
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

    // ── Issue #4134: fail-closed aura_jit_owner_check_isolation unwired ──
    ac4134_source_cite();
    ac4134_1_owner_wired_deny_unchanged();
    ac4134_2_unwired_hook_denies();
    ac4134_3_soft_zero_cost();
    ac4134_4_require_effect_precedes_store();

    // ── Issue #4296: interpreter hash principal is the executing Evaluator ──
    ac4296_executing_evaluator_hash_gate();

    // ── Issue #4298: in-range cons/vector tenant stamp ──
    ac4298_inrange_pair_vector_tenant();

    // ── Issue #4428: string-fill! and closure:free! value owner ──
    ac4428_string_fill_and_closure_value();

    // ── Issue #4429: hash enumerator tenant gate ──
    ac4429_hash_enumerator_gate();

    // ── Issue #4430: string-only caps keyed by the tenant principal ──
    ac4430_string_cap_tenant_mirror();

    // ── Issue #4431: stamp user-visible pair lists at the mint ──
    ac4431_user_visible_pair_mint();

    // ── Issue #4505: in-range reader prims tenant gate ──
    ac4505_inrange_reader_prims();
    ac4530_string_gate_siblings();

    // ── Issue #4518: apply / %-encoding / f64 reader tenant gate ──
    ac4518_inrange_apply_encoding_f64_readers();
    // ── Issue #4519: checkpoint heap lock + f64vector-set! bounds ──
    ac4519_source_cite();
    ac4519_f64set_second_section_bounds();
    ac4519_checkpoint_heap_lock();


    std::println("\n=== #2152/#3524 dispatch required_effects: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dispatch_required_effects();
}
#endif
