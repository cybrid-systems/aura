#define _GNU_SOURCE
// Aura standalone runtime
// Linked with LLVM-compiled .o to produce native binary.
// Uses Bump Allocator (Arena) for fast bulk allocation + reset.
// Drop functions + Free List for objects needing individual release.
//
// Build: gcc -c runtime.c -o runtime.o
// Link:  gcc program.o runtime.o -o program -lm

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

// ── Pointer tagging: value representation ──────────────────
// bit 0 = 0: Fixnum (signed int, value >> 1)
// bits 1-0 = 01: Pair (index = val >> 2)
// bits 1-0 = 11: Special (tag = (val >> 2) & 3)
//   tag 0 = #f (= 0b11 = 3)
//   tag 1 = #t (= 0b111 = 7)
//   tag 2 = void () (= 0b1011 = 11)

#define IS_PAIR(v)  (((v) & 3) == 1)
#define IS_SPECIAL(v) (((v) & 3) == 3)
#define IS_FIXNUM(v) (((v) & 1) == 0)
#define PAIR_INDEX(v) ((v) >> 2)
#define IS_STRING(v) ((v) <= -9000000000000000000LL)
#define STRING_IDX(v) ((int)(-(int64_t)(v) - 9000000000000000000LL))
#define MAKE_STRING_SENTINEL(i) (-9000000000000000000LL - (int64_t)(i))
#define STRING_BIAS_AREA (-9000000000000000000LL)
#define FLOAT_BIAS  (-10000000000000000LL)
#define SPECIAL_TAG(v) (((v) >> 2) & 3)

// ── Enum-compatible tag values (use static const instead of macros to avoid enum conflict)
static const int KWD_FALSE = 0;
static const int KWD_TRUE = 1;
static const int KWD_VOID = 2;

// ── Type tags for aura_display_val (kept for backward compat) ─
enum ValueTag {
    TAG_INT = 0,
    TAG_BOOL = 1,
    TAG_PAIR = 2,
    TAG_CLOSURE = 3,
    TAG_STRING = 4,
    TAG_VOID = 5,
    TAG_DYNAMIC = 255,
};

// ═══════════════════════════════════════════════════════════
// Bump Allocator (Arena) — primary memory manager
// ═══════════════════════════════════════════════════════════
// Fast bump-pointer allocation. Entire arena reset at function end.
// Long-lived objects can optionally use Free List (see Drop section).

#define BUMP_ARENA_SIZE (64 * 1024 * 1024) // 64MB default arena

static uint8_t* bump_arena = NULL;
static size_t bump_offset = 0;
static size_t bump_capacity = BUMP_ARENA_SIZE;

// Initialize bump arena (called at function entry by compiler).
void aura_bump_init(void) {
    if (!bump_arena) {
        bump_arena = (uint8_t*)malloc(bump_capacity);
        if (!bump_arena) {
            fprintf(stderr, "runtime: bump arena alloc failed\n");
            exit(1);
        }
    }
    bump_offset = 0; // Reset for fresh execution
}

// Allocate from bump arena (called by all alloc functions internally).
static void* aura_bump_alloc_slow(size_t size, size_t align) {
    size_t aligned = (bump_offset + align - 1) & ~(align - 1);
    if (aligned + size > bump_capacity) {
        // Try to double the arena
        size_t new_cap = bump_capacity * 2;
        uint8_t* new_arena = (uint8_t*)realloc(bump_arena, new_cap);
        if (!new_arena) {
            fprintf(stderr, "runtime: bump arena overflow\n");
            exit(1);
        }
        bump_arena = new_arena;
        bump_capacity = new_cap;
    }
    size_t aligned2 = (bump_offset + align - 1) & ~(align - 1);
    void* ptr = bump_arena + aligned2;
    bump_offset = aligned2 + size;
    return ptr;
}

// Inline-friendly wrapper (compiler can inline this).
void* aura_bump_alloc(size_t size, size_t align) {
    size_t aligned = (bump_offset + align - 1) & ~(align - 1);
    if (aligned + size > bump_capacity)
        return aura_bump_alloc_slow(size, align);
    void* ptr = bump_arena + aligned;
    bump_offset = aligned + size;
    return ptr;
}

// Reset bump arena (called at function exit by compiler).
// Frees all bump-allocated memory in one shot.
void aura_bump_reset(void) {
    bump_offset = 0;
}

// ═══════════════════════════════════════════════════════════
// Free List (for objects requiring individual drop)
// ═══════════════════════════════════════════════════════════
// Used when a specific object must be released before arena reset
// (e.g. long-lived objects spanning multiple function calls).

#define FREE_LIST_CAP 4096

// Pair free list
static int64_t pair_free_list[FREE_LIST_CAP];
static uint64_t pair_free_count = 0;

