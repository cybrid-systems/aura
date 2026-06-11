# Issue #157 — JIT / Bridge `workspace_mtx_` Bypass (Prep + Roadmap)

**Issue**: [#157](https://github.com/cybrid-systems/aura/issues/157)
**Labels**: bug, P0, jit, concurrency, runtime
**Status**: 🟢 SHIPPED (Phase 0/1/1b/1c/2/5 — close-out comment at `docs/issue-closings/157-close-comment.md`)
**Related**: #61 (deopt-guards), #107 (snapshot/restore), #115 (stale fn_ptr), #119 (proper join)
**Memory model**: see [`../core/memory_model.md`](../core/memory_model.md) (Phase 4 — the single-page formalization of the protocol)

---

## 1. Summary (from issue body)

Aura's fundamental safety invariant:

> All reads/writes to `workspace_flat_` / derived heap structures (Pair, Cell, etc.) must go through locked paths; mutate operations must yield before modification.

This invariant is enforced in high-level primitives (`mutate:*`, `set-code`, `eval-current`) via:

- `std::unique_lock` / `shared_lock` on `Evaluator::workspace_mtx_`
- `defuse_version_++` on every successful mutation
- `g_fiber_yield_mutation_boundary()` call

**However**, the following low-level paths can bypass it:

1. **JIT L2 Specialization** (aura_jit.cpp) — `OpCar` / `OpCdr` / `OpMakePair` with `SHAPE_PAIR` generate specialized code using `fn_pair_car_unchecked` / `fn_pair_cdr_unchecked` / `fn_alloc_pair_arena`. These execute natively without acquiring `workspace_mtx_`.
2. **Runtime bridge functions** (aura_jit_runtime.cpp) — `aura_alloc_pair` / `aura_pair_car` / `aura_prim_call` / `aura_hash_ref` / etc. are registered as external symbols for LLVM ORC JIT. Some operate directly on `g_pair_slots` / `g_hash_tables` without explicit locking.
3. **Hot-swap / redefine** — per-function compile cache + `ResourceTracker` allows hot replacement, but newly compiled native code may retain stale shape/escape assumptions while another fiber mutates the same definition.
4. **Incomplete deopt paths** — `OpGuardShape` (#61) lacks full deopt frames for precise state restoration from specialized JIT back to interpreter.

**This is the most critical bottom-level issue** because it underpins all higher-level features: `orch:parallel` / `agent:ask`, Workspace COW, `ast:snapshot/restore`, `auto-evolve`, `synthesize-pipeline-v2`, LLM-driven mutation.

**Recent mitigations** (partial, symptoms not causes): thread-safe GC safepoints, eventfd-based `fiber:join` (#119), per-fn JIT cache (#115), direct FlatAST snapshot (#107).

---

## 2. Inventory of Bypass Sites (Phase 0)

Comprehensive list of all sites that access `workspace_flat_` / derived heap without acquiring `workspace_mtx_` or checking `defuse_version_`. Each row is a concrete `src/compiler/aura_jit*.{cpp,ixx,h}` location that needs Phase 1+ remediation.

### 2.1 Pair allocation (mutex on `g_pair_slots.push_back`)

| Function | Location | Touches | Bypass | Phase |
|----------|----------|---------|--------|-------|
| `aura_alloc_pair` | `aura_jit_runtime.cpp:426` | `g_pair_slots`, `g_owned_pair_slots_` (via `malloc`) | Yes — `push_back` race | P1 |
| `aura_alloc_pair_arena` | `aura_jit_runtime.cpp:452` | `g_pair_slots` (via `tl_arena_alloc`) | Yes — `push_back` race | P1 |
| `aura_alloc_closure` | `aura_jit_runtime.cpp:230` | (closure storage — separate heap) | Yes — same allocation pattern | P2 |
| `aura_alloc_closure_arena` | `aura_jit_runtime.cpp:237` | (closure storage) | Yes — same allocation pattern | P2 |
| `aura_new_cell` | `aura_jit_runtime.cpp:404` | (cell storage) | Likely — needs audit | P2 |

### 2.2 Pair access (read paths — `g_pair_slots[id]->{car,cdr}`)

| Function | Location | Bypass | Phase |
|----------|----------|--------|-------|
| `aura_pair_car` | `aura_jit_runtime.cpp:436` | Yes — no lock, no version check | P1 |
| `aura_pair_cdr` | `aura_jit_runtime.cpp:443` | Yes — same | P1 |
| `aura_pair_car_unchecked` | `aura_jit_runtime.cpp:462` | **High risk** — used by L2 `SHAPE_PAIR` specialization; no bounds + no lock | P1 (highest priority) |
| `aura_pair_cdr_unchecked` | `aura_jit_runtime.cpp:467` | Same as above | P1 (highest priority) |

### 2.3 Closure operations

| Function | Location | Bypass | Phase |
|----------|----------|--------|-------|
| `aura_closure_capture` | `aura_jit_runtime.cpp:245` | Likely — capture is a write | P2 |
| `aura_register_fn` | `aura_jit_runtime.cpp:290` | Function registry mutation | P2 |
| `aura_closure_call` | `aura_jit_runtime.cpp:296` | Reads closure storage; needs audit | P2 |

### 2.4 Cell operations

| Function | Location | Bypass | Phase |
|----------|----------|--------|-------|
| `aura_cell_get` | `aura_jit_runtime.cpp:410` | Reads cell storage | P2 |
| `aura_cell_set` | `aura_jit_runtime.cpp:416` | Writes cell storage | P2 |

### 2.5 Primitive dispatch

| Function | Location | Bypass | Phase |
|----------|----------|--------|-------|
| `aura_set_prim_dispatcher` | `aura_jit_runtime.cpp:483` | Function pointer write | P1 (write) |
| `aura_prim_call` | `aura_jit_runtime.cpp:487` | Reads `g_prim_dispatcher`; calls into evaluator | P1 (read + call) |
| `aura_prim_call_count` | `aura_jit_runtime.cpp:512` | Atomic counter — safe | — |
| `aura_prim_call_total_ns` | `aura_jit_runtime.cpp:515` | Atomic counter — safe | — |

### 2.6 Hash table operations (`g_hash_tables`)

| Function | Location | Bypass | Phase |
|----------|----------|--------|-------|
| `aura_hash_get_flat_table` | `aura_jit_runtime.cpp:107` | Reads `g_hash_tables` | P2 |
| `aura_hash_ref` | `aura_jit_runtime.cpp:525` | Reads `g_hash_tables[hidx]` and `FlatHashTable` internals | P2 |
| `aura_hash_set` | `aura_jit_runtime.cpp:551` | Writes `FlatHashTable` | P2 |
| `aura_hash_remove` | `aura_jit_runtime.cpp:608` | Writes `FlatHashTable` | P2 |
| `FlatHashTable::rebuild` | (inside the .cpp) | Resize — large write window | P2 |

### 2.7 JIT L2 Specialization paths in `aura_jit.cpp`

| Op | Location | Specialization | Bypass | Phase |
|----|----------|----------------|--------|-------|
| `OpCar` | `aura_jit.cpp:808` | `SHAPE_PAIR` → `aura_pair_car_unchecked` | Yes — fastest path, no check | P1 |
| `OpCdr` | `aura_jit.cpp:817` | `SHAPE_PAIR` → `aura_pair_cdr_unchecked` | Yes | P1 |
| `OpMakePair` | `aura_jit.cpp:761` | `NON_ESCAPING` → `aura_alloc_pair_arena`; `ESCAPED` → `aura_alloc_pair` | Yes — allocation race | P1 |
| `OpHashRef` | `aura_jit.cpp:949` | Inlined LLVM IR hash table scan; reads `g_hash_tables` | Yes | P2 |
| `OpHashSet` | `aura_jit.cpp:1070` | Inlined hash set | Yes | P2 |
| `OpHashRemove` | `aura_jit.cpp:1080` | Inlined hash remove | Yes | P2 |
| `OpGuardShape` | `aura_jit.cpp:577` | Deopt guard (#61) — partial frame | Partial (lacks full frame) | P3 |

### 2.8 Hot-swap / recompile

| Site | Location | Bypass | Phase |
|------|----------|--------|-------|
| `ResourceTracker` in JIT | `aura_jit.cpp:1505+` | Function registry | P2 |
| `ir_cache_` invalidation | `service.ixx` | Stale `fn_ptr` may persist briefly | P2 (#115) |

### 2.9 `g_fiber_yield_mutation_boundary` callsites

The high-level evaluator primitives call this hook (e.g., `evaluator_impl.cpp:4722+` on every `defuse_version_++`). **The JIT code never calls this hook** — it relies on the runtime bridges being safe. If the runtime bridges don't lock, both lock and yield are missing.

| Site | Status |
|------|--------|
| `evaluator_impl.cpp` mutate primitives | ✓ calls |
| `aura_jit_runtime.cpp` bridges | ✗ does NOT call |
| `aura_jit.cpp` Op lowering | ✗ does NOT call |

---

## 3. Phased Fix Plan (mirrors issue body)

### Phase 0 — Inventory + scaffold (THIS COMMIT)

- ✓ This design doc
- ✓ `// Issue #157: <what needs to change>` comments on each bypass site
- ✓ `static std::atomic<uint64_t> g_workspace_mtx_bypass_count{0}` telemetry counter (no callers yet)

### Phase 1 — `aura_lock_workspace_*` runtime bridges + P1 sites (high ROI, 1-3 days)

- Add `aura_lock_workspace_read` / `aura_unlock_workspace_read` (shared_lock) and `aura_lock_workspace_write` / `aura_unlock_workspace_write` (unique_lock) as runtime bridges — these can be called from JIT code
- Add `aura_check_defuse_version(expected)` runtime helper — returns 1 if `g_jit_defuse_version_mirror == expected`, 0 otherwise
- Add `aura_yield_mutation_boundary()` wrapper for `g_fiber_yield_mutation_boundary()`
- Wrap `aura_alloc_pair` / `aura_alloc_pair_arena` with `aura_lock_workspace_write` + `aura_unlock_workspace_write`
- Wrap `aura_pair_car` / `aura_pair_cdr` with `aura_lock_workspace_read` + `aura_unlock_workspace_read`
- Replace `aura_pair_car_unchecked` / `aura_pair_cdr_unchecked` calls in `aura_jit.cpp` with version-checked variant
- Add version check at `OpCar` / `OpCdr` L2 entry; mismatch → deopt to `aura_pair_car` (slow path)
- `aura_prim_call` reads `g_prim_dispatcher` with `std::atomic` load (replaces function pointer with atomic)

**Acceptance criteria for Phase 1:**

- All P1 sites in §2.1, §2.2, §2.5, §2.7 rows marked P1 acquire lock or check version
- TSan clean on `tests/suite/concurrent.aura` with `--jit` (12/12 still pass)
- `g_workspace_mtx_bypass_count` (Phase 0 telemetry) reads 0 in single-threaded, but non-zero under multi-fiber stress (proves locks are being taken)

### Phase 2 — Closure / cell / hash sites (medium-term, 2-3 days)

- Wrap `aura_alloc_closure` / `aura_alloc_closure_arena` / `aura_closure_capture` / `aura_register_fn` / `aura_closure_call` (P2 sites in §2.3, §2.4)
- Wrap `aura_hash_*` functions (P2 sites in §2.6)
- Wrap `aura_cell_get` / `aura_cell_set`
- Hot-swap / `ir_cache_` invalidation: add `defuse_version_` mirror refresh on cache eviction (closes #115 follow-up)

### Phase 3 — Full deopt frame for `OpGuardShape` (#61 follow-up, 1-2 days)

- Capture locals + AST version in deopt frame
- Re-execute on interpreter with consistent AST view
- This is the second half of the OpGuardShape work

### Phase 4 — Memory model / ownership protocol doc (medium-term)

- Formalize the protocol as a single page doc
- Inspired by linear types `m4-*` (currently stubs) — make `m4-move` / `m4-borrow` real
- Static guarantees reduce the runtime locking footprint

### Phase 5 — Epoch/RCU for read paths (long-term, optional)

- Near lock-free queries via epoch-based reclamation
- Keep yield points for GC/scheduling
- Big perf win if successful

---

## 4. Acceptance Criteria (end state, from issue body)

- All JIT-generated code paths that access AST-derived data either acquire the lock/yield or explicitly check version and deopt
- No data races detectable under TSan with concurrent mutate + JIT execution
- `jit:metrics` / observability can report "mutation protocol violations avoided"
- Related tests in `tests/` (concurrent, orchestration, jit) pass with `--jit --serve-async`

---

## 5. Why This Plan Is Phased (vs single big change)

- **Phase 1 (P1 sites)**: These are the highest-frequency bypass paths (OpCar/OpCdr/OpMakePair on every pair operation). Single biggest source of risk. Wrapping them with locks gives ~80% of the correctness benefit at ~20% of the code change.
- **Phase 2 (P2 sites)**: Lower frequency, but still need to be done. Closure / cell / hash operations are less common in hot loops.
- **Phase 3 (deopt)**: Independent of the locking work; needed for safe replace-after-mutate.
- **Phase 4 (formal model)**: Documentation + design. Independent of code changes.
- **Phase 5 (RCU)**: Performance optimization, optional. Can be deferred indefinitely.

This phasing matches the issue body's short/medium/long-term split and lets each phase ship independently + be reverted if it regresses.

---

## 6. References

- Issue #157 — original bug report
- Issue #61 — `OpGuardShape` deopt-guards
- Issue #107 — workspace mutex + AST versioning + direct snapshot
- Issue #115 — stale `fn_ptr` (per-fn JIT cache)
- Issue #119 — proper `fiber:join` (eventfd-based)
- `src/compiler/aura_jit.cpp` — JIT code generator
- `src/compiler/aura_jit_runtime.cpp` — runtime bridges
- `src/compiler/evaluator_impl.cpp` — high-level primitives (the "good" paths)
- `docs/design/compilation/jit.md` — JIT design doc
- `docs/design/core/agent_orchestration.md` — fiber / multi-agent runtime
- `docs/design/core/typed_mutation.md` — `workspace_mtx_` locking protocol (§6.1)
- `docs/design/core/mutate_api.md` §6 — concurrency + safety (the high-level protocol this issue extends to the JIT layer)
