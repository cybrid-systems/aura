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
#include "compiler/lock_order_audit.h"     // Issue #2557 production soft lock-order audit
#include "compiler/mutate_type_gate.hh"    // Issue #2219 post-mutate type gate
#include "compiler/pipeline_policy.hh"     // Issue #2213 tree-walker fallback production gate
#include "core/gc_hooks.h"                 // Issue #2338: gc_defer production lock wire-up
#include "core/capability_model.hh"
#include "core/lifetime_pin.hh" // #2597 g_general_object_pin_required_pref
#include "core/mutation_audit_wal.hh"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"
#include "core/arena_auto_policy_stats.h" // #2596 g_moving_untracked_hard_abort_pref

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>

// Issue #2369: force live-closure remap name-fallback off under production.
// Declared in runtime_shared.h / aura_jit_runtime.cpp; weak stub in bridge stub.
extern "C" void aura_set_remap_name_fallback_enabled(int v);

// Issue #2501: post-bump epoch invariant mode (0=off, 1=soft, 2=hard).
// Defined in aura_jit_bridge.cpp; weak stub may exist for non-JIT links.
extern "C" void aura_set_epoch_invariant_mode(int mode);
extern "C" int aura_epoch_invariant_mode(void);

// Issue #2372: production Soft steal-snapshot lock (defined in serve/fiber.cpp).
// Soft env ignored when locked; strong force-deopt ABI required under production.
namespace aura::serve {
void set_steal_snapshot_soft_production_locked(bool v) noexcept;
}