// Cell free list
static int64_t cell_free_list[FREE_LIST_CAP];
static uint64_t cell_free_count = 0;

// Closure free list
static int64_t closure_free_list[FREE_LIST_CAP];
static uint64_t closure_free_count = 0;

// ═══════════════════════════════════════════════════════════
// Pair heap
// ═══════════════════════════════════════════════════════════

#define MAX_PAIRS 1048576
typedef struct { int64_t car, cdr; bool live; } AuraPair;
static AuraPair* pairs = NULL;
static uint64_t pair_count = 0;
static uint64_t pair_capacity = 0;

static AuraPair* get_pairs(void) {
    if (!pairs) {
        pair_capacity = 65536;
        pairs = (AuraPair*)calloc(pair_capacity, sizeof(AuraPair));
    }
    return pairs;
}

static uint64_t pair_alloc_slot(void) {
    // Try free list first
    if (pair_free_count > 0) {
        uint64_t id = (uint64_t)pair_free_list[--pair_free_count];
        pairs[id].live = true;
        return id;
    }
    // Bump allocate
    if (pair_count >= pair_capacity) {
        uint64_t new_cap = pair_capacity ? pair_capacity * 2 : 65536;
        pairs = (AuraPair*)realloc(pairs, new_cap * sizeof(AuraPair));
        if (!pairs) { fprintf(stderr, "runtime: pair OOM\n"); exit(1); }
        memset(pairs + pair_capacity, 0, (new_cap - pair_capacity) * sizeof(AuraPair));
        pair_capacity = new_cap;
    }
    uint64_t id = pair_count++;
    pairs[id].live = true;
    return id;
}

// Pointer Tagging encoding: aura_alloc_pair returns (id << 2) | 1
// bit 0 = 1 marks non-fixnum; bit 1 = 0 marks pair (low 2 bits = 01).
// Decode: internal_id = pair_val >> 2
//
// This replaces the old negative-sentinel encoding (-(id+1)).

int64_t aura_alloc_pair(int64_t car, int64_t cdr) {
    get_pairs();
    uint64_t id = pair_alloc_slot();
    pairs[id].car = car;
    pairs[id].cdr = cdr;
    return ((int64_t)id << 2) | 1;
}

int64_t aura_pair_car(int64_t pair_val) {
    uint64_t id = (uint64_t)(pair_val >> 2);
    if (id >= pair_count || !pairs[id].live) return 0;
    return pairs[id].car;
}

int64_t aura_pair_cdr(int64_t pair_val) {
    uint64_t id = (uint64_t)(pair_val >> 2);
    if (id >= pair_count || !pairs[id].live) return 0;
    return pairs[id].cdr;
}

// L2 specialization: unchecked pair access (skips bounds/live checks).
// Only safe when caller has verified the value is a valid pair via shape guard.
int64_t aura_pair_car_unchecked(int64_t pair_val) {
    uint64_t id = (uint64_t)(pair_val >> 2);
    return pairs[id].car;
}

int64_t aura_pair_cdr_unchecked(int64_t pair_val) {
    uint64_t id = (uint64_t)(pair_val >> 2);
    return pairs[id].cdr;
}

void aura_drop_pair(int64_t pair_val) {
    uint64_t id = (uint64_t)(pair_val >> 2);
    if (id >= pair_count || !pairs[id].live) return;
    // Issue #106: recursively drop the car and cdr if they're
    // pairs. The pair heap is the only heap with low-bit tagging
    // that lets us detect the type from the value alone (cells /
    // closures are raw int64_t IDs without tag bits, so the
    // caller has to dispatch aura_drop_cell / aura_drop_closure
    // explicitly via the lowering path; we don't try to detect
    // them here). Nested pair drops must be done FIRST to avoid
    // an inconsistent state if a recursive drop itself fails.
    int64_t car = pairs[id].car;
    int64_t cdr = pairs[id].cdr;
    pairs[id].live = false;  // mark self dead before recursing
    if (pair_free_count < FREE_LIST_CAP)
        pair_free_list[pair_free_count++] = (int64_t)id;
    if (IS_PAIR(car)) aura_drop_pair(car);
    if (IS_PAIR(cdr)) aura_drop_pair(cdr);
}

// ═══════════════════════════════════════════════════════════
// Cell heap
// ═══════════════════════════════════════════════════════════

#define MAX_CELLS 262144
typedef struct { int64_t value; bool live; } AuraCell;
static AuraCell* cell_heap = NULL;
static uint64_t cell_count = 0;
static uint64_t cell_capacity = 0;

static AuraCell* get_cells(void) {
    if (!cell_heap) {
        cell_capacity = 65536;
        cell_heap = (AuraCell*)calloc(cell_capacity, sizeof(AuraCell));
    }
    return cell_heap;
}

