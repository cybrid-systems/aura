// aura_jit_runtime.cpp — JIT runtime functions for closure/cell/pair/prim ops
// These are compiled as regular C++ and registered in the ORC JIT as symbols.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstring>
#include <new>
#include "runtime_shared.h"

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

// ── Forward declarations ──

// ── Shared pair storage (must be outside extern "C" for C++ type) ──
std::vector<PairSlot*> g_pair_slots;

// ── FlatHashTable index space (Phase 4c) ──
std::vector<FlatHashTable*> g_hash_tables;

extern "C" const FlatHashTable* aura_hash_get_flat_table(int64_t hash_val) {
    auto hidx = static_cast<std::size_t>(static_cast<uint64_t>(hash_val) >> 6);
    if (hidx >= g_hash_tables.size())
        return nullptr;
    return g_hash_tables[hidx];
}

// ── FlatHashTable allocation ──
FlatHashTable* FlatHashTable::create(uint64_t cap) {
    auto* ht = (FlatHashTable*)std::malloc(total_bytes(cap));
    if (!ht) return nullptr;
    ht->capacity = cap;
    ht->size = 0;
    auto meta = ht->metadata();
    for (uint64_t i = 0; i < cap; ++i) meta[i] = 0xFF;
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
    for (uint64_t i = 0; i < old_cap; ++i) {
        if (old_meta[i] != 0xFF) {
            for (uint64_t j = 0; j < new_cap; ++j) {
                if (new_meta[j] == 0xFF) {
                    new_meta[j] = old_meta[i];
                    new_keys[j] = old_keys[i];
                    new_vals[j] = old_vals[i];
                    ++new_ht->size;
                    break;
                }
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
    int64_t id = static_cast<int64_t>(g_closure_func_ids.size());
    g_closure_func_ids.push_back(func_id);
    g_closure_envs.emplace_back();
    return id;
}

int64_t aura_alloc_closure_arena(int64_t func_id) {
    int64_t id = static_cast<int64_t>(g_closure_func_ids.size());
    g_closure_func_ids.push_back(func_id);
    g_closure_envs.emplace_back();  // still push heap env as fallback
    g_arena_closure_envs.push_back(nullptr);  // arena ptr, set by later captures
    return id;
}

void aura_closure_capture(int64_t closure_id, int64_t idx, int64_t val) {
    if (closure_id < 0)
        return;
    size_t cid = static_cast<size_t>(closure_id);
    if (cid >= g_closure_envs.size())
        return;
    // Arena env path: allocate from TL arena on first capture
    if (cid < g_arena_closure_envs.size()) {
        int64_t*& arena_ptr = g_arena_closure_envs[cid];
        if (arena_ptr) {
            // Env already allocated on arena — write directly
            arena_ptr[static_cast<size_t>(idx)] = val;
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
            return;
        }
    }
    // Fallback: use heap std::vector
    auto& env = g_closure_envs[cid];
    if (static_cast<size_t>(idx) >= env.size())
        env.resize(static_cast<size_t>(idx) + 1);
    env[static_cast<size_t>(idx)] = val;
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
    if (func_id >= 0 && func_id < 512)
        g_jit_fns[func_id] = {fn, local_count, arg_count, env_count};
}

int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc) {
    if (closure_id < 0 || static_cast<size_t>(closure_id) >= g_closure_func_ids.size())
        return closure_id; /* match IRInterpreter: return callee_val for non-callable */

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

            return ce.fn(locals, static_cast<uint32_t>(argc));
        }
    }

    // ── Slow path: full dispatch + cache update ──
    int64_t func_id = g_closure_func_ids[static_cast<size_t>(closure_id)];
    if (func_id < 0 || func_id >= 512 || !g_jit_fns[func_id].fn)
        return 0;

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

    return result;
}

// === Cell runtime ===
static std::vector<int64_t> g_cell_heap;

int64_t aura_new_cell() {
    int64_t id = static_cast<int64_t>(g_cell_heap.size());
    g_cell_heap.push_back(0);
    return id;
}

