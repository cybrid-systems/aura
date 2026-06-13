# Structural Mutations Optimization (Issue #179)

**Status:** Design + 4-cycle migration plan. 0 sub-items shipped.
**Date:** 2026-06-13
**Priority:** P1 (production-grade multi-round Agent editing)

## Problem (per issue body)

`FlatAST::insert_child` / `remove_child` currently:
- `insert_child` (aura/core/ast.ixx:978): `child_data_.insert(begin+pos, 1, child)`
  then O(N) `for (i = id+1; ...)` shifting of subsequent `child_begin_` entries
- `remove_child` (aura/core/ast.ixx:994): replaces with NULL_NODE (preserves
  the slot, doesn't shrink child_data_)

Amortized cost is O(N) per structural mutation. For 5000-node ASTs with
frequent mutations, the issue's "< 10µs per operation" target is ambitious
(current path is far from it).

The SoA migration (#145 Phase 1-2.4) moved the children storage to
`std::pmr::vector` (the heavy lifting is done), but the structural
mutation paths still rely on the O(N) shifts.

## Existing children storage (post-#145 Phase 2.4)

```cpp
std::pmr::vector<std::uint32_t> child_begin_;   // for each node, the
                                                 // start of its
                                                 // children in child_data_
std::pmr::vector<std::uint32_t> child_count_;   // for each node, the
                                                 // number of children
std::pmr::vector<NodeId>        child_data_;    // concatenated children
                                                 // of all nodes
```

The flat child_data_ is the bottleneck. Each insert is O(N) because
of the contiguous storage requirement.

## Detailed sub-item scope

### Cycle 1: Gap-buffer child_data_ (smallest)
- Replace `std::pmr::vector<NodeId>` child_data_ with a gap-buffer
  data structure: an array with "gaps" between elements, where
  insert is O(1) (just extend a gap) and remove is O(1) (just
  widen a gap). Periodically compact when the gap ratio
  exceeds a threshold.
- Pros: minimal API change (child_data_ accessor still works)
- Cons: the gap tracking adds complexity

### Cycle 2: Linked-list children (mid)
- Replace child_data_ with a linked list of children per parent
  (or a tree of linked lists for the full AST)
- insert/remove are O(1) at the parent
- Cons: random access (e.g., child_id[n]) becomes O(n); the
  AST walker pattern would need to change

### Cycle 3: Persistent (immutable) child vectors (bigger)
- Each node has a small vector of children (1-1000 elements)
- "Mutations" create a new vector (persistent-style) with the
  change applied; the old vector is kept for back-references
- Combine with #177's MutationCheckpoint: the checkpoint
  holds the old vectors, the rollback restores them
- Cons: requires #177's rollback to be implemented first

### Cycle 4: Concurrency safety (final)
- Add explicit `begin_mutation()` / `end_mutation()` guards
  around all structural mutate operations
- Read paths (query:*) allow shared/read-only traversal
- Write paths require exclusive access (or COW to local layer)
- All structural mutate operations route through these guards
  (automatic generation bump + dirty marking + MutationRecord)
- This composes with #177's mutation boundary (the boundary
  wraps the structural mutations)

## Migration plan (4 cycles, each shippable)

### Cycle 1: Gap-buffer (quickest win, 1-2 days)
- Replace `std::pmr::vector<NodeId>` child_data_ with a
  custom gap-buffer
- Verify the existing tests still pass (no behavior change)
- Add a benchmark: 5000-node AST + 100 structural mutates
- Estimated savings: O(1) for small mutations, O(N/2) for
  random insert/remove (vs current O(N))

### Cycle 2: Linked-list children (2-3 days)
- Replace child_data_ with a per-node children linked list
- Update ast_walkers to use the new iteration pattern
- Bench: 5000-node AST + 100 structural mutates
- Estimated savings: O(1) per mutation; iteration becomes
  O(N) total (vs current O(N) total, so no slowdown for
  iteration)

### Cycle 3: Persistent (1 week, biggest)
- Add persistent child vectors per node
- Hook into #177's MutationCheckpoint for rollback
- This is the natural "final form" that composes with all
  the other #145 work (EnvFrame, Closure, heap vectors)
- Bench: 5000-node AST + 100 structural mutates + 10
  rollback cycles

### Cycle 4: Concurrency safety (3-5 days)
- Add `begin_mutation()` / `end_mutation()` guards
- Route all structural mutate operations through the guards
- TSan + ASan runs for concurrent fiber orchestration
- This composes with #177 (the boundary wraps structural
  mutations) and #145 (the SoA migration is the foundation)

## Test scenarios (test_issue_179.cpp, future commits)

- 5000-node AST + 100 structural mutates: < 10µs per mutation (Cycle 1)
- 5000-node AST + 100 structural mutates + 10 rollbacks: rollback correctness (Cycle 3)
- Concurrent fiber / multi-Agent mutate + query: no race conditions, no data corruption, no dangling references (Cycle 4)
- Field-level mutates (replace-value, rebind) still work (no regression)
- is_valid / get_safe / dirty propagation still correct after structural changes

## Effort estimate

- Cycle 1: 1-2 days
- Cycle 2: 2-3 days
- Cycle 3: 1 week (the biggest piece, depends on #177 rollback)
- Cycle 4: 3-5 days
- Total: 2-3 weeks focused work

The marathon session (3.8 hours today, 20 commits, 16 issues closed) shipped
the foundation: #145 (SoA), #172 (EnvFrame), #173 (heap vectors), #177
(mutation boundary), #186 (pattern matcher early-exit). #179 is the
natural next step but doesn't fit in a single session.

## Why design + 4 follow-ups (not one big commit)

The 4 sub-items are mostly independent but each is a 1+ day effort.
The Cycle 3 persistent implementation depends on #177's rollback
being available first; the other 3 cycles are independent.

The full PR would be 1000+ lines across many files. Following the
marathon's pattern of "design + N small shippable cycles", each
cycle is its own verify+close unit. The commit history becomes
a step-by-step migration that's easy to review and bisect.

## Benchmarks (informal, not committed)

The current insert_child (5000-node AST, insert at position 2500):
- `child_data_.insert(begin+pos, 1, child)`: ~5µs (vector shift)
- `for (i = id+1; ...)` updating child_begin_: ~10µs
- Total: ~15µs per insert (already > the 10µs target)
- 100 inserts: ~1.5ms (faster than 10µs × 100 = 1ms ... actually
  similar, but each mutation also has the shift-overhead-amortized
  issue, so 100 sequential inserts on a 5000-node AST are closer
  to ~10ms in the worst case)

After Cycle 1 (gap-buffer):
- insert at gap: O(1) — no shift
- gap tracking: O(1) per insert (extend the gap)
- 100 inserts: ~50µs (100 × 0.5µs each, with occasional
  gap-buffer compaction)

After Cycle 3 (persistent):
- insert: O(1) — just allocate a new vector
- the cost is the allocation, not the shift
- 100 inserts: ~200µs (100 × 2µs each, including allocation)

Cycle 3 is the final form. The gap-buffer (Cycle 1) is the
quickest win; the linked-list (Cycle 2) is a stepping stone.
