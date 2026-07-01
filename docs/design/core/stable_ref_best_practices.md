# StableNodeRef Best Practices (Issue #347)

The `StableNodeRef` + `generation_` + `node_gen_` mechanism
(#221 / #222) is the production-readiness surface for
holding references to AST nodes across eval / mutate
cycles. This document is the consolidated best-practices
guide for AI agent developers writing long-running
self-modification loops.

## TL;DR — raw `query:children` / `query:parent` are deprecated

> **DEPRECATION (Issue #393 follow-up #249)**: the raw
> Aura primitives `(query:children <node-id>)` and
> `(query:parent <node-id>)` return raw `NodeId` values
> that may be invalidated by any structural mutation.
> **Prefer the stable variants**:
>
> - `(query:children-stable <node-id>)` → list of
>   `(id . gen)` pairs (the `(cons id gen)` encoding
>   used by all `-stable` query primitives)
> - `(query:parent-stable <node-id>)` → single
>   `(id . gen)` pair (or `void` for root)
> - `(query:ref-valid? <pair>)` → `#t`/`#f` (validate a
>   captured `(id . gen)`; uses the flat-style
>   `is_valid_id_gen` check from #393, so respects
>   scoped invalidation — refs in non-bumped subtrees
>   stay valid even after `compile:subtree-bump` on
>   other subtrees)
>
> In C++, the `FlatAST` C++26 modules also ship
> `children_stable(NodeId)` and `parent_stable(NodeId)`
> that return `std::vector<StableNodeRef>` /
> `StableNodeRef` directly. New code should use these
> APIs; existing call sites should migrate when the
> node id is held across a `mutate:*` or `eval-current`
> boundary.
>
> The raw variants are kept for backward compatibility
> and one-shot inspection (see "When to use
> StableNodeRef vs raw NodeId" below), but they should
> NOT be the default for cross-mutate code paths. See
> `docs/api-reference.md` for the canonical pointer.

## Why StableNodeRef exists

A raw `NodeId` is just an index into the workspace's
`FlatAST` parallel-array storage. When nodes are
recycled (via the free-list) or when the flat is
copied-on-write (COW) by the workspace layering, the
index may now refer to a **different node** than it did
before the mutation. A raw `NodeId` becomes a stale
reference without warning.

`StableNodeRef` wraps a `(NodeId, generation_)` pair
where `generation_` is the value of the flat's monotonic
generation counter at the time the ref was created.
Validity is checked via `FlatAST::is_valid(ref)`, which
returns `false` when the node's stored generation
(`node_gen_[id]`) no longer matches the ref's generation
— i.e. the slot was recycled (or COW-copied) after the
ref was created.

## When to use StableNodeRef vs raw NodeId

| Use case | Recommendation |
|---|---|
| One-shot query / inspection (e.g. `(query:node-type 0)`) | Raw `NodeId` is fine — the id is consumed in the same eval cycle. The raw `(query:children …)` / `(query:parent …)` variants are also fine for one-shot reads, but **prefer the `-stable` variants** (see "TL;DR" above) for any code that stores the result. |
| Hold a node ref across a `mutate:*` call | **StableNodeRef** required — the id may be invalidated |
| Hold a node ref across `eval-current` re-invocations | **StableNodeRef** required — eval may trigger compaction that recycles slots |
| Hold a node ref across `(ast:snapshot / ast:restore)` | **StableNodeRef** required — restore rewrites the entire flat |
| Hold a node ref across `(set-code ...)` workspace replacement | Raw `NodeId` invalid (always); use the new flat's `NodeId` mapping |
| Hold a node ref across fiber boundary (yield + resume) | **StableNodeRef** — fiber yield can race with a mutating fiber |
| Cross-`Workspace` reference (one workspace holds a ref into another) | **StableNodeRef** — workspace layering is COW; the ref must be valid against the source workspace |

**Rule of thumb**: if the reference outlives the single
`cs.eval` call that produced it, use `StableNodeRef`.

## Safe patterns for multi-round query-mutate-eval loops

The canonical AI agent loop is:

```cpp
// 1. Get a stable ref to the target
auto ref = cs.get_stable_node_ref(target_node_id);
if (!cs.is_valid(ref)) { /* error: ref stale */ }

// 2. Mutate the target
cs.eval("(mutate:query-and-replace ...)");

// 3. Re-validate the ref (the mutation may have
//    recycled the slot)
if (!cs.is_valid(ref)) {
    // The ref is stale; re-resolve via a fresh query.
    auto fresh_id = cs.eval("(query:defines-by-marker ...)");
    ref = cs.get_stable_node_ref(fresh_id);
}

// 4. Now use ref safely for the rest of the loop.
```

The key insight: **never assume a ref is still valid
after a mutation**. Always re-check with `is_valid` and
have a re-resolution path ready.

### The "ref + retry" pattern

For the common case where the mutation target is
discoverable by a query (e.g. a define by name), the
agent's loop should be:

```cpp
for (int round = 0; round < max_rounds; ++round) {
    auto target_id = cs.eval("(query:defines-by-marker ...)");
    if (!target_id) continue;  // no target; skip round

    // Snapshot before mutation so we can rollback
    auto snap = cs.eval("(ast:snapshot)");

    cs.eval("(mutate:query-and-replace (query:defines-by-marker ...) ...)");

    // Check the mutation took effect
    auto new_value = cs.eval("(query:type ...)");
    if (!is_expected(new_value)) {
        cs.eval(std::format("(ast:restore {})", snap));
        continue;  // try again with different refine candidate
    }
}
```

The snapshot + restore bracket is the safety net for
"failed refine" cases. StableNodeRef is the safety net
for "successful refine, but the next round's query needs
to re-resolve the target" cases.

## Handling stale refs and retry logic

`is_valid(ref)` is the only staleness check. It is:

- **Cheap**: O(1), a single 16-bit compare (`node_gen_[id]
  == ref.generation_`).
- **Thread-safe**: the `node_gen_` column is a `uint16_t`
  (atomic on the supported platforms) and is bumped
  atomically on slot recycling + COW copy.
- **Total**: the ref is either valid (the slot still
  holds the original node) or invalid (the slot was
  recycled or the flat was copied).

When `is_valid` returns `false`, the agent has 3
recovery strategies:

1. **Re-resolve via query** (most common): re-run the
   discovery query (e.g. `(query:defines-by-marker "X")`)
   to find the new NodeId, then re-acquire the ref.
2. **Snapshot rollback** (when the mutation was
   part of a failed refine cycle): restore the snapshot
   + try a different refine candidate.
3. **Give up + alert** (when neither recovery is
   viable): the ref points to a node that no longer
   exists; the agent should report the failure rather
   than guess.

## Interaction with Workspace layering + COW

The Aura workspace is layered: `workspace_flat_` is the
current COW-copied instance, and the layering supports
rollback + multi-workspace experiments. When the flat
is copied:

1. The new flat's `node_gen_[id]` for any copied node
   is set to the **new** generation (`generation_` is
   bumped during the copy).
2. Any `StableNodeRef` whose `generation_` doesn't match
   the new flat's `node_gen_[id]` is invalid.
3. The `is_valid` check returns `false` for those refs.

This is the right behavior: after a workspace COW, the
old refs are stale and the agent should re-resolve.
There is no "transparently migrate the ref" — that
would be incorrect (the new flat may have moved the
node to a different slot, or the mutation may have
changed the node's contents).

**Practical rule**: if you're holding a `StableNodeRef`
into workspace A and you switch to workspace B (via
`set_workspace_flat` or a COW copy), the ref is invalid
against B. Re-resolve.

## Concurrency considerations with fibers

When a fiber yields inside an eval, the engine can
context-switch to another fiber that may mutate the
shared workspace. The reference validity check
(`is_valid`) protects against this:

- The fiber that holds a `StableNodeRef` should
  re-check `is_valid` after every yield.
- The yield point is the safe boundary for the
  validity check (the mutating fiber would have
  already completed its mutation before the yielding
  fiber resumes).
- The `generation_` counter is monotonic; a stale ref
  is always detected (no false-positive valid checks).

For a multi-fiber agent, the canonical pattern is
**per-fiber ref table**: each fiber holds its own set
of refs, and the ref validity is checked at every
yield point.

## Cross-mutate storage guidelines (Issue #393)

The following rules summarize when and how to use
`StableNodeRef` for **storage across mutating calls**.
The previous sections cover when to use refs in
general; this section is the storage-specific
playbook.

### Rule 1: prefer `-stable` query variants by default

If the query result will be stored, returned, or
passed across an eval boundary, use the `-stable`
variant. The raw `(query:children <id>)` etc. variants
should be reserved for one-shot inspection (read the
result and discard it within the same `cs.eval`
call). The cost difference is one extra integer
(the `gen`) — the win is automatic invalidation
detection.

### Rule 2: pair storage with `is_valid_subtree` for scoped workloads

For large workspaces where mutations target one
subtree at a time (EDA RTL/SV, multi-define libraries,
agent self-modification loops), pair `-stable` query
results with `FlatAST::is_valid_subtree(ref)` rather
than `is_valid(ref)`. The `-subtree` variant is the
#392 relaxed check: it accepts refs in subtrees that
were not scoped-bumped, even when other subtrees
were mutated. This is the over-invalidation fix that
matters for the >10k-node ASTs common in production.

Bench data (`tests/bench/stable_ref_bench.cpp`):
in a 100-define × 10-child workload with one
subtree-bump every 7 rounds over 1000 rounds, the
stable path does **143 re-queries** (only the bumped
subtree) vs the raw path's **1,000,000 re-queries**
(forced re-query every round for every node). The
stable path is **2x faster** wall-clock (40 µs vs
20 µs per round) and meets the #393 AC: stable
re-queries < 50% of raw, AND speedup ≥ 30%.

### Rule 3: explicit (id, gen) pair ↔ `StableNodeRef` round-trip

The C++ API for working with stable refs as raw
`(id, gen)` pairs (e.g. when persisting to a
checkpoint file, or when crossing an FFI boundary
that doesn't know about `StableNodeRef`):

```cpp
// Construct a StableNodeRef from a known (id, gen)
// pair. Captures the current FlatAST state for all
// other fields (mutation_id, wrap_epoch, etc.).
auto ref = flat.make_ref_from_gen(id, gen);

// Flat-style validity check for callers that have
// the (id, gen) pair inline (e.g. in a hot loop that
// reads thousands of pairs from a side-vector).
// Returns true iff the slot is in-bounds AND its
// stored generation matches AND the wrap_epoch
// matches (when explicit).
if (flat.is_valid_id_gen(id, gen)) { /* still valid */ }
```

The default `wrap_epoch_at_capture = 0` means "use
the current wrap_epoch" — a fresh capture always
passes. Pass an explicit non-zero value to check
against a captured wrap_epoch (e.g. when the (id,
gen) pair was deserialized from a checkpoint file
written under a different wrap epoch).

### Rule 4: scope the ref table to the mutation boundary

For long-running agent loops, maintain a **per-loop
ref table**: at the start of each loop iteration,
capture fresh stable refs for the targets of this
iteration's mutations. Discard them at the end.
Don't accumulate refs across loop iterations — the
re-capture cost is one `make_ref` call per target
(negligible), and the alternative (checking `is_valid`
on stale refs from prior iterations) is strictly
worse because it forces the agent to handle the
stale-ref case on every iteration.

### Rule 5: store `gen` alongside `id` in any side-vector

If you build a side-vector or index keyed by node
id (e.g. an EDA netlist table, a marker→node map,
a fiber-local query cache), **also store the `gen`**
in the same record. This lets you run `is_valid_id_gen`
on the pair without allocating a `StableNodeRef`
wrapper, which matters for hot paths that read
thousands of pairs per second. From Aura, use
`(query:ref-valid? (cons <id> (cons <gen> nil)))`;
from C++, use `flat.is_valid_id_gen(id, gen)`.

## Anti-patterns to avoid

- **Don't cache raw `NodeId` across a `mutate` call**.
  The slot may have been recycled; the cached id now
  points to a different node (or a free slot).
- **Don't use a `StableNodeRef` across workspaces**.
  Each workspace has its own `generation_` counter; a
  ref into one is meaningless against another.
- **Don't assume `is_valid` is the only check**. The
  ref is "valid" if the slot still holds the original
  node, but the node's contents may have changed
  through legitimate means (e.g. the mutation the
  ref's holder requested). Use `(query:type <id>)` to
  check the contents.
- **Don't skip the validity check for "obviously
  local" refs**. Even one eval call can recycle the
  slot if the eval path mutates (e.g.
  `eval-current` after a `mutate:*` chain).
- **Don't `static_cast` a `NodeId` to a `StableNodeRef`
  without going through `get_stable_node_ref`**. The
  ref needs the current `generation_`; a static cast
  would use a stale generation (0 in the common case)
  and falsely report "valid" against the current
  generation.
- **Don't store `StableNodeRef` across a process
  boundary** (save/load). The `generation_` is
  per-process; serializing it would alias with the
  new process's own generation counter.

## Code examples

### Example 1: simple ref + mutate + use

```cpp
// Setup: workspace has a (define x 0).
aura::compiler::CompilerService cs;
cs.set_code("(begin (define x 0))");
cs.eval("(eval-current)");

// Get a stable ref to the define's NodeId.
const auto x_id = cs.eval("(query:defines-by-marker \"x\")");
if (!x_id) { /* error: x not found */ }
auto x_ref = cs.get_stable_node_ref(x_id.value());

// Mutate x. The ref may now be invalid.
cs.eval("(mutate:query-and-replace (query:defines-by-marker \"x\") "
         "\"(define x 99)\" \"ref-test\")");

// Re-validate.
if (!cs.is_valid(x_ref)) {
    // Re-resolve via the same query.
    x_ref = cs.get_stable_node_ref(x_id.value());
    if (!cs.is_valid(x_ref)) { /* error: still stale */ }
}

// Now use the ref safely.
const auto x_value = cs.eval(std::format(
    "(query:type {})", x_ref.id));
// x_value is 99 (post-mutation).
```

### Example 2: ref + snapshot/rollback

```cpp
// Take a snapshot before the mutation.
const auto snap = cs.eval("(ast:snapshot)");

// Mutate.
cs.eval("(mutate:query-and-replace ...)");

// Verify the mutation was correct.
if (!is_expected(cs.eval("(query:type ...)"))) {
    // Rollback.
    cs.eval(std::format("(ast:restore {})", snap));
}
```

The ref + snapshot + rollback triple is the canonical
"safety bracket" for AI agent self-modification loops.

### Example 3: ref across a fiber yield

```cpp
// Fiber 1 holds the ref.
auto ref = cs.get_stable_node_ref(target_id);
co_yield;

// After yield, fiber 2 may have mutated.
if (!cs.is_valid(ref)) {
    // Re-resolve.
    auto new_id = cs.eval("(query:defines-by-marker ...)");
    ref = cs.get_stable_node_ref(new_id);
}
```

## See also

- `docs/design/ast-workspace-decision.md` — the
  COW / layering design (the foundation for
  `StableNodeRef`).
- `docs/incremental_dirty_propagation.md` — the
  dirty bitmask system that pairs with `StableNodeRef`
  for incremental re-analysis.
- `docs/sanitizers.md` — the sanitizer matrix that
  catches ref-related races (TSan + ASan).
- The test `tests/test_issue_329.cpp` — the
  StableNodeRef stress test (the canonical example
  of ref usage under load).
- `src/core/ast.ixx` — the `FlatAST` implementation
  (`is_valid`, `get_stable_node_ref`, `generation_`,
  `node_gen_`, free-list).
- `src/compiler/service.ixx` — the
  `CompilerService` API surface for `get_stable_node_ref`
  + `is_valid`.

## Issue #347 follow-ups (TODO)

1. **Add a `(query:ref-valid? ref-id)` Aura primitive**
   so the agent can check ref validity from Aura
   (today the check is C++-only).
2. **Add a `(compile:ref-stats)` Aura primitive**
   exposing the lifetime counters (creation, hits,
   invalidations, generation wraps). Similar to
   `(query:stable-ref-stats)` but in the compile:*
   namespace.
3. **Document the `(query:provenance-of ...)`
   pattern** — provenance (the reason a ref points
   to the node it does) is a separate concern from
   validity; the doc covers validity only.
4. **Add a per-workspace generation counter** so that
   COW-copied workspaces can re-validate refs
   independently. Today the global `generation_` is
   shared across the layering, which works but is
   suboptimal for the multi-workspace case.
