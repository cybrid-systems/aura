# Universal Escape Analysis + Arena Allocation

## 1. Current State

### 1.1 Three Parallel Pair/Value Storage Systems

Aura currently has **three independent allocation paths** for heap objects:

| System | Location | Data Structure | Scope | Reset Mechanism |
|--------|----------|---------------|-------|-----------------|
| Tree-walker evaluator | `evaluator_impl.cpp` | `pairs_` (vector), `hash_heap_`, `string_heap_` | Per-`Evaluator` instance | Manual (not automatic) |
| JIT runtime | `aura_jit_runtime.cpp` | `g_pair_cars/g_pair_cdrs` (global vectors) | Process-global | Never shrinks |
| Native C runtime | `runtime.c` | Bump arena (global) + free list | Process-global | `aura_bump_reset()` resets offset |

Key issues:
- **No unified pair ID space**: A pair created by the tree-walker (ID=X) and a pair created by the JIT (ID=Y) have overlapping but incompatible IDs
- **JIT pairs never freed**: `g_pair_cars/g_pair_cdrs` grow monotonically
- **Double arena exists** (`double-arena.md`) but only covers AST/closure allocation, not EvalValue heap objects

### 1.2 EvalValue Encoding

```cpp
// value.ixx
using value_t = int64_t;
struct EvalValue { value_t val; };

// Tag encoding (low 2 bits):
//   00 = fixnum (n << 1)
//   01 = pair/vector ref: (id << 2) | 1
//   10 = closure/ref: (id << 2) | 2
//   11 = special (bool, void, char, primitive)
```

For pair/vector/closure values, the "ID" is an index into a parallel array (`g_pair_cars`, `g_closure_func_ids`, etc.). This is essentially a **semi-space** design where the index serves as both the identity and the storage location.

### 1.3 The ~200B/key Problem

An evo-kv hash entry creates:
```
1 hash entry struct (internal Aura hash implementation)
2+ pair cells (for hash bucket chains)
1 key string
1 value (fixnum or ref)
```

Each pair = 2x int64_t (car + cdr) + vector overhead. With `g_pair_cars/g_pair_cdrs` being separate vectors,
the pair itself is ~32-40B (two vectors of int64_t entries) plus hash table overhead.

The `runtime.c` bump arena already gives O(1) allocation, but objects allocated there are only freed
when the arena resets — at function call boundaries. Long-lived hash entries survive arena resets and leak.

### 1.4 Existing Bump Arena (runtime.c)

```c
static uint8_t* bump_arena;       // 64MB, doubles on overflow
static size_t bump_offset;        // bump pointer
// API:
void aura_bump_init(void);        // init or reset
void* aura_bump_alloc(size_t, size_t);  // aligned bump alloc
void aura_bump_reset(void);       // reset offset
```

This is already used by the native C runtime, but:
- Not used by the JIT runtime (`aura_jit_runtime.cpp` has its own pair vectors)
- Not integrated with escape analysis
- No scoped arena support (cannot push/pop a sub-arena)

---

## 2. Design Overview

