// lifetime_pin.ixx — module re-export of the LifetimePin SSOT.
//
// Full implementation lives in lifetime_pin.hh (header-only). This unit
// only re-exports for C++ module consumers (resource_quota.ixx pattern).
//
// Rationale (architecture review / dual-track elimination):
//   Prior Phase-2 .hh + Phase-3 .ixx drift meant header consumers
//   (JIT linear pin, security_defaults) and module consumers (arena
//   densify, MutationBoundaryGuard bulk restamp) used different
//   registries → pin-or-remap / stats split-brain risk.
//
// Do NOT reintroduce a second LifetimePin body here.
// Issue history (implementation still in .hh banner + body):
//   #2000 Phase 2 pin, #2048 FFI batch, #2265 remap, #2342 shards,
//   #2374/#2375 all-shard bulk, #2280 linear roots, #2298/#2363/#2709
//   general-object pin, #2496/#2597 required-mode gate.

module;
#include "core/lifetime_pin.hh"

export module aura.core.lifetime_pin;

import std;

export namespace aura::core::lifetime {

// Phase / inventory stamps
using ::aura::core::lifetime::kGeneralObjectPinAdoptIssue;
using ::aura::core::lifetime::kGeneralObjectPinAdoptSiteCount;
using ::aura::core::lifetime::kGeneralObjectPinAutoWireIssue;
using ::aura::core::lifetime::kGeneralObjectPinCoverageGateIssue;
using ::aura::core::lifetime::kGeneralObjectPinIssue;
using ::aura::core::lifetime::kLifetimePinPhase;

// Stats + process atomics
using ::aura::core::lifetime::apply_general_object_pin_required_env;
using ::aura::core::lifetime::clear_general_object_pin_required_breach;
using ::aura::core::lifetime::g_general_object_pin_required_breach;
using ::aura::core::lifetime::g_general_object_pin_required_breach_densify_fail_total;
using ::aura::core::lifetime::g_general_object_pin_required_enforced_total;
using ::aura::core::lifetime::g_general_object_pin_required_pref;
using ::aura::core::lifetime::g_lifetime_pin_stats;
using ::aura::core::lifetime::g_linear_pin_miss_total;
using ::aura::core::lifetime::g_linear_pin_total;
using ::aura::core::lifetime::g_linear_unpin_total;
using ::aura::core::lifetime::g_moving_compact_bytes_reclaimed_total;
using ::aura::core::lifetime::g_moving_compact_count_total;
using ::aura::core::lifetime::g_moving_compact_pin_contract_fail_total;
using ::aura::core::lifetime::g_moving_compact_pin_hits_total;
using ::aura::core::lifetime::g_moving_compact_remap_us_total;
using ::aura::core::lifetime::g_pin_registry_lock_wait_us_total;
using ::aura::core::lifetime::general_object_pin_required_active;
using ::aura::core::lifetime::general_object_pin_required_breach_active;
using ::aura::core::lifetime::kGeneralObjectPinRequiredProdDefaultIssue;
using ::aura::core::lifetime::LifetimePinStats;
using ::aura::core::lifetime::pin_registry_lock_wait_us_total;

// Pin owner + pin type
using ::aura::core::lifetime::GeneralObjectPin;
using ::aura::core::lifetime::LifetimePin;
using ::aura::core::lifetime::PinOwner;

// Sharded registry
using ::aura::core::lifetime::kPinRegistryShardCount;
using ::aura::core::lifetime::kPinRegistryShardMask;
using ::aura::core::lifetime::live_pin_count;
using ::aura::core::lifetime::pin_registry_shard_index;
using ::aura::core::lifetime::pin_registry_shard_max_pin_count;
using ::aura::core::lifetime::pin_registry_shard_pin_count;
using ::aura::core::lifetime::pin_registry_shards;
using ::aura::core::lifetime::pin_registry_total_pinned_count;
using ::aura::core::lifetime::PinRegistryShard;

// Bulk compact / densify hooks (SSOT in .hh; re-exported here)
using ::aura::core::lifetime::invalidate_all_pins_for_arena;
using ::aura::core::lifetime::invalidate_pins_not_in_new_addrs;
using ::aura::core::lifetime::lifetime_pin_contract_fail_total;
using ::aura::core::lifetime::lifetime_pin_remap_miss_total;
using ::aura::core::lifetime::lifetime_pin_remap_total;
using ::aura::core::lifetime::remap_pins_pointing_to;
using ::aura::core::lifetime::RemapResult;
using ::aura::core::lifetime::restamp_all_pins_for_arena;
using ::aura::core::lifetime::verify_linear_pins_under_moving_compact;
using ::aura::core::lifetime::verify_pins_under_moving_compact;

// Linear roots
using ::aura::core::lifetime::linear_root_snapshot;
using ::aura::core::lifetime::linear_roots;
using ::aura::core::lifetime::linear_roots_mtx;
using ::aura::core::lifetime::LinearRootSnapshot;
using ::aura::core::lifetime::pin_linear_root;
using ::aura::core::lifetime::reset_linear_roots_for_test;
using ::aura::core::lifetime::unpin_linear_root;

// General-object pin protocol
using ::aura::core::lifetime::general_object_pin_adopt_site_count;
using ::aura::core::lifetime::general_object_pin_adopt_site_count_v_read;
using ::aura::core::lifetime::general_object_pin_auto_wire_total_v_read;
using ::aura::core::lifetime::general_object_pin_exempt_total_v_read;
using ::aura::core::lifetime::note_general_object_pin_mutate_wire;
using ::aura::core::lifetime::pin_or_fail;
using ::aura::core::lifetime::validate_general_object;
using ::aura::core::lifetime::wire_general_object_create_pair;
using ::aura::core::lifetime::wire_general_object_create_pair_or_exempt;
using ::aura::core::lifetime::wire_general_object_create_pair_or_required_fail;

// Macro re-export for module TUs that import this module.
// GENERAL_OBJECT_PIN_EXEMPT is a preprocessor macro from the header GMF;
// module importers that need it should also see it via the include in
// their own GMF, or rely on the header when non-module. Left documented
// here for discoverability (macro cannot be `using`-exported).

} // namespace aura::core::lifetime
