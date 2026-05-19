// aura_jit_runtime.cpp — JIT runtime functions for closure/cell/pair ops
// These are compiled as regular C++ and registered in the ORC JIT as symbols.
#include <cstdint>
#include <cstdio>
#include <vector>

// ── Runtime state (shared between all JIT functions) ──────────
// In production, these would be per-session. For now, global is fine.

extern "C" {

// Closure runtime
static std::vector<int64_t> g_closure_func_ids;  // func_id for each closure
static std::vector<std::vector<int64_t>> g_closure_envs;  // env for each closure

int64_t aura_alloc_closure(int64_t func_id) {
    int64_t id = static_cast<int64_t>(g_closure_func_ids.size());
    g_closure_func_ids.push_back(func_id);
    g_closure_envs.emplace_back();
    return id;  // positive = not a cell reference
}

void aura_closure_capture(int64_t closure_id, int32_t idx, int64_t val) {
    if (closure_id >= 0 && static_cast<size_t>(closure_id) < g_closure_envs.size()) {
        auto& env = g_closure_envs[static_cast<size_t>(closure_id)];
        if (static_cast<size_t>(idx) >= env.size())
            env.resize(static_cast<size_t>(idx) + 1);
        env[static_cast<size_t>(idx)] = val;
    }
}

// Call a closure: looks up by id, calls the function via its func_id
// func_id 0 = entry function (not a closure)
// In production, this would look up the compiled function by func_id
int64_t aura_closure_call(int64_t closure_id, int64_t* args, int32_t argc) {
    fprintf(stderr, "JIT: closure call id=%ld, argc=%d: stub\\n",
            (long)closure_id, argc);
    return 0;  // TEMP: will link to compiled functions in P4
}

// Cell runtime
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

// Pair runtime
static std::vector<int64_t> g_pair_cars;
static std::vector<int64_t> g_pair_cdrs;

int64_t aura_alloc_pair(int64_t car, int64_t cdr) {
    int64_t id = static_cast<int64_t>(g_pair_cars.size());
    g_pair_cars.push_back(car);
    g_pair_cdrs.push_back(cdr);
    // Encode as negative sentinel for pair: -id - 1
    return (-id - 1);
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

// Smoke test: allocate a cell, set, get
int64_t aura_jit_runtime_test() {
    auto c = aura_new_cell();
    aura_cell_set(c, 42);
    return aura_cell_get(c);
}

} // extern "C"
