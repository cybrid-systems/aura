# Memory Model & Workspace Locking Protocol

> **Status (2026-06-12, Issue #157 Phase 4)**: 🟢 Formalized. The
> memory model was implicit across `typed_mutation.md §6`, `mutate_api.md §6`,
> `developer/evaluator.md §3`, and the `issue-157-jit-workspace-invariant.md`
> design note. This doc consolidates them into a single page.

**Audience**: anyone writing C++ code that reads or writes the workspace
FlatAST (mutate/query primitives, JIT runtime bridges, JIT inline IR,
fiber scheduler integration, GC safepoints). **AI Agent readers**:
you should not need to read this — Aura code interacts with the
workspace via the `mutate:*` / `query:*` / `ast:*` / `ws:*` EDSL
primitives, which already enforce the protocol internally. This doc
is for the maintainer who adds a new C++ primitive or JIT bridge.

---

## 0. Implementation Status (2026-06-12, Issue #157 Phase 4)

**重要**：本文档记录了 **正式实装** 的内存模型。Issue #107（workspace_mtx_ + defuse_version_）和 Issue #157（Phase 0/1/1b/1c/2/5）已全部完成；future work 见 §10 版本说明。

### C++ Core Layer (`src/compiler/evaluator.ixx` / `evaluator_impl.cpp` / `service.ixx`)

| 组件 | 实装 | 备注 |
|------|------|------|
| `workspace_mtx_` (`std::shared_mutex`) | ✓ (#107 part 1) | `evaluator.ixx:586`；shared/unique 协议 |
| `defuse_version_` (monotonic counter) | ✓ (#107 part 3) | `evaluator.ixx`；`defuse_version_++` 在每个 mutate primitive |
| Canonical mutate skeleton（6 步） | ✓ | `developer/evaluator.md §3.1`；所有 12 `mutate:*` 原语遵循 |
| `query:*` shared_lock 读路径 | ✓ | `typed_mutation.md §6.3` |
| `g_fiber_yield_mutation_boundary` 协议 | ✓ | `developer/evaluator.md §3.1` step 3 |
| `InvariantCheckMode` + post-mutation invariant check | ✓ (#147) | WarningsOnly default；可调 Disabled/Strict |
| JIT 锁 hooks 表 (`g_lock_hooks` + `aura_set_lock_hooks`) | ✓ (#157 Phase 1) | `aura_jit_runtime.cpp`；NO-OP 当无 CompilerService |
| L2 SHAPE_PAIR version-check fastpath + deopt | ✓ (#157 Phase 1b/1c) | `aura_pair_{car,cdr}_unchecked` 真无锁；bb_slow 路径带锁 |
| Runtime bridges lock wraps (closure / cell / hash + pair + prim_dispatcher) | ✓ (#157 Phase 1/2) | 19 bypass sites 全部 wrapped |
| OpHashRef inline IR scan 锁 brackets | ✓ (#157 Phase 2b) | `fn_lock_workspace_read` / `fn_unlock_workspace_read` |
| `jit:metrics` `--serve` 命令暴露遥测 | ✓ (#157 Phase 5) | `bypass_count` / `unchecked_fastpath_count` / `deopt_count` |
| `bypass_count` 回归检测器 | ✓ | 0 write sites；新增 bypass site 会 tick |
| `OpGuardShape` lazy-deopt frame 完整实装 | 🟡 (#61 Iter 2/3 部分) | JIT 处理 OpGuardShape 但 lowering 未发出；specialized_for 字段存在但永为 0；见 §10 |
| Service.ixx registry mutation (`ir_cache_` 失效) | ✓ (#115 closing 7150af3) | `AuraJIT::invalidate(name)` 在 redefine 路径调用 |
| Epoch/RCU lock-free read path | 🔴 (设计) | 长期（设计 doc Phase 5） |

### 文档状态

- ✅ Memory model 正式实装并文档化（本文档）
- 🟡 `OpGuardShape` 完整 deopt frame 待 §10 future work（独立 issue）
- 🔴 Epoch/RCU 待 §10 future work（设计阶段）

---

## 1. Core Invariant

> **No consumer ever observes a half-modified FlatAST, and no
> producer ever sees a stale read of workspace metadata.**

The workspace FlatAST (`Evaluator::workspace_flat_`) is the single
source of truth. The data structure can be mutated by:

- `mutate:*` primitives (C++)
- `--serve` protocol `typed-mutate` command (C++, dispatches into
  the same `mutate:*` machinery)
- AOT/JIT-compiled code that calls into runtime bridges (C++)

The data structure is read by:

- `query:*` primitives (C++)
- IR interpreter (`eval_flat`, C++)
- JIT-compiled functions (LLVM IR)
- AOT-compiled binaries (C++)

These producers and consumers can run on different threads / fibers
concurrently under `--serve-async` or `--serve` with multi-worker
schedulers. The invariant is enforced by two pieces of state on
`Evaluator`:

| State | Type | Purpose |
|-------|------|---------|
| `workspace_mtx_` | `std::shared_mutex` | exclusive ↔ shared mutual exclusion across the FlatAST |
| `defuse_version_` | `std::uint64_t` (atomic-ish under lock) | monotonic counter; bumped on every mutation |

Together, these two pieces of state are the **memory model**.

---

## 2. The Three Layers

The protocol is implemented at three layers; each layer's job is
to translate the workspace invariants into something the layer
above can use.

### Layer 0 — C++ Mutate Primitives (the "trusted" writers)

The 12 `mutate:*` primitives in `evaluator_impl.cpp` are the only
writers of the FlatAST in the safe path. They follow the canonical
mutate skeleton (see `developer/evaluator.md §3.1`):

1. Acquire `std::unique_lock(workspace_mtx_)`.
2. Validate args (cheap; no lock needed).
3. `defuse_version_++` — bump the version under the unique lock.
4. Yield at mutation boundary (`g_fiber_yield_mutation_boundary`) so
   other fibers get a chance to run.
5. Apply the mutation to `workspace_flat_`.
6. Mark dirty + record history + invalidate defuse.

The lock is held for the entire mutation. No other thread / fiber
can read or write the FlatAST during this window. The version bump
in step 3 is the signal that downstream consumers use to detect the
mutation.

### Layer 1 — C++ Read Paths (the "trusted" readers)

The `query:*` primitives + the IR interpreter's read paths acquire
`std::shared_lock(workspace_mtx_)`. Multiple readers can proceed
in parallel; writers (Layer 0) block until all readers release.

Read paths that don't need to coordinate with writers (e.g. pure
constant folding, intrinsic evaluations) may skip the lock.

### Layer 2 — JIT Runtime Bridges + Inline IR (the "untrusted" path)

JIT-compiled code is **not** trusted to follow the C++ convention
because:

- It is generated by LLVM IR emission, not C++ discipline.
- It runs in the same process and can call C++ runtime functions
  (`aura_pair_car`, `aura_alloc_closure`, `aura_hash_ref`, ...).
- It must not block the writer (which would deadlock if the
  writer is waiting on a fiber that holds the runtime lock).

So the JIT layer translates the C++ convention into something
mechanical and provably correct:

| Operation | Translation | Cost |
|-----------|-------------|------|
| Read of `g_pair_slots[id]->car` | `aura_lock_workspace_read()` + load + `aura_unlock_workspace_read()` | ~50ns |
| Read of `g_pair_slots[id]->car` (SHAPE_PAIR L2 specialization) | `aura_get_defuse_version()` + compare against captured version + branch to either unchecked fastpath (no lock) or slow path (with lock) | ~5ns on match / ~50ns on deopt |
| Push-back on `g_closure_func_ids` | `aura_lock_workspace_write()` + push_back + `aura_unlock_workspace_write()` | ~50ns |
| OpHashRef inline IR scan | `aura_lock_workspace_read()` before `fn_hash_get_flat_table` + `aura_unlock_workspace_read()` in `done_bb` | ~50ns |

The lock hooks table (`g_lock_hooks` in `aura_jit_runtime.cpp`) is
a function-pointer table set by `service.ixx` on `CompilerService`
init. When no `CompilerService` is registered (the
single-threaded default), the hooks are no-ops — the locks cost
zero. Under multi-fiber serve, the hooks point at the
`Evaluator`'s `lock_workspace_{read,write}` methods.

The version-check fastpath is a **speculation**: the JIT
captures `defuse_version_` at function entry, then at each L2
use site, compares the current version against the captured one.
On match, the unchecked fastpath is safe (no mutation happened
between entry and now). On mismatch, the slow path takes over
(with the lock). This is the same lazy-deopt pattern as the
`OpGuardShape` instruction in #61.

---

## 3. The Protocol, Formally

### 3.1 Mutate (writers)

```
unique_lock(workspace_mtx_);     // exclusive
defuse_version_++;               // signal to readers
yield_at_mutation_boundary();    // let other fibers run
apply_mutation(workspace_flat_); // write the FlatAST
record_history();                // append to mutation_log_
invalidate_defuse();             // mark dependent defs stale
unique_lock's destructor runs;   // release
```

**Required:** all six steps. **Forbidden:** skipping the version
bump, skipping the yield (fiber starvation), or releasing the lock
before step 5 (then re-acquiring — that allows readers between).

### 3.2 Query (readers)

```
shared_lock(workspace_mtx_);     // shared (parallel reads OK)
read(workspace_flat_);           // read the FlatAST
shared_lock's destructor runs;   // release
```

**Required:** step 1. **Forbidden:** writing to `workspace_flat_`
under the shared lock. Mutations must go through Layer 0.

### 3.3 JIT runtime bridges

For each runtime bridge that reads or writes global state shared
with the FlatAST (e.g. `g_pair_slots`, `g_closure_func_ids`,
`g_cell_heap`, `g_hash_tables`, `g_prim_dispatcher`,
`g_jit_fns`):

- **Read**: acquire `aura_lock_workspace_read()` at function entry,
  release at exit. Refactor to a single return path if the
  function has multiple returns.
- **Write**: acquire `aura_lock_workspace_write()` at function
  entry, release at exit. Same refactor for multi-return.
- **Speculative read** (L2 SHAPE_PAIR): the function has an
  `_unchecked` variant that skips bounds + lock. The JIT emits a
  version check + deopt branch at the use site; on match, the
  unchecked path is safe; on mismatch, the slow (locked) path is
  taken. Telemetry: `aura_unchecked_fastpath_count` /
  `aura_deopt_count`.

### 3.4 JIT inline IR

Inline IR (e.g. `OpHashRef`'s `aura_hash_get_flat_table` + GEP
scan) must bracket the entire operation with a single
`aura_lock_workspace_{read,write}()` /
`aura_unlock_workspace_{read,write}()` pair, with the unlock in
the function-exit block (`done_bb`, `bb_done`, etc.). Multi-block
inline IR must not have any other exit point that bypasses the
unlock.

### 3.5 Inline IR fastpath exception

The L2 SHAPE_PAIR fastpath (Phase 1b/1c) is the **only** inline
IR that runs without a lock. It does so by capturing
`defuse_version_` at function entry and comparing at each use
site. The comparison itself is racy, but the **result** is
sound: a false negative (deopt when version matches) just costs
the slow-path latency; a false positive (no deopt when version
mismatched) is impossible because `defuse_version_` is bumped
under the unique lock, and the version load in the JIT
acquires the relevant memory barrier (the same `shared_mutex`).

---

## 4. When You Don't Need a Lock

| Situation | Why |
|-----------|-----|
| Single-threaded default execution (no `CompilerService` registered) | The lock hooks are no-ops; no one else can race. |
| `ensure_defuse` from within an already-locked section | Itself takes the lock — drop the outer one first. |
| Pure constant folding / intrinsic evaluation | No `workspace_flat_` access; no coordination needed. |
| Internal `Evaluator` bookkeeping not exposed to mutations | E.g. `defuse_version_` reads that aren't used for deopt decisions. |

The rule of thumb: if your code reads `workspace_flat_` or any
global that a `mutate:*` primitive might write (`g_pair_slots`,
`g_closure_func_ids`, `g_cell_heap`, `g_hash_tables`,
`g_prim_dispatcher`, `g_jit_fns`), you need the lock or the
version-check fastpath.

---

## 5. Adding a New Runtime Bridge (Checklist)

1. **Identify the shared state** the bridge reads / writes. If
   it's only read by other runtime bridges (not by `mutate:*`
   primitives), the lock is not needed. E.g.
   `aura_alloc_string` writes to `g_string_pool`, which is not
   read by `mutate:*` — no lock needed.
2. **Read or write?** Choose the appropriate lock. Refactor
   multi-return functions to a single return path so the
   `aura_lock_workspace_*()` / `aura_unlock_workspace_*()` pair
   is balanced.
3. **Add the lock**. Place `aura_lock_workspace_{read,write}()`
   at the function entry (after the bypass-counter increment is
   removed — see step 4) and `aura_unlock_workspace_*()` at
   the function exit.
4. **Remove the bypass counter** (`g_workspace_mtx_bypass_count.fetch_add`).
   The counter is now 0-write (used only for `aura_bypass_count()`
   observability); leaving a fetch_add at a new site without a
   lock is a regression — the counter would tick and ops
   dashboards would alert.
5. **Add a test**. The existing
   `tests/test_concurrent.cpp` (5258/5258 cases) exercises the
   locked paths. Add a new test case if the new bridge has a
   non-trivial behavior (e.g. resize, arena allocation).
6. **Cross-link this doc** in the bridge's source comment.

---

## 6. Adding a New Mutate Primitive (Checklist)

1. **Follow the canonical mutate skeleton** in
   `developer/evaluator.md §3.1`. All six steps.
2. **Bump the version** (`defuse_version_++`) under the unique
   lock, before the yield. This is the signal that triggers
   version-check fastpath deopt in JIT-compiled code.
3. **Yield at the boundary**. The fiber scheduler integration
   is the difference between "a single fiber starving" and
   "fair scheduling under mutation load".
4. **Add an `InvariantCheckMode`-aware post-mutation check** (see
   `typed_mutation.md §6.1`). At least the read of the modified
   region should be re-validated.
5. **Cross-link this doc** in the primitive's source comment.

---

## 7. Adding a New Query Primitive

1. **Acquire `std::shared_lock(workspace_mtx_)`** at function
   entry, release at exit. Multiple readers in parallel; writers
   block.
2. **No need to bump the version** — readers don't signal.
3. **Yield at the boundary** is optional but recommended for
   long-running queries.

---

## 8. Observability

The runtime telemetry is exposed via the `--serve` protocol
command `jit:metrics` (Issue #157 Phase 5). Three counters:

- `bypass_count` — number of runtime bridge calls without a
  lock. Steady-state goal: **0**. A non-zero value means a
  new bypass site was added without the lock protocol
  (regression). `aura_bypass_count()` / `aura_counters_reset()`.
- `unchecked_fastpath_count` — number of L2 SHAPE_PAIR uses
  that took the unchecked (no-lock) fastpath. Should dominate
  in single-threaded execution.
- `deopt_count` — number of L2 SHAPE_PAIR uses that took the
  slow path (with lock) because the captured version didn't
  match the current one. Should stay at 0 in single-threaded
  execution. Non-zero under multi-fiber serve with concurrent
  mutate.

Sample with `{"cmd":"jit:metrics","reset":true}` to read +
reset in one call.

---

## 9. Why This Doc Exists

The memory model was implicit before this doc. Each layer had
its own conventions documented in different places
(`typed_mutation.md §6`, `mutate_api.md §6`,
`developer/evaluator.md §3`, the `#107` closing doc, the
`#157` design note). New contributors had to find all four to
understand the full picture. This doc:

- Consolidates the four sources into a single page.
- Formalizes the protocol in §3 (was informal in
  `developer/evaluator.md §3`).
- Provides per-layer checklists (§5/§6/§7) so the next
  contributor doesn't have to re-derive the protocol.
- Cross-references the existing docs rather than duplicating
  them.

## 10. References

| Doc | Covers |
|-----|--------|
| `typed_mutation.md §6.3` | Mutate primitive integration with `workspace_mtx_` |
| `mutate_api.md §6` | High-level concurrency + safety protocol |
| `developer/evaluator.md §3` | C++ canonical mutate skeleton |
| `docs/design/notes/issue-107-workspace-mutex.md` | Original `#107` design (workspace_mtx_ + version counter) |
| `docs/design/notes/issue-157-jit-workspace-invariant.md` | JIT-layer analysis (Phase 0/1/1b/1c/2/5 fix the races) |
| `docs/design/notes/issue-61-deopt-guards.md` | `OpGuardShape` / lazy-deopt design (entry-guard vs mid-function guard) |
| `docs/design/notes/issue-115-closing.md` | The stale `fn_ptr` fix that ties to Phase 4 |

## 11. Versioning

- **v1 (2026-06-12)** — Issue #157 Phase 4. Initial
  consolidation. Covers C++ mutate / query primitives + JIT
  runtime bridges + JIT inline IR (after Phase 0/1/1b/1c/2/5
  landed).
- **Future** — When `OpGuardShape`'s full deopt frame ships
  (#61 Iter 4), add a §3.6 covering the deopt-to-interpreter
  path.
- **Future** — When epoch/RCU ships (Phase 5 in the design
  doc), add a §3.7 covering the epoch-based read path.
