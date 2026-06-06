// runtime.c unit test harness
// Compile: gcc -g tests/runtime_test_harness.c lib/runtime.c -o /tmp/runtime_test -lm
// Run:     /tmp/runtime_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <math.h>

// Runtime functions declared in runtime.c
extern void aura_bump_init(void);
extern void* aura_bump_alloc(size_t size, size_t align);
extern void aura_bump_reset(void);
extern int64_t aura_alloc_pair(int64_t car, int64_t cdr);
extern int64_t aura_pair_car(int64_t pair_id);
extern int64_t aura_pair_cdr(int64_t pair_id);
extern void aura_drop_pair(int64_t pair_id);
extern int64_t aura_new_cell(void);
extern int64_t aura_cell_get(int64_t cell_id);
extern void aura_cell_set(int64_t cell_id, int64_t val);
extern void aura_drop_cell(int64_t cell_id);
extern int64_t aura_alloc_closure(int64_t func_id);
extern void aura_register_closure_fn(int64_t closure_id, int64_t fn_ptr, int32_t local_count);
extern void aura_closure_capture(int64_t closure_id, int64_t idx, int64_t val);
extern int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc);
extern void aura_drop_closure(int64_t closure_id);
extern int64_t aura_alloc_string(const char* s);
extern const char* aura_string_ref(int64_t str_id);

static int passed = 0, failed = 0;

#define TEST(name) void name()
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  ❌ %s: %s\n", __func__, msg); \
        failed++; \
        return; \
    } \
} while(0)
#define PASS() do { \
    printf("  ✅ %s\n", __func__); \
    passed++; \
} while(0)

// ═══════════════════════════════════════════════════════════
// Bump Allocator tests
// ═══════════════════════════════════════════════════════════

TEST(test_bump_basic) {
    aura_bump_init();
    void* p1 = aura_bump_alloc(64, 8);
    void* p2 = aura_bump_alloc(64, 8);
    CHECK(p1 != NULL, "p1 alloc");
    CHECK(p2 != NULL, "p2 alloc");
    CHECK((char*)p2 - (char*)p1 == 64, "contiguous");
    aura_bump_reset();
    void* p3 = aura_bump_alloc(64, 8);
    CHECK(p3 == p1, "reset reuse");
    PASS();
}

TEST(test_bump_alignment) {
    aura_bump_init();
    void* p4 = aura_bump_alloc(1, 4);
    CHECK(((uintptr_t)p4 & 3) == 0, "4-byte align");
    void* p8 = aura_bump_alloc(1, 8);
    CHECK(((uintptr_t)p8 & 7) == 0, "8-byte align");
    void* p16 = aura_bump_alloc(1, 16);
    CHECK(((uintptr_t)p16 & 15) == 0, "16-byte align");
    aura_bump_reset();
    PASS();
}

TEST(test_bump_multi_reset) {
    for (int i = 0; i < 10; i++) {
        aura_bump_init();
        for (int j = 0; j < 100; j++) {
            void* p = aura_bump_alloc(64, 8);
            CHECK(p != NULL, "multi alloc");
        }
        aura_bump_reset();
    }
    PASS();
}

TEST(test_bump_overflow) {
    // Allocate more than the default 64MB arena to trigger realloc
    aura_bump_init();
    // 64KB chunks — 2000 of them = 128MB total
    for (int i = 0; i < 2000; i++) {
        void* p = aura_bump_alloc(65536, 8);
        CHECK(p != NULL, "large alloc");
    }
    aura_bump_reset();
    PASS();
}

// ═══════════════════════════════════════════════════════════
// Pair tests
// ═══════════════════════════════════════════════════════════

TEST(test_pair_basic) {
    int64_t p1 = aura_alloc_pair(10, 20);
    int64_t p2 = aura_alloc_pair(30, 40);
    CHECK(aura_pair_car(p1) == 10, "car");
    CHECK(aura_pair_cdr(p1) == 20, "cdr");
    CHECK(aura_pair_car(p2) == 30, "car2");
    CHECK(aura_pair_cdr(p2) == 40, "cdr2");
    PASS();
}

TEST(test_pair_drop_reuse) {
    int64_t p1 = aura_alloc_pair(10, 20);
    int64_t p2 = aura_alloc_pair(30, 40);
    aura_drop_pair(p1);
    int64_t p3 = aura_alloc_pair(50, 60);
    CHECK(p3 == p1, "slot reuse");
    CHECK(aura_pair_car(p3) == 50, "reused car");
    (void)p2;
    PASS();
}