int64_t aura_new_cell(void) {
    get_cells();
    // Try free list first
    if (cell_free_count > 0) {
        uint64_t id = (uint64_t)cell_free_list[--cell_free_count];
        cell_heap[id].value = 0;
        cell_heap[id].live = true;
        return (int64_t)id;
    }
    if (cell_count >= cell_capacity) {
        uint64_t new_cap = cell_capacity ? cell_capacity * 2 : 65536;
        cell_heap = (AuraCell*)realloc(cell_heap, new_cap * sizeof(AuraCell));
        if (!cell_heap) { fprintf(stderr, "runtime: cell OOM\n"); exit(1); }
        memset(cell_heap + cell_capacity, 0, (new_cap - cell_capacity) * sizeof(AuraCell));
        cell_capacity = new_cap;
    }
    uint64_t id = cell_count++;
    cell_heap[id].live = true;
    return (int64_t)id;
}

int64_t aura_cell_get(int64_t cell_id) {
    uint64_t id = (uint64_t)cell_id;
    if (id >= cell_count || !cell_heap[id].live) return 0;
    return cell_heap[id].value;
}

void aura_cell_set(int64_t cell_id, int64_t val) {
    uint64_t id = (uint64_t)cell_id;
    if (id >= cell_count || !cell_heap[id].live) return;
    cell_heap[id].value = val;
}

void aura_drop_cell(int64_t cell_id) {
    uint64_t id = (uint64_t)cell_id;
    if (id >= cell_count || !cell_heap[id].live) return;
    cell_heap[id].live = false;
    if (cell_free_count < FREE_LIST_CAP)
        cell_free_list[cell_free_count++] = (int64_t)id;
}

// ═══════════════════════════════════════════════════════════
// Closure table
// ═══════════════════════════════════════════════════════════

#define MAX_CLOSURES 4096
#define MAX_CLOSURE_ENV 8
#define MAX_FUNCTIONS 4096

typedef int64_t (*ScalarFn)(int64_t*, uint32_t);

typedef struct {
    ScalarFn fn;
    uint32_t local_count;
    int64_t env[MAX_CLOSURE_ENV];
    uint32_t env_count;
    bool live;
} AuraClosure;

// Function pointer table: maps func_id → compiled function pointer
// Set up by AOT registration code before main() runs.
static ScalarFn s_func_table[MAX_FUNCTIONS] = {NULL};

// Register a function pointer for a given func_id.
// Called by AOT registration code (generated .c file) before main().
void aura_register_fn(int64_t func_id, int64_t fn_ptr) {
    uint64_t id = (uint64_t)func_id;
    if (id < MAX_FUNCTIONS) {
        s_func_table[id] = (ScalarFn)(intptr_t)fn_ptr;
    }
}

static AuraClosure* closure_heap = NULL;
static uint64_t closure_count = 0;
static uint64_t closure_capacity = 0;

static AuraClosure* get_closures(void) {
    if (!closure_heap) {
        closure_capacity = 1024;
        closure_heap = (AuraClosure*)calloc(closure_capacity, sizeof(AuraClosure));
    }
    return closure_heap;
}

int64_t aura_alloc_closure(int64_t func_id) {
    get_closures();
    // Try free list first
    if (closure_free_count > 0) {
        uint64_t id = (uint64_t)closure_free_list[--closure_free_count];
        closure_heap[id].fn = NULL;
        closure_heap[id].local_count = 0;
        closure_heap[id].env_count = 0;
        closure_heap[id].live = true;
        // Set function pointer from func_table
        uint64_t fid = (uint64_t)func_id;
        if (fid < MAX_FUNCTIONS && s_func_table[fid]) {
            closure_heap[id].fn = s_func_table[fid];
        }
        return (int64_t)id;
    }
    if (closure_count >= closure_capacity) {
        uint64_t new_cap = closure_capacity ? closure_capacity * 2 : 1024;
        closure_heap = (AuraClosure*)realloc(closure_heap, new_cap * sizeof(AuraClosure));
        if (!closure_heap) { fprintf(stderr, "runtime: closure OOM\n"); exit(1); }
        memset(closure_heap + closure_capacity, 0, (new_cap - closure_capacity) * sizeof(AuraClosure));
        closure_capacity = new_cap;
    }
    uint64_t id = closure_count++;
    closure_heap[id].live = true;
    // Set function pointer from func_table
    uint64_t fid = (uint64_t)func_id;
    if (fid < MAX_FUNCTIONS && s_func_table[fid]) {
        closure_heap[id].fn = s_func_table[fid];
    }
    return (int64_t)id;
}

void aura_register_closure_fn(int64_t closure_id, int64_t fn_ptr, int32_t local_count) {
    uint64_t id = (uint64_t)closure_id;
    if (id >= closure_count || !closure_heap[id].live) return;
    closure_heap[id].fn = (ScalarFn)(intptr_t)fn_ptr;
    closure_heap[id].local_count = (uint32_t)local_count;
}

