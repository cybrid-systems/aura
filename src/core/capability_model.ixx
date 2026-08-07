// capability_model.ixx — module re-export of Capability / Effect SSOT.
//
// Full check_and_record_effect + CapabilityRegistry live in
// capability_model.hh. This unit only re-exports for C++ module
// consumers (resource_quota / lifetime_pin pattern).
//
// Do NOT reintroduce a second Effect enum / registry scaffold here
// (prior stub drifted from the header authority).

module;
#include "core/capability_model.hh"

export module aura.core.capability_model;

import std;

export namespace aura::core::capability {

using ::aura::core::capability::kCapabilityModelIssue;
using ::aura::core::capability::kCapabilityModelPhase;
using ::aura::core::capability::kDefaultGrantEpochRetainWindowMultiTenant;
using ::aura::core::capability::kDefaultGrantEpochRetainWindowRestricted;
using ::aura::core::capability::kEffectEpochUnifyIssue;
using ::aura::core::capability::kGrantEpochFiberBindIssue;
using ::aura::core::capability::kGrantEpochRetainRestrictedIssue;
using ::aura::core::capability::kGrantEpochRetainWindowIssue;
using ::aura::core::capability::kHardFiberIsolationIssue;

using ::aura::core::capability::Effect;
using ::aura::core::capability::operator|;
using ::aura::core::capability::operator&;
using ::aura::core::capability::has_effect;

using ::aura::core::capability::AtomicEffectSandboxMode;
using ::aura::core::capability::AtomicTenantId;
using ::aura::core::capability::CapabilityEffectMetrics;
using ::aura::core::capability::CapabilityGrant;
using ::aura::core::capability::CapabilityRegistry;
using ::aura::core::capability::EffectAuditEntry;
using ::aura::core::capability::EffectProvenance;
using ::aura::core::capability::EffectSandboxMode;
using ::aura::core::capability::MacroSelfEvoCheck;
using ::aura::core::capability::MacroSelfEvoPolicy;
using ::aura::core::capability::PublishedAuditSlot;
using ::aura::core::capability::RegistryStateSnapshot;
using ::aura::core::capability::TenantId;

using ::aura::core::capability::CapabilityEffectStatsSnapshot;
using ::aura::core::capability::check_and_record_effect;
using ::aura::core::capability::check_macro_self_evo;
using ::aura::core::capability::effect_fiber_id_or;
using ::aura::core::capability::effect_for_cap_name;
using ::aura::core::capability::g_capability_effect_metrics;
using ::aura::core::capability::g_capability_registry;
using ::aura::core::capability::g_effect_fiber_id_override;
using ::aura::core::capability::grant_epoch_window_bump_trampoline;
using ::aura::core::capability::install_grant_epoch_window_hook;
using ::aura::core::capability::reset_capability_effects_for_test;
using ::aura::core::capability::set_effect_fiber_id_override;
using ::aura::core::capability::snapshot_capability_effect_stats;

} // namespace aura::core::capability
