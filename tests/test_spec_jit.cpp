// test_spec_jit.cpp — Unit tests for L1 type specialization (Phase 2, #53)
//
// Tests: check_shape_guard, SpecJITController cache
//
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include "../src/compiler/spec_jit_controller.h"
#include "../src/compiler/shape.h"

using namespace aura::compiler::shape;

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

// ── Dummy AuraJIT for testing SpecJITController ──────────────
// Minimal mock that satisfies the AuraJIT reference.
static int dummy_compile_called = 0;

int main() {
    // ═══════════════════════════════════════════════════════════
    // Section 1: check_shape_guard
    // ═══════════════════════════════════════════════════════════

    // ── 1a: Null shape_map → always matches ──────────────────
    {
        std::int64_t args[] = {0, 3, 11};
        TEST("null shape_map always matches",
             check_shape_guard(args, 3, nullptr, 0));
    }

    // ── 1b: Dynamic (0) matches everything ───────────────────
    {
        std::int64_t args[] = {42, -1, 100};
        std::uint8_t map[] = {0, 0, 0};
        TEST("Dynamic (0) matches any value",
             check_shape_guard(args, 3, map, 3));
    }

    // ── 1c: Int guard (1) matches fixnums ────────────────────
    {
        std::int64_t args[] = {42 << 1, 0, (-100LL) << 1};  // all fixnums
        std::uint8_t map[] = {1, 1, 1};
        TEST("Int guard matches fixnums",
             check_shape_guard(args, 3, map, 3));
    }

    // ── 1d: Int guard rejects non-fixnums ────────────────────
    {
        std::int64_t args[] = {42 << 1, 7, 0};  // 7 is bool #t
        std::uint8_t map[] = {1, 1, 1};
        TEST("Int guard rejects bool (7)",
             !check_shape_guard(args, 3, map, 3));
    }

    // ── 1e: Bool guard (3) matches 3 and 7 ───────────────────
    {
        std::int64_t args[] = {3, 7, 3};
        std::uint8_t map[] = {3, 3, 3};
        TEST("Bool guard matches 3 and 7",
             check_shape_guard(args, 3, map, 3));
    }

    // ── 1f: Bool guard rejects fixnum ────────────────────────
    {
        std::int64_t args[] = {42 << 1};  // fixnum 42, not bool
        std::uint8_t map[] = {3};
        TEST("Bool guard rejects fixnum",
             !check_shape_guard(args, 1, map, 1));
    }

    // ── 1g: Void guard (5) matches 11 ────────────────────────
    {
        std::int64_t args[] = {11};
        std::uint8_t map[] = {5};
        TEST("Void guard matches 11",
             check_shape_guard(args, 1, map, 1));
    }

    // ── 1h: Void guard rejects non-void ──────────────────────
    {
        std::int64_t args[] = {0};
        std::uint8_t map[] = {5};
        TEST("Void guard rejects 0",
             !check_shape_guard(args, 1, map, 1));
    }

    // ── 1i: Mixed shape map ──────────────────────────────────
    {
        // arg0=Int, arg1=Bool, arg2=Dynamic
        std::int64_t args[] = {42 << 1, 7, 0xDEADBEEF};
        std::uint8_t map[] = {1, 3, 0};
        TEST("Mixed shape map (Int, Bool, Dynamic)",
             check_shape_guard(args, 3, map, 3));
    }

    // ── 1j: Map shorter than arg count ───────────────────────
    {
        std::int64_t args[] = {42 << 1, 0, 999};
        std::uint8_t map[] = {1, 1};  // only checks first 2 args
        TEST("Map shorter than args (checks prefix)",
             check_shape_guard(args, 3, map, 2));
    }

    // ── 1k: Map longer than arg count ───────────────────────
    {
        std::int64_t args[] = {42 << 1};
        std::uint8_t map[] = {1, 1, 1};  // map has 3, args has 1
        TEST("Map longer than args (min length used)",
             check_shape_guard(args, 1, map, 3));
    }

    // ── 1l: Float guard (2) ────────────────────────────────
    {
        // Float encoded values are <= FLOAT_BIAS && > STRING_BIAS
        std::int64_t args[] = {-10000000000000000LL};  // FLOAT_BIAS
        std::uint8_t map[] = {2};
        TEST("Float guard matches float-encoded value",
             check_shape_guard(args, 1, map, 1));
    }

    // ── 1m: Float guard rejects fixnum ──────────────────────
    {
        std::int64_t args[] = {42 << 1};  // fixnum
        std::uint8_t map[] = {2};
        TEST("Float guard rejects fixnum",
             !check_shape_guard(args, 1, map, 1));
    }

    // ═══════════════════════════════════════════════════════════
    // Summary
    // ═══════════════════════════════════════════════════════════
    std::fprintf(stdout, "\n=== Results: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