```
┌──────────────────────────────────────────────────────────────┐
│                    Escape Analysis Pipeline                    │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  1. IR Pass: escape analysis on IRFunction                    │
│     - Tracks each `AllocPair` / `AllocVector` / `AllocString` │
│     - Marks as ESCAPED or NON_ESCAPING                       │
│     - Propagates through Return, Call, Capture, Store ops     │
│                                                              │
│  2. Arena Hierarchy                                          │
│     - TL (thread-local) arena: short-lived values            │
│     - Global heap: long-lived values (hash entries, cells)   │
│     - Scoped arena: (with-arena (size) body)                 │
│                                                              │
│  3. Unified Allocation Runtime                                │
│     - Single alloc_pair / alloc_vector / alloc_string API     │
│     - All backends (JIT, tree-walker, native) use same API   │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. IR-Level Escape Analysis

### 3.1 New IR Opcodes

No new opcodes. Existing allocation opcodes already exist:

```
Opcode:     Allocs heap object?   Arguments
──────────────────────────────────────────────
MakePair    Yes (pair)            car_slot, cdr_slot, result_slot
MakeVector  Yes (vector)          size_slot, fill_slot, result_slot  
MakeString  Yes (string)          (string data)
MakeClosure Yes (closure)         func_slot, env_size, ...
NewCell     Yes (cell)            init_slot, result_slot
```

### 3.2 Scope (Phase 2 only: Pair + Vector)

Phase 2 of the escape analysis pass only covers `MakePair` and `MakeVector`.
`MakeString`, `MakeClosure`, and `NewCell` are handled in later phases.
This covers the evo-kv hot path (hash bucket chains, list processing)
without the complexity of string internment or closure environment analysis.

### 3.3 Escape Analysis Pass: Backward (Reverse) Dataflow

A **reverse dataflow analysis** that runs after lowering, before JIT codegen.
Instead of propagating forward from allocation sites, it propagates **backward**
from known escape points (Return, Store, Capture). This is simpler because:

- Escape points are few and well-defined
- Default (unmarked) = NON_ESCAPING
- No need to track allocation edges forward

**Lattice values for each slot:**

```
ESCAPED          → Value may be returned, stored in hash, or captured
NON_ESCAPING     → Default. Value never leaves its defining scope.
```

(No UNKNOWN needed — conservative assumption is NON_ESCAPING.)

**Backward transfer rules:**

Rules propagate the ESCAPED mark **upward** through the IR (from result to operands).

```
Instruction                    Effect (if output is ESCAPED)
─────────────────────────────────────────────────────────────────
Return(result_slot)            result_slot → ESCAPED         ← ESCAPE POINT
Call(callee, arg_slot, ...)    arg_slot → ESCAPED            ← ESCAPE POINT
Store(hash, key, val)          val → ESCAPED                 ← ESCAPE POINT
Capture(env, val)              val → ESCAPED                 ← ESCAPE POINT
CellSet(cell, val)             val → ESCAPED                 ← ESCAPE POINT
Local(a, b)                    if b is ESCAPED → a is ESCAPED
MakePair(a, b, result)         if result is ESCAPED → a, b are ESCAPED
                               Also: result stays NON_ESCAPING if no escape point reachable