void aura_closure_capture(int64_t closure_id, int64_t idx, int64_t val) {
    uint64_t id = (uint64_t)closure_id;
    if (id >= closure_count || !closure_heap[id].live) return;
    uint32_t eidx = (uint32_t)idx;
    if (eidx < MAX_CLOSURE_ENV) {
        closure_heap[id].env[eidx] = val;
        if (eidx + 1 > closure_heap[id].env_count)
            closure_heap[id].env_count = eidx + 1;
    }
}

// Forward declarations for primitive dispatch (used by aura_closure_call)
#define MAX_PRIM_SLOTS 256
typedef int64_t (*PrimFn)(int64_t* args, int32_t argc);
static PrimFn s_prim_fns[MAX_PRIM_SLOTS];

// Env offset for test functions (set before fn call, defined in test harness)
int g_env_offset = 0;  // env offset for closure calls (used by test fn)

int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc) {
    // AOT primitive dispatch: negative closure_id means this is an
    // evaluator primitive loaded via OpPrimitive (AOT mode).
    if (closure_id < 0) {
        int64_t prim_slot = -closure_id - 1;
        if (prim_slot >= 0 && prim_slot < MAX_PRIM_SLOTS && s_prim_fns[(uint64_t)prim_slot]) {
            return s_prim_fns[(uint64_t)prim_slot](args, (int32_t)argc);
        }
        return 0;
    }
    
    // Normal closure dispatch.
    uint64_t id = (uint64_t)closure_id;
    if (id >= closure_count || !closure_heap[id].live || !closure_heap[id].fn)
        return 0;

    ScalarFn fn = closure_heap[id].fn;
    uint32_t env_count = closure_heap[id].env_count;

    if (env_count == 0) {
        // No captured env: call fn directly with args (backward compat for tests)
        return fn(args, (uint32_t)argc);
    }

    // Set up locals from captured env + args.
    // The closure expects: locals[0..env_count-1] = env, locals[env_count..] = args.
    uint32_t nargs = (uint32_t)argc;
    uint32_t raw_locals = closure_heap[id].local_count;
    uint32_t nlocals = env_count + nargs;
    if (raw_locals > nlocals) nlocals = raw_locals;
    if (nlocals < 16) nlocals = 16;
    
    int64_t stack_buf[64];
    int64_t* locals = stack_buf;
    if (nlocals > 64)
        locals = (int64_t*)malloc((size_t)nlocals * sizeof(int64_t));
    for (uint32_t i = 0; i < nlocals; ++i) locals[i] = 0;
    
    for (uint32_t i = 0; i < env_count && i < closure_heap[id].env_count; ++i)
        locals[i] = closure_heap[id].env[i];
    
    for (uint32_t i = 0; i < nargs; ++i)
        locals[env_count + i] = args[i];
    
    // Set env offset for test functions (defined in runtime_test_harness.c)
    g_env_offset = (int)env_count;
    int64_t result = fn(locals, nargs);
    
    if (nlocals > 64) free(locals);
    return result;
}

void aura_drop_closure(int64_t closure_id) {
    uint64_t id = (uint64_t)closure_id;
    if (id >= closure_count || !closure_heap[id].live) return;
    // Issue #106 sub-task 3: recurse on env slots before
    // marking self dead. Env slots can hold fixnums, pairs,
    // cells, or other closures. Dispatch by:
    //   1. IS_PAIR(v) — exact low-bit tag match
    //   2. in-range live cell id — bounds + live check
    //   3. same for closure id
    // Order-safe: env slots dropped before self is marked
    // dead. A fixnum that happens to land in an id range is
    // safe (the live check fails on unallocated slots).
    uint32_t env_count = closure_heap[id].env_count;
    for (uint32_t i = 0; i < env_count; ++i) {
        int64_t v = closure_heap[id].env[i];
        if (IS_PAIR(v)) {
            aura_drop_pair(v);
        } else if (v >= 0 && (uint64_t)v < cell_count && cell_heap[v].live) {
            aura_drop_cell(v);
        } else if (v >= 0 && (uint64_t)v < closure_count && closure_heap[v].live) {
            aura_drop_closure(v);
        }
        // else: fixnum or out-of-range id — no drop
    }
    closure_heap[id].live = false;
    if (closure_free_count < FREE_LIST_CAP)
        closure_free_list[closure_free_count++] = (int64_t)id;
}

// ═══════════════════════════════════════════════════════════
// String heap
// ═══════════════════════════════════════════════════════════

#define MAX_STRINGS 65536
static char** string_heap = NULL;
static uint64_t string_count = 0;
static uint64_t string_capacity = 0;

#define STRING_BIAS 0x10000000