namespace aura::compiler::security {

// Issue #2584: commercial tenant config profile — optional hardening for
// commercial Restricted deployments (fiber-level grant isolation by
// default). Read once during apply_production_security_defaults (step 6);
// cache exposed via is_commercial_tenant_profile() so query:security-posture
// (#2534) can surface the active profile flag to Agents. Function-local
// static + inline function give single shared instance across TUs (C++17
// inline-variable rules).
inline std::atomic<bool>& commercial_tenant_profile_flag() noexcept {
    static std::atomic<bool> v{false};
    return v;
}

// Read the cached commercial profile flag (true when AURA_COMMERCIAL_TENANT
// was set under !dev_off and not explicitly overridden off by
// AURA_HARD_FIBER_ISOLATION=0|false|off|...).
[[nodiscard]] inline bool is_commercial_tenant_profile() noexcept {
    return commercial_tenant_profile_flag().load(std::memory_order_acquire);
}

// Test helper: reset the cached flag (unit tests need a clean slate).
inline void reset_commercial_tenant_profile_for_test(bool v = false) noexcept {
    commercial_tenant_profile_flag().store(v, std::memory_order_release);
}

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
    // Issue #2657: route through the process-wide authority. set_mode
    // atomically updates sandbox_mode atomic + plain enum +
    // CapabilityRegistry + workspace_isolation strict link +
    // provenance_tracker policy. The old three-write sequence
    // (set_mode + reg.sandbox_mode = + set_strict_sandbox_linked) is
    // a single call now; the registers can no longer drift.
    set_mode(static_cast<SandboxMode>(mode));
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
//   8. LinearEnforceMode (#2207 / #2182 / #2222 / refine #2103):
//        - process default Strict (incomplete linear×provenance hard-fails)
//        - Soft only via set_linear_enforce_mode(Soft), AURA_LINEAR_ENFORCE=soft,
//          or AURA_SANDBOX=off (unit Soft-path ergonomics)
//        - AURA_LINEAR_ENFORCE=soft|strict always wins when set (canary)
//        - MutationBoundary fiber hold forces *effective* Strict (#2222)
//          even when process Soft (early detect; #2108 composite remains)
//   9. Coercion provenance miss (#2185 / refine #2102) + blame commit
//      hard-require (#2221):
//        - production → reject_apply_on_provenance_miss (no CoercionNode)
//        - production → require_blame_complete_on_commit (composite gate)
//        - AURA_SANDBOX=off → soft apply + sentinel + observe-only commit
//        - force_audit_on_provenance_miss always true
//        - AURA_COERCION_PROVENANCE_REJECT=reject|soft canary override
//        - AURA_BLAME_COMMIT_REQUIRE=on|off canary override
//  10. Pipeline strict / tree-walker fallback (#2213):
//        - production → Forbidden (hard-fail; never silent tree-walker)
//        - AURA_SANDBOX=off → Allow (unit Soft ergonomics)
//        - AURA_PIPELINE_STRICT=0|allow|force-soa|1|forbid overrides
//  11. Mutate type gate (#2219):
//        - production → Hard (match exhaustiveness / TypeError reject)
//        - AURA_SANDBOX=off → Soft (unit Soft-path ergonomics)
//        - AURA_MUTATE_TYPE_GATE=soft|hard always wins when set
//  12. Lock-order audit (#2557 / refine #2354):
//        - production Restricted/Strict → soft (metrics-only inversions)
//        - AURA_SANDBOX=off → OFF (zero atomics; unit Soft path)
//        - AURA_LOCK_ORDER_CANARY=1 → hard abort (always wins)
//        - AURA_LOCK_ORDER_AUDIT=1|0|off overrides soft/off when set
// Dev/test: AURA_SANDBOX=off restores Off + Sampled/4 audit + no WAL + soft
// fiber + Soft linear process mode (boundary still forces effective Strict
// when force-on-boundary is on, #2222) + soft coercion apply + observe-only
// blame commit + tree-walker Allow + Soft mutate type gate + lock-order OFF.
inline void apply_production_security_defaults() noexcept {
    // Issue #2688 / #2835: production-default hard_fiber_isolation + grant
    // epoch retain window. Arming contract:
    //   multi-tenant → hard=true + K=64 (sandbox may stay Restricted #2835)
    //   pure Restricted single-tenant → K=16 hard=false (#2536 soft share)
    //   Soft / sandbox=off → K=0 hard=false
    // Env AURA_HARD_FIBER_ISOLATION / AURA_GRANT_EPOCH_RETAIN always win.
    using namespace ::aura::core::sandbox;
    using namespace ::aura::core::capability;
    using namespace ::aura::core::workspace_isolation;
    using namespace ::aura::core::audit_wal;
    using namespace ::aura::compiler::typed_audit;

    // 1) Sandbox: Restricted by default (#2076).
    apply_aura_sandbox_env();

    const char* sandbox_e = std::getenv("AURA_SANDBOX");
    const bool dev_off = sandbox_e && *sandbox_e && std::string_view(sandbox_e) == "off";

    // 2) Multi-tenant profile when AURA_MULTI_TENANT is set (unless
    //    AURA_SANDBOX=off). Issue #2835: do **not** force Strict — commercial
    //    multi-tenant often stays Restricted for latency while still needing
    //    fiber-level grant isolation (step 6 hard_fiber). Explicit
    //    AURA_SANDBOX=strict remains Strict. Issue #2657 set_mode is still
    //    the sole writer when operators choose strict/off/restricted via env.
    bool multi_tenant = false;
    if (!dev_off) {
        const char* mt = std::getenv("AURA_MULTI_TENANT");
        if (mt && *mt) {
            std::string_view mv(mt);
            if (mv == "1" || mv == "true" || mv == "yes" || mv == "on") {
                multi_tenant = true;
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
    //    Explicit path env always wins. Under multi-tenant OR Strict OR
    //    Restricted, force a durable default dir so commercial deploys
    //    (including single-tenant Restricted) get forensic trail without
    //    tribal knowledge of env vars. AURA_SANDBOX=off never enables WAL
    //    (unit tests must not spill files). Issue #2492: Restricted-only
    //    commercial deploys were silent under deny storms because force_wal
    //    only fired for multi-tenant/Strict — single-tenant Restricted
    //    (the #2076 production default) lost early forensic events to
    //    ring wrap (1024 entries). Adding restricted closes the gap.
    if (!dev_off) {
        const bool strict = g_sandbox_state().mode == SandboxMode::Strict ||
                            g_capability_registry().sandbox_mode == EffectSandboxMode::Strict;
        const bool restricted =
            g_sandbox_state().mode == SandboxMode::Restricted ||
            g_capability_registry().sandbox_mode == EffectSandboxMode::Restricted;
        const bool force_wal = multi_tenant || strict || restricted;
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
                    // Issue #2492: distinct counter for Restricted-only force
                    // so Agent dashboards can break out single-tenant vs
                    // multi-tenant/Strict WAL pressure.
                    if (restricted && !multi_tenant && !strict) {
                        g_audit_wal_metrics().audit_wal_forced_by_restricted_total.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
                if (used_default || (force_wal && !has_explicit)) {
                    g_audit_wal_metrics().audit_wal_using_default_dir.store(
                        1, std::memory_order_relaxed);
                    // Quiet by default (metrics already track force/default-dir).
                    // One-shot note only under AURA_VERBOSE=1 so simple REPL
                    // `(+ 1 2)` is not spammed by production security defaults.
                    static const bool verbose = [] {
                        const char* e = std::getenv("AURA_VERBOSE");
                        return e != nullptr && e[0] != '\0' && e[0] != '0';
                    }();
                    if (verbose) {
                        static std::atomic<bool> warned{false};
                        bool expected = false;
                        if (warned.compare_exchange_strong(expected, true,
                                                           std::memory_order_relaxed)) {
                            std::fprintf(stderr,
                                         "[aura] mutation audit WAL forced under "
                                         "multi-tenant/Strict/Restricted (#2492) → %s "
                                         "(set AURA_MUTATION_AUDIT_WAL to override)\n",
                                         dir.c_str());
                        }
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

    // 5b) Issue #2205 / #2208: HotUpdate reemit boundary policy — production
    //     default Defer (fail-closed under multi-fiber; SoftEnter is not
    //     steal-safe). SoftEnter only via AURA_REEMIT_SOFT_ENTER=1 (or
    //     explicit setter in tests). Process default remains Defer.
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

    // 5c) Issue #2369: live-closure remap sole primary = stable_func_id.
    //     Force name-fallback rewrite off under production (not sandbox=off).
    //     Migration tests may re-enable via aura_set_remap_name_fallback_enabled(1)
    //     only under AURA_SANDBOX=off or after explicitly opting in.
    if (!dev_off)
        ::aura_set_remap_name_fallback_enabled(0);

    // 6) Issue #2151 / #2536 / #2584 / #2835: hard fiber isolation policy.
    //
    //    Contract:
    //      - TenantScope (#2491) is the principal boundary.
    //      - Pure Restricted single-tenant (#2076 default): soft share —
    //        fiber B may use fiber A's grant; mismatch metric only (#2536).
    //      - Multi-tenant profile (AURA_MULTI_TENANT): hard fiber deny on
    //        grant_fiber_id mismatch (#2835) even when sandbox stays
    //        Restricted for latency (no forced Strict).
    //      - Commercial profile (AURA_COMMERCIAL_TENANT): hard under
    //        Restricted (#2584).
    //      - multi_tenant + Strict (AURA_SANDBOX=strict): hard (#2151).
    //
    //    Defaults when AURA_HARD_FIBER_ISOLATION unset:
    //      multi_tenant            → hard=true  (#2835 / #2151)
    //      commercial_tenant       → hard=true  (#2584)
    //      Restricted alone        → hard=false (#2536 soft share)
    //      Strict alone (no multi) → hard=false (unchanged)
    //    Env always wins when set:
    //      AURA_HARD_FIBER_ISOLATION=1|true|yes|on  → hard
    //      AURA_HARD_FIBER_ISOLATION=0|false|off|… → soft (AC3)
    //    AURA_SANDBOX=off forces soft (unit tests).
    if (dev_off) {
        commercial_tenant_profile_flag().store(false, std::memory_order_release);
        g_capability_registry().set_hard_fiber_isolation(false);
    } else {
        const char* ct = std::getenv("AURA_COMMERCIAL_TENANT");
        bool commercial_active = false;
        if (ct && *ct) {
            std::string_view cv(ct);
            commercial_active = (cv == "1" || cv == "true" || cv == "yes" || cv == "on");
        }
        // Explicit off from AURA_HARD_FIBER_ISOLATION wins (AC3).
        const char* hfi = std::getenv("AURA_HARD_FIBER_ISOLATION");
        bool hfi_explicit_off = false;
        if (hfi && *hfi) {
            std::string_view hv(hfi);
            hfi_explicit_off = (hv == "0" || hv == "false" || hv == "off" || hv == "no");
        }
        commercial_tenant_profile_flag().store(commercial_active, std::memory_order_release);
        if (commercial_active && !hfi_explicit_off) {
            // #2584: commercial profile forces hard fiber even under
            // pure Restricted. AURA_HARD_FIBER_ISOLATION=0 still wins.
            g_capability_registry().set_hard_fiber_isolation(true);
        } else if (hfi && *hfi) {
            // Issue #2536: env applies under Restricted as well as Strict.
            std::string_view hv(hfi);
            const bool on = (hv == "1" || hv == "true" || hv == "yes" || hv == "on");
            g_capability_registry().set_hard_fiber_isolation(on);
        } else {
            // Issue #2835: multi-tenant → hard under Restricted or Strict
            // (fiber grant isolation without requiring full Strict sandbox).
            // Pure Restricted single-tenant → soft (#2536).
            // Lineage: #2688 used multi_tenant && strict when multi forced
            // Strict; after #2835 multi alone is sufficient (covers both
            // Restricted multi-tenant AC1 and multi_tenant && strict Strict).
            const bool hard_default = multi_tenant; // was: multi_tenant && strict
            g_capability_registry().set_hard_fiber_isolation(hard_default);
        }
    }

    // 7) Issue #2154 / #2529: sliding grant_min_valid_epoch retain window.
    //    Multi-tenant or Strict → K=64 (#2154). Restricted single-tenant
    //    production (#2076 default) → K=16 (#2529) so privilege-sticky grants
    //    still slide under long Mutation epoch advance. Off/other → K=0
    //    (manual fence only). Env AURA_GRANT_EPOCH_RETAIN always wins when
    //    set. sandbox=off → K=0 + min_valid=0 (unit Soft path).
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
            const bool restricted =
                g_sandbox_state().mode == SandboxMode::Restricted ||
                g_capability_registry().sandbox_mode == EffectSandboxMode::Restricted;
            if (multi_tenant || strict) {
                g_capability_registry().set_grant_epoch_retain_window(
                    kDefaultGrantEpochRetainWindowMultiTenant);
            } else if (restricted) {
                // Issue #2529: anti privilege-sticky under pure Restricted.
                g_capability_registry().set_grant_epoch_retain_window(
                    kDefaultGrantEpochRetainWindowRestricted);
            } else {
                g_capability_registry().set_grant_epoch_retain_window(0);
            }
        }
    }

    // 7b) Issue #2705: hard-close FlatAST global capture fallback under
    //     production multi-tenant / Strict. Soft / AURA_SANDBOX=off stay
    //     permissive (#2687 AC4). Env AURA_HARD_CAPTURE_TENANT always wins
    //     when set (1|true|on|yes / 0|false|off|no). Stricter default =
    //     always refuse process-global stamp when armed (no TLS probe needed);
    //     Evaluator::stamp_stable_ref remains the production authority.
    {
        using aura::core::provenance::set_hard_capture_tenant;
        if (dev_off) {
            set_hard_capture_tenant(false);
        } else {
            const char* hct = std::getenv("AURA_HARD_CAPTURE_TENANT");
            if (hct && *hct) {
                std::string_view hv(hct);
                const bool on = (hv == "1" || hv == "true" || hv == "yes" || hv == "on");
                set_hard_capture_tenant(on);
            } else {
                const bool strict =
                    g_sandbox_state().mode == SandboxMode::Strict ||
                    g_capability_registry().sandbox_mode == EffectSandboxMode::Strict;
                // multi-tenant always escalates to Strict above, so multi_tenant
                // || strict covers both AURA_MULTI_TENANT and AURA_SANDBOX=strict.
                set_hard_capture_tenant(multi_tenant || strict);
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

    // 9) Issue #2185 / #2221: production refuse CoercionNode insert on
    //    incomplete provenance chain + require complete DeltaBlameChain on
    //    composite_txn_commit (forensic completeness under Restricted/Strict).
    //    Dev sandbox=off keeps soft apply + observe-only commit so iterative
    //    typecheck remains ergonomic (#2102 AC soft path). force_audit stays
    //    true always. AURA_COERCION_PROVENANCE_REJECT and
    //    AURA_BLAME_COMMIT_REQUIRE always win when set.
    {
        using aura::compiler::apply_blame_commit_require_env_override;
        using aura::compiler::apply_coercion_provenance_reject_env_override;
        using aura::compiler::apply_production_coercion_provenance_defaults;
        apply_production_coercion_provenance_defaults(/*dev_sandbox_off=*/dev_off);
        (void)apply_coercion_provenance_reject_env_override();
        (void)apply_blame_commit_require_env_override();
    }

    // 10) Issue #2213: production forbids silent tree-walker fallback that
    //     would abandon SoA + Impact + partial-relower under AI mutate.
    //     Forbidden hard-fail by default in production; Allow under
    //     AURA_SANDBOX=off. AURA_PIPELINE_STRICT=0|allow|force-soa|1 overrides.
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/dev_off);

    // 11) Issue #2219 + #2279: post-mutate type gate — Hard under production
    //     so Agents cannot treat soft-passed typecheck as success. Soft under
    //     AURA_SANDBOX=off. AURA_MUTATE_TYPE_GATE=soft|hard overrides (always
    //     wins when set). Issue #2279 production lock contract:
    //       - AURA_ALLOW_SOFT_TYPE_GATE=1 → explicit dev-only Soft escape
    //         under production (sets mutate_type_gate::allow_soft_override;
    //         alarm bumps on first mutate under lock).
    //       - apply_production_security_defaults flips production_locked 0→1
    //         AFTER apply_production_defaults, so any Soft under prod that
    //         survived (env override + override allowed) bumps the alarm
    //         counter on the next run_post_mutate_typecheck_no_lock.
    //       - AURA_HARD_TYPE_GATE_ABORT=1 → process abort on Soft-in-prod
    //         detection (fail-closed; canary).
    mutate_type_gate::apply_production_defaults(/*dev_sandbox_off=*/dev_off);
    // Issue #2279: stamp the lock state. Locked iff production profile
    // (sandbox != off). AURA_ALLOW_SOFT_TYPE_GATE does NOT unlock — it
    // just allows the existing Soft mode to persist under the lock with
    // a metric alarm (so a mis-deployed binary is observable, not silent).
    mutate_type_gate::set_production_locked(!dev_off);
    // Issue #2338: production lock for gc_defer_overflow_policy. Default
    // to HardFail (not ProcessWide silent fallback) under production.
    // Dev / AURA_SANDBOX=off keeps the legacy ProcessWide for stress tests
    // that intentionally fill the table. Lock is captured at first
    // gc_defer_overflow_policy() call (lazy cache).
    aura::gc_hooks::set_gc_defer_production_locked(!dev_off);
    // Issue #2372: production hard-forbid AURA_STEAL_SNAPSHOT_SOFT under
    // production security defaults. Soft remains usable under
    // AURA_SANDBOX=off and via set_steal_snapshot_soft_for_test (test
    // override wins over lock). Multi-worker production builds must
    // also link the strong force-deopt ABI (worker.cpp / fiber_bridge
    // abort on null/weak-noop under this lock).
    aura::serve::set_steal_snapshot_soft_production_locked(!dev_off);
    if (mutate_type_gate::production_locked() && !mutate_type_gate::is_hard()) {
        // Preview alarm: Soft is set at apply-time and the lock just
        // turned on. Bump the counter now so a mis-deployed binary is
        // observable even before the first mutate. The runtime check
        // in check_soft_in_production_or_abort (called from
        // run_post_mutate_typecheck_no_lock) handles AURA_HARD_TYPE_GATE_ABORT.
        mutate_type_gate::check_soft_in_production_or_abort();
    }

    // 12) Issue #2501 / #2541: post-bump epoch invariant soft-on under
    //     production so long-running Agents detect gen-behind AOT/closure
    //     survivors and force MustDeopt + physical slot clear (soft never
    //     aborts). AURA_EPOCH_INVARIANT=soft|hard|0|off still wins.
    //     AURA_SANDBOX=off leaves mode alone (unit Soft path / zero walk
    //     unless tests set mode explicitly).
    if (!dev_off) {
        const char* ei = std::getenv("AURA_EPOCH_INVARIANT");
        if (!ei || !*ei) {
            // Unset under production Restricted → soft (mode 1). #2541 AC1.
            aura_set_epoch_invariant_mode(1);
        } else {
            // Explicit env re-apply (covers "0"/"off" after prior soft set).
            const std::string_view v(ei);
            if (v == "0" || v == "off" || v == "false" || v == "no")
                aura_set_epoch_invariant_mode(0); // #2541 AC5 unit Soft path
            else if (v == "hard")
                aura_set_epoch_invariant_mode(2);
            else if (v == "soft" || v == "1" || v == "true" || v == "on")
                aura_set_epoch_invariant_mode(1);
        }
    }

    // 13) Issue #2557: production soft lock-order audit (metrics-only).
    //     Rank inversions surface on g_lock_inversion_detected_total without
    //     abort risk. Hard canary (AURA_LOCK_ORDER_CANARY=1) still aborts.
    //     AURA_SANDBOX=off → OFF (zero atomics) for unit Soft path.
    //     AURA_LOCK_ORDER_AUDIT=0|off forces off under production; =1 forces soft.
    ::aura::compiler::lock_order::apply_production_lock_order_default(
        /*sandbox_off=*/dev_off);

    // 14) Issue #2596: production default AURA_MOVING_UNTRACKED=hard (align
    //     with Moving default ON, #2256). Closes silent-UAF risk under
    //     sustained agent mutate when #2256's Moving compact production
    //     default ON is paired with #2495's untracked detection (which sets
    //     moving_incomplete_remap + clears pin_contract_held but only
    //     hard-aborts when explicitly env=hard). Operator env always wins
    //     (AC3: AURA_MOVING_UNTRACKED=off under production keeps Soft).
    //     Soft / AURA_SANDBOX=off keeps observe-only unless env=hard
    //     (operator override wins). Existing #2495 hard-abort path at
    //     arena.ixx:1360 reads this pref and sets
    //     moving_blocked_precondition + soft_gated when pref > 0.
    {
        using aura::ast::g_moving_untracked_hard_abort_pref;
        const char* mvt = std::getenv("AURA_MOVING_UNTRACKED");
        int env_pref = -1;
        if (mvt && *mvt) {
            std::string_view mv(mvt);
            if (mv == "hard" || mv == "1" || mv == "on" || mv == "true" || mv == "yes") {
                env_pref = 1;
            } else if (mv == "off" || mv == "0" || mv == "soft" || mv == "false" || mv == "no") {
                env_pref = 0;
            }
        }
        if (env_pref != -1) {
            // Operator env always wins (AC3).
            g_moving_untracked_hard_abort_pref.store(env_pref, std::memory_order_relaxed);
        } else if (!dev_off) {
            // Production default: lock to hard when unset (align with
            // #2256 Moving default ON — silent-UAF risk under sustained
            // agent mutate). Soft / sandbox=off keeps observe-only.
            if (g_moving_untracked_hard_abort_pref.load(std::memory_order_relaxed) < 0) {
                g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
            }
        }
    }

    // 15) Issue #2597 / #2840: production default AURA_GENERAL_OBJECT_PIN=required
    //     (align with Moving default ON + GeneralObjectPin inventory).
    //     Closes the GeneralObjectPin vs render dual-track gap that lets
    //     new mutate/agent/scratch creates land without a pin wire
    //     (creating Moving densify untracked externals — #2495 / #2840).
    //     Operator env always wins (AC3: AURA_GENERAL_OBJECT_PIN=off under
    //     production keeps Soft observe-only). Soft / AURA_SANDBOX=off +
    //     env unset keeps observe-only (pref=-1). Calls the existing
    //     apply_general_object_pin_required_env helper which handles env
    //     parsing (required / off), then applies the production-default
    //     lock when env was unset. #2840: required mode + wire failure
    //     sets sticky densify breach so Moving fail-closes (not observe-only).
    {
        using aura::core::lifetime::apply_general_object_pin_required_env;
        using aura::core::lifetime::g_general_object_pin_required_pref;
        // Operator env parsing first (env always wins when set).
        apply_general_object_pin_required_env();
        // Production-default lock: only when env was unset (still -1).
        // Issue #2840: force required under production (mirror #2596).
        if (g_general_object_pin_required_pref.load(std::memory_order_relaxed) == -1 && !dev_off) {
            g_general_object_pin_required_pref.store(1, std::memory_order_release);
        }
    }
}

} // namespace aura::compiler::security

#endif // AURA_COMPILER_SECURITY_DEFAULTS_HH
