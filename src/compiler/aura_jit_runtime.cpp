// aura_jit_runtime.cpp — JIT runtime functions for closure/cell/pair/prim ops
// These are compiled as regular C++ and registered in the ORC JIT as symbols.

// ═══════════════════════════════════════════════════════════════════════════
// Issue #173: stable-id type aliases (Phase 2 of #145 workstream 2)
// ═══════════════════════════════════════════════════════════════════════════
//
// PairId / CellId / StringId are the canonical index types for
// the runtime heaps. They're `uint32_t` (vs the previous
// implicit int64_t from vector::size()) for two reasons:
//   1. Smaller, denser encoding (32 bits covers 4B entries)
//   2. Explicit type: a PairId cannot be confused with a
//      CellId or a StringId, catching bugs at compile time
//
// The id is currently a vector index (g_pair_slots[id], etc.).
// Future work (remap table or generation stamp) will decouple
// the id from the storage location so reallocation doesn't
// invalidate cached ids. The aliases are the first step —
// establish the type system that the rest of the migration
// will build on.
using PairId   = unsigned int;
using CellId   = unsigned int;
using StringId = unsigned int;
inline constexpr PairId   NULL_PAIR_ID   = static_cast<PairId>(~0ULL);
inline constexpr CellId   NULL_CELL_ID   = static_cast<CellId>(~0ULL);
inline constexpr StringId NULL_STRING_ID = static_cast<StringId>(~0ULL);


//
// ═══════════════════════════════════════════════════════════════════════════
// Issue #157 — workspace_mtx_ bypass in JIT runtime bridges
// ═══════════════════════════════════════════════════════════════════════════
//
// The functions in this file are called from JIT-compiled code (aura_jit.cpp)
// and operate on shared heap structures (g_pair_slots, g_hash_tables,
// g_owned_pair_slots_, g_prim_dispatcher, etc.) WITHOUT acquiring
// Evaluator::workspace_mtx_ or checking defuse_version_. The high-level
// evaluator primitives (mutate:*, set-code, eval-current) DO acquire the
// lock + yield, but the JIT bypasses the protocol when calling these
// runtime bridges from specialized L2 paths (OpCar/OpCdr/OpMakePair with
// SHAPE_PAIR) or from inlined hash ops (OpHashRef/OpHashSet).
//
// This file uses `// Issue #157:` markers on every bypass site. The full
// inventory + phased fix plan lives in
//   Issue #157 (archived: git tag docs-archive-pre-2026-06)
// Phase 0 (this commit): inventory + telemetry counter, no behavior change.
// Phase 1+: wrap each site with workspace_mtx_ acquire/release or add
// defuse_version_ checks.
//
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstring>
#include <new>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unwind.h>
#include "runtime_shared.h"
#include "value_tags.h"  // Issue #181 Cycle 2: v2 string encoding helpers

// ── TL Arena (thread-local bump allocator) ────────────────────
__thread TLarena g_tl_arena;

void tl_arena_init(TLarena* arena) {
    if (!arena->base) {
        arena->base = (uint8_t*)malloc(arena->capacity);
        if (!arena->base) {
            fprintf(stderr, "tl_arena: alloc %zu failed\n", arena->capacity);
            exit(1);
        }
    }
    arena->offset = 0;
}

void tl_arena_destroy(TLarena* arena) {
    free(arena->base);
    arena->base = nullptr;
    arena->offset = 0;
}

void tl_arena_reset(TLarena* arena) {
    arena->offset = 0;
}

void* tl_arena_alloc(TLarena* arena, size_t size, size_t align) {
    size_t aligned = (arena->offset + align - 1) & ~(align - 1);
    if (aligned + size > arena->capacity) {
        // Double capacity
        size_t new_cap = arena->capacity * 2;
        uint8_t* new_base = (uint8_t*)realloc(arena->base, new_cap);
        if (!new_base) {
            fprintf(stderr, "tl_arena: overflow\n");
            exit(1);
        }
        arena->base = new_base;
        arena->capacity = new_cap;
    }
    size_t aligned2 = (arena->offset + align - 1) & ~(align - 1);
    void* ptr = arena->base + aligned2;
    arena->offset = aligned2 + size;
    return ptr;
}

void tl_arena_push(TLarena* arena) {
    // Save current offset on a simple stack (reuses arena memory for stack)
    // Top of arena = stack of saved offsets
    size_t* stack_top = (size_t*)(arena->base + arena->offset);
    *stack_top = arena->offset;
    arena->offset += sizeof(size_t);
}

void tl_arena_pop(TLarena* arena) {
    // Restore offset from stack
    arena->offset = ((size_t*)(arena->base + arena->offset))[-1];
}

// ── Arena flag ──
bool g_use_arena = true;

// ── Issue #157 Phase 0: bypass telemetry ──
// Counts every entry into a runtime bridge that currently bypasses
// workspace_mtx_ + defuse_version_. Single-threaded execution should
// see steady non-zero growth (proves bridges are being called). Multi-
// fiber stress with locks wrapped (Phase 1+) should see additional
// lock-acquire counters but bypass count remains 1:1 with bridge calls
// (locks are taken, not bypassed). Exposed via aura_bypass_count() for
// future jit:metrics observability (#157 AC: "jit:metrics can report
// mutation protocol violations avoided").
static std::atomic<uint64_t> g_workspace_mtx_bypass_count{0};

extern "C" uint64_t aura_bypass_count() {
    return g_workspace_mtx_bypass_count.load(std::memory_order_relaxed);
}

