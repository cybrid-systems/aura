// security_defaults.hh — Issue #2076 / #2053 production security defaults.
//
// Header form so main / tests / evaluator TUs share one definition without
// module-attachment linkage breaks (free functions defined inside a module
// partition are not visible to non-module callers via a plain header decl).

#ifndef AURA_COMPILER_SECURITY_DEFAULTS_HH
#define AURA_COMPILER_SECURITY_DEFAULTS_HH

#include "typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <cstdlib>
#include <cstdint>
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

// Issue #2053: production multi-tenant AI security defaults (single entry).
// Applies (in order):
//   1. AURA_SANDBOX → Restricted (default) | off | strict  (#2076)
//   2. AURA_MULTI_TENANT=1|true|yes → escalate to Strict
//   3. TypedMutationAudit Full (or AURA_TYPED_AUDIT=sampled|off|full)
//   4. AURA_MUTATION_AUDIT_WAL or AURA_PERSIST_DIR → enable WAL when set
// Dev/test: AURA_SANDBOX=off restores Off + Sampled/4 audit.
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
    if (!dev_off) {
        const char* mt = std::getenv("AURA_MULTI_TENANT");
        if (mt && *mt) {
            std::string_view mv(mt);
            if (mv == "1" || mv == "true" || mv == "yes" || mv == "on") {
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

    // 4) Mutation audit WAL: enable when path env is set (single-flag).
    //    Skipped under AURA_SANDBOX=off so tests do not spill WAL files.
    if (!dev_off) {
        const char* wal = std::getenv("AURA_MUTATION_AUDIT_WAL");
        if (!wal || !*wal)
            wal = std::getenv("AURA_PERSIST_DIR");
        if (wal && *wal) {
            // Process-wide enable; ring replay happens when an Evaluator
            // later calls enable_mutation_audit_wal on the same path.
            (void)g_mutation_audit_wal().enable(std::string_view(wal), nullptr, 0);
        }
    }
}

} // namespace aura::compiler::security

#endif // AURA_COMPILER_SECURITY_DEFAULTS_HH