TEST(test_pair_double_drop) {
    int64_t p = aura_alloc_pair(1, 2);
    aura_drop_pair(p);
    aura_drop_pair(p); // second drop should be no-op
    // After drop, car/cdr should return 0
    CHECK(aura_pair_car(p) == 0, "post-drop car");
    PASS();
}

TEST(test_pair_oob) {
    // Out-of-bounds access should return 0, not crash
    CHECK(aura_pair_car(999999) == 0, "oob car");
    CHECK(aura_pair_cdr(999999) == 0, "oob cdr");
    PASS();
}

// Issue #106: recursive drop. A pair whose car or cdr is itself
// a pair should, when dropped, also drop the nested pair. We
// verify by keeping the inner id, dropping the outer, and
// checking that reads of the inner return 0 (dead). Without
// the recursion the inner would still report its car/cdr.
TEST(test_pair_nested_drop) {
    int64_t inner = aura_alloc_pair(10, 20);
    int64_t outer = aura_alloc_pair(inner, 99);
    aura_drop_pair(outer);
    // The inner pair should be dead now (recursive drop).
    CHECK(aura_pair_car(inner) == 0, "inner car after outer drop");
    CHECK(aura_pair_cdr(inner) == 0, "inner cdr after outer drop");
    PASS();
}

TEST(test_pair_doubly_nested_drop) {
    int64_t leaf = aura_alloc_pair(1, 2);
    int64_t mid = aura_alloc_pair(leaf, 3);
    int64_t top = aura_alloc_pair(mid, 4);
    aura_drop_pair(top);
    CHECK(aura_pair_car(leaf) == 0, "leaf dead");
    CHECK(aura_pair_car(mid) == 0, "mid dead");
    CHECK(aura_pair_car(top) == 0, "top dead");
    PASS();
}

TEST(test_pair_drop_fixes_non_pair) {
    // Outer has fixnum car and cdr — drop should be a no-op
    // for the car/cdr (no recursion needed). We just check
    // the outer is dead.
    int64_t p = aura_alloc_pair(42, 100);
    aura_drop_pair(p);
    CHECK(aura_pair_car(p) == 0, "car after drop");
    CHECK(aura_pair_cdr(p) == 0, "cdr after drop");
    PASS();
}

TEST(test_pair_large_count) {
    // Allocate and drop many pairs in sequence
    for (int i = 0; i < 10000; i++) {
        int64_t p = aura_alloc_pair(i, i * 2);
        CHECK(aura_pair_car(p) == i, "large seq car");
        aura_drop_pair(p);
    }
    PASS();
}

// ═══════════════════════════════════════════════════════════
// Cell tests
// ═══════════════════════════════════════════════════════════

TEST(test_cell_basic) {
    int64_t c1 = aura_new_cell();
    int64_t c2 = aura_new_cell();
    aura_cell_set(c1, 42);
    aura_cell_set(c2, 99);
    CHECK(aura_cell_get(c1) == 42, "cell1");
    CHECK(aura_cell_get(c2) == 99, "cell2");
    PASS();
}

TEST(test_cell_drop_reuse) {
    int64_t c1 = aura_new_cell();
    aura_cell_set(c1, 42);
    aura_drop_cell(c1);
    int64_t c2 = aura_new_cell();
    CHECK(c2 == c1, "cell reuse");
    aura_cell_set(c2, 100);
    CHECK(aura_cell_get(c2) == 100, "reused cell");
    PASS();
}

TEST(test_cell_double_drop) {
    int64_t c = aura_new_cell();
    aura_cell_set(c, 42);
    aura_drop_cell(c);
    aura_drop_cell(c); // no-op
    CHECK(aura_cell_get(c) == 0, "post-drop get");
    PASS();
}

TEST(test_cell_oob) {
    CHECK(aura_cell_get(999999) == 0, "oob get");
    aura_cell_set(999999, 42); // should be no-op, not crash
    PASS();
}

// ═══════════════════════════════════════════════════════════
// Closure tests
// ═══════════════════════════════════════════════════════════

static int64_t test_add_fn(int64_t* locals, uint32_t argc);
static int64_t test_mul_fn(int64_t* locals, uint32_t argc);

// Static offset for env (set by aura_closure_call before calling fn)
extern int g_env_offset;

int64_t test_add_fn(int64_t* locals, uint32_t argc) {
    (void)argc;
    int off = g_env_offset;
    return locals[off] + locals[off + 1];
}