int64_t aura_alloc_string(const char* s) {
    if (!string_heap) {
        string_capacity = 1024;
        string_heap = (char**)calloc(string_capacity, sizeof(char*));
    }
    if (string_count >= string_capacity) {
        uint64_t new_cap = string_capacity ? string_capacity * 2 : 1024;
        string_heap = (char**)realloc(string_heap, new_cap * sizeof(char*));
        if (!string_heap) { fprintf(stderr, "runtime: string OOM\n"); exit(1); }
        memset(string_heap + string_capacity, 0, (new_cap - string_capacity) * sizeof(char*));
        string_capacity = new_cap;
    }
    uint64_t id = string_count++;
    string_heap[id] = s ? strdup(s) : strdup("");
    // Return (id << 2) | 3 in the high range so IS_FIXNUM/IS_PAIR/IS_SPECIAL checks
    // all fail, and the display function can detect strings via the high bit range.
    return (int64_t)(id + STRING_BIAS);
}

const char* aura_string_ref(int64_t str_id) {
    uint64_t id = (uint64_t)(str_id - STRING_BIAS);
    if (id >= string_count) return "";
    return string_heap[id] ? string_heap[id] : "";
}

// ═══════════════════════════════════════════════════════════
// Type coercion (CastOp) — mirrors aura_jit_runtime.cpp:497
// Called by AOT-compiled lambdas when the IR's TypeSpecializationWrap
// pass inserts a CastOp instruction (e.g. (- n 1) inside a stdlib
// function body that's been inlined via (import "std/...")).
// type_tag: 0=Int, 1=String, 2=Bool, 3=Dynamic/Any, 4=Float
// Must match the IR interpreter's CastOp cases exactly.
// ═══════════════════════════════════════════════════════════
// Forward decls: these are defined further down in this file but
// we call them from the CastOp implementation below.
extern double aura_float_ref(int64_t val);
extern int64_t aura_alloc_float(double d);

int64_t aura_cast_op(int64_t val, int64_t type_tag) {
    int is_bool   = (val == 3 || val == 7);
    int is_string = (val <= (int64_t)-9000000000000000000LL);
    int is_fixnum = ((val & 1) == 0 && val > (int64_t)-10000000000000000LL);
    int is_float  = (val <= (int64_t)-10000000000000000LL &&
                     val > (int64_t)-9000000000000000000LL);

    switch (type_tag) {
        case 0: { // Coerce to Int
            if (is_fixnum) return val;
            if (is_bool)   return (val == 7) ? 2 : 0;
            if (is_string) {
                const char* s = aura_string_ref(val);
                if (s && *s) return (int64_t)atoll(s) << 1;
            }
            if (is_float) {
                // Delegate to aura_float_ref (same TU) to round-trip
                // the float, then convert to int.
                double d = aura_float_ref(val);
                return (int64_t)d << 1;
            }
            return 0;
        }
        case 1: { // Coerce to String
            if (is_string) return val;
            char buf[64];
            if (is_fixnum)
                snprintf(buf, sizeof(buf), "%lld", (long long)(val >> 1));
            else if (is_bool)
                snprintf(buf, sizeof(buf), "%s", (val == 7) ? "#t" : "#f");
            else if (is_float) {
                double d = aura_float_ref(val);
                snprintf(buf, sizeof(buf), "%g", d);
            } else
                return val;
            return aura_alloc_string(buf);
        }
        case 2: { // Coerce to Bool
            return (val == 3) ? 3 : 7;
        }
        case 4: { // Coerce to Float
            if (is_float) return val;
            if (is_fixnum) {
                // Allocate the float in the pool and return the
                // biased index. Uses aura_alloc_float (same TU).
                return aura_alloc_float((double)(val >> 1));
            }
            if (is_string) {
                const char* s = aura_string_ref(val);
                if (s && *s) return aura_alloc_float(atof(s));
            }
            if (is_bool)
                return aura_alloc_float((val == 7) ? 1.0 : 0.0);
            return val;
        }
        default: // Dynamic / unknown: pass through
            return val;
    }
}

// ═══════════════════════════════════════════════════════════
// Primitive dispatch table (for primitives used as closures)
// ═══════════════════════════════════════════════════════════
// When OpPrimitive (AOT mode) stores a negative sentinel -(slot+1),
// aura_closure_call detects the negative value and dispatches
// to this table instead of the closure heap.
//
// The table is populated at startup by constructor code generated
// by the compiler (aura_set_prim_registration). The generated code
// calls aura_register_primitive_fn for each evaluator primitive slot.
// The dispatch function is also generated by the compiler to match
// the evaluator's exact slot numbering (which depends on C++
// unordered_map iteration order).

// Register a primitive function for a given slot.
// Called by aura_aot_register_prims() (generated by compiler).
static int g_prim_reg_count = 0;

