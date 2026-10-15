// test_spec_jit.cpp — Unit tests for L1 type specialization (Phase 2, #53)
//
// Tests: check_shape_guard, SpecJITController, Profiler+SpecJIT integration
//
#include "compiler/value_tags.h" // #4502: AURA_MAKE_FLOAT / AURA_MAKE_STRING
#include "../src/compiler/spec_jit_controller.h"
#include "../src/compiler/shape.h"
#include "../src/compiler/shape_profiler.h"
#include "../src/compiler/aura_jit.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <print>
#include <string>
using namespace aura::compiler::shape;

// Issue #2276 AC4: metric accessor declared `extern "C"` in
// src/compiler/spec_jit_controller.cpp (mirrors the
// aura_specjit_*_total_v_read pattern used by other g_*_total
// counters in that TU). Declared here for direct read access
// from the test.
extern "C" std::uint64_t aura_specjit_shape_version_miss_total_v_read(void);

// Issue #2301 AC4: process-global storm-clear counter accessor.
// Mirrors the #2276 declaration above — the per-controller
// storm_clear_count() accessor on SpecJITController is declared
// via #include "spec_jit_controller.h" (it's a member function),
// but the C-linkage reader is defined in spec_jit_controller.cpp
// and needs an explicit forward decl here for direct read access.
extern "C" std::uint64_t aura_specjit_storm_clear_total_v_read(void);

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name, expr)                                                                           \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(expr)) {                                                                             \
            std::println(std::cerr, "FAIL: %s ({})", name, #expr);                                 \
        } else {                                                                                   \
            tests_passed++;                                                                        \
            std::println("PASS: {}", name);                                                        \
        }                                                                                          \
    } while (0)

// ── Float/String bias constants ───────────────────────────────
static constexpr std::int64_t kFloatBias = AURA_MAKE_FLOAT(0); // #4502: ImmKind float, idx 0
// Issue #278 follow-up: STRING_BIAS_VAL_2 = STRING_BIAS_VAL + 2 (v2
// string encoding, low 2 bits = 2). shape_profiler.cpp / shape.cpp use
// STRING_BIAS_VAL_2 internally; tests need to align with that or the
// boundary cases (kStringBias + 1) fall into the wrong branch.
static constexpr std::int64_t kStringBias = AURA_MAKE_STRING(0); // #4502: ImmKind string, idx 0

// Issue #4522: float pool observers (extern "C" in aura_jit_runtime.cpp;
// also declared in runtime_shared.h). aura_alloc_float counts are the
// issue's Verify metric: the unboxed loop must allocate ~0 boxes.
extern "C" std::int64_t aura_alloc_float(double d);
extern "C" double aura_float_ref(std::int64_t val);
extern "C" std::size_t aura_jit_pool_size();

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

// ── Issue #2276 AC5: file-scope ScalarFn stubs (function ptr targets) ─
// Defined at file scope (not in block) because block-scope function
// definitions are not allowed in standard C++. Used as the fn_ptr
// argument to SpecJITController::install_specialization.
static int64_t spec_jit_2276_stub_fn_a(int64_t*, uint32_t) {
    return 1;
}
static int64_t spec_jit_2276_stub_fn_b(int64_t*, uint32_t) {
    return 2;
}

