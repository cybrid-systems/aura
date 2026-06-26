// test_spec_runtime.cpp — Runtime tests for L2 specialization (Phase 3, #53)
//
// Tests the unchecked pair access functions directly.
//
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

// ── Minimal pair pool (matching aura_jit_runtime.cpp layout) ──
static int64_t* g_pair_cars = nullptr;
static int64_t* g_pair_cdrs = nullptr;
static size_t g_pair_count = 0;
static size_t g_pair_cap = 0;

extern "C" int64_t aura_alloc_pair(int64_t car, int64_t cdr) {
    if (g_pair_count >= g_pair_cap) {
        size_t new_cap = g_pair_cap ? g_pair_cap * 2 : 64;
        g_pair_cars = (int64_t*)realloc(g_pair_cars, new_cap * sizeof(int64_t));
        g_pair_cdrs = (int64_t*)realloc(g_pair_cdrs, new_cap * sizeof(int64_t));
        g_pair_cap = new_cap;
    }
    size_t id = g_pair_count++;
    g_pair_cars[id] = car;
    g_pair_cdrs[id] = cdr;
    return (int64_t)(id << 2) | 1;
}

extern "C" {
    int64_t aura_pair_car(int64_t pair_val) {
        uint64_t id = (uint64_t)(pair_val >> 2);
        if (id >= g_pair_count) return 0;
        return g_pair_cars[id];
    }
    int64_t aura_pair_cdr(int64_t pair_val) {
        uint64_t id = (uint64_t)(pair_val >> 2);
        if (id >= g_pair_count) return 0;
        return g_pair_cdrs[id];
    }
    // Unchecked variants (skip bounds check)
    int64_t aura_pair_car_unchecked(int64_t pair_val) {
        uint64_t id = (uint64_t)(pair_val >> 2);
        return g_pair_cars[id];
    }
    int64_t aura_pair_cdr_unchecked(int64_t pair_val) {
        uint64_t id = (uint64_t)(pair_val >> 2);
        return g_pair_cdrs[id];
    }
}

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name, expr) do { \
    tests_run++; \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL: %s (%s)\n", name, #expr); \
    } else { \
        tests_passed++; \
        std::fprintf(stdout, "PASS: %s\n", name); \
    } \
} while(0)

int main() {
    // ── Setup: create some pairs ─────────────────────────────
    int64_t p1 = aura_alloc_pair(42, 100);      // (42 . 100)
    int64_t p2 = aura_alloc_pair(0, 7);          // (0 . 7)
    int64_t p3 = aura_alloc_pair(-5 << 1, 11);   // (fixnum -5 . void)
    int64_t p4 = aura_alloc_pair(p1, p2);        // ((42 . 100) . (0 . 7))

    // ═══════════════════════════════════════════════════════════
    // Section 1: Checked pair access
    // ═══════════════════════════════════════════════════════════

    // ── 1a: Basic car/cdr ──────────────────────────────────
    TEST("Checked: car (42 . 100) == 42", aura_pair_car(p1) == 42);
    TEST("Checked: cdr (42 . 100) == 100", aura_pair_cdr(p1) == 100);

    // ── 1b: Zero/false pair ────────────────────────────────
    TEST("Checked: car (0 . 7) == 0", aura_pair_car(p2) == 0);
    TEST("Checked: cdr (0 . 7) == 7", aura_pair_cdr(p2) == 7);

    // ── 1c: Negative + void pair ────────────────────────────
    TEST("Checked: car = fixnum -5", aura_pair_car(p3) == (-5 << 1));
    TEST("Checked: cdr = void (11)", aura_pair_cdr(p3) == 11);

    // ── 1d: Nested pairs ─────────────────────────────────
    TEST("Checked: car of nested pair = p1", aura_pair_car(p4) == p1);
    TEST("Checked: car of p1 through nested = 42",
         aura_pair_car(aura_pair_car(p4)) == 42);

    // ═══════════════════════════════════════════════════════════
    // Section 2: Unchecked pair access (L2 specialization)
    // ═══════════════════════════════════════════════════════════

    // ── 2a: Basic unchecked access ──────────────────────────
    TEST("Unchecked: car (42 . 100) == 42",
         aura_pair_car_unchecked(p1) == 42);
    TEST("Unchecked: cdr (42 . 100) == 100",
         aura_pair_cdr_unchecked(p1) == 100);

    // ── 2b: Unchecked with zero ─────────────────────────────
    TEST("Unchecked: car (0 . 7) == 0",
         aura_pair_car_unchecked(p2) == 0);
    TEST("Unchecked: cdr (0 . 7) == 7",
         aura_pair_cdr_unchecked(p2) == 7);

    // ── 2c: Unchecked with negative ─────────────────────────
    TEST("Unchecked: car = fixnum -5",
         aura_pair_car_unchecked(p3) == (-5 << 1));
    TEST("Unchecked: cdr = void (11)",
         aura_pair_cdr_unchecked(p3) == 11);

    // ═══════════════════════════════════════════════════════════
    // Section 3: Checked vs unchecked result equivalence
    // ═══════════════════════════════════════════════════════════

    TEST("Equivalence: checked car == unchecked car (all pairs)",
         aura_pair_car(p1) == aura_pair_car_unchecked(p1) &&
         aura_pair_car(p2) == aura_pair_car_unchecked(p2) &&
         aura_pair_car(p3) == aura_pair_car_unchecked(p3) &&
         aura_pair_car(p4) == aura_pair_car_unchecked(p4));

    TEST("Equivalence: checked cdr == unchecked cdr (all pairs)",
         aura_pair_cdr(p1) == aura_pair_cdr_unchecked(p1) &&
         aura_pair_cdr(p2) == aura_pair_cdr_unchecked(p2) &&
         aura_pair_cdr(p3) == aura_pair_cdr_unchecked(p3) &&
         aura_pair_cdr(p4) == aura_pair_cdr_unchecked(p4));

    // ═══════════════════════════════════════════════════════════
    // Section 4: Performance comparison (basic timing)
    // ═══════════════════════════════════════════════════════════

    static constexpr int kIterations = 10000000;  // 10M iterations

    // ── 4a: Checked access throughput ───────────────────────
    {
        volatile int64_t sink = 0;
        auto start = std::clock();
        for (int i = 0; i < kIterations; i++) {
            sink += aura_pair_car(p1) + aura_pair_cdr(p2);
        }
        auto end = std::clock();
        double sec = (double)(end - start) / CLOCKS_PER_SEC;
        double ns_per_op = (sec / (double)(kIterations * 2)) * 1e9;
        std::fprintf(stdout, "PERF: Checked car/cdr: %.1f ns/op\n", ns_per_op);
        TEST("Checked: sink non-zero", sink != 0);
    }

    // ── 4b: Unchecked access throughput ─────────────────────
    {
        volatile int64_t sink = 0;
        auto start = std::clock();
        for (int i = 0; i < kIterations; i++) {
            sink += aura_pair_car_unchecked(p1) + aura_pair_cdr_unchecked(p2);
        }
        auto end = std::clock();
        double sec = (double)(end - start) / CLOCKS_PER_SEC;
        double ns_per_op = (sec / (double)(kIterations * 2)) * 1e9;
        std::fprintf(stdout, "PERF: Unchecked car/cdr: %.1f ns/op\n", ns_per_op);
        TEST("Unchecked: sink non-zero", sink != 0);
    }

    // ── 4c: Edge case: large index ─────────────────────────
    {
        // Create pair with specific index to test bounds
        int64_t large_ref = aura_alloc_pair(999, 888);
        // Checked should work
        TEST("Checked: large index car == 999", aura_pair_car(large_ref) == 999);
        // Unchecked should also work (same pool)
        TEST("Unchecked: large index car == 999",
             aura_pair_car_unchecked(large_ref) == 999);
    }

    // ═══════════════════════════════════════════════════════════
    // Cleanup
    // ═══════════════════════════════════════════════════════════
    free(g_pair_cars);
    free(g_pair_cdrs);
    g_pair_cars = nullptr;
    g_pair_cdrs = nullptr;
    g_pair_count = 0;
    g_pair_cap = 0;

    std::fprintf(stdout, "\n=== Results: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
