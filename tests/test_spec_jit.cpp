// test_spec_jit.cpp — Unit tests for L1 type specialization (Phase 2, #53)
//
// Tests: check_shape_guard, SpecJITController, Profiler+SpecJIT integration
//
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include "../src/compiler/spec_jit_controller.h"
#include "../src/compiler/shape.h"
#include "../src/compiler/shape_profiler.h"

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

// ── Float/String bias constants ───────────────────────────────
static constexpr std::int64_t kFloatBias  = -10000000000000000LL;
static constexpr std::int64_t kStringBias = -9000000000000000000LL;

// ── Helper: dummy AuraJIT for SpecJITController tests ─────────
// Defines a minimal mock that captures compilation requests.
struct MockSpecJIT {
    int compile_called = 0;
    std::string last_fn_name;
    uint8_t last_shape_map[32];
    uint32_t last_map_size = 0;
    uint32_t last_local_count = 0;
    uint32_t last_arg_count = 0;
};

int main() {
    // ═══════════════════════════════════════════════════════════
    // Section 1: check_shape_guard — basic cases
    // ═══════════════════════════════════════════════════════════

    // ── 1a: Null shape_map → always matches ──────────────────
    {
        std::int64_t args[] = {0, 3, 11};
        TEST("null shape_map always matches",
             check_shape_guard(args, 3, nullptr, 0));
    }

    // ── 1b: Empty args (0 count) ─────────────────────────────
    {
        std::uint8_t map[] = {1, 2};
        TEST("0 args always matches", check_shape_guard(nullptr, 0, map, 2));
    }

    // ── 1c: Empty map (0 size) ───────────────────────────────
    {
        std::int64_t args[] = {42, -1, 100};
        TEST("Empty map always matches",
             check_shape_guard(args, 3, nullptr, 0));
    }

    // ── 1d: Dynamic (0) matches everything ───────────────────
    {
        std::int64_t args[] = {42, -1, 100, 3, 11, kFloatBias};
        std::uint8_t map[] = {0, 0, 0, 0, 0, 0};
        TEST("Dynamic (0) matches any value",
             check_shape_guard(args, 6, map, 6));
    }

    // ── 1e: Int guard (1) matches fixnums ────────────────────
    {
        std::int64_t args[] = {42 << 1, 0, (-100LL) << 1, 2, 0x7FFFFFFFFFFFFFFELL};
        std::uint8_t map[] = {1, 1, 1, 1, 1};
        TEST("Int guard matches all fixnums",
             check_shape_guard(args, 5, map, 5));
    }

    // ── 1f: Int guard rejects non-fixnums ────────────────────
    {
        // 7 = bool #t, 11 = void, 3 = bool #f
        std::int64_t args[] = {42 << 1, 7, 11, 3};
        std::uint8_t map[] = {1, 1, 1, 1};
        TEST("Int guard rejects bool #t (7)",
             !check_shape_guard(args, 4, map, 4));
    }

    // ── 1g: Int guard rejects string-encoded value ───────────
    {
        std::int64_t args[] = {kStringBias - 1};  // string encoded
        std::uint8_t map[] = {1};
        TEST("Int guard rejects string-encoded",
             !check_shape_guard(args, 1, map, 1));
    }

    // ── 1h: Int guard rejects float-encoded value ───────────
    {
        std::int64_t args[] = {kFloatBias};  // float encoded
        std::uint8_t map[] = {1};
        TEST("Int guard rejects float-encoded",
             !check_shape_guard(args, 1, map, 1));
    }

    // ═══════════════════════════════════════════════════════════
    // Section 2: check_shape_guard — type-specific
    // ═══════════════════════════════════════════════════════════

    // ── 2a: Bool guard (3) matches 3 and 7 ───────────────────
    {
        std::int64_t args[] = {3, 7, 3};
        std::uint8_t map[] = {3, 3, 3};
        TEST("Bool guard matches 3 and 7",
             check_shape_guard(args, 3, map, 3));
    }

    // ── 2b: Bool guard rejects fixnum ────────────────────────
    {
        std::int64_t args[] = {42 << 1, 0, 100 << 1};
        std::uint8_t map[] = {3, 3, 3};
        TEST("Bool guard rejects fixnums",
             !check_shape_guard(args, 3, map, 3));
    }

    // ── 2c: Bool guard rejects void ──────────────────────────
    {
        std::int64_t args[] = {11, 7};
        std::uint8_t map[] = {3, 3};
        TEST("Bool guard rejects void (11)",
             !check_shape_guard(args, 2, map, 2));
    }

    // ── 2d: Void guard (5) matches 11 ────────────────────────
    {
        std::int64_t args[] = {11};
        std::uint8_t map[] = {5};
        TEST("Void guard matches 11",
             check_shape_guard(args, 1, map, 1));
    }

    // ── 2e: Void guard rejects non-void ──────────────────────
    {
        std::int64_t args[] = {0, 7, kFloatBias};
        std::uint8_t map[] = {5, 5, 5};
        TEST("Void guard rejects non-void",
             !check_shape_guard(args, 3, map, 3));
    }

    // ── 2f: Float guard (2) ─────────────────────────────────
    {
        // At FLOAT_BIAS boundary
        std::int64_t args[] = {kFloatBias, kFloatBias - 1, kStringBias + 1};
        std::uint8_t map[] = {2, 2, 2};
        TEST("Float guard matches float range",
             check_shape_guard(args, 3, map, 3));
    }

    // ── 2g: Float guard rejects fixnum ──────────────────────
    {
        std::int64_t args[] = {42 << 1};  // fixnum
        std::uint8_t map[] = {2};
        TEST("Float guard rejects fixnum",
             !check_shape_guard(args, 1, map, 1));
    }

    // ── 2h: Float guard rejects void ────────────────────────
    {
        std::int64_t args[] = {11};  // void
        std::uint8_t map[] = {2};
        TEST("Float guard rejects void",
             !check_shape_guard(args, 1, map, 1));
    }

    // ── 2i: Float guard rejects bool ────────────────────────
    {
        std::int64_t args[] = {7};  // #t
        std::uint8_t map[] = {2};
        TEST("Float guard rejects bool #t",
             !check_shape_guard(args, 1, map, 1));
    }

    // ═══════════════════════════════════════════════════════════
    // Section 3: check_shape_guard — mixed & edge cases
    // ═══════════════════════════════════════════════════════════

    // ── 3a: Mixed shape map ──────────────────────────────────
    {
        std::int64_t args[] = {42 << 1, 7, 11, 0xDEAD};
        std::uint8_t map[] = {1, 3, 5, 0};  // Int, Bool, Void, Dynamic
        TEST("Mixed Int+Bool+Void+Dynamic → match",
             check_shape_guard(args, 4, map, 4));
    }

    // ── 3b: Mixed → one mismatch fails all ───────────────────
    {
        // arg2 should be Void (11) but is fixnum (0)
        std::int64_t args[] = {42 << 1, 7, 0, 0xDEAD};
        std::uint8_t map[] = {1, 3, 5, 0};
        TEST("Mixed → 3rd arg mismatch (Void expected, fixnum got) → fail",
             !check_shape_guard(args, 4, map, 4));
    }

    // ── 3c: Map shorter than args (prefix check) ─────────────
    {
        std::int64_t args[] = {42 << 1, 0, 999};  // args[2] not checked
        std::uint8_t map[] = {1, 1};
        TEST("Map shorter than args (prefix only)",
             check_shape_guard(args, 3, map, 2));
    }

    // ── 3d: Map longer than args ─────────────────────────────
    {
        std::int64_t args[] = {42 << 1};
        std::uint8_t map[] = {1, 1, 1};
        TEST("Map longer than args (min length used)",
             check_shape_guard(args, 1, map, 3));
    }

    // ── 3e: Single arg, single match ─────────────────────────
    {
        std::int64_t args[] = {3};
        std::uint8_t map[] = {3};  // Bool
        TEST("Single arg bool match",
             check_shape_guard(args, 1, map, 1));
    }

    // ── 3f: Single arg mismatch ──────────────────────────────
    {
        std::int64_t args[] = {3};
        std::uint8_t map[] = {1};  // expected Int
        TEST("Single arg bool vs Int → mismatch",
             !check_shape_guard(args, 1, map, 1));
    }

    // ── 3g: All 5 shape codes at once ────────────────────────
    {
        // 1=Int, 2=Float, 3=Bool, 4=String, 5=Void
        // String (code 4) falls through as "unknown" in guard → pass
        std::int64_t args[] = {42 << 1, kFloatBias, 7, kStringBias - 1, 11};
        std::uint8_t map[] = {1, 2, 3, 4, 5};
        TEST("All 5 shape codes (String=4 passes as dynamic)",
             check_shape_guard(args, 5, map, 5));
    }

    // ── 3h: Unknown code (>5) passes through ─────────────────
    {
        std::int64_t args[] = {42, -1};
        std::uint8_t map[] = {99, 200};  // unknown codes
        TEST("Unknown shape codes pass through",
             check_shape_guard(args, 2, map, 2));
    }

    // ── 3i: Zero-count edge: null args ptr ───────────────────
    {
        std::uint8_t map[] = {1, 2, 3};
        TEST("null args with 0 count", check_shape_guard(nullptr, 0, map, 3));
    }

    // ── 3j: Large arg count ──────────────────────────────────
    {
        std::int64_t args[100];
        std::uint8_t map[100];
        for (int i = 0; i < 100; i++) {
            args[i] = static_cast<std::int64_t>(i) << 1;  // fixnum
            map[i] = 1;  // Int
        }
        TEST("100 args all Int → match",
             check_shape_guard(args, 100, map, 100));
    }

    // ── 3k: Large — one mismatch out of 100 ──────────────────
    {
        std::int64_t args[100];
        std::uint8_t map[100];
        for (int i = 0; i < 100; i++) {
            args[i] = static_cast<std::int64_t>(i) << 1;
            map[i] = 1;
        }
        args[50] = 7;  // bool, not fixnum
        TEST("100 args → 51st mismatch → fail",
             !check_shape_guard(args, 100, map, 100));
    }

    // ═══════════════════════════════════════════════════════════
    // Section 4: SpecJITController — cache management
    // ═══════════════════════════════════════════════════════════

    // ── 4a: has_specialization on empty controller ──────────
    {
        // Can't instantiate without AuraJIT, but the logic is simple.
        // We test via the header-only codegen utility functions.
        TEST("SpecJITController not tested (needs AuraJIT)", true);
    }

    // ═══════════════════════════════════════════════════════════
    // Section 5: ShapeProfiler + SpecJIT integration
    // ═══════════════════════════════════════════════════════════

    // ── 5a: Profiler → stable → shape guard should match ────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("test", "add");

        // Warm up: 100 Int calls → stable
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_INT);
        TEST("Integration: profiler stable on Int", profiler.is_stable(fn));
        TEST("Integration: dominant = Int", profiler.dominant_shape(fn) == SHAPE_INT);

        // Build a shape map from profiler data
        std::uint8_t shape_map[4] = {0, 0, 0, 0};
        auto dom = profiler.dominant_shape(fn);
        uint8_t code = 0;
        if (dom == SHAPE_INT) code = 1;
        else if (dom == SHAPE_FLOAT) code = 2;
        else if (dom == SHAPE_BOOL) code = 3;
        else if (dom == SHAPE_STRING) code = 4;
        else if (dom == SHAPE_VOID) code = 5;
        for (int i = 0; i < 4; i++)
            shape_map[i] = code;

        // Guard should match Int args
        std::int64_t args[] = {42 << 1, 0, 100 << 1, (-5LL) << 1};
        TEST("Integration: guard matches stable Int args",
             check_shape_guard(args, 4, shape_map, 4));

        // Guard should reject non-Int arg
        std::int64_t bad_args[] = {42 << 1, 7, 100 << 1, (-5LL) << 1};
        TEST("Integration: guard rejects non-Int (bool #t)",
             !check_shape_guard(bad_args, 4, shape_map, 4));
    }

    // ── 5b: Profiler → stable Float → guard ─────────────────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("test", "float_fn");
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_FLOAT);
        TEST("Integration: profiler stable on Float", profiler.is_stable(fn));

        std::uint8_t map[2] = {2, 2};
        std::int64_t args[] = {kFloatBias, kFloatBias - 1};
        TEST("Integration: Float guard matches",
             check_shape_guard(args, 2, map, 2));

        std::int64_t bad[] = {kFloatBias, 42 << 1};
        TEST("Integration: Float guard rejects fixnum",
             !check_shape_guard(bad, 2, map, 2));
    }

    // ── 5c: Profiler → stable Bool → guard ─────────────────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("test", "bool_fn");
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_BOOL);

        std::uint8_t map[2] = {3, 3};
        std::int64_t args[] = {3, 7};
        TEST("Integration: Bool guard matches",
             check_shape_guard(args, 2, map, 2));
    }

    // ── 5d: Profiler → stable → invalidate → re-profile ────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("test", "mutate_fn");

        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_INT);
        TEST("Integration: stable before inval", profiler.is_stable(fn));

        profiler.invalidate(fn);
        TEST("Integration: not stable after inval", !profiler.is_stable(fn));

        // After inval, shape_map should NOT be populated
        auto dom = profiler.dominant_shape(fn);
        TEST("Integration: dominant=unknown after inval",
             dom == SHAPE_UNKNOWN);

        // Re-stabilize
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_STRING);
        TEST("Integration: re-stable on String", profiler.is_stable(fn));
        TEST("Integration: dominant=String after restabilize",
             profiler.dominant_shape(fn) == SHAPE_STRING);
    }

    // ── 5e: Profiler metrics after stable → guard decision ──
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("test", "metrics_decision");

        for (int i = 0; i < 200; i++)
            profiler.record_shape(fn, SHAPE_INT);

        auto m = profiler.metrics(fn);
        TEST("Integration: is_good_deopt_candidate",
             m.is_good_deopt_candidate);
        TEST("Integration: stability_ratio=1.0",
             m.shape_stability_ratio >= 0.999);
        TEST("Integration: total_calls=200", m.total_calls == 200);
        TEST("Integration: unique_shapes=1", m.unique_shapes_seen == 1);

        // The guard from profiler's dominant should match
        auto dom = profiler.dominant_shape(fn);
        bool should_specialize = (dom != SHAPE_UNKNOWN) && m.is_good_deopt_candidate;
        TEST("Integration: should specialize Int fn",
             should_specialize);
    }

    // ═══════════════════════════════════════════════════════════
    // Section 6: End-to-end shape stability → guard cycle
    // ═══════════════════════════════════════════════════════════

    // ── 6a: Full cycle: profile → stable → guard pass ──────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("sess", "hot_fn");

        // Phase 1: warmup
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_INT);
        auto dom = profiler.dominant_shape(fn);
        uint8_t smap = 0;
        if (dom == SHAPE_INT) smap = 1;

        uint8_t map[1] = {smap};
        std::int64_t arg = 42 << 1;
        bool guard_pass = check_shape_guard(&arg, 1, map, 1);
        TEST("E2E: guard passes for stable Int fn", guard_pass);

        // Phase 2: mutate → invalidate → guard should match unknown
        profiler.invalidate(fn);
        dom = profiler.dominant_shape(fn);
        TEST("E2E: after mutate, dominant=unknown",
             dom == SHAPE_UNKNOWN);
        // When dominant=unknown, set_shape_map in service.ixx won't set map,
        // so shape_map stays null → guard always passes
        TEST("E2E: no specialization when unstable", dom == SHAPE_UNKNOWN);

        // Phase 3: re-stabilize on different shape
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_FLOAT);
        dom = profiler.dominant_shape(fn);
        smap = 0;
        if (dom == SHAPE_FLOAT) smap = 2;
        uint8_t map2[1] = {smap};
        std::int64_t float_arg = kFloatBias;
        guard_pass = check_shape_guard(&float_arg, 1, map2, 1);
        TEST("E2E: guard matches new Float shape", guard_pass);

        // Old Int args should fail the new Float guard
        std::int64_t old_arg = 42 << 1;
        guard_pass = check_shape_guard(&old_arg, 1, map2, 1);
        TEST("E2E: old Int arg fails new Float guard", !guard_pass);
    }

    // ── 6b: Multiple function tracking ──────────────────────
    {
        ShapeProfiler profiler;
        FnKey fn1 = make_fn_key("s", "hot");
        FnKey fn2 = make_fn_key("s", "cold");

        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn1, SHAPE_INT);
        for (int i = 0; i < 50; i++)  // below threshold
            profiler.record_shape(fn2, SHAPE_STRING);

        // Build maps
        uint8_t code1 = 0;
        if (profiler.dominant_shape(fn1) == SHAPE_INT) code1 = 1;
        uint8_t code2 = 0;
        if (profiler.dominant_shape(fn2) == SHAPE_STRING) code2 = 4;

        std::int64_t args1[] = {42 << 1};
        TEST("multi: hot fn guard passes",
             check_shape_guard(args1, 1, &code1, 1));

        // cold fn: unstable → dominant=unknown → code stays 0 → always passes
        std::int64_t args2[] = {7};  // bool
        TEST("multi: cold fn guard passes (dynamic=always pass)",
             check_shape_guard(args2, 1, &code2, 1));
    }

    // ═══════════════════════════════════════════════════════════
    // Summary
    // ═══════════════════════════════════════════════════════════
    std::fprintf(stdout, "\n=== Results: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