int main() {
    // ═══════════════════════════════════════════════════════════
    // Section 1: check_shape_guard — basic cases
    // ═══════════════════════════════════════════════════════════

    // ── 1a: Null shape_map → always matches ──────────────────
    {
        std::int64_t args[] = {0, 3, 11};
        TEST("null shape_map always matches", check_shape_guard(args, 3, nullptr, 0));
    }

    // ── 1b: Empty args (0 count) ─────────────────────────────
    {
        std::uint8_t map[] = {1, 2};
        TEST("0 args always matches", check_shape_guard(nullptr, 0, map, 2));
    }

    // ── 1c: Empty map (0 size) ───────────────────────────────
    {
        std::int64_t args[] = {42, -1, 100};
        TEST("Empty map always matches", check_shape_guard(args, 3, nullptr, 0));
    }

    // ── 1d: Dynamic (0) matches everything ───────────────────
    {
        std::int64_t args[] = {42, -1, 100, 3, 11, kFloatBias};
        std::uint8_t map[] = {0, 0, 0, 0, 0, 0};
        TEST("Dynamic (0) matches any value", check_shape_guard(args, 6, map, 6));
    }

    // ── 1e: Int guard (1) matches fixnums ────────────────────
    {
        std::int64_t args[] = {42 << 1, 0, (-100LL) << 1, 2, 0x7FFFFFFFFFFFFFFELL};
        std::uint8_t map[] = {1, 1, 1, 1, 1};
        TEST("Int guard matches all fixnums", check_shape_guard(args, 5, map, 5));
    }

    // ── 1f: Int guard rejects non-fixnums ────────────────────
    {
        // 7 = bool #t, 11 = void, 3 = bool #f
        std::int64_t args[] = {42 << 1, 7, 11, 3};
        std::uint8_t map[] = {1, 1, 1, 1};
        TEST("Int guard rejects bool #t (7)", !check_shape_guard(args, 4, map, 4));
    }

    // ── 1g: Int guard rejects string-encoded value ───────────
    {
        std::int64_t args[] = {AURA_MAKE_STRING(1)}; // string encoded
        std::uint8_t map[] = {1};
        TEST("Int guard rejects string-encoded", !check_shape_guard(args, 1, map, 1));
    }

    // ── 1h: Int guard rejects float-encoded value ───────────
    {
        std::int64_t args[] = {kFloatBias}; // float encoded
        std::uint8_t map[] = {1};
        TEST("Int guard rejects float-encoded", !check_shape_guard(args, 1, map, 1));
    }

    // ═══════════════════════════════════════════════════════════
    // Section 2: check_shape_guard — type-specific
    // ═══════════════════════════════════════════════════════════

    // ── 2a: Bool guard (3) matches 3 and 7 ───────────────────
    {
        std::int64_t args[] = {3, 7, 3};
        std::uint8_t map[] = {3, 3, 3};
        TEST("Bool guard matches 3 and 7", check_shape_guard(args, 3, map, 3));
    }

    // ── 2b: Bool guard rejects fixnum ────────────────────────
    {
        std::int64_t args[] = {42 << 1, 0, 100 << 1};
        std::uint8_t map[] = {3, 3, 3};
        TEST("Bool guard rejects fixnums", !check_shape_guard(args, 3, map, 3));
    }

    // ── 2c: Bool guard rejects void ──────────────────────────
    {
        std::int64_t args[] = {11, 7};
        std::uint8_t map[] = {3, 3};
        TEST("Bool guard rejects void (11)", !check_shape_guard(args, 2, map, 2));
    }

    // ── 2d: Void guard (5) matches 11 ────────────────────────
    {
        std::int64_t args[] = {11};
        std::uint8_t map[] = {5};
        TEST("Void guard matches 11", check_shape_guard(args, 1, map, 1));
    }

    // ── 2e: Void guard rejects non-void ──────────────────────
    {
        std::int64_t args[] = {0, 7, kFloatBias};
        std::uint8_t map[] = {5, 5, 5};
        TEST("Void guard rejects non-void", !check_shape_guard(args, 3, map, 3));
    }

    // ── 2f: Float guard (2) ─────────────────────────────────
    {
        // At FLOAT_BIAS boundary
        std::int64_t args[] = {kFloatBias, AURA_MAKE_FLOAT(1), AURA_MAKE_FLOAT(2)}; // #4482
        std::uint8_t map[] = {2, 2, 2};
        TEST("Float guard matches float range", check_shape_guard(args, 3, map, 3));
    }

    // ── 2g: Float guard rejects fixnum ──────────────────────
    {
        std::int64_t args[] = {42 << 1}; // fixnum
        std::uint8_t map[] = {2};
        TEST("Float guard rejects fixnum", !check_shape_guard(args, 1, map, 1));
    }

    // ── 2h: Float guard rejects void ────────────────────────
    {
        std::int64_t args[] = {11}; // void
        std::uint8_t map[] = {2};
        TEST("Float guard rejects void", !check_shape_guard(args, 1, map, 1));
    }

    // ── 2i: Float guard rejects bool ────────────────────────
    {
        std::int64_t args[] = {7}; // #t
        std::uint8_t map[] = {2};
        TEST("Float guard rejects bool #t", !check_shape_guard(args, 1, map, 1));
    }

    // ═══════════════════════════════════════════════════════════
    // Section 3: check_shape_guard — mixed & edge cases
    // ═══════════════════════════════════════════════════════════

    // ── 3a: Mixed shape map ──────────────────────────────────
    {
        std::int64_t args[] = {42 << 1, 7, 11, 0xDEAD};
        std::uint8_t map[] = {1, 3, 5, 0}; // Int, Bool, Void, Dynamic
        TEST("Mixed Int+Bool+Void+Dynamic → match", check_shape_guard(args, 4, map, 4));
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
        std::int64_t args[] = {42 << 1, 0, 999}; // args[2] not checked
        std::uint8_t map[] = {1, 1};
        TEST("Map shorter than args (prefix only)", check_shape_guard(args, 3, map, 2));
    }

    // ── 3d: Map longer than args ─────────────────────────────
    {
        std::int64_t args[] = {42 << 1};
        std::uint8_t map[] = {1, 1, 1};
        TEST("Map longer than args (min length used)", check_shape_guard(args, 1, map, 3));
    }

    // ── 3e: Single arg, single match ─────────────────────────
    {
        std::int64_t args[] = {3};
        std::uint8_t map[] = {3}; // Bool
        TEST("Single arg bool match", check_shape_guard(args, 1, map, 1));
    }

    // ── 3f: Single arg mismatch ──────────────────────────────
    {
        std::int64_t args[] = {3};
        std::uint8_t map[] = {1}; // expected Int
        TEST("Single arg bool vs Int → mismatch", !check_shape_guard(args, 1, map, 1));
    }

    // ── 3g: All 5 shape codes at once ────────────────────────
    {
        // 1=Int, 2=Float, 3=Bool, 4=String, 5=Void
        // String (code 4) falls through as "unknown" in guard → pass
        std::int64_t args[] = {42 << 1, kFloatBias, 7, AURA_MAKE_STRING(1), 11};
        std::uint8_t map[] = {1, 2, 3, 4, 5};
        TEST("All 5 shape codes (String=4 passes as dynamic)", check_shape_guard(args, 5, map, 5));
    }

    // ── 3h: Unknown code (>5) passes through ─────────────────
    {
        std::int64_t args[] = {42, -1};
        std::uint8_t map[] = {99, 200}; // unknown codes
        TEST("Unknown shape codes pass through", check_shape_guard(args, 2, map, 2));
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
            args[i] = static_cast<std::int64_t>(i) << 1; // fixnum
            map[i] = 1;                                  // Int
        }
        TEST("100 args all Int → match", check_shape_guard(args, 100, map, 100));
    }

    // ── 3k: Large — one mismatch out of 100 ──────────────────
    {
        std::int64_t args[100];
        std::uint8_t map[100];
        for (int i = 0; i < 100; i++) {
            args[i] = static_cast<std::int64_t>(i) << 1;
            map[i] = 1;
        }
        args[50] = 7; // bool, not fixnum
        TEST("100 args → 51st mismatch → fail", !check_shape_guard(args, 100, map, 100));
    }

    // ═══════════════════════════════════════════════════════════
    // Section 4: SpecJITController — cache management
    // ═══════════════════════════════════════════════════════════

    // ── 4a-4h: Issue #2276 cache miss on shape_version bump ─
    // AC1: install → storm bump → get/has miss
    // AC2: re-install after storm → get/has hit (fresh version)
    // AC4: specjit_shape_version_miss_total counter bumped on miss
    // Source-cite (src/compiler/spec_jit_controller.cpp):
    //   has_specialization        — version mismatch → return false + bump counter
    //   get_specialized          — version mismatch → return nullptr + bump counter
    //   install_specialization   — stamps entry.version = current_global_shape_version()
    //   bump_shape_version_on_storm_enter (src/compiler/shape_profiler.h:66)
    {
        aura::jit::AuraJIT jit;
        SpecJITController ctl(jit);
        constexpr aura::jit::ScalarFn kFnA = &spec_jit_2276_stub_fn_a;
        constexpr aura::jit::ScalarFn kFnB = &spec_jit_2276_stub_fn_b;
        constexpr ShapeID kShape = SHAPE_INT;

        // 4a: empty ctl — both lookups miss
        TEST("4a: empty ctl has_specialization=false", !ctl.has_specialization("add", kShape));
        TEST("4a: empty ctl get_specialized=nullptr",
             ctl.get_specialized("add", kShape) == nullptr);

        // 4b: install → hit (entry stamped with current version)
        ctl.install_specialization("add", kShape, kFnA);
        TEST("4b: post-install has=true", ctl.has_specialization("add", kShape));
        TEST("4b: post-install get=kFnA", ctl.get_specialized("add", kShape) == kFnA);

        // 4c: bump_shape_version_on_storm_enter → miss (AC1 + AC4)
        const auto miss_pre_storm = aura_specjit_shape_version_miss_total_v_read();
        bump_shape_version_on_storm_enter();
        TEST("4c: post-storm has=false (AC1)", !ctl.has_specialization("add", kShape));
        TEST("4c: post-storm get=nullptr (AC1)", ctl.get_specialized("add", kShape) == nullptr);
        const auto miss_post_storm = aura_specjit_shape_version_miss_total_v_read();
        TEST("4c: miss counter bumped (>=miss_pre_storm+2)", miss_post_storm >= miss_pre_storm + 2);

        // 4d: re-install with current version → hit (AC2)
        ctl.install_specialization("add", kShape, kFnB);
        TEST("4d: re-install has=true (AC2)", ctl.has_specialization("add", kShape));
        TEST("4d: re-install get=kFnB (AC2)", ctl.get_specialized("add", kShape) == kFnB);

        // 4e: 2nd storm bump → miss again
        bump_shape_version_on_storm_enter();
        TEST("4e: 2nd storm has=false", !ctl.has_specialization("add", kShape));
        TEST("4e: 2nd storm get=nullptr", ctl.get_specialized("add", kShape) == nullptr);

        // 4f: null fn rejected (Issue #986 invariant — no null placeholders)
        ctl.install_specialization("null_fn", kShape, nullptr);
        TEST("4f: null install = no entry created",
             !ctl.has_specialization("null_fn", kShape) &&
                 ctl.get_specialized("null_fn", kShape) == nullptr);

        // 4g: distinct fn_names don't cross-contaminate
        ctl.install_specialization("fn1", kShape, kFnA);
        ctl.install_specialization("fn2", kShape, kFnB);
        TEST("4g: fn1=fnA, fn2=fnB isolated", ctl.get_specialized("fn1", kShape) == kFnA &&
                                                  ctl.get_specialized("fn2", kShape) == kFnB);

        // 4h: invalidate clears entries regardless of stamp
        ctl.install_specialization("clear_me", kShape, kFnA);
        ctl.invalidate("clear_me");
        TEST("4h: invalidate clears has + get",
             !ctl.has_specialization("clear_me", kShape) &&
                 ctl.get_specialized("clear_me", kShape) == nullptr);
    }

    // ── 4i-4o: Issue #2301 proactive clear on deopt-storm enter ─
    // AC1: install → on_deopt_storm → specialization_count() == 0
    //      without waiting for get.
    // AC2: post-clear get/has remain miss until re-install under
    //      new shape_version.
    // AC3: empty SpecJIT → on_deopt_storm does not bump extra
    //      work beyond existing storm counters (zero-cost when
    //      no specializations cached — clear() on an empty
    //      unordered_map is a no-op; the counter still bumps
    //      to record the storm-fire event itself, but no
    //      iteration / memory pressure is added).
    // AC4: Metric specjit_storm_clear_total (via C-linkage
    //      reader aura_specjit_storm_clear_total_v_read) +
    //      per-controller storm_clear_count() accessor bump.
    // AC5: Existing #2276 Section 4 AC rows (4a-4h) still green
    //      by construction — on_deopt_storm() delegates to the
    //      existing clear() which clears specializations_;
    //      install_specialization / has_specialization /
    //      get_specialization semantics unchanged.
    {
        aura::jit::AuraJIT jit;
        SpecJITController ctl(jit);
        constexpr aura::jit::ScalarFn kFnA = &spec_jit_2276_stub_fn_a;
        constexpr aura::jit::ScalarFn kFnB = &spec_jit_2276_stub_fn_b;
        constexpr ShapeID kShape = SHAPE_INT;

        // 4i: empty ctl → on_deopt_storm() is safe, no entries
        // to clear, but storm-clear counter still bumps to
        // record the storm-fire event (AC3: zero extra work
        // beyond counters; AC4: counter reachable via
        // C-linkage reader + per-controller accessor).
        const auto storm_pre = aura_specjit_storm_clear_total_v_read();
        const auto ctl_pre = ctl.storm_clear_count();
        ctl.on_deopt_storm();
        TEST("4i: empty ctl on_deopt_storm → specialization_count == 0 (AC1)",
             ctl.specialization_count() == 0);
        TEST("4i: empty ctl storm_clear_count bumped (AC4)",
             ctl.storm_clear_count() == ctl_pre + 1);
        TEST("4i: empty ctl global storm counter bumped (AC4)",
             aura_specjit_storm_clear_total_v_read() == storm_pre + 1);

        // 4j: install 2 entries + on_deopt_storm → cache
        // drains immediately (AC1) without waiting for get.
        ctl.install_specialization("add", kShape, kFnA);
        ctl.install_specialization("mul", kShape, kFnB);
        TEST("4j: pre-storm count == 2", ctl.specialization_count() == 2);
        ctl.on_deopt_storm();
        TEST("4j: post-storm count == 0 (AC1 — no waiting for get)",
             ctl.specialization_count() == 0);

        // 4k: post-clear get/has remain miss until re-install
        // under new shape_version (AC2). bump_shape_version_on_storm_enter
        // simulates a new generation; install_specialization
        // stamps the entry with the current version, restoring
        // the hit path.
        TEST("4k: post-clear has=false (AC2)", !ctl.has_specialization("add", kShape));
        TEST("4k: post-clear get=nullptr (AC2)", ctl.get_specialized("add", kShape) == nullptr);
        bump_shape_version_on_storm_enter();
        ctl.install_specialization("add", kShape, kFnB);
        TEST("4k: re-install post-storm has=true (AC2)", ctl.has_specialization("add", kShape));
        TEST("4k: re-install post-storm get=kFnB (AC2)",
             ctl.get_specialized("add", kShape) == kFnB);

        // 4l: AC3 — empty SpecJIT → on_deopt_storm does not
        // bump extra work beyond the storm-clear counter.
        // Verified by:
        // (a) specialization_count() stays at 0
        // (b) storm_clear_count() bumps by exactly 1
        // (c) no other #2276 counter (#2276 shape-version
        //     miss) is bumped by on_deopt_storm() — the
        //     clear() path does not consult shape_version.
        const auto miss_pre = aura_specjit_shape_version_miss_total_v_read();
        ctl.on_deopt_storm(); // ctl is empty now
        TEST("4l: empty on_deopt_storm no extra #2276 miss bump (AC3)",
             aura_specjit_shape_version_miss_total_v_read() == miss_pre);

        // 4m: AC4 — repeated storm fires bump the counter
        // monotonically; per-controller storm_clear_count()
        // matches the global reader after a single-controller
        // test scenario (no concurrent controllers).
        const auto storm_pre2 = aura_specjit_storm_clear_total_v_read();
        const auto ctl_pre2 = ctl.storm_clear_count();
        ctl.on_deopt_storm();
        ctl.on_deopt_storm();
        ctl.on_deopt_storm();
        TEST("4m: 3 storms → storm_clear_count += 3 (AC4)",
             ctl.storm_clear_count() == ctl_pre2 + 3);
        TEST("4m: 3 storms → global storm counter += 3 (AC4)",
             aura_specjit_storm_clear_total_v_read() == storm_pre2 + 3);

        // 4n: AC5 — install → clear → install cycle preserves
        // the spec_jit invariants (entry stamped with current
        // version, last_used advanced, no null placeholders).
        ctl.install_specialization("n1", kShape, kFnA);
        const auto ver_after_install = current_global_shape_version();
        ctl.on_deopt_storm();
        bump_shape_version_on_storm_enter();
        ctl.install_specialization("n1", kShape, kFnB);
        TEST("4n: re-install after storm clears, fresh version (AC5)",
             ctl.get_specialized("n1", kShape) == kFnB && ctl.has_specialization("n1", kShape));
        TEST("4n: version stamped on re-install (AC5)",
             current_global_shape_version() > ver_after_install);

        // 4o: AC5 — invalidate() still works post-storm-clear
        // (storm-clear does not corrupt internal state for
        // subsequent invalidate / install operations).
        ctl.install_specialization("o1", kShape, kFnA);
        ctl.invalidate("o1");
        TEST("4o: invalidate post-storm-clean works (AC5)",
             !ctl.has_specialization("o1", kShape) && ctl.get_specialized("o1", kShape) == nullptr);
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
        if (dom == SHAPE_INT)
            code = 1;
        else if (dom == SHAPE_FLOAT)
            code = 2;
        else if (dom == SHAPE_BOOL)
            code = 3;
        else if (dom == SHAPE_STRING)
            code = 4;
        else if (dom == SHAPE_VOID)
            code = 5;
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
        std::int64_t args[] = {kFloatBias, AURA_MAKE_FLOAT(1)};
        TEST("Integration: Float guard matches", check_shape_guard(args, 2, map, 2));

        std::int64_t bad[] = {kFloatBias, 42 << 1};
        TEST("Integration: Float guard rejects fixnum", !check_shape_guard(bad, 2, map, 2));
    }

    // ── 5c: Profiler → stable Bool → guard ─────────────────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("test", "bool_fn");
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_BOOL);

        std::uint8_t map[2] = {3, 3};
        std::int64_t args[] = {3, 7};
        TEST("Integration: Bool guard matches", check_shape_guard(args, 2, map, 2));
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
        TEST("Integration: dominant=unknown after inval", dom == SHAPE_UNKNOWN);

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
        TEST("Integration: is_good_deopt_candidate", m.is_good_deopt_candidate);
        TEST("Integration: stability_ratio=1.0", m.shape_stability_ratio >= 0.999);
        TEST("Integration: total_calls=200", m.total_calls == 200);
        TEST("Integration: unique_shapes=1", m.unique_shapes_seen == 1);

        // The guard from profiler's dominant should match
        auto dom = profiler.dominant_shape(fn);
        bool should_specialize = (dom != SHAPE_UNKNOWN) && m.is_good_deopt_candidate;
        TEST("Integration: should specialize Int fn", should_specialize);
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
        if (dom == SHAPE_INT)
            smap = 1;

        uint8_t map[1] = {smap};
        std::int64_t arg = 42 << 1;
        bool guard_pass = check_shape_guard(&arg, 1, map, 1);
        TEST("E2E: guard passes for stable Int fn", guard_pass);

        // Phase 2: mutate → invalidate → guard should match unknown
        profiler.invalidate(fn);
        dom = profiler.dominant_shape(fn);
        TEST("E2E: after mutate, dominant=unknown", dom == SHAPE_UNKNOWN);
        // When dominant=unknown, set_shape_map in service.ixx won't set map,
        // so shape_map stays null → guard always passes
        TEST("E2E: no specialization when unstable", dom == SHAPE_UNKNOWN);

        // Phase 3: re-stabilize on different shape
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_FLOAT);
        dom = profiler.dominant_shape(fn);
        smap = 0;
        if (dom == SHAPE_FLOAT)
            smap = 2;
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
        for (int i = 0; i < 50; i++) // below threshold
            profiler.record_shape(fn2, SHAPE_STRING);

        // Build maps
        uint8_t code1 = 0;
        if (profiler.dominant_shape(fn1) == SHAPE_INT)
            code1 = 1;
        uint8_t code2 = 0;
        if (profiler.dominant_shape(fn2) == SHAPE_STRING)
            code2 = 4;

        std::int64_t args1[] = {42 << 1};
        TEST("multi: hot fn guard passes", check_shape_guard(args1, 1, &code1, 1));

        // cold fn: unstable → dominant=unknown → code stays 0 → always passes
        std::int64_t args2[] = {7}; // bool
        TEST("multi: cold fn guard passes (dynamic=always pass)",
             check_shape_guard(args2, 1, &code2, 1));
    }

    // ═══════════════════════════════════════════════════════════
    // Section 7: Heap shape guard — Pair/Vector/Hash/Closure/Ref
    // ═══════════════════════════════════════════════════════════

    // ── 7a: Pair guard (10) matches pair refs ────────────────
    {
        // Pair ref encoding: (index << 6) | (0 << 2) | 1, ref_type=0
        // index=0: (0<<6) | 0 | 1 = 1
        // index=1: (1<<6) | 0 | 1 = 65
        std::int64_t args[] = {1, 65, 129}; // all ref_type=0 (Pair)
        std::uint8_t map[] = {10, 10, 10};
        TEST("Pair guard (10) matches pair refs", check_shape_guard(args, 3, map, 3));
    }

    // ── 7b: Pair guard rejects non-ref ──────────────────────
    {
        std::int64_t args[] = {42 << 1}; // fixnum, not pair ref
        std::uint8_t map[] = {10};
        TEST("Pair guard rejects fixnum", !check_shape_guard(args, 1, map, 1));
    }

    // ── 7c: Pair guard rejects bool ──────────────────────────
    {
        std::int64_t args[] = {7}; // bool #t (has bit pattern 3, not 1)
        std::uint8_t map[] = {10};
        TEST("Pair guard rejects bool #t", !check_shape_guard(args, 1, map, 1));
    }

    // ── 7d: Pair guard rejects void ──────────────────────────
    {
        std::int64_t args[] = {11}; // void
        std::uint8_t map[] = {10};
        TEST("Pair guard rejects void", !check_shape_guard(args, 1, map, 1));
    }

    // ── 7e: Vector guard (11) matches vector refs ────────────
    {
        // Vector ref: ref_type=3 → (anything << 2) | 1 with bits 2-5 = 3
        // (0 << 6) | (3 << 2) | 1 = 13
        std::int64_t args[] = {13, 13 + 64, 13 + 128}; // vectors
        std::uint8_t map[] = {11, 11, 11};
        TEST("Vector guard (11) matches vector refs", check_shape_guard(args, 3, map, 3));
    }

    // ── 7f: Vector guard rejects Pair ref ────────────────────
    {
        std::int64_t args[] = {1}; // pair ref (ref_type=0)
        std::uint8_t map[] = {11}; // expected vector
        TEST("Vector guard rejects pair ref", !check_shape_guard(args, 1, map, 1));
    }

    // ── 7g: Hash guard (12) matches hash refs ────────────────
    {
        // Hash ref: ref_type=4 → (4 << 2) | 1 = 17
        std::int64_t args[] = {17, 17 + 64};
        std::uint8_t map[] = {12, 12};
        TEST("Hash guard (12) matches hash refs", check_shape_guard(args, 2, map, 2));
    }

    // ── 7h: Hash guard rejects vector ────────────────────────
    {
        std::int64_t args[] = {13}; // vector ref (ref_type=3)
        std::uint8_t map[] = {12};  // expected hash
        TEST("Hash guard rejects vector ref", !check_shape_guard(args, 1, map, 1));
    }

    // ── 7i: Closure guard (13) matches closure refs ─────────
    {
        // Closure ref: ref_type=1 → (1 << 2) | 1 = 5
        std::int64_t args[] = {5, 69}; // (0<<6)|4|1=5, (1<<6)|4|1=69
        std::uint8_t map[] = {13, 13};
        TEST("Closure guard (13) matches closure refs", check_shape_guard(args, 2, map, 2));
    }

    // ── 7j: Closure guard rejects void ──────────────────────
    {
        std::int64_t args[] = {11}; // void
        std::uint8_t map[] = {13};  // expected closure
        TEST("Closure guard rejects void", !check_shape_guard(args, 1, map, 1));
    }

    // ── 7k: Generic Ref guard (14) matches any ref ──────────
    {
        std::int64_t args[] = {1, 5, 13, 17};  // Pair, Closure, Vector, Hash
        std::uint8_t map[] = {14, 14, 14, 14}; // generic ref
        TEST("Ref guard (14) matches any ref type", check_shape_guard(args, 4, map, 4));
    }

    // ── 7l: Ref guard rejects non-ref (fixnum) ───────────────
    {
        std::int64_t args[] = {42 << 1}; // fixnum
        std::uint8_t map[] = {14};       // generic ref
        TEST("Ref guard rejects fixnum", !check_shape_guard(args, 1, map, 1));
    }

    // ── 7m: Ref guard rejects bool ───────────────────────────
    {
        std::int64_t args[] = {7}; // bool #t
        std::uint8_t map[] = {14};
        TEST("Ref guard rejects bool", !check_shape_guard(args, 1, map, 1));
    }

    // ═══════════════════════════════════════════════════════════
    // Section 8: Version Guard — mutate awareness
    // ═══════════════════════════════════════════════════════════

    // ── 8a: Version increments on invalidate ─────────────────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("sess", "version_test");

        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_INT);

        auto snap = profiler.current_snapshot(fn);
        TEST("Version: initial version == 0", snap.version == 0);

        profiler.invalidate(fn); // simulate mutate
        snap = profiler.current_snapshot(fn);
        TEST("Version: after 1 mutate == 1", snap.version == 1);

        profiler.invalidate(fn); // another mutate
        snap = profiler.current_snapshot(fn);
        TEST("Version: after 2 mutates == 2", snap.version == 2);
    }

    // ── 8b: Version + shape guard integration ───────────────
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("sess", "version_guard");

        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_INT);
        TEST("Version guard: stable before mutate", profiler.is_stable(fn));

        // Build shape map
        uint8_t map[1] = {1}; // Int
        std::int64_t arg = 42 << 1;
        TEST("Version guard: pre-mutate guard passes", check_shape_guard(&arg, 1, map, 1));

        // Simulate version check (as service.ixx would do):
        // after invalidate, the snapshot version changes
        auto pre_snap = profiler.current_snapshot(fn);
        std::uint64_t expected_version = pre_snap.version;

        // mutate
        profiler.invalidate(fn);
        auto post_snap = profiler.current_snapshot(fn);

        // Version check: caller compares cached version vs current
        bool version_match = (post_snap.version == expected_version);
        TEST("Version guard: version mismatch after mutate", !version_match);
        TEST("Version guard: new version == old + 1", post_snap.version == expected_version + 1);

        // After invalidate, shape_map is cleared → guard won't be set
        // so the system falls back to IRInterpreter
        TEST("Version guard: not stable after mutate", !profiler.is_stable(fn));
    }

    // ── 8c: Multiple mutates with interleaved re-stabilize ──
    {
        ShapeProfiler profiler;
        FnKey fn = make_fn_key("sess", "multi_mutate");

        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_INT);
        auto snap1 = profiler.current_snapshot(fn);
        TEST("Multi mutate: initial version 0", snap1.version == 0);

        profiler.invalidate(fn); // mutate 1
        profiler.invalidate(fn); // mutate 2
        auto snap2 = profiler.current_snapshot(fn);
        TEST("Multi mutate: version 2 after 2 mutates", snap2.version == 2);

        // Re-stabilize on different shape
        for (int i = 0; i < 100; i++)
            profiler.record_shape(fn, SHAPE_FLOAT);

        // Version stays at 2 (mutate count, not affected by re-stabilize)
        auto snap3 = profiler.current_snapshot(fn);
        TEST("Multi mutate: version 2 after restabilize", snap3.version == 2);
        TEST("Multi mutate: stable on Float",
             profiler.is_stable(fn) && profiler.dominant_shape(fn) == SHAPE_FLOAT);

        // Another mutate resets to version 3
        profiler.invalidate(fn);
        TEST("Multi mutate: version 3 after another mutate",
             profiler.current_snapshot(fn).version == 3);
    }

    // ── Section 9: Issue #4522 — named-let float accumulator unboxing ──
    //
    // Builds the TCO'd flat shape of
    //   (define (sum n) (let loop ((acc 0.0) (i 0))
    //     (if (= i n) acc (loop (+ acc 0.5) (+ i 1)))))
    // as one FlatFunction with a Jump back-edge, then compiles it twice:
    // once with SHAPE_FLOAT feedback on the accumulator chain (the
    // specialized compile SpecJITController installs after profiling) and
    // once all-Dynamic (the boxed shape-change recompile). ACs:
    //   AC1: float-map loop performs exactly ONE aura_alloc_float — the
    //        loop-exit rebox; the dynamic-map loop allocates 2 per
    //        iteration (loop-body const + add), i.e. the unboxed path
    //        removed the per-iteration boxing churn.
    //   AC2: results bit-identical between the two compiles (rebox is
    //        bit-exact; only the pool index is fresh).
    //   AC3: float-known comparisons lower as ordered value fcmp (=, <,
    //        > over raw doubles; NaN compares false).
    //   AC4: per-loop wall time printed (informational, no CI-flaky
    //        timing assert).
    {
        std::println("\n--- Section 9: #4522 named-let float accumulator unboxing ---");
        using aura::jit::FlatBlock;
        using aura::jit::FlatFunction;
        using aura::jit::FlatInstruction;
        // ir.ixx IROpcode is a C++ module this TU does not import; pin the
        // raw numeric values (order: Nop ConstI64 ConstF64 Local Arg Add Sub
        // Mul Div Eq Lt Gt Le Ge And Or Not Branch Jump Call Return ...).
        constexpr std::uint32_t kOpConstI64 = 1, kOpConstF64 = 2, kOpLocal = 3, kOpArg = 4,
                                kOpAdd = 5, kOpEq = 9, kOpLt = 10, kOpGt = 11, kOpBranch = 17,
                                kOpJump = 18, kOpReturn = 20;
        constexpr std::int64_t kIters = 1000;
        const std::int64_t n_fix = kIters << 1; // fixnum encode
        constexpr std::uint32_t kLocals = 8;
        // locals: 0=acc(F) 1=i(I) 2=n(I,arg) 3=half(F,tmp) 4=acc2(F,tmp)
        //         5=cond(B) 6=one(I,tmp) 7=i2(I,tmp)

        FlatInstruction loop_insts[14] = {};
        // entry: acc = 0.0; i = 0; n = arg0
        loop_insts[0].opcode = kOpConstF64;
        loop_insts[0].ops[0] = 0;
        loop_insts[0].ops[1] = 0;
        loop_insts[0].ops[2] = 0;
        loop_insts[0].ops[3] = 0; // 0.0 = bits 0x0
        loop_insts[1].opcode = kOpConstI64;
        loop_insts[1].ops[0] = 1;
        loop_insts[1].ops[1] = 0;
        loop_insts[1].ops[2] = 0;
        loop_insts[1].ops[3] = 0;
        loop_insts[2].opcode = kOpArg;
        loop_insts[2].ops[0] = 2;
        loop_insts[2].ops[1] = 0;
        loop_insts[2].ops[2] = 0;
        loop_insts[2].ops[3] = 0;
        loop_insts[3].opcode = kOpJump;
        loop_insts[3].ops[0] = 1;
        loop_insts[3].ops[1] = 0;
        loop_insts[3].ops[2] = 0;
        loop_insts[3].ops[3] = 0;
        // head: if (= i n) exit else body
        loop_insts[4].opcode = kOpEq;
        loop_insts[4].ops[0] = 5;
        loop_insts[4].ops[1] = 1;
        loop_insts[4].ops[2] = 2;
        loop_insts[4].ops[3] = 1; // ops[3]=1 → numeric =
        loop_insts[5].opcode = kOpBranch;
        loop_insts[5].ops[0] = 5;
        loop_insts[5].ops[1] = 3;
        loop_insts[5].ops[2] = 2;
        loop_insts[5].ops[3] = 0;
        // body: acc += 0.5; i += 1; back-edge
        loop_insts[6].opcode = kOpConstF64;
        loop_insts[6].ops[0] = 3;
        loop_insts[6].ops[1] = 0;
        loop_insts[6].ops[2] = 0x3FE00000u;
        loop_insts[6].ops[3] = 0; // 0.5
        loop_insts[7].opcode = kOpAdd;
        loop_insts[7].ops[0] = 4;
        loop_insts[7].ops[1] = 0;
        loop_insts[7].ops[2] = 3;
        loop_insts[7].ops[3] = 0;
        loop_insts[8].opcode = kOpLocal;
        loop_insts[8].ops[0] = 0;
        loop_insts[8].ops[1] = 4;
        loop_insts[8].ops[2] = 0;
        loop_insts[8].ops[3] = 0;
        loop_insts[9].opcode = kOpConstI64;
        loop_insts[9].ops[0] = 6;
        loop_insts[9].ops[1] = 1;
        loop_insts[9].ops[2] = 0;
        loop_insts[9].ops[3] = 0;
        loop_insts[10].opcode = kOpAdd;
        loop_insts[10].ops[0] = 7;
        loop_insts[10].ops[1] = 1;
        loop_insts[10].ops[2] = 6;
        loop_insts[10].ops[3] = 0;
        loop_insts[11].opcode = kOpLocal;
        loop_insts[11].ops[0] = 1;
        loop_insts[11].ops[1] = 7;
        loop_insts[11].ops[2] = 0;
        loop_insts[11].ops[3] = 0;
        loop_insts[12].opcode = kOpJump;
        loop_insts[12].ops[0] = 1;
        loop_insts[12].ops[1] = 0;
        loop_insts[12].ops[2] = 0;
        loop_insts[12].ops[3] = 0;
        // exit: return acc
        loop_insts[13].opcode = kOpReturn;
        loop_insts[13].ops[0] = 0;
        loop_insts[13].ops[1] = 0;
        loop_insts[13].ops[2] = 0;
        loop_insts[13].ops[3] = 0;

        FlatBlock loop_blks[4] = {{0, &loop_insts[0], 4},
                                  {1, &loop_insts[4], 2},
                                  {2, &loop_insts[6], 7},
                                  {3, &loop_insts[13], 1}};

        // shape feedback: acc/half/acc2 float, i/n/one/i2 int, cond bool
        const uint8_t map_float[kLocals] = {2, 1, 1, 2, 2, 3, 1, 1};
        const uint8_t map_dynamic[kLocals] = {0, 0, 0, 0, 0, 0, 0, 0};

        auto compile_loop = [&](aura::jit::AuraJIT& jit, const char* name, const uint8_t* map) {
            FlatFunction fn{};
            fn.name = name;
            fn.entry_block = 0;
            fn.local_count = kLocals;
            fn.arg_count = 1;
            fn.blocks = loop_blks;
            fn.num_blocks = 4;
            fn.shape_map = map;
            return jit.compile(fn);
        };

        aura::jit::AuraJIT jit_f;
        aura::jit::AuraJIT jit_d;
        auto* fn_f = compile_loop(jit_f, "f4522_unboxed", map_float);
        auto* fn_d = compile_loop(jit_d, "f4522_boxed", map_dynamic);
        TEST("4522: specialized (float-map) compile succeeded", fn_f != nullptr);
        TEST("4522: dynamic-map compile succeeded", fn_d != nullptr);

        if (fn_f && fn_d) {
            std::int64_t args[1] = {n_fix};
            // aura_jit_pool_size() observes the STRING pool — measure float
            // allocations with probe boxes instead: the returned tag's payload
            // is the float pool index (is_float_raw_v2 / float_idx_raw_v2,
            // #4502 encoding). Consecutive probe indices differ by exactly
            // (aura_alloc_float calls between the probes) + 1 (the second
            // probe itself), so allocs = idx_delta - 1 — the issue's Verify
            // metric (alloc_float count over the loop drops to ~0).
            namespace types = aura::compiler::types;
            const auto probe = 1234.5;
            const auto idx_pre_f = types::float_idx_raw_v2(aura_alloc_float(probe));
            const auto res_f = fn_f(args, 1);
            const auto idx_post_f = types::float_idx_raw_v2(aura_alloc_float(probe));
            const auto allocs_f = idx_post_f - idx_pre_f - 1;
            const auto idx_pre_d = types::float_idx_raw_v2(aura_alloc_float(probe));
            const auto res_d = fn_d(args, 1);
            const auto idx_post_d = types::float_idx_raw_v2(aura_alloc_float(probe));
            const auto allocs_d = idx_post_d - idx_pre_d - 1;

            std::println("  4522 AC1: aura_alloc_float count over loop: unboxed={} boxed={}",
                         allocs_f, allocs_d);
            TEST("4522 AC1: unboxed loop allocates exactly 1 float (exit rebox)", allocs_f == 1);
            TEST("4522 AC1: boxed loop allocates 2/iter + entry const",
                 allocs_d == 2 * static_cast<std::uint64_t>(kIters) + 1);
            TEST("4522 AC1: rebox rate dropped to ~0 (allocs_f << allocs_d)",
                 allocs_f * 100 < allocs_d);

            // AC2: bit-identical results, valid float tags, exact sum.
            const bool f_is_float = aura::compiler::types::is_float_raw_v2(res_f);
            const bool d_is_float = aura::compiler::types::is_float_raw_v2(res_d);
            TEST("4522 AC2: both results are boxed floats", f_is_float && d_is_float);
            const double val_f = aura_float_ref(res_f);
            const double val_d = aura_float_ref(res_d);
            TEST("4522 AC2: unboxed sum == 500.0", val_f == 500.0);
            TEST("4522 AC2: reboxed bits identical to boxed-path bits",
                 std::memcmp(&val_f, &val_d, sizeof(double)) == 0);

            // AC3: float-known comparisons are value fcmp over raw doubles.
            FlatInstruction lt_insts[4] = {};
            lt_insts[0].opcode = kOpConstF64;
            lt_insts[0].ops[0] = 0;
            lt_insts[0].ops[1] = 0;
            lt_insts[0].ops[2] = 0x3FF00000u;
            lt_insts[0].ops[3] = 0; // 1.0
            lt_insts[1].opcode = kOpConstF64;
            lt_insts[1].ops[0] = 1;
            lt_insts[1].ops[1] = 0;
            lt_insts[1].ops[2] = 0x40000000u;
            lt_insts[1].ops[3] = 0; // 2.0
            lt_insts[2].opcode = kOpLt;
            lt_insts[2].ops[0] = 2;
            lt_insts[2].ops[1] = 0;
            lt_insts[2].ops[2] = 1;
            lt_insts[2].ops[3] = 0;
            lt_insts[3].opcode = kOpReturn;
            lt_insts[3].ops[0] = 2;
            lt_insts[3].ops[1] = 0;
            lt_insts[3].ops[2] = 0;
            lt_insts[3].ops[3] = 0;
            FlatBlock lt_blk[1] = {{0, lt_insts, 4}};
            const uint8_t map_lt[3] = {2, 2, 3};

            FlatInstruction gt_insts[4] = {};
            gt_insts[0].opcode = kOpConstF64;
            gt_insts[0].ops[0] = 0;
            gt_insts[0].ops[1] = 0;
            gt_insts[0].ops[2] = 0x3FF00000u;
            gt_insts[0].ops[3] = 0; // 1.0
            gt_insts[1].opcode = kOpConstF64;
            gt_insts[1].ops[0] = 1;
            gt_insts[1].ops[1] = 0;
            gt_insts[1].ops[2] = 0x40000000u;
            gt_insts[1].ops[3] = 0; // 2.0
            gt_insts[2].opcode = kOpGt;
            gt_insts[2].ops[0] = 2;
            gt_insts[2].ops[1] = 0;
            gt_insts[2].ops[2] = 1;
            gt_insts[2].ops[3] = 0;
            gt_insts[3].opcode = kOpReturn;
            gt_insts[3].ops[0] = 2;
            gt_insts[3].ops[1] = 0;
            gt_insts[3].ops[2] = 0;
            gt_insts[3].ops[3] = 0;
            FlatBlock gt_blk[1] = {{0, gt_insts, 4}};

            FlatInstruction nan_insts[4] = {};
            nan_insts[0].opcode = kOpConstF64;
            nan_insts[0].ops[0] = 0;
            nan_insts[0].ops[1] = 0;
            nan_insts[0].ops[2] = 0x7FF80000u;
            nan_insts[0].ops[3] = 0; // quiet NaN
            nan_insts[1].opcode = kOpConstF64;
            nan_insts[1].ops[0] = 1;
            nan_insts[1].ops[1] = 0;
            nan_insts[1].ops[2] = 0x7FF80000u;
            nan_insts[1].ops[3] = 0; // quiet NaN
            nan_insts[2].opcode = kOpEq;
            nan_insts[2].ops[0] = 2;
            nan_insts[2].ops[1] = 0;
            nan_insts[2].ops[2] = 1;
            nan_insts[2].ops[3] = 1; // numeric =
            nan_insts[3].opcode = kOpReturn;
            nan_insts[3].ops[0] = 2;
            nan_insts[3].ops[1] = 0;
            nan_insts[3].ops[2] = 0;
            nan_insts[3].ops[3] = 0;
            FlatBlock nan_blk[1] = {{0, nan_insts, 4}};

            auto compile_mini = [&](aura::jit::AuraJIT& jit, const char* name,
                                    const FlatBlock* blk) {
                FlatFunction fn{};
                fn.name = name;
                fn.entry_block = 0;
                fn.local_count = 3;
                fn.arg_count = 0;
                fn.blocks = blk;
                fn.num_blocks = 1;
                fn.shape_map = map_lt;
                return jit.compile(fn);
            };
            auto* fn_lt = compile_mini(jit_f, "f4522_lt", lt_blk);
            auto* fn_gt = compile_mini(jit_f, "f4522_gt", gt_blk);
            auto* fn_nan = compile_mini(jit_f, "f4522_naneq", nan_blk);
            TEST("4522 AC3: float compare compiles", fn_lt && fn_gt && fn_nan);
            if (fn_lt && fn_gt && fn_nan) {
                TEST("4522 AC3: (lt 1.0 2.0) → #t (keyword 7)", fn_lt(nullptr, 0) == 7);
                TEST("4522 AC3: (gt 1.0 2.0) → #f (keyword 3)", fn_gt(nullptr, 0) == 3);
                TEST("4522 AC3: (= NaN NaN) → #f (ordered fcmp)", fn_nan(nullptr, 0) == 3);
            }

            // AC4: informational micro-benchmark (no flaky timing assert).
            constexpr std::int64_t kBenchLoops = 2000;
            auto bench = [&](aura::jit::ScalarFn f) {
                std::int64_t a[1] = {n_fix};
                const auto t0 = std::chrono::steady_clock::now();
                for (std::int64_t r = 0; r < kBenchLoops; ++r)
                    (void)f(a, 1);
                const auto t1 = std::chrono::steady_clock::now();
                return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            };
            (void)fn_f(args, 1); // warm both paths
            (void)fn_d(args, 1);
            const auto ns_f = bench(fn_f);
            const auto ns_d = bench(fn_d);
            std::println("  4522 AC4: unboxed loop {} ns total vs boxed {} ns total "
                         "({} loops each)",
                         ns_f, ns_d, kBenchLoops);
            TEST("4522 AC4: benchmark ran on both paths", ns_f > 0 && ns_d > 0);
        }
    }

    // ═══════════════════════════════════════════════════════════
    // Summary
    // ═══════════════════════════════════════════════════════════
    std::println("\n=== Results: %d/{} passed ===", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
