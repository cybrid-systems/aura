// test_shape.cpp — Unit tests for shape infrastructure (Phase 1, #53)
//
// Tests: ShapeID, inline_shape_of, ShapeProfiler
//
#include <cstdio>
#include <cassert>
#include <string>
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

int main() {
    // ── Test 1: Basic ShapeID values ─────────────────────────
    TEST("SHAPE_UNKNOWN == 0", SHAPE_UNKNOWN == 0);
    TEST("SHAPE_INT == 2", SHAPE_INT == 2);
    TEST("SHAPE_BOOL == 4", SHAPE_BOOL == 4);

    // ── Test 2: inline_shape_of ─────────────────────────────
    // Fixnum (int): bit0=0, value > FLOAT_BIAS
    TEST("shape_of(42 << 1) == Int", inline_shape_of(42 << 1) == SHAPE_INT);
    TEST("shape_of(0) == Int", inline_shape_of(0) == SHAPE_INT);  // fixnum 0
    // Fixnum encoding: val << 1, so -1 → -2
    TEST("shape_of(-1 << 1) == Int", inline_shape_of(-2LL) == SHAPE_INT);

    // Bool: 3 = #f, 7 = #t
    TEST("shape_of(3) == Bool", inline_shape_of(3) == SHAPE_BOOL);
    TEST("shape_of(7) == Bool", inline_shape_of(7) == SHAPE_BOOL);

    // Void: 11
    TEST("shape_of(11) == Void", inline_shape_of(11) == SHAPE_VOID);

    // Float: between FLOAT_BIAS (-10000000000000000) and STRING_BIAS (-9000000000000000000)
    // Use a value that's clearly in the range: -10000000000000001
    TEST("shape_of(-10000000000000001) == Float",
         inline_shape_of(-10000000000000001LL) == SHAPE_FLOAT);

    // String: <= STRING_BIAS
    TEST("shape_of(-9000000000000000001) == String",
         inline_shape_of(-9000000000000000001LL) == SHAPE_STRING);

    // ── Test 3: compute_shape_id ────────────────────────────
    {
        Shape int_shape;
        int_shape.tag = ShapeTag::Int;
        int_shape.id = compute_shape_id(int_shape);
        TEST("Int shape_id is deterministic",
             compute_shape_id(int_shape) == int_shape.id);
    }

    {
        Shape pair_shape;
        pair_shape.tag = ShapeTag::Pair;
        pair_shape.id = compute_shape_id(pair_shape);
        TEST("Pair shape_id is deterministic",
             compute_shape_id(pair_shape) == pair_shape.id);
    }

    {
        Shape int_shape;
        int_shape.tag = ShapeTag::Int;
        int_shape.id = compute_shape_id(int_shape);

        Shape str_shape;
        str_shape.tag = ShapeTag::String;
        str_shape.id = compute_shape_id(str_shape);

        Shape hash_shape;
        hash_shape.tag = ShapeTag::Hash;
        hash_shape.type_id = 42;
        hash_shape.key_shape = &int_shape;
        hash_shape.value_shape = &str_shape;
        hash_shape.id = compute_shape_id(hash_shape);
        TEST("Hash shape_id is deterministic",
             compute_shape_id(hash_shape) == hash_shape.id);
        TEST("Hash shape_id != Int shape_id",
             hash_shape.id != int_shape.id);
    }

    // ── Test 4: ShapeProfiler basic recording ────────────────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("test", "add");

        // First 99 calls: not yet stable
        for (int i = 0; i < 99; i++) {
            bool stable = profiler.record_shape(fn, SHAPE_INT);
            if (i < 99 - 1) {
                TEST("Before 100, not stable (iter " + std::to_string(i) + ")", !stable);
            }
        }

        // 100th call: should become stable
        bool stable = profiler.record_shape(fn, SHAPE_INT);
        TEST("100 identical shapes → stable", stable);
        TEST("is_stable returns true", profiler.is_stable(fn));
        TEST("dominant_shape == Int", profiler.dominant_shape(fn) == SHAPE_INT);
    }

    // ── Test 5: ShapeProfiler stability detection ────────────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("test", "mixed");

        // 900 Int + 100 Float = 90% Int
        for (int i = 0; i < 900; i++)
            profiler.record_shape(fn, SHAPE_INT);
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_FLOAT);

        TEST("90% Int 10% Float → stable (90% ratio)", profiler.is_stable(fn));
        TEST("dominant is Int", profiler.dominant_shape(fn) == SHAPE_INT);
    }

    // ── Test 6: ShapeProfiler invalidate ─────────────────────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("test", "mutable");

        // Become stable
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_INT);
        TEST("Stable before invalidate", profiler.is_stable(fn));

        // Invalidate (simulates mutate:*)
        profiler.invalidate(fn);
        TEST("Not stable after invalidate", !profiler.is_stable(fn));
        TEST("Version incremented", profiler.current_snapshot(fn).version == 1);

        // Re-stabilize
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_STRING);
        TEST("Re-stabilized after invalidate", profiler.is_stable(fn));
        TEST("New dominant is String", profiler.dominant_shape(fn) == SHAPE_STRING);
    }

    // ── Test 7: ShapeProfiler metrics ────────────────────────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("test", "metrics_test");

        for (int i = 0; i < 200; i++)
            profiler.record_shape(fn, SHAPE_INT);

        auto m = profiler.metrics(fn);
        TEST("Metrics total_calls == 200", m.total_calls == 200);
        TEST("Metrics unique_shapes == 1", m.unique_shapes_seen == 1);
        TEST("Metrics is_good_deopt_candidate", m.is_good_deopt_candidate);
        TEST("Metrics deopt_count == 0", m.deopt_count == 0);
    }

    // ── Test 8: FnKey ───────────────────────────────────────
    {
        auto k1 = make_fn_key("session1", "fn1");
        auto k2 = make_fn_key("session1", "fn1");
        auto k3 = make_fn_key("session2", "fn1");
        TEST("Same session+name → same key", k1 == k2);
        TEST("Different session → different key", k1 != k3);
    }

    // ── Test 9: shape_tag_name ──────────────────────────────
    {
        TEST("shape_tag_name(Int) == 'int'",
             std::string(shape_tag_name(ShapeTag::Int)) == "int");
        TEST("shape_tag_name(String) == 'string'",
             std::string(shape_tag_name(ShapeTag::String)) == "string");
    }

    // ── Summary ─────────────────────────────────────────────
    std::fprintf(stdout, "\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
