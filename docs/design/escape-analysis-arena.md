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

### 3.2 Escape Analysis Pass

A dataflow analysis that runs after lowering, before JIT codegen.

**Lattice values for each slot:**

```
NON_ESCAPING     → Value never leaves its defining scope
ESCAPED          → Value may be returned, stored in hash, or captured
UNKNOWN          → Initial state (needs analysis)
```

**Transfer rules:**

```
Operation                      Input → Output
──────────────────────────────────────────────────────────
Return(result_slot)            result_slot → ESCAPED
Call(callee, arg_slot, ...)   arg_slot → ESCAPED (passed to unknown function)
Store(hash, key, val)         val → ESCAPED (stored in persistent hash)
Capture(env, val)             val → ESCAPED (captured in closure)
CellSet(cell, val)            val → ESCAPED (could outlive scope)
MakePair(a, b, result)        result.mark = max(a.mark, b.mark)
                              a, b stay unchanged
Arg(pos, result)              result.mark = ESCAPED (function argument may escape)

Branch/Jump                   No effect (values flow through PHI-like merge)
Local(a, b)                   b.mark = a.mark (copy propagation)

AllocPair(result)             result.mark = ESCAPED if returned/stored
                              else NON_ESCAPING
```

**Implementation: iterative dataflow on IRFunction's CFG.**

Pseudo-code:
```
for each block in function:
    for each instruction:
        if instruction creates a heap object (AllocPair/AllocVector/...):
            slot.mark = NON_ESCAPING (optimistic)
        else:
            apply transfer rules

Repeat until fixed point:
    for each block in reverse postorder:
        for each instruction:
            update output slot marks based on transfer rules
```

### 3.3 Output: EscapeInfo

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

## 9. Implementation Plan

### Phase 1: Unified Pair Storage (estimated: 2-3 days)

**Goal:** Replace `g_pair_cars/g_pair_cdrs` and `evaluator::pairs_` with a single `g_pair_slots` structure.

- [ ] Define `PairSlot` struct in `value.h` or `runtime.h`
- [ ] Create `g_pair_slots` global (small vector of pointers)
- [ ] Rewrite `alloc_pair` in `runtime.c` + `aura_jit_runtime.cpp` to use `g_pair_slots`
- [ ] Rewrite `pair_car/pair_cdr` to dereference through `g_pair_slots[id]->car`
- [ ] Update tree-walker evaluator to use same API
- [ ] **Verify:** 106/106 tests pass

### Phase 2: Per-Thread Arena (estimated: 2 days)

**Goal:** Replace global `bump_arena` with thread-local arena, add push/pop.

- [ ] Define `TLarena` struct with base, offset, capacity
- [ ] Create `__thread TLarena tl_arena` in `runtime.c`
- [ ] Move `aura_bump_init/reset/alloc` to per-thread
- [ ] Add `tl_arena_push/pop` (save/restore offset)
- [ ] Make non-escaping `alloc_pair` use `tl_arena_alloc`
- [ ] **Verify:** 106/106 tests pass

### Phase 3: IR Escape Analysis Pass (estimated: 3-4 days)

**Goal:** Dataflow analysis marking each allocation slot as ESCAPED/NON_ESCAPING.

- [ ] Define `EscapeInfo` struct
- [ ] Implement iterative dataflow in `src/compiler/escape_analysis.cpp`
- [ ] Hook into `pass_manager` (run after lowering, before JIT)
- [ ] **Verify:** Escape analysis correctly classifies simple cases
- [ ] **Verify:** No regression on 106 tests

### Phase 4: JIT Integration (estimated: 2 days)

**Goal:** JIT emits `tl_arena_alloc` for non-escaping, `gc_alloc` for escaping.

- [ ] Pass `EscapeInfo` to `aura_jit.cpp` compilation
- [ ] LLVMBuilder reads escape status for each `AllocPair`/`AllocVector`/`AllocString`
- [ ] Emit different allocation call for escaping vs non-escaping
- [ ] **Verify:** Benchmarks show arena allocations

### Phase 5: `with-arena` Primitive (estimated: 1-2 days)

**Goal:** Lisp-level arena control.

- [ ] Lower `(with-arena (size) body)` to IR push/pop
- [ ] Escape analysis rejects escaping results
- [ ] Add test cases
- [ ] **Verify:** `with-arena` correctly scopes arena allocations

### Phase 6: evo-kv Optimization (estimated: 1 day)

**Goal:** Measure ≤ 110B/key.

- [ ] Add memory usage tracking (arena offset delta per operation)
- [ ] Benchmark hash-intensive evo-kv workloads
- [ ] Profile and identify remaining allocations
- [ ] **Verify:** Memory ≤ 110B/key

### Regression Prevention

- All 106 existing tests must pass at each phase
- New test: `tests/test_escape.cpp` with 20+ escape analysis test cases
- New test: `tests/test_arena.cpp` with arena push/pop/alloc tests
- evo-kv suite: benchmark + memory measurement

---

## 10. Open Questions

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

## 11. Related Documents

- `docs/design/double-arena.md` — Existing double arena for AST/closure memory
- `docs/design/unify_cell_heap.md` — Unified cell heap (completed)
- `docs/design/speculative-jit.md` — Shape specialization (#53)
- `docs/design/value_encoding.md` — EvalValue encoding
- `docs/design/llvm_jit.md` — JIT architecture
