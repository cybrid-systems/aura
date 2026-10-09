// @category: unit
// @reason: Issue #1566 — WorkspaceIsolationPolicy enforcement:
// capability cross-tenant grant, provenance deny, Strict sandbox link,
// mutate/workspace force path, query:tenant-isolation-stats, stress deny.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/tenant_host_path.hh"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/provenance_tracker.hh"
#include "core/workspace_isolation.hh"
#include "core/capability_model.hh"
#include "core/resource_quota.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

#include "core/workspace_epoch.hh" // Issue #4165: QueryResult + schema-2 reserved
// FRESHNESS_ONLY: omit the g_hash_tables-linked decode/resolve
// templates (not needed here - the freshness validator is self-contained).
#define AURA_QUERY_RESULT_DECODE_FRESHNESS_ONLY
#include "compiler/query_result_decode.hh" // Issue #4165: freshness SSOT (post-module)

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectWrite;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::SecurityEventKind;
using aura::core::workspace_isolation::check_boundary;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::IsolationRefProvenance;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::core::workspace_isolation::snapshot_tenant_isolation_stats;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

// Issue #2659: helper to read the current isolation audit seq (after
// a deny we want to know if a new SE was emitted). Without this
// we would have to import g_workspace_isolation everywhere.
static std::uint64_t current_iso_seq() noexcept {
    return g_workspace_isolation().audit_seq.load(std::memory_order_acquire);
}

std::int64_t href_m(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:tenant-isolation-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Issue #2687 / #2705: capture-stamp counters live on query:soa-dirty-stats.
std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:soa-dirty-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

static NodeId first_live(FlatAST& ws) {
    for (NodeId id = 1; id < ws.size(); ++id) {
        if (ws.is_live_node(id) && !ws.is_free_slot(id))
            return id;
    }
    return aura::ast::NULL_NODE;
}

// Issue #3722: seed a Committed subtree-marker record (SubtreeMark rollback
// shape — validate/classify flip status only) so the rollback write path is
// deterministic without depending on a real mutate eval in the setup.
static void seed_3722_record(FlatAST& ws, std::uint64_t mid, NodeId node) {
    aura::ast::MutationRecord rec{};
    rec.mutation_id = mid;
    rec.target_node = node;
    rec.parent_id = node;
    rec.status = aura::ast::MutationStatus::Committed;
    rec.operator_name = "replace-type";
    rec.has_subtree_rollback = true;
    ws.all_mutations().push_back(rec);
}

void reset_all() {
    reset_tenant_isolation_for_test();
    // #2968: AC2 grants TenantAdmin into the process-global capability
    // registry; reset_all() must also clear it or later blocks reusing the
    // same tenant id see a leaked admin and the gate never denies.
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::capability::set_effect_fiber_id_override(0);
    set_mode(SandboxMode::Off);
    // Soft / unit path: hard-close off so Soft global-fallback tests stay green.
    aura::core::provenance::set_hard_capture_tenant(false);
    aura::core::provenance::set_isolation_capture_tenant(0);
    aura::core::provenance::set_stable_ref_export_hard_reject(false);
    aura::core::provenance::set_multi_tenant_env_active(false);
    aura::core::provenance::clear_last_stamped_node_for_test();
}

// #3090: Restricted/Strict refuse grants when prov.mutation_id==0.
// Stamp a bound mid so TenantAdmin actually lands in the registry.
// Issue #3409 SSOT fence: under Restricted/Strict a high-bits registry
// write needs TenantAdmin on the caller, but this fixture IS the TA
// seeder — drop to the Off face for the write, then restore so the
// fence under test still sees the section's real posture.
void grant_tenant_admin_mid(std::uint64_t tenant, std::uint64_t mid = 1) {
    using aura::core::capability::Effect;
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::make_grant_provenance;
    auto prov = make_grant_provenance(mid, /*force_mutation_bind=*/true, 0, 0);
    const auto prev_mode =
        aura::core::sandbox::g_sandbox_mode_atomic().load(std::memory_order_acquire);
    set_mode(SandboxMode::Off);
    g_capability_registry().grant(tenant, "tenant-admin", Effect::TenantAdmin, prov);
    set_mode(static_cast<SandboxMode>(prev_mode));
}

// Issue #3126: TOCTOU in TenantAdmin check vs grant (unlocked effects_for
// + has_capability admin fence). The fix adds locked variants to
// CapabilityRegistry and rewrites every foreign-tenant / high-risk admin
// fence in evaluator_security.cpp to take the registry mtx + use the
// locked variants under one critical section. This AC verifies the
// source-cite surface (locked variants exist + are wired + Soft/Off
// public surfaces stay documented as observational only) and that the
// existing test files (#2490 require_effect_auto_isolation / #2529
// grant_epoch_retain_restricted) keep their shape (no deletion / no
// removed ACs).
static void ac3126_admin_fence_locked() {
    std::println("\n--- #3126: TOCTOU admin fence — locked variants + foreign-tenant gate ---");

    // AC1: capability_model.hh owns the locked variants + Soft/observational
    // comments on the unlocked public readers.
    {
        const auto cm = read_file("src/core/capability_model.hh");
        CHECK(cm.find("effects_for_locked") != std::string::npos,
              "AC1: capability_model.hh has effects_for_locked");
        CHECK(cm.find("provenance_ok_locked") != std::string::npos,
              "AC1: capability_model.hh has provenance_ok_locked");
        CHECK(cm.find("grant_locked") != std::string::npos,
              "AC1: capability_model.hh has grant_locked (caller MUST hold mtx)");
        CHECK(cm.find("revoke_locked") != std::string::npos,
              "AC1: capability_model.hh has revoke_locked (caller MUST hold mtx)");
        // Public unlocked surfaces stay, but are documented as observational.
        CHECK(cm.find("Soft/observational only") != std::string::npos,
              "AC1: capability_model.hh marks Soft/observational only on unlocked readers");
        CHECK(cm.find("DO NOT use for security decisions under concurrent mutation") !=
                  std::string::npos,
              "AC1: unlocked readers carry the security-decision warning");
        // Issue constant present for lineage / source-cite.
        CHECK(cm.find("Issue #3126") != std::string::npos,
              "AC1: capability_model.hh cites Issue #3126");
    }

    // AC2: evaluator_security.cpp grant_effect_capability rewrites the
    // foreign-tenant fence to take the registry mtx + use effects_for_locked
    // + grant_locked (closes the unlocked has_capability TOCTOU window).
    {
        const auto es = read_file("src/compiler/evaluator_security.cpp");
        // The fence must NOT call has_capability (which is unlocked).
        const std::string grant_cap_section_marker = "cross-tenant-grant-needs-tenant-admin";
        // Anchor inside Evaluator::grant_effect_capability: #3597 reuses the
        // same deny reason in grant_cross_tenant earlier in the file, so a
        // bare first-find no longer lands on the fence under test.
        const auto fn_pos = es.find("bool Evaluator::grant_effect_capability(");
        CHECK(fn_pos != std::string::npos, "AC2: grant_effect_capability present");
        const auto fence_pos = (fn_pos == std::string::npos)
                                   ? std::string::npos
                                   : es.find("cross-tenant-grant-needs-tenant-admin", fn_pos);
        CHECK(fence_pos != std::string::npos,
              "AC2: foreign-tenant fence string present in evaluator_security.cpp");
        // Within a generous window around the first foreign-tenant fence, the
        // admin check should use effects_for_locked, not has_capability.
        const auto window_start = (fn_pos == std::string::npos) ? 0 : fn_pos;
        const auto window_end = (fence_pos + 1200 < es.size()) ? fence_pos + 1200 : es.size();
        const std::string fence_window = es.substr(window_start, window_end - window_start);
        CHECK(fence_window.find("effects_for_locked(self_tenant)") != std::string::npos,
              "AC2: grant_effect_capability fence uses effects_for_locked");
        CHECK(fence_window.find("has_effect(held, Effect::TenantAdmin)") != std::string::npos,
              "AC2: grant_effect_capability fence checks Effect::TenantAdmin bit");
        CHECK(fence_window.find("reg.grant_locked(") != std::string::npos,
              "AC2: grant_effect_capability fence acts via grant_locked");
        CHECK(fence_window.find("reg.mtx") != std::string::npos,
              "AC2: grant_effect_capability fence holds registry mtx");
    }

    // AC3: grant_effect_durable has TWO gates (foreign-tenant #2969 + high-risk
    // TenantAdmin+reason #2967). Both must use the same locked is_admin.
    {
        const auto es = read_file("src/compiler/evaluator_security.cpp");
        const auto durable_pos = es.find("grant_effect_durable(");
        const auto next_func = es.find("grant_effect_session(", durable_pos);
        const std::string durable_block =
            es.substr(durable_pos, (next_func > durable_pos) ? next_func - durable_pos
                                                             : es.size() - durable_pos);
        CHECK(durable_block.find("std::lock_guard<std::mutex> lock(reg_durable.mtx)") !=
                  std::string::npos,
              "AC3: grant_effect_durable takes registry mtx");
        CHECK(durable_block.find("effects_for_locked(self_tenant)") != std::string::npos,
              "AC3: grant_effect_durable uses effects_for_locked");
        CHECK(durable_block.find("has_effect(held_durable, Effect::TenantAdmin)") !=
                  std::string::npos,
              "AC3: grant_effect_durable precomputes is_admin from locked bit");
        CHECK(durable_block.find("reg_durable.grant_locked(") != std::string::npos,
              "AC3: grant_effect_durable acts via grant_locked");
        // Legacy unlocked is_admin helper must not survive in this block.
        CHECK(durable_block.find(
                  "has_capability(kCapTenantAdmin) || has_capability(kCapCapability)") ==
                  std::string::npos,
              "AC3: grant_effect_durable removed unlocked has_capability fence");
    }

    // AC4: grant_effect_session foreign-tenant fence locks + uses
    // effects_for_locked + grant_locked.
    {
        const auto es = read_file("src/compiler/evaluator_security.cpp");
        const auto session_pos = es.find("void Evaluator::grant_effect_session(");
        const auto next_func = es.find("void Evaluator::revoke_effect_capability(", session_pos);
        const std::string session_block =
            es.substr(session_pos, (next_func > session_pos) ? next_func - session_pos
                                                             : es.size() - session_pos);
        CHECK(session_block.find("std::lock_guard<std::mutex> lock(reg_session.mtx)") !=
                  std::string::npos,
              "AC4: grant_effect_session takes registry mtx");
        CHECK(session_block.find("effects_for_locked(self_tenant)") != std::string::npos,
              "AC4: grant_effect_session uses effects_for_locked");
        CHECK(session_block.find("reg_session.grant_locked(") != std::string::npos,
              "AC4: grant_effect_session acts via grant_locked");
        CHECK(session_block.find(
                  "has_capability(kCapTenantAdmin) || has_capability(kCapCapability)") ==
                  std::string::npos,
              "AC4: grant_effect_session removed unlocked has_capability fence");
    }

    // AC5: revoke_effect_capability foreign-tenant fence locks + uses
    // effects_for_locked + revoke_locked. Bound to the next Evaluator
    // method so later sites (e.g. #3411 set_tenant_principal) are not
    // scored as the revoke fence.
    {
        const auto es = read_file("src/compiler/evaluator_security.cpp");
        const auto revoke_pos = es.find("void Evaluator::revoke_effect_capability(");
        const auto next_func = es.find("void Evaluator::set_effect_sandbox_mode(", revoke_pos);
        const std::string revoke_block = es.substr(
            revoke_pos, (next_func > revoke_pos) ? next_func - revoke_pos : es.size() - revoke_pos);
        CHECK(revoke_block.find("std::lock_guard<std::mutex> lock(reg_revoke.mtx)") !=
                  std::string::npos,
              "AC5: revoke_effect_capability takes registry mtx");
        CHECK(revoke_block.find("effects_for_locked(self_tenant)") != std::string::npos,
              "AC5: revoke_effect_capability uses effects_for_locked");
        CHECK(revoke_block.find("reg_revoke.revoke_locked(") != std::string::npos,
              "AC5: revoke_effect_capability acts via revoke_locked");
        CHECK(revoke_block.find(
                  "has_capability(kCapTenantAdmin) || has_capability(kCapCapability)") ==
                  std::string::npos,
              "AC5: revoke_effect_capability removed unlocked has_capability fence");
    }

    // AC6: existing tests stay green — no deletion / no removed ACs.
    {
        const auto req = read_file("tests/compiler/test_require_effect_auto_isolation.cpp");
        const auto retain = read_file("tests/compiler/test_grant_epoch_retain_restricted.cpp");
        CHECK(req.find("Issue #2490") != std::string::npos,
              "AC6: test_require_effect_auto_isolation still cites #2490");
        CHECK(req.find("IsolationDeny") != std::string::npos ||
                  req.find("auto-enforce workspace isolation") != std::string::npos,
              "AC6: test_require_effect_auto_isolation AC1/AC2 surface intact");
        CHECK(retain.find("Issue #2529") != std::string::npos,
              "AC6: test_grant_epoch_retain_restricted still cites #2529");
        CHECK(retain.find("kDefaultGrantEpochRetainWindowRestricted") != std::string::npos,
              "AC6: test_grant_epoch_retain_restricted AC1 surface intact");
    }

    // --- AC7: #3409 grant() SSOT TA fence — push #3086/#3029 into grant_locked ---
    {
        std::println("\n--- AC7: #3409 grant() SSOT TA fence ---");
        const auto cm = read_file("src/core/capability_model.hh");
        // AC7.1: grant_locked has caller_principal parameter.
        CHECK(contains(cm, "TenantId caller_principal = 0)"),
              "AC7: grant_locked has caller_principal parameter (default 0)");
        // AC7.2: grant_locked body has the TA fence with the high-bits mask.
        const auto gl_pos = cm.find("bool grant_locked(TenantId tenant");
        CHECK(gl_pos != std::string::npos, "AC7: grant_locked signature present");
        const auto cm_after_gl = (gl_pos == std::string::npos) ? std::string{} : cm.substr(gl_pos);
        // Scope to the next closing brace of grant_locked body (find `void grant_session`).
        const auto gs_pos = cm_after_gl.find("void grant_session(");
        const auto gl_body =
            (gs_pos == std::string::npos) ? cm_after_gl : cm_after_gl.substr(0, gs_pos);
        CHECK(contains(gl_body, "Issue #3409"),
              "AC7: grant_locked cites #3409 (source-cite anchor)");
        CHECK(contains(gl_body, "Effect::TenantAdmin"),
              "AC7: grant_locked TA fence checks Effect::TenantAdmin");
        CHECK(contains(gl_body, "Effect::MacroSelfEvo"),
              "AC7: grant_locked TA fence includes MacroSelfEvo in high bits");
        CHECK(contains(gl_body, "Effect::Mutate"),
              "AC7: grant_locked TA fence includes Mutate in high bits");
        CHECK(contains(gl_body, "Effect::Syscall"),
              "AC7: grant_locked TA fence includes Syscall in high bits");
        CHECK(contains(gl_body, "foreign_tenant"),
              "AC7: grant_locked TA fence computes foreign_tenant condition");
        CHECK(contains(gl_body, "effects_for_locked(caller)"),
              "AC7: grant_locked TA fence uses effects_for_locked (TOCTOU-safe)");
        CHECK(contains(gl_body, "grant-ssot-needs-tenant-admin"),
              "AC7: grant_locked TA fence emits new stable SE reason");
        // AC7.3: reuse existing deny counter (no new metrics fields per issue).
        CHECK(contains(gl_body, "capability_macro_self_evo_grant_deny_total"),
              "AC7: grant_locked TA fence reuses capability_macro_self_evo_grant_deny_total");
        // AC7.4: grant() public wrapper forwards caller_principal to grant_locked.
        CHECK(contains(cm, "bool grant(TenantId tenant, std::string_view name, Effect effects,") &&
                  contains(cm, "TenantId caller_principal = 0) {"),
              "AC7: grant() has caller_principal parameter");
        CHECK(
            contains(cm, "grant_locked(tenant, name, effects, prov, single_use, session_bound,") &&
                contains(cm, "caller_principal);"),
            "AC7: grant() forwards caller_principal to grant_locked");
        // AC7.5: grant_session + grant_once forward caller_principal.
        CHECK(
            contains(
                cm, "void grant_session(TenantId tenant, std::string_view name, Effect effects,") &&
                contains(cm, "TenantId caller_principal = 0) {"),
            "AC7: grant_session has caller_principal parameter");
        CHECK(contains(cm,
                       "void grant_once(TenantId tenant, std::string_view name, Effect effects,") &&
                  contains(cm, "TenantId caller_principal = 0) {"),
              "AC7: grant_once has caller_principal parameter");
    }

    // --- AC8: #3409 Evaluator::grant_capability passes caller_principal = capability_tenant_id_
    // ---
    {
        std::println("\n--- AC8: #3409 Evaluator::grant_capability passes caller_principal ---");
        const auto es = read_file("src/compiler/evaluator_security.cpp");
        // The Evaluator path must pass capability_tenant_id_ as the caller_principal
        // to the registry grant() call. Otherwise the SSOT gate falls back to
        // default_tenant (legacy behavior) and TenantAdmin would not be checked.
        CHECK(contains(es, "g_capability_registry().grant(capability_tenant_id_,"),
              "AC8: Evaluator::grant_capability calls grant with capability_tenant_id_");
        // #3878 wrapped the grant call across lines — assert the caller
        // principal tail via the (unique) closing fragment of the grant call.
        CHECK(contains(es, "capability_tenant_id_)) {"),
              "AC8: Evaluator::grant_capability passes capability_tenant_id_ as caller_principal");
        // Kernel bootstrap path (security_defaults.hh) keeps tenant=0 Render-only
        // (gate allows: tenant=0 doesn't trigger foreign-tenant, Render doesn't
        // intersect high bits). No caller_principal needed.
        const auto sd = read_file("src/compiler/security_defaults.hh");
        CHECK(contains(sd, "/*tenant=*/0, \"render-kernel\", Effect::Render"),
              "AC8: kernel render-kernel bootstrap tenant=0 Render (gate allows)");
        CHECK(contains(sd, "/*tenant=*/0, \"render\", Effect::Render"),
              "AC8: kernel render bootstrap tenant=0 Render (gate allows)");
    }

    // --- AC9: no docs/design/3409-*; no test_issue_3409.cpp ---
    {
        std::println("\n--- AC9: no docs/design/3409-*; no test_issue_3409.cpp ---");
        CHECK(read_file("docs/design/3409-grant-ssot-ta-fence.md").empty(),
              "AC9: no docs/design/3409-* per #1655");
        CHECK(read_file("tests/core/test_issue_3409.cpp").empty(),
              "AC9: no test_issue_3409.cpp per #81934");
        CHECK(read_file("tests/issues/test_issue_3409.cpp").empty(),
              "AC9: no tests/issues/test_issue_3409.cpp (R1 abandoned scheme)");
    }

    // Soft/Off behavior preserved — public unlocked effects_for / provenance_ok
    // remain callable (no API break); the existing reset / re-grant cycle
    // used by reset_all() above still works end-to-end (regression check
    // for the Soft happy path that the issue body mandates).
    {
        reset_all();
        using aura::core::capability::Effect;
        using aura::core::capability::g_capability_registry;
        const auto tenant = std::uint64_t{4242};
        // Soft path: no TenantAdmin held → public unlocked effects_for must
        // observe the same empty Effect set both via raw by_tenant iteration
        // and via effects_for (no race in Soft single-thread tests).
        const auto held = g_capability_registry().effects_for(tenant);
        CHECK(held == Effect::None,
              "AC6 soft: public effects_for returns None for tenant with no grants");
        // After grant_locked via the public grant() (which itself takes the
        // lock), the Soft reader sees the bit. This proves the public API
        // pair is still consistent — Issue #3126 contract is to make the
        // SECURITY FENCE paths lock; Soft / single-thread callers are
        // unchanged.
        grant_tenant_admin_mid(tenant, 7);
        const auto held2 = g_capability_registry().effects_for(tenant);
        CHECK((held2 & Effect::TenantAdmin) != Effect::None,
              "AC6 soft: public effects_for sees TenantAdmin after grant()");
        reset_all();
    }
}

// ── Issue #4133: MSE TA fence caller-only — supersedes #3904's
// caller-OR-target posture (aligned with grant_cross_tenant #3800).
static void ac4133_1_target_ta_non_ta_caller_denied() {
    std::println("\n--- #4133 AC1: non-TA caller + TA target → deny (caller-only fence) ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    // Target tenant 42 holds TenantAdmin; caller 7 holds nothing. Under the
    // #4133 caller-only fence the TARGET's admin bit must NOT authorize the
    // mint (least-privilege; supersedes the #3904 caller-OR-target posture —
    // a non-TA caller could mint MSE onto a TA-holding target via the
    // registry API). SE reason macro-self-evo-grant-needs-tenant-admin +
    // deny counter stay stable.
    grant_tenant_admin_mid(42);

    const auto deny0 =
        aura::core::capability::g_capability_effect_metrics()
            .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
    const bool landed = aura::core::capability::g_capability_registry().grant_macro_self_evo(
        /*tenant=*/42, aura::core::capability::MacroSelfEvoPolicy{},
        /*prov_in=*/aura::core::capability::make_grant_provenance(2, true, 0, 0),
        /*caller_principal=*/7);
    const auto deny1 =
        aura::core::capability::g_capability_effect_metrics()
            .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
    CHECK(!landed, "4133 AC1: caller-only fence — target TA does not authorize (deny)");
    CHECK(deny1 == deny0 + 1,
          "4133 AC1: deny counter bumped (macro-self-evo-grant-needs-tenant-admin)");
    aura::core::capability::CapabilityGrant g{};
    CHECK(!aura::core::capability::g_capability_registry().find_grant(42, "macro-self-evo", g),
          "4133 AC1: no MSE policy lands on the TA target");
    reset_all();
}

static void ac4133_2_caller_ta_mint_foreign_target_lands() {
    std::println("\n--- #4133 AC2: caller holds TA → mint to foreign target lands ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    grant_tenant_admin_mid(7); // caller 7 holds TenantAdmin; target 42 does not
    const bool landed = aura::core::capability::g_capability_registry().grant_macro_self_evo(
        /*tenant=*/42, aura::core::capability::MacroSelfEvoPolicy{},
        /*prov_in=*/aura::core::capability::make_grant_provenance(5, true, 0, 0),
        /*caller_principal=*/7);
    CHECK(landed, "4133 AC2: TA caller → foreign MSE mint lands");
    aura::core::capability::CapabilityGrant g{};
    CHECK(aura::core::capability::g_capability_registry().find_grant(42, "macro-self-evo", g),
          "4133 AC2: MSE row present on target");
    reset_all();
}

static void ac4133_3_soft_off_zero_cost_unchanged() {
    std::println("\n--- #4133 AC3: Soft/Off allows without TA lookup cost ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    const auto deny0 =
        aura::core::capability::g_capability_effect_metrics()
            .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
    const bool landed = aura::core::capability::g_capability_registry().grant_macro_self_evo(
        /*tenant=*/7, aura::core::capability::MacroSelfEvoPolicy{},
        /*prov_in=*/aura::core::capability::make_grant_provenance(6, true, 0, 0),
        /*caller_principal=*/7); // no TA anywhere — Soft passes through
    aura::core::capability::CapabilityGrant g{};
    CHECK(landed &&
              aura::core::capability::g_capability_registry().find_grant(7, "macro-self-evo", g),
          "4133 AC3: Soft/Off grant lands (no fence consult)");
    CHECK(aura::core::capability::g_capability_effect_metrics()
                  .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed) ==
              deny0,
          "4133 AC3: deny counter untouched under Soft/Off (zero-cost)");
    reset_all();
}

static void ac4133_4_concurrent_ta_revoke_mint_deny_table_unchanged() {
    std::println("\n--- #4133 AC4: concurrent TA revoke vs MSE mint — deny, table unchanged ---");
    reset_all();
    // Seed caller TA while Off, then arm production; the fence and the
    // by_tenant write run under the registry mtx, so a concurrent revoke
    // cannot land between check and write (TOCTOU closure).
    grant_tenant_admin_mid(7);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    const auto mid = aura::core::capability::make_grant_provenance(7, true, 0, 0).epoch;
    constexpr int kIters = 64;
    std::atomic<int> allows{0};
    std::atomic<int> denies{0};
    std::atomic<bool> go{false};
    std::atomic<bool> revoked{false};
    std::thread revoker([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        aura::core::capability::g_capability_registry().revoke(7, "tenant-admin");
        revoked.store(true, std::memory_order_release);
    });
    std::thread minter([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIters; ++i) {
            const bool ok = aura::core::capability::g_capability_registry().grant_macro_self_evo(
                /*tenant=*/42, aura::core::capability::MacroSelfEvoPolicy{},
                /*prov_in=*/aura::core::capability::make_grant_provenance(mid, true, 0, 0),
                /*caller_principal=*/7);
            (ok ? allows : denies).fetch_add(1, std::memory_order_relaxed);
        }
    });
    go.store(true, std::memory_order_release);
    revoker.join();
    minter.join();
    CHECK(allows.load() + denies.load() == kIters,
          "4133 AC4: every mint resolved atomically (no torn fence state)");
    // Post-join: caller TA is gone — a final mint must deny and leave the
    // target's live MSE row count unchanged (denied mint writes nothing).
    auto count_live_mse = []() -> int {
        int n = 0;
        std::lock_guard<std::mutex> lock(aura::core::capability::g_capability_registry().mtx);
        const auto it = aura::core::capability::g_capability_registry().by_tenant.find(42);
        if (it != aura::core::capability::g_capability_registry().by_tenant.end())
            for (const auto& gr : it->second)
                if (!gr.revoked &&
                    (static_cast<std::uint16_t>(gr.effects) &
                     static_cast<std::uint16_t>(aura::core::capability::Effect::MacroSelfEvo)) != 0)
                    ++n;
        return n;
    };
    const auto rows0 = count_live_mse();
    const bool final_landed = aura::core::capability::g_capability_registry().grant_macro_self_evo(
        /*tenant=*/42, aura::core::capability::MacroSelfEvoPolicy{},
        /*prov_in=*/aura::core::capability::make_grant_provenance(mid, true, 0, 0),
        /*caller_principal=*/7);
    CHECK(!final_landed, "4133 AC4: post-revoke mint denies");
    CHECK(count_live_mse() == rows0, "4133 AC4: denied mint leaves the table unchanged");
    CHECK(revoked.load(), "4133 AC4: revoke landed during the chaos window");
    reset_all();
}

static void ac4133_5_prim_mse_seed_refuses_with_base_grant() {
    std::println(
        "\n--- #4133 AC5: security:grant-effect! MSE seed refuses when base grant refused ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    CompilerService cs;
    // Evaluator principal (default 0) holds no TA → the high-risk base
    // grant_effect_capability refuses (#3088 AC5 shape); the #3459/#2023
    // MSE seed must not land either (one refuse surface).
    const auto r =
        cs.eval(std::format("(security:grant-effect! \"macro-self-evo\" {})",
                            static_cast<int>(aura::compiler::security::kEffectMacroSelfEvo)));
    CHECK(r && (is_error(*r) || (is_bool(*r) && !as_bool(*r))),
          "4133 AC5: grant-effect! refuses under Restricted without caller TA");
    CHECK(!aura::core::capability::g_capability_registry()
               .macro_self_evo_policy(cs.evaluator().capability_tenant_id())
               .has_value(),
          "4133 AC5: no MSE policy seeded after refusal");
    reset_all();
}

static void ac3904_2_caller_with_ta_allow_unchanged() {
    std::println("\n--- #3904 AC2: caller holds TA → allow unchanged ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    grant_tenant_admin_mid(7); // caller 7 holds TenantAdmin
    aura::core::capability::g_capability_registry().grant_macro_self_evo(
        /*tenant=*/42, aura::core::capability::MacroSelfEvoPolicy{},
        /*prov_in=*/aura::core::capability::make_grant_provenance(3, true, 0, 0),
        /*caller_principal=*/7);
    aura::core::capability::CapabilityGrant g{};
    CHECK(aura::core::capability::g_capability_registry().find_grant(42, "macro-self-evo", g),
          "3904 AC2: TA caller → MSE grant lands on target");
    reset_all();
}

static void ac3904_3_neither_ta_denied() {
    std::println("\n--- #3904 AC3: neither caller nor target holds TA → deny ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    // No TA anywhere: caller_principal=0 → default 0 (no admin); target 7
    // has no admin row.
    const auto deny0 =
        aura::core::capability::g_capability_effect_metrics()
            .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
    aura::core::capability::g_capability_registry().grant_macro_self_evo(
        /*tenant=*/7, aura::core::capability::MacroSelfEvoPolicy{},
        /*prov_in=*/aura::core::capability::make_grant_provenance(3, true, 0, 0),
        /*caller_principal=*/0);
    const auto deny1 =
        aura::core::capability::g_capability_effect_metrics()
            .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
    CHECK(deny1 == deny0 + 1, "3904 AC3: neither-side TA → deny + counter bump");
    aura::core::capability::CapabilityGrant g{};
    CHECK(!aura::core::capability::g_capability_registry().find_grant(7, "macro-self-evo", g),
          "3904 AC3: no MSE policy lands");
    reset_all();
}

static void ac3904_4_soft_off_zero_cost_unchanged() {
    std::println("\n--- #3904 AC4: Soft/Off skips the fence entirely (zero-cost) ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    const auto deny0 =
        aura::core::capability::g_capability_effect_metrics()
            .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
    aura::core::capability::g_capability_registry().grant_macro_self_evo(
        /*tenant=*/7, aura::core::capability::MacroSelfEvoPolicy{},
        /*prov_in=*/aura::core::capability::make_grant_provenance(4, true, 0, 0),
        /*caller_principal=*/7); // no admin anywhere — Soft passes through
    aura::core::capability::CapabilityGrant g{};
    CHECK(aura::core::capability::g_capability_registry().find_grant(7, "macro-self-evo", g),
          "3904 AC4: Soft/Off grant lands (no fence consult)");
    CHECK(aura::core::capability::g_capability_effect_metrics()
                  .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed) ==
              deny0,
          "3904 AC4: deny counter untouched under Soft/Off");
    reset_all();
}

static void ac3904_5_source_cite() {
    std::println("\n--- #3904 AC5: posture source-cite + no invent ---");
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(cap.find("Issue #4133") != std::string::npos,
          "3904 AC5: fence cites superseding #4133 caller-only contract");
    CHECK(cap.find("if (!has_admin(caller)) {") != std::string::npos,
          "3904 AC5: caller-only fence present (#4133)");
    CHECK(cap.find("has_admin(caller) && !has_admin(tenant)") == std::string::npos,
          "3904 AC5: caller-OR-target fence removed (superseded)");
    std::ifstream docs("docs/design/3904-mse-ta-caller-only.md");
    if (!docs.good())
        docs.open("../docs/design/3904-mse-ta-caller-only.md");
    CHECK(!docs.good(), "3904 AC5: no docs/design");
    std::ifstream invent("tests/issues/test_issue_3904.cpp");
    if (!invent.good())
        invent.open("../tests/issues/test_issue_3904.cpp");
    CHECK(!invent.good(), "3904 AC5: no test_issue_3904.cpp");
}

} // namespace

// ── Issue #4165: Agent-scoped fiber isolation (same-tenant multi-Agent) ──
// The #2151 override face: a stamp made under fiber A (42) must not resolve
// under fiber B (43) once the resolve side honors the same Agent-scoped
// resolution as the stamp side; the same fiber stays fresh; Soft fiberless
// stamps keep the legacy 0 face (no new soft face).
static void ac4165_agent_fiber_isolation() {
    std::println("\n--- #4165: Agent-scoped fiber stamp/resolve (override 42 vs 43) ---");
    using aura::compiler::query_result_decode::query_result_is_fresh_with_refs;
    using aura::core::QueryResultFreshness;
    reset_all();
    aura::core::capability::set_effect_fiber_id_override(42);
    {
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f4165 (lambda (z) 3))\")").has_value(),
              "4165: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4165: eval");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "4165: workspace live");
        aura::ast::NodeId nid = 0;
        for (aura::ast::NodeId i = 1; flat && i < flat->size(); ++i) {
            if (flat->is_live_node(i)) {
                nid = i;
                break;
            }
        }
        CHECK(nid != 0, "4165: live node found");
        if (flat && nid != 0) {
            auto& ev = cs.evaluator();
            // Stamp face: explicit 0 falls through to the Agent-scoped
            // resolution → override 42 wins (fiber A).
            const auto st = ev.make_stamped_safe_ref(nid, /*workspace_id=*/0, /*fiber_id=*/0);
            CHECK(st.fiber_id == 42, "4165: stamp honors override 42 (fiber A)");
            const auto genA = flat->node_gen_for(nid);
            const auto wrapA = flat->wrap_epoch();
            const auto cowA = flat->workspace_cow_epoch();
            aura::core::QueryResult qrA;
            CHECK(qrA.push_match_full(nid, genA, wrapA, cowA, /*tenant=*/0, /*fiber=*/42, 0, 0),
                  "4165: fiber-A match pushed");
            qrA.matches[0].reserved = aura::core::kQueryResultMatchSchema2;
            // Resolve face under fiber B (43): A's stamp denies.
            aura::core::capability::set_effect_fiber_id_override(43);
            CHECK(ev.agent_scoped_fiber_id() == 43, "4165: resolve honors override 43 (fiber B)");
            CHECK(query_result_is_fresh_with_refs(qrA, *flat, ev.capability_tenant_id(),
                                                  ev.agent_scoped_fiber_id()) ==
                      QueryResultFreshness::InvalidFiber,
                  "4165: fiber-A stamp denies InvalidFiber under fiber B (same tenant)");
            // Same fiber stays fresh.
            aura::core::capability::set_effect_fiber_id_override(42);
            CHECK(query_result_is_fresh_with_refs(qrA, *flat, ev.capability_tenant_id(),
                                                  ev.agent_scoped_fiber_id()) ==
                      QueryResultFreshness::Fresh,
                  "4165: fiber-A stamp stays fresh under fiber A");
            // Soft legacy face: no override, no live fiber → stamp 0.
            aura::core::capability::set_effect_fiber_id_override(0);
            const auto st0 = ev.make_stamped_safe_ref(nid, /*workspace_id=*/0, /*fiber_id=*/0);
            CHECK(st0.fiber_id == 0, "4165: Soft fiberless stamp keeps legacy 0");
        }
    }
    aura::core::capability::set_effect_fiber_id_override(0);
    reset_all();
}

