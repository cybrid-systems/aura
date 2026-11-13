// linear_frame_sweep_budget.hh — Issue #5274.
//
// The outermost MutationBoundaryGuard runs the linear post-mutate frame
// sweep (linear_post_mutate_enforce_all) several times per mutate. The
// sweep used to walk EVERY slot of the env_frames_ arena on every call,
// taking one env-frame shard lock per slot. In a long-lived serve process
// the arena extent is the append-only high-water mark (a slot is only ever
// reused through the GC sweeper freelist, which stands down whenever a
// foreign tree-walker body is parked), so the per-boundary cost grew with
// the number of frames the process had ever allocated: 65ms -> 973ms
// holds in a 240s game run, which tripped the #2349 hold-SLO
// circuit-breaker and turned every later mutate:rebind into a rollback.
//
// Two bounds, both exactness-preserving for the check that matters:
//   1. a frame whose bindings_linear_ownership_state_ is empty cannot
//      hold a Moved entry, so the sweep skips it without taking its
//      shard lock (same answer, no lock);
//   2. the raw slot walk is sliced: each boundary examines at most
//      kLinearFrameSweepSlice slots, continuing from the last cursor, so
//      a boundary's cost is bounded and every slot is still covered
//      within ceil(extent / slice) boundaries.
//
// Deferred on purpose: slot coverage of the frame sweep (never the
// per-closure linear capture scan, which stays exact and complete on
// every boundary). The cursor makes the deferral round-robin, so a live
// linear frame is re-examined within one full pass of the arena.
#ifndef AURA_COMPILER_LINEAR_FRAME_SWEEP_BUDGET_HH
#define AURA_COMPILER_LINEAR_FRAME_SWEEP_BUDGET_HH

#include <cstddef>

namespace aura::compiler {

inline constexpr std::size_t kLinearFrameSweepSlice = 8192;

} // namespace aura::compiler

#endif // AURA_COMPILER_LINEAR_FRAME_SWEEP_BUDGET_HH