int64_t aura_cell_get(int64_t cell_id) {
    if (cell_id >= 0 && static_cast<size_t>(cell_id) < g_cell_heap.size())
        return g_cell_heap[static_cast<size_t>(cell_id)];
    return 0;
}

void aura_cell_set(int64_t cell_id, int64_t val) {
    if (cell_id >= 0 && static_cast<size_t>(cell_id) < g_cell_heap.size())
        g_cell_heap[static_cast<size_t>(cell_id)] = val;
}

// === Pair runtime (unified PairSlot pointer-based storage) ===
// Phase 2: g_pair_slots stores pointers to PairSlot structs.
// NON_ESCAPING: PairSlot allocated from TL arena
// ESCAPED: PairSlot allocated from global heap

int64_t aura_alloc_pair(int64_t car, int64_t cdr) {
    auto* slot = (PairSlot*)malloc(sizeof(PairSlot));
    slot->car = car;
    slot->cdr = cdr;
    int64_t id = static_cast<int64_t>(g_pair_slots.size());
    g_pair_slots.push_back(slot);
    return (id << 2) | 1;
}

int64_t aura_pair_car(int64_t pair_val) {
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    if (id < g_pair_slots.size() && g_pair_slots[id])
        return g_pair_slots[id]->car;
    return 0;
}

int64_t aura_pair_cdr(int64_t pair_val) {
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    if (id < g_pair_slots.size() && g_pair_slots[id])
        return g_pair_slots[id]->cdr;
    return 0;
}

// Arena-based pair allocation (for NON_ESCAPING pairs, Phase 2)
// Allocates PairSlot from TL arena instead of global heap.
int64_t aura_alloc_pair_arena(int64_t car, int64_t cdr) {
    auto* slot = (PairSlot*)tl_arena_alloc(&g_tl_arena, sizeof(PairSlot), alignof(PairSlot));
    slot->car = car;
    slot->cdr = cdr;
    int64_t id = static_cast<int64_t>(g_pair_slots.size());
    g_pair_slots.push_back(slot);
    return (id << 2) | 1;
}

// L2 specialization: unchecked pair access (skips bounds check)
int64_t aura_pair_car_unchecked(int64_t pair_val) {
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    return g_pair_slots[id]->car;
}

int64_t aura_pair_cdr_unchecked(int64_t pair_val) {
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    return g_pair_slots[id]->cdr;
}

// === Primitive call bridge ===
// Global dispatcher function pointer — set by service.ixx to wrap evaluator primitives
static int64_t (*g_prim_dispatcher)(int64_t slot, int64_t* args, int32_t argc) = nullptr;

void aura_set_prim_dispatcher(int64_t (*fn)(int64_t, int64_t*, int32_t)) {
    g_prim_dispatcher = fn;
}

int64_t aura_prim_call(int64_t slot, int64_t a, int64_t b, int64_t count) {
    if (!g_prim_dispatcher)
        return 0;
    int64_t args[3] = {a, b, 0};
    return g_prim_dispatcher(slot, args, static_cast<int32_t>(count));
}

static int64_t (*g_hash_set_fn)(int64_t hash, int64_t key, int64_t val) = nullptr;

// Hash-ref: (hash-ref hash key) → value or void



// ── FlatHashTable direct accessors (Phase 4c-d) ──
int64_t aura_hash_ref(int64_t hash_val, int64_t key_val) {
    auto hidx = static_cast<std::size_t>(static_cast<uint64_t>(hash_val) >> 6);
    if (hidx < g_hash_tables.size() && g_hash_tables[hidx]) {
        auto* ht = g_hash_tables[hidx];
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        for (uint64_t i = 0; i < ht->capacity; ++i) {
            if (meta[i] == 0xFF) continue;
            if (keys[i] == key_val) return vals[i];
        }
    }
    return 11; // void sentinel
}

