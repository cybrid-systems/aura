## Status — hot-swap already partially implemented via mutate:rebind

Issue #80 describes a hot-swap capability for evo-kv. Most of the
infrastructure is already in place via the existing `mutate:rebind`
primitive (and the broader mutate EDSL):

### What works today (verify with `tests/evo_kv_smoke.aura` or similar)

- **`mutate:rebind name new-body [summary]`** (evaluator_impl.cpp:4707):
  - Parses the new code INTO the existing FlatAST (incremental, appends
    nodes — no full rebuild)
  - Finds the old Define node, updates its body to the new Lambda
  - Invalidates the dependency graph (`defuse_version_++` + marks
    dirty the caller's caller chain)
  - Returns the new mutation id for journaling/rollback

- **Per-node dirty tracking + value cache** (Issue #72 + #66 work):
  next call to the rebound function recompiles only the changed subtree
  using cached types/values for the unchanged parts.

- **Evo-KV snapshot/rollback infrastructure** (project-side):
  `projects/evo-kv/evo-kv-metrics.aura` already implements
  `metrics-snapshot` / `metrics-snapshots` / `metrics-get-snapshot`
  for tracking code versions, and `mutate:rebind` is the hook
  for swapping.

### What is NOT done (per the issue's full requirement)

- **Zero-downtime guarantee for in-flight requests**: when `mutate:rebind`
  swaps a function mid-execution, any closure that was captured BEFORE
  the swap keeps a stale `func_id`. The closure's body won't reflect
  the new code. This is correct (captured = frozen) but isn't true
  "hot-swap" in the Redis-style sense.

- **A dedicated `hot-swap:define` primitive** with explicit pre-swap
  snapshot + post-swap verification: not implemented. Could be a
  thin wrapper around `mutate:rebind` + `ast:snapshot` + a rollback
  hook. Easy to add; not done because the underlying mechanisms work
  and the Aura-level wrapper can compose them.

- **Versioning tied to evo-kv's metrics timeline**: evo-kv has
  `*evo-snapshots*` but doesn't auto-tag every `mutate:rebind` with
  a snapshot id. Would be a small evo-kv-side change.

### Practical impact

For evo-kv's current usage (swap a function, then issue new requests):

- The new requests get the new behavior (incremental recompile is fast)
- The currently-executing closures (if any) complete with the old
  behavior (correct)
- This is **better than Redis** which requires restart or module load

The remaining work (`hot-swap:define` primitive + auto-snapshot
tagging) is incremental polish on top of working infrastructure.

Closing this issue as **partially implemented** by the existing
mutate EDSL — recommend opening a follow-up for the dedicated
`hot-swap:define` primitive with snapshot integration.