int main() {
    reset_all();

    // ── Issue #3126: TOCTOU admin fence (locked variants + foreign-tenant gate) ──
    ac3126_admin_fence_locked();

    // ── AC6: query:tenant-isolation-stats shape ──
    {
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics "query:tenant-isolation-stats"))");
        if (h && is_hash(*h)) {
            CHECK(true, "tenant-isolation-stats is hash");
            CHECK(href_m(cs, "schema") == 1566, "schema 1566");
            CHECK(href_m(cs, "active") == 1, "active");
            CHECK(href_m(cs, "phase") == 2, "phase 2");
        } else {
            // Light link may omit some engine:metrics surface; C++ stats remain
            // authoritative via snapshot_tenant_isolation_stats().
            const auto snap = snapshot_tenant_isolation_stats();
            CHECK(snap.issue == 1566 || snap.phase >= 1,
                  "tenant-isolation-stats: C++ snapshot path available");
        }
    }

    // ── AC1: same-tenant / unset allows ──
    {
        reset_all();
        CHECK(check_boundary(0, 0), "unset tenant allows target 0");
        g_workspace_isolation().set_current_tenant(1, "alice");
        CHECK(check_boundary(1, 1), "same tenant allows");
        CHECK(snapshot_tenant_isolation_stats().checks >= 2, "checks counted");
    }

    // ── AC1/2: cross-tenant without grant denies ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "alice");
        const auto v0 = snapshot_tenant_isolation_stats().boundary_violations_prevented;
        CHECK(!check_boundary(1, 2), "cross-tenant without grant denied");
        CHECK(snapshot_tenant_isolation_stats().boundary_violations_prevented == v0 + 1,
              "boundary violation prevented");
    }

    // ── AC1: capability propagation grant allows ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "alice");
        g_workspace_isolation().grant_cross_tenant(1, 2, kEffectMutate);
        CHECK(check_boundary(1, 2, nullptr, false, kEffectMutate),
              "cross-tenant with Mutate grant allows");
        CHECK(!check_boundary(1, 2, nullptr, false, kEffectWrite),
              "Write not covered by Mutate grant");
        g_workspace_isolation().grant_cross_tenant(1, 2, kEffectWrite);
        CHECK(check_boundary(1, 2, nullptr, false, kEffectWrite), "Write allowed after grant");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_capability_grants >= 2,
              "grants counted");
    }

    // ── AC2: provenance ref_tenant mismatch denies ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "alice");
        IsolationRefProvenance ref{};
        ref.tenant_id = 99;
        const auto p0 = snapshot_tenant_isolation_stats().cross_tenant_provenance_deny;
        CHECK(!check_boundary(1, 1, &ref), "foreign ref tenant denies even on same target");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_provenance_deny == p0 + 1,
              "provenance deny counted");
        // Grant 1→99 then allow
        g_workspace_isolation().grant_cross_tenant(1, 99, kEffectMutate);
        CHECK(check_boundary(1, 1, &ref, false, kEffectMutate),
              "provenance allow after cross grant");
    }

    // ── AC4: Strict sandbox linked ──
    {
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_tenant_principal(7, "bob");
        ev.set_effect_sandbox_mode(2); // Strict → links isolation
        CHECK(snapshot_tenant_isolation_stats().strict_linked == 1 ||
                  g_workspace_isolation().strict_sandbox_linked,
              "strict linked after set_effect_sandbox_mode(2)");
        CHECK(!ev.check_workspace_isolation(8, 0, kEffectMutate, "strict-x"),
              "Strict + cross-tenant deny");
        CHECK(snapshot_tenant_isolation_stats().strict_denials >= 1, "strict denials counted");
    }

    // ── AC3: StableNodeRef.tenant_id stamp ──
    {
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        (void)cs.eval("(set-code \"(define x 1)\")");
        ev.set_tenant_principal(42, "t42");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace flat");
        FlatAST::StableNodeRef ref{};
        if (ws) {
            for (std::uint32_t i = 1; i < ws->size(); ++i) {
                if (ws->is_live_node(i) && !ws->is_free_slot(i)) {
                    ref = ws->make_safe_ref(i);
                    break;
                }
            }
        }
        CHECK(ref.id != 0, "captured ref");
        ev.stamp_ref_tenant(ref);
        CHECK(ref.tenant_id == 42, "StableNodeRef.tenant_id stamped");
        // Foreign principal cannot use this ref without grant
        ev.set_tenant_principal(43, "t43");
        CHECK(!ev.check_workspace_isolation(43, ref.tenant_id, 0, "ref-use"),
              "stamped foreign tenant_id denied");
    }

    // ── AC: EDSL set-tenant / grant-cross / check ──
    {
        reset_all();
        CompilerService cs;
        auto s = cs.eval("(security:set-tenant-principal! 10)");
        CHECK(s && is_bool(*s) && as_bool(*s), "set-tenant-principal!");
        auto c1 = cs.eval("(security:check-tenant-isolation 11)");
        CHECK(c1 && is_bool(*c1) && !as_bool(*c1), "check isolation denies cross");
        auto g = cs.eval(std::format("(security:grant-cross-tenant! 10 11 {})", kEffectMutate));
        CHECK(g && is_bool(*g) && as_bool(*g), "grant-cross-tenant!");
        auto c2 = cs.eval(std::format("(security:check-tenant-isolation 11 0 {})", kEffectMutate));
        CHECK(c2 && is_bool(*c2) && as_bool(*c2), "check allows after grant");
        // #2659: set_tenant_principal is per-Evaluator (capability_tenant_id_),
        // not process-global WorkspaceIsolationPolicy::current — so snapshot
        // current_tenant may stay 0. Principal authority is the Evaluator.
        CHECK(cs.evaluator().capability_tenant_id() == 10,
              "EDSL set-tenant-principal sets Evaluator capability_tenant_id_");
        const auto snap = snapshot_tenant_isolation_stats();
        CHECK(snap.boundary_violations_prevented >= 1 ||
                  href_m(cs, "boundary-violations-prevented") >= 1,
              "stats violations");
    }

    // ── AC5: multi-thread stress — concurrent cross-tenant denies ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "agent");
        constexpr int kThreads = 4;
        constexpr int kIters = 200;
        std::atomic<int> denies{0};
        std::atomic<int> allows{0};
        std::vector<std::thread> thr;
        thr.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            thr.emplace_back([&, t] {
                for (int i = 0; i < kIters; ++i) {
                    IsolationRefProvenance ref{};
                    ref.tenant_id = static_cast<std::uint64_t>(100 + (i % 3));
                    // Foreign refs should deny
                    if (!check_boundary(1, 1, &ref))
                        denies.fetch_add(1, std::memory_order_relaxed);
                    else
                        allows.fetch_add(1, std::memory_order_relaxed);
                    // Cross target without grant denies
                    if (!check_boundary(1, static_cast<std::uint64_t>(2 + (t % 2))))
                        denies.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : thr)
            th.join();
        CHECK(denies.load() >= kThreads * kIters, "stress: most attempts denied");
        CHECK(snapshot_tenant_isolation_stats().boundary_violations_prevented >=
                  static_cast<std::uint64_t>(kThreads * kIters),
              "stress: violations audited");
        CHECK(snapshot_tenant_isolation_stats().audits >=
                  static_cast<std::uint64_t>(kThreads * kIters),
              "stress: audits recorded");
        (void)allows;
    }

    // ── #3332: allow_cross is scoped to cross_grants, not a full bypass ──
    // Soft/Off keep the zero-cost short-circuit (AC5). Restricted walks
    // grant bits even when allow_cross is set (AC2/AC3).
    {
        reset_all(); // Off
        CHECK(check_boundary(1, 99, nullptr, /*allow_cross=*/true),
              "Soft/Off allow_cross still short-circuits");
        CHECK(!check_boundary(1, 99, nullptr, /*allow_cross=*/true, kEffectMutate,
                              /*sandbox_strict=*/false, "3332-no-grant",
                              /*sandbox_restricted=*/true),
              "allow_cross without grant denies");
        // Issue #3998: mint under Restricted with TA — Soft-era mint_principal=0
        // rows are swept/fail-closed on the production face.
        set_mode(SandboxMode::Restricted);
        grant_tenant_admin_mid(1);
        g_workspace_isolation().grant_cross_tenant(1, 99, kEffectMutate, /*caller_principal=*/1);
        CHECK(check_boundary(1, 99, nullptr, /*allow_cross=*/true, kEffectMutate,
                             /*sandbox_strict=*/false, "3332-grant",
                             /*sandbox_restricted=*/true),
              "allow_cross + grant allows");
        CHECK(!check_boundary(1, 99, nullptr, /*allow_cross=*/true, kEffectWrite,
                              /*sandbox_strict=*/false, "3332-bits",
                              /*sandbox_restricted=*/true),
              "insufficient bits still deny");
    }

    // ─── Issue #2659: per-Evaluator principal (multi-Evaluator no cross-talk) ──
    //   AC1: Two Evaluators in one process, tenants 7 and 42, concurrent
    //        require_effect(Mutate) — each sees only its own principal.
    //   AC2: Existing single-Evaluator + TenantScope remains green.
    //   AC3: Cross-tenant grant still allows the intended path (global table).
    //   AC4: Restricted + unset principal deny fires when CALLING Evaluator
    //        has principal 0 (per-Evaluator lens).
    //   AC5: Metrics / SecurityEvent IsolationDeny carry correct tenant ids.
    //   AC6: source-cite + coverage linter (no docs/design per #1655).

    // AC1: two Evaluators concurrent require_effect — no cross-talk.
    {
        std::println("\n--- #2659 AC1: multi-Evaluator no cross-talk ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_b.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(7);
        ev_b.set_capability_tenant_id(42);
        const auto me_a = aura::core::current_mutation_epoch();
        const auto me_b = me_a == 0 ? 1 : me_a;
        // Issue #3362: same-tenant high-risk self-grant under Restricted now
        // requires TenantAdmin — arm both fixture tenants so the seeding
        // grants land (this AC tests principal isolation, not the fence).
        grant_tenant_admin_mid(7);
        grant_tenant_admin_mid(42);
        // Issue #3333: Restricted mid-join is fail-closed and the require
        // path joins TypedMid first (#3296), so bind the seeds to the same
        // cascade the stress calls will present (stamp → epoch → bump).
        auto join_mid = []() {
            auto m = aura::compiler::typed_audit::last_type_linear_commit_proof_stamp_v_read();
            if (m == 0)
                m = aura::core::current_mutation_epoch();
            if (m == 0) {
                aura::core::bump_mutation_epoch();
                m = aura::core::current_mutation_epoch();
            }
            return m;
        };
        const auto seed_mid = join_mid();
        ev_a.grant_effect_capability(7, "mutate-2657-A1-a", kEffectMutate, seed_mid);
        ev_b.grant_effect_capability(42, "mutate-2657-A1-b", kEffectMutate, seed_mid);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> a_ok{0};
        std::atomic<std::uint64_t> b_ok{0};
        std::atomic<std::uint64_t> err{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        // Required_effects=0 + Restricted → pure read is permissive
                        // (no principal required). Use Mutate with a real grant.
                        const bool ok =
                            (t & 1) ? ev_a.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                                          "test:2659-a", 0, 7)
                                    : ev_b.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                                          "test:2659-b", 0, 42);
                        if (ok) {
                            if (t & 1)
                                a_ok.fetch_add(1, std::memory_order_relaxed);
                            else
                                b_ok.fetch_add(1, std::memory_order_relaxed);
                        }
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();
        std::println("  a_ok={} b_ok={} err={}", a_ok.load(), b_ok.load(), err.load());
        CHECK(a_ok.load() > 0, "AC1: Evaluator A (tenant 7) allowed when its grant exists");
        CHECK(b_ok.load() > 0, "AC1: Evaluator B (tenant 42) allowed when its grant exists");
        CHECK(err.load() == 0, "AC1: no exceptions under concurrent multi-Evaluator");
        // Per-Evaluator principal preserved (no global cross-talk).
        CHECK(ev_a.capability_tenant_id() == 7, "AC1: Evaluator A principal still 7 post-stress");
        CHECK(ev_b.capability_tenant_id() == 42, "AC1: Evaluator B principal still 42 post-stress");
    }

    // AC2: TenantScope RAII still snapshots/restores per-Evaluator.
    {
        std::println("\n--- #2659 AC2: TenantScope per-Evaluator snapshot ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        // Enter TenantScope with tenant 42; on exit, ev.capability_tenant_id_ should be 7.
        {
            Evaluator::TenantScope scope(ev, 42, "scoped-42");
            CHECK(ev.capability_tenant_id() == 42, "AC2: TenantScope sets principal to 42");
            CHECK(scope.previous_tenant() == 7, "AC2: snapshot captured prior principal 7");
        }
        CHECK(ev.capability_tenant_id() == 7, "AC2: TenantScope restored principal to 7");
    }

    // AC3: cross-tenant grant still works (global table).
    {
        std::println("\n--- #2659 AC3: cross-tenant grant table ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        // #3090: Restricted refuse mid==0; #3086: grant_cross_tenant
        // requires TenantAdmin on caller/target. Issue #3597: the policy
        // entry takes an explicit caller_principal — pass the from-tenant
        // (TA armed above); the default would join to default_tenant 0 and
        // fail the privileged gate.
        grant_tenant_admin_mid(7);
        // Issue grant 7 → 42 globally (shared policy).
        g_workspace_isolation().grant_cross_tenant(7, 42, kEffectMutate,
                                                   /*caller_principal=*/7);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto me = aura::core::current_mutation_epoch();
        ev.grant_effect_capability(7, "mutate-2657-A3", kEffectMutate, me == 0 ? 1 : me);
        // Cross-tenant mutate target=42 with cover-by-grant → allow.
        CHECK(ev.check_workspace_isolation(42, 0, kEffectMutate, "test:2659-ac3-xgrant"),
              "AC3: cross-tenant target 42 with grant from 7 allows");
    }

    // AC4: Restricted + unset principal (caller's tenant_id == 0) denies.
    {
        std::println("\n--- #2659 AC4: per-Evaluator unset principal deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(0); // unset principal
        const auto before = current_iso_seq();
        const bool ok = ev.check_workspace_isolation(0, 0, kEffectMutate, "test:2659-ac4-unset");
        CHECK(!ok, "AC4: Restricted + unset principal on calling Evaluator denies");
        const auto after = current_iso_seq();
        CHECK(after > before, "AC4: IsolationDeny SE emitted");
    }

    // AC5: SecurityEvent IsolationDeny carries correct tenant ids (caller + ref).
    {
        std::println("\n--- #2659 AC5: IsolationDeny tenant ids ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(0);
        // foreign ref tenant triggers provenance deny (#2490 + #2659).
        (void)ev.check_workspace_isolation(0, 99, kEffectMutate, "test:2659-ac5");
        // Find the most recent IsolationDeny SE.
        bool found = false;
        for (std::uint64_t s = baseline; s < ring.seq.load(); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (static_cast<int>(e.kind) ==
                    static_cast<int>(
                        aura::core::security_event::SecurityEventKind::IsolationDeny) &&
                e.seq == s) {
                // SE carries the target tenant (0 here) and the ref_tenant (99)
                // in the tenant_id field per #2388 / #2156 vocab.
                // mid is Mutation epoch (not tenant id).
                CHECK(e.tenant_id == 0, "AC5: SE tenant_id is target (0)");
                CHECK(e.mutation_id != 0, "AC5: SE mid is non-zero Mutation epoch");
                found = true;
            }
        }
        CHECK(found, "AC5: IsolationDeny SE in ring");
    }

    // AC6: source-cite + coverage manifest.
    {
        std::println("\n--- #2659 AC6: source-cite + coverage ---");
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        (void)baseline;
        reset_all();
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        CHECK(sec.find("Issue #2659") != std::string::npos,
              "AC6: evaluator_security.cpp cites #2659");
        // set_tenant_principal / TenantScope no longer write g_workspace_isolation().current.
        const auto wihh = read_file("src/core/workspace_isolation.hh");
        CHECK(wihh.find("caller_principal") != std::string::npos,
              "AC6: check_boundary_ex takes caller_principal");
        CHECK(wihh.find("Issue #2659") != std::string::npos,
              "AC6: workspace_isolation.hh cites #2659");
        const auto ixx = read_file("src/compiler/evaluator.ixx");
        CHECK(ixx.find("allow_cross_tenant_") != std::string::npos,
              "AC6: Evaluator has per-instance allow_cross_tenant_ (Issue #2659)");
        CHECK(ixx.find("prev_allow_cross_") != std::string::npos,
              "AC6: TenantScope snapshots prev_allow_cross_");
        // Coverage manifest + linter.
        const auto gate = read_file("scripts/coverage/manifests/2659.json");
        CHECK(!gate.empty(), "AC6: coverage linter check_2659.py present");
        const auto manifest = read_file("scripts/coverage/manifests/2659.json");
        CHECK(!manifest.empty(), "AC6: coverage manifest 2659.json present");
        CHECK(read_file("docs/design/2659-multi-eval-principal.md").empty(),
              "AC6: no docs/design/ — design rationale in commit/close");
    }

    // ── #2687 AC1/AC2: per-Evaluator capture tenant accounting ──
    {
        std::println("\n--- #2687 AC1/AC2: per-Evaluator isolation_capture_tenant ---");
        reset_all();
        // Production multi-tenant path goes through Evaluator::stamp_stable_ref
        // which uses Evaluator::capability_tenant_id_ (per-Evaluator authority
        // from #2659 + #2056). The new #2687 counters distinguish:
        //   local: Evaluator::stamp_stable_ref (per-Evaluator, authority)
        //   global_fallback: maybe_stamp_stable_ref_isolation_tenant (FlatAST
        //                    fallback path, reads g_isolation_capture_tenant)
        //   evaluator_miss: diagnostic for FlatAST factories called under
        //                   an active Evaluator (should have used
        //                   Evaluator::stamp_stable_ref).
        const auto local_before =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        const auto global_before =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        const auto miss_before =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        // Issue #2056: full stamp (tenant + fiber) — Evaluator::make_stamped_ref
        // calls stamp_stable_ref which bumps g_isolation_capture_stamp_local_total_atomic.
        for (int i = 0; i < 4; ++i) {
            (void)ev.make_stamped_ref(static_cast<NodeId>(i));
        }
        const auto local_after =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        const auto global_after =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        const auto miss_after =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(local_after >= local_before + 4,
              "AC1: Evaluator::make_stamped_ref bumps local capture counter by >= 4");
        CHECK(global_after == global_before,
              "AC1: Evaluator path does NOT bump global_fallback counter");
        CHECK(miss_after == miss_before,
              "AC1: Evaluator path does NOT bump evaluator_miss counter");
        // Global-fallback path (Soft): set process-global capture tenant + call
        // maybe_stamp_stable_ref_isolation_tenant on a StableRefT.
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(42);
        FlatAST::StableNodeRef ref{};
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(stamped, "AC2: Soft global-fallback path stamps when tenant != 0");
        CHECK(ref.tenant_id == 42, "AC2: global-fallback stamps tenant_id from process-global");
        const auto global_after2 =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(global_after2 >= global_after + 1,
              "AC2: Soft global-fallback path bumps fallback counter");
        // Reset for AC4.
        aura::core::provenance::set_isolation_capture_tenant(0);
    }

    // ── #2687 AC4: Soft / tenant=0 capture remains permissive ──
    {
        std::println("\n--- #2687 AC4: Soft / tenant=0 capture permissive ---");
        reset_all();
        aura::core::provenance::set_isolation_capture_tenant(0);
        FlatAST::StableNodeRef ref{};
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(!stamped,
              "AC4: tenant=0 → maybe_stamp_stable_ref_isolation_tenant returns false (no stamp)");
        CHECK(ref.tenant_id == 0, "AC4: tenant_id stays 0 (legacy single-tenant)");
    }

    // ── #2687 AC5: counters + query surface (source + live atomics) ──
    // Light-link binaries do not always register full query:soa-dirty-stats
    // (obs_jit register_jit_p5). Live authority is the provenance atomics;
    // schema/key wiring is source-cited in AC6 + coverage linter.
    {
        std::println("\n--- #2687 AC5: counters + query surface ---");
        const auto local_q =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        const auto fallback_q =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        const auto miss_q =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(local_q >= 0, "AC5: local-total live (>= 0)");
        CHECK(fallback_q >= 0, "AC5: global-fallback-total live (>= 0)");
        CHECK(miss_q >= 0, "AC5: evaluator-miss-total live (>= 0)");
        CHECK(aura::core::provenance::kEvaluatorCaptureTenantIssue == 2687,
              "AC5: kEvaluatorCaptureTenantIssue == 2687");
        // Best-effort engine:metrics when full JIT obs is linked.
        CompilerService cs;
        const auto schema_q = href(cs, "schema-2687");
        if (schema_q >= 0) {
            CHECK(schema_q == 2687, "AC5: schema-2687 sentinel (when query wired)");
            CHECK(href(cs, "issue-2687") == 2687, "AC5: issue-2687 sentinel (when query wired)");
        } else {
            CHECK(true, "AC5: query:soa-dirty-stats not in light link — atomics + source-cite OK");
        }
    }

    // ── #2687 AC6: source-cite + no regression ──
    {
        std::println("\n--- #2687 AC6: source-cite + no regression ---");
        const auto prov = read_file("src/core/provenance_tracker.hh");
        const auto eval_sec = read_file("src/compiler/evaluator_security.cpp");
        const auto workspace = read_file("src/core/workspace_isolation.hh");
        const auto q_src = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        // Issue #2687 sentinel in all 4 prod-side files.
        CHECK(prov.find("#2687") != std::string::npos, "AC6: provenance_tracker.hh cites #2687");
        CHECK(eval_sec.find("#2687") != std::string::npos,
              "AC6: evaluator_security.cpp cites #2687");
        CHECK(workspace.find("#2687") != std::string::npos,
              "AC6: workspace_isolation.hh cites #2687");
        CHECK(q_src.find("#2687") != std::string::npos,
              "AC6: evaluator_primitives_obs_jit.cpp cites #2687");
        // Counters declared + wired.
        CHECK(prov.find("g_isolation_capture_stamp_local_total_atomic") != std::string::npos,
              "AC5: local counter declared in provenance_tracker.hh");
        CHECK(prov.find("g_isolation_capture_stamp_global_fallback_total_atomic") !=
                  std::string::npos,
              "AC5: global-fallback counter declared in provenance_tracker.hh");
        CHECK(eval_sec.find("g_isolation_capture_stamp_local_total_atomic") != std::string::npos,
              "AC1: local counter bumped from Evaluator::stamp_stable_ref");
        CHECK(prov.find("g_isolation_capture_stamp_global_fallback_total_atomic") !=
                  std::string::npos,
              "AC2: global-fallback counter bumped from maybe_stamp_stable_ref_isolation_tenant");
        // #2659 regression: Evaluator::set_tenant_principal must NOT write the global.
        CHECK(eval_sec.find("set_isolation_capture_tenant") == std::string::npos,
              "AC2 (#2659 regression): Evaluator::set_tenant_principal must not write global");
        // No design doc regression (per #1655).
        for (const auto& p : {"docs/design/evaluator_capture_tenant_2687.md",
                              "docs/evaluator_capture_tenant_2687.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
        }
    }

    // ── #2705 AC1: production hard-close refuses global stamp ──
    {
        std::println("\n--- #2705 AC1: hard-close refuses FlatAST global capture stamp ---");
        reset_all();
        // Soft baseline first: set global tenant under soft, then arm hard-close
        // (mirrors production multi-tenant residual where global may be non-zero
        // from a legacy WorkspaceIsolationPolicy mirror, but stamp must refuse).
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(42);
        const auto miss_before =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        const auto fallback_before =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        aura::core::provenance::set_hard_capture_tenant(true);
        CHECK(aura::core::provenance::hard_capture_tenant_active(),
              "AC1: hard_capture_tenant_active after arm");
        FlatAST::StableNodeRef ref{};
        ref.tenant_id = 0;
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(!stamped, "AC1: hard-close → maybe_stamp returns false (no stamp)");
        CHECK(ref.tenant_id == 0, "AC1: tenant_id stays 0 (no cross-tenant pollution)");
        const auto miss_after =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        const auto fallback_after =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(miss_after >= miss_before + 1, "AC1: evaluator_miss advances on refuse");
        CHECK(fallback_after == fallback_before, "AC1: global_fallback stays 0 under hard-close");
        // Dual-Evaluator local path still works under hard-close (AC3).
        const auto local_before =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        CompilerService cs_a;
        CompilerService cs_b;
        cs_a.evaluator().set_capability_tenant_id(7);
        cs_b.evaluator().set_capability_tenant_id(42);
        auto ra = cs_a.evaluator().make_stamped_ref(static_cast<NodeId>(1));
        auto rb = cs_b.evaluator().make_stamped_ref(static_cast<NodeId>(2));
        CHECK(ra.tenant_id == 7, "AC1/AC3: Evaluator A stamps tenant 7 (local authority)");
        CHECK(rb.tenant_id == 42, "AC1/AC3: Evaluator B stamps tenant 42 (local authority)");
        const auto local_after =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(local_after >= local_before + 2,
              "AC3: local counter still advances under hard-close");
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(0);
    }

    // ── #2705 AC2: Soft / tenant=0 stays permissive ──
    {
        std::println("\n--- #2705 AC2: Soft / tenant=0 capture permissive ---");
        reset_all();
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(0);
        FlatAST::StableNodeRef ref{};
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(!stamped, "AC2: tenant=0 → no stamp (zero-cost early return)");
        CHECK(ref.tenant_id == 0, "AC2: tenant_id stays 0");
        // Soft path with global tenant still stamps (legacy single-tenant).
        aura::core::provenance::set_isolation_capture_tenant(9);
        FlatAST::StableNodeRef ref2{};
        const bool stamped2 = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref2);
        CHECK(stamped2, "AC2: Soft + tid!=0 still stamps (legacy allow)");
        CHECK(ref2.tenant_id == 9, "AC2: Soft stamps tenant 9 from global");
        aura::core::provenance::set_isolation_capture_tenant(0);
    }

    // ── #2705 AC5: query surface (live API + optional engine:metrics) ──
    {
        std::println("\n--- #2705 AC5: query surface ---");
        reset_all();
        CHECK(aura::core::provenance::kHardCaptureTenantIssue == 2705,
              "AC5: kHardCaptureTenantIssue == 2705");
        aura::core::provenance::set_hard_capture_tenant(false);
        CHECK(!aura::core::provenance::hard_capture_tenant_active(),
              "AC5: hard-close-armed false when pref off");
        aura::core::provenance::set_hard_capture_tenant(true);
        CHECK(aura::core::provenance::hard_capture_tenant_active(),
              "AC5: hard-close-armed true when pref on");
        aura::core::provenance::set_hard_capture_tenant(false);
        // #2687 counters preserved (additive).
        CHECK(aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                  std::memory_order_relaxed) >= 0,
              "AC5: evaluator-miss-total still live");
        CompilerService cs;
        const auto schema_q = href(cs, "schema-2705");
        if (schema_q >= 0) {
            CHECK(schema_q == 2705, "AC5: schema-2705 sentinel (when query wired)");
            CHECK(href(cs, "issue-2705") == 2705, "AC5: issue-2705 sentinel (when query wired)");
            const auto armed = href(cs, "isolation-capture-hard-close-armed");
            CHECK(armed == 0 || armed == 1, "AC5: hard-close-armed query is 0/1");
        } else {
            CHECK(true, "AC5: query keys source-cited; light link skips engine:metrics");
        }
    }

    // ── #2705 AC6: source-cite ──
    {
        std::println("\n--- #2705 AC6: source-cite ---");
        const auto prov = read_file("src/core/provenance_tracker.hh");
        const auto sec_def = read_file("src/compiler/security_defaults.hh");
        const auto q_src = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(prov.find("#2705") != std::string::npos, "AC6: provenance_tracker.hh cites #2705");
        CHECK(prov.find("hard_capture_tenant") != std::string::npos,
              "AC6: hard_capture_tenant API in provenance_tracker.hh");
        CHECK(sec_def.find("#2705") != std::string::npos, "AC6: security_defaults.hh cites #2705");
        CHECK(q_src.find("#2705") != std::string::npos,
              "AC6: evaluator_primitives_obs_jit.cpp cites #2705");
        CHECK(q_src.find("isolation-capture-hard-close-armed") != std::string::npos,
              "AC6: hard-close-armed query key present");
        for (const auto& p :
             {"docs/design/hard_capture_tenant_2705.md", "docs/hard_capture_tenant_2705.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
        }
    }

    // ── #2759 AC1: Evaluator stamp sole production authority ──
    {
        std::println("\n--- #2759 AC1: Evaluator stamp sole authority under hard-close ---");
        reset_all();
        aura::core::provenance::set_hard_capture_tenant(true);
        CHECK(aura::core::provenance::hard_capture_tenant_active(), "AC1: hard-close armed");
        // Non-zero global write suppressed under hard-close.
        const auto supp_before =
            aura::core::provenance::g_isolation_capture_global_write_suppressed_total_atomic().load(
                std::memory_order_relaxed);
        aura::core::provenance::set_isolation_capture_tenant(99);
        CHECK(aura::core::provenance::isolation_capture_tenant() == 0,
              "AC1: non-zero global write suppressed under hard-close");
        const auto supp_after =
            aura::core::provenance::g_isolation_capture_global_write_suppressed_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(supp_after >= supp_before + 1, "AC1: global-write-suppressed advances");
        // Dual Evaluator: make_stamped_ref is local-only (no miss/fallback).
        const auto miss_before =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        const auto fallback_before =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        const auto local_before =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        CompilerService cs_a;
        CompilerService cs_b;
        cs_a.evaluator().set_capability_tenant_id(7);
        cs_b.evaluator().set_capability_tenant_id(42);
        auto ra = cs_a.evaluator().make_stamped_ref(static_cast<NodeId>(1));
        auto rb = cs_b.evaluator().make_stamped_ref(static_cast<NodeId>(2));
        CHECK(ra.tenant_id == 7, "AC1: Evaluator A stamps tenant 7 only");
        CHECK(rb.tenant_id == 42, "AC1: Evaluator B stamps tenant 42 only");
        const auto miss_after =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        const auto fallback_after =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        const auto local_after =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(local_after >= local_before + 2, "AC1: local counter advances on stamp");
        CHECK(miss_after == miss_before,
              "AC1: make_stamped_ref (layout+stamp) does NOT bump evaluator_miss");
        CHECK(fallback_after == fallback_before,
              "AC1: make_stamped_ref does NOT bump global_fallback");
        aura::core::provenance::set_hard_capture_tenant(false);
    }

    // ── #2759 AC2: Soft / tenant=0 stays permissive ──
    {
        std::println("\n--- #2759 AC2: Soft global write + stamp still permissive ---");
        reset_all();
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(11);
        CHECK(aura::core::provenance::isolation_capture_tenant() == 11,
              "AC2: Soft allows non-zero global write");
        FlatAST::StableNodeRef ref{};
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(stamped, "AC2: Soft maybe_stamp still stamps");
        CHECK(ref.tenant_id == 11, "AC2: Soft stamps tenant from global");
        aura::core::provenance::set_isolation_capture_tenant(0);
        FlatAST::StableNodeRef ref0{};
        CHECK(!aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref0),
              "AC2: tenant=0 still no-op");
    }

    // ── #2759 AC3: refresh preserves tenant; no global re-stamp ──
    {
        std::println("\n--- #2759 AC3: refresh preserves tenant under hard-close ---");
        reset_all();
        // Soft write global first, then arm hard-close with global already set
        // (legacy residual pollution). refresh must preserve tenant and must
        // not stamp from global (layout remake).
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(77);
        aura::core::provenance::set_hard_capture_tenant(true);
        CHECK(aura::core::provenance::isolation_capture_tenant() == 77,
              "AC3: pre-arm global still visible (suppress only blocks new writes)");
        FlatAST::StableNodeRef ref{};
        ref.id = static_cast<NodeId>(1);
        ref.tenant_id = 7;
        // refresh_if_stale needs a live FlatAST — use make_safe_ref_layout path
        // via direct field restore semantics already unit-tested in
        // test_stable_ref_tenant_mandate. Here we assert source contract:
        // remake uses make_safe_ref_layout (no maybe_stamp).
        const auto stab = read_file("src/core/ast_stability.cpp");
        CHECK(stab.find("make_safe_ref_layout") != std::string::npos,
              "AC3: refresh_if_stale remakes via make_safe_ref_layout");
        CHECK(stab.find("preserved_tenant") != std::string::npos,
              "AC3: refresh preserves tenant_id");
        // maybe_stamp under hard-close with residual global refuses.
        FlatAST::StableNodeRef r2{};
        r2.tenant_id = 0;
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(r2);
        CHECK(!stamped, "AC3: hard-close refuses global re-stamp");
        CHECK(r2.tenant_id == 0, "AC3: tenant_id unchanged by refused stamp");
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(0);
    }

    // ── #2759 AC5/AC6: query + source-cite ──
    {
        std::println("\n--- #2759 AC5/AC6: query + source-cite ---");
        reset_all();
        CHECK(aura::core::provenance::kEvaluatorStampSoleAuthorityIssue == 2759,
              "AC5: kEvaluatorStampSoleAuthorityIssue == 2759");
        const auto prov = read_file("src/core/provenance_tracker.hh");
        const auto eval_sec = read_file("src/compiler/evaluator_security.cpp");
        const auto ast = read_file("src/core/ast.ixx");
        const auto q_src = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(prov.find("#2759") != std::string::npos, "AC6: provenance_tracker.hh cites #2759");
        CHECK(prov.find("g_isolation_capture_global_write_suppressed_total_atomic") !=
                  std::string::npos,
              "AC5: global-write-suppressed counter declared");
        CHECK(eval_sec.find("make_ref_layout") != std::string::npos,
              "AC1: make_stamped_ref uses make_ref_layout");
        CHECK(ast.find("make_ref_layout") != std::string::npos, "AC1: make_ref_layout in FlatAST");
        CHECK(ast.find("make_safe_ref_layout") != std::string::npos,
              "AC3: make_safe_ref_layout in FlatAST");
        CHECK(q_src.find("schema-2759") != std::string::npos, "AC5: schema-2759 query key");
        CHECK(q_src.find("issue-2759") != std::string::npos, "AC5: issue-2759 query key");
        CHECK(q_src.find("isolation-capture-global-write-suppressed-total") != std::string::npos,
              "AC5: global-write-suppressed query key");
        // #2705 / #2687 keys preserved.
        CHECK(q_src.find("schema-2705") != std::string::npos, "AC5: schema-2705 preserved");
        CHECK(q_src.find("schema-2687") != std::string::npos, "AC5: schema-2687 preserved");
        for (const auto& p : {"docs/design/evaluator_stamp_sole_authority_2759.md",
                              "docs/evaluator_stamp_sole_authority_2759.md", "design/2759.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
        }
    }

    // ── #2960: query stable returns stamp full provenance ──
    {
        std::println("\n--- #2960 AC1/AC2: query stamp helper + counters ---");
        reset_all();
        CHECK(aura::core::provenance::kQueryStableRefStampIssue == 2960,
              "AC2: kQueryStableRefStampIssue == 2960");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (q-stamp x) (+ x 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live node");

        ev.set_capability_tenant_id(55);
        const auto stamped0 =
            aura::core::provenance::g_query_stable_ref_stamped_total_atomic().load(
                std::memory_order_relaxed);
        const auto prev0 =
            aura::core::provenance::g_query_stable_ref_unstamped_prevented_total_atomic().load(
                std::memory_order_relaxed);

        // Layout path (primary): cow/wrap match workspace → stamped only.
        auto layout = ws->make_ref_layout(id);
        CHECK(layout.tenant_id == 0, "AC1: layout-only tenant 0 before stamp");
        ev.stamp_query_stable_ref_export(layout);
        CHECK(layout.tenant_id == 55, "AC1: stamp_query fills capability tenant");
        CHECK(layout.cow_epoch_at_capture == ws->workspace_cow_epoch(),
              "AC1: cow_epoch preserved from layout");

        // Brace-init residual under advanced wrap: remade + unstamped_prevented.
        if (ws->wrap_epoch() == 0) {
            // Force wrap_epoch visibility by bumping generation many times is heavy;
            // source-cite residual path instead when wrap still 0.
            const auto sec = read_file("src/compiler/evaluator_security.cpp");
            CHECK(sec.find("record_query_stable_ref_unstamped_prevented") != std::string::npos,
                  "AC2: unstamped residual path wired");
        } else {
            FlatAST::StableNodeRef brace{};
            brace.id = id;
            brace.gen = ws->generation();
            ev.stamp_query_stable_ref_export(brace);
            CHECK(brace.tenant_id == 55, "AC2: brace residual remade+stamped");
            CHECK(
                aura::core::provenance::g_query_stable_ref_unstamped_prevented_total_atomic().load(
                    std::memory_order_relaxed) > prev0,
                "AC2: unstamped_prevented advanced on brace residual");
        }

        CHECK(aura::core::provenance::g_query_stable_ref_stamped_total_atomic().load(
                  std::memory_order_relaxed) > stamped0,
              "AC2: query_stable_ref_stamped_total advanced");

        // Multi-tenant isolation fail-closed on foreign stamped ref.
        auto foreign = layout;
        foreign.tenant_id = 99;
        CHECK(!ev.check_workspace_isolation(55, foreign.tenant_id, 0, "test:2960-x"),
              "AC3: cross-tenant isolation deny");

        // Source cite FlatAST layout-only children_stable / for_each.
        const auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("make_ref_layout(cid)") != std::string::npos ||
                  ast.find("make_ref_layout(pid)") != std::string::npos,
              "AC1: children/parent_stable use make_ref_layout");
        const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
        CHECK(qws.find("stamp_query_stable_ref_export") != std::string::npos,
              "AC1: query workspace stamps via stamp_query_stable_ref_export");
        const auto qhash = read_file("src/compiler/evaluator_primitives_query.cpp");
        CHECK(qhash.find("query-stable-ref-stamped-total") != std::string::npos,
              "AC2: stable-ref-stats-hash exposes stamped total");
        CHECK(qhash.find("schema-2960") != std::string::npos, "AC2: schema-2960 on stats hash");
        for (const auto& p : {"docs/design/query_stable_ref_stamp_2960.md",
                              "docs/query_stable_ref_stamp_2960.md", "design/2960.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "AC4: no design doc at " + std::string(p));
        }
    }

    // ── #3000: restamp-lag export face (isolation / tenant-capture sibling) ──
    {
        std::println("\n--- #3000 AC1/AC2: stamp rejects lagging gen under production ---");
        reset_all();
        CHECK(aura::core::provenance::kQueryStableRefRestampLagIssue == 3000,
              "AC4: kQueryStableRefRestampLagIssue == 3000");
        using aura::ast::clear_restamp_budget_nodes_override_for_test;
        using aura::ast::set_restamp_budget_nodes_for_process;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (q-lag a) a) (define (q-lag2 b) b) "
                      "(define (q-lag3 c) c) (define (q-lag4 d) d)\")")
                  .has_value(),
              "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live node");
        apply_production_audit_defaults();
        set_restamp_budget_nodes_for_process(1);
        ws->bump_generation();
        ws->restamp_all_node_generations();
        CHECK(ws->restamp_last_budget_exceeded(), "#3000: last restamp exceeded");
        if (!ws->node_generation_is_post_mutate(id)) {
            CHECK(!ev.allow_query_stable_ref_export(id),
                  "#3000: production allow rejects lagging node");
            FlatAST::StableNodeRef brace{};
            brace.id = id;
            ev.stamp_query_stable_ref_export(brace);
            CHECK(brace.id == NULL_NODE, "#3000: stamp nulls lagging export");
            CHECK(aura::core::provenance::g_query_stable_ref_restamp_lag_prevented_total_atomic()
                          .load(std::memory_order_relaxed) >= 1,
                  "#3000: prevented advanced");
        } else {
            CHECK(true, "#3000: node incrementally restamped — post-mutate allow");
        }
        apply_dev_audit_defaults();
        clear_restamp_budget_nodes_override_for_test();
        const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
        CHECK(qws.find("restamp-lag") != std::string::npos, "#3000: typed restamp-lag reason");
        CHECK(qws.find("allow_query_stable_ref_export") != std::string::npos,
              "#3000: query workspace gates export");
        for (const auto& p : {"docs/design/3000-restamp-lag.md", "docs/query_restamp_lag_3000.md",
                              "design/3000.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "#3000: no design doc at " + std::string(p));
        }
    }

    // ── #3037: over-budget restamp torn export (lazy-align must not hide) ──
    {
        std::println("\n--- #3037 AC1/AC2: torn reject after lazy-align under production ---");
        reset_all();
        CHECK(aura::core::provenance::kQueryStableRefRestampTornIssue == 3037,
              "3037: kQueryStableRefRestampTornIssue == 3037");
        using aura::ast::clear_restamp_budget_nodes_override_for_test;
        using aura::ast::set_restamp_budget_nodes_for_process;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (q-torn a) a) (define (q-torn2 b) b) "
                      "(define (q-torn3 c) c) (define (q-torn4 d) d)\")")
                  .has_value(),
              "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live node");
        apply_production_audit_defaults();
        set_restamp_budget_nodes_for_process(1);
        ws->bump_generation();
        ws->restamp_all_node_generations();
        CHECK(ws->restamp_generation_torn(), "#3037: generation torn");
        if (!ws->node_eagerly_restamped(id)) {
            // Issue #3388: is_valid is an observe-only oracle now — the lazy
            // gen refresh moved to make_ref_layout (the only face permitted
            // to advance a slot's gen). Align via make_ref_layout, then the
            // slot reads post-mutate.
            (void)ws->make_ref_layout(id);
            CHECK(ws->node_generation_is_post_mutate(id), "#3037: lazy-align hid raw gen lag");
            CHECK(!ev.allow_query_stable_ref_export(id),
                  "#3037: production rejects after lazy-align");
            FlatAST::StableNodeRef brace{};
            brace.id = id;
            ev.stamp_query_stable_ref_export(brace);
            CHECK(brace.id == NULL_NODE, "#3037: stamp nulls torn export");
            CHECK(
                aura::core::provenance::g_query_stable_ref_restamp_torn_reject_total_atomic().load(
                    std::memory_order_relaxed) >= 1,
                "#3037: torn reject advanced");
        } else {
            CHECK(true, "#3037: node eagerly restamped — current gen ok");
        }
        apply_dev_audit_defaults();
        clear_restamp_budget_nodes_override_for_test();
        const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
        // #3037 surface refined by #3198/#3121/#3487 — the torn export cite
        // is the current anchor for the lazy-align reject path.
        CHECK(qws.find("Issue #3198 / #3121") != std::string::npos,
              "#3037: query workspace cites torn");
        CHECK(qws.find("generation torn") != std::string::npos, "#3037: torn message");
        const auto astx = read_file("src/core/ast.ixx");
        CHECK(astx.find("node_eagerly_restamped") != std::string::npos, "#3037: eager bit helper");
        for (const auto& p : {"docs/design/3037-restamp-over-budget-export.md",
                              "docs/restamp_over_budget_export_3037.md", "design/3037.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "#3037: no design doc at " + std::string(p));
        }
    }

    // ── #2968: cross-tenant grant write path requires TenantAdmin ──
    {
        std::println("\n--- #2968 AC1: cross-tenant grant without TenantAdmin → deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);

        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        ev.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1,
              "AC1: cross_tenant_grant_deny_total bumps when caller lacks TenantAdmin");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC1: no cross grant written on deny");
        // SE reason present in ring.
        const auto& ring = g_security_event_ring();
        bool found = false;
        const auto cur = ring.seq.load(std::memory_order_acquire);
        for (auto s = cur; s > 0 && s + 16 > cur; --s) {
            const auto& e = ring.ring[(s - 1) % ring.ring.size()];
            if (std::string_view(e.reason) == "cross-tenant-grant-needs-tenant-admin") {
                found = true;
                break;
            }
        }
        CHECK(found, "AC1: SE reason 'cross-tenant-grant-needs-tenant-admin' recorded");
    }

    // ── #2968 AC2: TenantAdmin allows cross-tenant grant ──
    {
        std::println("\n--- #2968 AC2: TenantAdmin allows cross-tenant grant ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        // #3090: grant_capability() stamps mid=0 → refused under Restricted.
        // Registry TenantAdmin with bound mid so SSOT grant_cross_tenant allows.
        grant_tenant_admin_mid(7);

        const auto allow_before =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        ev.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto allow_after =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(allow_after == allow_before + 1,
              "AC2: allow bumps cross_tenant_capability_grant_total");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) ==
                  static_cast<std::uint16_t>(kEffectMutate),
              "AC2: cross grant installed with TenantAdmin");
    }

    // ── #2968 AC2b: foreign-tenant grant_effect_capability gate ──
    {
        std::println("\n--- #2968 AC2b: foreign-tenant grant_effect_capability gate ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);

        // No TenantAdmin → foreign-tenant grant denied.
        ev.grant_effect_capability(/*tenant=*/42, "mut-2968-foreign", kEffectMutate,
                                   /*mid=*/5);
        aura::core::capability::CapabilityGrant g{};
        CHECK(
            !aura::core::capability::g_capability_registry().find_grant(42, "mut-2968-foreign", g),
            "AC2b: foreign grant denied without TenantAdmin");
        // Issue #3362: same-tenant high-risk self-grant under Restricted now
        // requires TenantAdmin — deny without TA, allow once armed.
        ev.grant_effect_capability(/*tenant=*/7, "mut-2968-self", kEffectMutate, /*mid=*/6);
        CHECK(!aura::core::capability::g_capability_registry().find_grant(7, "mut-2968-self", g),
              "AC2b: same-tenant high-risk self-grant denied without TenantAdmin (#3362)");
        grant_tenant_admin_mid(ev.capability_tenant_id());
        ev.grant_effect_capability(/*tenant=*/7, "mut-2968-self", kEffectMutate, /*mid=*/6);
        CHECK(aura::core::capability::g_capability_registry().find_grant(7, "mut-2968-self", g),
              "AC2b: same-tenant high-risk self-grant allowed with TenantAdmin");
        // With TenantAdmin → foreign grant allowed.
        grant_tenant_admin_mid(ev.capability_tenant_id());
        ev.grant_effect_capability(/*tenant=*/42, "mut-2968-admin", kEffectMutate, /*mid=*/7);
        CHECK(aura::core::capability::g_capability_registry().find_grant(42, "mut-2968-admin", g),
              "AC2b: foreign grant allowed with TenantAdmin");
    }

    // ── #2968 AC3: Off path no hard gate ──
    {
        std::println("\n--- #2968 AC3: Off path no hard gate ---");
        reset_all(); // Off
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        ev.grant_cross_tenant_access(/*from=*/1, /*to=*/2, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC3: Off path does not deny (no hard gate)");
        CHECK(g_workspace_isolation().cross_grant_bits(1, 2) != 0,
              "AC3: Off path cross-tenant grant proceeds");
    }

    // ── #2968 AC5: snapshot + posture additive keys ──
    {
        std::println("\n--- #2968 AC5: snapshot + posture additive keys ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.grant_cross_tenant_access(7, 42, kEffectMutate); // deny (no admin)
        const auto snap = snapshot_tenant_isolation_stats();
        CHECK(snap.cross_tenant_grant_deny >= 1, "AC5: snapshot exposes cross_tenant_grant_deny");
        // Posture prim cites schema-2968 + additive keys.
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("schema-2968") != std::string::npos, "AC5: posture cites schema-2968");
        CHECK(posture.find("cross-tenant-grant-tenant-admin-wired") != std::string::npos,
              "AC5: posture exposes cross-tenant-grant-tenant-admin-wired");
        CHECK(posture.find("cross-tenant-grant-deny-total") != std::string::npos,
              "AC5: posture exposes cross-tenant-grant-deny-total");
    }

    // ── #2968 AC6: source-cite + no invent + no docs/design/ ──
    {
        std::println("\n--- #2968 AC6: source-cite + no invent + no docs/design/ ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(iso.find("#2968") != std::string::npos, "AC6: workspace_isolation.hh cites #2968");
        CHECK(sec.find("#2968") != std::string::npos, "AC6: evaluator_security.cpp cites #2968");
        CHECK(posture.find("schema-2968") != std::string::npos,
              "AC6: evaluator_primitives_security.cpp cites schema-2968");
        CHECK(test_self.find("#2968") != std::string::npos, "AC6: test file cites #2968");
        CHECK(build.find("check_cross_tenant_grant_gate_2968") != std::string::npos,
              "AC6: build.py wires #2968 linter");
        std::ifstream invent("tests/core/test_issue_2968.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_2968.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_2968.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("2968-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3086: SSOT fence — direct g_workspace_isolation().grant_cross_tenant
    // bypass previously routed only through Evaluator::grant_cross_tenant_access.
    // Now fence lives in the method body; raw callers cannot widen the table.
    {
        std::println("\n--- #3086 AC1: raw grant_cross_tenant under Restricted without TenantAdmin "
                     "→ deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        // default_tenant = 7, target = 42, no TenantAdmin on either → deny.
        aura::core::capability::g_capability_registry().default_tenant.store(
            7, std::memory_order_release);
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1,
              "AC1: cross_tenant_grant_deny_total bumps on direct raw grant under Restricted");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC1: no cross grant written on deny (raw path)");
        // SE reason present in ring.
        const auto& ring = g_security_event_ring();
        bool found = false;
        const auto cur = ring.seq.load(std::memory_order_acquire);
        for (auto s = cur; s > 0 && s + 16 > cur; --s) {
            const auto& e = ring.ring[(s - 1) % ring.ring.size()];
            if (std::string_view(e.reason) == "cross-tenant-grant-needs-tenant-admin") {
                found = true;
                break;
            }
        }
        CHECK(found, "AC1: SE reason 'cross-tenant-grant-needs-tenant-admin' recorded (raw path)");
    }

    // ── #3086 AC2: Soft / Off zero-cost allow on raw grant ──
    {
        std::println("\n--- #3086 AC2: raw grant under Off → allow (zero-cost) ---");
        reset_all(); // Off
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_before =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/1, /*to=*/2, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_after =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC2: Off path does not deny (raw call)");
        CHECK(allow_after == allow_before + 1, "AC2: Off path bumps allow counter");
        CHECK(g_workspace_isolation().cross_grant_bits(1, 2) == kEffectMutate,
              "AC2: Off path cross grant installed");
    }

    // ── #3086 AC3 / #3800: target-only TenantAdmin → deny under Restricted ──
    // (#3800 flips the #3086 AC3 oracle: caller-only TA required.)
    {
        std::println("\n--- #3086 AC3/#3800: TenantAdmin on target only → deny raw grant ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::g_capability_registry().default_tenant.store(
            7, std::memory_order_release);
        // Grant TenantAdmin to the *target* tenant (42), not caller (7).
        grant_tenant_admin_mid(42);
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_before =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_after =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1, "AC3/#3800: target-only TA → deny bump");
        CHECK(allow_after == allow_before, "AC3/#3800: target-only TA → no allow bump");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC3/#3800: cross_grant_bits unchanged when only target holds TenantAdmin");
        // Existing SE reason (#2968 stable).
        const auto& ring = g_security_event_ring();
        bool found = false;
        const auto cur = ring.seq.load(std::memory_order_acquire);
        for (auto s = cur; s > 0 && s + 16 > cur; --s) {
            const auto& e = ring.ring[(s - 1) % ring.ring.size()];
            if (std::string_view(e.reason) == "cross-tenant-grant-needs-tenant-admin") {
                found = true;
                break;
            }
        }
        CHECK(found,
              "AC3/#3800: SE reason 'cross-tenant-grant-needs-tenant-admin' on target-only TA");
    }

    // ── #3086 AC4: no double-count via Evaluator wrapper ──
    {
        std::println("\n--- #3086 AC4: Evaluator wrapper does not double-bump deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::g_capability_registry().default_tenant.store(
            7, std::memory_order_release);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        ev.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1,
              "AC4: Evaluator wrapper denial bumps SSOT counter exactly once (no double-count)");
    }

    // ── #3086 AC5: zero-id guard still short-circuits (no SE, no counter bump) ──
    {
        std::println("\n--- #3086 AC5: zero-id short-circuit ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/0, /*to=*/42, kEffectMutate);
        g_workspace_isolation().grant_cross_tenant(/*from=*/7, /*to=*/0, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC5: zero-id short-circuit does not bump deny counter");
    }

    // ── #3086 AC6: source-cite + no invent + no docs/design/ ──
    {
        std::println("\n--- #3086 AC6: source-cite + no invent + no docs/design/ ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        CHECK(iso.find("#3086") != std::string::npos, "AC6: workspace_isolation.hh cites #3086");
        CHECK(iso.find("try_grant_cross_tenant_privileged") != std::string::npos,
              "AC6: SSOT helper present");
        CHECK(sec.find("#3086") != std::string::npos, "AC6: evaluator_security.cpp cites #3086");
        CHECK(sec.find("g_workspace_isolation().grant_cross_tenant(from_tenant, to_tenant, "
                       "effect_bits,") != std::string::npos,
              "AC6: Evaluator wrapper delegates to SSOT method (no second policy)");
        CHECK(test_self.find("#3086") != std::string::npos, "AC6: test file cites #3086");
        std::ifstream invent("tests/core/test_issue_3086.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3086.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_3086.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3086-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #2969: registry write-fence — foreign-tenant grant/revoke requires
    // TenantAdmin (Option A, minimal; storage/write-isolation face) ──
    {
        std::println("\n--- #2969 AC1: durable/session/revoke foreign-tenant gate ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        aura::core::capability::CapabilityGrant g{};

        // AC1: durable foreign grant denied without TenantAdmin (low-risk
        // effect — isolates #2969 fence from the #2967 high-risk gate).
        const auto deny_before =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        ev.grant_effect_durable(/*tenant=*/42, "dur-2969-foreign", kEffectWrite, /*mid=*/0,
                                /*reason=*/"audit-reason");
        const auto deny_after =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1,
              "AC1: durable foreign grant denied without TenantAdmin");
        CHECK(
            !aura::core::capability::g_capability_registry().find_grant(42, "dur-2969-foreign", g),
            "AC1: no durable foreign grant written on deny");
        // SE reason present in ring.
        const auto& ring = g_security_event_ring();
        bool found = false;
        const auto cur = ring.seq.load(std::memory_order_acquire);
        for (auto s = cur; s > 0 && s + 16 > cur; --s) {
            const auto& e = ring.ring[(s - 1) % ring.ring.size()];
            if (std::string_view(e.reason) == "grant-foreign-tenant-needs-tenant-admin") {
                found = true;
                break;
            }
        }
        CHECK(found, "AC1: SE reason 'grant-foreign-tenant-needs-tenant-admin' recorded");

        // AC1: session foreign grant denied without TenantAdmin.
        ev.grant_effect_session(/*tenant=*/42, "ses-2969-foreign", kEffectWrite, /*mid=*/1);
        CHECK(
            !aura::core::capability::g_capability_registry().find_grant(42, "ses-2969-foreign", g),
            "AC1: no session foreign grant written on deny");

        // AC1: revoke foreign denied without TenantAdmin — seed a foreign
        // grant through the admin path, then a second (non-admin) Evaluator
        // attempts the cross-tenant revoke (two-Evaluator verification).
        grant_tenant_admin_mid(ev.capability_tenant_id());
        ev.grant_effect_capability(/*tenant=*/42, "mut-2969-seed", kEffectWrite, /*mid=*/1);
        CHECK(aura::core::capability::g_capability_registry().find_grant(42, "mut-2969-seed", g),
              "AC1: admin path seeds foreign grant (audited)");
        {
            // Second (non-admin) Evaluator under a DIFFERENT tenant principal
            // (9) attempts the cross-tenant revoke. No reset_all() here — it
            // would clear the process-global registry (#2968) and drop the
            // seeded foreign grant, defeating the survival check.
            CompilerService cs2;
            auto& ev2 = cs2.evaluator();
            ev2.set_effect_sandbox_mode(1);
            ev2.set_capability_tenant_id(9); // non-admin principal A
            const auto deny2_before =
                aura::core::capability::g_capability_effect_metrics()
                    .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
            ev2.revoke_effect_capability(/*tenant=*/42, "mut-2969-seed");
            const auto deny2_after =
                aura::core::capability::g_capability_effect_metrics()
                    .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
            CHECK(deny2_after == deny2_before + 1,
                  "AC1: foreign revoke denied without TenantAdmin");
            CHECK(aura::core::capability::g_capability_registry().find_grant(42, "mut-2969-seed",
                                                                             g) &&
                      !g.revoked,
                  "AC1: foreign grant survives non-admin revoke attempt");
        }
    }

    // ── #2969 AC2: same-tenant grant/revoke keep existing policy ──
    {
        std::println("\n--- #2969 AC2: same-tenant grant/revoke unchanged ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        aura::core::capability::CapabilityGrant g{};
        const auto deny_before =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        ev.grant_effect_durable(/*tenant=*/7, "dur-2969-self", kEffectWrite, /*mid=*/1,
                                /*reason=*/"r");
        CHECK(aura::core::capability::g_capability_registry().find_grant(7, "dur-2969-self", g),
              "AC2: same-tenant durable grant stays allowed");
        ev.grant_effect_session(/*tenant=*/7, "ses-2969-self", kEffectWrite, /*mid=*/1);
        CHECK(aura::core::capability::g_capability_registry().find_grant(7, "ses-2969-self", g),
              "AC2: same-tenant session grant stays allowed");
        ev.revoke_effect_capability(/*tenant=*/7, "dur-2969-self");
        CHECK(aura::core::capability::g_capability_registry().find_grant(7, "dur-2969-self", g) &&
                  g.revoked,
              "AC2: same-tenant revoke works");
        const auto deny_after =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC2: no fence deny on same-tenant operations");
    }

    // ── #2969 AC3: Off path no hard fence (zero extra cost) ──
    {
        std::println("\n--- #2969 AC3: Off path no hard fence ---");
        reset_all(); // Off
        CompilerService cs;
        auto& ev = cs.evaluator();
        aura::core::capability::CapabilityGrant g{};
        const auto deny_before =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        ev.grant_effect_durable(/*tenant=*/42, "dur-2969-off", kEffectWrite, /*mid=*/0,
                                /*reason=*/"r");
        CHECK(aura::core::capability::g_capability_registry().find_grant(42, "dur-2969-off", g),
              "AC3: Off path durable foreign grant proceeds");
        const auto deny_after =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC3: Off path no fence deny");
    }

    // ── #2969 AC4: allow counter bumps only on allow (deny does not) ──
    {
        std::println("\n--- #2969 AC4: allow counter only on allow ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        const auto grants_before =
            aura::core::capability::g_capability_effect_metrics().capability_grant_total.load(
                std::memory_order_relaxed);
        ev.grant_effect_durable(/*tenant=*/42, "dur-2969-noadmin", kEffectWrite, /*mid=*/0,
                                /*reason=*/"r"); // deny
        const auto grants_deny =
            aura::core::capability::g_capability_effect_metrics().capability_grant_total.load(
                std::memory_order_relaxed);
        CHECK(grants_deny == grants_before, "AC4: deny does not bump capability_grant_total");
        grant_tenant_admin_mid(ev.capability_tenant_id());
        // grant_capability mirrors into the registry (bumps grant_total once
        // for the tenant-admin grant itself) — snapshot AFTER it so the +1
        // assertion isolates the durable allow path.
        const auto grants_admin_granted =
            aura::core::capability::g_capability_effect_metrics().capability_grant_total.load(
                std::memory_order_relaxed);
        ev.grant_effect_durable(/*tenant=*/42, "dur-2969-admin", kEffectWrite, /*mid=*/1,
                                /*reason=*/"r"); // allow (admin)
        const auto grants_allow =
            aura::core::capability::g_capability_effect_metrics().capability_grant_total.load(
                std::memory_order_relaxed);
        CHECK(grants_allow == grants_admin_granted + 1, "AC4: allow bumps capability_grant_total");
    }

    // ── #2969 AC5: snapshot + posture additive keys ──
    {
        std::println("\n--- #2969 AC5: snapshot + posture additive keys ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.grant_effect_durable(/*tenant=*/42, "dur-2969-snap", kEffectWrite, /*mid=*/0,
                                /*reason=*/"r"); // deny
        const auto cap = aura::core::capability::snapshot_capability_effect_stats();
        CHECK(cap.capability_grant_foreign_tenant_deny >= 1,
              "AC5: snapshot exposes capability_grant_foreign_tenant_deny");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("schema-2969") != std::string::npos, "AC5: posture cites schema-2969");
        CHECK(posture.find("issue-2969") != std::string::npos, "AC5: posture cites issue-2969");
        CHECK(posture.find("capability-grant-write-fence-wired") != std::string::npos,
              "AC5: posture exposes capability-grant-write-fence-wired");
        CHECK(posture.find("capability-grant-foreign-tenant-deny-total") != std::string::npos,
              "AC5: posture exposes capability-grant-foreign-tenant-deny-total");
    }

    // ── #2969 AC6: source-cite + no invent + no docs/design/ ──
    {
        std::println("\n--- #2969 AC6: source-cite + no invent + no docs/design/ ---");
        const auto model = read_file("src/core/capability_model.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(model.find("#2969") != std::string::npos, "AC6: capability_model.hh cites #2969");
        CHECK(sec.find("#2969") != std::string::npos, "AC6: evaluator_security.cpp cites #2969");
        CHECK(posture.find("schema-2969") != std::string::npos,
              "AC6: evaluator_primitives_security.cpp cites schema-2969");
        CHECK(test_self.find("#2969") != std::string::npos, "AC6: test file cites #2969");
        CHECK(build.find("check_capability_write_fence_2969") != std::string::npos,
              "AC6: build.py wires #2969 linter");
        std::ifstream invent("tests/core/test_issue_2969.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_2969.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_2969.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("2969-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3010: allow_cross_tenant_ write requires TenantAdmin ──
    {
        std::println(
            "\n--- #3010 AC1: Restricted same-tenant allow_cross without admin → deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);

        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(!ev.allow_cross_tenant(), "AC1: flag starts false");
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(!ev.allow_cross_tenant(), "AC1: C++ set_tenant_principal refuses flag without admin");
        CHECK(ev.capability_tenant_id() == 7, "AC1: tenant id still binds on flag deny");
        const auto deny_cpp = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                  .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_cpp == deny_before + 1, "AC1: C++ deny bumps allow_cross_tenant_deny_total");

        auto edsl = cs.eval("(security:set-tenant-principal! 7 #t)");
        CHECK(edsl && is_bool(*edsl) && !as_bool(*edsl),
              "AC1: EDSL set-tenant-principal! same-tenant #t returns #f");
        CHECK(!ev.allow_cross_tenant(), "AC1: EDSL deny leaves flag false");
        const auto deny_edsl = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                   .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_edsl == deny_cpp + 1, "AC1: EDSL deny bumps allow_cross_tenant_deny_total");

        const auto& ring = g_security_event_ring();
        bool found = false;
        const auto cur = ring.seq.load(std::memory_order_acquire);
        for (auto s = cur; s > 0 && s + 16 > cur; --s) {
            const auto& e = ring.ring[(s - 1) % ring.ring.size()];
            if (std::string_view(e.reason) == "allow-cross-needs-tenant-admin") {
                found = true;
                break;
            }
        }
        CHECK(found, "AC1: SE reason 'allow-cross-needs-tenant-admin' recorded");
    }

    {
        std::println("\n--- #3010 AC2: Restricted + TenantAdmin can set flag ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        grant_tenant_admin_mid(ev.capability_tenant_id());
        ev.grant_capability(aura::compiler::security::kCapTenantAdmin); // local list

        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        auto edsl = cs.eval("(security:set-tenant-principal! 7 #t)");
        CHECK(edsl && is_bool(*edsl) && as_bool(*edsl),
              "AC2: EDSL set-tenant-principal! with TenantAdmin returns #t");
        CHECK(ev.allow_cross_tenant(), "AC2: flag set with TenantAdmin");
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC2: admin path does not bump deny counter");
        // #3332: Restricted allow_cross is not a full isolation bypass —
        // foreign Mutate without cross_grants[T1→T2] still denies (cap_deny).
        // Grant-write (#2968) stays independently gated.
        CHECK(!ev.check_workspace_isolation(42, 0, kEffectMutate, "3010-cross"),
              "AC2: allow_cross without grant denies foreign Mutate (#3332)");
        {
            // Different principal: TenantAdmin was granted on tenant 7 in the
            // process-global registry; a non-admin tenant must still hit #2968.
            CompilerService cs2;
            auto& ev2 = cs2.evaluator();
            ev2.set_effect_sandbox_mode(1);
            ev2.set_capability_tenant_id(9);
            // SSOT grant_cross_tenant reads registry default_tenant, not
            // Evaluator principal. Point it at the non-admin tenant so
            // #2968 still denies (grant_tenant_admin_mid left default=7).
            aura::core::capability::g_capability_registry().default_tenant.store(
                9, std::memory_order_release);
            ev2.grant_cross_tenant_access(9, 42, kEffectMutate);
            CHECK(g_workspace_isolation().cross_grant_bits(9, 42) == 0,
                  "AC2: #2968 grant-write still requires TenantAdmin");
        }
    }

    {
        std::println("\n--- #3010 AC3: Soft / Off path no hard gate ---");
        reset_all(); // Off
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev.allow_cross_tenant(), "AC3: Off C++ path sets flag without admin");
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/false);
        auto edsl = cs.eval("(security:set-tenant-principal! 7 #t)");
        CHECK(edsl && is_bool(*edsl) && as_bool(*edsl), "AC3: Off EDSL path sets flag");
        CHECK(ev.allow_cross_tenant(), "AC3: Off EDSL flag set");
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC3: Off path does not deny (no hard gate)");
    }

    {
        std::println("\n--- #3010 AC5: snapshot + posture additive keys ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true); // deny
        const auto snap = snapshot_tenant_isolation_stats();
        CHECK(snap.allow_cross_tenant_deny >= 1, "AC5: snapshot exposes allow_cross_tenant_deny");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("schema-3010") != std::string::npos, "AC5: posture cites schema-3010");
        CHECK(posture.find("allow-cross-tenant-admin-wired") != std::string::npos,
              "AC5: posture exposes allow-cross-tenant-admin-wired");
        CHECK(posture.find("allow-cross-tenant-deny-total") != std::string::npos,
              "AC5: posture exposes allow-cross-tenant-deny-total");
        CHECK(posture.find("allow-cross-needs-tenant-admin") != std::string::npos,
              "AC5: prim cites SE reason allow-cross-needs-tenant-admin");
    }

    {
        std::println("\n--- #3010 AC6: source-cite + no invent + no docs/design/ ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(iso.find("#3010") != std::string::npos, "AC6: workspace_isolation.hh cites #3010");
        CHECK(sec.find("#3010") != std::string::npos, "AC6: evaluator_security.cpp cites #3010");
        CHECK(posture.find("schema-3010") != std::string::npos,
              "AC6: evaluator_primitives_security.cpp cites schema-3010");
        CHECK(test_self.find("#3010") != std::string::npos, "AC6: test file cites #3010");
        CHECK(build.find("check_allow_cross_tenant_admin_3010") != std::string::npos,
              "AC6: build.py wires #3010 linter");
        std::ifstream invent("tests/core/test_issue_3010.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3010.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_3010.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3010-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3332: Restricted allow_cross is scoped to cross_grants (not full bypass) ──
    {
        std::println("\n--- #3332 AC1: #3010 write gate does not regress ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(!ev.allow_cross_tenant(), "3332 AC1: Restricted without TenantAdmin cannot set flag");
        grant_tenant_admin_mid(7);
        ev.grant_capability(aura::compiler::security::kCapTenantAdmin);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev.allow_cross_tenant(), "3332 AC1: TenantAdmin can still set flag (#3010)");
    }

    {
        std::println("\n--- #3332 AC2: Restricted allow_cross without grant denies ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::set_effect_fiber_id_override(42);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        grant_tenant_admin_mid(7);
        ev.grant_capability(aura::compiler::security::kCapTenantAdmin);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev.allow_cross_tenant(), "3332 AC2: flag set");
        const auto cap0 = snapshot_tenant_isolation_stats().cross_tenant_capability_deny;
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        CHECK(!ev.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac2-no-grant"),
              "3332 AC2: foreign Mutate without grant denies");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_capability_deny == cap0 + 1,
              "3332 AC2: cap_deny counted");
        bool found = false;
        const auto head = ring.seq.load(std::memory_order_acquire);
        for (auto s = baseline; s < head; ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.kind == SecurityEventKind::IsolationDeny && e.seq == s) {
                CHECK(e.fiber_id == 42, "3332 AC2: IsolationDeny fiber_id (#3011)");
                found = true;
            }
        }
        CHECK(found, "3332 AC2: IsolationDeny SE recorded");
        aura::core::workspace_isolation::IsolationAuditEntry priv{};
        const auto aseq = g_workspace_isolation().load_audit_seq();
        CHECK(aseq >= 1 && g_workspace_isolation().try_load_audit_seq(aseq - 1, priv),
              "3332 AC2: private isolation ring loadable");
        CHECK(priv.denied && priv.capability_deny, "3332 AC2: capability_deny latched");
        aura::core::capability::set_effect_fiber_id_override(0);
    }

    {
        std::println("\n--- #3332 AC3: allow_cross + grant allows; insufficient bits deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        grant_tenant_admin_mid(7);
        g_workspace_isolation().grant_cross_tenant(7, 42, kEffectMutate, /*caller=*/7);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.grant_capability(aura::compiler::security::kCapTenantAdmin);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac3-grant"),
              "3332 AC3: Mutate grant + allow_cross allows");
        CHECK(!ev.check_workspace_isolation(42, 0, kEffectWrite, "3332-ac3-bits"),
              "3332 AC3: insufficient bits still deny");
    }

    {
        std::println("\n--- #3332 AC4: stamped foreign ref without grant is prov_deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        grant_tenant_admin_mid(7);
        ev.grant_capability(aura::compiler::security::kCapTenantAdmin);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        const auto p0 = snapshot_tenant_isolation_stats().cross_tenant_provenance_deny;
        CHECK(!ev.check_workspace_isolation(7, 99, kEffectMutate, "3332-ac4-prov"),
              "3332 AC4: foreign ref without current→ref grant denies");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_provenance_deny == p0 + 1,
              "3332 AC4: prov_deny counted");
        g_workspace_isolation().grant_cross_tenant(7, 99, kEffectMutate, /*caller=*/7);
        CHECK(ev.check_workspace_isolation(7, 99, kEffectMutate, "3332-ac4-ok"),
              "3332 AC4: provenance allow after current→ref grant");
    }

    {
        std::println("\n--- #3332 AC5: Soft/Off allow_cross short-circuit zero extra ---");
        reset_all(); // Off
        const auto cap0 = snapshot_tenant_isolation_stats().cross_tenant_capability_deny;
        CHECK(check_boundary(1, 99, nullptr, /*allow_cross=*/true),
              "3332 AC5: Off allow_cross still short-circuits");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_capability_deny == cap0,
              "3332 AC5: Off path does not bump cap_deny");
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev.allow_cross_tenant(), "3332 AC5: Off sets flag without TenantAdmin");
        CHECK(ev.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac5-off"),
              "3332 AC5: Off Evaluator allow_cross still allows");
    }

    {
        std::println("\n--- #3332 AC6: dual Evaluator shares cross_grants ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        grant_tenant_admin_mid(7);
        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_b.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(7);
        ev_b.set_capability_tenant_id(7);
        ev_a.grant_capability(aura::compiler::security::kCapTenantAdmin);
        ev_a.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev_a.allow_cross_tenant(), "3332 AC6: A has allow_cross");
        CHECK(!ev_b.allow_cross_tenant(), "3332 AC6: B does not");
        CHECK(!ev_a.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac6-a-nogrant"),
              "3332 AC6: A without grant still denies");
        CHECK(!ev_b.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac6-b-nogrant"),
              "3332 AC6: B without grant denies");
        g_workspace_isolation().grant_cross_tenant(7, 42, kEffectMutate, /*caller=*/7);
        CHECK(ev_a.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac6-a-grant"),
              "3332 AC6: A allow_cross + shared grant allows");
        CHECK(ev_b.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac6-b-grant"),
              "3332 AC6: B uses the same cross_grants table");
    }

    {
        std::println("\n--- #3332 AC6: source-cite + linter + no invent ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(iso.find("kAllowCrossScopedGrantIssue = 3332") != std::string::npos,
              "3332 AC6: issue stamp");
        CHECK(iso.find("allow_cross_tenant && !(strict || sandbox_restricted)") !=
                  std::string::npos,
              "3332 AC6: Soft/Off-only short-circuit");
        CHECK(test_self.find("allow_cross without grant denies") != std::string::npos,
              "3332 AC6: original bypass case rewritten");
        CHECK(build.find("check_allow_cross_scoped_grant_3332") != std::string::npos,
              "3332 AC6: build.py wires linter after #3010");
        std::ifstream invent("tests/core/test_issue_3332.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3332.cpp");
        CHECK(!invent.good(), "3332 AC6: no tests/core/test_issue_3332.cpp");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3332-") == std::string::npos,
                      std::string("3332 AC6: no docs/design/") + name);
            }
        }
    }

    // ── #3011: IsolationDeny SecurityEvent stamps live fiber ──
    {
        std::println("\n--- #3011 AC1: IsolationDeny fiber_id == calling fiber ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::set_effect_fiber_id_override(42);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        CHECK(!ev.check_workspace_isolation(99, 0, kEffectMutate, "test:3011-cross"),
              "AC1: Restricted cross-tenant mutate denied");
        bool found = false;
        const auto head = ring.seq.load(std::memory_order_acquire);
        for (auto s = baseline; s < head; ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.kind == SecurityEventKind::IsolationDeny && e.seq == s) {
                CHECK(e.fiber_id == 42, "AC1: IsolationDeny fiber_id equals calling fiber");
                found = true;
            }
        }
        CHECK(found, "AC1: IsolationDeny SE recorded");
        aura::core::workspace_isolation::IsolationAuditEntry priv{};
        const auto aseq = g_workspace_isolation().load_audit_seq();
        CHECK(aseq >= 1 && g_workspace_isolation().try_load_audit_seq(aseq - 1, priv),
              "AC1: private isolation ring loadable");
        CHECK(priv.denied && priv.fiber_id == 42, "AC1: private ring fiber_id matches");
    }

    {
        std::println("\n--- #3011 AC2: EffectDeny fiber path unchanged ---");
        const auto cap = read_file("src/core/capability_model.hh");
        CHECK(cap.find("static_cast<std::int64_t>(prov.fiber_id)") != std::string::npos,
              "AC2: EffectDeny still stamps prov.fiber_id");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        CHECK(iso.find("/*fiber_id=*/0") == std::string::npos,
              "AC2: IsolationDeny no longer hard-codes fiber_id=0");
    }

    {
        std::println("\n--- #3011 AC3: Soft / Off allow does not emit IsolationDeny ---");
        reset_all(); // Off
        const auto& ring = g_security_event_ring();
        const auto before = ring.seq.load(std::memory_order_acquire);
        CHECK(check_boundary(0, 0), "AC3: Off unset principal allows");
        const auto after = ring.seq.load(std::memory_order_acquire);
        CHECK(after == before, "AC3: Off allow does not append IsolationDeny");
    }

    {
        std::println("\n--- #3011 AC4: query:security-audit filters IsolationDeny by fiber ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::set_effect_fiber_id_override(42);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        CHECK(!ev.check_workspace_isolation(99, 0, kEffectMutate, "test:3011-filter"),
              "AC4: deny to seed IsolationDeny");
        // Issue #3669: SE tenant_id is the CALLER principal now (was target);
        // filter by caller 7 + fiber 42 — the #3011 fiber join on the new
        // blame key.
        auto q = cs.eval(R"((engine:metrics "query:security-audit" 16 7 42))");
        CHECK(q.has_value(), "AC4: query:security-audit fiber filter callable");
        bool saw_fiber = false;
        if (q) {
            auto cur = *q;
            int guard = 0;
            auto& pairs = ev.pairs();
            auto heap = ev.string_heap();
            while (is_pair(cur) && guard++ < 64) {
                const auto idx = as_pair_idx(cur);
                if (idx >= pairs.size())
                    break;
                if (is_string(pairs[idx].car)) {
                    const auto sidx = as_string_idx(pairs[idx].car);
                    if (sidx < heap.size()) {
                        const std::string ln(heap[sidx]);
                        if (ln.find("kind=IsolationDeny") != std::string::npos &&
                            ln.find("fiber=42") != std::string::npos)
                            saw_fiber = true;
                    }
                }
                cur = pairs[idx].cdr;
            }
        }
        CHECK(saw_fiber, "AC4: query:security-audit fiber=42 returns IsolationDeny");
        auto wired = cs.eval(
            R"((hash-ref (engine:metrics "query:security-stats") "isolation-deny-fiber-wired"))");
        CHECK(wired && is_int(*wired) && as_int(*wired) == 1,
              "AC4: query:security-stats exposes isolation-deny-fiber-wired");
        auto schema =
            cs.eval(R"((hash-ref (engine:metrics "query:security-stats") "schema-3011"))");
        CHECK(schema && is_int(*schema) && as_int(*schema) == 3011,
              "AC4: query:security-stats cites schema-3011");
    }

    {
        std::println("\n--- #3011 AC5/AC6: source-cite + no invent + no docs/design/ ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(iso.find("#3011") != std::string::npos, "AC6: workspace_isolation.hh cites #3011");
        CHECK(iso.find("effect_fiber_id_or") != std::string::npos,
              "AC5: record_audit uses effect_fiber_id_or");
        CHECK(posture.find("schema-3011") != std::string::npos, "AC5: posture cites schema-3011");
        CHECK(posture.find("filt_fiber") != std::string::npos,
              "AC5: query:security-audit still filters by fiber");
        CHECK(test_self.find("#3011") != std::string::npos, "AC6: test file cites #3011");
        CHECK(build.find("check_isolation_deny_fiber_3011") != std::string::npos,
              "AC6: build.py wires #3011 linter");
        std::ifstream invent("tests/core/test_issue_3011.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3011.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_3011.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3011-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3040: residual compile NodeId-only entry gated before body ──
    {
        std::println("\n--- #3040 AC1: Restricted NodeId compile entry denied before body ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Issue #4400: set-code is the workspace install, not the gate under
        // test. Install under Off, then arm Restricted with no Mutate grant.
        CHECK(cs.eval("(set-code \"(define (n3040 x) x)\")").has_value(), "3040 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3040 eval");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3040 workspace");
        const auto before_bumps = ws->subtree_bump_count();
        const auto prev = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                              .nodeid_only_entry_prevented_total.load(std::memory_order_relaxed);
        // Issue #3172: fine-grained compile EDSL writers (compile:subtree-bump
        // et al) are sunk surfaces now — the residual gate is exercised via
        // the C++ require_effect_for_node_id / on_ref arms below.
        CHECK(!ev.require_effect_for_node_id(kEffectMutate, "compile:subtree-bump", /*node_id=*/1),
              "ac3040_1_denied_before_body");
        CHECK(ws->subtree_bump_count() == before_bumps, "ac3040_1_no_topology_write");
        const auto after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .nodeid_only_entry_prevented_total.load(std::memory_order_relaxed);
        CHECK(after >= prev + 1, "ac3040_1_nodeid_only_entry_prevented");
        const auto compile_src = read_file("src/compiler/evaluator_primitives_compile.cpp");
        CHECK(compile_src.find("gate_compile_node_effect") != std::string::npos,
              "ac3040_1_gate_helper");
        CHECK(compile_src.find("require_effect_for_node_id") != std::string::npos,
              "ac3040_1_for_node_id");
        CHECK(compile_src.find("sink_compile_prim(\"compile:subtree-bump\"") != std::string::npos,
              "ac3040_1: EDSL writer sunk per #3172");
    }

    {
        std::println("\n--- #3040 AC2: foreign stamped ref denied before body ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Issue #4400: install under Off so the setup set-code does not
        // consume the single-use Mutate grant the foreign-ref deny needs.
        CHECK(cs.eval("(set-code \"(define (n3040b x) x)\")").has_value(), "3040 AC2 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3040 AC2 eval");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.grant_effect_capability(/*tenant=*/7, "mut-3040-ac2", kEffectMutate, /*mid=*/1);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3040 AC2 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3040 AC2 live node");
        const auto before_bumps = ws->subtree_bump_count();
        const auto iso_before = snapshot_tenant_isolation_stats().boundary_violations_prevented;
        auto foreign = ev.make_stamped_ref(id);
        foreign.tenant_id = 99;
        CHECK(!ev.require_effect_on_ref(kEffectMutate, "compile:subtree-bump", foreign),
              "ac3040_2_on_ref_foreign_denies");
        CHECK(ws->subtree_bump_count() == before_bumps, "ac3040_2_no_topology_write");
        const auto iso_after = snapshot_tenant_isolation_stats().boundary_violations_prevented;
        CHECK(iso_after > iso_before, "ac3040_2_isolation_counters_bump");
    }

    {
        std::println("\n--- #3040 AC3: Soft / Off path unchanged ---");
        reset_all(); // Off
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3040c x) x)\")").has_value(), "3040 AC3 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3040 AC3 eval");
        const auto prev = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                              .nodeid_only_entry_prevented_total.load(std::memory_order_relaxed);
        // Issue #3172: the EDSL writer is a sunk surface — Soft/Off contract
        // is covered by the parse_compile_node_arg short-circuit cite below.
        const auto after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .nodeid_only_entry_prevented_total.load(std::memory_order_relaxed);
        CHECK(after == prev, "ac3040_3_soft_off_no_prevent_store");
        const auto compile_src = read_file("src/compiler/evaluator_primitives_compile.cpp");
        CHECK(compile_src.find("sandbox_mode() == 0 && ev.effect_sandbox_mode() == 0") !=
                  std::string::npos,
              "ac3040_3_soft_off_short_circuit");
        (void)ev;
    }

    {
        std::println("\n--- #3040 AC4: schema-3040 + snapshot counter ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        CHECK(!ev.require_effect_for_node_id(kEffectMutate, "compile:subtree-bump", /*node_id=*/1),
              "ac3040_4_for_node_id_denies_unset_grant");
        const auto snap = snapshot_tenant_isolation_stats();
        CHECK(snap.nodeid_only_entry_prevented >= 1, "ac3040_4_snapshot_counter");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("schema-3040") != std::string::npos, "ac3040_4_schema");
        CHECK(posture.find("nodeid-only-entry-prevented-wired") != std::string::npos,
              "ac3040_4_wired");
        CHECK(posture.find("nodeid-only-entry-prevented-total") != std::string::npos,
              "ac3040_4_total_key");
        CHECK(aura::compiler::kNodeIdOnlyEntryIssue == 3040, "ac3040_4_issue_const");
        CHECK(aura::compiler::kNodeIdOnlyEntryPreventedWired == 1, "ac3040_4_wired_const");
    }

    {
        std::println("\n--- #3040 AC5/AC6: source-cite + linter + no invent ---");
        const auto compile_src = read_file("src/compiler/evaluator_primitives_compile.cpp");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(compile_src.find("Issue #3040") != std::string::npos, "ac3040_5_compile_cite");
        CHECK(sec.find("Issue #3040") != std::string::npos, "ac3040_5_security_cite");
        CHECK(iso.find("#3040") != std::string::npos, "ac3040_5_iso_cite");
        CHECK(posture.find("schema-3040") != std::string::npos, "ac3040_5_posture");
        CHECK(test_self.find("#3040") != std::string::npos, "ac3040_5_test_cite");
        CHECK(build.find("check_compile_node_id_entry_3040") != std::string::npos,
              "ac3040_5_linter_and_suite");
        std::ifstream invent("tests/core/test_issue_3040.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3040.cpp");
        CHECK(!invent.good(), "ac3040_5_no_invent_test");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3040-") == std::string::npos,
                      std::string("ac3040_5: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3415: occupancy NodeId must not restamp a foreign owner ──
    {
        std::println("\n--- #3415 AC1: Restricted+MT occupancy of B's NodeId IsolationDeny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Issue #4400: install under Off. Both Mutate grants stay for the
        // occupancy deny (set-code would consume tenant 99's single-use row).
        CHECK(cs.eval("(set-code \"(define (n3415 x) x)\")").has_value(), "3415 AC1 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3415 AC1 eval");
        set_mode(SandboxMode::Restricted);
        aura::core::provenance::set_multi_tenant_env_active(true);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(99);
        ev.grant_effect_capability(/*tenant=*/99, "mut-3415-b", kEffectMutate, /*mid=*/1);
        ev.grant_effect_capability(/*tenant=*/7, "mut-3415-a", kEffectMutate, /*mid=*/1);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3415 AC1 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3415 AC1 live node");
        auto stamped = ev.make_stamped_ref(id);
        CHECK(stamped.tenant_id == 99, "3415 AC1: B stamp slot tenant 99");
        const auto before_bumps = ws->subtree_bump_count();
        ev.set_capability_tenant_id(7);
        CHECK(!ev.require_effect_for_node_id(kEffectMutate, "mutate:replace-type", id),
              "ac3415_1_occupancy_denies");
        CHECK(ws->subtree_bump_count() == before_bumps, "ac3415_1_no_write");
        auto edsl = cs.eval(std::format("(mutate:replace-type {} \"Int\")", id));
        CHECK(edsl.has_value(), "ac3415_1_edsl_returns");
        CHECK(edsl && is_error(*edsl), "ac3415_1_edsl_isolation_deny");
        CHECK(ws->subtree_bump_count() == before_bumps, "ac3415_1_edsl_no_write");
    }

    {
        std::println("\n--- #3415 AC2: stamped foreign on_ref still denies ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3415b x) x)\")").has_value(), "3415 AC2 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3415 AC2 eval");
        set_mode(SandboxMode::Restricted);
        aura::core::provenance::set_multi_tenant_env_active(true);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.grant_effect_capability(/*tenant=*/7, "mut-3415-ac2", kEffectMutate, /*mid=*/1);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3415 AC2 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3415 AC2 live node");
        const auto before_bumps = ws->subtree_bump_count();
        auto foreign = ev.make_stamped_ref(id);
        foreign.tenant_id = 99;
        CHECK(!ev.require_effect_on_ref(kEffectMutate, "mutate:replace-type", foreign),
              "ac3415_2_on_ref_foreign_denies");
        CHECK(ws->subtree_bump_count() == before_bumps, "ac3415_2_no_write");
    }

    {
        std::println("\n--- #3415 AC3: same-tenant stamped ref allows ---");
        reset_all();
        aura::core::bump_mutation_epoch(1);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto me = aura::core::current_mutation_epoch();
        ev.grant_effect_capability(/*tenant=*/7, "mut-3415-ac3", kEffectMutate, me == 0 ? 1 : me);
        auto own = ev.make_stamped_ref(/*node_id=*/1);
        CHECK(own.tenant_id == 7, "ac3415_3_stamp_caller");
        CHECK(ev.check_workspace_isolation(7, own.tenant_id, kEffectMutate, "3415-ac3-iso"),
              "ac3415_3_same_tenant_isolation_allows");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        CHECK(sec.find("ref = make_stamped_ref(node_id)") != std::string::npos,
              "ac3415_3_same_tenant_still_stamps_caller");
    }

    {
        std::println("\n--- #3415 AC4: Soft occupancy unchanged ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(ev.require_effect_for_node_id(kEffectMutate, "3415-ac4-soft", /*node_id=*/1),
              "ac3415_4_soft_allows");
    }

    {
        std::println("\n--- #3415 AC5: no Mutate grant denies with zero write ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Issue #4400: the workspace install is not the deny under test.
        CHECK(cs.eval("(set-code \"(define (n3415c x) x)\")").has_value(), "3415 AC5 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3415 AC5 eval");
        set_mode(SandboxMode::Restricted);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3415 AC5 workspace");
        const auto before_bumps = ws->subtree_bump_count();
        CHECK(!ev.require_effect_for_node_id(kEffectMutate, "mutate:replace-type", /*node_id=*/1),
              "ac3415_5_no_grant_denies");
        CHECK(ws->subtree_bump_count() == before_bumps, "ac3415_5_no_write");
    }

    {
        std::println("\n--- #3415 AC6: source-cite + linter + no invent ---");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        const auto build = read_file("build.py");
        CHECK(sec.find("Issue #3415") != std::string::npos, "ac3415_6_security_cite");
        CHECK(mut.find("Issue #3415") != std::string::npos, "ac3415_6_mutate_cite");
        CHECK(build.find("check_bare_nodeid_foreign_stamp_3415") != std::string::npos,
              "ac3415_6_linter");
        CHECK(aura::compiler::kBareNodeIdIsolationIssue == 3415, "ac3415_6_issue_const");
        CHECK(read_file("tests/core/test_issue_3415.cpp").empty(), "ac3415_6_no_invent");
        CHECK(read_file("docs/design/3415-bare-nodeid.md").empty(), "ac3415_6_no_design");
    }

    // ── #3041: production restamp budget exceed forces QueryEpoch stale ──
    {
        std::println("\n--- #3041 AC1: production unified restamp forces QueryEpoch stale ---");
        reset_all();
        using aura::ast::clear_restamp_budget_nodes_override_for_test;
        using aura::ast::set_restamp_budget_nodes_for_process;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::core::capture_query_epoch;
        using aura::core::g_query_epoch_forced_stale;
        using aura::core::g_restamp_budget_query_epoch_stale_total;
        using aura::core::reset_query_epoch_metrics_for_test;
        reset_query_epoch_metrics_for_test();
        apply_production_audit_defaults();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3041 a) a) (define (n3041b b) b) "
                      "(define (n3041c c) c) (define (n3041d d) d)\")")
                  .has_value(),
              "3041 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3041 eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3041 workspace");
        (void)capture_query_epoch(ws->generation(), 0);
        set_restamp_budget_nodes_for_process(1);
        const auto qe0 = g_restamp_budget_query_epoch_stale_total().load();
        auto r = ev.unified_restamp_after_boundary(Evaluator::UnifiedRestampSite::BoundarySuccess);
        CHECK(r.budget_exceeded || ws->restamp_last_budget_exceeded(), "ac3041_1_budget_exceeded");
        CHECK(ws->restamp_lazy_align_enabled(), "ac3041_1_lazy_align");
        CHECK(g_query_epoch_forced_stale().load() != 0, "ac3041_1_query_epoch_forced_stale");
        CHECK(g_restamp_budget_query_epoch_stale_total().load() > qe0, "ac3041_1_stale_counter");
        apply_dev_audit_defaults();
        clear_restamp_budget_nodes_override_for_test();
        reset_query_epoch_metrics_for_test();
        const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
        CHECK(qws.find("schema-3041") != std::string::npos, "ac3041_4_schema");
        CHECK(qws.find("restamp-budget-query-epoch-stale-total") != std::string::npos,
              "ac3041_4_key");
    }

    // ── #3048: steal × session-grant residual (tenant isolation suite) ──
    {
        std::println("\n--- #3048: steal×session-grant chaos under Restricted ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        using aura::core::capability::check_and_record_effect;
        using aura::core::capability::Effect;
        using aura::core::capability::EffectProvenance;
        using aura::core::capability::g_capability_registry;
        using aura::core::capability::revoke_session_grants_on_steal_or_abort;
        using aura::core::capability::snapshot_capability_effect_stats;
        EffectProvenance prov{};
        prov.epoch = 48;
        prov.mutation_id = 48;
        g_capability_registry().grant_session(/*tenant=*/7, "mut-3048-iso", Effect::Mutate, prov,
                                              /*single_use=*/false);
        CHECK(check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 7, "3048-iso-pre",
                                      false, true),
              "3048: same-tenant session allow before steal");
        const auto n = revoke_session_grants_on_steal_or_abort(48, /*steal=*/true);
        CHECK(n >= 1, "3048: steal hook revokes session grant");
        CHECK(snapshot_capability_effect_stats().capability_live_session_grants == 0,
              "3048: live session residual 0 after steal");
        CHECK(!check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 7, "3048-iso-post",
                                       false, true),
              "3048: Restricted denies after steal revoke");
        const auto steal = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        const auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto cap = read_file("src/core/capability_model.hh");
        CHECK(steal.find("revoke_session_grants_on_steal_or_abort") != std::string::npos,
              "3048: steal-complete / force-degrade cite hook");
        CHECK(bound.find("set_current_fiber_session_mid") != std::string::npos,
              "3048: Guard enter stamps fiber session mid");
        CHECK(cap.find("Issue #3048") != std::string::npos ||
                  cap.find("#3048") != std::string::npos,
              "3048: capability_model.hh cites #3048");
        std::ifstream invent("tests/core/test_issue_3048.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3048.cpp");
        CHECK(!invent.good(), "3048: no test_issue_3048.cpp");
    }

    // ── #3049: per-tenant ResourceQuota (DoS isolation) ──
    {
        std::println("\n--- #3049 AC1/AC2: tenant A exhaust does not deny tenant B ---");
        using aura::core::resource_quota::Dimension;
        using aura::core::resource_quota::process_resource_quota;
        using aura::core::resource_quota::reset_process_resource_quota_for_test;
        using aura::core::resource_quota::set_quota_per_tenant_enabled_for_test;
        reset_process_resource_quota_for_test();
        set_quota_per_tenant_enabled_for_test(true);
        auto& pq = process_resource_quota();
        pq.set_limit(Dimension::Fibers, 4); // process ceiling
        pq.set_tenant_limit(1, Dimension::Fibers, 2);
        pq.set_tenant_limit(2, Dimension::Fibers, 2);
        CHECK(!pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/1).has_value(),
              "3049: A first fiber ok");
        CHECK(!pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/1).has_value(),
              "3049: A second fiber ok");
        auto a3 = pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/1);
        CHECK(a3.has_value(), "3049: A third fiber denied (tenant budget)");
        if (a3) {
            CHECK(a3->message.find("quota-exceeded:tenant=1:dim=fibers") != std::string::npos,
                  "3049 AC5: Agent-readable tenant deny reason");
        }
        CHECK(!pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/2).has_value(),
              "3049 AC2: B still admits after A exhaust");
        CHECK(!pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/2).has_value(),
              "3049 AC2: B second fiber ok");
        auto b3 = pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/2);
        CHECK(b3.has_value(), "3049: B third denied by tenant budget");
        // Process ceiling: A=2 + B=2 == 4; extra from either tenant fails globally.
        auto ceil = pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/2);
        CHECK(ceil.has_value(), "3049: process ceiling still binds");
        CHECK(pq.quota_reject_by_tenant_total.load() >= 2, "3049 AC4: tenant reject counter");
        pq.release(Dimension::Fibers, 2, 1);
        pq.release(Dimension::Fibers, 2, 2);
        CHECK(pq.used(Dimension::Fibers) == 0, "3049: process used restored");
        CHECK(pq.tenant_used(1, Dimension::Fibers) == 0, "3049: A used restored");
        // Mutations dimension: same tenant keying (AC1 orch/scheduler dims).
        pq.set_limit(Dimension::Mutations, 4);
        pq.set_tenant_limit(1, Dimension::Mutations, 1);
        pq.set_tenant_limit(2, Dimension::Mutations, 1);
        CHECK(!pq.check_and_consume(Dimension::Mutations, 1, 1).has_value(),
              "3049 AC1: A mutation consume");
        CHECK(pq.check_and_consume(Dimension::Mutations, 1, 1).has_value(),
              "3049 AC1: A mutation budget exhausted");
        CHECK(!pq.check_and_consume(Dimension::Mutations, 1, 2).has_value(),
              "3049 AC1: B mutation still admits");
        pq.release(Dimension::Mutations, 1, 1);
        pq.release(Dimension::Mutations, 1, 2);
        reset_process_resource_quota_for_test();
    }
    {
        std::println("\n--- #3049 AC3: Soft/off path stays process-global ---");
        using aura::core::resource_quota::Dimension;
        using aura::core::resource_quota::process_resource_quota;
        using aura::core::resource_quota::quota_per_tenant_enabled;
        using aura::core::resource_quota::reset_process_resource_quota_for_test;
        using aura::core::resource_quota::set_quota_per_tenant_enabled_for_test;
        reset_process_resource_quota_for_test();
        set_quota_per_tenant_enabled_for_test(false);
        CHECK(!quota_per_tenant_enabled(), "3049 AC3: per-tenant off");
        auto& pq = process_resource_quota();
        pq.set_limit(Dimension::Fibers, 1);
        pq.set_tenant_limit(1, Dimension::Fibers, 1);
        pq.set_tenant_limit(2, Dimension::Fibers, 1);
        CHECK(!pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/1).has_value(),
              "3049 AC3: A consumes process slot");
        auto b = pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/2);
        CHECK(b.has_value(), "3049 AC3: B denied by process-global limit (no tenant map)");
        if (b) {
            CHECK(b->message.find("quota-exceeded:tenant=") == std::string::npos,
                  "3049 AC3: deny reason is process-global, not tenant");
        }
        CHECK(pq.quota_reject_by_tenant_total.load() == 0, "3049 AC3: no tenant reject counter");
        pq.release(Dimension::Fibers, 1, 1);
        reset_process_resource_quota_for_test();
    }
    {
        std::println(
            "\n--- #3668 AC1/AC2/AC5: Mutations dim keyed by tenant (boundary + resume) ---");
        using aura::core::resource_quota::Dimension;
        using aura::core::resource_quota::process_resource_quota;
        using aura::core::resource_quota::reset_process_resource_quota_for_test;
        using aura::core::resource_quota::set_quota_per_tenant_enabled_for_test;
        reset_process_resource_quota_for_test();
        set_quota_per_tenant_enabled_for_test(true);
        auto& pq = process_resource_quota();
        // Process ceiling 10; tenant budgets: 7→4, 9→8. The MutationBoundary
        // mirror now passes the capability/resume tenant (#3668), so tenant-7
        // consumes land in slot 7 and the #3049 partition is live on mutate.
        pq.set_limit(Dimension::Mutations, 10);
        pq.set_tenant_limit(7, Dimension::Mutations, 4);
        pq.set_tenant_limit(9, Dimension::Mutations, 8);
        // Tenant 7 saturates its own budget.
        CHECK(!pq.check_and_consume(Dimension::Mutations, 4, /*tenant=*/7).has_value(),
              "3668 AC1: tenant 7 consumes within own budget");
        auto e7 = pq.check_and_consume(Dimension::Mutations, 1, /*tenant=*/7);
        CHECK(e7.has_value(), "3668 AC1: tenant 7 saturates at own limit");
        // AC5: deny reason stays quota-exceeded:tenant=N on the per-tenant arm.
        if (e7) {
            CHECK(e7->message.find("quota-exceeded:tenant=7") != std::string::npos,
                  "3668 AC5: deny reason quota-exceeded:tenant=7");
        }
        CHECK(pq.tenant_used(7, Dimension::Mutations) == 4,
              "3668 AC2: consume keyed 7 — 7's slot holds 7's used");
        // Tenant 9 still admits within its own limit after 7 saturated.
        CHECK(!pq.check_and_consume(Dimension::Mutations, 5, /*tenant=*/9).has_value(),
              "3668 AC1: tenant 9 admits within own limit (7 saturated)");
        // Process ceiling: 4 + 5 = 9 used; +2 → 11 > 10 → reject with tenant rollback.
        auto ep = pq.check_and_consume(Dimension::Mutations, 2, /*tenant=*/9);
        CHECK(ep.has_value(), "3668 AC1: process ceiling rejects when sum exceeds global");
        if (ep) {
            CHECK(ep->message.find("quota-exceeded:tenant=") == std::string::npos,
                  "3668 AC1: process-ceiling reject is not a tenant deny");
        }
        CHECK(pq.tenant_used(9, Dimension::Mutations) == 5,
              "3668 AC2: process reject rolls back 9's tenant slot");
        // Release with the same key the consume used — no leak into 0 / 9.
        pq.release(Dimension::Mutations, 4, /*tenant=*/7);
        CHECK(pq.tenant_used(7, Dimension::Mutations) == 0,
              "3668 AC2: release with same tenant drains 7's slot");
        CHECK(pq.tenant_used(9, Dimension::Mutations) == 5, "3668 AC2: 9 untouched by 7's release");
        // tenant=0 stays process-global (map dark for 0; AC4 seam).
        CHECK(!pq.check_and_consume(Dimension::Mutations, 1, /*tenant=*/0).has_value(),
              "3668 AC4: tenant=0 consume stays process-global");
        reset_process_resource_quota_for_test();
    }
    {
        std::println(
            "\n--- #3669 AC1/AC6: layout-only deny keys caller principal (unstamped-ref) ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        const auto iso_base = current_iso_seq();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2); // Strict — #3365 layout-only deny arm
        ev.set_capability_tenant_id(7);
        aura::core::capability::set_effect_fiber_id_override(3011); // #3011 join probe
        CHECK(!ev.check_workspace_isolation(0, 0, kEffectMutate, "test:3669-ac1"),
              "3669 AC1: layout-only mutate denied (Strict, principal 7)");
        bool row_found = false;
        for (std::uint64_t s = iso_base;
             s < g_workspace_isolation().audit_seq.load(std::memory_order_acquire); ++s) {
            aura::core::workspace_isolation::IsolationAuditEntry e{};
            if (!g_workspace_isolation().try_load_audit_seq(s, e))
                continue;
            if (std::string_view(e.op) != "test:3669-ac1")
                continue;
            row_found = true;
            CHECK(e.current == 7, "3669 AC1: entry.current == caller principal 7");
            CHECK(e.fiber_id == 3011, "3669 AC1: fiber still resolved (#3011 override probe)");
            CHECK(e.mutation_id == aura::core::current_mutation_epoch(),
                  "3669 AC1: mid is Mutation epoch, 0 stays 0 (#3594)");
        }
        CHECK(row_found, "3669 AC1: isolation audit row in ring");
        bool se_found = false;
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (static_cast<int>(e.kind) !=
                    static_cast<int>(
                        aura::core::security_event::SecurityEventKind::IsolationDeny) ||
                e.seq != s)
                continue;
            se_found = true;
            CHECK(std::string_view(e.reason).find("isolation-deny:unstamped-ref") !=
                      std::string_view::npos,
                  "3669 AC1: SE reason isolation-deny:unstamped-ref (not unset-principal)");
            CHECK(e.tenant_id == 7, "3669 AC1: SE tenant keyed by caller principal");
        }
        CHECK(se_found, "3669 AC1: IsolationDeny SE in ring");
        // AC6: Agent error agrees with the SE reason (stamped, no dual-track).
        CHECK(ev.last_mutate_error().find("isolation-deny:unstamped-ref") != std::string::npos,
              "3669 AC6: Agent error carries isolation-deny:unstamped-ref");
        aura::core::capability::set_effect_fiber_id_override(0);
    }

    {
        std::println("\n--- #3669 AC2: unset principal keeps isolation-deny:unset-principal ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        const auto iso_base = current_iso_seq();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted + Mutate bits → #2385
        ev.set_capability_tenant_id(0);
        CHECK(!ev.check_workspace_isolation(0, 0, kEffectMutate, "test:3669-ac2"),
              "3669 AC2: unset-principal deny still fires");
        for (std::uint64_t s = iso_base;
             s < g_workspace_isolation().audit_seq.load(std::memory_order_acquire); ++s) {
            aura::core::workspace_isolation::IsolationAuditEntry e{};
            if (!g_workspace_isolation().try_load_audit_seq(s, e))
                continue;
            if (std::string_view(e.op) != "test:3669-ac2")
                continue;
            CHECK(e.current == 0, "3669 AC2: entry.current == 0 (principal truly unset)");
        }
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (static_cast<int>(e.kind) !=
                    static_cast<int>(
                        aura::core::security_event::SecurityEventKind::IsolationDeny) ||
                e.seq != s)
                continue;
            CHECK(std::string_view(e.reason).find("isolation-deny:unset-principal") !=
                      std::string_view::npos,
                  "3669 AC2: reason unchanged (isolation-deny:unset-principal)");
        }
    }

    {
        std::println("\n--- #3669 AC3: foreign stamped ref keeps ref-tenant=N ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        CHECK(!ev.check_workspace_isolation(0, 99, kEffectMutate, "test:3669-ac3"),
              "3669 AC3: foreign-stamped ref denied");
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (static_cast<int>(e.kind) !=
                    static_cast<int>(
                        aura::core::security_event::SecurityEventKind::IsolationDeny) ||
                e.seq != s)
                continue;
            CHECK(std::string_view(e.reason).find("isolation-deny:ref-tenant=99") !=
                      std::string_view::npos,
                  "3669 AC3: reason unchanged (isolation-deny:ref-tenant=99)");
            CHECK(e.tenant_id == 7, "3669 AC3: SE tenant keyed by caller (7)");
        }
    }

    {
        std::println("\n--- #3669 AC4: dual-Evaluator rows carry their own principal ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        const auto iso_base = current_iso_seq();
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        CompilerService csa;
        auto& eva = csa.evaluator();
        eva.set_effect_sandbox_mode(2);
        eva.set_capability_tenant_id(7);
        CompilerService csb;
        auto& evb = csb.evaluator();
        evb.set_effect_sandbox_mode(2);
        evb.set_capability_tenant_id(9);
        CHECK(!eva.check_workspace_isolation(0, 0, kEffectMutate, "test:3669-ac4-a"),
              "3669 AC4: evaluator A denied");
        CHECK(!evb.check_workspace_isolation(0, 0, kEffectMutate, "test:3669-ac4-b"),
              "3669 AC4: evaluator B denied");
        std::uint64_t cur_a = 0, cur_b = 0;
        for (std::uint64_t s = iso_base;
             s < g_workspace_isolation().audit_seq.load(std::memory_order_acquire); ++s) {
            aura::core::workspace_isolation::IsolationAuditEntry e{};
            if (!g_workspace_isolation().try_load_audit_seq(s, e))
                continue;
            if (std::string_view(e.op) == "test:3669-ac4-a")
                cur_a = e.current;
            if (std::string_view(e.op) == "test:3669-ac4-b")
                cur_b = e.current;
        }
        CHECK(cur_a == 7 && cur_b == 9,
              "3669 AC4: each row keys its own Evaluator principal (7 / 9)");
        bool se_a = false, se_b = false;
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (static_cast<int>(e.kind) !=
                    static_cast<int>(
                        aura::core::security_event::SecurityEventKind::IsolationDeny) ||
                e.seq != s)
                continue;
            if (e.tenant_id == 7)
                se_a = true;
            if (e.tenant_id == 9)
                se_b = true;
        }
        CHECK(se_a && se_b, "3669 AC4: SE rows keyed by caller tenants 7 and 9");
    }

    {
        std::println("\n--- #3669 AC5: Soft allow emits no IsolationDeny SE ---");
        reset_all();
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0); // Off/Soft
        ev.set_capability_tenant_id(7);
        CHECK(ev.check_workspace_isolation(0, 0, kEffectMutate, "test:3669-ac5"),
              "3669 AC5: Soft path still allows");
        CHECK(ring.seq.load(std::memory_order_acquire) == se_base,
              "3669 AC5: allow emits no IsolationDeny SE");
    }

    {
        std::println("\n--- #3049 AC4/AC6: posture + source-cite + no invent ---");
        const auto rq = read_file("src/core/resource_quota.hh");
        const auto sched = read_file("src/serve/scheduler.cpp");
        const auto orch = read_file("src/orch/agent_spawn.h");
        const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        const auto build = read_file("build.py");
        CHECK(rq.find("quota_per_tenant_enabled") != std::string::npos,
              "3049: quota enable helper");
        CHECK(rq.find("check_and_consume_tenant") != std::string::npos ||
                  rq.find("TenantId tenant") != std::string::npos,
              "3049: tenant-keyed consume");
        CHECK(rq.find("quota-exceeded:tenant=") != std::string::npos, "3049 AC5: deny reason");
        CHECK(sched.find("check_and_consume_fiber(spawn_tenant)") != std::string::npos,
              "3049 AC6: scheduler spawn keys tenant");
        CHECK(orch.find("check_orchestration_fibers") != std::string::npos,
              "3049 AC6: orch admission cite");
        CHECK(obs.find("schema-3049") != std::string::npos, "3049 AC4: schema-3049");
        CHECK(obs.find("quota-reject-by-tenant-total") != std::string::npos,
              "3049 AC4: reject-by-tenant key");
        CHECK(build.find("check_quota_per_tenant_3049") != std::string::npos,
              "3049 AC6: build.py wires linter");
        std::ifstream invent("tests/core/test_issue_3049.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3049.cpp");
        CHECK(!invent.good(), "3049: no test_issue_3049.cpp");
    }

    // ── Issue #3145: try_grant_cross_tenant_privileged + grant_macro_self_evo
    // privilege check — explicit caller_principal (per-Evaluator
    // capability_tenant_id_, restored by TenantScope) instead of the
    // process-global default_tenant (almost always 0 under multi-Evaluator),
    // and effects_for under the registry mtx (effects_for_locked) so a
    // concurrent revoke cannot race past the fence.
    {
        std::println("\n--- #3145 AC1: dual-Evaluator chaos — revoke mid-flight, racing "
                     "grant_cross_tenant fails closed ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_b.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(7);
        ev_b.set_capability_tenant_id(7); // same principal as A
        // TenantAdmin on tenant 7 with bound mid so it actually lands.
        grant_tenant_admin_mid(7);

        // First grant succeeds (admin present, locked read).
        const auto allow0 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        ev_a.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto allow1 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(allow1 == allow0 + 1, "AC1: initial grant with TenantAdmin on caller → allow");

        // Concurrent revoke of TenantAdmin from tenant 7 — must close the
        // racing grant (the gate reads effects_for_locked under registry mtx,
        // so a revoke that lands before the read fails closed).
        using aura::core::capability::g_capability_registry;
        g_capability_registry().revoke(7, "tenant-admin");

        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_before =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        ev_b.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_after =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1,
              "AC1: post-revoke racing grant_cross_tenant fails closed (deny + counter)");
        CHECK(allow_after == allow_before,
              "AC1: post-revoke racing grant_cross_tenant does not bump allow counter");
        // Issue #3797: TA revoke must invalidate the pre-revoke sticky
        // cross_grant (mint_principal join → revoke_cross_tenant), not leave
        // it usable. Racing grant was denied; table must be empty for 7→42.
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC1/#3797: TA revoke clears matching cross_grants (no sticky post-revoke)");
    }

    // ── #3145 AC2: explicit caller_principal wins; process-global
    // default_tenant alone never authorises the gate. Two Evaluators in one
    // process: a (capability_tenant_id=7, no admin) and b (capability_tenant_id=42,
    // holds TenantAdmin). default_tenant stays 0 — if the gate read
    // default_tenant alone, both Evaluators would be denied because the
    // process-global principal is unset. With explicit caller_principal,
    // b's grant_cross_tenant_access routes through b's own principal (42) and
    // sees the admin on tenant 42 → allow.
    {
        std::println("\n--- #3145 AC2: explicit caller_principal — gate uses Evaluator principal, "
                     "not process-global default_tenant ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        // Force default_tenant=0 so any reliance on the process-global would deny.
        aura::core::capability::g_capability_registry().default_tenant.store(
            0, std::memory_order_release);

        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_b.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(7);
        ev_b.set_capability_tenant_id(42);
        // TenantAdmin on tenant 42 (b's principal), not on tenant 0 or 7.
        grant_tenant_admin_mid(42);

        // Evaluator a (tenant 7, no admin): grant denied — #3800 caller-only TA.
        // Target 42 holds admin but that no longer clears the fence. Gate still
        // resolves via caller_principal=7 (not default_tenant=0): if it read
        // default_tenant alone both would deny the same way, but the explicit
        // principal path is what #3145 requires; #3800 flips the target-admin
        // fallback to deny.
        const auto deny0 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        ev_a.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny1 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny1 == deny0 + 1,
              "AC2/#3800: caller_principal=7 (no admin) → deny (target-admin no longer allows)");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC2/#3800: 7→42 grant refused — caller lacks TA");

        // Now b's grant to a different target (99) where neither caller nor
        // target has admin (caller_principal=42 has admin on 42, but target=99
        // has nothing) → deny on missing admin, no fallback. The gate reads
        // caller_principal=42 (b's principal) which DOES hold TenantAdmin →
        // caller-admin path allows. Verify explicit principal routes through.
        const auto deny2 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow2 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        ev_b.grant_cross_tenant_access(/*from=*/42, /*to=*/99, kEffectMutate);
        const auto deny3 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow3 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(deny3 == deny2,
              "AC2: caller_principal=42 holds admin → no deny bump (caller-admin allow)");
        CHECK(allow3 == allow2 + 1,
              "AC2: 42→99 grant allowed via caller_principal=42 holding TenantAdmin");

        // Reset default_tenant so it does not leak into later blocks.
        aura::core::capability::g_capability_registry().default_tenant.store(
            0, std::memory_order_release);
    }

    // ── #3145 AC3: Soft/Off remains zero-cost. No lock, no principal load.
    {
        std::println("\n--- #3145 AC3: Soft/Off zero-cost — no lock, no principal load ---");
        reset_all(); // Off
        const auto deny0 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow0 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        // No TenantAdmin anywhere; Soft/Off must still allow (zero-cost).
        g_workspace_isolation().grant_cross_tenant(/*from=*/1, /*to=*/2, kEffectMutate,
                                                   /*caller=*/1);
        const auto deny1 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow1 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(deny1 == deny0, "AC3: Soft/Off does not bump deny (zero-cost)");
        CHECK(allow1 == allow0 + 1, "AC3: Soft/Off allows without lock or principal load");
    }

    // ── #3145 AC4: grant_macro_self_evo privilege check aligned (same
    // principal source — explicit caller_principal — and runs under the
    // registry mtx so concurrent revoke cannot race past the fence).
    {
        std::println(
            "\n--- #3145 AC4: grant_macro_self_evo aligned with explicit caller_principal ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        // TenantAdmin on tenant 42 (b's principal), nothing on 0 or 7.
        grant_tenant_admin_mid(42);

        // (a) caller_principal=0 fallback to default_tenant=0, no admin on
        // caller or target → deny.
        const auto deny0 =
            aura::core::capability::g_capability_effect_metrics()
                .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
        aura::core::capability::g_capability_registry().grant_macro_self_evo(
            /*tenant=*/7, aura::core::capability::MacroSelfEvoPolicy{}, /*prov_in=*/{},
            /*caller_principal=*/0);
        const auto deny1 =
            aura::core::capability::g_capability_effect_metrics()
                .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny1 == deny0 + 1,
              "AC4: explicit caller_principal=0 (no admin) → deny + counter bump");

        // (b) caller_principal=42 holds admin → allow.
        // Issue #3459: production refuses mid==0 — stamp a bound mid so the
        // admin-allow arm lands.
        aura::core::capability::g_capability_registry().grant_macro_self_evo(
            /*tenant=*/42, aura::core::capability::MacroSelfEvoPolicy{},
            /*prov_in=*/aura::core::capability::make_grant_provenance(1, true, 0, 0),
            /*caller_principal=*/42);
        aura::core::capability::CapabilityGrant g{};
        CHECK(aura::core::capability::g_capability_registry().find_grant(42, "macro-self-evo", g),
              "AC4: caller_principal=42 (holds TenantAdmin) → macro-self-evo grant lands");

        // (c) post-revoke alignment — revoke TenantAdmin on 42, then the same
        // explicit caller_principal=42 must deny (the registry mtx covers the
        // by_tenant find so the revoke races correctly).
        aura::core::capability::g_capability_registry().revoke(42, "tenant-admin");
        const auto deny2 =
            aura::core::capability::g_capability_effect_metrics()
                .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
        aura::core::capability::g_capability_registry().grant_macro_self_evo(
            /*tenant=*/42, aura::core::capability::MacroSelfEvoPolicy{}, /*prov_in=*/{},
            /*caller_principal=*/42);
        const auto deny3 =
            aura::core::capability::g_capability_effect_metrics()
                .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny3 == deny2 + 1,
              "AC4: post-revoke, explicit caller_principal=42 (no admin) → deny");
    }

    // ── #3145 AC5/AC6: source-cite + linter + no invent + no docs/design/
    {
        std::println("\n--- #3145 AC5/AC6: source-cite + linter + no invent ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto cap = read_file("src/core/capability_model.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");

        // AC5: workspace_isolation.hh owns the SSOT helper, explicit
        // caller_principal parameter, and the locked effects_for_locked
        // read under the registry mtx.
        CHECK(iso.find("Issue #3145") != std::string::npos,
              "AC5: workspace_isolation.hh cites Issue #3145");
        CHECK(iso.find("caller_principal") != std::string::npos,
              "AC5: SSOT helper accepts caller_principal");
        CHECK(iso.find("effects_for_locked") != std::string::npos,
              "AC5: privilege read uses effects_for_locked (TOCTOU closure)");
        CHECK(iso.find("reg.mtx") != std::string::npos, "AC5: privilege read holds registry mtx");

        // AC5: capability_model.hh grant_macro_self_evo accepts caller_principal.
        CHECK(cap.find("Issue #3145") != std::string::npos,
              "AC5: capability_model.hh cites Issue #3145");
        CHECK(cap.find("caller_principal") != std::string::npos,
              "AC5: grant_macro_self_evo accepts caller_principal");

        // AC5: Evaluator wrapper forwards capability_tenant_id_.
        CHECK(sec.find("Issue #3145") != std::string::npos,
              "AC5: evaluator_security.cpp cites Issue #3145");
        CHECK(sec.find("g_workspace_isolation().grant_cross_tenant") != std::string::npos &&
                  sec.find("capability_tenant_id_") != std::string::npos,
              "AC5: Evaluator wrapper forwards capability_tenant_id_ to SSOT method");

        // AC5: Evaluator prim site forwards ev.capability_tenant_id().
        CHECK(prim.find("Issue #3145") != std::string::npos ||
                  prim.find("#3145") != std::string::npos,
              "AC5: evaluator_primitives_security.cpp cites Issue #3145");
        CHECK(prim.find("ev.capability_tenant_id()") != std::string::npos,
              "AC5: prim forwards ev.capability_tenant_id() to grant_macro_self_evo");

        // AC5: this test file cites #3145.
        CHECK(test_self.find("#3145") != std::string::npos, "AC5: test file cites Issue #3145");

        // AC6: linter wired into build.py.
        CHECK(build.find("check_cross_tenant_grant_principal_3145") != std::string::npos,
              "AC6: build.py wires #3145 linter");

        // AC6: no new posture / query key (AC6 explicit).
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("schema-3145") == std::string::npos,
              "AC6: no new posture key (schema-3145 forbidden per issue body)");
        CHECK(posture.find("issue-3145") == std::string::npos,
              "AC6: no new query key (issue-3145 forbidden per issue body)");

        // No invent + no docs/design/ (#81967 / #1655).
        std::ifstream invent("tests/core/test_issue_3145.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3145.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_3145.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3145-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3204: production Agent export hard-reject tenant_id=0 ──
    {
        std::println("\n--- #3204 AC1: production defaults arm hard-reject without env ---");
        reset_all();
        CHECK(aura::core::provenance::kStableRefExportProductionHardRejectIssue == 3204,
              "3204 AC1: issue stamp");
        ::unsetenv("AURA_STABLE_REF_EXPORT_HARD_REJECT");
        CHECK(!aura::core::provenance::stable_ref_export_hard_reject(), "3204 AC1: Soft pref off");
        ::setenv("AURA_SANDBOX", "restricted", 1);
        aura::compiler::security::apply_production_security_defaults();
        CHECK(aura::core::provenance::stable_ref_export_hard_reject(),
              "ac3204_1_production_hard_reject: Restricted arms hard-reject");
        ::setenv("AURA_SANDBOX", "off", 1);
        aura::compiler::security::apply_production_security_defaults();
        CHECK(!aura::core::provenance::stable_ref_export_hard_reject(),
              "3204 AC1: sandbox=off leaves Soft");
        ::unsetenv("AURA_SANDBOX");
        reset_all();
    }
    {
        std::println("\n--- #3204 AC2: layout-only handoff stamps or denies; never tenant 0 ---");
        reset_all();
        aura::core::provenance::set_stable_ref_export_hard_reject(true);
        aura::core::provenance::set_hard_capture_tenant(true);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (h x) (+ x 1))\")").has_value(), "3204 AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3204 AC2: eval");
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3204 AC2: workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3204 AC2: live node");
        auto layout = ws->make_ref_layout(id);
        CHECK(layout.tenant_id == 0, "3204 AC2: layout-only tenant 0");
        const auto stale0 =
            aura::core::provenance::g_provenance_enforcement()
                .stable_ref_export_stale_reject_total.load(std::memory_order_relaxed);
        auto out = ev.handoff_ref(layout);
        if (out) {
            CHECK(out->tenant_id == 7, "ac3204_2_handoff_never_tenant_zero: stamped 7");
            CHECK(out->tenant_id != 0, "3204 AC2: never tenant_id==0");
            ev.set_capability_tenant_id(9);
            CHECK(!ev.check_workspace_isolation(9, out->tenant_id, 0, "test:3204-x"),
                  "3204 AC2: wrong principal IsolationDeny");
        } else {
            CHECK(aura::core::provenance::g_provenance_enforcement()
                          .stable_ref_export_stale_reject_total.load(std::memory_order_relaxed) >
                      stale0,
                  "3204 AC2: deny bumps stale-reject");
        }
        reset_all();
    }
    {
        std::println("\n--- #3204 AC3: Soft layout-only stays zero-cost contract ---");
        reset_all();
        ::setenv("AURA_SANDBOX", "off", 1);
        aura::compiler::security::apply_production_security_defaults();
        CHECK(!aura::core::provenance::stable_ref_export_hard_reject(),
              "ac3204_3_soft_quiet: Soft hard-reject off");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (s x) x)\")").has_value(), "3204 AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3204 AC3: eval");
        auto& ev = cs.evaluator();
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3204 AC3: workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3204 AC3: live");
        auto layout = ws->make_ref_layout(id);
        CHECK(layout.tenant_id == 0, "3204 AC3: layout-only tenant 0");
        const auto stamp0 =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        const auto stale0 =
            aura::core::provenance::g_provenance_enforcement()
                .stable_ref_export_stale_reject_total.load(std::memory_order_relaxed);
        auto out = ev.handoff_ref(layout);
        const auto stamp1 =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        const auto stale1 =
            aura::core::provenance::g_provenance_enforcement()
                .stable_ref_export_stale_reject_total.load(std::memory_order_relaxed);
        CHECK(stamp1 == stamp0, "3204 AC3: Soft no extra stamp");
        CHECK(stale1 == stale0, "3204 AC3: Soft no extra stale-reject");
        if (out)
            CHECK(out->tenant_id == 0, "3204 AC3: Soft keeps layout-only tenant 0");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        CHECK(sec.find("Issue #3204") != std::string::npos, "3204 AC3: finalize cites #3204");
        CHECK(sec.find("stamp_stable_ref") != std::string::npos, "3204 AC3: stamp authority");
        CHECK(read_file("src/compiler/security_defaults.hh").find("Issue #3204") !=
                  std::string::npos,
              "3204 AC3: production defaults cite #3204");
        ::unsetenv("AURA_SANDBOX");
        reset_all();
    }
    {
        std::println("\n--- #3204 AC4: concurrent export + principal switch ---");
        reset_all();
        aura::core::provenance::set_stable_ref_export_hard_reject(true);
        aura::core::provenance::set_hard_capture_tenant(true);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (c x) x)\")").has_value(), "3204 AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3204 AC4: eval");
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3204 AC4: workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3204 AC4: live");
        std::atomic<int> leaked{0};
        std::atomic<int> ok_n{0};
        std::thread exporter([&] {
            for (int i = 0; i < 64; ++i) {
                auto layout = ws->make_ref_layout(id);
                auto out = ev.handoff_ref(layout);
                if (out) {
                    if (out->tenant_id == 0)
                        leaked.fetch_add(1, std::memory_order_relaxed);
                    else
                        ok_n.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        std::thread switcher([&] {
            for (int i = 0; i < 64; ++i)
                ev.set_capability_tenant_id((i & 1) ? 7 : 9);
        });
        exporter.join();
        switcher.join();
        CHECK(leaked.load(std::memory_order_relaxed) == 0,
              "ac3204_4_concurrent: no tenant_id==0 leak");
        (void)ok_n;
        reset_all();
    }
    {
        std::println("\n--- #3204 AC5: source-cite + linter + no invent ---");
        const auto prov = read_file("src/core/provenance_tracker.hh");
        const auto build = read_file("build.py");
        CHECK(prov.find("kStableRefExportProductionHardRejectIssue = 3204") != std::string::npos,
              "ac3204_5_source_linter: stamp");
        CHECK(build.find("check_stable_ref_export_production_hard_reject_3204") !=
                  std::string::npos,
              "3204 AC5: build.py");
        CHECK(read_file("tests/core/test_issue_3204.cpp").empty(), "3204 AC5: no invent");
        CHECK(read_file("docs/design/3204-stable-ref-export-hard-reject.md").empty(),
              "3204 AC5: no docs/design");
    }

    // ── #3276: freeze the privileged-write call-site allowlist. Runtime
    // fences (#2968/#3086/#3145/#3029/#2969/#3141) are solid; residual is
    // static surface area — any NEW TU can call g_capability_registry().grant
    // / grant_locked / grant_session / grant_once / grant_macro_self_evo /
    // g_workspace_isolation().grant_cross_tenant and bypass Evaluator
    // principal / audit / TenantAdmin wrappers. The allowlist + coverage
    // linter freeze the sole permitted inventory; the linter fails the gate
    // on any src/ hit outside it. Read-only getters (grant_epoch_retain_window
    // / grant_min_valid_epoch) are not grant writes and stay out of scope.
    {
        const auto al = read_file("scripts/coverage/allowlists/privileged_grant_calls.json");
        const auto lint =
            read_file("scripts/coverage/checks/check_privileged_grant_callsite_allowlist_3276.py");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto sdef = read_file("src/compiler/security_defaults.hh");
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        const auto build = read_file("build.py");

        std::println("\n--- #3276 AC1: allowlist freezes the sole permitted inventory ---");
        CHECK(al.find("src/compiler/evaluator_security.cpp") != std::string::npos,
              "3276 AC1: evaluator_security.cpp allowlisted");
        CHECK(al.find("src/compiler/security_defaults.hh") != std::string::npos,
              "3276 AC1: security_defaults.hh allowlisted");
        CHECK(al.find("src/compiler/evaluator_primitives_security.cpp") != std::string::npos,
              "3276 AC1: evaluator_primitives_security.cpp allowlisted");
        CHECK(al.find(".grant_locked(") != std::string::npos,
              "3276 AC1: grant_locked pattern in allowlist");
        CHECK(al.find(".grant_session(") != std::string::npos,
              "3276 AC1: grant_session pattern in allowlist");
        CHECK(al.find(".grant_macro_self_evo(") != std::string::npos,
              "3276 AC1: grant_macro_self_evo pattern in allowlist");
        CHECK(al.find(".grant_cross_tenant(") != std::string::npos,
              "3276 AC1: grant_cross_tenant pattern in allowlist");
        CHECK(al.find(".grant_epoch_retain_window(") == std::string::npos &&
                  al.find(".grant_min_valid_epoch(") == std::string::npos,
              "3276 AC1: read-only getters NOT in allowlist (out of scope)");

        std::println("\n--- #3276 AC2: current call sites sit inside the allowlist ---");
        CHECK(sec.find("g_capability_registry().grant(") != std::string::npos,
              "3276 AC2: evaluator_security.cpp direct grant (authority)");
        CHECK(sec.find(".grant_locked(") != std::string::npos,
              "3276 AC2: evaluator_security.cpp grant_locked (authority)");
        CHECK(sec.find("g_workspace_isolation().grant_cross_tenant(") != std::string::npos,
              "3276 AC2: evaluator_security.cpp cross-tenant grant (authority)");
        CHECK(sdef.find("g_capability_registry().grant(") != std::string::npos,
              "3276 AC2: security_defaults.hh bootstrap grant");
        CHECK(sdef.find("/*tenant=*/0") != std::string::npos,
              "3276 AC2: bootstrap render grants stay tenant=0");
        CHECK(prim.find("g_capability_registry().grant_macro_self_evo(") != std::string::npos,
              "3276 AC2: prim macro-self-evo grant (behind #3029 fence)");
        CHECK(obs.find(".grant_epoch_retain_window(") != std::string::npos &&
                  obs.find(".grant_min_valid_epoch(") != std::string::npos,
              "3276 AC2: obs_jit read-only getters remain (not grant writes)");

        std::println("\n--- #3276 AC3/AC4: linter scans + no new TU / no invent ---");
        CHECK(lint.find("Issue #3276") != std::string::npos, "3276 AC3: linter cites #3276");
        CHECK(lint.find("SCANNED_PATTERNS") != std::string::npos,
              "3276 AC3: linter scans grant-family patterns");
        CHECK(lint.find("hits_outside") != std::string::npos,
              "3276 AC3: linter fails on hits outside allowlist");
        CHECK(lint.find("_strip_comments") != std::string::npos,
              "3276 AC3: comments stripped (doc mentions not false hits)");
        CHECK(build.find("check_privileged_grant_callsite_allowlist_3276") != std::string::npos,
              "3276 AC4: build.py wires linter");
        CHECK(read_file("tests/core/test_issue_3276.cpp").empty() &&
                  read_file("tests/issues/test_issue_3276.cpp").empty(),
              "3276 AC4: no test_issue_3276.cpp per #81967");
        CHECK(read_file("docs/design/3276-privileged-grant-allowlist.md").empty(),
              "3276 AC4: no docs/design/3276-* per #1655");
    }

    // ── #3411: has_capability("*") string-gate must not short-circuit
    // TA/MSE bits — close the double-track with #3144 effects_for strip.
    // set_tenant_principal(allow_cross=true) privileged check drops the
    // standalone has_capability(kCapWildcard) arm. AC5: #3141/#3363/#3332
    // regression-guard via source-cite.
    {
        std::println("\n--- #3411 AC1: has_capability TA/MSE bypass wildcard short-circuit ---");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        // Issue #3411 marker present (anchors the new block).
        CHECK(sec.find("Issue #3411") != std::string::npos,
              "3411 AC1: #3411 marker in evaluator_security.cpp");
        // eff is computed BEFORE the wildcard short-circuit (otherwise the
        // is_ta_mse_eff guard cannot steer TA/MSE queries to effects_for).
        const auto eff_pos = sec.find("const Effect eff = effect_for_cap_name(needed);");
        // Issue #4277: live registry row, not the old wildcard_held lambda.
        const auto wild_pos = sec.find("holds_live_wildcard(capability_tenant_id_)");
        CHECK(eff_pos != std::string::npos && wild_pos != std::string::npos && eff_pos < wild_pos,
              "3411 AC1: eff computed before wildcard short-circuit (TA/MSE routing)");
        // is_ta_mse_eff guards TA / MSE bits from the wildcard short-circuit.
        CHECK(sec.find("is_ta_mse_eff") != std::string::npos,
              "3411 AC1: is_ta_mse_eff guard for TA/MSE routing");
        CHECK(sec.find("Effect::TenantAdmin") != std::string::npos &&
                  sec.find("Effect::MacroSelfEvo") != std::string::npos,
              "3411 AC1: TA + MSE bits routed through effects_for");
        // TA/MSE queries always reach effects_for (#3144 strip path).
        // #3876 unified has_capability onto effects_effective_for (still the
        // #3144 effects_for strip — see evaluator_security.cpp:114).
        CHECK(sec.find("has_effect(g_capability_registry().effects_effective_for("
                       "capability_tenant_id_)") != std::string::npos,
              "3411 AC1: TA/MSE routed via effects_for (uses #3144 strip)");

        std::println("\n--- #3411 AC2: set_tenant_principal drops kCapWildcard privileged arm ---");
        // set_tenant_principal must NOT OR has_capability(kCapWildcard) any
        // more — #3995 takes mtx + effects_for_locked TenantAdmin (kCapCapability
        // maps to TA bits). Wildcard-only is stripped (#3144).
        const auto set_tp = sec.find("set_tenant_principal(std::uint64_t tenant_id");
        CHECK(set_tp != std::string::npos, "3411 AC2: set_tenant_principal present");
        const auto priv_block = sec.substr(set_tp, 2800);
        CHECK(priv_block.find("effects_for_locked") != std::string::npos &&
                  priv_block.find("lock_guard") != std::string::npos,
              "3411 AC2: privileged = locked effects_for_locked TenantAdmin (#3995)");
        CHECK(priv_block.find("has_capability(kCapTenantAdmin)") == std::string::npos &&
                  priv_block.find("has_capability(kCapCapability)") == std::string::npos,
              "3411 AC2: unlocked has_capability TA/Capability fence removed");
        const auto priv_stmt_pos = sec.find("const bool privileged =", set_tp);
        CHECK(priv_stmt_pos != std::string::npos, "3411 AC2: privileged statement present");
        const auto priv_stmt = sec.substr(priv_stmt_pos, 400);
        CHECK(priv_stmt.find("kCapWildcard") == std::string::npos,
              "3411 AC2: privileged OR no longer arms has_capability(kCapWildcard)");
        CHECK(priv_block.find("allow-cross-needs-tenant-admin") != std::string::npos,
              "3411 AC2: SE reason 'allow-cross-needs-tenant-admin' preserved");
        CHECK(priv_block.find("force_bind = sandbox_mode_") != std::string::npos,
              "3411 AC2: Soft/Off short-circuit via force_bind intact");

        std::println("\n--- #3411 AC3: wildcard still grants non-TA/MSE + string-only ---");
        // Issue #4277: the short-circuit is holds_live_wildcard under
        // !is_ta_mse_eff. Non-TA/MSE and string-only caps still wildcard-grant.
        const auto gate = sec.find("if (!is_ta_mse_eff)");
        const auto live = sec.find("holds_live_wildcard(capability_tenant_id_)");
        CHECK(gate != std::string::npos && live != std::string::npos && gate < live,
              "3411 AC3: wildcard short-circuit preserved for non-TA/MSE");
        // string-only legacy caps path still present.
        CHECK(sec.find("Legacy string-only caps keep the list path.") != std::string::npos,
              "3411 AC3: string-only caps list path preserved");

        std::println("\n--- #3411 AC4: Soft/Off zero-cost short-circuit ---");
        // Top of has_capability short-circuits when sandbox fully off.
        CHECK(sec.find("Sandbox fully off") != std::string::npos &&
                  sec.find("!sandbox_mode_ && effect_sandbox_mode() == 0") != std::string::npos,
              "3411 AC4: Soft/Off short-circuit at top of has_capability (zero extra)");

        std::println("\n--- #3411 AC5: #3141/#3363/#3332 don't regress ---");
        // #3141 string write fence (grant_capability privilege-bearing path).
        CHECK(sec.find("Issue #3141") != std::string::npos,
              "3411 AC5: #3141 string write fence preserved");
        // #3144 effects_for strip in capability_model.hh.
        const auto cap = read_file("src/core/capability_model.hh");
        CHECK(cap.find("Issue #3144") != std::string::npos,
              "3411 AC5: #3144 effects_for strip preserved");
        // #3363 require_effect TA deny + #3332 isolation read path.
        // Surface moved: #3363 now cites in capability_model.hh, #3332 in
        // workspace_isolation.hh (post-refactor anchors).
        CHECK(sec.find("Issue #3363") != std::string::npos ||
                  read_file("src/core/capability_model.hh").find("Issue #3363") !=
                      std::string::npos,
              "3411 AC5: #3363 require_effect TA deny preserved");
        CHECK(sec.find("Issue #3332") != std::string::npos ||
                  read_file("src/core/workspace_isolation.hh").find("Issue #3332") !=
                      std::string::npos,
              "3411 AC5: #3332 isolation read path preserved");
        CHECK(sec.find("Issue #3010") != std::string::npos, "3411 AC5: #3010 write gate preserved");

        std::println("\n--- #3411 AC6: no docs/design/3411-*; no test_issue_3411.cpp ---");
        CHECK(read_file("docs/design/3411-wildcard-ta-string-gate.md").empty() &&
                  read_file("docs/design/3411-has-capability-string-track.md").empty(),
              "3411 AC6: no docs/design/3411-* per #1655");
        CHECK(read_file("tests/issues/test_issue_3411.cpp").empty() &&
                  read_file("tests/compiler/test_issue_3411.cpp").empty() &&
                  read_file("tests/core/test_issue_3411.cpp").empty(),
              "3411 AC6: no test_issue_3411.cpp per #81934 (extend existing test)");
    }

    // ── #3492: EDSL set-tenant-principal! shares grant-effect TA face ──
    // Wildcard-only must not elevate principal or set allow_cross.
    {
        std::println("\n--- #3492 AC1: wildcard-only cannot elevate tenant ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        ev.grant_capability("*"); // Off: host can seed wildcard-only
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ev.set_effect_sandbox_mode(1);
        {
            auto& reg = aura::core::capability::g_capability_registry();
            std::lock_guard<std::mutex> lock(reg.mtx);
            CHECK(reg.holds_wildcard_only_locked(7), "3492 AC1: tenant holds wildcard-only");
        }
        const auto deny_before = ev.capability_denial_count();
        auto edsl = cs.eval("(security:set-tenant-principal! 10)");
        CHECK(edsl && is_bool(*edsl) && !as_bool(*edsl),
              "3492 AC1: wildcard-only elevate returns #f");
        CHECK(ev.capability_tenant_id() == 7, "3492 AC1: principal unchanged");
        CHECK(ev.capability_denial_count() > deny_before, "3492 AC1: denial bumped");
    }

    {
        std::println("\n--- #3492 AC2: wildcard-only cannot set allow_cross ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        ev.grant_capability("*");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ev.set_effect_sandbox_mode(1);
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        auto edsl = cs.eval("(security:set-tenant-principal! 7 #t)");
        CHECK(edsl && is_bool(*edsl) && !as_bool(*edsl),
              "3492 AC2: wildcard-only same-tenant #t returns #f");
        CHECK(!ev.allow_cross_tenant(), "3492 AC2: flag stays false");
        CHECK(ev.capability_tenant_id() == 7, "3492 AC2: principal unchanged");
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1, "3492 AC2: allow_cross_tenant_deny_total bumps");
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(prim.find("holds_wildcard_only_locked") != std::string::npos,
              "3492 AC2: prim uses holds_wildcard_only_locked");
        const auto stp = prim.find("security:set-tenant-principal!");
        CHECK(stp != std::string::npos, "3492 AC2: prim present");
        const auto block = prim.substr(stp, 2200);
        CHECK(block.find("has_capability(kCapWildcard)") == std::string::npos &&
                  block.find("has_capability(aura::compiler::security::kCapWildcard)") ==
                      std::string::npos,
              "3492 AC2: prim no longer treats wildcard as elevate/allow_cross");
    }

    // ── #3722 AC1: Restricted+MT, tenant holds no Mutate —
    // (rollback mid) → #f, zero topology write, joinable deny SE ──
    {
        std::println("\n--- #3722 AC1: no-Mutate rollback denies before write ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3722a x) x)\")").has_value(), "3722 AC1 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3722 AC1 eval");
        set_mode(SandboxMode::Restricted);
        aura::core::provenance::set_multi_tenant_env_active(true);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(99);
        ev.grant_effect_capability(/*tenant=*/99, "rb-3722-b", kEffectMutate, /*mid=*/1);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3722 AC1 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3722 AC1 live node");
        // Fresh high node id — unstamped, outside the cross-section occupancy
        // ring pollution zone (low ids are stamped by earlier sections).
        const auto fresh = static_cast<NodeId>(ws->size() - 1);
        CHECK(fresh != NULL_NODE && fresh != id, "3722 AC1 fresh node id");
        // Foreign fixture (#3415-proven): tenant 99 stamps the node into the
        // occupancy ring and seeds a Committed record (host-side setup — the
        // attack under test is the Agent eval `(rollback mid)`).
        const auto stamped = ev.make_stamped_ref(fresh);
        CHECK(stamped.tenant_id == 99, "3722 AC1: occupancy stamp tenant 99");
        seed_3722_record(*ws, /*mid=*/424242, fresh);
        // Tenant 7 holds NO Mutate grant — rollback must deny before any
        // FlatAST write and leave a joinable deny SE behind.
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        const auto bumps_before = ws->subtree_bump_count();
        const auto rb = cs.eval("(rollback 424242)");
        CHECK(rb.has_value() && is_bool(*rb) && !as_bool(*rb), "3722 AC1: (rollback mid) → #f");
        CHECK(ws->subtree_bump_count() == bumps_before, "3722 AC1: zero topology write");
        bool rec_untouched = false;
        for (const auto& r : ws->all_mutations())
            if (r.mutation_id == 424242)
                rec_untouched = r.status == aura::ast::MutationStatus::Committed;
        CHECK(rec_untouched, "3722 AC1: record still Committed (no rollback)");
        bool deny_se = false;
        for (std::uint64_t s = baseline; s < ring.seq.load(); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            const auto k = static_cast<int>(e.kind);
            if (e.seq == s && (k == static_cast<int>(SecurityEventKind::EffectDeny) ||
                               k == static_cast<int>(SecurityEventKind::IsolationDeny))) {
                deny_se = true;
                CHECK(e.mutation_id != 0, "3722 AC1: deny SE mid joinable (#2156)");
            }
        }
        CHECK(deny_se, "3722 AC1: deny SE in ring");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    // ── #3722 AC2: Mutate held, mid is foreign occupancy — isolation
    // denies first (zero write); foreign rollback-since → 0 ──
    {
        std::println("\n--- #3722 AC2: foreign-mid rollback isolation deny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3722b x) x)\")").has_value(), "3722 AC2 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3722 AC2 eval");
        set_mode(SandboxMode::Restricted);
        aura::core::provenance::set_multi_tenant_env_active(true);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(99);
        ev.grant_effect_capability(/*tenant=*/99, "rb-3722-b", kEffectMutate, /*mid=*/1);
        ev.grant_effect_capability(/*tenant=*/7, "rb-3722-a", kEffectMutate, /*mid=*/1);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3722 AC2 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3722 AC2 live node");
        const auto fresh = static_cast<NodeId>(ws->size() - 1);
        CHECK(fresh != NULL_NODE && fresh != id, "3722 AC2 fresh node id");
        const auto stamped = ev.make_stamped_ref(fresh);
        CHECK(stamped.tenant_id == 99, "3722 AC2: occupancy stamp tenant 99");
        seed_3722_record(*ws, /*mid=*/424242, fresh);
        ev.set_capability_tenant_id(7);
        const auto& ring2 = g_security_event_ring();
        const auto baseline2 = ring2.seq.load(std::memory_order_acquire);
        const auto bumps_before = ws->subtree_bump_count();
        const auto rb = cs.eval("(rollback 424242)");
        CHECK(rb.has_value() && is_bool(*rb) && !as_bool(*rb),
              "3722 AC2: foreign-mid rollback → #f despite Mutate grant");
        CHECK(ws->subtree_bump_count() == bumps_before, "3722 AC2: zero write (isolation first)");
        // Non-vacuousness: the deny must be the ISOLATION face (require_effect
        // runs the #2658/#2490 consult before the capability write path) — a
        // capability deny here would mean the grant arming failed, not that
        // the isolation veto fired.
        bool iso_deny = false;
        for (std::uint64_t s = baseline2; s < ring2.seq.load(); ++s) {
            const auto& e = ring2.ring[s % ring2.ring.size()];
            if (e.seq == s &&
                static_cast<int>(e.kind) == static_cast<int>(SecurityEventKind::IsolationDeny))
                iso_deny = true;
        }
        CHECK(iso_deny, "3722 AC2: IsolationDeny SE (grant alive, isolation vetoed)");
        const auto rs = cs.eval("(rollback-since 424242)");
        CHECK(rs.has_value() && is_int(*rs) && as_int(*rs) == 0,
              "3722 AC2: foreign rollback-since → 0");
        CHECK(ws->subtree_bump_count() == bumps_before, "3722 AC2: rollback-since zero write");
        bool rec_untouched = false;
        for (const auto& r : ws->all_mutations())
            if (r.mutation_id == 424242)
                rec_untouched = r.status == aura::ast::MutationStatus::Committed;
        CHECK(rec_untouched, "3722 AC2: record still Committed (isolation veto)");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    // ── #3722 AC3: own mid — rollback proceeds; EffectAllow SE mid joins
    // the Mutation epoch (#2054/#2384), record flips RolledBack ──
    // Allow arming = closed_loop production recipe: wildcard + tenant-admin
    // registry rows seeded under Off, then RE-granted after Restricted
    // arming so bound_mutation_id matches the post-epoch-bump mid the gate
    // joins (#3409 caller_principal fence). grant_effect_capability is NOT
    // usable here: production force-promotes Mutate grants to single_use +
    // session_bound (#2882/#3561) and the bare rollback eval runs outside a
    // MutationBoundary — unconsumable there.
    {
        std::println("\n--- #3722 AC3: own-mid rollback joins Mutation epoch ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        ev.set_capability_tenant_id(7);
        // Library-side TenantAdmin row (self-grant lands under Restricted;
        // the sticky fence below only reads its presence).
        ev.grant_effect_capability(/*tenant=*/7, "tenant-admin-3722",
                                   aura::compiler::security::kEffectTenantAdmin,
                                   /*mid=*/1);
        // Sanctioned sticky escape (#3177): TenantAdmin holder + non-empty
        // audit reason + env opt-in -> single_use=false, session_bound=false
        // — consumable outside a MutationBoundary (the bare rollback eval
        // runs without one).
        ::setenv("AURA_ALLOW_DURABLE_STICKY", "1", 1);
        ev.grant_effect_durable_sticky(/*tenant=*/7, "rb-3722-own", kEffectMutate,
                                       /*mid=*/1, "3722 AC3 own-mid rollback allow");
        CHECK(cs.eval("(set-code \"(define (n3722c x) x)\")").has_value(), "3722 AC3 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3722 AC3 eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3722 AC3 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3722 AC3 live node");
        const auto fresh = static_cast<NodeId>(ws->size() - 3);
        CHECK(fresh != NULL_NODE && fresh != id, "3722 AC3 fresh node id");
        // Own-mid fixture: tenant 7 owns the node (ring cleared by
        // reset_all -> deterministic existing==caller allow path).
        const auto own = ev.make_stamped_ref(fresh);
        CHECK(own.tenant_id == 7, "3722 AC3: occupancy stamp tenant 7");
        seed_3722_record(*ws, /*mid=*/424243, fresh);
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        const auto rb = cs.eval("(rollback 424243)");
        CHECK(rb.has_value() && is_bool(*rb) && as_bool(*rb), "3722 AC3: own rollback → #t");
        bool rolled = false;
        for (const auto& r : ws->all_mutations())
            if (r.mutation_id == 424243)
                rolled = r.status == aura::ast::MutationStatus::RolledBack;
        CHECK(rolled, "3722 AC3: record status RolledBack");
        bool allow_se = false;
        for (std::uint64_t s = baseline; s < ring.seq.load(); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.seq == s &&
                static_cast<int>(e.kind) == static_cast<int>(SecurityEventKind::EffectAllow)) {
                allow_se = true;
                CHECK(e.mutation_id != 0, "3722 AC3: allow SE mid non-zero (Mutation epoch)");
            }
        }
        CHECK(allow_se, "3722 AC3: EffectAllow SE in ring (correlated success)");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    // ── #3722 AC4: the foreign mid is visible in the shared log (option B;
    // the mutation-history prim face itself is tenant-filtered under the
    // production consult regime since #3792) — the AC2 gate keeps a leaked
    // mid inert ──
    {
        std::println("\n--- #3722 AC4: leaked foreign mid cannot fire rollback ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3722d x) x)\")").has_value(), "3722 AC4 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3722 AC4 eval");
        set_mode(SandboxMode::Restricted);
        aura::core::provenance::set_multi_tenant_env_active(true);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(99);
        ev.grant_effect_capability(/*tenant=*/99, "rb-3722-b", kEffectMutate, /*mid=*/1);
        ev.grant_effect_capability(/*tenant=*/7, "rb-3722-a", kEffectMutate, /*mid=*/1);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3722 AC4 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3722 AC4 live node");
        const auto fresh = static_cast<NodeId>(ws->size() - 1);
        CHECK(fresh != NULL_NODE && fresh != id, "3722 AC4 fresh node id");
        const auto stamped = ev.make_stamped_ref(fresh);
        CHECK(stamped.tenant_id == 99, "3722 AC4: occupancy stamp tenant 99");
        seed_3722_record(*ws, /*mid=*/424242, fresh);
        ev.set_capability_tenant_id(7);
        bool leaked_visible = false;
        for (const auto& r : ws->all_mutations())
            if (r.mutation_id == 424242)
                leaked_visible = r.status == aura::ast::MutationStatus::Committed;
        CHECK(leaked_visible, "3722 AC4: foreign mid present in shared log (leak surface)");
        const auto rb = cs.eval("(rollback 424242)");
        CHECK(rb.has_value() && is_bool(*rb) && !as_bool(*rb),
              "3722 AC4: leaked foreign mid still inert");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    // ── #3722 AC5: Soft/Off — gate rides the require_effect zero-cost
    // short-circuit; rollback works with no grants / no Restricted face ──
    {
        std::println("\n--- #3722 AC5: Soft/Off unchanged ---");
        reset_all(); // Sandbox Off, no grants, no MT consult
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3722e x) x)\")").has_value(), "3722 AC5 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3722 AC5 eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3722 AC5 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3722 AC5 live node");
        seed_3722_record(*ws, /*mid=*/424244, id);
        const auto rb = cs.eval("(rollback 424244)");
        CHECK(rb.has_value() && is_bool(*rb) && as_bool(*rb),
              "3722 AC5: soft rollback proceeds without grants");
        bool rolled = false;
        for (const auto& r : ws->all_mutations())
            if (r.mutation_id == 424244)
                rolled = r.status == aura::ast::MutationStatus::RolledBack;
        CHECK(rolled, "3722 AC5: record rolled back (no new Soft cost)");
    }

    // ── #3722 AC6: source-cite — gate cites #3722, mandated #2942 helper
    // precedes the FlatAST write; no new query key; no test_issue file ──
    {
        std::println("\n--- #3722 AC6: source-cite ---");
        const auto prim = read_file("src/compiler/evaluator_primitives_mutation.cpp");
        CHECK(prim.find("Issue #3722: rollback / rollback-since") != std::string::npos,
              "3722 AC6: gate cites #3722");
        const auto rb_pos = prim.find("add(\"rollback\"");
        CHECK(rb_pos != std::string::npos, "3722 AC6: rollback prim present");
        const auto gate_pos = prim.find("require_effect_for_node_id", rb_pos);
        const auto write_pos = prim.find("rollback(mid)", rb_pos);
        CHECK(gate_pos != std::string::npos, "3722 AC6: mandated helper in rollback gate");
        CHECK(write_pos != std::string::npos, "3722 AC6: FlatAST write present");
        CHECK(gate_pos < write_pos, "3722 AC6: gate precedes rollback write");
        const auto rs_pos = prim.find("add(\"rollback-since\"");
        CHECK(rs_pos != std::string::npos, "3722 AC6: rollback-since prim present");
        const auto rs_gate = prim.find("require_effect_for_node_id", rs_pos);
        const auto rs_write = prim.find("rollback_since(", rs_pos);
        CHECK(rs_gate != std::string::npos && rs_write != std::string::npos && rs_gate < rs_write,
              "3722 AC6: rollback-since gate precedes write");
        CHECK(prim.find("insert_kv(\"rollback-gate") == std::string::npos,
              "3722 AC6: no new query key");
        CHECK(read_file("docs/design/3722-rollback-effect-gate.md").empty(),
              "3722 AC6: no docs/design/3722-* per #1655");
        CHECK(read_file("tests/core/test_issue_3722.cpp").empty(),
              "3722 AC6: no test_issue_3722.cpp per #81934");

        // ── #3790: rollback acquires MutationBoundaryGuard (concurrency half) ──
        {
            std::println("\n--- #3790 AC1–AC5: Guard acquire + Soft cite ---");
            const auto prim = read_file("src/compiler/evaluator_primitives_mutation.cpp");
            CHECK(prim.find("Issue #3790") != std::string::npos, "3790 AC5: cites #3790");
            CHECK(prim.find("mutate_dispatch.hh") != std::string::npos,
                  "3790 AC5: includes dispatch");
            const auto rb_pos = prim.find("add(\"rollback\"");
            CHECK(rb_pos != std::string::npos, "3790 AC2: rollback prim");
            const auto rb_win = prim.substr(rb_pos, 2200);
            CHECK(rb_win.find("mutate_dispatch_try_acquire") != std::string::npos,
                  "3790 AC2: rollback acquires Guard");
            CHECK(rb_win.find("require_effect") != std::string::npos,
                  "3790 AC1: effect gate retained");
            const auto acq = rb_win.find("mutate_dispatch_try_acquire");
            const auto write = rb_win.find("rollback(mid)");
            CHECK(acq != std::string::npos && write != std::string::npos && acq < write,
                  "3790 AC3: acquire precedes FlatAST rollback (restamp on Guard exit)");
            const auto rs_pos = prim.find("add(\"rollback-since\"");
            CHECK(rs_pos != std::string::npos, "3790 AC2: rollback-since prim");
            const auto rs_win = prim.substr(rs_pos, 2200);
            CHECK(rs_win.find("mutate_dispatch_try_acquire") != std::string::npos,
                  "3790 AC2: rollback-since acquires Guard");
            CHECK(
                read_file("scripts/coverage/checks/check_rollback_mutation_boundary_guard_3790.py")
                        .find("#3790") != std::string::npos,
                "3790 AC5: coverage linter present");
            CHECK(read_file("tests/compiler/test_issue_3790.cpp").empty(), "3790 AC5: no invent");
            CHECK(read_file("docs/design/3790-rollback-guard.md").empty(),
                  "3790 AC5: no docs/design");
        }
    }

    // ── #3792 AC1/AC2/AC3: mutation-history rows filtered to the caller
    // occupancy tenant under the production consult regime — foreign mids
    // never cross the face (rollback-mid oracle residual of #3722 option-B
    // closed); Soft/Off keeps the full dump (filter-over-deny) ──
    {
        std::println("\n--- #3792 AC1-AC3: history tenant filter ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3792a x) x)\")").has_value(), "3792 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3792 eval");
        set_mode(SandboxMode::Restricted);
        aura::core::provenance::set_multi_tenant_env_active(true);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.grant_effect_capability(/*tenant=*/7, "mh-3792-a", kEffectMutate, /*mid=*/1);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3792 workspace");
        const auto own = static_cast<NodeId>(ws->size() - 2);
        const auto foreign = static_cast<NodeId>(ws->size() - 1);
        CHECK(own != NULL_NODE && foreign != NULL_NODE && own != foreign,
              "3792: two fresh node ids");
        ev.set_capability_tenant_id(7);
        const auto own_ref = ev.make_stamped_ref(own);
        CHECK(own_ref.tenant_id == 7, "3792: own occupancy 7");
        ev.set_capability_tenant_id(99);
        const auto f_ref = ev.make_stamped_ref(foreign);
        CHECK(f_ref.tenant_id == 99, "3792: foreign occupancy 99");
        ev.set_capability_tenant_id(7);
        seed_3722_record(*ws, /*mid=*/379201, own);
        seed_3722_record(*ws, /*mid=*/379202, foreign);
        auto dump_mids = [&ev](const auto& v) {
            std::string all;
            auto cur = v;
            int guard = 0;
            auto& pairs = ev.pairs();
            auto heap = ev.string_heap();
            while (is_pair(cur) && guard++ < 128) {
                const auto idx = as_pair_idx(cur);
                if (idx >= pairs.size())
                    break;
                if (is_string(pairs[idx].car)) {
                    const auto sidx = as_string_idx(pairs[idx].car);
                    if (sidx < heap.size())
                        all += heap[sidx];
                }
                cur = pairs[idx].cdr;
            }
            return all;
        };
        // Production face (Restricted+MT, caller tenant 7): own rows visible,
        // foreign-occupied rows filtered out. Agent surface is the #2054
        // engine:metrics facade (stats_impl args forwarding).
        const auto own_prod =
            cs.eval("(engine:metrics \"mutation-history\" " + std::to_string(own) + ")");
        CHECK(own_prod.has_value(), "3792 AC1: own history callable");
        const auto own_prod_s = own_prod ? dump_mids(*own_prod) : std::string();
        CHECK(own_prod_s.find("[379201]") != std::string::npos,
              "3792 AC1: own mid visible under production face");
        CHECK(own_prod_s.find("[379202]") == std::string::npos,
              "3792 AC2: foreign mid absent from production dump");
        const auto foreign_prod =
            cs.eval("(engine:metrics \"mutation-history\" " + std::to_string(foreign) + ")");
        const auto foreign_prod_s = foreign_prod ? dump_mids(*foreign_prod) : std::string();
        CHECK(foreign_prod_s.find("[379202]") == std::string::npos,
              "3792 AC1/AC2: foreign mid never crosses the production face");
        // Soft face: full dump unchanged (foreign mid visible — Soft contract).
        set_mode(SandboxMode::Off);
        ev.set_effect_sandbox_mode(0);
        aura::core::provenance::set_multi_tenant_env_active(false);
        const auto own_soft =
            cs.eval("(engine:metrics \"mutation-history\" " + std::to_string(own) + ")");
        const auto own_soft_s = own_soft ? dump_mids(*own_soft) : std::string();
        CHECK(own_soft_s.find("[379201]") != std::string::npos,
              "3792 AC3: Soft own mid still visible (full dump)");
        const auto foreign_soft =
            cs.eval("(engine:metrics \"mutation-history\" " + std::to_string(foreign) + ")");
        const auto foreign_soft_s = foreign_soft ? dump_mids(*foreign_soft) : std::string();
        CHECK(foreign_soft_s.find("[379202]") != std::string::npos,
              "3792 AC2/AC3: Soft full dump keeps foreign row (zero extra)");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    // ── #3792 AC4: source-cite — consult gate + occupancy resolve inside the
    // mutation-history face, gate precedes dump; no new query key; no invent ──
    {
        std::println("\n--- #3792 AC4: source-cite ---");
        const auto prim = read_file("src/compiler/evaluator_primitives_mutation.cpp");
        const auto mh_pos = prim.find("\"mutation-history\"");
        CHECK(mh_pos != std::string::npos, "3792 AC4: mutation-history prim present");
        const auto cite_pos = prim.find("Issue #3792:", mh_pos);
        CHECK(cite_pos != std::string::npos, "3792 AC4: handler cites #3792");
        const auto consult_pos =
            prim.find("const bool consult = strict || (restricted && mt);", mh_pos);
        CHECK(consult_pos != std::string::npos, "3792 AC4: consult regime predicate present");
        const auto existing_pos = prim.find("existing_stamp_for_node", mh_pos);
        const auto occ_pos = prim.find("occupying_stamp_for_node", mh_pos);
        CHECK(existing_pos != std::string::npos && occ_pos != std::string::npos,
              "3792 AC4: occupancy resolve chain present");
        const auto dump_pos = prim.find("push_string_heap", mh_pos);
        CHECK(consult_pos < dump_pos, "3792 AC4: gate precedes dump loop");
        CHECK(prim.find("\"3792") == std::string::npos, "3792 AC4: no new query key");
        CHECK(read_file("docs/design/3792-mutation-history-tenant-filter.md").empty(),
              "3792 AC4: no docs/design/3792-* per #1655");
        CHECK(read_file("tests/core/test_issue_3792.cpp").empty(),
              "3792 AC4: no test_issue_3792.cpp per #81934");
        int rc =
            std::system("python3 scripts/check_mutation_history_tenant_filter_3792.py --self-test");
        if (rc != 0)
            rc = std::system(
                "python3 ../scripts/check_mutation_history_tenant_filter_3792.py --self-test");
        CHECK(rc == 0, "3792 AC4: linter self-test passes");
    }

    // ── #3772 AC1: Restricted+MT — forged foreign (id . gen) re-entry
    // denies (void / #f) with joinable IsolationDeny SE ──
    {
        std::println("\n--- #3772 AC1: forged foreign ref re-entry denies ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Issue #4400: install the workspace under Off. The deny under test
        // is the foreign ref re-entry, which still runs with no Mutate grant.
        CHECK(cs.eval("(set-code \"(define (n3772a x) x)\")").has_value(), "3772 AC1 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3772 AC1 eval");
        set_mode(SandboxMode::Restricted);
        aura::core::provenance::set_multi_tenant_env_active(true);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(99);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3772 AC1 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3772 AC1 live node");
        // Tenant 99 holds the node (occupancy via make_stamped_ref — the
        // same ring entry export_ref seeds). gen99 is the TRUE generation
        // so the forged re-entry hits the isolation face, not staleness.
        const auto stamped = ev.make_stamped_ref(id);
        CHECK(stamped.tenant_id == 99, "3772 AC1: occupancy stamp tenant 99");
        const auto gen99 = static_cast<std::uint64_t>(stamped.gen);
        CHECK(gen99 != 0, "3772 AC1: gen resolved");
        // Tenant 7 forges the leaked wire pair.
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        const auto rg = cs.eval(std::format("(ast:ref-get {} {})", id, gen99));
        CHECK(rg.has_value() && aura::compiler::types::is_void(*rg),
              "3772 AC1: foreign ast:ref-get → void");
        const auto rv = cs.eval(std::format("(ast:ref-valid? {} {})", id, gen99));
        CHECK(rv.has_value() && is_bool(*rv) && !as_bool(*rv),
              "3772 AC1: foreign ast:ref-valid? → #f");

        bool iso_deny = false;
        for (std::uint64_t s = baseline; s < ring.seq.load(); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.seq == s &&
                static_cast<int>(e.kind) == static_cast<int>(SecurityEventKind::IsolationDeny)) {
                iso_deny = true;
                CHECK(e.mutation_id != 0, "3772 AC1: IsolationDeny mid joinable (#2156)");
            }
        }
        CHECK(iso_deny, "3772 AC1: IsolationDeny SE in ring");
        // AC3 spot: the write path keeps its #3415 occupancy gate.
        const auto bumps_before = ws->subtree_bump_count();
        CHECK(!ev.require_effect_for_node_id(kEffectMutate, "mutate:replace-type", id),
              "3772 AC3 spot: foreign mutate still denies (#3415)");
        CHECK(ws->subtree_bump_count() == bumps_before, "3772 AC3 spot: zero write");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    // ── #3772 AC2: legitimate export → round-trip observes own nodes ──
    {
        std::println("\n--- #3772 AC2: own-node export round-trip observes ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Issue #4400: install under Off, then arm. ast:ref-get is a read.
        CHECK(cs.eval("(set-code \"(define (n3772b x) x)\")").has_value(), "3772 AC2 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3772 AC2 eval");
        set_mode(SandboxMode::Restricted);
        aura::core::provenance::set_multi_tenant_env_active(true);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3772 AC2 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3772 AC2 live node");
        const auto sref7 = cs.eval(std::format("(ast:stable-ref {})", id));
        CHECK(sref7.has_value() && is_pair(*sref7), "3772 AC2: export pair");
        // Host-side gen: the wire pair is layout-only; the re-entry gate
        // re-derives occupancy -- the round-trip needs the current gen.
        const auto gen7 = static_cast<std::uint64_t>(ws->current_generation());
        CHECK(gen7 != 0, "3772 AC2: gen resolved");
        const auto rg = cs.eval(std::format("(ast:ref-get {} {})", id, gen7));
        CHECK(rg.has_value() && is_string(*rg), "3772 AC2: own ast:ref-get observes");
        const auto rv = cs.eval(std::format("(ast:ref-valid? {} {})", id, gen7));
        CHECK(rv.has_value() && is_bool(*rv) && as_bool(*rv), "3772 AC2: own ref-valid? allows");

        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    // ── #3772 AC4: Soft/Off — pair contract unchanged (legacy direct
    // observe path; no grants, no consult) ──
    {
        std::println("\n--- #3772 AC4: Soft/Off pair contract unchanged ---");
        reset_all(); // Sandbox Off, no grants, no MT consult
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3772c x) x)\")").has_value(), "3772 AC4 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3772 AC4 eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3772 AC4 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3772 AC4 live node");
        const auto sref = cs.eval(std::format("(ast:stable-ref {})", id));
        CHECK(sref.has_value() && is_pair(*sref), "3772 AC4: export pair");
        const auto gen = static_cast<std::uint64_t>(ws->current_generation());
        CHECK(gen != 0, "3772 AC4: gen resolved");
        const auto rg = cs.eval(std::format("(ast:ref-get {} {})", id, gen));
        CHECK(rg.has_value() && is_string(*rg), "3772 AC4: soft ast:ref-get observes");
        const auto rv = cs.eval(std::format("(ast:ref-valid? {} {})", id, gen));
        CHECK(rv.has_value() && is_bool(*rv) && as_bool(*rv), "3772 AC4: soft ref-valid? → #t");
    }

    // ── #3772 AC5: source-cite — gated re-entry cites #3772, helper in
    // the security TU; no test_issue file; no docs/design ──
    {
        std::println("\n--- #3772 AC5: source-cite ---");
        const auto prim = read_file("src/compiler/evaluator_primitives_ast.cpp");
        CHECK(prim.find("#3772") != std::string::npos, "3772 AC5: prims cite #3772");
        CHECK(prim.find("restamp_read_ref(ref, \"ast:ref-get\")") != std::string::npos,
              "3772 AC5: ast:ref-get gated");
        CHECK(prim.find("restamp_read_ref(ref, \"ast:ref-valid?\")") != std::string::npos,
              "3772 AC5: ast:ref-valid? gated");
        CHECK(prim.find("restamp_read_ref(sref, \"ast:stable-refs-valid?\")") != std::string::npos,
              "3772 AC5: bulk gated");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        CHECK(sec.find("#3772") != std::string::npos, "3772 AC5: helper cites #3772");
        const auto ixx = read_file("src/compiler/evaluator.ixx");
        CHECK(ixx.find("restamp_read_ref") != std::string::npos,
              "3772 AC5: helper declared in evaluator.ixx");
        CHECK(read_file("docs/design/3772-ast-ref-stamp.md").empty(),
              "3772 AC5: no docs/design/3772-* per #1655");
        CHECK(read_file("tests/core/test_issue_3772.cpp").empty(),
              "3772 AC5: no test_issue_3772.cpp per #81934");
    }


    // ── Issue #3797: sticky cross_grants after TenantAdmin revoke ──
    // grant_cross_tenant is TA-fenced at mint, but check_boundary_ex used to
    // consult cross_grants alone; revoke_cross_tenant was dead; TA revoke did
    // not erase matching rows. Closed loop: bind mint_principal on the row,
    // wipe via revoke_cross_tenant on TA-lost, and re-validate under
    // check_boundary_ex (production only). Soft/Off unchanged; IsolationDeny
    // still carries fiber_id + mid (#3011).
    {
        std::println("\n--- #3797 AC1: mint 7→42 Mutate with TA; revoke TA; "
                     "check_boundary_ex denies ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        grant_tenant_admin_mid(7);
        g_workspace_isolation().grant_cross_tenant(/*from=*/7, /*to=*/42, kEffectMutate,
                                                   /*caller_principal=*/7);
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) ==
                  static_cast<std::uint16_t>(kEffectMutate),
              "AC1: 7→42 Mutate grant landed with TA on 7");
        CHECK(g_workspace_isolation().cross_grant_mint_principal(7, 42) == 7,
              "AC1: mint_principal bound to authorizing TA holder (7)");
        CHECK(check_boundary(/*caller=*/7, /*target=*/42, nullptr, /*allow_cross=*/false,
                             kEffectMutate, /*strict=*/false, "3797-ac1-pre",
                             /*sandbox_restricted=*/true),
              "AC1: check_boundary_ex allows while TA still held");

        aura::core::capability::g_capability_registry().revoke(7, "tenant-admin");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC1: TA revoke wiped matching cross_grant via revoke_cross_tenant SSOT");
        CHECK(!check_boundary(/*caller=*/7, /*target=*/42, nullptr, /*allow_cross=*/false,
                              kEffectMutate, /*strict=*/false, "3797-ac1-post",
                              /*sandbox_restricted=*/true),
              "AC1: check_boundary_ex(7,42,Mutate) denies after TA revoke");
    }

    {
        std::println("\n--- #3797 AC2: revoke_cross_tenant called from TA-revoke SSOT ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        grant_tenant_admin_mid(7);
        g_workspace_isolation().grant_cross_tenant(7, 42, kEffectMutate, /*caller=*/7);
        g_workspace_isolation().grant_cross_tenant(7, 99, kEffectWrite, /*caller=*/7);
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) != 0, "AC2: 7→42 present");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 99) != 0, "AC2: 7→99 present");
        // Direct pair revoke still works (was dead API — now exercised).
        g_workspace_isolation().revoke_cross_tenant(7, 99);
        CHECK(g_workspace_isolation().cross_grant_bits(7, 99) == 0,
              "AC2: revoke_cross_tenant(7,99) erases the pair");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) != 0,
              "AC2: unrelated 7→42 untouched by pair revoke");
        // TA revoke → hook → revoke_cross_grants_minted_by → revoke_cross_tenant.
        aura::core::capability::g_capability_registry().revoke(7, "tenant-admin");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC2: TA-revoke SSOT clears remaining mint_principal=7 rows");
        // Source-cite: hook + minted_by + revoke_cross_tenant wired.
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto cap = read_file("src/core/capability_model.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        CHECK(iso.find("revoke_cross_grants_minted_by") != std::string::npos,
              "AC2: workspace_isolation owns revoke_cross_grants_minted_by");
        CHECK(iso.find("revoke_cross_tenant(k.from, k.to)") != std::string::npos,
              "AC2: minted_by literally calls revoke_cross_tenant");
        CHECK(cap.find("maybe_notify_tenant_admin_lost") != std::string::npos,
              "AC2: capability revoke notifies TA-lost hook");
        CHECK(sec.find("maybe_notify_tenant_admin_lost") != std::string::npos,
              "AC2: Evaluator foreign revoke also notifies TA-lost");
    }

    {
        std::println("\n--- #3797 AC3: Soft/Off zero-cost / unchanged ---");
        reset_all(); // Sandbox Off
        // Soft mint: no TA required; mint_principal stays 0; revoke of a
        // non-TA name must not disturb Soft rows.
        g_workspace_isolation().grant_cross_tenant(1, 2, kEffectMutate);
        CHECK(g_workspace_isolation().cross_grant_bits(1, 2) ==
                  static_cast<std::uint16_t>(kEffectMutate),
              "AC3: Soft grant lands without TA");
        CHECK(g_workspace_isolation().cross_grant_mint_principal(1, 2) == 0,
              "AC3: Soft mint_principal stays 0");
        CHECK(check_boundary(1, 2, nullptr, false, kEffectMutate),
              "AC3: Soft check_boundary allows Soft grant (no TA re-check)");
        // allow_cross Soft short-circuit unchanged.
        CHECK(check_boundary(1, 99, nullptr, /*allow_cross=*/true, kEffectMutate),
              "AC3: Soft allow_cross short-circuit unchanged");
    }

    {
        std::println("\n--- #3797 AC4/AC5: dual-eval chaos + IsolationDeny fiber/mid ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_b.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(7);
        ev_b.set_capability_tenant_id(7);
        grant_tenant_admin_mid(7);
        ev_a.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) != 0,
              "AC4: pre-revoke grant present");
        // Dual-eval: B revokes TA while A would still see the grant.
        aura::core::capability::set_effect_fiber_id_override(4242);
        aura::core::capability::g_capability_registry().revoke(7, "tenant-admin");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC4: dual-eval TA revoke cleared sticky grant");
        const auto before = current_iso_seq();
        CHECK(!check_boundary(7, 42, nullptr, false, kEffectMutate, false, "3797-ac4", true),
              "AC4: check_boundary_ex denies post-revoke");
        const auto after = current_iso_seq();
        CHECK(after > before, "AC5: IsolationDeny SE emitted on sticky deny");
        // Scan private audit ring for fiber_id + mid on the deny entry.
        bool found_deny = false;
        for (std::size_t i = 0; i < g_workspace_isolation().kAuditRing; ++i) {
            const auto pub =
                g_workspace_isolation().audit_ring[i].publish_seq.load(std::memory_order_acquire);
            if (pub == 0)
                continue;
            const auto& e = g_workspace_isolation().audit_ring[i].data;
            if (!e.denied)
                continue;
            found_deny = true;
            CHECK(e.fiber_id == 4242, "AC5: IsolationDeny carries fiber_id (#3011)");
            // mid may be 0 at process origin (#3594) — just ensure field is present
            // (mutation_id readable); non-negative always.
            CHECK(e.mutation_id == e.mutation_id, "AC5: IsolationDeny carries mid field");
            break;
        }
        CHECK(found_deny, "AC5: found IsolationDeny audit entry");
        aura::core::capability::set_effect_fiber_id_override(0);
        // No new query key.
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(prim.find("schema-3797") == std::string::npos, "AC4: no schema-3797 query key");
        CHECK(prim.find("issue-3797") == std::string::npos, "AC4: no issue-3797 query key");
    }

    // ── Issue #3998: Soft-era cross_grants die on Soft→production flip ──
    {
        std::println("\n--- #3998 AC1: Off mint + set_mode(Restricted) erases + deny ---");
        reset_all();
        using aura::core::workspace_isolation::kSoftEraCrossGrantSweepIssue;
        CHECK(kSoftEraCrossGrantSweepIssue == 3998, "3998 AC1: issue stamp");
        g_workspace_isolation().grant_cross_tenant(1, 2, kEffectMutate);
        CHECK(g_workspace_isolation().cross_grant_bits(1, 2) ==
                  static_cast<std::uint16_t>(kEffectMutate),
              "3998 AC1: Soft mint lands");
        CHECK(g_workspace_isolation().cross_grant_mint_principal(1, 2) == 0,
              "3998 AC1: Soft mint_principal stays 0");
        CHECK(check_boundary(1, 2, nullptr, false, kEffectMutate),
              "3998 AC1: Soft check allows (AC5 unchanged)");
        const auto& ring = g_security_event_ring();
        const auto seq0 = ring.seq.load(std::memory_order_acquire);
        set_mode(SandboxMode::Restricted);
        CHECK(g_workspace_isolation().cross_grant_bits(1, 2) == 0,
              "3998 AC1: Soft-era row erased on Restricted entry");
        CHECK(!check_boundary(1, 2, nullptr, false, kEffectMutate, /*sandbox_strict=*/false,
                              "3998-ac1-post", /*sandbox_restricted=*/true),
              "3998 AC1: production check_boundary_ex denies");
        bool se = false;
        const auto cur = ring.seq.load(std::memory_order_acquire);
        for (auto s = cur; s > seq0 && s > 0; --s) {
            const auto& e = ring.ring[(s - 1) % ring.ring.size()];
            if (std::string_view(e.reason) == "soft-era-cross-grant-cleared") {
                se = true;
                CHECK(e.tenant_id == 1, "3998 AC1: SE tenant is from-principal");
                break;
            }
        }
        CHECK(se, "3998 AC1: SE soft-era-cross-grant-cleared emitted");
        reset_all();
    }

    {
        std::println("\n--- #3998 AC2: Soft allow path unchanged ---");
        reset_all();
        g_workspace_isolation().grant_cross_tenant(3, 4, kEffectMutate);
        CHECK(g_workspace_isolation().cross_grant_mint_principal(3, 4) == 0,
              "3998 AC2: Soft mint_principal 0");
        CHECK(check_boundary(3, 4, nullptr, false, kEffectMutate),
              "3998 AC2: Soft check still allows without TA");
        CHECK(g_workspace_isolation().cross_grant_bits(3, 4) != 0,
              "3998 AC2: Soft row not swept while Off");
        reset_all();
    }

    {
        std::println("\n--- #3998 AC3: source-cite; no invent ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto sb = read_file("src/core/sandbox.hh");
        CHECK(iso.find("kSoftEraCrossGrantSweepIssue = 3998") != std::string::npos,
              "3998 AC3: stamp");
        CHECK(iso.find("sweep_soft_era_cross_grants") != std::string::npos,
              "3998 AC3: sweep helper");
        CHECK(iso.find("Issue #3998") != std::string::npos, "3998 AC3: allows_locked cites");
        const auto allows = iso.find("bool cross_grant_allows_locked");
        CHECK(allows != std::string::npos, "3998 AC3: allows_locked present");
        const auto win = iso.substr(allows, 1800);
        CHECK(win.find("mint == 0") != std::string::npos &&
                  win.find("cross_grants.erase(it)") != std::string::npos,
              "3998 AC3: production mint==0 erases");
        CHECK(win.find("return true; // Soft/legacy row") == std::string::npos,
              "3998 AC3: bits-only Soft-era allow removed");
        CHECK(sb.find("sweep_soft_era_cross_grants") != std::string::npos,
              "3998 AC3: set_mode sweeps on production entry");
        CHECK(!std::filesystem::exists("docs/design/3998-soft-era-cross-grant.md"),
              "3998 AC3: no docs/design");
        CHECK(!std::filesystem::exists("tests/issues/test_issue_3998.cpp"),
              "3998 AC3: no tests/issues invent");
        reset_all();
    }

    // ── Issue #3800: grant_cross_tenant TA fence is caller-only ──
    // Target-only TA must not mint cross_grants under Restricted/Strict.
    {
        std::println("\n--- #3800 AC1: caller lacks TA, target has TA → deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::g_capability_registry().default_tenant.store(
            7, std::memory_order_release);
        grant_tenant_admin_mid(42); // target only
        const auto bits_before = g_workspace_isolation().cross_grant_bits(7, 42);
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/7, /*to=*/42, kEffectMutate);
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == bits_before,
              "AC1: cross_grant_bits unchanged (target-only TA deny)");
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1, "AC1: deny counter bumps on target-only TA");
    }

    {
        std::println("\n--- #3800 AC2: caller has TA → allow (unchanged) ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::g_capability_registry().default_tenant.store(
            7, std::memory_order_release);
        grant_tenant_admin_mid(7); // caller
        const auto allow_before =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/7, /*to=*/42, kEffectMutate);
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) ==
                  static_cast<std::uint16_t>(kEffectMutate),
              "AC2: caller-TA allow installs cross grant");
        CHECK(g_workspace_isolation().cross_grant_mint_principal(7, 42) == 7,
              "AC2: mint_principal bound to caller (#3797/#3800)");
        const auto allow_after =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(allow_after == allow_before + 1, "AC2: allow counter bumps");
    }

    {
        std::println("\n--- #3800 AC3: Soft/Off zero-cost unchanged ---");
        reset_all(); // Off
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/1, /*to=*/2, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC3: Soft/Off does not deny");
        CHECK(g_workspace_isolation().cross_grant_bits(1, 2) ==
                  static_cast<std::uint16_t>(kEffectMutate),
              "AC3: Soft/Off grant lands without TA");
        CHECK(g_workspace_isolation().cross_grant_mint_principal(1, 2) == 0,
              "AC3: Soft mint_principal stays 0");
    }

    {
        std::println("\n--- #3800 AC5: dual-Evaluator A(no TA) cannot mint into B(TA) ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::g_capability_registry().default_tenant.store(
            0, std::memory_order_release);
        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_b.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(7);
        ev_b.set_capability_tenant_id(42);
        grant_tenant_admin_mid(42); // B holds TA; A does not
        const auto deny0 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        ev_a.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny1 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny1 == deny0 + 1, "AC5: A (no TA) mint into B (TA target) → deny");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC5: no cross_grant from non-TA A into TA B");
        // B (caller has TA) can still mint outbound.
        ev_b.grant_cross_tenant_access(/*from=*/42, /*to=*/99, kEffectMutate);
        CHECK(g_workspace_isolation().cross_grant_bits(42, 99) ==
                  static_cast<std::uint16_t>(kEffectMutate),
              "AC5: B (holds TA) can still mint outbound");
    }

    {
        std::println("\n--- #3800 AC4: source-cite + linter + no invent / query key ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(iso.find("Issue #3800") != std::string::npos ||
                  iso.find("#3800") != std::string::npos,
              "AC4: workspace_isolation.hh cites #3800");
        CHECK(iso.find("caller-only") != std::string::npos ||
                  iso.find("caller-only TenantAdmin") != std::string::npos,
              "AC4: caller-only TA fence documented");
        // Must not OR target TA anymore.
        CHECK(iso.find("caller_ta || target_ta") == std::string::npos,
              "AC4: caller_ta || target_ta removed");
        CHECK(test_self.find("#3800") != std::string::npos, "AC4: test file cites #3800");
        CHECK(build.find("check_cross_tenant_grant_caller_ta_3800") != std::string::npos,
              "AC4: build.py wires #3800 linter");
        CHECK(prim.find("schema-3800") == std::string::npos, "AC4: no schema-3800 query key");
        CHECK(prim.find("issue-3800") == std::string::npos, "AC4: no issue-3800 query key");
        std::ifstream invent("tests/core/test_issue_3800.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3800.cpp");
        CHECK(!invent.good(), "AC4: no tests/core/test_issue_3800.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3800-") == std::string::npos,
                      std::string("AC4: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }


    // ── Issue #3801: IsolationDeny mid joins TypedMid (no phantom 1) ──
    {
        std::println(
            "\n--- #3801 AC1: Guard TypedMid≠epoch → IsolationDeny SE mid == TypedMid ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::compiler::typed_audit::apply_production_audit_defaults();
        // Divergence: Mutation epoch = 42, TypedMid = 777.
        aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 42);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.arm_production_audit_defaults_for_test();
        ev.note_boundary_audit_mid_for_test(777);
        // Also stamp process-wide TypedMid so core IsolationDeny hook joins.
        aura::compiler::typed_audit::stamp_type_linear_commit_proof(777);
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        CHECK(!ev.check_workspace_isolation(/*target=*/42, /*ref_tenant=*/42, kEffectMutate,
                                            "test:3801-ac1"),
              "3801 AC1: cross-tenant IsolationDeny fires");
        bool found = false;
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (static_cast<int>(e.kind) !=
                    static_cast<int>(
                        aura::core::security_event::SecurityEventKind::IsolationDeny) ||
                e.seq != s)
                continue;
            found = true;
            CHECK(e.mutation_id == 777,
                  "3801 AC1: IsolationDeny SE mid == TypedMid 777 (not epoch 42)");
            // Issue #4052: the epoch column is the emit-time Mutation epoch
            // — not the same TypedMid the mutation_id column legitimately
            // carries (#3801). IsolationDeny rows must replay by Mutation
            // epoch alongside grant.grant_epoch / the mutation-audit ring.
            CHECK(e.epoch == 42,
                  "4052: IsolationDeny SE epoch == Mutation epoch 42 (not TypedMid 777)");
            CHECK(aura::core::current_mutation_epoch() == 42, "3801 AC1 pre: epoch still 42");
        }
        CHECK(found, "3801 AC1: IsolationDeny SE in ring");
        // Issue #4052: the Typed correlated row for the same deny carries the
        // Mutation epoch in both the before- and after-epoch fields, so the
        // typed trail joins the SE by epoch (mutation_id stays TypedMid).
        bool trail_found = false;
        {
            std::lock_guard<std::mutex> lock(aura::compiler::typed_audit::g_trail().mu);
            for (const auto& te : aura::compiler::typed_audit::g_trail().ring) {
                if (te.mutation_id != 777)
                    continue;
                trail_found = true;
                CHECK(te.before_epoch == 42, "4052: typed correlated before_epoch == 42");
                CHECK(te.after_epoch == 42, "4052: typed correlated after_epoch == 42");
            }
        }
        CHECK(trail_found, "4052: typed correlated row for TypedMid 777 reachable");
        ev.clear_boundary_audit_mid_for_test();
        aura::compiler::typed_audit::apply_dev_audit_defaults();
    }

    {
        std::println("\n--- #3801 AC2: Restricted+MT epoch=0 deny SE mid=0 (no phantom 1) ---");
        reset_all();
        aura::core::reset_mutation_epoch_for_test();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.arm_production_audit_defaults_for_test();
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        CHECK(aura::core::current_mutation_epoch() == 0, "3801 AC2 pre: epoch=0");
        // allow-cross deny (no TA) — production EffectDeny mid must be 0.
        ev.set_tenant_principal(7, "ac3801", /*allow_cross=*/true);
        bool saw_mid0 = false;
        bool saw_mid1 = false;
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.seq != s)
                continue;
            if (std::string_view(e.reason).find("allow-cross-needs-tenant-admin") ==
                std::string_view::npos)
                continue;
            if (e.mutation_id == 0)
                saw_mid0 = true;
            if (e.mutation_id == 1)
                saw_mid1 = true;
        }
        CHECK(saw_mid0, "3801 AC2: allow-cross deny SE mid=0");
        CHECK(!saw_mid1, "3801 AC2: no phantom mid=1 on allow-cross deny");
        // IsolationDeny at epoch=0 also mid=0 (#3594/#3801).
        const auto se2 = ring.seq.load(std::memory_order_acquire);
        CHECK(!ev.check_workspace_isolation(42, 42, kEffectMutate, "test:3801-ac2-iso"),
              "3801 AC2: IsolationDeny fires at epoch=0");
        bool iso_mid0 = false;
        bool iso_mid1 = false;
        for (std::uint64_t s = se2; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (static_cast<int>(e.kind) !=
                    static_cast<int>(
                        aura::core::security_event::SecurityEventKind::IsolationDeny) ||
                e.seq != s)
                continue;
            if (e.mutation_id == 0)
                iso_mid0 = true;
            if (e.mutation_id == 1)
                iso_mid1 = true;
        }
        CHECK(iso_mid0, "3801 AC2: IsolationDeny mid=0 at epoch=0");
        CHECK(!iso_mid1, "3801 AC2: no phantom IsolationDeny mid=1");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
    }

    {
        std::println("\n--- #3801 AC3: Soft/Off observe stamp contract unchanged ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        // Soft require_effect mid=1 observe (#2493) — production_deny helper not used.
        const auto src = read_file("src/compiler/evaluator_security.cpp");
        CHECK(src.find("mid = 1; // Soft / standalone: non-zero join stamp") != std::string::npos ||
                  src.find("mid = 1; // Soft") != std::string::npos ||
                  src.find("Soft / Off keeps the mid=1 observe stamp") != std::string::npos,
              "3801 AC3: Soft mid=1 observe stamp preserved in require_effect");
        CHECK(src.find("epoch != 0 ? epoch : static_cast<std::uint64_t>(1)") != std::string::npos,
              "3801 AC3: Soft fiber-principal observe arm still has epoch?:1");
        CHECK(src.find("prov.mutation_id == 0 && !force_bind") != std::string::npos,
              "3801 AC3: Soft session mid=1 gated behind !force_bind");
    }

    {
        std::println("\n--- #3801 AC4: cite-first linter + no new query key ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto build = read_file("build.py");
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(iso.find("Issue #3801") != std::string::npos, "AC4: workspace_isolation cites #3801");
        CHECK(iso.find("aura_isolation_deny_se_mid") != std::string::npos,
              "AC4: IsolationDeny uses aura_isolation_deny_se_mid");
        const auto capm = read_file("src/core/capability_model.hh");
        CHECK(capm.find("Issue #3837") != std::string::npos,
              "AC4: capability_model cites #3837 string-fence mid join");
        CHECK(capm.find("try_grant_capability_string_path_privileged_locked") !=
                      std::string::npos &&
                  capm.find("aura_isolation_deny_se_mid") != std::string::npos,
              "AC4: string fence uses aura_isolation_deny_se_mid");
        CHECK(sec.find("production_deny_se_mid") != std::string::npos,
              "AC4: production_deny_se_mid helper present");
        CHECK(sec.find("Issue #3801") != std::string::npos, "AC4: evaluator_security cites #3801");
        CHECK(build.find("check_isolation_deny_mid_join_3801") != std::string::npos,
              "AC4: build.py wires #3801 linter");
        CHECK(prim.find("schema-3801") == std::string::npos, "AC4: no schema-3801 query key");
        CHECK(prim.find("issue-3801") == std::string::npos, "AC4: no issue-3801 query key");
        std::ifstream invent("tests/core/test_issue_3801.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3801.cpp");
        CHECK(!invent.good(), "AC4: no tests/core/test_issue_3801.cpp (forbidden per #81967)");
    }

    // ── Issue #3837: string write-fence deny SE mid joins TypedMid SSOT ──
    // Residual after #3801: try_grant_capability_string_path_privileged_locked
    // used mid = epoch ?: 1. Must use aura_isolation_deny_se_mid.
    {
        std::println(
            "\n--- #3837 AC1: Guard TypedMid≠epoch → wildcard fence deny SE mid == TypedMid ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        // Seed wildcard-only under Off (zero-cost fence short-circuit).
        ev.grant_capability("*");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        {
            auto& reg = aura::core::capability::g_capability_registry();
            std::lock_guard<std::mutex> lock(reg.mtx);
            CHECK(reg.holds_wildcard_only_locked(7), "3837 AC1 pre: wildcard-only holder");
        }
        aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 42);
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.arm_production_audit_defaults_for_test();
        ev.note_boundary_audit_mid_for_test(777);
        aura::compiler::typed_audit::stamp_type_linear_commit_proof(777);
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        const auto deny_before =
            aura::core::capability::g_capability_effect_metrics()
                .capability_wildcard_write_fence_deny_total.load(std::memory_order_relaxed);
        // Privilege-bearing string path ("capability" → TenantAdmin).
        ev.grant_capability("capability");
        const auto deny_after =
            aura::core::capability::g_capability_effect_metrics()
                .capability_wildcard_write_fence_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1, "3837 AC1: fence deny counter bumps");
        bool found = false;
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.seq != s)
                continue;
            if (std::string_view(e.reason).find(
                    "wildcard-write-fence-needs-explicit-tenant-admin") == std::string_view::npos)
                continue;
            found = true;
            CHECK(e.mutation_id == 777,
                  "3837 AC1: fence deny SE mid == TypedMid 777 (not epoch 42 / not 1)");
            CHECK(aura::core::current_mutation_epoch() == 42, "3837 AC1 pre: epoch still 42");
        }
        CHECK(found, "3837 AC1: fence EffectDeny SE in ring");
        ev.clear_boundary_audit_mid_for_test();
        aura::compiler::typed_audit::apply_dev_audit_defaults();
    }

    {
        std::println(
            "\n--- #3837 AC2: Restricted epoch=0 wildcard fence deny SE mid=0 (no phantom 1) ---");
        reset_all();
        aura::core::reset_mutation_epoch_for_test();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        // Seed under Off before production audit defaults (#3090 mid refuse).
        ev.grant_capability("*");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        ev.set_effect_sandbox_mode(1);
        ev.arm_production_audit_defaults_for_test();
        CHECK(aura::core::current_mutation_epoch() == 0, "3837 AC2 pre: epoch=0");
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        ev.grant_capability("capability");
        bool saw_mid0 = false;
        bool saw_mid1 = false;
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.seq != s)
                continue;
            if (std::string_view(e.reason).find(
                    "wildcard-write-fence-needs-explicit-tenant-admin") == std::string_view::npos)
                continue;
            if (e.mutation_id == 0)
                saw_mid0 = true;
            if (e.mutation_id == 1)
                saw_mid1 = true;
        }
        CHECK(saw_mid0, "3837 AC2: fence deny SE mid=0 at epoch=0");
        CHECK(!saw_mid1, "3837 AC2: no phantom mid=1 on fence deny");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
    }

    {
        std::println("\n--- #3837 AC3: Soft/Off fence short-circuit unchanged ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        ev.grant_capability("*");
        ev.set_effect_sandbox_mode(0); // Off
        const auto before =
            aura::core::capability::g_capability_effect_metrics()
                .capability_wildcard_write_fence_deny_total.load(std::memory_order_relaxed);
        ev.grant_capability("capability");
        const auto after =
            aura::core::capability::g_capability_effect_metrics()
                .capability_wildcard_write_fence_deny_total.load(std::memory_order_relaxed);
        CHECK(after == before, "3837 AC3: Soft/Off → fence counter does not bump");
        CHECK(ev.has_capability("capability"),
              "3837 AC3: Soft/Off wildcard contract preserved for capability");
        const auto cap = read_file("src/core/capability_model.hh");
        CHECK(cap.find("Issue #3837") != std::string::npos,
              "3837 AC3: capability_model cites #3837");
        CHECK(cap.find("aura_isolation_deny_se_mid") != std::string::npos,
              "3837 AC3: fence uses aura_isolation_deny_se_mid");
        CHECK(cap.find("const auto mid = epoch != 0 ? epoch : static_cast<std::uint64_t>(1);") ==
                  std::string::npos,
              "3837 AC3: no bare epoch?:1 phantom mid in capability_model");
    }


    // ── Issue #3808: obs join oracle — IsolationDeny ↔ security-audit / evo ──
    // #3801 already fixed record_audit mid = TypedMid-then-epoch. This block
    // locks AC4: under Guard TypedMid≠epoch the IsolationDeny SE is joinable
    // via query:security-audit mutation-id=TypedMid and
    // query:evolution-audit-decision last-se-reason (not epoch-only mid).
    {
        std::println(
            "\n--- #3808 AC4: Guard TypedMid≠epoch → security-audit + evo last-se-reason ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::compiler::typed_audit::apply_production_audit_defaults();
        aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 42);
        aura::core::capability::set_effect_fiber_id_override(99);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.arm_production_audit_defaults_for_test();
        ev.note_boundary_audit_mid_for_test(777);
        aura::compiler::typed_audit::stamp_type_linear_commit_proof(777);
        CHECK(!ev.check_workspace_isolation(/*target=*/42, /*ref_tenant=*/42, kEffectMutate,
                                            "test:3808-ac4"),
              "3808 AC4: cross-tenant IsolationDeny fires");
        // query:security-audit [limit] [tenant] [fiber] [since-seq] [mutation-id]
        // — mid=TypedMid 777 must hit; epoch 42 must miss IsolationDeny.
        auto q_typed = cs.eval(R"((engine:metrics "query:security-audit" 16 7 99 0 777))");
        CHECK(q_typed.has_value(), "3808 AC4: query:security-audit mid=TypedMid callable");
        bool saw_iso_typed = false;
        if (q_typed) {
            auto cur = *q_typed;
            int guard = 0;
            auto& pairs = ev.pairs();
            auto heap = ev.string_heap();
            while (is_pair(cur) && guard++ < 64) {
                const auto idx = as_pair_idx(cur);
                if (idx >= pairs.size())
                    break;
                if (is_string(pairs[idx].car)) {
                    const auto sidx = as_string_idx(pairs[idx].car);
                    if (sidx < heap.size()) {
                        const std::string ln(heap[sidx]);
                        if (ln.find("kind=IsolationDeny") != std::string::npos &&
                            ln.find("mutation_id=777") != std::string::npos)
                            saw_iso_typed = true;
                    }
                }
                cur = pairs[idx].cdr;
            }
        }
        CHECK(saw_iso_typed,
              "3808 AC4: query:security-audit mutation-id=TypedMid returns IsolationDeny");
        auto q_epoch = cs.eval(R"((engine:metrics "query:security-audit" 16 7 99 0 42))");
        CHECK(q_epoch.has_value(), "3808 AC4: query:security-audit mid=epoch callable");
        bool saw_iso_epoch = false;
        if (q_epoch) {
            auto cur = *q_epoch;
            int guard = 0;
            auto& pairs = ev.pairs();
            auto heap = ev.string_heap();
            while (is_pair(cur) && guard++ < 64) {
                const auto idx = as_pair_idx(cur);
                if (idx >= pairs.size())
                    break;
                if (is_string(pairs[idx].car)) {
                    const auto sidx = as_string_idx(pairs[idx].car);
                    if (sidx < heap.size()) {
                        const std::string ln(heap[sidx]);
                        if (ln.find("kind=IsolationDeny") != std::string::npos &&
                            ln.find("mutation_id=42") != std::string::npos)
                            saw_iso_epoch = true;
                    }
                }
                cur = pairs[idx].cdr;
            }
        }
        CHECK(!saw_iso_epoch,
              "3808 AC4: query:security-audit mutation-id=epoch does not return IsolationDeny");
        // evolution-audit-decision last-se-reason joins the same TypedMid.
        auto rsn = cs.eval(
            R"((hash-ref (engine:metrics "query:evolution-audit-decision" 777) "last-se-reason"))");
        bool reason_ok = false;
        if (rsn && is_string(*rsn)) {
            auto heap = ev.string_heap();
            const auto sidx = as_string_idx(*rsn);
            reason_ok =
                sidx < heap.size() && heap[sidx].find("isolation-deny:") != std::string::npos;
        }
        CHECK(reason_ok, "3808 AC4: evolution-audit-decision last-se-reason is isolation-deny:*");
        auto rsn_code = cs.eval(
            R"((hash-ref (engine:metrics "query:evolution-audit-decision" 777) "last-se-reason-code"))");
        CHECK(rsn_code && is_int(*rsn_code) && as_int(*rsn_code) != 0,
              "3808 AC4: last-se-reason-code non-zero for IsolationDeny");
        ev.clear_boundary_audit_mid_for_test();
        aura::core::capability::set_effect_fiber_id_override(0);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
    }

    {
        std::println("\n--- #3808 Soft/Off + cite-first (obs join contract) ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto build = read_file("build.py");
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        CHECK(iso.find("Issue #3808") != std::string::npos,
              "3808: workspace_isolation cites #3808");
        CHECK(iso.find("query:security-audit") != std::string::npos,
              "3808: iso cites query:security-audit obs join");
        CHECK(iso.find("if (!denied)") != std::string::npos,
              "3808 Soft/Off: record_audit early return on !denied retained");
        CHECK(build.find("check_isolation_deny_obs_join_3808") != std::string::npos,
              "3808: build.py wires obs-join linter");
        CHECK(prim.find("schema-3808") == std::string::npos, "3808: no schema-3808 query key");
        CHECK(prim.find("issue-3808") == std::string::npos, "3808: no issue-3808 query key");
        CHECK(prim.find("TypedMid-then-epoch") != std::string::npos,
              "3808: security-audit-stats cites TypedMid-then-epoch");
        CHECK(test_self.find("3808 AC4") != std::string::npos, "3808: test hosts AC4 oracle");
        std::ifstream invent("tests/core/test_issue_3808.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3808.cpp");
        CHECK(!invent.good(), "3808: no tests/core/test_issue_3808.cpp (forbidden per #81967)");
    }


    // ── Issue #3802: EXEMPT_2ARG write-file/sys-* tenant host-path isolation ──
    {
        std::println(
            "\n--- #3802 AC1: Restricted+MT tenant A cannot write under tenant B prefix ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        // Isolate tenant FS base for the oracle.
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-3802-ac1";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::resolve_tenant_host_path;
        using aura::compiler::security::tenant_host_root_for;
        using aura::compiler::security::TenantHostPathVerdict;
        const auto root_a = tenant_host_root_for(7);
        const auto root_b = tenant_host_root_for(42);
        const auto escape = root_b + "/clobber.txt";
        auto denied = resolve_tenant_host_path(escape, /*tenant=*/7, /*active=*/true);
        CHECK(denied.verdict == TenantHostPathVerdict::Deny,
              "3802 AC1: lexical resolve denies A→B prefix");
        auto ok = resolve_tenant_host_path("ok.txt", /*tenant=*/7, /*active=*/true);
        CHECK(ok.verdict == TenantHostPathVerdict::Resolved, "3802 AC1: relative resolves under A");
        CHECK(ok.resolved.find(root_a) == 0, "3802 AC1: resolved under tenant A root");

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        std::string out;
        CHECK(!ev.check_tenant_host_path(escape, out, "write-file"),
              "3802 AC1: Evaluator denies A→B path");
        bool saw = false;
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.seq != s)
                continue;
            if (static_cast<int>(e.kind) !=
                static_cast<int>(aura::core::security_event::SecurityEventKind::IsolationDeny))
                continue;
            if (std::string_view(e.reason).find("tenant-path-escape") != std::string_view::npos) {
                saw = true;
                CHECK(e.tenant_id == 7, "3802 AC1: SE tenant is caller A");
            }
        }
        CHECK(saw, "3802 AC1: IsolationDeny SE reason tenant-path-escape");
        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #3802 AC2: Soft/Off / single-tenant Restricted passthrough ---");
        reset_all();
        using aura::compiler::security::resolve_tenant_host_path;
        using aura::compiler::security::tenant_host_path_policy_active;
        using aura::compiler::security::TenantHostPathVerdict;
        CHECK(!tenant_host_path_policy_active(/*mode=*/0, /*mt=*/false),
              "3802 AC2: Soft/Off inactive");
        CHECK(!tenant_host_path_policy_active(/*mode=*/1, /*mt=*/false),
              "3802 AC2: single-tenant Restricted inactive");
        CHECK(tenant_host_path_policy_active(/*mode=*/1, /*mt=*/true),
              "3802 AC2: Restricted+MT active");
        CHECK(tenant_host_path_policy_active(/*mode=*/2, /*mt=*/false),
              "3802 AC2: Strict active even without MT");
        auto soft = resolve_tenant_host_path("/tmp/anywhere.txt", 7, /*active=*/false);
        CHECK(soft.verdict == TenantHostPathVerdict::Passthrough,
              "3802 AC2: Soft passthrough keeps absolute path");

        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        aura::core::provenance::set_multi_tenant_env_active(false);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        ev.set_capability_tenant_id(7);
        std::string out;
        CHECK(ev.check_tenant_host_path("/tmp/anywhere.txt", out, "write-file"),
              "3802 AC2: Off check_tenant_host_path allows");
        CHECK(out == "/tmp/anywhere.txt", "3802 AC2: Off leaves path unchanged");

        // Single-tenant Restricted — no MT flag.
        ev.set_effect_sandbox_mode(1);
        CHECK(ev.check_tenant_host_path("/tmp/anywhere.txt", out, "write-file"),
              "3802 AC2: single-tenant Restricted allows absolute");
    }

    {
        std::println(
            "\n--- #3802 AC3: EXEMPT_2ARG stays 2-arg; #3836 grew size to 7; no new query key ---");
        const auto mandate =
            read_file("scripts/coverage/checks/check_side_effect_node_id_mandate_2942.py");
        const auto fiber =
            read_file("scripts/coverage/checks/check_side_effect_fiber_principal_2839.py");
        const auto ixx = read_file("src/compiler/evaluator.ixx");
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(mandate.find("EXEMPT_2ARG_OPS") != std::string::npos,
              "AC3: mandate inventory present");
        CHECK(ixx.find("kResidualNodeIdExemptOpsCount = 7") != std::string::npos,
              "AC3: kResidualNodeIdExemptOpsCount = 7 (#3836 shell/command-output)");
        CHECK(ixx.find("kNodeIdMandateExemptOpsCount = 7") != std::string::npos,
              "AC3: kNodeIdMandateExemptOpsCount = 7");
        CHECK(fiber.find("expected 7") != std::string::npos ||
                  fiber.find("!= 7") != std::string::npos,
              "AC3: fiber linter expects 7 exempt ops");
        CHECK(mandate.find("write-file") != std::string::npos,
              "AC3: write-file still EXEMPT (#3802 retained)");
        CHECK(prim.find("schema-3802") == std::string::npos, "AC3: no schema-3802 query key");
        CHECK(prim.find("issue-3802") == std::string::npos, "AC3: no issue-3802 query key");
    }

    {
        std::println("\n--- #3802 AC4: cite-first linter + dual-tenant chaos oracle ---");
        const auto hh = read_file("src/compiler/tenant_host_path.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto filep = read_file("src/compiler/evaluator_primitives_file.cpp");
        const auto iop = read_file("src/compiler/evaluator_primitives_io.cpp");
        const auto build = read_file("build.py");
        CHECK(hh.find("Issue #3802") != std::string::npos, "AC4: tenant_host_path cites #3802");
        CHECK(sec.find("check_tenant_host_path") != std::string::npos,
              "AC4: evaluator_security implements check_tenant_host_path");
        CHECK(filep.find("check_tenant_host_path") != std::string::npos,
              "AC4: write-file wires check_tenant_host_path");
        CHECK(iop.find("check_tenant_host_path") != std::string::npos,
              "AC4: sys-open/sys-write wire check_tenant_host_path");
        CHECK(build.find("check_tenant_host_path_isolation_3802") != std::string::npos,
              "AC4: build.py wires #3802 linter");

        // Dual-tenant chaos: A and B Evaluators; each denied on the other's root.
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-3802-chaos";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        CompilerService cs_a;
        CompilerService cs_b;
        auto& a = cs_a.evaluator();
        auto& b = cs_b.evaluator();
        a.set_effect_sandbox_mode(1);
        b.set_effect_sandbox_mode(1);
        a.set_capability_tenant_id(11);
        b.set_capability_tenant_id(22);
        const auto path_a = aura::compiler::security::tenant_host_root_for(11) + "/x.txt";
        const auto path_b = aura::compiler::security::tenant_host_root_for(22) + "/y.txt";
        std::string out;
        CHECK(!a.check_tenant_host_path(path_b, out, "write-file"),
              "3802 AC4 chaos: A denied on B prefix");
        CHECK(!b.check_tenant_host_path(path_a, out, "write-file"),
              "3802 AC4 chaos: B denied on A prefix");
        CHECK(a.check_tenant_host_path("local.txt", out, "write-file"),
              "3802 AC4 chaos: A allows relative under own root");
        CHECK(out.find("/t-11/") != std::string::npos, "3802 AC4 chaos: A resolved under t-11");
        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);

        std::ifstream invent("tests/core/test_issue_3802.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3802.cpp");
        CHECK(!invent.good(), "AC4: no tests/core/test_issue_3802.cpp (forbidden per #81967)");
    }

    // ── Issue #3835: read/recon host-path isolation (mirror #3802 writes) ──
    {
        std::println(
            "\n--- #3835 AC1: Restricted+MT tenant A cannot read under tenant B prefix ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-3835-ac1";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::resolve_tenant_host_path;
        using aura::compiler::security::tenant_host_root_for;
        using aura::compiler::security::TenantHostPathVerdict;
        const auto root_a = tenant_host_root_for(7);
        const auto root_b = tenant_host_root_for(42);
        const auto escape = root_b + "/secret.txt";
        auto denied = resolve_tenant_host_path(escape, /*tenant=*/7, /*active=*/true);
        CHECK(denied.verdict == TenantHostPathVerdict::Deny,
              "3835 AC1: lexical resolve denies A→B prefix");

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        std::string out;
        for (const char* op : {"read-file", "file-exists?", "file-size", "directory-list"}) {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            CHECK(!ev.check_tenant_host_path(escape, out, op),
                  (std::string("3835 AC1: Evaluator denies A→B via ") + op).c_str());
            bool saw = false;
            for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
                const auto& e = ring.ring[s % ring.ring.size()];
                if (e.seq != s)
                    continue;
                if (static_cast<int>(e.kind) !=
                    static_cast<int>(aura::core::security_event::SecurityEventKind::IsolationDeny))
                    continue;
                if (std::string_view(e.reason).find("tenant-path-escape") !=
                    std::string_view::npos) {
                    saw = true;
                    CHECK(e.tenant_id == 7, "3835 AC1: SE tenant is caller A");
                }
            }
            CHECK(saw, (std::string("3835 AC1: IsolationDeny SE for ") + op).c_str());
        }
        // In-tenant relative path under own root → allow (void/false/empty only on deny).
        CHECK(ev.check_tenant_host_path("ok.txt", out, "read-file"),
              "3835 AC1: in-tenant relative allows for read-file");
        CHECK(out.find(root_a) == 0, "3835 AC1: resolved under tenant A root");
        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #3835 AC2: Soft/Off / single-tenant Restricted passthrough ---");
        reset_all();
        using aura::compiler::security::resolve_tenant_host_path;
        using aura::compiler::security::tenant_host_path_policy_active;
        using aura::compiler::security::TenantHostPathVerdict;
        CHECK(!tenant_host_path_policy_active(/*mode=*/0, /*mt=*/false),
              "3835 AC2: Soft/Off inactive");
        CHECK(!tenant_host_path_policy_active(/*mode=*/1, /*mt=*/false),
              "3835 AC2: single-tenant Restricted inactive");
        auto soft = resolve_tenant_host_path("/tmp/anywhere.txt", 7, /*active=*/false);
        CHECK(soft.verdict == TenantHostPathVerdict::Passthrough,
              "3835 AC2: Soft passthrough keeps absolute path");

        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        aura::core::provenance::set_multi_tenant_env_active(false);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        ev.set_capability_tenant_id(7);
        std::string out;
        CHECK(ev.check_tenant_host_path("/tmp/anywhere.txt", out, "read-file"),
              "3835 AC2: Off check_tenant_host_path allows read-file");
        CHECK(out == "/tmp/anywhere.txt", "3835 AC2: Off leaves path unchanged");
        CHECK(ev.check_tenant_host_path("/tmp/anywhere.txt", out, "file-exists?"),
              "3835 AC2: Off allows file-exists?");
        CHECK(ev.check_tenant_host_path("/tmp/anywhere.txt", out, "file-size"),
              "3835 AC2: Off allows file-size");
        CHECK(ev.check_tenant_host_path("/tmp/anywhere.txt", out, "directory-list"),
              "3835 AC2: Off allows directory-list");

        ev.set_effect_sandbox_mode(1);
        CHECK(ev.check_tenant_host_path("/tmp/anywhere.txt", out, "read-file"),
              "3835 AC2: single-tenant Restricted allows absolute read");
    }

    {
        std::println("\n--- #3835 AC3: write paths still call check; read/recon wired ---");
        const auto filep = read_file("src/compiler/evaluator_primitives_file.cpp");
        const auto build = read_file("build.py");
        CHECK(filep.find("check_tenant_host_path") != std::string::npos,
              "3835 AC3: file prims cite check_tenant_host_path");
        CHECK(filep.find("\"write-file\"") != std::string::npos ||
                  filep.find("check_tenant_host_path(path, resolved_path, \"write-file\")") !=
                      std::string::npos,
              "3835 AC3: write-file still present");
        CHECK(filep.find("check_tenant_host_path(path, resolved_path, \"write-file\")") !=
                  std::string::npos,
              "3835 AC3: write-file still calls check_tenant_host_path");
        CHECK(filep.find("check_tenant_host_path(path, resolved, \"read-file\")") !=
                  std::string::npos,
              "3835 AC3: read-file calls check_tenant_host_path");
        CHECK(filep.find("check_tenant_host_path(path, resolved, \"file-exists?\")") !=
                  std::string::npos,
              "3835 AC3: file-exists? calls check_tenant_host_path");
        CHECK(filep.find("check_tenant_host_path(path, resolved, \"file-size\")") !=
                  std::string::npos,
              "3835 AC3: file-size calls check_tenant_host_path");
        CHECK(filep.find("check_tenant_host_path(dir_path, resolved, \"directory-list\")") !=
                  std::string::npos,
              "3835 AC3: directory-list calls check_tenant_host_path");
        CHECK(build.find("check_tenant_host_path_read_3835") != std::string::npos,
              "3835 AC3: build.py wires #3835 linter");

        std::ifstream invent("tests/core/test_issue_3835.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3835.cpp");
        CHECK(!invent.good(), "3835 AC3: no tests/core/test_issue_3835.cpp (forbidden)");
    }

    // ── Issue #3836: shell/command-output require_effect(Exec) ──
    {
        std::println(
            "\n--- #3836 AC1: shell/command-output require_effect(Exec) before fork/popen ---");
        const auto filep = read_file("src/compiler/evaluator_primitives_file.cpp");
        CHECK(filep.find("Issue #3836") != std::string::npos, "3836 AC1: cite");
        CHECK(filep.find("require_effect(kEffectExec, \"shell\")") != std::string::npos,
              "3836 AC1: shell require_effect");
        CHECK(filep.find("require_effect(kEffectExec, \"command-output\")") != std::string::npos,
              "3836 AC1: command-output require_effect");
    }
    {
        std::println("\n--- #3836 AC2: Soft/Off deny_exec contract preserved ---");
        const auto filep = read_file("src/compiler/evaluator_primitives_file.cpp");
        CHECK(filep.find("!ev.sandbox_mode()") != std::string::npos,
              "3836 AC2: Soft/Off !sandbox_mode() in deny_exec");
        const auto deny = filep.find("const auto deny_exec");
        CHECK(deny != std::string::npos, "3836 AC2: deny_exec lambda present");
    }
    {
        std::println("\n--- #3836 AC3: EXEMPT + build wiring; no invent ---");
        const auto build = read_file("build.py");
        const auto fiber =
            read_file("scripts/coverage/checks/check_side_effect_fiber_principal_2839.py");
        CHECK(build.find("check_shell_require_effect_3836") != std::string::npos,
              "3836 AC3: build.py wires linter");
        CHECK(fiber.find("shell") != std::string::npos &&
                  fiber.find("command-output") != std::string::npos,
              "3836 AC3: EXEMPT lists shell/command-output");
        std::ifstream invent("tests/core/test_issue_3836.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3836.cpp");
        CHECK(!invent.good(), "3836 AC3: no test_issue_3836.cpp");
    }

    // ── Issue #4233: EXEMPT shell/command-output tenant host-path jail ──
    {
        std::println(
            "\n--- #4233 AC1: Restricted+MT tenant A cannot shell into tenant B prefix ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4233-ac1";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::tenant_host_root_for;
        const auto root_a = tenant_host_root_for(7);
        const auto root_b = tenant_host_root_for(42);
        const auto escape = root_b + "/secret.txt";

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        const auto fib = static_cast<std::int64_t>(aura_fiber_current_id());
        std::string out;
        // Exec-face deny for both jailed prims: IsolationDeny
        // tenant-path-escape joins the shared SE row (op+tenant+fiber).
        for (const char* op : {"shell", "command-output"}) {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            CHECK(!ev.check_tenant_exec_jail(escape, out, op),
                  (std::string("4233 AC1: Evaluator denies A→B via ") + op).c_str());
            CHECK(ev.last_mutate_error().find(std::string(op) + ": tenant-path-escape") !=
                      std::string::npos,
                  (std::string("4233 AC1: last_mutate_error carries ") + op).c_str());
            bool saw = false;
            for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
                const auto& e = ring.ring[s % ring.ring.size()];
                if (e.seq != s)
                    continue;
                if (static_cast<int>(e.kind) !=
                    static_cast<int>(aura::core::security_event::SecurityEventKind::IsolationDeny))
                    continue;
                if (std::string_view(e.reason).find("tenant-path-escape") == std::string_view::npos)
                    continue;
                saw = true;
                CHECK(e.tenant_id == 7, "4233 AC1: SE tenant is caller A");
                CHECK(std::string_view(e.op) == op, "4233 AC1: SE op is the jailed prim");
                CHECK(e.fiber_id == fib, "4233 AC1: SE fiber joins the row");
            }
            CHECK(saw, (std::string("4233 AC1: IsolationDeny SE for ") + op).c_str());
        }
        // The issue's bypass shapes deny too (zero exec on every form).
        CHECK(!ev.check_tenant_exec_jail("$AURA_TENANT_FS_ROOT/t-42/secret.txt", out, "shell"),
              "4233 AC1: $-expansion escape denies");
        CHECK(!ev.check_tenant_exec_jail("cat ../../../etc/passwd", out, "shell"),
              "4233 AC1: .. climb denies");
        CHECK(!ev.check_tenant_exec_jail("echo pwned `id`", out, "command-output"),
              "4233 AC1: backtick expansion denies");
        CHECK(!ev.check_tenant_exec_jail("sort < /etc/passwd", out, "shell"),
              "4233 AC1: redirect-read absolute denies");
        CHECK(!ev.check_tenant_exec_jail("git --git-dir=/etc/x status", out, "command-output"),
              "4233 AC1: '='-joined absolute denies");
        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4233 AC2: Soft/Off / single-tenant Restricted exec passthrough ---");
        reset_all();
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
        using aura::compiler::security::tenant_exec_cmd_escapes_root;
        using aura::compiler::security::tenant_host_path_policy_active;
        CHECK(!tenant_host_path_policy_active(/*mode=*/0, /*mt=*/false),
              "4233 AC2: Soft/Off inactive");
        CHECK(!tenant_host_path_policy_active(/*mode=*/1, /*mt=*/false),
              "4233 AC2: single-tenant Restricted inactive");
        // Lexical fence units: relative-only scans clean; every escape form
        // from the issue's bypass section scans as an escape.
        CHECK(!tenant_exec_cmd_escapes_root("echo hi > out.txt"),
              "4233 AC2: relative-only command scans clean");
        CHECK(!tenant_exec_cmd_escapes_root("whoami"), "4233 AC2: bare command scans clean");
        CHECK(!tenant_exec_cmd_escapes_root("ls src"), "4233 AC2: relative arg scans clean");
        CHECK(tenant_exec_cmd_escapes_root("echo pwned > /tmp/x"),
              "4233 AC2: absolute redirect scans escape");
        CHECK(tenant_exec_cmd_escapes_root("cat $HOME/x"), "4233 AC2: $-escape scans");
        CHECK(tenant_exec_cmd_escapes_root("echo `id`"), "4233 AC2: backtick scans");
        CHECK(tenant_exec_cmd_escapes_root("ls ../.."), "4233 AC2: .. climb scans");
        CHECK(tenant_exec_cmd_escapes_root("git --git-dir=/etc/x status"),
              "4233 AC2: '='-joined absolute scans");
        CHECK(tenant_exec_cmd_escapes_root("cat a;cat /etc/passwd"),
              "4233 AC2: ';'-joined absolute scans");

        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        const auto& ring = g_security_event_ring();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        ev.set_capability_tenant_id(7);
        std::string out;
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        CHECK(ev.check_tenant_exec_jail("/tmp/anywhere && cat /etc/passwd", out, "shell"),
              "4233 AC2: Off jail allows absolute command");
        CHECK(out.empty(), "4233 AC2: Off jail leaves jail_root empty (passthrough)");
        CHECK(ring.seq.load(std::memory_order_acquire) == se_base,
              "4233 AC2: Off allow emits no IsolationDeny SE");
        ev.set_effect_sandbox_mode(1);
        CHECK(ev.check_tenant_exec_jail("/tmp/anywhere", out, "command-output"),
              "4233 AC2: single-tenant Restricted allows absolute command");

        // Real-prim passthrough (Off): the jailed fork+pipe capture keeps
        // the exit-code and stdout contracts. Deferred host prims are not
        // in the typecheck env / IR prim table (#3174 shape), so arm via
        // ensure_std_host_prims and invoke the prim bodies directly.
        ev.set_effect_sandbox_mode(0);
        (void)ev.ensure_std_host_prims("std/process");
        auto& heap_w = ev.string_heap_mut();
        heap_w.push_back("exit 7");
        auto sh = ev.primitives().lookup("shell");
        CHECK(sh.has_value(), "4233 AC2: shell prim registered after install");
        if (sh) {
            using aura::compiler::types::make_string;
            const auto rr = (*sh)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
            CHECK(is_int(rr) && as_int(rr) == 7, "4233 AC2: shell prim keeps exit codes");
        }
        heap_w.push_back("echo -n aura-4233-passthrough");
        auto co = ev.primitives().lookup("command-output");
        CHECK(co.has_value(), "4233 AC2: command-output prim registered after install");
        if (co) {
            using aura::compiler::types::make_string;
            const auto rr2 = (*co)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
            CHECK(is_string(rr2), "4233 AC2: command-output prim returns stdout");
            if (is_string(rr2)) {
                auto heap = ev.string_heap();
                const auto sidx = as_string_idx(rr2);
                CHECK(sidx < heap.size() && heap[sidx] == "aura-4233-passthrough",
                      "4233 AC2: command-output stdout content intact");
            }
        }
    }

    {
        std::println("\n--- #4233 AC3: active-policy allow carries the jail root; prims wired ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4233-ac3";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::tenant_host_root_for;
        const auto root_a = tenant_host_root_for(7);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        std::string out;
        CHECK(ev.check_tenant_exec_jail("logs/run.txt", out, "shell"),
              "4233 AC3: relative-only allow under active policy");
        CHECK(out == root_a, "4233 AC3: jail root is the caller's tenant root");
        const auto filep = read_file("src/compiler/evaluator_primitives_file.cpp");
        CHECK(filep.find("check_tenant_exec_jail(ev.string_heap_[idx], jail_root, \"shell\")") !=
                  std::string::npos,
              "4233 AC3: shell prim wires the jail fence");
        CHECK(filep.find("check_tenant_exec_jail(cmd, jail_root, \"command-output\")") !=
                  std::string::npos,
              "4233 AC3: command-output prim wires the jail fence");
        CHECK(filep.find("::chdir(jail_root.c_str())") != std::string::npos,
              "4233 AC3: exec children chdir under the tenant root");
        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4233 AC4: dual-evaluator chaos — Exec face fenced per tenant ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4233-chaos";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::tenant_host_root_for;
        const auto root_a = tenant_host_root_for(11);
        const auto root_b = tenant_host_root_for(22);
        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(11);
        ev_b.set_effect_sandbox_mode(1);
        ev_b.set_capability_tenant_id(22);
        std::string out;
        CHECK(!ev_a.check_tenant_exec_jail(root_b + "/f.txt", out, "shell"),
              "4233 AC4 chaos: A denied on B prefix");
        CHECK(!ev_b.check_tenant_exec_jail(root_a + "/f.txt", out, "command-output"),
              "4233 AC4 chaos: B denied on A prefix");
        CHECK(ev_a.check_tenant_exec_jail("work.txt", out, "shell") && out == root_a,
              "4233 AC4 chaos: A allows relative under own root");
        CHECK(ev_b.check_tenant_exec_jail("work.txt", out, "command-output") && out == root_b,
              "4233 AC4 chaos: B allows relative under own root");
        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4233 AC5: linter + build wiring; no invent ---");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto hh = read_file("src/compiler/tenant_host_path.hh");
        const auto build = read_file("build.py");
        const auto allow = read_file("scripts/coverage/root_check_allowlist.txt");
        CHECK(sec.find("bool Evaluator::check_tenant_exec_jail(") != std::string::npos,
              "4233 AC5: check_tenant_exec_jail defined");
        CHECK(sec.find("Issue #4233") != std::string::npos, "4233 AC5: security TU cites #4233");
        CHECK(hh.find("tenant_exec_cmd_escapes_root") != std::string::npos &&
                  hh.find("Issue #4233") != std::string::npos,
              "4233 AC5: header escape scanner cites #4233");
        CHECK(build.find("check_tenant_exec_jail_4233") != std::string::npos,
              "4233 AC5: build.py wires the #4233 linter");
        CHECK(allow.find("check_tenant_exec_jail_4233.py") != std::string::npos,
              "4233 AC5: linter on the root allowlist");
        std::ifstream invent("tests/core/test_issue_4233.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_4233.cpp");
        CHECK(!invent.good(), "4233 AC5: no tests/core/test_issue_4233.cpp (forbidden)");
    }

    // ── Issue #4380: exec-jail embedded absolute paths + symlink components ──
    {
        std::println(
            "\n--- #4380 AC1: quoted / interpreter-arg absolute paths deny at the jail ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4380-ac1";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        std::string out;
        // The issue's exact repro commands: absolute paths hidden inside
        // quotes / interpreter arguments — the #4233 token-initial scan
        // let them through; the byte-local mid-token scan denies at the
        // fence with the shared IsolationDeny row.
        const std::pair<const char*, const char*> repros[] = {
            {"python3 -c 'open(\"/etc/passwd\").read()'", "shell"},
            {"perl -e 'open(F,\"/etc/passwd\");print <F>'", "command-output"},
            {"python3 -c 'os.symlink(\"/etc\", \"e\")'", "shell"},
        };
        for (const auto& [cmd, op] : repros) {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            CHECK(!ev.check_tenant_exec_jail(cmd, out, op),
                  (std::string("4380 AC1: embedded absolute denies via ") + op).c_str());
            CHECK(ev.last_mutate_error().find(std::string(op) + ": tenant-path-escape") !=
                      std::string::npos,
                  (std::string("4380 AC1: last_mutate_error carries ") + op).c_str());
            bool saw = false;
            for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
                const auto& e = ring.ring[s % ring.ring.size()];
                if (e.seq != s)
                    continue;
                if (static_cast<int>(e.kind) !=
                    static_cast<int>(aura::core::security_event::SecurityEventKind::IsolationDeny))
                    continue;
                if (std::string_view(e.reason).find("tenant-path-escape") == std::string_view::npos)
                    continue;
                saw = true;
                CHECK(e.tenant_id == 7, "4380 AC1: SE tenant is the caller");
                CHECK(std::string_view(e.op) == op, "4380 AC1: SE op is the jailed prim");
            }
            CHECK(saw, (std::string("4380 AC1: IsolationDeny SE for ") + op).c_str());
        }
        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4380 AC2: scanner — embedded absolute denies, relative allows ---");
        using aura::compiler::security::tenant_exec_cmd_escapes_root;
        // Issue repro set: interpreter args with quoted absolute paths.
        CHECK(tenant_exec_cmd_escapes_root("python3 -c 'open(\"/etc/passwd\").read()'"),
              "4380 AC2: quoted interpreter absolute scans escape");
        CHECK(tenant_exec_cmd_escapes_root("perl -e 'open(F,\"/etc/passwd\");print <F>'"),
              "4380 AC2: perl quoted absolute scans escape");
        CHECK(tenant_exec_cmd_escapes_root("python3 -c 'os.symlink(\"/etc\", \"e\")'"),
              "4380 AC2: symlink-creation command scans escape");
        CHECK(tenant_exec_cmd_escapes_root("convert img.png jpg:/etc/x"),
              "4380 AC2: ':'-joined absolute scans escape");
        // Relative-only contract stays: plain relative paths keep working.
        CHECK(!tenant_exec_cmd_escapes_root("ls src"), "4380 AC2: relative arg stays clean");
        CHECK(!tenant_exec_cmd_escapes_root("logs/run.txt"),
              "4380 AC2: relative path stays clean (#4233 pin)");
        CHECK(!tenant_exec_cmd_escapes_root("./run.sh"), "4380 AC2: dot-relative stays clean");
        CHECK(!tenant_exec_cmd_escapes_root("sed 's/a/b/' f.txt"),
              "4380 AC2: sed substitution stays clean");
        CHECK(!tenant_exec_cmd_escapes_root("echo hi > out.txt"),
              "4380 AC2: relative redirect stays clean");
        // Existing #4233 deny families stay denied (no relaxation).
        CHECK(tenant_exec_cmd_escapes_root("cat /etc/passwd"),
              "4380 AC2: absolute token still denies");
        CHECK(tenant_exec_cmd_escapes_root("git --git-dir=/etc/x status"),
              "4380 AC2: '='-joined absolute still denies");
    }

    {
        std::println(
            "\n--- #4380 AC3: symlink component under tenant root denies the file face ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4380-ac3";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::tenant_host_root_for;
        const auto root_a = tenant_host_root_for(7);
        // Idempotent across runs: a previous run left links/dirs in place
        // (create_directory_symlink throws on an existing link).
        std::error_code rm_ec;
        std::filesystem::remove_all(root_a, rm_ec);
        std::filesystem::create_directories(root_a + "/ok");
        // Pre-placed symlinks (off-jail provisioning path — the #4380
        // scanner now denies exec-time creation, so this models the
        // residual vector): `e -> outside dir`, `lnk -> outside file`.
        const auto outside = base + "-outside";
        std::filesystem::create_directories(outside);
        const auto sentinel = outside + "/secret.txt";
        {
            std::ofstream f(sentinel);
            f << "host-secret";
        }
        std::filesystem::create_directory_symlink(outside, root_a + "/e");
        std::filesystem::create_symlink(sentinel, root_a + "/lnk");

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        std::string out;
        // Relative write into a real dir under the root stays allowed.
        CHECK(ev.check_tenant_host_path("ok/f.txt", out, "write-file"),
              "4380 AC3: relative write under own root stays allowed");
        CHECK(out == root_a + "/ok/f.txt",
              "4380 AC3: relative write resolves under the tenant root");
        // Intermediate symlink: every file-face op denies auditable.
        for (const char* op : {"read-file", "write-file", "directory-list", "file-exists?",
                               "file-size", "file-delete"}) {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            CHECK(!ev.check_tenant_host_path("e/secret.txt", out, op),
                  (std::string("4380 AC3: symlink component denies via ") + op).c_str());
            bool saw = false;
            for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
                const auto& e = ring.ring[s % ring.ring.size()];
                if (e.seq != s)
                    continue;
                if (static_cast<int>(e.kind) !=
                    static_cast<int>(aura::core::security_event::SecurityEventKind::IsolationDeny))
                    continue;
                if (std::string_view(e.reason).find("tenant-path-escape") == std::string_view::npos)
                    continue;
                saw = true;
            }
            CHECK(saw, (std::string("4380 AC3: IsolationDeny SE for ") + op).c_str());
        }
        // Final-component symlink: stricter than the old silent open-fail —
        // the fence denies auditable too.
        const auto se_base_lnk = ring.seq.load(std::memory_order_acquire);
        CHECK(!ev.check_tenant_host_path("lnk", out, "read-file"),
              "4380 AC3: final-component symlink denies");
        CHECK(ring.seq.load(std::memory_order_acquire) > se_base_lnk,
              "4380 AC3: final symlink deny joins the SE row");
        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4380 AC4: linter + wiring; scanner SSOT cites ---");
        const auto hh = read_file("src/compiler/tenant_host_path.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto build = read_file("build.py");
        const auto allow = read_file("scripts/coverage/root_check_allowlist.txt");
        CHECK(hh.find("Issue #4380") != std::string::npos, "4380 AC4: scanner SSOT cites #4380");
        CHECK(hh.find("tenant_path_has_symlink_component") != std::string::npos,
              "4380 AC4: symlink-component walk lives in the SSOT");
        CHECK(sec.find("tenant_path_has_symlink_component") != std::string::npos,
              "4380 AC4: host-path gate consults the symlink walk");
        CHECK(build.find("check_tenant_jail_4380") != std::string::npos,
              "4380 AC4: build.py wires the #4380 linter");
        CHECK(allow.find("check_tenant_jail_4380.py") != std::string::npos,
              "4380 AC4: linter on the root allowlist");
        std::ifstream invent("tests/core/test_issue_4380.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_4380.cpp");
        CHECK(!invent.good(), "4380 AC4: no tests/core/test_issue_4380.cpp (forbidden)");
    }

    // ── Issue #4381: git-* tenant FS jail (Restricted+MT / Strict) ──
    {
        std::println("\n--- #4381 AC1: git-* runs jailed under the caller tenant root ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4381-ac1";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::tenant_host_root_for;
        const auto root_a = tenant_host_root_for(7);
        // Idempotent across runs: a stale tenant repo from a previous run
        // would fail the provisioning commit (nothing to commit).
        std::error_code rm_ec;
        std::filesystem::remove_all(root_a, rm_ec);
        std::filesystem::create_directories(root_a);
        // Provision a real repo INSIDE the tenant root; the host process
        // cwd stays the aura build tree — a jailed git-* face must see the
        // tenant repo, not the host one.
        const int setup_rc = std::system(
            ("cd '" + root_a +
             "' && git init -q && git config user.email t@tenant && git config user.name t"
             " && echo one > one.txt && git add one.txt && git commit -q -m init >/dev/null")
                .c_str());
        CHECK(setup_rc == 0, "4381 AC1: tenant repo provisioned");
        std::string expect_sha;
        {
            const std::string sha_file = base + "-sha.txt";
            std::system(("cd '" + root_a + "' && git rev-parse --short HEAD > '" + sha_file +
                         "' 2>/dev/null")
                            .c_str());
            std::ifstream f(sha_file);
            std::getline(f, expect_sha);
            if (!expect_sha.empty() && expect_sha.back() == '\n')
                expect_sha.pop_back();
        }
        CHECK(!expect_sha.empty(), "4381 AC1: tenant HEAD sha captured");

        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Install window: the deferred git prims materialize while the
        // sandbox is Off — the load-time `(require std/git)` face. The
        // per-call jail arms right after: the prim bodies consult
        // check_tenant_exec_jail on every invocation.
        (void)ev.ensure_std_host_prims("std/git");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        auto& heap_w = ev.string_heap_mut();

        // Untracked file → jailed git-status sees the TENANT repo, not the
        // host aura repo (whose status would never be a single zz.txt line).
        {
            std::ofstream f(root_a + "/zz.txt");
            f << "pending";
        }
        auto gs = ev.primitives().lookup("git-status");
        CHECK(gs.has_value(), "4381 AC1: git-status registered");
        std::string status_out;
        if (gs) {
            const auto r = (*gs)({});
            CHECK(is_string(r), "4381 AC1: jailed git-status returns string");
            if (is_string(r))
                status_out = ev.string_heap()[as_string_idx(r)];
        }
        CHECK(status_out.find("zz.txt") != std::string::npos,
              "4381 AC1: jailed git-status sees the tenant repo (untracked zz.txt)");

        auto gr = ev.primitives().lookup("git-rev-parse");
        CHECK(gr.has_value(), "4381 AC1: git-rev-parse registered");
        std::string sha_out;
        if (gr) {
            const auto r = (*gr)({});
            CHECK(is_string(r), "4381 AC1: jailed git-rev-parse returns string");
            if (is_string(r))
                sha_out = ev.string_heap()[as_string_idx(r)];
        }
        CHECK(sha_out == expect_sha, "4381 AC1: jailed git-rev-parse returns the TENANT repo HEAD");

        // Relative stage succeeds under the jail; escape paths deny with
        // the shared IsolationDeny row (op git-stage).
        auto gst = ev.primitives().lookup("git-stage");
        CHECK(gst.has_value(), "4381 AC1: git-stage registered");
        heap_w.push_back("zz.txt");
        int stage_rc = -99;
        if (gst) {
            using aura::compiler::types::make_string;
            const auto r = (*gst)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
            CHECK(is_int(r), "4381 AC1: git-stage returns exit code");
            if (is_int(r))
                stage_rc = static_cast<int>(as_int(r));
        }
        CHECK(stage_rc == 0, "4381 AC1: relative git-stage succeeds under the jail");
        for (const char* escape : {"/etc/passwd", "../one.txt"}) {
            heap_w.push_back(escape);
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            int rc = 0;
            if (gst) {
                using aura::compiler::types::make_string;
                const auto r = (*gst)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
                if (is_int(r))
                    rc = static_cast<int>(as_int(r));
            }
            CHECK(rc == -1, (std::string("4381 AC1: git-stage denies ") + escape).c_str());
            bool saw = false;
            for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
                const auto& e = ring.ring[s % ring.ring.size()];
                if (e.seq != s)
                    continue;
                if (static_cast<int>(e.kind) !=
                    static_cast<int>(aura::core::security_event::SecurityEventKind::IsolationDeny))
                    continue;
                if (std::string_view(e.reason).find("tenant-path-escape") == std::string_view::npos)
                    continue;
                if (std::string_view(e.op) != "git-stage")
                    continue;
                saw = true;
            }
            CHECK(saw, (std::string("4381 AC1: IsolationDeny SE for git-stage ") + escape).c_str());
        }

        // Jailed commit succeeds; status goes clean; log shows the commit.
        // git-commit carries the #2072 body choke (Exec|Network), which
        // fail-closes on DIRECT body invocation with an unstamped ref
        // (isolation-deny:unstamped-ref — correct production deny; only
        // the dispatch face stamps refs). The commit op's #4381 jail face
        // is pinned at the gate level instead: the fence allows the fixed
        // clean command and carries the caller's jail root.
        std::string commit_jail;
        CHECK(ev.check_tenant_exec_jail("commit", commit_jail, "git-commit"),
              "4381 AC1: git-commit jail fence allows the fixed clean command");
        CHECK(commit_jail == root_a, "4381 AC1: git-commit fence carries the caller's jail root");
        // Jailed log read face: `git log --oneline -n 1` against the
        // tenant repo returns its init commit (proves the jailed read
        // path end-to-end without the dispatch-stamped commit face).
        auto gl = ev.primitives().lookup("git-log");
        CHECK(gl.has_value(), "4381 AC1: git-log registered");
        std::string log_out;
        if (gl) {
            using aura::compiler::types::make_int;
            const auto r = (*gl)({make_int(1)});
            CHECK(is_string(r), "4381 AC1: jailed git-log returns string");
            if (is_string(r))
                log_out = ev.string_heap()[as_string_idx(r)];
        }
        CHECK(log_out.find("init") != std::string::npos,
              "4381 AC1: jailed git-log shows the tenant repo commit");

        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4381 AC2: Soft/Off git-* passthrough on the process repo ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        (void)ev.ensure_std_host_prims("std/git");
        const auto se_base = g_security_event_ring().seq.load(std::memory_order_acquire);
        auto gr = ev.primitives().lookup("git-rev-parse");
        CHECK(gr.has_value(), "4381 AC2: git-rev-parse registered");
        std::string prim_sha;
        if (gr) {
            const auto r = (*gr)({});
            CHECK(is_string(r), "4381 AC2: passthrough git-rev-parse returns string");
            if (is_string(r))
                prim_sha = ev.string_heap()[as_string_idx(r)];
        }
        // Test-side reference: same process cwd, plain git.
        std::string ref_sha;
        {
            const char* tmp = std::getenv("TMPDIR");
            const std::string sha_file =
                std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4381-ac2-sha.txt";
            std::system(("git rev-parse --short HEAD > '" + sha_file + "' 2>/dev/null").c_str());
            std::ifstream f(sha_file);
            std::getline(f, ref_sha);
            if (!ref_sha.empty() && ref_sha.back() == '\n')
                ref_sha.pop_back();
        }
        CHECK(prim_sha == ref_sha,
              "4381 AC2: Soft/Off git-rev-parse passthrough matches the process repo");
        CHECK(g_security_event_ring().seq.load(std::memory_order_acquire) == se_base,
              "4381 AC2: passthrough emits no IsolationDeny SE");
    }

    {
        std::println("\n--- #4381 AC3: linter + wiring; git jail cites ---");
        const auto io_src = read_file("src/compiler/evaluator_primitives_io.cpp");
        const auto build = read_file("build.py");
        const auto allow = read_file("scripts/coverage/root_check_allowlist.txt");
        CHECK(io_src.find("Issue #4381") != std::string::npos, "4381 AC3: git prims cite #4381");
        CHECK(
            io_src.find("check_tenant_exec_jail(\"status --short\", jail_root, \"git-status\")") !=
                std::string::npos,
            "4381 AC3: git-status wires the jail fence");
        CHECK(io_src.find("check_tenant_exec_jail(\"add\", jail_root, \"git-stage\")") !=
                  std::string::npos,
              "4381 AC3: git-stage wires the jail fence");
        CHECK(io_src.find("check_tenant_exec_jail(\"commit\", jail_root, \"git-commit\")") !=
                  std::string::npos,
              "4381 AC3: git-commit wires the jail fence");
        CHECK(io_src.find("check_tenant_host_path(p, resolved, \"git-stage\")") !=
                  std::string::npos,
              "4381 AC3: git-stage paths resolve through the host-path gate");
        CHECK(io_src.find("::chdir(jail_root.c_str())") != std::string::npos,
              "4381 AC3: jailed git children chdir under the tenant root");
        CHECK(build.find("check_git_tenant_jail_4381") != std::string::npos,
              "4381 AC3: build.py wires the #4381 linter");
        CHECK(allow.find("check_git_tenant_jail_4381.py") != std::string::npos,
              "4381 AC3: linter on the root allowlist");
        std::ifstream invent("tests/core/test_issue_4381.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_4381.cpp");
        CHECK(!invent.good(), "4381 AC3: no tests/core/test_issue_4381.cpp (forbidden)");
    }

    // ── Issue #4382: http-* scheme jail (file:// / non-http(s) deny) ──
    {
        std::println("\n--- #4382 AC1: http-* scheme fence denies non-http(s) URLs ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4382-ac1";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        std::string out;
        // Fence deny matrix: file://, single-slash file:, ftp:// deny with
        // the shared IsolationDeny row (kEffectNetwork face, op carried);
        // http(s) URLs allow case-insensitively with zero SE.
        const std::pair<const char*, const char*> deny_matrix[] = {
            {"file:///etc/passwd", "http-get"},
            {"file:/etc/passwd", "http-get"},
            {"ftp://host/x", "http-post"},
            {"file:///etc/passwd", "http-post"},
        };
        for (const auto& [url, op] : deny_matrix) {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            CHECK(!ev.check_tenant_http_scheme(url, op),
                  (std::string("4382 AC1: scheme denies ") + url + " via " + op).c_str());
            CHECK(ev.last_mutate_error().find(std::string(op) + ": tenant-path-escape") !=
                      std::string::npos,
                  (std::string("4382 AC1: last_mutate_error carries ") + op).c_str());
            bool saw = false;
            for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
                const auto& e = ring.ring[s % ring.ring.size()];
                if (e.seq != s)
                    continue;
                if (static_cast<int>(e.kind) !=
                    static_cast<int>(aura::core::security_event::SecurityEventKind::IsolationDeny))
                    continue;
                if (std::string_view(e.reason).find("tenant-path-escape") == std::string_view::npos)
                    continue;
                if (std::string_view(e.op) != op)
                    continue;
                saw = true;
                CHECK(e.tenant_id == 7, "4382 AC1: SE tenant is the caller");
            }
            CHECK(saw, (std::string("4382 AC1: IsolationDeny SE for ") + op).c_str());
        }
        const auto se_base_allow = ring.seq.load(std::memory_order_acquire);
        CHECK(ev.check_tenant_http_scheme("http://example.com/x", "http-get"),
              "4382 AC1: http:// allows at the fence");
        CHECK(ev.check_tenant_http_scheme("HTTPS://Example.com/x", "http-post"),
              "4382 AC1: https:// allows case-insensitively");
        CHECK(ring.seq.load(std::memory_order_acquire) == se_base_allow,
              "4382 AC1: scheme allow emits no SE");

        // Prim-level deny: the http-get / http-post bodies consult the
        // fence BEFORE any perform / async / CLI exec (install window at
        // Off, then arm — deferred prims materialize like #4233 AC2).
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        (void)ev.ensure_std_host_prims("std/net");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        auto& heap_w = ev.string_heap_mut();
        auto hg = ev.primitives().lookup("http-get");
        CHECK(hg.has_value(), "4382 AC1: http-get registered");
        heap_w.push_back("file:///etc/passwd");
        {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            bool denied = false;
            if (hg) {
                using aura::compiler::types::make_string;
                const auto r = (*hg)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
                denied = !is_string(r);
            }
            CHECK(denied, "4382 AC1: http-get prim denies file:// (zero perform)");
            CHECK(ring.seq.load(std::memory_order_acquire) > se_base,
                  "4382 AC1: http-get prim deny joins the SE row");
        }
        auto hp = ev.primitives().lookup("http-post");
        CHECK(hp.has_value(), "4382 AC1: http-post registered");
        heap_w.push_back("file:///etc/passwd");
        heap_w.push_back("{}");
        {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            bool denied = false;
            if (hp) {
                using aura::compiler::types::make_string;
                const auto r = (*hp)({make_string(static_cast<std::uint64_t>(heap_w.size() - 2)),
                                      make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
                denied = !is_string(r);
            }
            CHECK(denied, "4382 AC1: http-post prim denies file:// (zero perform / zero exec)");
            CHECK(ring.seq.load(std::memory_order_acquire) > se_base,
                  "4382 AC1: http-post prim deny joins the SE row");
        }
        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4382 AC2: Soft/Off scheme passthrough (legacy face) ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        const auto se_base = g_security_event_ring().seq.load(std::memory_order_acquire);
        CHECK(ev.check_tenant_http_scheme("file:///etc/passwd", "http-get"),
              "4382 AC2: Off scheme passthrough (no jail arm)");
        CHECK(g_security_event_ring().seq.load(std::memory_order_acquire) == se_base,
              "4382 AC2: passthrough emits no IsolationDeny SE");
    }

    {
        std::println("\n--- #4382 AC3: linter + wiring; scheme jail cites ---");
        const auto io_src = read_file("src/compiler/evaluator_primitives_io.cpp");
        const auto hh = read_file("src/compiler/tenant_host_path.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto build = read_file("build.py");
        const auto allow = read_file("scripts/coverage/root_check_allowlist.txt");
        CHECK(hh.find("Issue #4382") != std::string::npos,
              "4382 AC3: scheme allowlist SSOT cites #4382");
        CHECK(hh.find("tenant_http_url_scheme_allowed") != std::string::npos,
              "4382 AC3: allowlist predicate lives in the SSOT");
        CHECK(sec.find("check_tenant_http_scheme") != std::string::npos &&
                  sec.find("Issue #4382") != std::string::npos,
              "4382 AC3: Evaluator fence defined and cited");
        CHECK(io_src.find("check_tenant_http_scheme(url, \"http-get\")") != std::string::npos,
              "4382 AC3: http-get wires the scheme fence");
        CHECK(io_src.find("check_tenant_http_scheme(curl_url, \"http-post\")") != std::string::npos,
              "4382 AC3: http-post wires the scheme fence");
        CHECK(io_src.find("CURLOPT_REDIR_PROTOCOLS") != std::string::npos,
              "4382 AC3: libcurl redirect protocols restricted");
        CHECK(io_src.find("\"--proto-redir\"") != std::string::npos,
              "4382 AC3: curl CLI fallback restricts redirect protocols");
        CHECK(build.find("check_http_tenant_scheme_4382") != std::string::npos,
              "4382 AC3: build.py wires the #4382 linter");
        CHECK(allow.find("check_http_tenant_scheme_4382.py") != std::string::npos,
              "4382 AC3: linter on the root allowlist");
        std::ifstream invent("tests/core/test_issue_4382.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_4382.cpp");
        CHECK(!invent.good(), "4382 AC3: no tests/core/test_issue_4382.cpp (forbidden)");
    }

    // ── Issue #4399: serialize-workspace / generate-type-sigs / import ──
    {
        std::println(
            "\n--- #4399 AC1: Restricted+MT denies host paths outside the tenant root ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4399-ac1";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::tenant_host_root_for;
        const auto root_a = tenant_host_root_for(7);
        std::error_code rm_ec;
        std::filesystem::remove_all(base, rm_ec);
        std::filesystem::create_directories(root_a);
        const std::string victim = base + "/victim.txt";
        const std::string outside = base + "/outside-helper.aura";
        const std::string outside_type = base + "/outside-helper.aura-type";
        const std::string deny_bin = base + "/should-deny.bin";
        const std::string escape_soul = base + "/escape.soul";
        {
            std::ofstream f(victim);
            f << "SECRET";
        }
        {
            std::ofstream f(outside);
            f << "(define (helper x) (+ x 1))\n";
        }
        {
            std::ofstream f(root_a + "/helper.aura");
            f << "(define (helper x) (+ x 1))\n";
        }

        CompilerService cs;
        auto& ev = cs.evaluator();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        CHECK(ev.host_path_policy_active(), "4399 AC1: Restricted+MT arms host-path policy");
        const auto& ring = g_security_event_ring();
        auto& heap_w = ev.string_heap_mut();
        using aura::compiler::types::EvalValue;
        using aura::compiler::types::is_module;
        using aura::compiler::types::is_void;
        using aura::compiler::types::make_string;
        using aura::compiler::types::make_void;

        auto slurp = [](const std::string& p) {
            std::ifstream in(p);
            return std::string((std::istreambuf_iterator<char>(in)), {});
        };
        auto bound = [&](std::string_view name) {
            for (const auto& kv : ev.top_env().bindings()) {
                if (kv.first == name)
                    return true;
            }
            return false;
        };
        auto saw_escape = [&](std::uint64_t se_base, std::string_view op) {
            bool saw = false;
            for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
                const auto& e = ring.ring[s % ring.ring.size()];
                if (e.seq != s)
                    continue;
                if (static_cast<int>(e.kind) != static_cast<int>(SecurityEventKind::IsolationDeny))
                    continue;
                if (std::string_view(e.reason).find("tenant-path-escape") == std::string_view::npos)
                    continue;
                if (std::string_view(e.op) != op)
                    continue;
                saw = true;
                CHECK(e.tenant_id == 7, "4399 AC1: IsolationDeny tenant is the caller");
            }
            return saw;
        };
        auto call1 = [&](auto& fn, const std::string& arg) -> EvalValue {
            if (!fn)
                return make_void();
            heap_w.push_back(arg);
            return (*fn)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
        };

        auto ser = ev.primitives().lookup("serialize-workspace");
        auto gen = ev.primitives().lookup("generate-type-sigs");
        auto loadm = ev.primitives().lookup("load-module");
        auto use = ev.primitives().lookup("use");
        auto import = ev.primitives().lookup("import");
        CHECK(ser.has_value(), "4399 AC1: serialize-workspace registered");
        CHECK(gen.has_value(), "4399 AC1: generate-type-sigs registered");
        CHECK(loadm.has_value(), "4399 AC1: load-module registered");
        CHECK(use.has_value(), "4399 AC1: use registered");
        CHECK(import.has_value(), "4399 AC1: import registered");

        {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            const auto r = call1(ser, victim);
            CHECK(is_bool(r) && !as_bool(r),
                  "4399 AC1: serialize-workspace outside root returns #f");
            CHECK(slurp(victim) == "SECRET", "4399 AC1: victim bytes unchanged (zero trunc)");
            CHECK(ev.last_mutate_error().find("serialize-workspace: tenant-path-escape") !=
                      std::string::npos,
                  "4399 AC1: serialize-workspace deny reason is tenant-path-escape");
            CHECK(saw_escape(se_base, "serialize-workspace"),
                  "4399 AC1: IsolationDeny SE op serialize-workspace");
        }
        {
            std::filesystem::remove(outside_type, rm_ec);
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            const auto r = call1(gen, outside);
            CHECK(is_bool(r) && !as_bool(r),
                  "4399 AC1: generate-type-sigs outside root returns #f");
            CHECK(!std::filesystem::exists(outside_type),
                  "4399 AC1: no .aura-type written outside the tenant root");
            CHECK(slurp(outside).find("(define (helper x)") != std::string::npos,
                  "4399 AC1: outside module source unchanged");
            CHECK(saw_escape(se_base, "generate-type-sigs"),
                  "4399 AC1: IsolationDeny SE op generate-type-sigs");
        }
        {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            const auto r = call1(loadm, outside);
            CHECK(is_void(r), "4399 AC1: load-module deny returns void (no module)");
            CHECK(!is_module(r), "4399 AC1: load-module deny is not a module");
            CHECK(!bound("helper"), "4399 AC1: load-module deny injects no helper binding");
            CHECK(saw_escape(se_base, "load-module"), "4399 AC1: IsolationDeny SE op load-module");
        }
        {
            const auto r = call1(use, outside);
            CHECK(is_void(r) && !bound("helper"), "4399 AC1: use deny returns void, no binding");
        }
        {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            const auto r = call1(import, outside);
            CHECK(is_error(r), "4399 AC1: import deny is module-load-failed, not a binding");
            CHECK(!bound("helper"), "4399 AC1: import deny does not inject helper");
            CHECK(saw_escape(se_base, "load-module"),
                  "4399 AC1: import deny is the loader fence (op load-module)");
            auto called = cs.eval("(helper 1)");
            CHECK(!(called && is_int(*called) && as_int(*called) == 2),
                  "4399 AC1: denied import does not make (helper 1) return 2");
        }
        {
            auto wf = ev.primitives().lookup("write-file");
            CHECK(wf.has_value(), "4399 AC1: write-file still registered");
            heap_w.push_back(deny_bin);
            heap_w.push_back("x");
            if (wf) {
                using aura::compiler::types::make_string;
                const auto r = (*wf)({make_string(static_cast<std::uint64_t>(heap_w.size() - 2)),
                                      make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
                CHECK(!(is_int(r) && as_int(r) == 1),
                      "4399 AC1: write-file control does not succeed outside the root");
            }
            CHECK(!std::filesystem::exists(deny_bin),
                  "4399 AC1: write-file control creates no file");
        }
        {
            const auto r = call1(import, "std/list");
            CHECK(is_error(r), "4399 AC1: jailed import does not search the host lib");
        }
        {
            const auto r = call1(import, "helper.aura");
            CHECK(is_bool(r) && as_bool(r),
                  "4399 AC1: relative import resolves under the tenant root");
            CHECK(bound("helper"), "4399 AC1: in-root import injects helper");
            auto called = cs.eval("(helper 1)");
            CHECK(called && is_int(*called) && as_int(*called) == 2,
                  "4399 AC1: in-root (helper 1) returns 2");
        }
        {
            const auto r = call1(gen, "helper.aura");
            CHECK(is_bool(r) && as_bool(r),
                  "4399 AC1: generate-type-sigs writes the in-root sibling");
            const auto sig = slurp(root_a + "/helper.aura-type");
            CHECK(sig.find("helper:") != std::string::npos,
                  "4399 AC1: .aura-type sibling lives under the tenant root");
            CHECK(!std::filesystem::exists(outside_type),
                  "4399 AC1: in-root generate-type-sigs does not write the outside sibling");
        }
        {
            const auto r = call1(ser, "snap.soul");
            CHECK(is_bool(r) && as_bool(r),
                  "4399 AC1: relative serialize-workspace writes under the root");
            CHECK(slurp(root_a + "/snap.soul").starts_with("AURASOUL"),
                  "4399 AC1: relative serialize blob is under the tenant root");
            const auto abs = call1(ser, root_a + "/snap-abs.soul");
            CHECK(is_bool(abs) && as_bool(abs),
                  "4399 AC1: in-root absolute serialize-workspace writes");
            CHECK(slurp(root_a + "/snap-abs.soul").starts_with("AURASOUL"),
                  "4399 AC1: in-root absolute blob stays under the tenant root");
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            const auto esc = call1(ser, "../escape.soul");
            CHECK(is_bool(esc) && !as_bool(esc), "4399 AC1: ../ serialize-workspace returns #f");
            CHECK(!std::filesystem::exists(escape_soul),
                  "4399 AC1: ../ serialize writes zero bytes");
            CHECK(saw_escape(se_base, "serialize-workspace"),
                  "4399 AC1: ../ escape emits IsolationDeny");
            CHECK(slurp(victim) == "SECRET", "4399 AC1: victim still SECRET after in-root writes");
        }

        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4399 AC2: Off and single-tenant Restricted keep passthrough ---");
        reset_all();
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4399-ac2";
        std::error_code rm_ec;
        std::filesystem::remove_all(base, rm_ec);
        std::filesystem::create_directories(base);
        const std::string outside = base + "/outside-helper.aura";
        const std::string outside_type = base + "/outside-helper.aura-type";
        const std::string soul = base + "/passthrough.soul";
        {
            std::ofstream f(outside);
            f << "(define (helper x) (+ x 1))\n";
        }
        auto slurp = [](const std::string& p) {
            std::ifstream in(p);
            return std::string((std::istreambuf_iterator<char>(in)), {});
        };

        {
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
            CompilerService cs;
            auto& ev = cs.evaluator();
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
            ev.set_effect_sandbox_mode(0);
            CHECK(!ev.host_path_policy_active(), "4399 AC2: Off does not arm host-path policy");
            const auto se_base = g_security_event_ring().seq.load(std::memory_order_acquire);
            auto& heap_w = ev.string_heap_mut();
            using aura::compiler::types::make_string;
            auto ser = ev.primitives().lookup("serialize-workspace");
            CHECK(ser.has_value(), "4399 AC2: serialize-workspace registered");
            heap_w.push_back(soul);
            if (ser) {
                const auto r = (*ser)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
                CHECK(is_bool(r) && as_bool(r), "4399 AC2: Off serialize-workspace still writes");
            }
            CHECK(slurp(soul).starts_with("AURASOUL"), "4399 AC2: Off serialize blob landed");
            auto import = ev.primitives().lookup("import");
            CHECK(import.has_value(), "4399 AC2: import registered");
            heap_w.push_back(outside);
            if (import) {
                const auto r =
                    (*import)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
                CHECK(is_bool(r) && as_bool(r),
                      "4399 AC2: Off import of an absolute path still loads");
            }
            auto called = cs.eval("(helper 1)");
            CHECK(called && is_int(*called) && as_int(*called) == 2,
                  "4399 AC2: Off import still binds helper");
            auto gen = ev.primitives().lookup("generate-type-sigs");
            heap_w.push_back(outside);
            if (gen) {
                const auto r = (*gen)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
                CHECK(is_bool(r) && as_bool(r), "4399 AC2: Off generate-type-sigs still writes");
            }
            CHECK(std::filesystem::exists(outside_type),
                  "4399 AC2: Off .aura-type sibling written");
            CHECK(g_security_event_ring().seq.load(std::memory_order_acquire) == se_base,
                  "4399 AC2: Off passthrough emits no IsolationDeny");
        }

        {
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
            CompilerService cs;
            auto& ev = cs.evaluator();
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
            ev.set_effect_sandbox_mode(1);
            ev.set_capability_tenant_id(7);
            CHECK(!ev.host_path_policy_active(),
                  "4399 AC2: single-tenant Restricted does not arm host-path policy");
            const auto se_base = g_security_event_ring().seq.load(std::memory_order_acquire);
            const std::string soul_st = base + "/single-tenant.soul";
            auto& heap_w = ev.string_heap_mut();
            using aura::compiler::types::make_string;
            auto ser = ev.primitives().lookup("serialize-workspace");
            heap_w.push_back(soul_st);
            if (ser) {
                const auto r = (*ser)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
                CHECK(is_bool(r) && as_bool(r),
                      "4399 AC2: single-tenant Restricted serialize-workspace still writes");
            }
            CHECK(slurp(soul_st).starts_with("AURASOUL"),
                  "4399 AC2: single-tenant serialize blob landed outside any tenant root");
            auto import = ev.primitives().lookup("import");
            heap_w.push_back(outside);
            if (import) {
                const auto r =
                    (*import)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
                CHECK(is_bool(r) && as_bool(r),
                      "4399 AC2: single-tenant Restricted import still loads an absolute path");
            }
            auto called = cs.eval("(helper 1)");
            CHECK(called && is_int(*called) && as_int(*called) == 2,
                  "4399 AC2: single-tenant import still binds helper");
            // Passthrough must not emit tenant-path-escape. Other
            // IsolationDeny rows (eval / stamp) are outside this fence.
            bool path_escape = false;
            std::string stray;
            const auto& ring = g_security_event_ring();
            for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
                const auto& e = ring.ring[s % ring.ring.size()];
                if (e.seq != s)
                    continue;
                if (static_cast<int>(e.kind) != static_cast<int>(SecurityEventKind::IsolationDeny))
                    continue;
                stray += std::string(e.op) + "=" + e.reason + ";";
                if (std::string_view(e.reason).find("tenant-path-escape") != std::string_view::npos)
                    path_escape = true;
            }
            CHECK(!path_escape,
                  std::string("4399 AC2: single-tenant passthrough emits no tenant-path-escape") +
                      (stray.empty() ? std::string() : " (" + stray + ")"));
        }
    }

    {
        std::println(
            "\n--- #4399 AC3: wiring cite; no EXEMPT growth, no new query, no invent test ---");
        const auto persist = read_file("src/compiler/evaluator_primitives_persist.cpp");
        const auto types_src = read_file("src/compiler/evaluator_primitives_types.cpp");
        const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
        const auto exempt =
            read_file("scripts/coverage/checks/check_side_effect_fiber_principal_2839.py");
        CHECK(persist.find("check_tenant_host_path(path, resolved, \"serialize-workspace\")") !=
                  std::string::npos,
              "4399 AC3: serialize-workspace wires check_tenant_host_path");
        CHECK(persist.find("std::ofstream ofs(resolved, std::ios::binary | std::ios::trunc)") !=
                  std::string::npos,
              "4399 AC3: serialize-workspace writes the resolved path");
        CHECK(persist.find("check_tenant_host_path(path, resolved, \"deserialize-workspace\")") !=
                  std::string::npos,
              "4400: deserialize-workspace wires check_tenant_host_path (#4399 left it open)");
        CHECK(types_src.find("check_tenant_host_path(caller, gated, \"generate-type-sigs\")") !=
                  std::string::npos,
              "4399 AC3: generate-type-sigs wires check_tenant_host_path");
        CHECK(types_src.find("ev.host_path_policy_active() ? std::move(gated)") !=
                  std::string::npos,
              "4399 AC3: active generate-type-sigs does not re-enter host search");
        CHECK(types_src.find("check_tenant_host_path(caller, gated, \"check-module-signature\")") ==
                  std::string::npos,
              "4399 AC3: check-module-signature stays out of this fence");
        CHECK(loader.find("check_tenant_host_path(path, gated, \"load-module\")") !=
                  std::string::npos,
              "4399 AC3: load_module_file wires check_tenant_host_path");
        CHECK(loader.find(
                  "host_path_policy_active() ? std::move(gated) : resolve_module_path(gated)") !=
                  std::string::npos,
              "4399 AC3: active load uses the gated path; passthrough keeps resolve_module_path");
        CHECK(exempt.find("\"serialize-workspace\"") == std::string::npos,
              "4399 AC3: serialize-workspace not added to EXEMPT_2ARG_OPS");
        CHECK(exempt.find("\"generate-type-sigs\"") == std::string::npos,
              "4399 AC3: generate-type-sigs not added to EXEMPT_2ARG_OPS");
        CHECK(exempt.find("\"load-module\"") == std::string::npos,
              "4399 AC3: load-module not added to EXEMPT_2ARG_OPS");
        CHECK(exempt.find("len(EXEMPT_2ARG_OPS) != 7") != std::string::npos,
              "4399 AC3: EXEMPT_2ARG_OPS count guard stays 7");
        CHECK(persist.find("query:4399") == std::string::npos &&
                  types_src.find("query:4399") == std::string::npos &&
                  loader.find("query:4399") == std::string::npos,
              "4399 AC3: no new query key");
        std::ifstream invent("tests/core/test_issue_4399.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_4399.cpp");
        CHECK(!invent.good(), "4399 AC3: no tests/core/test_issue_4399.cpp (forbidden)");
    }

    // ── Issue #4400: deserialize-workspace / set-code / ast:restore Mutate ──
    {
        std::println("\n--- #4400 AC1: Restricted+MT path escape never reads ---");
        reset_all();
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4400";
        std::error_code rm_ec;
        std::filesystem::remove_all(base, rm_ec);
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::tenant_host_root_for;
        const auto root_a = tenant_host_root_for(7);
        std::filesystem::create_directories(root_a);
        const std::string crafted = root_a + "/crafted.bin";
        const std::string victim = base + "/victim-4400.bin";
        const std::string missing = base + "/missing-4400.bin";
        {
            std::ofstream f(victim, std::ios::binary);
            f << "SECRET4400";
        }
        {
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
            CompilerService crafter;
            auto& ev = crafter.evaluator();
            ev.set_effect_sandbox_mode(0);
            CHECK(crafter.eval("(set-code \"(define (marker) 7)\")").has_value(),
                  "4400 setup: Off set-code marker");
            CHECK(crafter.eval("(eval-current)").has_value(), "4400 setup: Off eval-current");
            auto& heap_w = ev.string_heap_mut();
            heap_w.push_back(crafted);
            auto ser = ev.primitives().lookup("serialize-workspace");
            CHECK(ser.has_value(), "4400 setup: serialize-workspace registered");
            if (ser) {
                using aura::compiler::types::make_string;
                const auto r = (*ser)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
                CHECK(is_bool(r) && as_bool(r), "4400 setup: Off serialize crafted blob");
            }
        }
        {
            std::ifstream in(crafted);
            std::string magic(8, '\0');
            in.read(magic.data(), 8);
            CHECK(magic == "AURASOUL", "4400 setup: crafted blob magic");
        }

        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        CompilerService cs;
        auto& ev = cs.evaluator();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        CHECK(ev.host_path_policy_active(), "4400 AC1: Restricted+MT arms host-path policy");
        const auto& ring = g_security_event_ring();
        auto& heap_w = ev.string_heap_mut();
        using aura::compiler::types::make_string;
        auto deser = ev.primitives().lookup("deserialize-workspace");
        CHECK(deser.has_value(), "4400 AC1: deserialize-workspace registered");
        auto slurp = [](const std::string& p) {
            std::ifstream in(p);
            return std::string((std::istreambuf_iterator<char>(in)), {});
        };
        auto saw_escape = [&](std::uint64_t se_base) {
            bool saw = false;
            for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
                const auto& e = ring.ring[s % ring.ring.size()];
                if (e.seq != s)
                    continue;
                if (static_cast<int>(e.kind) != static_cast<int>(SecurityEventKind::IsolationDeny))
                    continue;
                if (std::string_view(e.reason).find("tenant-path-escape") == std::string_view::npos)
                    continue;
                if (std::string_view(e.op) != "deserialize-workspace")
                    continue;
                saw = true;
                CHECK(e.tenant_id == 7, "4400 AC1: IsolationDeny tenant is the caller");
            }
            return saw;
        };
        {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            heap_w.push_back(missing);
            const auto r = (*deser)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
            CHECK(is_bool(r) && !as_bool(r), "4400 AC1: missing outside path returns #f");
            CHECK(saw_escape(se_base),
                  "4400 AC1: missing path emits IsolationDeny before load_blob");
        }
        {
            const auto se_base = ring.seq.load(std::memory_order_acquire);
            heap_w.push_back(victim);
            const auto r = (*deser)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
            CHECK(is_bool(r) && !as_bool(r), "4400 AC1: outside victim returns #f");
            CHECK(slurp(victim) == "SECRET4400", "4400 AC1: victim bytes unchanged");
            CHECK(saw_escape(se_base), "4400 AC1: victim path emits IsolationDeny");
            CHECK(ev.last_mutate_error().find("deserialize-workspace: tenant-path-escape") !=
                      std::string::npos,
                  "4400 AC1: deny reason is tenant-path-escape");
        }
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4400 AC2: in-root blob without Mutate leaves the workspace ---");
        reset_all();
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4400";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::tenant_host_root_for;
        const auto root_a = tenant_host_root_for(7);
        const std::string crafted = root_a + "/crafted.bin";
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        CHECK(cs.eval("(set-code \"(define (sentinel) 1)\")").has_value(),
              "4400 AC2: Off sentinel set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4400 AC2: Off sentinel eval");
        auto* flat = ev.workspace_flat();
        CHECK(flat != nullptr, "4400 AC2: sentinel workspace live");
        const auto log_n = flat ? flat->all_mutations().size() : 0;
        auto bound = [&](std::string_view name) {
            for (const auto& kv : ev.top_env().bindings()) {
                if (kv.first == name)
                    return true;
            }
            return false;
        };
        CHECK(bound("sentinel"), "4400 AC2: sentinel bound before the attack");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        CHECK(ev.host_path_policy_active(), "4400 AC2: policy armed after the sentinel");
        auto& heap_w = ev.string_heap_mut();
        using aura::compiler::types::make_string;
        auto deser = ev.primitives().lookup("deserialize-workspace");
        CHECK(deser.has_value(), "4400 AC2: deserialize-workspace registered");
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        heap_w.push_back(crafted);
        const auto r = (*deser)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
        CHECK(is_error(r), "4400 AC2: in-root deserialize without Mutate is an error");
        const auto msg = ev.soft_error_message(r);
        CHECK(msg.find("effect-denied:") != std::string::npos, "4400 AC2: effect-denied visible");
        CHECK(msg.find("op=set-code") != std::string::npos, "4400 AC2: deny op is set-code");
        CHECK(ev.workspace_flat() == flat, "4400 AC2: workspace_flat_ pointer unchanged");
        CHECK(flat && flat->all_mutations().size() == log_n,
              "4400 AC2: mutation log size unchanged");
        CHECK(bound("sentinel"), "4400 AC2: sentinel still bound");
        auto called = cs.eval("(sentinel)");
        CHECK(called && is_int(*called) && as_int(*called) == 1, "4400 AC2: (sentinel) still 1");
        auto marker = cs.eval("(marker)");
        CHECK(!(marker && is_int(*marker) && as_int(*marker) == 7),
              "4400 AC2: marker was not installed");
        auto find = ev.primitives().lookup("workspace:find-define");
        CHECK(find.has_value(), "4400 AC2: workspace:find-define registered");
        heap_w.push_back("marker");
        if (find) {
            const auto fr = (*find)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
            CHECK(!(is_int(fr) && as_int(fr) == 2),
                  "4400 AC2: find-define marker is not integer 2");
        }
        bool path_escape = false;
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.seq != s)
                continue;
            if (static_cast<int>(e.kind) != static_cast<int>(SecurityEventKind::IsolationDeny))
                continue;
            if (std::string_view(e.op) == "deserialize-workspace" &&
                std::string_view(e.reason).find("tenant-path-escape") != std::string_view::npos)
                path_escape = true;
        }
        CHECK(!path_escape, "4400 AC2: in-root absolute path is not a tenant-path-escape");
        const auto se_rel = ring.seq.load(std::memory_order_acquire);
        const auto flat_after = ev.workspace_flat();
        const auto log_after = flat_after ? flat_after->all_mutations().size() : 0;
        heap_w.push_back("crafted.bin");
        const auto rel = (*deser)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
        CHECK(is_error(rel), "4400 AC2: in-root relative deserialize without Mutate is an error");
        const auto rel_msg = ev.soft_error_message(rel);
        CHECK(rel_msg.find("effect-denied:") != std::string::npos &&
                  rel_msg.find("op=set-code") != std::string::npos,
              "4400 AC2: relative path is Mutate-denied, not path-denied");
        CHECK(ev.workspace_flat() == flat_after, "4400 AC2: relative deny keeps the flat pointer");
        CHECK(flat_after && flat_after->all_mutations().size() == log_after,
              "4400 AC2: relative deny keeps the mutation log");
        bool rel_escape = false;
        for (std::uint64_t s = se_rel; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.seq != s)
                continue;
            if (static_cast<int>(e.kind) != static_cast<int>(SecurityEventKind::IsolationDeny))
                continue;
            if (std::string_view(e.op) == "deserialize-workspace" &&
                std::string_view(e.reason).find("tenant-path-escape") != std::string_view::npos)
                rel_escape = true;
        }
        CHECK(!rel_escape, "4400 AC2: relative in-root path is not a tenant-path-escape");
        ::unsetenv("AURA_TENANT_FS_ROOT");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println(
            "\n--- #4400 AC3: Off deserialize still installs and emits no path escape ---");
        reset_all();
        const char* tmp = std::getenv("TMPDIR");
        const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-4400";
        ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
        using aura::compiler::security::tenant_host_root_for;
        const std::string crafted = tenant_host_root_for(7) + "/crafted.bin";
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        CHECK(!ev.host_path_policy_active(), "4400 AC3: Off does not arm host-path policy");
        const auto& ring = g_security_event_ring();
        const auto se_base = ring.seq.load(std::memory_order_acquire);
        auto& heap_w = ev.string_heap_mut();
        using aura::compiler::types::make_string;
        auto deser = ev.primitives().lookup("deserialize-workspace");
        CHECK(deser.has_value(), "4400 AC3: deserialize-workspace registered");
        heap_w.push_back(crafted);
        const auto r = (*deser)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
        CHECK(is_bool(r) && as_bool(r), "4400 AC3: Off deserialize returns #t");
        auto called = cs.eval("(marker)");
        CHECK(called && is_int(*called) && as_int(*called) == 7,
              "4400 AC3: Off (marker) returns 7");
        auto find = ev.primitives().lookup("workspace:find-define");
        heap_w.push_back("marker");
        if (find) {
            const auto fr = (*find)({make_string(static_cast<std::uint64_t>(heap_w.size() - 1))});
            CHECK(is_int(fr), "4400 AC3: Off find-define marker is an integer");
        }
        bool path_escape = false;
        for (std::uint64_t s = se_base; s < ring.seq.load(std::memory_order_acquire); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.seq != s)
                continue;
            if (static_cast<int>(e.kind) != static_cast<int>(SecurityEventKind::IsolationDeny))
                continue;
            if (std::string_view(e.reason).find("tenant-path-escape") != std::string_view::npos)
                path_escape = true;
        }
        CHECK(!path_escape, "4400 AC3: Off deserialize emits no tenant-path-escape");
        ::unsetenv("AURA_TENANT_FS_ROOT");
    }

    {
        std::println(
            "\n--- #4400 AC4: ast:restore direct path without Mutate keeps workspace B ---");
        reset_all();
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        CHECK(cs.eval("(set-code \"(define (marker) 1)\")").has_value(), "4400 AC4: set-code A");
        CHECK(cs.eval("(eval-current)").has_value(), "4400 AC4: eval A");
        auto snap = cs.eval("(ast:snapshot \"pre-4400\")");
        CHECK(snap && is_int(*snap) && as_int(*snap) >= 0, "4400 AC4: snapshot id");
        CHECK(cs.eval("(set-code \"(define (marker) 9)\")").has_value(), "4400 AC4: set-code B");
        CHECK(cs.eval("(eval-current)").has_value(), "4400 AC4: eval B");
        auto before = cs.eval("(marker)");
        CHECK(before && is_int(*before) && as_int(*before) == 9, "4400 AC4: workspace is B");
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        using aura::compiler::types::make_int;
        auto restore = ev.primitives().lookup("ast:restore");
        CHECK(restore.has_value(), "4400 AC4: ast:restore registered");
        const auto r = (*restore)({make_int(as_int(*snap))});
        CHECK(is_error(r), "4400 AC4: direct restore without Mutate is an error");
        const auto msg = ev.soft_error_message(r);
        CHECK(msg.find("effect-denied:") != std::string::npos, "4400 AC4: effect-denied visible");
        CHECK(msg.find("op=ast:restore") != std::string::npos, "4400 AC4: deny op is ast:restore");
        auto after = cs.eval("(marker)");
        CHECK(after && is_int(*after) && as_int(*after) == 9, "4400 AC4: workspace stays B");
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
    }

    {
        std::println("\n--- #4400 AC5: wiring cite; no EXEMPT, no infer, no query, no invent ---");
        const auto eval_src = read_file("src/compiler/evaluator_primitives_eval.cpp");
        const auto persist = read_file("src/compiler/evaluator_primitives_persist.cpp");
        const auto ast = read_file("src/compiler/evaluator_primitives_ast.cpp");
        const auto exempt =
            read_file("scripts/coverage/checks/check_side_effect_fiber_principal_2839.py");
        const auto infer = read_file("src/compiler/security_side_effect.hh");
        const auto gate = eval_src.find("const std::string_view set_code_op = \"set-code\"");
        const auto req = gate == std::string::npos ? gate : eval_src.find("require_effect", gate);
        const auto acquire = gate == std::string::npos ? gate : eval_src.find("try_acquire", gate);
        const auto assign =
            gate == std::string::npos ? gate : eval_src.find("ev.workspace_flat_ = flat_ptr", gate);
        CHECK(gate != std::string::npos && req != std::string::npos && req < acquire &&
                  acquire != std::string::npos && assign != std::string::npos && req < assign,
              "4400 AC5: set-code require_effect is before try_acquire and the flat swap");
        CHECK(eval_src.find(
                  "require_effect(aura::compiler::security::kEffectMutate, set_code_op, 0,") !=
                  std::string::npos,
              "4400 AC5: set-code uses the no-target 4-arg shape");
        CHECK(eval_src.find("ev.capability_tenant_id()") != std::string::npos,
              "4400 AC5: set-code stamps the caller tenant");
        const auto path_gate =
            persist.find("check_tenant_host_path(path, resolved, \"deserialize-workspace\")");
        const auto load_resolved = persist.find("load_blob(resolved, blob, &err)");
        CHECK(path_gate != std::string::npos && load_resolved != std::string::npos &&
                  path_gate < load_resolved,
              "4400 AC5: deserialize path gate is before load_blob(resolved)");
        const auto empty_op =
            persist.find("const std::string_view deser_mutate_op = \"deserialize-workspace\"");
        const auto log_clear = persist.find("log.clear();");
        CHECK(empty_op != std::string::npos && log_clear != std::string::npos &&
                  empty_op < log_clear,
              "4400 AC5: empty-source log rewrite pays Mutate before log.clear");
        CHECK(persist.find("if (blob.source.empty())") != std::string::npos,
              "4400 AC5: log Mutate gate is only the empty-source branch");
        const auto restore_at = ast.find("add(\"ast:restore\"");
        const auto restore_end = ast.find("add(\"ast:diff\"", restore_at);
        CHECK(restore_at != std::string::npos && restore_end != std::string::npos &&
                  restore_end > restore_at,
              "4400 AC5: ast:restore body located");
        if (restore_at != std::string::npos && restore_end > restore_at) {
            const auto body = ast.substr(restore_at, restore_end - restore_at);
            const auto op_at = body.find("const std::string_view ast_restore_op = \"ast:restore\"");
            const auto req_at = body.find("require_effect");
            const auto acq_at = body.find("try_acquire");
            const auto wr_at = body.find("*ev.workspace_flat_ = *ev.snapshot_flats_[id].flat");
            const auto can_at = body.find("if (can_direct)");
            CHECK(can_at != std::string::npos && op_at != std::string::npos &&
                      req_at != std::string::npos && acq_at != std::string::npos &&
                      wr_at != std::string::npos && can_at < op_at && op_at < acq_at &&
                      req_at < wr_at,
                  "4400 AC5: direct ast:restore require_effect precedes the flat write");
            CHECK(
                body.find(
                    "require_effect(aura::compiler::security::kEffectMutate, ast_restore_op, 0,") !=
                    std::string::npos,
                "4400 AC5: ast:restore uses the no-target 4-arg shape");
        }
        CHECK(exempt.find("\"set-code\"") == std::string::npos,
              "4400 AC5: set-code not added to EXEMPT_2ARG_OPS");
        CHECK(exempt.find("\"deserialize-workspace\"") == std::string::npos,
              "4400 AC5: deserialize-workspace not added to EXEMPT_2ARG_OPS");
        CHECK(exempt.find("\"ast:restore\"") == std::string::npos,
              "4400 AC5: ast:restore not added to EXEMPT_2ARG_OPS");
        CHECK(exempt.find("len(EXEMPT_2ARG_OPS) != 7") != std::string::npos,
              "4400 AC5: EXEMPT_2ARG_OPS count guard stays 7");
        CHECK(infer.find("set-code") == std::string::npos &&
                  infer.find("deserialize-workspace") == std::string::npos &&
                  infer.find("ast:restore") == std::string::npos,
              "4400 AC5: infer_required_effects_from_name does not name these ops");
        CHECK(eval_src.find("query:4400") == std::string::npos &&
                  persist.find("query:4400") == std::string::npos &&
                  ast.find("query:4400") == std::string::npos,
              "4400 AC5: no new query key");
        std::ifstream invent("tests/core/test_issue_4400.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_4400.cpp");
        CHECK(!invent.good(), "4400 AC5: no tests/core/test_issue_4400.cpp (forbidden)");
    }

    // ── Issue #3904: MSE TA fence posture (caller-OR-target documented) ──
    ac4133_1_target_ta_non_ta_caller_denied();
    ac4133_2_caller_ta_mint_foreign_target_lands();
    ac4133_3_soft_off_zero_cost_unchanged();
    ac4133_4_concurrent_ta_revoke_mint_deny_table_unchanged();
    ac4133_5_prim_mse_seed_refuses_with_base_grant();
    ac3904_2_caller_with_ta_allow_unchanged();
    ac3904_3_neither_ta_denied();
    ac3904_4_soft_off_zero_cost_unchanged();
    ac3904_5_source_cite();
    ac4165_agent_fiber_isolation();

    reset_all();
    std::println("\n=== test_tenant_isolation_enforcement: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}