int64_t test_mul_fn(int64_t* locals, uint32_t argc) {
    (void)argc;
    int off = g_env_offset;
    return locals[off] * locals[off + 1];
}

TEST(test_closure_basic) {
    int64_t cid = aura_alloc_closure(0);
    aura_register_closure_fn(cid, (int64_t)(intptr_t)test_add_fn, 2);
    int64_t args[2] = {10, 20};
    int64_t result = aura_closure_call(cid, args, 2);
    CHECK(result == 30, "closure call");
    PASS();
}

TEST(test_closure_capture) {
    int64_t cid = aura_alloc_closure(1);
    aura_register_closure_fn(cid, (int64_t)(intptr_t)test_add_fn, 2);
    aura_closure_capture(cid, 0, 42);
    int64_t args[2] = {10, 20};
    int64_t result = aura_closure_call(cid, args, 2);
    CHECK(result == 30, "capture + call");
    PASS();
}

TEST(test_closure_drop) {
    int64_t cid = aura_alloc_closure(0);
    aura_register_closure_fn(cid, (int64_t)(intptr_t)test_add_fn, 2);
    aura_drop_closure(cid);
    int64_t args[2] = {1, 2};
    int64_t result = aura_closure_call(cid, args, 2);
    CHECK(result == 0, "dropped closure returns 0");
    PASS();
}

// Issue #106 sub-task 3: closure env with a captured pair. When
// the closure is dropped, the env-slot pair should also be
// dropped (the captured pair is no longer reachable through the
// closure). We verify by reading the pair's car / cdr after
// the closure drop — they return 0 for a dead pair.
TEST(test_closure_env_pair_drop) {
    int64_t cid = aura_alloc_closure(0);
    aura_register_closure_fn(cid, (int64_t)(intptr_t)test_add_fn, 2);
    int64_t captured = aura_alloc_pair(100, 200);
    aura_closure_capture(cid, 0, captured);
    aura_drop_closure(cid);
    // The pair should be dead now (recursive drop). Car/cdr
    // return 0 for dead pairs.
    CHECK(aura_pair_car(captured) == 0, "captured pair car after closure drop");
    CHECK(aura_pair_cdr(captured) == 0, "captured pair cdr after closure drop");
    PASS();
}

// Issue #106 sub-task 3: closure env with a captured cell. The
// runtime detects cells by checking if the env value falls in
// the cell-id range and the slot is live. We allocate a cell
// first to push the id into a non-zero range, then capture.
TEST(test_closure_env_cell_drop) {
    int64_t sentinel = aura_new_cell();  // advance cell_count to 1
    int64_t captured_cell = aura_new_cell();
    aura_cell_set(captured_cell, 42);
    int64_t cid = aura_alloc_closure(0);
    aura_register_closure_fn(cid, (int64_t)(intptr_t)test_add_fn, 2);
    aura_closure_capture(cid, 0, captured_cell);
    aura_drop_closure(cid);
    // Captured cell should be dead after closure drop.
    CHECK(aura_cell_get(captured_cell) == 0, "captured cell after closure drop");
    PASS();
}

// Issue #106 sub-task 3: closure env with multiple captured
// pairs (nested recursion). Dropping the outer closure should
// drop all the pairs in its env in one shot.
TEST(test_closure_env_multi_pair_drop) {
    int64_t cid = aura_alloc_closure(0);
    aura_register_closure_fn(cid, (int64_t)(intptr_t)test_add_fn, 4);
    int64_t p1 = aura_alloc_pair(1, 2);
    int64_t p2 = aura_alloc_pair(3, 4);
    int64_t p3 = aura_alloc_pair(5, 6);
    aura_closure_capture(cid, 0, p1);
    aura_closure_capture(cid, 1, p2);
    aura_closure_capture(cid, 2, p3);
    aura_drop_closure(cid);
    CHECK(aura_pair_car(p1) == 0, "p1 dead");
    CHECK(aura_pair_car(p2) == 0, "p2 dead");
    CHECK(aura_pair_car(p3) == 0, "p3 dead");
    PASS();
}

// Issue #106 sub-task 3: fixnum env slot is a no-op. A
// fixnum that happens to be in the cell-id range but on an
// unallocated slot must NOT trigger a drop. The live check
// catches this — if cell_count > 5 but cell id 5 was never
// allocated, cell_heap[5].live is false and the drop is
// skipped.
TEST(test_closure_env_fixnum_noop) {
    // Allocate a closure whose env has a fixnum value. The
    // closure's local_count must be > 0 to allow captures.
    int64_t cid = aura_alloc_closure(0);
    aura_register_closure_fn(cid, (int64_t)(intptr_t)test_add_fn, 2);
    // env[0] = fixnum 42 (low bit 0, not a pair tag).
    aura_closure_capture(cid, 0, 42);
    // Drop the closure. env[0] is 42, which is in range of
    // cell_count / closure_count after they grow, but the
    // slot is not live so the heuristic should no-op.
    aura_drop_closure(cid);
    PASS();
}

