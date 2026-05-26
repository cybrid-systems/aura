// aura_jit_runtime.cpp — JIT runtime functions for closure/cell/pair/prim ops
// These are compiled as regular C++ and registered in the ORC JIT as symbols.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>

// ── Runtime state (shared between all JIT functions) ──────────
extern "C" {

// === Closure runtime ===
static std::vector<int64_t> g_closure_func_ids;          // func_id for each closure
static std::vector<std::vector<int64_t>> g_closure_envs; // env for each closure

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

void aura_closure_capture(int64_t closure_id, int64_t idx, int64_t val) {
    if (closure_id >= 0 && static_cast<size_t>(closure_id) < g_closure_envs.size()) {
        auto& env = g_closure_envs[static_cast<size_t>(closure_id)];
        if (static_cast<size_t>(idx) >= env.size())
            env.resize(static_cast<size_t>(idx) + 1);
        env[static_cast<size_t>(idx)] = val;
    }
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
        return 0;

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

            // Place captured env values first
            auto& env = g_closure_envs[static_cast<size_t>(closure_id)];
            for (int32_t i = 0; i < env_count && static_cast<size_t>(i) < env.size(); ++i)
                locals[i] = env[i];

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
    auto& env = g_closure_envs[static_cast<size_t>(closure_id)];

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

    // Place captured env values first
    for (size_t i = 0; i < env.size(); ++i) {
        if (i < static_cast<size_t>(nlocals))
            locals[i] = env[i];
    }

    // Place call arguments after env
    int32_t nargs = argc < entry.arg_count ? static_cast<int32_t>(argc) : entry.arg_count;
    int32_t env_offset = static_cast<int32_t>(env.size());
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
    g_closure_cache[cache_idx].env_count = static_cast<int32_t>(env.size());

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

// === Pair runtime ===
static std::vector<int64_t> g_pair_cars;
static std::vector<int64_t> g_pair_cdrs;

int64_t aura_alloc_pair(int64_t car, int64_t cdr) {
    int64_t id = static_cast<int64_t>(g_pair_cars.size());
    g_pair_cars.push_back(car);
    g_pair_cdrs.push_back(cdr);
    return (id << 2) | 1; // pointer tagging: low 2 bits = 01
}

int64_t aura_pair_car(int64_t pair_val) {
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    if (id < g_pair_cars.size())
        return g_pair_cars[id];
    return 0;
}

int64_t aura_pair_cdr(int64_t pair_val) {
    uint64_t id = static_cast<uint64_t>(pair_val >> 2);
    if (id < g_pair_cdrs.size())
        return g_pair_cdrs[id];
    return 0;
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

// === Type coercion (CastOp) ===
// Checks if value matches expected type_tag and coerces if needed.
// type_tag: 0=Int, 1=String, 2=Bool, 3=Dynamic/Any (always passes)
// Returns the (possibly coerced) value.
// The IR interpreter path reports blame with source location.
int64_t aura_cast_op(int64_t val, int64_t type_tag) {
    (void)type_tag;
    // For now: all values are int64_t, so Int/Dynamic always pass.
    // Future: proper runtime type checks when EvalValue is fully tagged.
    return val;
}

// === Reset runtime state (for session isolation) ===
void aura_reset_runtime() {
    g_closure_func_ids.clear();
    g_closure_envs.clear();
    g_cell_heap.clear();
    g_pair_cars.clear();
    g_pair_cdrs.clear();
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

} // extern "C"
