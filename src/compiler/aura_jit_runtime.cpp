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
static std::vector<int64_t> g_closure_func_ids;  // func_id for each closure
static std::vector<std::vector<int64_t>> g_closure_envs;  // env for each closure

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

void aura_register_fn(int64_t func_id, int64_t (*fn)(int64_t*, uint32_t),
                       int32_t local_count, int32_t arg_count, int32_t env_count) {
    if (func_id >= 0 && func_id < 512)
        g_jit_fns[func_id] = {fn, local_count, arg_count, env_count};
}

int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc) {
    if (closure_id < 0 || static_cast<size_t>(closure_id) >= g_closure_func_ids.size()) {
        return 0;
    }
    int64_t func_id = g_closure_func_ids[static_cast<size_t>(closure_id)];
    if (func_id < 0 || func_id >= 512 || !g_jit_fns[func_id].fn)
        return 0;

    auto& entry = g_jit_fns[func_id];
    auto& env = g_closure_envs[static_cast<size_t>(closure_id)];

    // Allocate locals array
    int32_t nlocals = entry.local_count > 0 ? entry.local_count : 16;
    std::vector<int64_t> locals(static_cast<size_t>(nlocals), 0);

    // Place captured env values first (Arg 0..env.size()-1)
    for (size_t i = 0; i < env.size(); ++i) {
        if (i < static_cast<size_t>(nlocals))
            locals[i] = env[i];
    }

    // Place call arguments after env (Arg env.size()..env.size()+arg_count-1)
    int32_t nargs = argc < entry.arg_count ? static_cast<int32_t>(argc) : entry.arg_count;
    int32_t env_offset = static_cast<int32_t>(env.size());
    for (int32_t i = 0; i < nargs; ++i) {
        size_t slot = static_cast<size_t>(env_offset + i);
        if (slot < locals.size())
            locals[slot] = args[i];
    }

    int64_t result = entry.fn(locals.data(), static_cast<uint32_t>(argc));
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
    return (-id - 1);  // negative sentinel
}

int64_t aura_pair_car(int64_t pair_val) {
    int64_t id = -pair_val - 1;
    if (id >= 0 && static_cast<size_t>(id) < g_pair_cars.size())
        return g_pair_cars[static_cast<size_t>(id)];
    return 0;
}

int64_t aura_pair_cdr(int64_t pair_val) {
    int64_t id = -pair_val - 1;
    if (id >= 0 && static_cast<size_t>(id) < g_pair_cdrs.size())
        return g_pair_cdrs[static_cast<size_t>(id)];
    return 0;
}

// === Primitive call bridge ===
// Global dispatcher function pointer — set by service.ixx to wrap evaluator primitives
static int64_t (*g_prim_dispatcher)(int64_t slot, int64_t* args, int32_t argc) = nullptr;

void aura_set_prim_dispatcher(int64_t (*fn)(int64_t, int64_t*, int32_t)) {
    g_prim_dispatcher = fn;
}

int64_t aura_prim_call(int64_t slot, int64_t a, int64_t b, int64_t count) {
    if (!g_prim_dispatcher) return 0;
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

} // extern "C"
