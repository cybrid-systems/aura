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

// Negative encoding: aura_alloc_pair returns -(id+1) instead of raw id.
// This matches the JIT runtime convention (aura_jit_runtime.cpp) and allows
// pair? type checks via simple val < 0 comparison in the LLVM IR.
//
// Decode: internal_id = -pair_val - 1
//
// The negative encoding also distinguishes pairs from closures and cells
// (which use non-negative IDs), enabling lightweight type dispatch.

int64_t aura_alloc_pair(int64_t car, int64_t cdr) {
    get_pairs();
    uint64_t id = pair_alloc_slot();
    pairs[id].car = car;
    pairs[id].cdr = cdr;
    return -(int64_t)id - 1;
}

int64_t aura_pair_car(int64_t pair_val) {
    int64_t id = -pair_val - 1; // decode from negative sentinel
    if (id < 0 || (uint64_t)id >= pair_count || !pairs[(uint64_t)id].live) return 0;
    return pairs[(uint64_t)id].car;
}

int64_t aura_pair_cdr(int64_t pair_val) {
    int64_t id = -pair_val - 1;
    if (id < 0 || (uint64_t)id >= pair_count || !pairs[(uint64_t)id].live) return 0;
    return pairs[(uint64_t)id].cdr;
}

void aura_drop_pair(int64_t pair_val) {
    int64_t id = -pair_val - 1;
    if (id < 0 || (uint64_t)id >= pair_count || !pairs[(uint64_t)id].live) return;
    pairs[(uint64_t)id].live = false;
    if (pair_free_count < FREE_LIST_CAP)
        pair_free_list[pair_free_count++] = (int64_t)id;
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

int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc) {
    // AOT primitive dispatch: negative closure_id means this is an
    // evaluator primitive loaded via OpPrimitive (AOT mode).
    // Encoding: closure_id = -(prim_slot + 1)
    // So prim_slot = -closure_id - 1
    if (closure_id < 0) {
        int64_t prim_slot = -closure_id - 1;
        if (prim_slot >= 0 && prim_slot < MAX_PRIM_SLOTS && s_prim_fns[(uint64_t)prim_slot]) {
            return s_prim_fns[(uint64_t)prim_slot](args, (int32_t)argc);
        }
        // No fallback — if no primitive was registered for this slot,
        // return 0 (passthrough). The registered function was generated
        // by the compiler at --emit-binary time.
        return 0;
    }
    
    // Normal closure dispatch
    uint64_t id = (uint64_t)closure_id;
    if (id >= closure_count || !closure_heap[id].live || !closure_heap[id].fn) {
        // Don't print error for AOT primitive fallback
        return 0;
    }
    return closure_heap[id].fn(args, (uint32_t)argc);
}

void aura_drop_closure(int64_t closure_id) {
    uint64_t id = (uint64_t)closure_id;
    if (id >= closure_count || !closure_heap[id].live) return;
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
    return (int64_t)id;
}

const char* aura_string_ref(int64_t str_id) {
    uint64_t id = (uint64_t)str_id;
    if (id >= string_count) return "";
    return string_heap[id] ? string_heap[id] : "";
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
void aura_register_primitive_fn(int64_t slot, int64_t fn_ptr) {
    uint64_t s = (uint64_t)slot;
    if (s < MAX_PRIM_SLOTS && fn_ptr != 0)
        s_prim_fns[s] = (PrimFn)(intptr_t)fn_ptr;
}

// Update aura_closure_call to dispatch primitives when called with
// a negative closure ID from OpPrimitive (AOT mode).
// Negative encoding: closure_id = -(prim_slot + 1)
// So prim_slot = -closure_id - 1

// ═══════════════════════════════════════════════════════════
// I/O primitives
// ═══════════════════════════════════════════════════════════

int64_t aura_display_int(int64_t val) {
    printf("%ld", (long)val);
    fflush(stdout);
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
// In Aura's tag scheme: positive values are pairs (pair_id + 1, negated)
// Actually pairs use negative encoding: -(pair_id+1)
static int is_pair_val(int64_t v) {
    return v < 0 && v > -65536;
}

int64_t aura_prim_call(int64_t prim_id, int64_t a1, int64_t a2, int64_t argc) {
    // Primitive IDs from PrimId enum in ir.ixx
    // Display=8, Write=9, Newline=10, Quotient=30, Remainder=31
    // Other IDs: 0=StringAppend, 1=StringLength, ...
    // These may shift — the key primitives for AOT are handled inline
    // in the LLVM IR (display, quotient, remainder are fast-pathed).
    // This function is a fallback for unknown primitives.
    (void)argc;
    
    switch (prim_id) {
    case 8:  // Display
        aura_display_int(a1);
        return a1;
    case 9:  // Write  
        printf("%ld", (long)a1);
        return a1;
    case 10: // Newline
        aura_newline();
        return 0;
    case 30: // Quotient
        if (a2 == 0) return 0;
        return a1 / a2;
    case 31: // Remainder
        if (a2 == 0) return 0;
        return a1 % a2;
    default:
        // Unknown primitive (pair?, null?, list, etc.) — fallback
        // These are not yet handled in the AOT runtime; return a1 as passthrough.
        return (argc > 0) ? a1 : 0;
    }
}

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
    if (result != 0)
        printf("%ld\n", (long)result);
    return 0;
}
#endif
