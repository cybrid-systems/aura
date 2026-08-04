// evaluator_mutation_boundary.cpp — Wave 3a/4: MutationBoundaryGuard + enter/exit_mutation_boundary
// out-of-line aura.compiler.evaluator module partition.
//
// Nested class declaration remains in evaluator.ixx (needs private Evaluator
// access). Heavy RAII paths (try_acquire, AcquireTag ctor, dtor, move,
// enable_fine_rollback) and enter/exit_mutation_boundary live here so
// evaluator.ixx stays a thinner interface.

module;

// Issue #221: PCV header in GMF (same as evaluator.ixx) so enter_mutation_boundary
// can name PersistentChildVector in the checkpoint snapshot type.
#include "../core/persistent_child_vector.hh"
#include "../core/layout_stamp.hh"       // Issue #2170: LayoutStamp capture + publisher
#include "../core/workspace_epoch.hh"    // Issue #2170: current_mutation_epoch() for capture
#include "coercion_provenance_policy.hh" // Issue #2640: g_coercion_provenance_miss_force_audit_total + blame_soft_escalate_* + consume_provenance_miss_for_boundary

#include "observability_metrics.h"
#include "lock_order_audit.h"
#include "gc_coord_scope.h" // Issue #2131: pin → cascade → audit
#include "core/gc_hooks.h"
#include "core/resource_quota.hh"
#include "security_capabilities.h"          // aura_fiber_current_id
#include "aura_jit_bridge.h"                // aura_invoke_long_mutation_scheduler_hook
#include "ownership_escape_lowering_gate.h" // Issue #2309: aura_escape_move_gate_clear + rollback counter
                                            // + aura_aot_func_table_epoch +
                                            //   aura_jit_batch_deopt_for (+ empty-name
                                            //   deopt-all, Issue #2162)
#include "compiler/hot_update_registry.hh"   // Issue #2090: AuraJITHotUpdateRegistry
                                             //   C-linkage shims —
                                             // aura_hot_update_should_throttle_reemit
                                             // aura_hot_update_on_reemit_throttled
                                             // aura_hot_update_notify_epoch_bump
                                             // aura_hot_update_reemit_provider_wired
                                             // aura_reemit_aot_for_dirty
#include "typed_mutation_audit.h"            // Issue #1589 / #1614 / #1894 / #2145
#include "core/sandbox.hh"                   // Issue #2145 Strict hard-gate
#include "core/provenance_tracker.hh"        // Issue #2222: boundary LinearEnforce Strict hold
#include "core/arena_auto_policy_stats.h"    // in_render_hotpath
#include "core/densify_consistency_report.h" // Issue #2341: DensifyConsistencyReport + counter
#include "core/moving_densify_health.hh"     // Issue #2619: Agent Moving densify health
#include "mutation_boundary_shared_exit.h"   // Issue #2600: shared exit helper (soft + full Guard)
#include "core/post_compact_lifecycle.hh"    // Issue #2436: canonical post-compact order
#include "compiler/frame_budget.hh"          // Issue #2137 frame-budget cascade isolation
#include "compiler/mutation_hold_budget.h"   // Issue #2313: mutation_hold_budget_us()
#include "serve/fiber.h"                     // Issue #2184: publish MutationSafetySnapshot
#include "serve/multi_fiber_mailbox.h"       // Issue #2347: clear recv boundary reject window
#include "compiler/shape_profiler.h"         // Issue #2255: current_global_shape_version
#include "orch/security_schedule_gate.h"     // Issue #2630: evaluate_security_schedule admit
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module aura.compiler.evaluator;

import aura.core.lifetime_pin;
import aura.compiler.coercion_map;    // Issue #2102: provenance-miss force-audit
import aura.compiler.root_remap_pass; // Issue #2341: last_root_remap_any_fail
import aura.compiler.ir_soa;          // Issue #2432: current_ir_soa_generation_fence
import aura.compiler.type_checker;    // Issue #2608: maybe_persist_occurrence_snapshot

extern "C" void aura_periodic_epoch_invariant_walk_if_due(void);


// Issue #2021: snapshot macro depth / concurrent peak into CompilerMetrics
// on outermost MutationBoundaryGuard exit (module-safe C entry).
extern "C" void aura_macro_hygiene_snapshot_metrics(void* metrics_ptr) noexcept;
// Issue #2210: JIT/Interpreter equivalence oracle (C ABI from ir_cache_pure).
extern "C" int aura_jit_equivalence_enabled(void) noexcept;
extern "C" int aura_check_primcall_equivalence(std::uint64_t interp_bits,
                                               std::uint64_t jit_bits) noexcept;
extern "C" std::uint64_t aura_jit_equivalence_runs_v_read(void) noexcept;
extern "C" std::uint64_t aura_jit_equivalence_ok_v_read(void) noexcept;
extern "C" std::uint64_t aura_jit_equivalence_mismatch_v_read(void) noexcept;
extern "C" std::uint64_t aura_jit_equivalence_deopt_force_v_read(void) noexcept;


// Issue #2641: production-default OccurrenceGoal persist ON. The
// `TypeChecker::maybe_persist_occurrence_snapshot` method exists (per #2608)
// but was never called from the dtor's outermost-success exit, so production
// hosts got persist opt-in (#2608 default = Soft OFF, env unset →
// production_defaults_active() makes it ON, but the dtor call was missing).
// This helper is what the dtor's outermost-success branch should call.
// Soft / sandbox=off / env=0 paths return 0 from the inner gate, so the
// call is a no-op in those cases (preserves #2608 AC2 zero-cost).
extern "C" void aura_outermost_success_persist_occurrence(void* ev_ptr,
                                                          std::uint64_t mutation_id) noexcept;