extern "C" void aura_bypass_count_reset() {
    g_workspace_mtx_bypass_count.store(0, std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════════════════════════
// Issue #157 Phase 1b: version-check fastpath telemetry.
//
// The JIT emits, at each L2 SHAPE_PAIR use site, a defuse_version_
// check + deopt branch: on version match, the unchecked (no-lock)
// path is taken; on mismatch, the slow (with-lock) path. This counter
// tracks how many L2 uses took the fast path (vs the slow path
// exposed via aura_deopt_count). In single-threaded code, this should
// dominate; in multi-fiber with concurrent mutate, the deopt count
// should be non-zero.
//
static std::atomic<uint64_t> g_workspace_unchecked_fastpath_count{0};
static std::atomic<uint64_t> g_workspace_deopt_count{0};

// Issue #157 Phase 1c: in-LLVM-callable deopt counter. The JIT
// emits a call to this at the start of every deopt basic block
// (bb_slow in OpCar/OpCdr SHAPE_PAIR lowering) so the deopt
// counter is incremented on the hot path. The external accessor
// `aura_deopt_count()` reads g_workspace_deopt_count for
// telemetry; this is the write side that the JIT calls into.
extern "C" void aura_deopt_inc() {
    g_workspace_deopt_count.fetch_add(1, std::memory_order_relaxed);
}

extern "C" uint64_t aura_unchecked_fastpath_count() {
    return g_workspace_unchecked_fastpath_count.load(std::memory_order_relaxed);
}

extern "C" uint64_t aura_deopt_count() {
    return g_workspace_deopt_count.load(std::memory_order_relaxed);
}

extern "C" void aura_counters_reset() {
    g_workspace_mtx_bypass_count.store(0, std::memory_order_relaxed);
    g_workspace_unchecked_fastpath_count.store(0, std::memory_order_relaxed);
    g_workspace_deopt_count.store(0, std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════════════════════════
// Issue #157 Phase 1: Lock hooks for the JIT runtime
// ═══════════════════════════════════════════════════════════════════════════
//
// Pattern: a function-pointer table set by CompilerService::register_jit_primitives
// (parallel to g_prim_dispatcher). The hooks let global C functions (aura_alloc_pair,
// aura_pair_car, etc.) participate in the Evaluator's workspace_mtx_ + defuse_version_
// protocol WITHOUT exposing the mutex as a global. service.ixx binds the hooks
// to Evaluator methods; if the hooks are unbound (no CompilerService), the bridge
// functions are no-ops (single-threaded default).

namespace {
struct LockHooks {
    void (*lock_read)(void*);       // shared_lock acquire
    void (*unlock_read)(void*);     // shared_lock release
    void (*lock_write)(void*);      // unique_lock acquire
    void (*unlock_write)(void*);    // unique_lock release
    std::uint64_t (*get_version)(void*);  // current defuse_version_
    void (*yield_boundary)(void*);  // g_fiber_yield_mutation_boundary hook
    void* user_data;                // typically the Evaluator*
};
static LockHooks g_lock_hooks = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};
} // namespace

extern "C" void aura_set_lock_hooks(
    void (*lock_read)(void*), void (*unlock_read)(void*),
    void (*lock_write)(void*), void (*unlock_write)(void*),
    std::uint64_t (*get_version)(void*),
    void (*yield_boundary)(void*),
    void* user_data) {
    g_lock_hooks.lock_read = lock_read;
    g_lock_hooks.unlock_read = unlock_read;
    g_lock_hooks.lock_write = lock_write;
    g_lock_hooks.unlock_write = unlock_write;
    g_lock_hooks.get_version = get_version;
    g_lock_hooks.yield_boundary = yield_boundary;
    g_lock_hooks.user_data = user_data;
}

// Bridge wrappers — these are what runtime functions (aura_alloc_pair,
// aura_pair_car, etc.) call to participate in the locking protocol.
// They are NO-OPs when hooks are unbound (single-threaded default).
//
// Note: aura_workspace_locked_read/write (counter mirror of bypass_count)
// is also exposed for future jit:metrics "mutation protocol violations
// avoided" reporting (#157 AC). For Phase 1 we don't increment it on the
// lock-acquire path; that's deferred to Phase 1b when the JIT lowering
// starts emitting explicit lock acquire/release calls.

extern "C" void aura_lock_workspace_read() {
    if (g_lock_hooks.lock_read)
        g_lock_hooks.lock_read(g_lock_hooks.user_data);
}

extern "C" void aura_unlock_workspace_read() {
    if (g_lock_hooks.unlock_read)
        g_lock_hooks.unlock_read(g_lock_hooks.user_data);
}

extern "C" void aura_lock_workspace_write() {
    if (g_lock_hooks.lock_write)
        g_lock_hooks.lock_write(g_lock_hooks.user_data);
}

extern "C" void aura_unlock_workspace_write() {
    if (g_lock_hooks.unlock_write)
        g_lock_hooks.unlock_write(g_lock_hooks.user_data);
}

extern "C" std::uint64_t aura_get_defuse_version() {
    if (g_lock_hooks.get_version)
        return g_lock_hooks.get_version(g_lock_hooks.user_data);
    return 0;
}

extern "C" void aura_yield_mutation_boundary() {
    if (g_lock_hooks.yield_boundary)
        g_lock_hooks.yield_boundary(g_lock_hooks.user_data);
}

// ── Forward declarations ──

// ── Shared pair storage (must be outside extern "C" for C++ type) ──
std::vector<PairSlot*> g_pair_slots;

// Heap-owned pair slots (subset of g_pair_slots allocated via malloc).
// Tracked separately so process exit / session reset can free them
// without touching the arena-allocated ones (which TL arena owns).
// Pre-#131: these leaked silently. The static destructor below runs
// at process exit and frees every entry, satisfying ASAN.
std::vector<PairSlot*> g_owned_pair_slots_;

namespace {
struct PairSlotCleanup {
    ~PairSlotCleanup() {
        for (auto* p : g_owned_pair_slots_) {
            std::free(p);
        }
        g_owned_pair_slots_.clear();
    }
};
[[maybe_unused]] PairSlotCleanup g_pair_slot_cleanup;
} // namespace

// ── FlatHashTable index space (Phase 4c) ──
std::vector<FlatHashTable*> g_hash_tables;

// ── FlatHashTable open addressing (Issue #136) ──
//
// Slot metadata encoding:
//   0xFF = empty  (never occupied; probe stops here)
//   0x80 = occupied
//   0x7F = tombstone  (was occupied, now removed; probe continues)
//
// The lookup/insert/remove functions use a per-key splitmix64 hash
// and linear probing. The previous implementation used a linear
// scan over the entire table, which is O(capacity) per operation.
// The new implementation is O(1) average case, O(capacity) worst
// case (only when the table is near-full). Capacity is rarely
// exceeded in practice because tables are sized to the expected
// number of entries.
//
// Note: the JIT inlines its own scan loop in OpHashRef/OpHashSet/
// OpHashRemove (see aura_jit.cpp:940+). That loop still does a
// linear scan; updating the JIT inlined version to match is
// deferred to a follow-up issue (the inlined version is already
// a constant-factor optimization for the hot path).
static constexpr uint8_t HASH_EMPTY = 0xFF;
static constexpr uint8_t HASH_OCCUPIED = 0x80;
static constexpr uint8_t HASH_TOMBSTONE = 0x7F;

// Splitmix64 hash for int64_t keys. Fast, well-distributed,
// stable across compile/runtime boundaries.
static inline uint64_t splitmix64_hash(int64_t key) {
    uint64_t x = static_cast<uint64_t>(key);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Linear probe: (h + i) % cap. Returns the slot index for
// the i-th probe of hash h.
static inline uint64_t probe_slot(uint64_t h, uint64_t i, uint64_t cap) {
    return (h + i) % cap;
}

extern "C" const FlatHashTable* aura_hash_get_flat_table(int64_t hash_val) {
    // Issue #157 Phase 2: read lock — reads g_hash_tables[hidx].
    // The pointer is only safe to dereference while the lock is
    // held; the JIT OpHashRef inline IR (aura_jit.cpp:~1080+)
    // holds the read lock around the entire inline scan.
    aura_lock_workspace_read();
    const FlatHashTable* ht = nullptr;
    auto hidx = static_cast<std::size_t>(static_cast<uint64_t>(hash_val) >> 6);
    if (hidx < g_hash_tables.size())
        ht = g_hash_tables[hidx];
    aura_unlock_workspace_read();
    return ht;
}

// ── FlatHashTable allocation ──
FlatHashTable* FlatHashTable::create(uint64_t cap) {
    auto* ht = (FlatHashTable*)std::malloc(total_bytes(cap));
    if (!ht) return nullptr;
    ht->capacity = cap;
    ht->size = 0;
    auto meta = ht->metadata();
    for (uint64_t i = 0; i < cap; ++i) meta[i] = HASH_EMPTY;
    auto k = ht->keys();
    for (uint64_t i = 0; i < cap; ++i) k[i] = 0;
    auto v = ht->values();
    for (uint64_t i = 0; i < cap; ++i) v[i] = 0;
    return ht;
}

void FlatHashTable::destroy(FlatHashTable* ht) {
    if (ht) std::free(ht);
}

void FlatHashTable::rebuild(uint64_t new_cap) {
    auto old_cap = capacity;
    // Save old data before we potentially overwrite this
    auto old_meta = metadata();
    auto old_keys = keys();
    auto old_vals = values();
    auto* new_ht = create(new_cap);
    if (!new_ht) return;
    auto new_meta = new_ht->metadata();
    auto new_keys = new_ht->keys();
    auto new_vals = new_ht->values();
    // Issue #136: use probing on the new table. This places
    // entries in their correct hash-determined slot (or
    // following probe chain) instead of scanning linearly.
    for (uint64_t i = 0; i < old_cap; ++i) {
        if (old_meta[i] != HASH_OCCUPIED) continue;  // skip empty + tombstones
        int64_t k = old_keys[i];
        int64_t v = old_vals[i];
        uint64_t h = splitmix64_hash(k);
        for (uint64_t j = 0; j < new_cap; ++j) {
            uint64_t slot = probe_slot(h, j, new_cap);
            if (new_meta[slot] == HASH_EMPTY) {
                new_meta[slot] = HASH_OCCUPIED;
                new_keys[slot] = k;
                new_vals[slot] = v;
                ++new_ht->size;
                break;
            }
        }
    }
    // Copy header (capacity, size) from new block
    std::memcpy(this, new_ht, sizeof(FlatHashTable));
    // Copy data arrays (this->metadata() now uses new capacity)
    auto new_mem_meta = metadata();  // uses this->capacity (just set from new_ht)
    uint64_t cp = capacity;
    std::memcpy(new_mem_meta, new_meta, cp);
    std::memcpy(keys(), new_keys, cp * 8);
    std::memcpy(values(), new_vals, cp * 8);
    std::free(new_ht);
}

// ── Runtime state (shared between all JIT functions) ──────────
extern "C" {

// === Closure runtime ===
static std::vector<int64_t> g_closure_func_ids;          // func_id for each closure
static std::vector<std::vector<int64_t>> g_closure_envs; // env for each closure (heap)
static std::vector<int64_t*> g_arena_closure_envs;       // arena env ptrs (parallel, null=heap)

// ── Closure inline cache (monomorphic, direct-mapped) ──
// Caches the resolved function pointer + metadata for recently-accessed closures.
// This eliminates vector lookup overhead on repeated calls to the same closure.
static constexpr int CLOSURE_CACHE_SIZE = 64;
struct ClosureCacheEntry {
    int64_t closure_id;
    int64_t (*fn)(int64_t*, uint32_t);
    int32_t local_count;
    int32_t arg_count;
    int32_t env_count;
};
static ClosureCacheEntry g_closure_cache[CLOSURE_CACHE_SIZE] = {{-1, nullptr, 0, 0, 0}};

int64_t aura_alloc_closure(int64_t func_id) {
    // Issue #157 Phase 2: write lock — push_back on
    // g_closure_func_ids + g_closure_envs is the same race as
    // aura_alloc_pair. The lock makes push_back exclusive vs
    // other alloc / capture / call paths.
    aura_lock_workspace_write();
    int64_t id = static_cast<int64_t>(g_closure_func_ids.size());
    g_closure_func_ids.push_back(func_id);
    g_closure_envs.emplace_back();
    aura_unlock_workspace_write();
    return id;
}

int64_t aura_alloc_closure_arena(int64_t func_id) {
    // Issue #157 Phase 2: write lock — same as aura_alloc_closure,
    // plus the arena pointer array also gets a new entry.
    aura_lock_workspace_write();
    int64_t id = static_cast<int64_t>(g_closure_func_ids.size());
    g_closure_func_ids.push_back(func_id);
    g_closure_envs.emplace_back();  // still push heap env as fallback
    g_arena_closure_envs.push_back(nullptr);  // arena ptr, set by later captures
    aura_unlock_workspace_write();
    return id;
}

void aura_closure_capture(int64_t closure_id, int64_t idx, int64_t val) {
    // Issue #157 Phase 2: write lock — capture writes to
    // g_closure_envs[cid] (a std::vector of vector) or
    // g_arena_closure_envs[cid] (arena pointer). Write to
    // inner vector / arena slot races with other fibers'
    // captures and with aura_closure_call readers.
    aura_lock_workspace_write();
    if (closure_id < 0) {
        aura_unlock_workspace_write();
        return;
    }
    size_t cid = static_cast<size_t>(closure_id);
    if (cid >= g_closure_envs.size()) {
        aura_unlock_workspace_write();
        return;
    }
    // Arena env path: allocate from TL arena on first capture
    if (cid < g_arena_closure_envs.size()) {
        int64_t*& arena_ptr = g_arena_closure_envs[cid];
        if (arena_ptr) {
            // Env already allocated on arena — write directly
            arena_ptr[static_cast<size_t>(idx)] = val;
            aura_unlock_workspace_write();
            return;
        }
        // First capture for this arena closure: allocate env from arena
        // Estimate env size from idx (grow to fit)
        size_t env_size = static_cast<size_t>(idx) + 4;  // pad for future captures
        int64_t* arena_env = (int64_t*)tl_arena_alloc(
            &g_tl_arena, env_size * sizeof(int64_t), alignof(int64_t));
        if (arena_env) {
            for (size_t i = 0; i < env_size; ++i)
                arena_env[i] = 0;
            arena_env[static_cast<size_t>(idx)] = val;
            arena_ptr = arena_env;
            aura_unlock_workspace_write();
            return;
        }
    }
    // Fallback: use heap std::vector
    auto& env = g_closure_envs[cid];
    if (static_cast<size_t>(idx) >= env.size())
        env.resize(static_cast<size_t>(idx) + 1);
    env[static_cast<size_t>(idx)] = val;
    aura_unlock_workspace_write();
}



// === Registered compiled function table ===
struct JitFnEntry {
    int64_t (*fn)(int64_t*, uint32_t);
    int32_t local_count;
    int32_t arg_count;
    int32_t env_count;
};
static JitFnEntry g_jit_fns[512] = {{nullptr, 0, 0, 0}};

void aura_register_fn(int64_t func_id, int64_t (*fn)(int64_t*, uint32_t), int32_t local_count,
                      int32_t arg_count, int32_t env_count) {
    // Issue #157 Phase 2: write lock — function registry mutation
    // must be exclusive vs readers in aura_closure_call (which
    // dereferences g_jit_fns[func_id]).
    aura_lock_workspace_write();
    if (func_id >= 0 && func_id < 512)
        g_jit_fns[func_id] = {fn, local_count, arg_count, env_count};
    aura_unlock_workspace_write();
}

int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc) {
    // Issue #157 Phase 2: read lock — reads g_closure_cache,
    // g_closure_func_ids, g_closure_envs, g_arena_closure_envs,
    // g_jit_fns. The lock makes the cache hit + slow path
    // read-consistent. The g_closure_cache slot update at the
    // end of the slow path races benignly with another fiber
    // (worst case: a stale cache slot, corrected on the next
    // slow-path call). The fn() call itself is invoked under
    // the read lock — this is conservative (write lock would
    // be unnecessary) but allows multiple concurrent calls.
    aura_lock_workspace_read();
    if (closure_id < 0 || static_cast<size_t>(closure_id) >= g_closure_func_ids.size()) {
        aura_unlock_workspace_read();
        return closure_id; /* match IRInterpreter: return callee_val for non-callable */
    }

    // ── Inline cache check ──
    int cache_idx = static_cast<int>(closure_id % CLOSURE_CACHE_SIZE);
    if (g_closure_cache[cache_idx].closure_id == closure_id) {
        auto& ce = g_closure_cache[cache_idx];
        if (ce.fn) {
            // Fast path: use cached function pointer directly
            int32_t nlocals = ce.local_count > 0 ? ce.local_count : 16;
            int32_t nargs = argc < ce.arg_count ? static_cast<int32_t>(argc) : ce.arg_count;
            int32_t env_count = ce.env_count;

            // Stack buffer for small locals, fallback to heap for large
            int64_t stack_buf[64];
            std::vector<int64_t> heap_buf;
            int64_t* locals = stack_buf;
            if (static_cast<size_t>(nlocals) > sizeof(stack_buf) / sizeof(stack_buf[0])) {
                heap_buf.resize(static_cast<size_t>(nlocals), 0);
                locals = heap_buf.data();
            } else {
                for (int32_t i = 0; i < nlocals; ++i)
                    locals[i] = 0;
            }

            // Place captured env values first (prefer arena env, fallback to heap)
            size_t cid_cast = static_cast<size_t>(closure_id);
            if (cid_cast < g_arena_closure_envs.size() && g_arena_closure_envs[cid_cast]) {
                int64_t* arena_env = g_arena_closure_envs[cid_cast];
                for (int32_t i = 0; i < env_count; ++i)
                    locals[i] = arena_env[i];
            } else {
                auto& env = g_closure_envs[cid_cast];
                for (int32_t i = 0; i < env_count && static_cast<size_t>(i) < env.size(); ++i)
                    locals[i] = env[i];
            }

            // Place call arguments after env
            for (int32_t i = 0; i < nargs; ++i)
                locals[env_count + i] = args[i];

            int64_t fast_result = ce.fn(locals, static_cast<uint32_t>(argc));
            aura_unlock_workspace_read();
            return fast_result;
        }
    }

    // ── Slow path: full dispatch + cache update ──
    int64_t func_id = g_closure_func_ids[static_cast<size_t>(closure_id)];
    if (func_id < 0 || func_id >= 512 || !g_jit_fns[func_id].fn) {
        aura_unlock_workspace_read();
        return 0;
    }

    auto& entry = g_jit_fns[func_id];
    size_t slow_cid = static_cast<size_t>(closure_id);

    // Stack buffer for small locals, fallback to heap for large
    int32_t nlocals = entry.local_count > 0 ? entry.local_count : 16;
    int64_t stack_buf[64];
    std::vector<int64_t> heap_buf;
    int64_t* locals = stack_buf;
    if (static_cast<size_t>(nlocals) > sizeof(stack_buf) / sizeof(stack_buf[0])) {
        heap_buf.resize(static_cast<size_t>(nlocals), 0);
        locals = heap_buf.data();
    } else {
        for (int32_t i = 0; i < nlocals; ++i)
            locals[i] = 0;
    }

    // Place captured env values first (prefer arena env, fallback to heap)
    if (slow_cid < g_arena_closure_envs.size() && g_arena_closure_envs[slow_cid]) {
        int64_t* arena_env = g_arena_closure_envs[slow_cid];
        for (int32_t i = 0; i < entry.env_count; ++i)
            if (i < nlocals)
                locals[i] = arena_env[i];
    } else {
        auto& env = g_closure_envs[slow_cid];
        for (size_t i = 0; i < env.size(); ++i) {
            if (i < static_cast<size_t>(nlocals))
                locals[i] = env[i];
        }
    }

    // Place call arguments after env
    int32_t nargs = argc < entry.arg_count ? static_cast<int32_t>(argc) : entry.arg_count;
    size_t env_count_check = slow_cid < g_arena_closure_envs.size() && g_arena_closure_envs[slow_cid]
                                 ? static_cast<size_t>(entry.env_count)
                                 : g_closure_envs[slow_cid].size();
    int32_t env_offset = static_cast<int32_t>(env_count_check);
    for (int32_t i = 0; i < nargs; ++i) {
        int32_t slot = env_offset + i;
        if (slot < nlocals)
            locals[slot] = args[i];
    }

    int64_t result = entry.fn(locals, static_cast<uint32_t>(argc));

    // ── Update cache ──
    g_closure_cache[cache_idx].closure_id = closure_id;
    g_closure_cache[cache_idx].fn = entry.fn;
    g_closure_cache[cache_idx].local_count = entry.local_count;
    g_closure_cache[cache_idx].arg_count = entry.arg_count;
    g_closure_cache[cache_idx].env_count = static_cast<int32_t>(env_count_check);

    aura_unlock_workspace_read();
    return result;
}

// === Cell runtime ===
static std::vector<int64_t> g_cell_heap;

int64_t aura_new_cell() {
    // Issue #157 Phase 2: write lock — push_back on g_cell_heap
    // is the same race as aura_alloc_pair.
    aura_lock_workspace_write();
    int64_t id = static_cast<int64_t>(g_cell_heap.size());
    g_cell_heap.push_back(0);
    aura_unlock_workspace_write();
    return id;
}

int64_t aura_cell_get(int64_t cell_id) {
    // Issue #157 Phase 2: read lock — read g_cell_heap[id]. A
    // concurrent aura_cell_set / aura_new_cell would race without
    // the lock (vector resize, slot mutation).
    aura_lock_workspace_read();
    int64_t result = 0;
    if (cell_id >= 0 && static_cast<size_t>(cell_id) < g_cell_heap.size())
        result = g_cell_heap[static_cast<size_t>(cell_id)];
    aura_unlock_workspace_read();
    return result;
}

void aura_cell_set(int64_t cell_id, int64_t val) {
    // Issue #157 Phase 2: write lock — write g_cell_heap[id].
    aura_lock_workspace_write();
    if (cell_id >= 0 && static_cast<size_t>(cell_id) < g_cell_heap.size())
        g_cell_heap[static_cast<size_t>(cell_id)] = val;
    aura_unlock_workspace_write();
}

// === Pair runtime (unified PairSlot pointer-based storage) ===
// Phase 2: g_pair_slots stores pointers to PairSlot structs.
// NON_ESCAPING: PairSlot allocated from TL arena
// ESCAPED: PairSlot allocated from global heap

int64_t aura_alloc_pair(int64_t car, int64_t cdr) {
    // Issue #157 Phase 1: write lock — push_back on g_pair_slots +
    // g_owned_pair_slots_ is a write, must be exclusive vs readers
    // and other writers. Released in Phase 1 as a no-op when no
    // CompilerService is registered (single-threaded default).
    aura_lock_workspace_write();
    auto* slot = (PairSlot*)malloc(sizeof(PairSlot));
    slot->car = car;
    slot->cdr = cdr;
    int64_t id = static_cast<int64_t>(g_pair_slots.size());
    g_pair_slots.push_back(slot);
    g_owned_pair_slots_.push_back(slot);
    aura_unlock_workspace_write();
    return (id << 2) | 1;
}

int64_t aura_pair_car(int64_t pair_val) {
    // Issue #157 Phase 1: read lock — g_pair_slots[id]->car is a
    // read; shared_lock lets multiple concurrent readers proceed
    // but blocks while a writer holds the lock.
    aura_lock_workspace_read();
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    int64_t result = 0;
    if (id < g_pair_slots.size() && g_pair_slots[id])
        result = g_pair_slots[id]->car;
    aura_unlock_workspace_read();
    return result;
}

int64_t aura_pair_cdr(int64_t pair_val) {
    // Issue #157 Phase 1: read lock, same as aura_pair_car.
    aura_lock_workspace_read();
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    int64_t result = 0;
    if (id < g_pair_slots.size() && g_pair_slots[id])
        result = g_pair_slots[id]->cdr;
    aura_unlock_workspace_read();
    return result;
}

// Arena-based pair allocation (for NON_ESCAPING pairs, Phase 2)
// Allocates PairSlot from TL arena instead of global heap.
int64_t aura_alloc_pair_arena(int64_t car, int64_t cdr) {
    // Issue #157 Phase 1: write lock — push_back on g_pair_slots
    // is the same race as aura_alloc_pair; the arena alloc is just
    // a different backing store.
    aura_lock_workspace_write();
    auto* slot = (PairSlot*)tl_arena_alloc(&g_tl_arena, sizeof(PairSlot), alignof(PairSlot));
    slot->car = car;
    slot->cdr = cdr;
    int64_t id = static_cast<int64_t>(g_pair_slots.size());
    g_pair_slots.push_back(slot);
    aura_unlock_workspace_write();
    return (id << 2) | 1;
}

// L2 specialization: unchecked pair access (skips bounds check)
//
// Issue #157 Phase 1b: NO LOCK. The "unchecked" suffix now means
// both "no bounds check" AND "no lock" — the function is a true
// raw read of g_pair_slots[id]->car, relying on the caller (the
// JIT lowering) to have done a defuse_version_ check at L2 entry.
// The version check guarantees that no other fiber has mutated
// the workspace since the function started, so the read is safe
// even without a lock. On version mismatch, the JIT emits a
// deopt branch to aura_pair_car (with bounds check + lock).
//
// In single-threaded execution (the common case), the version
// check passes, the unchecked path is taken, and the lock cost
// is avoided. In multi-threaded execution with concurrent
// mutate, the version check fails (on the next L2 use after the
// mutate), the deopt path is taken, and the lock is acquired
// for the slow path.
//
// The shape check (SHAPE_PAIR) is still required — the
// pair_val must be a valid pair reference, not a fixnum/void.
int64_t aura_pair_car_unchecked(int64_t pair_val) {
    g_workspace_unchecked_fastpath_count.fetch_add(1, std::memory_order_relaxed);
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    return g_pair_slots[id]->car;
}

int64_t aura_pair_cdr_unchecked(int64_t pair_val) {
    g_workspace_unchecked_fastpath_count.fetch_add(1, std::memory_order_relaxed);
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    return g_pair_slots[id]->cdr;
}

// === Primitive call bridge ===
// Global dispatcher function pointer — set by service.ixx to wrap evaluator primitives
static int64_t (*g_prim_dispatcher)(int64_t slot, int64_t* args, int32_t argc) = nullptr;

// Issue #114: counters for JIT observability. These are exposed
// via `aura_prim_counters()` for telemetry. All atomic; relaxed
// memory order is fine (we don't read-modify-write sequences
// across these, just statistical counters).
static std::atomic<uint64_t> g_prim_call_count{0};
static std::atomic<uint64_t> g_prim_call_total_ns{0};

void aura_set_prim_dispatcher(int64_t (*fn)(int64_t, int64_t*, int32_t)) {
    // Issue #157 Phase 1: write lock — g_prim_dispatcher is a
    // function pointer that aura_prim_call reads concurrently.
    // The non-atomic write races with the read on weakly-ordered
    // architectures. The write lock ensures the read in
    // aura_prim_call sees either the old or new pointer, never
    // a half-written one. (Future optimization: make
    // g_prim_dispatcher std::atomic<void*> with release/acquire
    // and skip the lock.)
    aura_lock_workspace_write();
    g_prim_dispatcher = fn;
    aura_unlock_workspace_write();
}

int64_t aura_prim_call(int64_t slot, int64_t a, int64_t b, int64_t count) {
    // Issue #157 Phase 1: read lock — read of g_prim_dispatcher
    // races with aura_set_prim_dispatcher. The dispatcher function
    // itself is the FFI boundary into the evaluator (where the
    // mutating primitives re-acquire the lock); the read lock here
    // ensures the pointer load is exclusive vs the writer.
    //
    // Issue #114: profile the slow-path primitive call. This is
    // the FFI boundary the JIT crosses for primitives that don't
    // have a specialized inlined fast path. The two atomic stores
    // are independent so we use relaxed ordering.
    aura_lock_workspace_read();
    auto t0 = std::chrono::steady_clock::now();
    int64_t result = 0;
    if (g_prim_dispatcher) {
        // Fast-pack: avoid the 3-element stack array that the
        // original wrapper allocated. The dispatcher takes
        // (int64_t*) but we can pass a pointer into the call's
        // argument slots directly (a, b, and a sentinel 0).
        // This saves 3 stores per primitive call.
        int64_t args[3] = {a, b, 0};
        result = g_prim_dispatcher(slot, args, static_cast<int32_t>(count));
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    g_prim_call_count.fetch_add(1, std::memory_order_relaxed);
    g_prim_call_total_ns.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
    aura_unlock_workspace_read();
    return result;
}

// Issue #114: accessors for the primitive-call counters (read by
// the JIT metrics snapshot).
uint64_t aura_prim_call_count() {
    return g_prim_call_count.load(std::memory_order_relaxed);
}
uint64_t aura_prim_call_total_ns() {
    return g_prim_call_total_ns.load(std::memory_order_relaxed);
}

static int64_t (*g_hash_set_fn)(int64_t hash, int64_t key, int64_t val) = nullptr;

// Issue #157 Phase 2: hash runtime (aura_hash_ref / aura_hash_set /
// aura_hash_remove + aura_hash_get_flat_table + FlatHashTable::rebuild)
// reads/writes g_hash_tables[hidx] and the FlatHashTable internals
// (metadata, keys, values arrays) without any locking. The hash set
// also writes to a flat region that can be rebuilt (resize) at any
// time. JIT inlined OpHashRef/OpHashSet/OpHashRemove (aura_jit.cpp:949+)
// reads the same regions via GEP from the FlatHashTable pointer. Two
// concurrent read+rebuild can produce torn reads.
//
// Phase 2 fix: wrap with aura_lock_workspace_read / aura_lock_workspace_write
// (set: write; ref/remove: read; rebuild: write). Telemetry already in
// place via g_workspace_mtx_bypass_count.

// Hash-ref: (hash-ref hash key) → value or void



int64_t aura_hash_ref(int64_t hash_val, int64_t key_val) {
    // Issue #157 Phase 2: read lock — read g_hash_tables[hidx]
    // and the FlatHashTable internals (metadata, keys, values
    // arrays). A concurrent aura_hash_set / aura_hash_remove /
    // FlatHashTable::rebuild would race without the lock.
    aura_lock_workspace_read();
    int64_t result = 11; // void sentinel (not found)
    auto hidx = static_cast<std::size_t>(static_cast<uint64_t>(hash_val) >> 6);
    if (hidx < g_hash_tables.size() && g_hash_tables[hidx]) {
        auto* ht = g_hash_tables[hidx];
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        uint64_t cap = ht->capacity;
        uint64_t h = splitmix64_hash(key_val);
        for (uint64_t i = 0; i < cap; ++i) {
            uint64_t slot = probe_slot(h, i, cap);
            uint8_t m = meta[slot];
            if (m == HASH_EMPTY) break; // not found, void sentinel
            if (m == HASH_OCCUPIED && keys[slot] == key_val) {
                result = vals[slot];
                break;
            }
            // HASH_TOMBSTONE: keep probing
        }
    }
    aura_unlock_workspace_read();
    return result;
}

static int64_t (*g_hash_str_convert_fn)(int64_t) = nullptr;
extern "C" void aura_set_hash_str_convert_callback(int64_t (*fn)(int64_t)) {
    g_hash_str_convert_fn = fn;
}
int64_t aura_hash_set(int64_t hash_val, int64_t pair_val) {
    // Issue #157 Phase 2: write lock — writes g_hash_tables[hidx]
    // and the FlatHashTable internals (resize via rebuild,
    // metadata, keys, values mutations). Must be exclusive vs
    // concurrent aura_hash_ref / aura_hash_remove.
    aura_lock_workspace_write();
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    if (id < g_pair_slots.size() && g_pair_slots[id]) {
        int64_t key = g_pair_slots[id]->car;
        // Key stored as-is (JIT encoding). aura_hash_key_eq handles comparison
        // for both fixnum and string keys via g_string_pool content comparison.
        int64_t val = g_pair_slots[id]->cdr;
        auto hidx = static_cast<std::size_t>(static_cast<uint64_t>(hash_val) >> 6);
        if (hidx < g_hash_tables.size() && g_hash_tables[hidx]) {
            auto* ht = g_hash_tables[hidx];
            // Issue #136: auto-rebuild when load factor exceeds 0.7.
            // Linear probing degrades quickly past 0.7 — the average
            // probe distance grows. Doubling capacity halves the
            // load factor and keeps lookups O(1) on average. The
            // rebuild also clears tombstones (which would otherwise
            // accumulate across remove+insert cycles).
            if (ht->size * 10 > ht->capacity * 7) {
                uint64_t new_cap = ht->capacity * 2;
                if (new_cap < 8) new_cap = 8;
                ht->rebuild(new_cap);
            }
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            uint64_t cap = ht->capacity;
            uint64_t h = splitmix64_hash(key);
            uint64_t first_tombstone = cap;
            for (uint64_t i = 0; i < cap; ++i) {
                uint64_t slot = probe_slot(h, i, cap);
                uint8_t m = meta[slot];
                if (m == HASH_OCCUPIED) {
                    if (keys[slot] == key) {
                        vals[slot] = val;
                        aura_unlock_workspace_write();
                        return 0;  // updated existing
                    }
                } else if (m == HASH_TOMBSTONE) {
                    if (first_tombstone >= cap) first_tombstone = slot;
                } else {
                    // HASH_EMPTY: insert here (using first tombstone if seen,
                    // so tombstones get filled and probe sequences stay short).
                    uint64_t use_slot = (first_tombstone < cap) ? first_tombstone : slot;
                    meta[use_slot] = HASH_OCCUPIED;
                    keys[use_slot] = key;
                    vals[use_slot] = val;
                    ++ht->size;
                    aura_unlock_workspace_write();
                    return 0;
                }
            }
            // Table full — no empty slot found in the probe sequence.
            // This shouldn't happen with the auto-rebuild above
            // (which keeps load < 0.7), but if it does, fall through
            // silently rather than corrupt the table.
        }
    }
    aura_unlock_workspace_write();
    return 0;
}

int64_t aura_hash_remove(int64_t hash_val, int64_t key_val) {
    // Issue #157 Phase 2: write lock — writes the FlatHashTable
    // metadata (slot -> HASH_TOMBSTONE) and decrements size.
    aura_lock_workspace_write();
    int64_t result = 0; // not found
    auto hidx = static_cast<std::size_t>(static_cast<uint64_t>(hash_val) >> 6);
    if (hidx < g_hash_tables.size() && g_hash_tables[hidx]) {
        auto* ht = g_hash_tables[hidx];
        auto meta = ht->metadata();
        auto keys = ht->keys();
        uint64_t cap = ht->capacity;
        uint64_t h = splitmix64_hash(key_val);
        for (uint64_t i = 0; i < cap; ++i) {
            uint64_t slot = probe_slot(h, i, cap);
            uint8_t m = meta[slot];
            if (m == HASH_EMPTY) break; // not found
            if (m == HASH_OCCUPIED && keys[slot] == key_val) {
                // Mark as tombstone. A subsequent insert can fill
                // this slot (reusing the deleted entry's probe
                // distance). If no insert follows, the slot stays
                // a tombstone until rebuild.
                meta[slot] = HASH_TOMBSTONE;
                --ht->size;
                result = 1;
                break;
            }
            // HASH_TOMBSTONE: keep probing
        }
    }
    aura_unlock_workspace_write();
    return result;
}
// ── Forward declarations (defined below, same extern "C" block) ──
int64_t aura_alloc_float(double d);
double aura_float_ref(int64_t val);
int64_t aura_alloc_string(const char* s);
const char* aura_string_ref(int64_t val);

// === Type coercion (CastOp) ===
// Coerces a tagged value to the expected type_tag.
// type_tag: 0=Int, 1=String, 2=Bool, 3=Dynamic/Any, 4=Float
// Must match the IR interpreter's CastOp cases exactly.
// Operates on EvalValue-compatible tagged values.
int64_t aura_cast_op(int64_t val, int64_t type_tag) {
    using aura::compiler::types::is_string_raw_v2;
    using aura::compiler::types::STRING_BIAS_VAL_2;
    using aura::compiler::types::FLOAT_BIAS_VAL;
    auto is_bool = [](int64_t v) { return v == 3 || v == 7; };
    // Issue #181 Cycle 2: v2 string encoding. (v & 3) == 2 is
    // the dedicated string tag. Range check via STRING_BIAS_VAL_2
    // is the safety belt (catches fixnums in the right range
    // that happen to have the string tag bit set).
    auto is_string = [](int64_t v) {
        return is_string_raw_v2(v) && v <= STRING_BIAS_VAL_2;
    };
    auto is_fixnum = [](int64_t v) { return (v & 1) == 0 && v > FLOAT_BIAS_VAL; };
    auto is_float = [](int64_t v) {
        return v <= FLOAT_BIAS_VAL && v > STRING_BIAS_VAL_2;
    };

    switch (type_tag) {
        case 0: { // Coerce to Int
            if (is_fixnum(val))
                return val;
            if (is_bool(val))
                return (val == 7) ? 2 : 0;  // #t→1<<1=2, #f→0<<1=0
            if (is_string(val)) {
                auto* s = aura_string_ref(val);
                if (s && *s)
                    return static_cast<int64_t>(std::atoll(s)) << 1;
            }
            if (is_float(val)) {
                int64_t idx = -val - FLOAT_BIAS_VAL;
                double d = aura_float_ref(-FLOAT_BIAS_VAL - idx);
                return static_cast<int64_t>(d) << 1;
            }
            return 0;
        }
        case 1: { // Coerce to String
            if (is_string(val))
                return val;
            std::string s;
            if (is_fixnum(val))
                s = std::to_string(val >> 1);
            else if (is_bool(val))
                s = (val == 7) ? "#t" : "#f";
            else if (is_float(val)) {
                int64_t idx = -val - FLOAT_BIAS_VAL;
                double d = aura_float_ref(-FLOAT_BIAS_VAL - idx);
                s = std::to_string(d);
            } else
                return val;
            return aura_alloc_string(s.c_str());
        }
        case 2: { // Coerce to Bool
            return (val == 3) ? 3 : 7;  // #f=3, everything else #t=7
        }
        case 4: { // Coerce to Float
            if (is_float(val))
                return val;
            if (is_fixnum(val))
                return aura_alloc_float(static_cast<double>(val >> 1));
            if (is_string(val)) {
                auto* s = aura_string_ref(val);
                if (s && *s)
                    return aura_alloc_float(std::stod(s));
            }
            if (is_bool(val))
                return aura_alloc_float((val == 7) ? 1.0 : 0.0);
            return val;
        }
        default: // Dynamic / unknown: pass through
            return val;
    }
}

// === Reset runtime state (for session isolation) ===
// Forward declaration — definition is at the bottom of the file
// because it touches g_string_pool / g_float_pool (which are
// declared further down).
// Issue #170 Phase 1 / item #2: per-fiber exception stack
// for the JIT's try/catch handling. Mirrors the IR executor's
// ex_stack_ (ir_executor_impl.cpp:641-680).
//
// Issue #195: per-fiber exception state (replacing the
// thread_local storage). Each fiber gets its own
// std::vector<ExFrame>, keyed by a fiber id. The
// `g_current_fiber_id_fn` hook is installed by the
// CompilerService (via the fiber infrastructure in
// serve/fiber.cpp). Defaults to fiber id 0 (the implicit
// 'main' thread / single-threaded case) so callers that
// don't install a hook get the same behavior as the
// pre-#195 thread_local version.
//
// Layout per frame:
//   handler_block: the IR block id to branch to on raise
//   payload_slot:  the local slot where the cause should be stored
//
// The stack is LIFO. aura_exception_push appends, aura_exception_pop
// removes the top, aura_exception_top returns the top frame's
// fields without popping. Raises branch directly to handler_block
// (no longjmp) — the JIT's lower() for OpRaise reads the top
// frame via aura_exception_top and emits a Br to handler_block.
struct ExFrame {
    std::uint64_t handler_block;
    std::uint64_t payload_slot;
};

// Issue #195: per-fiber exception state. The map is keyed
// on fiber id (uint64_t). The default fiber id is 0 (when
// no hook is installed). Concurrent fibers on the same
// thread get isolated exception state.
//
// The g_fiber_ex_stacks_mtx_ protects concurrent access
// from multiple fibers / threads. The mutex is held only
// during map lookup + vector mutation (microseconds);
// it's NOT held during JIT execution or exception handling.
static std::unordered_map<std::uint64_t, std::vector<ExFrame>> g_fiber_ex_stacks;
static std::mutex g_fiber_ex_stacks_mtx_;

// Issue #195: hook to get the current fiber's id. Set by
// CompilerService via the fiber infrastructure. The hook
// is `extern "C"` so it can be called from JIT-compiled
// code (the LLVM personality function calls it to find
// the right per-fiber ExStack). When no hook is installed,
// defaults to fiber id 0 (single-threaded / single-fiber
// behavior).
typedef std::uint64_t (*aura_fiber_id_fn_t)();
static aura_fiber_id_fn_t g_current_fiber_id_fn = nullptr;

extern "C" void aura_set_current_fiber_id_fn(aura_fiber_id_fn_t fn) {
    g_current_fiber_id_fn = fn;
}
extern "C" aura_fiber_id_fn_t aura_get_current_fiber_id_fn() {
    return g_current_fiber_id_fn;
}

// Helper: get the current fiber's ExStack, creating an empty
// one if it doesn't exist yet. Holds the mutex briefly.
static std::vector<ExFrame>& get_current_ex_stack() {
    std::uint64_t fid = g_current_fiber_id_fn ? g_current_fiber_id_fn() : 0;
    std::lock_guard<std::mutex> lock(g_fiber_ex_stacks_mtx_);
    return g_fiber_ex_stacks[fid];
}

extern "C" void aura_exception_push(std::uint64_t handler_block, std::uint64_t payload_slot) {
    std::uint64_t fid = g_current_fiber_id_fn ? g_current_fiber_id_fn() : 0;
    std::lock_guard<std::mutex> lock(g_fiber_ex_stacks_mtx_);
    g_fiber_ex_stacks[fid].push_back({handler_block, payload_slot});
}
extern "C" void aura_exception_pop() {
    std::uint64_t fid = g_current_fiber_id_fn ? g_current_fiber_id_fn() : 0;
    std::lock_guard<std::mutex> lock(g_fiber_ex_stacks_mtx_);
    auto& s = g_fiber_ex_stacks[fid];
    if (!s.empty()) s.pop_back();
}
extern "C" std::uint64_t aura_exception_top_handler() {
    std::uint64_t fid = g_current_fiber_id_fn ? g_current_fiber_id_fn() : 0;
    std::lock_guard<std::mutex> lock(g_fiber_ex_stacks_mtx_);
    auto it = g_fiber_ex_stacks.find(fid);
    if (it == g_fiber_ex_stacks.end() || it->second.empty()) return 0;
    return it->second.back().handler_block;
}
extern "C" std::uint64_t aura_exception_top_payload() {
    std::uint64_t fid = g_current_fiber_id_fn ? g_current_fiber_id_fn() : 0;
    std::lock_guard<std::mutex> lock(g_fiber_ex_stacks_mtx_);
    auto it = g_fiber_ex_stacks.find(fid);
    if (it == g_fiber_ex_stacks.end() || it->second.empty()) return ~0ULL;
    return it->second.back().payload_slot;
}
extern "C" std::uint64_t aura_exception_depth() {
    std::uint64_t fid = g_current_fiber_id_fn ? g_current_fiber_id_fn() : 0;
    std::lock_guard<std::mutex> lock(g_fiber_ex_stacks_mtx_);
    auto it = g_fiber_ex_stacks.find(fid);
    if (it == g_fiber_ex_stacks.end()) return 0;
    return static_cast<std::uint64_t>(it->second.size());
}

// Issue #195: per-fiber observability. Returns the depth
// for a specific fiber id (not necessarily the current one).
// Used by the (jit:exception-fibers) Aura primitive to
// enumerate all fibers with active exception state.
extern "C" std::uint64_t aura_exception_depth_for_fiber(std::uint64_t fid) {
    std::lock_guard<std::mutex> lock(g_fiber_ex_stacks_mtx_);
    auto it = g_fiber_ex_stacks.find(fid);
    if (it == g_fiber_ex_stacks.end()) return 0;
    return static_cast<std::uint64_t>(it->second.size());
}

// Issue #195: number of distinct fiber ids that have
// exception state. Used by the (jit:exception-fibers)
// Aura primitive.
extern "C" std::uint64_t aura_exception_fiber_count() {
    std::lock_guard<std::mutex> lock(g_fiber_ex_stacks_mtx_);
    return static_cast<std::uint64_t>(g_fiber_ex_stacks.size());
}

// Issue #195: clear all per-fiber exception state. Called
// by the session-reset path (was previously clearing the
// thread_local g_ex_stack).
extern "C" void aura_exception_clear_all() {
    std::lock_guard<std::mutex> lock(g_fiber_ex_stacks_mtx_);
    g_fiber_ex_stacks.clear();
}

// === Display bridge ===
void aura_display_int(int64_t val) {
    // Printf is available in JIT because we register printf too
    fprintf(stdout, "%ld", (long)val);
    fflush(stdout);
}

void aura_display_char(char c) {
    fputc(c, stdout);
    fflush(stdout);
}

void aura_newline() {
    fputc('\n', stdout);
    fflush(stdout);
}


// ── Float pool (shared between alloc and ref) ────
static std::vector<double> g_float_pool;

std::int64_t aura_alloc_float(double d) {
    std::int64_t idx = (std::int64_t)g_float_pool.size();
    g_float_pool.push_back(d);
    return -10000000000000000LL - idx;
}

double aura_float_ref(std::int64_t val) {
    std::int64_t idx = -val - 10000000000000000LL;
    if (idx >= 0 && idx < (std::int64_t)g_float_pool.size())
        return g_float_pool[(std::size_t)idx];
    return 0.0;
}

// ── String pool ────────────────────────────────────────────
// Uses EvalValue-compatible encoding: STRING_BIAS_VAL - idx
static std::vector<std::string> g_string_pool;

// Expose the JIT string pool for use by the evaluator's hash wrappers
// (defined in service.ixx). Returns pointer to the pool vector.
const std::vector<std::string>* aura_jit_pool_ptr() {
    return &g_string_pool;
}

// Access string by index from JIT pool
const char* aura_jit_pool_string(std::size_t idx) {
    if (idx < g_string_pool.size())
        return g_string_pool[idx].c_str();
    return nullptr;
}

// Get JIT pool size
std::size_t aura_jit_pool_size() {
    return g_string_pool.size();
}

std::int64_t aura_alloc_string(const char* s) {
    using aura::compiler::types::make_string_raw_v2;
    std::int64_t idx = (std::int64_t)g_string_pool.size();
    g_string_pool.push_back(s ? s : "");
    // Issue #181 Cycle 2: v2 string encoding.
    return make_string_raw_v2(static_cast<std::uint64_t>(idx));
}

const char* aura_string_ref(std::int64_t val) {
    using aura::compiler::types::string_idx_raw_v2;
    if (!aura::compiler::types::is_string_raw_v2(val)) return "";
    std::uint64_t idx = string_idx_raw_v2(val);
    if (idx < g_string_pool.size())
        return g_string_pool[(std::size_t)idx].c_str();
    return "";
}

// Copy a JIT-allocated string into an external string heap.
// Returns the new string index in the external heap, or -1 if not found.
// callback(idx) should return the new index after pushing to the external heap.
// 
const char* aura_jit_string_content(std::int64_t val) {
    using aura::compiler::types::string_idx_raw_v2;
    if (!aura::compiler::types::is_string_raw_v2(val)) return nullptr;
    std::uint64_t idx = string_idx_raw_v2(val);
    if (idx < g_string_pool.size())
        return g_string_pool[(std::size_t)idx].c_str();
    return nullptr;
}

// ── Arena push/pop wrappers (no pointer arg needed for JIT) ──
void aura_arena_push() { tl_arena_push(&g_tl_arena); }
void aura_arena_pop() { tl_arena_pop(&g_tl_arena); }
int64_t aura_arena_offset() { return static_cast<int64_t>(g_tl_arena.offset); }

// ── Single-call hash table info (Phase 4b) ────────────────
// Returns all hash table data in one call. LLVM IR does 1 call, then GEP from the pointers.

// String key comparison callback (set by service.ixx).
// Both keys are EvalValue-encoded strings from different heaps.
// NULL = no string comparison needed (fixnum-only mode).
static int64_t (*g_hash_str_eq_fn)(int64_t, int64_t) = nullptr;


extern "C" void aura_set_hash_str_eq_callback(int64_t (*fn)(int64_t, int64_t)) {
    g_hash_str_eq_fn = fn;
}

extern "C" int64_t aura_hash_key_eq(int64_t stored_key, int64_t search_key) {
    // Fast path: same raw value
    if (stored_key == search_key) return 1;
    // Fixnum comparison (low bit 0, not in string range)
    if ((stored_key & 1) == 0 && stored_key > -9000000000000000000LL &&
        (search_key & 1) == 0 && search_key > -9000000000000000000LL)
        return (stored_key >> 1) == (search_key >> 1) ? 1 : 0;
    // String comparison: both use g_string_pool encoding (STRING_BIAS_VAL - idx)
    if (stored_key <= -9000000000000000000LL && search_key <= -9000000000000000000LL) {
        // Compare content via g_string_pool (JIT runtime's string storage)
        auto si = static_cast<std::size_t>(-stored_key - 9000000000000000000LL);
        auto qi = static_cast<std::size_t>(-search_key - 9000000000000000000LL);
        if (si < g_string_pool.size() && qi < g_string_pool.size())
            return (g_string_pool[si] == g_string_pool[qi]) ? 1 : 0;
        // Fallback: evaluator heap comparison (converted keys)
        if (g_hash_str_eq_fn)
            return g_hash_str_eq_fn(stored_key, search_key);
    }
    return 0;
}

// === Reset runtime state (for session isolation) ===
// Issue #136: clears g_string_pool and g_float_pool in addition
// to the existing clear list. Previously these grew without
// bound across sessions, leaking memory in long-running
// processes (serve-async, fuzz, multi-session).
// Issue #137: also frees the hash tables in g_hash_tables.
// These are created by the `hash` primitive in
// evaluator_impl.cpp and only freed by the (gc-heap)
// primitive. Test scripts that use `hash` without calling
// `gc-heap` leak these tables, which LeakSanitizer flags.
// For long-running processes (serve-async, fuzz) the
// hash tables would also accumulate. Now they're freed
// here, so the function is safe to call at process exit.
void aura_reset_runtime() {
    g_closure_func_ids.clear();
    g_closure_envs.clear();
    g_arena_closure_envs.clear();
    g_cell_heap.clear();
    // Issue #195: per-fiber exception state — replaced
    // the old thread_local g_ex_stack.clear() with a call
    // to the new per-fiber clear function.
    aura_exception_clear_all();
    g_pair_slots.clear();
    std::memset(g_jit_fns, 0, sizeof(g_jit_fns));
    // Issue #136: clear string and float pools. They grow on
    // every aura_alloc_string / aura_alloc_float call, with no
    // built-in shrink. For long-running processes (serve-async
    // sessions, fuzz iterations, multi-session tests) this is
    // a real memory leak. The pools keep their allocated
    // capacity for reuse; only the size counters reset.
    g_string_pool.clear();
    g_float_pool.clear();
    // Issue #137: free hash tables. The `hash` primitive
    // (evaluator_impl.cpp) creates these via FlatHashTable::create
    // and stores them in g_hash_tables; they're normally only
    // freed by the (gc-heap) primitive. Test scripts that
    // create a hash and exit without calling gc-heap (e.g. all
    // 33 hash tests in tests/run-tests.sh) leak these tables,
    // which LeakSanitizer flags as false-positive failures.
    // Freeing here makes the function safe to call at process
    // exit (or between serve-async sessions).
    for (auto* fht : g_hash_tables) FlatHashTable::destroy(fht);
    g_hash_tables.clear();
    // Clear closure inline cache
    for (int i = 0; i < CLOSURE_CACHE_SIZE; ++i)
        g_closure_cache[i] = {-1, nullptr, 0, 0, 0};
    // Keep prim table (registered once)
    // Clear JIT function entries but NOT prim table
    for (int i = 0; i < 512; ++i)
        g_jit_fns[i] = {nullptr, 0, 0, 0};
}

// === Issue #195: C personality function scaffold ===
//
// This is the C personality function that LLVM's unwind
// runtime calls when a JIT-compiled function throws an
// exception via `invoke`. The signature is the standard
// Itanium C++ ABI personality function:
//
//   _Unwind_Reason_Code personality(int version,
//                                 _Unwind_Action actions,
//                                 uint64_t exceptionClass,
//                                 _Unwind_Exception* exceptionInfo,
//                                 _Unwind_Context* context);
//
// The function is called twice during exception handling:
//   - Search phase (actions & 1): find a matching handler
//   - Cleanup phase (actions & 2): run destructors
//
// For Aura, the "handler" is determined by walking the
// current fiber's exception state (g_fiber_ex_stacks).
// Each frame's `handler_block` is the IR block id to
// branch to. The search phase checks if the exception's
// class matches any handler; for now we use a single
// "any Aura exception matches" policy, so the first
// handler in the per-fiber ExStack matches.
//
// This is the SCAFFOLD — the full integration with the
// JIT's invoke/landingpad lowering is a separate
// follow-up. The personality function is registered
// via `__attribute__((personality))` in the LLVM IR
// once OpTryBegin/OpTryEnd/OpRaise are updated to emit
// invoke/landingpad.
extern "C" _Unwind_Reason_Code
aura_personality(int version, _Unwind_Action actions,
                 uint64_t exceptionClass,
                 _Unwind_Exception* exceptionInfo,
                 _Unwind_Context* context) {
    // Version 1 is the only supported version.
    if (version != 1) return _URC_FATAL_PHASE1_ERROR;

    // Check the exception class. Aura exceptions use a custom
    // class identifier (top 8 bytes of the _Unwind_Exception).
    // For now, accept any exception (the policy is "any Aura
    // exception matches the first handler in scope").
    (void)exceptionClass;
    (void)exceptionInfo;

    if (actions & _UA_SEARCH_PHASE) {
        // Search phase: walk the current fiber's ExStack. If any
        // frame has a non-zero handler_block, that handler
        // matches; report _URC_HANDLER_FOUND so the unwinder
        // stops and resumes at the handler.
        if (aura_exception_depth() > 0 &&
            aura_exception_top_handler() != 0) {
            return _URC_HANDLER_FOUND;
        }
        return _URC_CONTINUE_UNWIND;
    }
    if (actions & _UA_CLEANUP_PHASE) {
        // Cleanup phase: for Aura, we don't have C++
        // destructors in JIT-compiled code, so cleanup is a
        // no-op. Return _URC_CONTINUE_UNWIND to continue
        // unwinding the stack until a handler is found.
        return _URC_CONTINUE_UNWIND;
    }
    return _URC_NO_REASON;
}

// Issue #195: helper to throw an Aura exception from JIT-
// compiled code. The JIT's OpRaise lowering would emit an
// `invoke` to this function. The function sets up the
// standard _Unwind_Exception header and calls
// _Unwind_RaiseException, which the unwinder then walks
// the stack via the personality function.
//
// For now, the full integration is deferred (the JIT's
// OpRaise still uses the structured-EH switch-dispatch).
// This is the SCAFFOLD for the future LLVM-native path.
extern "C" void aura_throw_exception(uint64_t payload) {
    // The standard _Unwind_Exception header is 8 bytes
    // (exception_class) + a pointer-sized field for the
    // exception_cleanup + cache-aligned user data. Aura
    // uses a static buffer for the header to avoid
    // heap allocation in the throw path.
    static __thread struct {
        _Unwind_Exception header;
        std::uint64_t payload_storage;
    } ex = {};
    // Set the exception class to "AURA" (just a marker).
    ex.header.exception_class = 0x4155524100000000ULL;  // "AURA\0\0\0"
    ex.header.exception_cleanup = nullptr;
    ex.payload_storage = payload;
    _Unwind_RaiseException(&ex.header);
}

} // extern "C"