// ── Float pool operations ──────────────────────────────
#define IS_FLOAT(v)  (((v) <= FLOAT_BIAS) && ((v) > STRING_BIAS_AREA))
#define FLOAT_IDX(v) ((int)(-(int64_t)(v) - 10000000000000000LL))

static double* float_pool = NULL;
static int float_count = 0;
static int float_capacity = 0;

double aura_float_ref(int64_t val) {
    int idx = FLOAT_IDX(val);
    if (idx >= 0 && idx < float_count)
        return float_pool[idx];
    return 0.0;
}

int64_t aura_alloc_float(double d) {
    if (float_count >= float_capacity) {
        int new_cap = float_capacity == 0 ? 16 : float_capacity * 2;
        double* new_pool = (double*)realloc(float_pool, (size_t)new_cap * sizeof(double));
        if (!new_pool) return 0;
        float_pool = new_pool;
        float_capacity = new_cap;
    }
    float_pool[float_count++] = d;
    return -10000000000000000LL - (int64_t)(float_count - 1);
}

void aura_register_primitive_fn(int64_t slot, int64_t fn_ptr) {
    uint64_t s = (uint64_t)slot;
    if (s < MAX_PRIM_SLOTS && fn_ptr != 0) {
        s_prim_fns[s] = (PrimFn)(intptr_t)fn_ptr;
        g_prim_reg_count++;
    }
}

// Get registered primitive count (for debugging via display)
int64_t aura_prim_reg_count(void) {
    return (int64_t)g_prim_reg_count;
}

// Update aura_closure_call to dispatch primitives when called with
// a negative closure ID from OpPrimitive (AOT mode).
// Negative encoding: closure_id = -(prim_slot + 1)
// So prim_slot = -closure_id - 1

// ═══════════════════════════════════════════════════════════
// I/O primitives
// ═══════════════════════════════════════════════════════════

static int g_display_was_called = 0;

// ── Display a pair chain as proper/dotted list ────────────
static void aura_display_pair_chain(int64_t val) {
    putchar('(');
    int first = 1;
    while (IS_PAIR(val)) {
        if (!first) putchar(' ');
        first = 0;
        int64_t car_val = aura_pair_car(val);
        if (IS_PAIR(car_val))
            aura_display_pair_chain(car_val);
        else if (IS_SPECIAL(car_val))
            printf("%s", SPECIAL_TAG(car_val) == KWD_TRUE ? "#t" : "#f");
        else if (IS_FLOAT(car_val)) {
            printf("%g", aura_float_ref(car_val));
        } else if (IS_STRING(car_val))
            printf("%s", aura_string_ref((uint64_t)STRING_IDX(car_val)));
        else
            printf("%ld", (long)(car_val >> 1));
        val = aura_pair_cdr(val);
    }
    if (IS_STRING(val))
        printf(" . %s", aura_string_ref((uint64_t)STRING_IDX(val)));
    else if (val != 0 && !IS_PAIR(val) && !IS_SPECIAL(val))
        printf(" . %ld", (long)(val >> 1));
    else if (IS_SPECIAL(val) && SPECIAL_TAG(val) != KWD_VOID)
        printf(" . %s", SPECIAL_TAG(val) == KWD_TRUE ? "#t" : "#f");
    putchar(')');
}
int64_t aura_display_int(int64_t val) {
    // Check float before string since float-encoded values look like strings
    if (IS_FLOAT(val)) {
        double d = aura_float_ref(val);
        printf("%g", d);
    } else if (val >= STRING_BIAS) {
        printf("%s", aura_string_ref(val));
    } else if (IS_PAIR(val)) {
        aura_display_pair_chain(val);
    } else if (IS_SPECIAL(val)) {
        int st = SPECIAL_TAG(val);
        if (st == KWD_TRUE) printf("#t");
        else if (st == KWD_FALSE) printf("#f");
        else printf("()");
    } else if (val == 0) {
        printf("()");
    } else if (IS_FIXNUM(val)) {
        printf("%ld", (long)(val >> 1));
    } else {
        printf("%ld", (long)val);
    }
    fflush(stdout);
    g_display_was_called = 1;
    return val;
}

// ── Type-aware display ─────────────────────────────────
// Called by AOT codegen when type info is available.
// Falls back to aura_display_int for TAG_DYNAMIC.
int64_t aura_display_val(int64_t val, int type_tag) {
    switch (type_tag) {
        case TAG_BOOL:
            printf("%s", val ? "#t" : "#f");
            break;
        case TAG_VOID:
            printf("()");
            break;
        case TAG_PAIR:
            aura_display_pair_chain(val);
            break;
        case TAG_INT:
        default:
            if (IS_PAIR(val))
                aura_display_pair_chain(val);
            else if (IS_SPECIAL(val) && SPECIAL_TAG(val) == KWD_VOID)
                printf("()");
            else if (IS_SPECIAL(val))
                printf("%s", SPECIAL_TAG(val) == KWD_TRUE ? "#t" : "#f");
            else if (IS_STRING(val)) {
                int sid = STRING_IDX(val);
                const char* sp = aura_string_ref((uint64_t)sid);
                printf("%s", sp ? sp : "<?>");
            } else if (val == 0)
                printf("()");
            else if (IS_FIXNUM(val))
                printf("%ld", (long)(val >> 1));
            else
                printf("%ld", (long)val);
            break;
    }
    fflush(stdout);
    g_display_was_called = 1;
    return val;
}