TEST(test_closure_oob) {
    int64_t args[2] = {0, 0};
    int64_t result = aura_closure_call(999999, args, 2);
    CHECK(result == 0, "oob call returns 0");
    PASS();
}

TEST(test_closure_multi_capture) {
    int64_t cid = aura_alloc_closure(0);
    aura_register_closure_fn(cid, (int64_t)(intptr_t)test_mul_fn, 2);
    for (int i = 0; i < 8; i++)
        aura_closure_capture(cid, i, i * 10);
    // Call should still work
    int64_t args[2] = {2, 3};
    int64_t result = aura_closure_call(cid, args, 2);
    CHECK(result == 6, "multi-capture call");
    PASS();
}

// ═══════════════════════════════════════════════════════════
// String tests
// ═══════════════════════════════════════════════════════════

TEST(test_string_basic) {
    int64_t s1 = aura_alloc_string("hello");
    int64_t s2 = aura_alloc_string("world");
    CHECK(strcmp(aura_string_ref(s1), "hello") == 0, "str1");
    CHECK(strcmp(aura_string_ref(s2), "world") == 0, "str2");
    PASS();
}

TEST(test_string_empty) {
    int64_t s = aura_alloc_string("");
    CHECK(strcmp(aura_string_ref(s), "") == 0, "empty");
    PASS();
}

TEST(test_string_oob) {
    const char* r = aura_string_ref(999999);
    CHECK(strcmp(r, "") == 0, "oob returns empty");
    PASS();
}

TEST(test_string_large) {
    // Create a ~100KB string
    int size = 100000;
    char* buf = (char*)malloc(size);
    memset(buf, 'A', size - 1);
    buf[size - 1] = '\0';
    int64_t s = aura_alloc_string(buf);
    const char* r = aura_string_ref(s);
    CHECK(strlen(r) == (size_t)(size - 1), "large len");
    CHECK(r[0] == 'A', "large first char");
    CHECK(r[size - 2] == 'A', "large last char");
    free(buf);
    PASS();
}

// ═══════════════════════════════════════════════════════════
// Mixed scenario tests
// ═══════════════════════════════════════════════════════════

TEST(test_bump_and_drop_mixed) {
    aura_bump_init();
    void* buf = aura_bump_alloc(256, 8);
    CHECK(buf != NULL, "bump alloc");
    int64_t p1 = aura_alloc_pair(1, 2);
    int64_t p2 = aura_alloc_pair(3, 4);
    aura_drop_pair(p1);
    aura_bump_reset();
    // After reset, bump memory is gone but pair memory survives
    CHECK(aura_pair_car(p2) == 3, "pair survives reset");
    PASS();
}

// ═══════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════

typedef void (*test_fn)(void);
static test_fn tests[] = {
    // Bump
    test_bump_basic,
    test_bump_alignment,
    test_bump_multi_reset,
    test_bump_overflow,
    // Pair
    test_pair_basic,
    test_pair_drop_reuse,
    test_pair_double_drop,
    test_pair_oob,
    test_pair_nested_drop,
    test_pair_doubly_nested_drop,
    test_pair_drop_fixes_non_pair,
    test_pair_large_count,
    // Cell
    test_cell_basic,
    test_cell_drop_reuse,
    test_cell_double_drop,
    test_cell_oob,
    // Closure
    test_closure_basic,
    test_closure_capture,
    test_closure_drop,
    test_closure_env_pair_drop,
    test_closure_env_cell_drop,
    test_closure_env_multi_pair_drop,
    test_closure_env_fixnum_noop,
    test_closure_oob,
    test_closure_multi_capture,
    // String
    test_string_basic,
    test_string_empty,
    test_string_oob,
    test_string_large,
    // Mixed
    test_bump_and_drop_mixed,
};

int main(void) {
    printf("=== runtime.c unit tests ===\n");
    int total = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < total; i++)
        tests[i]();
    printf("\n  %d/%d passed", passed, total);
    if (failed > 0) printf(", %d failed", failed);
    printf("\n");
    return failed > 0 ? 1 : 0;
}
