// security_defaults.hh — Issue #2076 / #2053 production security defaults.
//
// Header form so main / tests / evaluator TUs share one definition without
// module-attachment linkage breaks (free functions defined inside a module
// partition are not visible to non-module callers via a plain header decl).

#ifndef AURA_COMPILER_SECURITY_DEFAULTS_HH
#define AURA_COMPILER_SECURITY_DEFAULTS_HH

#include "typed_mutation_audit.h"
#include "coercion_provenance_policy.hh"   // Issue #2185 reject-on-miss production default
#include "compiler/hot_update_registry.hh" // Issue #2205 reemit boundary production default
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>

namespace aura::compiler::security {

// Issue #2076: free-function sandbox env apply for main() before Evaluator.
inline void apply_aura_sandbox_env() noexcept {
    const char* e = std::getenv("AURA_SANDBOX");
    auto mode = static_cast<std::uint8_t>(1); // default Restricted
    if (e && *e) {
        std::string_view v(e);
        if (v == "off")
            mode = 0;
        else if (v == "strict")
            mode = 2;
        else
            mode = 1; // restricted (also default for unknown values)
    }
    using namespace ::aura::core::sandbox;
    using namespace ::aura::core::capability;
    using namespace ::aura::core::workspace_isolation;
    set_mode(static_cast<SandboxMode>(mode));
    g_capability_registry().sandbox_mode = static_cast<EffectSandboxMode>(mode);
    // Strict links workspace isolation (parity with Evaluator::set_effect_sandbox_mode).
    g_workspace_isolation().set_strict_sandbox_linked(mode == 2);
}

// Issue #2136: permanent Render effect for the kernel principal (tenant 0).
// Ordinary multi-tenant Agents must acquire "render" (or wildcard) explicitly.
// Called from production defaults so unrestricted host/REPL paths keep working
// under Restricted sandbox without silent grant to foreign tenants.
inline void grant_render_kernel_principal() noexcept {
    using namespace ::aura::core::capability;
    EffectProvenance prov{};
    prov.epoch = 1;
    prov.mutation_id = 1;
    // Name "render-kernel" is Agent-discoverable; effects include Render.
    g_capability_registry().grant(/*tenant=*/0, "render-kernel", Effect::Render, prov);
    // Also grant the canonical cap name so has_capability("render") is true.
    g_capability_registry().grant(/*tenant=*/0, "render", Effect::Render, prov);
}

// Issue #2053 / #2150 / #2151 / #2182 / #2185: production multi-tenant AI
// security defaults. Applies (in order):
//   1. AURA_SANDBOX → Restricted (default) | off | strict  (#2076)
//   2. AURA_MULTI_TENANT=1|true|yes → escalate to Strict
//   3. TypedMutationAudit Full (or AURA_TYPED_AUDIT=sampled|off|full)
//   4. Mutation audit WAL (#2150):
//        - AURA_MUTATION_AUDIT_WAL / AURA_PERSIST_DIR when set (always win)
//        - else force default dir under multi-tenant OR Strict
//        - skipped entirely when AURA_SANDBOX=off (tests / local)
//   5. Kernel principal (tenant 0) holds permanent Render (#2136)
//   6. Hard fiber isolation (#2151):
//        - default soft (false) preserves #2055 same-tenant multi-fiber share
//        - multi-tenant + Strict enables hard-deny on grant_fiber_id mismatch
//        - AURA_HARD_FIBER_ISOLATION=0|1|true|false|on|off overrides
//   7. Grant epoch retain window (#2154):
//        - multi-tenant / Strict default K=64 (last 64 Mutation epochs)
//        - AURA_GRANT_EPOCH_RETAIN=<N> overrides (0 disables auto fence)
//        - AURA_SANDBOX=off forces K=0 (unit tests must not auto-fence)
//   8. LinearEnforceMode (#2207 / #2182 / refine #2103):
//        - process default Strict (incomplete linear×provenance hard-fails)
//        - Soft only via set_linear_enforce_mode(Soft), AURA_LINEAR_ENFORCE=soft,
//          or AURA_SANDBOX=off (unit Soft-path ergonomics)
//        - AURA_LINEAR_ENFORCE=soft|strict always wins when set (canary)
//   9. Coercion provenance miss (#2185 / refine #2102):
//        - production → reject_apply_on_provenance_miss (no CoercionNode)
//        - AURA_SANDBOX=off → soft apply + sentinel (dev ergonomics)
//        - force_audit_on_provenance_miss always true
//        - AURA_COERCION_PROVENANCE_REJECT=reject|soft canary override
// Dev/test: AURA_SANDBOX=off restores Off + Sampled/4 audit + no WAL + soft
// fiber + Soft linear enforce + soft coercion apply.
inline void apply_production_security_defaults() noexcept {
    using namespace ::aura::core::sandbox;
    using namespace ::aura::core::capability;
    using namespace ::aura::core::workspace_isolation;
    using namespace ::aura::core::audit_wal;
    using namespace ::aura::compiler::typed_audit;

    // 1) Sandbox: Restricted by default (#2076).
    apply_aura_sandbox_env();

    const char* sandbox_e = std::getenv("AURA_SANDBOX");
    const bool dev_off = sandbox_e && *sandbox_e && std::string_view(sandbox_e) == "off";

    // 2) Multi-tenant: escalate to Strict when AURA_MULTI_TENANT is set
    //    (unless explicitly AURA_SANDBOX=off for local iteration).
    bool multi_tenant = false;
    if (!dev_off) {
        const char* mt = std::getenv("AURA_MULTI_TENANT");
        if (mt && *mt) {
            std::string_view mv(mt);
            if (mv == "1" || mv == "true" || mv == "yes" || mv == "on") {
                multi_tenant = true;
                set_mode(SandboxMode::Strict);
                g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
                g_workspace_isolation().set_strict_sandbox_linked(true);
            }
        }
    }

    // 3) TypedMutationAudit: Full under production; Sampled/4 when Off.
    //    Override: AURA_TYPED_AUDIT=full|sampled|off
    if (dev_off) {
        apply_dev_audit_defaults();
    } else {
        const char* aud = std::getenv("AURA_TYPED_AUDIT");
        if (aud && *aud) {
            std::string_view av(aud);
            if (av == "off") {
                set_strategy(AuditStrategy::Off);
                g_typed_mutation_audit_counters.production_defaults_active.store(
                    0, std::memory_order_relaxed);
            } else if (av == "sampled") {
                // Production Sampled still uses ratio=1 (every event).
                set_strategy(AuditStrategy::Sampled);
                set_sample_ratio(1);
                g_typed_mutation_audit_counters.production_defaults_active.store(
                    1, std::memory_order_relaxed);
            } else {
                apply_production_audit_defaults(); // full
            }
        } else {
            apply_production_audit_defaults(); // Full
        }
    }

    // 4) Mutation audit WAL (#2150).
    //    Explicit path env always wins. Under multi-tenant OR Strict, force
    //    a durable default dir so commercial deploys get forensic trail
    //    without tribal knowledge of env vars. AURA_SANDBOX=off never enables
    //    WAL (unit tests must not spill files).
    if (!dev_off) {
        const bool strict = g_sandbox_state().mode == SandboxMode::Strict ||
                            g_capability_registry().sandbox_mode == EffectSandboxMode::Strict;
        const bool force_wal = multi_tenant || strict;
        const char* explicit_wal = std::getenv("AURA_MUTATION_AUDIT_WAL");
        if (!explicit_wal || !*explicit_wal)
            explicit_wal = std::getenv("AURA_PERSIST_DIR");
        const bool has_explicit = explicit_wal && *explicit_wal;

        if (has_explicit || force_wal) {
            bool used_default = false;
            const auto dir = has_explicit ? std::string(explicit_wal)
                                          : resolve_mutation_audit_wal_dir(&used_default);
            // Process-wide enable; ring replay happens when an Evaluator
            // later calls enable_mutation_audit_wal on the same path.
            if (g_mutation_audit_wal().enable(std::string_view(dir), nullptr, 0)) {
                if (force_wal && !has_explicit) {
                    g_audit_wal_metrics().audit_wal_forced_by_multi_tenant_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
                if (used_default || (force_wal && !has_explicit)) {
                    g_audit_wal_metrics().audit_wal_using_default_dir.store(
                        1, std::memory_order_relaxed);
                    // One-shot stderr note so operators see the durable path.
                    static std::atomic<bool> warned{false};
                    bool expected = false;
                    if (warned.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                        std::fprintf(stderr,
                                     "[aura] mutation audit WAL forced under multi-tenant/Strict "
                                     "→ %s (set AURA_MUTATION_AUDIT_WAL to override)\n",
                                     dir.c_str());
                    }
                }
            }
        }
    }

    // 5) Issue #2136: kernel principal (tenant 0) always holds Render under
    //    production sandbox so host/REPL terminal I/O keeps working while
    //    multi-tenant Agents still need an explicit grant.
    if (!dev_off)
        grant_render_kernel_principal();

    // 5b) Issue #2205: HotUpdate reemit boundary policy — production default
    //     Defer (fail-closed under multi-fiber; SoftEnter is not steal-safe).
    //     SoftEnter only via AURA_REEMIT_SOFT_ENTER=1 (or explicit setter in
    //     tests). AURA_SANDBOX=off leaves SoftEnter available for unit tests
    //     that set policy explicitly; process default remains Defer.
    {
        using P = ::aura::compiler::HotUpdateRegistry::ReemitBoundaryPolicy;
        const char* soft = std::getenv("AURA_REEMIT_SOFT_ENTER");
        const bool want_soft = soft && *soft &&
                               (std::string_view(soft) == "1" || std::string_view(soft) == "true" ||
                                std::string_view(soft) == "yes" || std::string_view(soft) == "on");
        if (want_soft)
            ::aura::compiler::hot_update_registry().set_reemit_boundary_policy(P::SoftEnter);
        else
            ::aura::compiler::hot_update_registry().set_reemit_boundary_policy(P::Defer);
    }

    // 6) Issue #2151: hard fiber isolation policy.
    //    Soft default preserves #2055 (same-tenant multi-fiber share grants;
    //    TenantScope remains the principal boundary). Commercial multi-tenant
    //    + Strict enables hard-deny so fiber B cannot exercise fiber A's grant.
    //    AURA_HARD_FIBER_ISOLATION=0|1|true|false|on|off always wins when set.
    //    AURA_SANDBOX=off forces soft (unit tests must not inherit hard deny).
    if (dev_off) {
        g_capability_registry().set_hard_fiber_isolation(false);
    } else {
        const char* hfi = std::getenv("AURA_HARD_FIBER_ISOLATION");
        if (hfi && *hfi) {
            std::string_view hv(hfi);
            const bool on = (hv == "1" || hv == "true" || hv == "yes" || hv == "on");
            g_capability_registry().set_hard_fiber_isolation(on);
        } else {
            const bool strict = g_sandbox_state().mode == SandboxMode::Strict ||
                                g_capability_registry().sandbox_mode == EffectSandboxMode::Strict;
            // Multi-tenant Strict: hard-deny on fiber mismatch (commercial default).
            g_capability_registry().set_hard_fiber_isolation(multi_tenant && strict);
        }
    }

    // 7) Issue #2154: sliding grant_min_valid_epoch retain window.
    //    K=0 (default for single-tenant / Restricted) preserves #2074 manual
    //    fence only. Multi-tenant or Strict production enables K=64 so
    //    ancient grants expire as Mutation epoch advances. Env
    //    AURA_GRANT_EPOCH_RETAIN always wins when set. sandbox=off → K=0.
    if (dev_off) {
        g_capability_registry().set_grant_epoch_retain_window(0);
        g_capability_registry().set_grant_min_valid_epoch(0);
    } else {
        const char* ger = std::getenv("AURA_GRANT_EPOCH_RETAIN");
        if (ger && *ger) {
            char* end = nullptr;
            const auto v = std::strtoull(ger, &end, 10);
            if (end != ger)
                g_capability_registry().set_grant_epoch_retain_window(
                    static_cast<std::uint64_t>(v));
        } else {
            const bool strict = g_sandbox_state().mode == SandboxMode::Strict ||
                                g_capability_registry().sandbox_mode == EffectSandboxMode::Strict;
            if (multi_tenant || strict) {
                g_capability_registry().set_grant_epoch_retain_window(
                    kDefaultGrantEpochRetainWindowMultiTenant);
            } else {
                g_capability_registry().set_grant_epoch_retain_window(0);
            }
        }
    }

    // 8) Issue #2207 / #2182: LinearEnforceMode process default is Strict
    //    (align with Full audit). Soft under AURA_SANDBOX=off so unit tests
    //    keep Soft metric-only incomplete trails. AURA_LINEAR_ENFORCE=soft|
    //    strict always wins when set (canary / intentional Soft prod).
    //    IR Move/Borrow/Drop + dual-path apply + boundary consistency all
    //    read linear_enforce_require_complete() → same mode.
    {
        using aura::core::provenance::LinearEnforceMode;
        using aura::core::provenance::set_linear_enforce_mode;
        const char* le = std::getenv("AURA_LINEAR_ENFORCE");
        if (le && *le) {
            std::string_view lv(le);
            const bool want_strict =
                (lv == "strict" || lv == "1" || lv == "true" || lv == "yes" || lv == "on");
            const bool want_soft =
                (lv == "soft" || lv == "0" || lv == "false" || lv == "no" || lv == "off");
            if (want_strict)
                set_linear_enforce_mode(LinearEnforceMode::Strict);
            else if (want_soft)
                set_linear_enforce_mode(LinearEnforceMode::Soft);
            else
                // Unknown value → production Strict / dev Soft (parity with
                // the no-env branch) so typos do not silently soft-fail prod.
                set_linear_enforce_mode(dev_off ? LinearEnforceMode::Soft
                                                : LinearEnforceMode::Strict);
        } else if (dev_off) {
            set_linear_enforce_mode(LinearEnforceMode::Soft);
        } else {
            set_linear_enforce_mode(LinearEnforceMode::Strict);
        }
    }

    // 9) Issue #2185: production refuse CoercionNode insert on incomplete
    //    provenance chain (forensic completeness under Restricted/Strict).
    //    Dev sandbox=off keeps soft apply so iterative typecheck remains
    //    ergonomic (#2102 AC soft path). force_audit stays true always.
    //    AURA_COERCION_PROVENANCE_REJECT always wins when set.
    {
        using aura::compiler::apply_coercion_provenance_reject_env_override;
        using aura::compiler::apply_production_coercion_provenance_defaults;
        apply_production_coercion_provenance_defaults(/*dev_sandbox_off=*/dev_off);
        (void)apply_coercion_provenance_reject_env_override();
    }
}

} // namespace aura::compiler::security

#endif // AURA_COMPILER_SECURITY_DEFAULTS_HH