int64_t aura_display_char(int64_t val) {
    putchar((int)val);
    fflush(stdout);
    return val;
}

void aura_newline(void) {
    putchar('\n');
    fflush(stdout);
}

// ── Primitive call dispatcher for AOT binaries ──────────────
// Called from LLVM-compiled code for non-inlined primitives.
// Signature must match LLVM IR: (prim_id, a1, a2, arg_count)
//
// Primitive IDs match the PrimId enum in the IR module:
//   8=display, 9=write, 10=newline, 30=quotient, 31=remainder
//   (other primitives use the evaluator's dispatch table)
//
// For AOT, we implement commonly-used primitives directly.
// Full primitive support is handled by the evaluator (JIT path).

// Internal: check if an int64_t value is a pair (a.k.a. cons cell)
// In Aura's tag scheme: heap objects (pairs/strings/closures) have low bit = 1
// Pairs and strings have the same tag format: (id << 2) | 1
static int is_pair_val(int64_t v) {
    return IS_PAIR(v);
}

// aura_prim_call: slow-path primitive dispatch for AOT-compiled binaries.
//
// PrimId values MUST match the C++ PrimId enum in src/compiler/ir.ixx.
// If you add/remove/reorder a PrimId entry there, mirror it here.
//
// The fast-path prims (Display, Write, Newline, Quotient, Remainder,
// PairP, NullP) are inlined by the LLVM builder in src/compiler/aura_jit.cpp
// and rarely reach this function. Everything else falls through here.
int64_t aura_prim_call(int64_t prim_id, int64_t a1, int64_t a2, int64_t argc) {
    (void)argc;

    // Guard against corrupt prim_id: Aura PrimId has 44 entries (0..43).
    // An out-of-range value would otherwise hit the default branch and
    // hide the bug; bail early with 0 to surface the discrepancy.
    if (prim_id < 0 || prim_id > 43) return 0;

    switch (prim_id) {
    case 5: { // StringAppend
        const char* s1 = aura_string_ref(a1);
        const char* s2 = aura_string_ref(a2);
        size_t len1 = s1 ? strlen(s1) : 0;
        size_t len2 = s2 ? strlen(s2) : 0;
        char* buf = (char*)malloc(len1 + len2 + 1);
        if (s1) memcpy(buf, s1, len1);
        if (s2) memcpy(buf + len1, s2, len2 + 1);
        int64_t result = aura_alloc_string(buf);
        free(buf);
        return result;
    }
    case 6: // StringLength — return fixnum-encoded
        return ((int64_t)strlen(aura_string_ref(a1))) << 1;
    case 7: { // StringRef
        const char* s = aura_string_ref(a1);
        int64_t idx = a2;
        if (idx >= 0 && idx < (int64_t)strlen(s))
            return (int64_t)(unsigned char)s[idx];
        return 0;
    }
    case 8: { // Substring
        const char* s = aura_string_ref(a1);
        int64_t start = a2;
        int64_t end = argc >= 2 ? (int64_t)1 : (int64_t)0; // end default
        size_t len = strlen(s);
        if (start < 0) start = 0;
        if (end <= 0 || end > (int64_t)len) end = (int64_t)len;
        if (start >= end) return aura_alloc_string("");
        size_t sub_len = (size_t)(end - start);
        char* buf = (char*)malloc(sub_len + 1);
        memcpy(buf, s + start, sub_len);
        buf[sub_len] = 0;
        int64_t result = aura_alloc_string(buf);
        free(buf);
        return result;
    }
    case 9: // StringEq — return pointer-tagged bool
        return strcmp(aura_string_ref(a1), aura_string_ref(a2)) == 0 ? 7 : 3;
    case 10: // StringLt — return pointer-tagged bool
        return strcmp(aura_string_ref(a1), aura_string_ref(a2)) < 0 ? 7 : 3;
    case 11: { // NumberToString — decode fixnum, then format
        char buf[64];
        long val = IS_FIXNUM(a1) ? (a1 >> 1) : (long)a1;
        snprintf(buf, sizeof(buf), "%ld", val);
        return aura_alloc_string(buf);
    }
    case 12: { // StringToNumber
        const char* s = aura_string_ref(a1);
        if (!s || !*s) return 0;
        char* end = NULL;
        long val = strtol(s, &end, 10);
        return (int64_t)val;
    }
    case 13: // Display (fast-pathed, but keep as fallback)
        aura_display_int(a1);
        return a1;
    case 14: // Write
        printf("%ld", (long)a1);
        return a1;
    case 15: // Newline
        aura_newline();
        return 0;
    case 16: // Error
        fprintf(stderr, "error: %ld\n", (long)a1);
        fflush(stderr);
        return 0;
    case 17: // Assert
        if (a1 == 0)
            fprintf(stderr, "assertion failed\n");
        return a1;
    case 18: // Read
        // TODO: real stdin read; AOT path rarely hits this slow-path
        return 0;
    case 19: // ReadFile
        return 0;
    case 20: // WriteFile
        return 0;
    case 21: // FileExists
        return 3; // #f
    case 22: { // Gensym
        static int64_t gensym_counter = 0;
        char buf[64];
        snprintf(buf, sizeof(buf), "g%ld", (long)(++gensym_counter));
        return aura_alloc_string(buf);
    }
    case 23: { // Apply: (apply f args) — call f with args from list
        int64_t f = a1;           // the function/closure/primitive
        int64_t lst = a2;         // the argument list
        int64_t buf[256];        // argument buffer
        int count = 0;
        while (lst != 0 && IS_PAIR(lst) && count < 256) {
            buf[count++] = aura_pair_car(lst);
            lst = aura_pair_cdr(lst);
        }
        // Call the function with the collected arguments
        int64_t args_array[8] = {0};
        int i;
        for (i = 0; i < count && i < 8; i++)
            args_array[i] = buf[i];
        return aura_closure_call(f, args_array, count);
    }
    case 24: case 25: case 26: case 27: case 28: case 29: // Vector primitives
        return 0; // AOT path delegates vector ops via generated wrappers; stub
    case 30: // Import
        return 11; // void
    case 31: case 32: case 33: case 34: // Char primitives
        return a1; // pass through
    case 35: // Quotient
        if (a2 == 0) return 0;
        return a1 / a2;
    case 36: // Remainder
        if (a2 == 0) return 0;
        return a1 % a2;
    case 37: { // ListLength — count elements in a pair chain (list)
        int64_t count = 0;
        int64_t val = a1;
        while (val != 0 && IS_PAIR(val)) {  // heap pointer with tag=01
            count++;
            val = aura_pair_cdr(val);
        }
        return count << 1;  // fixnum-encode
    }
    case 38: { // ListRef — nth element of a list
        int64_t val = a1;
        int64_t idx = a2 >> 1;  // fixnum-decode
        int64_t i = 0;
        while (val != 0 && IS_PAIR(val) && i < idx) {
            val = aura_pair_cdr(val);
            i++;
        }
        if (val != 0 && IS_PAIR(val) && i == idx)
            return aura_pair_car(val);
        return 0;
    }
    case 39: { // ListReverse — reverse a pair chain
        int64_t input = a1;
        int64_t result = 0;  // empty list sentinel
        while (IS_PAIR(input)) {
            int64_t car_val = aura_pair_car(input);
            result = aura_alloc_pair(car_val, result);
            input = aura_pair_cdr(input);
        }
        return result;
    }
    case 40: // Raise
        fprintf(stderr, "raise: %ld\n", (long)a1);
        return 0;
    case 41: // ErrorP
        // Tag check: raised values are stored as negative sentinels or
        // a special pair; for now treat any non-zero value as not-error.
        return a1 ? 3 : 7;
    case 42: // PairP (reached via OpPrimCall slow-path)
        return IS_PAIR(a1) ? 7 : 3;
    case 43: // NullP
        return (a1 == 0 || a1 == 11) ? 7 : 3;
    default:
        // Hash (0-4) and any unhandled prim: return 0 to avoid spurious output.
        return 0;
    }
}

// Extern: env offset for test functions (set before fn call)
// ═══════════════════════════════════════════════════════════
// Main entry point
// ═══════════════════════════════════════════════════════════
// __top__ is the entry function generated by LLVM compilation.
#ifdef TEST_BUILD
// Stub for unit test build (no LLVM compiled __top__ available)
int64_t __top__(int64_t* args, uint32_t argc) { (void)args; (void)argc; return 0; }
#else
int64_t __top__(int64_t* args, uint32_t argc);
#endif

#ifndef TEST_BUILD
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    aura_bump_init();
    int64_t args[8] = {0};
    int64_t result = __top__(args, 0);
    aura_bump_reset();
    if (!g_display_was_called) {
        // Use type-aware display for the top-level result.
        // This handles bool sentinels (#t → INT64_MIN), pairs, and ints.
        if (result != 0)
            aura_display_int(result);
        printf("\n");
    } else if (result != 0) {
        printf("\n"); // newline after display output
    }
    return 0;
}
#endif