Arg(pos, result)               if result is ESCAPED → (nothing; args can't be un-escaped)
```

**Algorithm:**

```
1. Initialize empty worklist
2. For each ESCAPE POINT (Return, Call, Store, Capture, CellSet):
     Mark the operand slot as ESCAPED
     Add the defining instruction to worklist
3. While worklist is not empty:
     Pop instruction I
     If I defines a slot S and S is ESCAPED:
       Propagate ESCAPED to I's operands following backward rules
       Add defining instructions of newly-escaped operands to worklist
4. Remaining unmarked slots = NON_ESCAPING
```

**Key advantage over forward analysis:**
No need to iterate to a fixed point over PHI-like merges. Each slot is
touched at most once (when it first becomes ESCAPED).

### 3.4 Mutation Safety

`mutate:*` can rewrite control flow, which may invalidate escape analysis results.
A pair that was non-escaping before mutation might escape after (e.g., mutation
wraps a function body in a lambda that captures a local pair).

**Strategy: conservative reset** (same pattern as shape profiler `reset()` in #53)

```
after mutation → escape_analysis_cache_.clear()
```

On the next evaluation, escape analysis re-runs from scratch for the mutated
function. All previously cached EscapeInfo entries are invalidated.

This is simpler than incremental update and is correct by construction.
The cost is a one-time re-analysis per mutation, which is negligible compared
to the mutation loop's total latency.

### 3.5 with-arena Safety: Intraprocedural Only, Runtime Guard

Escape analysis in Phase 2-3 is **intraprocedural** — it looks at one function
at a time. Cross-function call sites are treated conservatively:

```lisp
(with-arena ()
  (let ((x (cons 1 2)))
    (set! *global* x)    ;; ← Call to set! is an escape point → x is ESCAPED
    ))                    ;;    Compiler allocates x on global heap, not arena
```

The `set!` call is visible in the current function's IR, so the backward
analysis marks `x` as ESCAPED. No interprocedural analysis needed.

**Runtime guard:** For Phase 2 TL Arena (before escape analysis is ready),
a runtime assertion catches arena-allocated values being stored to globals:

```c
void hash_set(Hash* h, int64_t key, int64_t val) {
    assert(!is_arena_ptr(val) && "storing arena-allocated value in hash");
    ...
}
```

This is a debugging aid, enabled only in debug builds. In release builds,
the compiler's escape analysis (Phase 3) makes the guarantee statically.

### 3.6 Output: EscapeInfo

```cpp
struct EscapeInfo {
    // Per-function: array of bool, indexed by slot
    // true = ESCAPED, false = NON_ESCAPING
    std::vector<bool> slot_escapes;

    // Non-escaping allocation sites
    // JIT can use bump arena instead of persistent heap
    std::vector<uint32_t> arena_alloc_sites;

    bool is_escaped(uint32_t slot) const;
    bool can_use_arena(uint32_t alloc_slot) const;
};
```

---

## 4. Arena Hierarchy

### 4.1 Architecture

```
┌───────────────────────────────────────┐
│          Thread-Local Arena            │
│  (64MB bump, reset per "major" scope)  │
├───────────────────────────────────────┤
│         ┌───────────────────┐          │
│         │ Scoped Arena #2   │ ← push/pop
│         │ (with-arena body) │          │
│         └───────────────────┘          │
│         ┌───────────────────┐          │
│         │ Scoped Arena #1   │          │
│         └───────────────────┘          │
├───────────────────────────────────────┤
│        Global RefCount Heap            │
│  (for objects that escape TL arena)    │
└───────────────────────────────────────┘
```

### 4.2 TL Arena (Thread-Local)

Replaces the existing global `bump_arena` in `runtime.c` with a per-thread instance.

```c
// Proposed API (replacing global bump_arena)
typedef struct {
    uint8_t* base;
    size_t offset;
    size_t capacity;
} TLarena;

extern __thread TLarena tl_arena;

void  tl_arena_init(void);        // called once per thread
void  tl_arena_reset(void);       // reset offset (O(1))
void* tl_arena_alloc(size_t, size_t);  // bump alloc
void  tl_arena_destroy(void);     // free base

// Stack of scoped sub-arenas
void  tl_arena_push(void);        // save offset
void  tl_arena_pop(void);         // restore to saved offset
```

### 4.3 Scoped Arena (`with-arena`)

```lisp
(with-arena (size 65536)
  ;; All pairs, vectors, strings allocated inside this block
  ;; go to a sub-arena. When block exits, sub-arena is freed.
  (let ((result (heavy-computation input)))
    result))  ;; result must NOT escape the arena
```

Implementation:

```cpp
// Lowering of (with-arena (size) body):
//
// entry:
//   tl_arena_push()              // save current offset
//   result = lower(body)
//   tl_arena_pop()               // restore offset → frees all body allocations
//   return result
```

**Constraint:** The return value of `with-arena` must be a value that doesn't reference
arena-allocated memory (fixnum, bool, or a value that was copied out before pop).
If body returns a pair allocated inside the arena, the pair is dangling after pop.

**Enforcement:** The escape analysis pass rejects programs where `with-arena`'s
result is an arena-allocated object that escapes the pop.

### 4.4 Global Heap (RefCount)

For objects that escape the arena:
- hash entries (persistent across calls)
- cells (`set!` targets)
- closures that outlive their creating scope
- strings/vectors stored in a persistent hash

These use the existing reference counting system in `runtime.c` (free list + drop).

---

## 5. Unified Allocation Runtime

### 5.1 Single Alloc API

Replace the three parallel systems with one:

```cpp
// Unified allocator (replaces g_pair_cars, evaluator::pairs_, runtime.c pairs)
int64_t alloc_pair(int64_t car, int64_t cdr);
int64_t alloc_vector(int64_t* elements, int64_t count);
int64_t alloc_string(const char* data, int64_t len);
int64_t alloc_closure(int64_t func_id, int64_t* env, int64_t env_size);
int64_t alloc_cell(int64_t init);

// Each checks EscapeInfo:
//   if NON_ESCAPING → allocate from TL arena
//   if ESCAPED      → allocate from global refcount heap
```

### 5.2 Pair Storage: Unified Arena vs Arena-Agnostic

**Option A: Arena-relative pointer**
```
Pair stored in arena: [car: int64][cdr: int64][next: ?]
Reference: byte offset from arena base
  → Pro: compact, cache-friendly, deref via base+offset
  → Con: need base pointer, invalid after arena reset
```

**Option B: Arena-agnostic ID**
```
Current approach: pair_id = global counter
Pair data: g_pair_data[pair_id] = {car, cdr}  (in arena if short-lived, global if long)
  → Pro: same encoding regardless of arena
  → Con: extra indirection, breaks arena locality
```

**Recommendation: Hybrid — use Option B for global heap, Option A for arena-only pairs.**

Actually, for simplicity and minimal encoding change:
- Keep the `(id << 2) | 1` encoding
- Non-escaping pairs: `g_pair_data[id]` stored in TL arena (not separate vectors)
- Escaped pairs: `g_pair_data[id]` stored in refcount heap
- The ID is still the index into `g_pair_data`, but the actual {car,cdr} bytes live with the arena/heap

This means: `g_pair_cars[id]` and `g_pair_cdrs[id]` are no longer separate vectors. Instead:

```cpp
// Unified pair storage
struct PairSlot {
    int64_t car;
    int64_t cdr;
};

// Pairs live in arena memory, but are indexed by ID
// For NON_ESCAPING: the PairSlot is on the TL arena
// For ESCAPED: the PairSlot is on the global heap

static TinyVector<PairSlot*, 1024> g_pair_slots;
// g_pair_slots[id] points to the PairSlot in arena or heap

int64_t alloc_pair(int64_t car, int64_t cdr, bool escaping) {
    int64_t id = g_pair_slots.size();
    PairSlot* slot;
    if (escaping) {
        slot = (PairSlot*)gc_alloc(sizeof(PairSlot));
    } else {
        slot = (PairSlot*)tl_arena_alloc(sizeof(PairSlot), alignof(PairSlot));
    }
    slot->car = car;
    slot->cdr = cdr;
    g_pair_slots.push_back(slot);
    return (id << 2) | 1;
}
```

This requires only one indirection for all pair access (down from two for JIT pairs that had separate car/cdr vectors).

### 5.3 Pair Access

```cpp
int64_t pair_car(int64_t pair_val) {
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    if (id < g_pair_slots.size())
        return g_pair_slots[id]->car;
    return 0;
}
```

---

## 6. JIT Integration

### 6.1 LLVM IR Generation

When compiling a function with EscapeInfo:

```llvm
;; Non-escaping pair allocation
define i64 @alloc_pair_nonescaping(i64 %car, i64 %cdr) {
    %id = atomicrmw add i64* @g_pair_count, i64 1  ;; get next ID
    %slot = call i8* @tl_arena_alloc(i64 16, i64 8)
    %car_ptr = getelementptr {i64, i64}, i8* %slot, i32 0, i32 0
    %cdr_ptr = getelementptr {i64, i64}, i8* %slot, i32 0, i32 1
    store i64 %car, i64* %car_ptr
    store i64 %cdr, i64* %cdr_ptr
    %id_shifted = shl i64 %id, 2
    %result = or i64 %id_shifted, 1
    ret i64 %result
}
```

The JIT emits direct calls to `tl_arena_alloc` for non-escaping pairs,
and `gc_alloc` for escaping pairs. The escape analysis result is baked
into the JIT-compiled function — no runtime ESCAPED check needed.

### 6.2 Shape Specialization Interaction

The SpecJIT (#53) already compiles specialized L1/L2 versions of functions.
Escape analysis runs on each specialized version independently.

For a function specialized for Int args:
- All pairs created from known-fixnum values → non-escaping → arena allocation
- This compounds: arena allocation is faster + shape specialization is faster

### 6.3 Hot Recompilation

When a function is hot-recompiled (profiler stabilizes after generic version runs):
- Escape analysis runs again with actual type information
- More allocations may be classified as non-escaping (because JIT can see concrete types)
- Recompiled function uses bump arena for newly discovered non-escaping sites

---

## 7. (with-arena) Primitive

### 7.1 Syntax

```lisp
(with-arena (size 65536)
  ;; body — all allocations use a sub-arena
  result)

;; size defaults to remaining TL arena capacity
(with-arena ()
  result)

;; nested:
(with-arena (size 4096)
  (with-arena (size 4096)
    (heavy-loop-2))
  (heavy-loop-1))
```

### 7.2 Semantics

```
Evaluation of (with-arena (size) body):
1. Push: save current TL arena offset
2. Allocate a sub-arena of `size` bytes (or use remaining TL arena if size=0)
3. Evaluate `body` within this sub-arena
4. Pop: restore TL arena offset → all body allocations are freed
5. Return result

Constraint: result must not reference arena memory
If result is an arena-allocated object → copy it to global heap first
```

### 7.3 Lowering

```cpp
// IR lowering for (with-arena ...)
case TagWithArena: {
    auto size = lower(children[0]);   // optional size arg
    auto body = lower(children[1]);

    auto saved_offset = emit(TLArenaPush);
    auto result = body;               // evaluate body
    emit(TLArenaPop, saved_offset);

    // If result is a pair, vector, or closure allocated in the sub-arena,
    // copy it to global heap before pop
    if (escape_info.is_arena_allocated(result.slot)) {
        result = emit(GlobalHeapCopy, result);
    }
    return result;
}
```

### 7.4 Use Cases

```lisp
;; evo-kv: batch GET — all intermediate results can use arena
(define (mget keys)
  (with-arena ()
    (map (lambda (k) (hash-get *evo-store* k)) keys)))

;; Temporary list processing
(define (score-top10)
  (with-arena ()
    (take (sort (hash->list *evo-store*)
                (lambda (a b) (> (cdr a) (cdr b))))
          10)))
```

---

## 8. evo-kv Memory Target: ≤ 110B/key

### 8.1 Current Breakdown (~200B/key)

| Component | Current | Notes |
|-----------|---------|-------|
| Hash entry | ~48B | Internal Aura hash table entry |
| Key string | ~40B | Short keys (8-16 chars) |
| Value (fixnum) | 0B | Inline in EvalValue |
| Pair chain | ~32B | Hash bucket collision chain |
| g_pair vector entry | ~32B | JIT pair car+cdr in separate vectors |
| Overhead/alignment | ~48B | Per-allocation metadata |
| **Total** | **~200B** | |

### 8.2 Target (~110B/key)

| Component | Target | How |
|-----------|--------|-----|
| Hash entry | ~32B | Direct pointer storage, no indirection |
| Key string | ~40B (same) | No change needed for small strings |
| Value | 0B (same) | |
| Pair chain | ~0B | Arena-allocated, 0 overhead (bump doesn't track individual frees) |
| g_pair vector entry | ~32B → ~8B | Unified PairSlot is one pointer (8B on 64-bit) instead of 2x int64 |
| Overhead/alignment | ~48B → ~32B | Bump arena has zero per-allocation metadata |
| **Total** | **~112B** | Close to target |

The main savings:
1. Arena allocation eliminates per-object tracking overhead (~16B)
2. Unified PairSlot eliminates double vector overhead (~24B)
3. Bump arena metadata is zero (no free list, no ref count for non-escaping)

---

## 9. `--no-arena` Runtime Flag

A command-line flag and environment variable to force all allocations to the
global heap, bypassing the TL arena entirely.

```bash
./aura --no-arena           # disable arena allocation
AURA_NO_ARENA=1 ./aura      # same via env var
./aura --ir --no-arena      # combine with --ir for debugging
```

**Purpose:**
- Debug arena-related bugs: if a bug reproduces with `--no-arena`, it's arena-related
- Regression testing: run test suite with and without arena to verify correctness
- Fallback if escape analysis misses an escape (wrong NON_ESCAPING classification)

**Implementation:**
```c
// runtime.h
extern bool g_use_arena;  // set from --no-arena flag

// alloc_pair: when g_use_arena is false, always use gc_alloc
int64_t alloc_pair(int64_t car, int64_t cdr, bool escaping) {
    bool use_arena = g_use_arena && !escaping;
    PairSlot* slot = use_arena
        ? (PairSlot*)tl_arena_alloc(sizeof(PairSlot), alignof(PairSlot))
        : (PairSlot*)gc_alloc(sizeof(PairSlot));
    ...
}
```

## 10. Implementation Plan (Revised from Review Feedback)

Review note: original plan had Unified Pair Storage as Phase 1.
Revised: TL Arena first (quick win), escape analysis second.
See reviewer comment: "Phase 1 只做 Thread-Local Arena（不做 escape analysis）".

### Phase 1: TL Arena + Unified Pair Storage (estimated: 3-4 days)

**Goal:** Single `g_pair_slots` global replacing three backends, plus
a thread-local bump arena. This phase requires **no escape analysis** —
all pairs go through the unified slot system. Immediate wins:
- JIT pairs no longer leak (arena resets per function call)
- Tree-walker and JIT share the same pair ID space
- Foundation for later escape-directed arena allocation

- [ ] Define `TLarena` struct with base, offset, capacity
- [ ] Create `__thread TLarena tl_arena` in `runtime.c`
- [ ] Move `aura_bump_init/reset/alloc` to per-thread
- [ ] Add `tl_arena_push/pop` (save/restore offset)
- [ ] Define `PairSlot` struct in `runtime.h` or `value.h`
- [ ] Create `g_pair_slots` global (small vector of pointers)
- [ ] Rewrite `alloc_pair` in `runtime.c` + `aura_jit_runtime.cpp` to use `g_pair_slots` + `tl_arena_alloc`
- [ ] Rewrite `pair_car/pair_cdr` to dereference through `g_pair_slots[id]->car`
- [ ] Update tree-walker evaluator to use same API
- [ ] Add `--no-arena` flag
- [ ] **Verify:** 106/106 tests pass with and without `--no-arena`

### Phase 2: IR Escape Analysis (estimated: 3-4 days)

**Goal:** Backward dataflow analysis marking Pair + Vector allocation
sites as ESCAPED or NON_ESCAPING. Phase 2 uses the escape info to
select TL arena vs global heap.

- [ ] Define `EscapeInfo` struct
- [ ] Implement backward dataflow in `src/compiler/escape_analysis.cpp`
  - Only MakePair + MakeVector initially
- [ ] Hook into `pass_manager` (run after lowering, before JIT codegen)
- [ ] IR interpreter reads escape info → selects allocator
- [ ] Add test file: `tests/test_escape.cpp` with 20+ cases
- [ ] **Verify:** Escape analysis correctly classifies fundamental patterns
  - `(cons 1 2)` in tail position → ESCAPED
  - `(cons 1 2)` as intermediate, never returned → NON_ESCAPING
  - `(set! global (cons 1 2))` → ESCAPED
  - `(cons 1 (cons 2 3))` → outer ESCAPED if returned, inner NON_ESCAPING
- [ ] **Verify:** No regression on 106 tests

### Phase 3: JIT + IRInterpreter Integration (estimated: 2 days)

**Goal:** Both backends read EscapeInfo and select allocator accordingly.

- [ ] Pass `EscapeInfo` to `aura_jit.cpp` compilation context
- [ ] LLVMBuilder reads escape status for `AllocPair`/`AllocVector`
- [ ] Emit `tl_arena_alloc` for NON_ESCAPING, `gc_alloc` for ESCAPED
- [ ] IR interpreter reads EscapeInfo and uses conditional alloc
- [ ] **Verify:** Benchmarks show arena allocation on hot paths

### Phase 4: `with-arena` Primitive (estimated: 1-2 days)

**Goal:** Lisp-level arena scoping.

- [ ] Lower `(with-arena (size) body)` to IR push/pop
- [ ] Escape analysis rejects escaping results from `with-arena` bodies
- [ ] Runtime guard in debug builds
- [ ] Add test cases
- [ ] **Verify:** `with-arena` correctly scopes arena allocations

### Phase 5: evo-kv Optimization + Extended Escape Coverage (estimated: 2 days)

**Goal:** Verify ≤ 110B/key, extend escape analysis to String + Closure.

- [ ] Add memory usage tracking (arena offset delta per operation)
- [ ] Benchmark hash-intensive evo-kv workloads
- [ ] Extend escape analysis to MakeString
- [ ] Extend escape analysis to MakeClosure (env allocation)
- [ ] Profile and identify remaining allocations
- [ ] **Verify:** Memory ≤ 110B/key
- [ ] **Verify:** No regression on 106 tests

### Regression Prevention

- All 106 existing tests must pass at each phase
- Phase 1: run full suite with and without `--no-arena`
- Phase 2: `tests/test_escape.cpp` with 20+ escape analysis test cases
- Phase 3: benchmark suite must not regress on non-arena benchmarks
- Phase 5: evo-kv suite: benchmark + memory measurement

---

## 11. Open Questions

1. **GC interaction**: When a non-escaping pair is stored in a global hash (escapes dynamically), how do we detect this and promote it to the global heap? 
   → *Possible answer:* Escape analysis is conservative — if the pair is passed to `hash-set!`, it's marked ESCAPED. No runtime promotion needed.

2. **Closure environments**: Non-escaping closures can allocate their env in the arena. But if the closure escapes (stored in a hash), the env must be in the global heap.
   → *Possible answer:* Same conservative analysis applies. If the closure escapes, its entire env escapes.

3. **`g_pair_slots` growth**: The `g_pair_slots` vector grows monotonically (IDs are never reused). Could this be a problem for long-running processes?
   → *Possible answer:* IDs are 64-bit, and even at 1B allocations/second, 64-bit wraps in 584 years. The pointer entries are 8B each, so 1M pairs = 8MB. Acceptable.

4. **Tree-walker + JIT pair ID collision**: If the tree-walker and JIT both use `g_pair_slots`, IDs are sequential from the same counter. This is fine — the encoding `(id << 2) | 1` works regardless of which backend created the pair.
   → *This is actually a win:* it unifies the pair ID space across all backends.

5. **`with-arena` result copy**: If the body returns a pair allocated in the sub-arena, we need to deep-copy it to the global heap before pop. When is deep-copying acceptable vs too expensive?
   → *Possible answer:* Only deep-copy the root value. If it's a pair, allocate a new `PairSlot` on the global heap and copy {car,cdr}. If car/cdr are also arena pointers, recursively copy (bounded by pair depth). For list-heavy workloads, this could be O(n). Users should design `with-arena` bodies to return fixnums or strings.

---

## 12. Related Documents

- `docs/design/double-arena.md` — Existing double arena for AST/closure memory
- `docs/design/unify_cell_heap.md` — Unified cell heap (completed)
- `docs/design/speculative-jit.md` — Shape specialization (#53)
- `docs/design/value_encoding.md` — EvalValue encoding
- `docs/design/llvm_jit.md` — JIT architecture