static int64_t (*g_hash_str_convert_fn)(int64_t) = nullptr;
extern "C" void aura_set_hash_str_convert_callback(int64_t (*fn)(int64_t)) {
    g_hash_str_convert_fn = fn;
}
int64_t aura_hash_set(int64_t hash_val, int64_t pair_val) {
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    if (id < g_pair_slots.size() && g_pair_slots[id]) {
        int64_t key = g_pair_slots[id]->car;
        // Key stored as-is (JIT encoding). aura_hash_key_eq handles comparison
        // for both fixnum and string keys via g_string_pool content comparison.
        int64_t val = g_pair_slots[id]->cdr;
        auto hidx = static_cast<std::size_t>(static_cast<uint64_t>(hash_val) >> 6);
        if (hidx < g_hash_tables.size() && g_hash_tables[hidx]) {
            auto* ht = g_hash_tables[hidx];
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            uint64_t cap = ht->capacity;
            uint64_t empty_slot = cap;
            for (uint64_t i = 0; i < cap; ++i) {
                if (meta[i] == 0xFF) { if (empty_slot >= cap) empty_slot = i; continue; }
                if (keys[i] == key) { vals[i] = val; return 0; }
            }
            if (empty_slot < cap) {
                meta[empty_slot] = 0x80;
                keys[empty_slot] = key;
                vals[empty_slot] = val;
                ++ht->size;
            }
        }
    }
    return 0;
}

int64_t aura_hash_remove(int64_t hash_val, int64_t key_val) {
    auto hidx = static_cast<std::size_t>(static_cast<uint64_t>(hash_val) >> 6);
    if (hidx < g_hash_tables.size() && g_hash_tables[hidx]) {
        auto* ht = g_hash_tables[hidx];
        auto meta = ht->metadata();
        auto keys = ht->keys();
        for (uint64_t i = 0; i < ht->capacity; ++i) {
            if (meta[i] == 0xFF) continue;
            if (keys[i] == key_val) { meta[i] = 0xFF; --ht->size; return 1; }
        }
    }
    return 0;
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
    auto is_bool = [](int64_t v) { return v == 3 || v == 7; };
    auto is_string = [](int64_t v) { return v <= -9000000000000000000LL; };
    auto is_fixnum = [](int64_t v) { return (v & 1) == 0 && v > -10000000000000000LL; };
    auto is_float = [](int64_t v) { return v <= -10000000000000000LL && v > -9000000000000000000LL; };

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
                int64_t idx = -val - 10000000000000000LL;
                double d = aura_float_ref(-10000000000000000LL - idx);
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
                int64_t idx = -val - 10000000000000000LL;
                double d = aura_float_ref(-10000000000000000LL - idx);
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
void aura_reset_runtime() {
    g_closure_func_ids.clear();
    g_closure_envs.clear();
    g_arena_closure_envs.clear();
    g_cell_heap.clear();
    g_pair_slots.clear();
    std::memset(g_jit_fns, 0, sizeof(g_jit_fns));
    // Clear closure inline cache
    for (int i = 0; i < CLOSURE_CACHE_SIZE; ++i)
        g_closure_cache[i] = {-1, nullptr, 0, 0, 0};
    // Keep prim table (registered once)
    // Clear JIT function entries but NOT prim table
    for (int i = 0; i < 512; ++i)
        g_jit_fns[i] = {nullptr, 0, 0, 0};
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
    std::int64_t idx = (std::int64_t)g_string_pool.size();
    g_string_pool.push_back(s ? s : "");
    return -9000000000000000000LL - idx;
}

const char* aura_string_ref(std::int64_t val) {
    std::int64_t idx = -val - 9000000000000000000LL;
    if (idx >= 0 && idx < (std::int64_t)g_string_pool.size())
        return g_string_pool[(std::size_t)idx].c_str();
    return "";
}

// Copy a JIT-allocated string into an external string heap.
// Returns the new string index in the external heap, or -1 if not found.
// callback(idx) should return the new index after pushing to the external heap.
// 
const char* aura_jit_string_content(std::int64_t val) {
    std::int64_t idx = -val - 9000000000000000000LL;
    if (idx >= 0 && idx < (std::int64_t)g_string_pool.size())
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

} // extern "C"
