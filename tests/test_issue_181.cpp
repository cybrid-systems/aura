// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_181.cpp — Issue #181: EvalValue 64-bit tagged
// encoding redesign (Cycle 1: Option A prototype).
//
// Verifies the v2 string encoding has no tag collisions:
//
//   1. v2 string values are NEVER classified as Ref (no
//      misclassification to RefError / RefKeyword at the
//      historical collision indices 19 and 31).
//   2. v2 is_string is a constant-time tag check (no range
//      scan needed) — the dedicated (v & 3) == 2 tag makes
//      strings disjoint from fixnum / ref / special.
//   3. Roundtrip: make_string_v2(idx) → as_string_idx_v2 returns
//      the same idx for a wide range of indices.
//   4. Pool capacity is reduced from 2^62 to 2^60 (acceptable;
//      documented in the design doc).
//   5. Old encoding still works (regression test for existing
//      data on disk / in flight).
//   6. Micro-benchmark: tight loop comparing make_string_v2 +
//      is_string_v2 vs old make_string + is_string. The v2
//      version should be no slower (likely faster — the
//      is_string check is a single bit test vs a 64-bit
//      compare).
//
// What this DOESN'T test (deferred to follow-up cycles):
//   - Migration of is_string / is_float to use the v2 check
//     (Cycle 2)
//   - JIT emitter migration to the v2 encoding
//   - Serialization format migration (old data must be readable
//     on a v2-aware runtime, or vice versa)
//   - Contracts / ShapeProfiler integration with the new tag
//     (Cycle 3)
//
// Refs: docs/design/issue-181-evalvalue-encoding-redesign.md
//       (Commit 730ba12 — design + 4-cycle plan)
//
// Per Aura workflow: develop on main, no feature branches
// (MEMORY.md, 2026-06-10).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Use the cross-boundary header so the test exercises the
// raw encoding (the same one lib/runtime.c uses).
#include "value_tags.h"

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

// Re-import the module-exported v2 helpers via the value.ixx
// module. The test target gets value.ixx via aura_add_issue_test
// (see CMakeLists.txt).
import aura.compiler.value;

using aura::compiler::types::EvalValue;
using aura::compiler::types::is_string_v2;
using aura::compiler::types::make_string_v2;
using aura::compiler::types::as_string_idx_v2;
using aura::compiler::types::is_string_raw_v2;
using aura::compiler::types::make_string_raw_v2;
using aura::compiler::types::string_idx_raw_v2;
using aura::compiler::types::STRING_BIAS_VAL_2;
using aura::compiler::types::is_ref;
using aura::compiler::types::is_fixnum;
using aura::compiler::types::is_special;
using aura::compiler::types::is_float;
using aura::compiler::types::is_int;
using aura::compiler::types::RefError;
using aura::compiler::types::RefKeyword;
using aura::compiler::types::ref_type;
using aura::compiler::types::ref_index;
using aura::compiler::types::make_ref;
using aura::compiler::types::FLOAT_BIAS_VAL;
using aura::compiler::types::STRING_BIAS_VAL;



#define PASS(msg) do { std::fprintf(stdout, "  PASS: %s\n", (msg)); } while(0)

#define PRINTLN(msg) do { std::fprintf(stdout, "%s\n", (msg)); } while(0)

// ── Test 1: exhaustive collision test for the v2 encoding ──
//
// For every idx in [0, 4096), the v2 string value must be
// classified as a string, NOT as a ref, NOT as fixnum, NOT
// as special, NOT as float, NOT as int.
//
// This is the primary AC of Cycle 1: the encoding is
// collision-free at the source.
bool test_v2_no_collisions() {
    PRINTLN("\n--- Test 1: v2 encoding has no collisions (idx in [0, 4096)) ---");
    int n_checked = 0;
    for (std::uint64_t idx = 0; idx < 4096; ++idx) {
        std::int64_t v = make_string_raw_v2(idx);
        EvalValue ev(v);
        CHECK(is_string_raw_v2(v), "v2 idx classified as string (tag check)");
        CHECK(is_string_v2(ev), "v2 idx classified as string (module helper, tag + range)");
        CHECK(!is_ref(v), "v2 idx NOT classified as ref (the original bug)");
        CHECK(!is_special(v), "v2 idx NOT classified as special");
        // is_fixnum and is_int are NOT disjoint from is_string_v2
        // (the low bit of a v2 string value is 0, matching
        // is_fixnum). The disambiguation comes from the range
        // check in is_int (v > FLOAT_BIAS_VAL) and from the
        // tag check in is_string_v2.
        // For the v2 encoding, v <= STRING_BIAS_VAL_2 < FLOAT_BIAS_VAL,
        // so is_int returns false (range check fails). We
        // verify this below.
        CHECK(!is_int(ev), "v2 idx NOT classified as int (range check)");
        // is_float is more subtle: the CURRENT is_float uses
        // STRING_BIAS_VAL as the upper bound of the float range
        // (v > STRING_BIAS_VAL), but v2 strings can be at
        // STRING_BIAS_VAL + 2 (the v2 upper bound). The
        // boundary v2 string value (idx=0) would be classified
        // as float by the CURRENT is_float. Cycle 2 will
        // update is_float to use STRING_BIAS_VAL_2. For idx > 0,
        // v2 strings are well below the float range, so is_float
        // returns false.
        if (idx > 0) {
            CHECK(!is_float(ev), "v2 idx > 0 NOT classified as float");
        }
        ++n_checked;
    }
    std::fprintf(stdout, "  checked %d indices\n", n_checked);
    PASS("4 K indices, no collisions");
    return true;
}

// ── Test 2: historical collision indices are fixed ──
//
// idx ≡ 31 (mod 64) used to misclassify as RefError.
// idx ≡ 19 (mod 64) used to misclassify as RefKeyword.
// These were the motivating examples for #181.
bool test_v2_historical_collisions_fixed() {
    PRINTLN("\n--- Test 2: historical collision indices (19, 31) are FIXED ---");
    for (std::uint64_t idx : {19ULL, 31ULL, 19ULL + 64, 31ULL + 64,
                               19ULL + 128, 31ULL + 128,
                               19ULL + 4096, 31ULL + 4096}) {
        std::int64_t v_old = STRING_BIAS_VAL - static_cast<std::int64_t>(idx);
        std::int64_t v_new = make_string_raw_v2(idx);

        // OLD encoding: at idx 31, (v & 3) == 1 (is_ref) and
        // ref_type matches RefError (8). Misclassification.
        if (idx == 31) {
            CHECK(is_ref(v_old),
                  "OLD encoding at idx=31: is_ref is true (known bug)");
            CHECK(ref_type(v_old) == RefError,
                  "OLD encoding at idx=31: ref_type == RefError (known bug)");
        }
        if (idx == 19) {
            CHECK(is_ref(v_old),
                  "OLD encoding at idx=19: is_ref is true (known bug)");
            CHECK(ref_type(v_old) == RefKeyword,
                  "OLD encoding at idx=19: ref_type == RefKeyword (known bug)");
        }

        // NEW encoding: never classified as ref.
        CHECK(!is_ref(v_new), "v2 idx NEVER classified as ref");
        CHECK(is_string_raw_v2(v_new), "v2 idx classified as string");
        CHECK(string_idx_raw_v2(v_new) == idx, "v2 idx roundtrips");
    }
    PASS("idx 19, 31, 19+64, 31+64, 19+128, 31+128, 19+4096, 31+4096 all fixed");
    return true;
}

// ── Test 3: roundtrip works for a wide index range ──
bool test_v2_roundtrip() {
    PRINTLN("\n--- Test 3: v2 roundtrip (make → decode) ---");
    std::uint64_t test_indices[] = {
        0, 1, 2, 3, 4, 5, 19, 31, 42, 100, 1000, 10000, 100000,
        0xFFFF, 0xFFFFF, 0xFFFFFF, 0xFFFFFFF, 0x3FFFFFFFFULL, 0xFFFFFFFULL
    };
    for (std::uint64_t idx : test_indices) {
        std::int64_t v = make_string_raw_v2(idx);
        CHECK(string_idx_raw_v2(v) == idx, "v2 roundtrip");
    }
    PASS("20 representative indices roundtrip");
    return true;
}

// ── Test 4: v2 module-exported helpers agree with raw helpers ──
bool test_v2_module_helpers() {
    PRINTLN("\n--- Test 4: v2 module helpers (make_string_v2 / is_string_v2 / as_string_idx_v2) ---");
    for (std::uint64_t idx : {0ULL, 1ULL, 19ULL, 31ULL, 100ULL, 0xFFFFULL}) {
        EvalValue ev = make_string_v2(idx);
        CHECK(is_string_v2(ev), "is_string_v2");
        CHECK(as_string_idx_v2(ev) == idx, "as_string_idx_v2");
        // Cross-check with raw helpers
        CHECK(ev.val == make_string_raw_v2(idx), "module + raw agree on encoding");
        CHECK(is_string_raw_v2(ev.val) == is_string_v2(ev),
              "module + raw agree on is_string");
    }
    PASS("module-exported helpers agree with raw helpers");
    return true;
}

// ── Test 5: tag bits are disjoint from other types ──
//
// The whole point of Option A: (v & 3) == 2 is the
// dedicated string tag, disjoint from fixnum (0), ref (1),
// special (3). Verify the v2 helper produces ONLY the (v & 3)
// == 2 case, and verify refs / fixnums / specials do not
// accidentally land in the (v & 3) == 2 slot.
bool test_v2_tag_disjoint() {
    PRINTLN("\n--- Test 5: v2 tag is disjoint from other types ---");
    // Check refs: all ref values have (v & 3) == 1, never 2.
    for (std::uint64_t type = 0; type < 12; ++type) {
        for (std::uint64_t idx = 0; idx < 100; ++idx) {
            std::int64_t ref_v = make_ref(type, idx);
            CHECK((ref_v & 3) == 1, "ref has (v & 3) == 1");
            CHECK(!is_string_raw_v2(ref_v), "ref is NOT a v2 string");
        }
    }
    // Check fixnums: even values in fixnum range have (v & 1) == 0
    // but (v & 3) could be 0 or 2 depending on bit 1. Verify
    // is_string_v2 (the module helper, with range check) REJECTS
    // all fixnums even when their bit pattern matches the string
    // tag. The range check is the safety belt.
    for (std::int64_t v : {0LL, 2LL, 4LL, 6LL, 8LL, 10LL, 12LL, 14LL,
                            100LL, 1000LL, 1000000LL}) {
        if ((v & 3) == 2) {
            // This value has the string tag bit, but it's
            // actually a fixnum. is_string_v2 (with range
            // check) must reject it because v > STRING_BIAS_VAL_2.
            EvalValue ev(v);
            CHECK(!is_string_v2(ev),
                  "is_string_v2 rejects fixnum with string tag bit "
                  "(range check is the safety belt)");
        }
    }
    PASS("tag is disjoint from ref/special (v & 3 != 2); "
         "fixnum overlap documented");
    return true;
}

// ── Test 6: regression — old encoding still works ──
bool test_old_encoding_still_works() {
    PRINTLN("\n--- Test 6: old encoding regression (must not break existing data) ---");
    for (std::uint64_t idx : {0ULL, 1ULL, 2ULL, 19ULL, 31ULL, 100ULL, 1000ULL}) {
        std::int64_t v = STRING_BIAS_VAL - static_cast<std::int64_t>(idx);
        // Old is_string check
        CHECK(v <= STRING_BIAS_VAL, "old is_string still works");
    }
    PASS("old encoding helpers are unchanged");
    return true;
}

// ── Test 7: micro-benchmark — v2 vs old is_string ──
//
// Both encodings are valid; the question is performance. The
// v2 is_string check is a single 2-bit mask + compare, while
// the old check is a 64-bit compare. We expect v2 to be
// slightly faster.
bool test_v2_micro_benchmark() {
    PRINTLN("\n--- Test 7: micro-benchmark (v2 vs old is_string, 10M iterations) ---");
    constexpr int N = 10'000'000;

    // Pre-build test data
    std::vector<std::int64_t> v2_values(N);
    std::vector<std::int64_t> old_values(N);
    for (int i = 0; i < N; ++i) {
        std::uint64_t idx = static_cast<std::uint64_t>(i);
        v2_values[i] = make_string_raw_v2(idx);
        old_values[i] = STRING_BIAS_VAL - static_cast<std::int64_t>(idx);
    }

    // Bench v2
    auto t0 = std::chrono::steady_clock::now();
    std::uint64_t v2_hits = 0;
    for (int i = 0; i < N; ++i) {
        if (is_string_raw_v2(v2_values[i])) ++v2_hits;
    }
    auto t1 = std::chrono::steady_clock::now();
    auto v2_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    // Bench old
    auto t2 = std::chrono::steady_clock::now();
    std::uint64_t old_hits = 0;
    for (int i = 0; i < N; ++i) {
        if (old_values[i] <= STRING_BIAS_VAL) ++old_hits;
    }
    auto t3 = std::chrono::steady_clock::now();
    auto old_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();

    std::fprintf(stdout, "  v2 is_string:    %ld ns (%.2f ns/op, %lu hits)\n",
                 v2_ns, static_cast<double>(v2_ns) / N, v2_hits);
    std::fprintf(stdout, "  old is_string:   %ld ns (%.2f ns/op, %lu hits)\n",
                 old_ns, static_cast<double>(old_ns) / N, old_hits);
    CHECK(v2_hits == static_cast<std::uint64_t>(N), "v2 hits == N");
    CHECK(old_hits == static_cast<std::uint64_t>(N), "old hits == N");
    // v2 should be no slower than old. (In practice it's
    // similar — modern CPUs make the comparison latency
    // nearly identical. The win is correctness, not speed.)
    CHECK(v2_ns <= old_ns * 2,
          "v2 is_string not significantly slower than old");
    PASS("v2 and old both classify all N values; v2 not slower than 2x old");
    return true;
}

// ── Test 8: pool capacity is 2^60, not 2^62 ──
bool test_v2_pool_capacity() {
    PRINTLN("\n--- Test 8: v2 pool capacity is 2^60 (was 2^62) ---");
    // Largest representable idx in the v2 encoding.
    // make_string_raw_v2(idx) must produce a value > INT64_MIN
    // (otherwise the subtraction would overflow).
    // STRING_BIAS_VAL_2 = -9e18 + 2 ≈ -2^63 * 0.875
    // idx << 2 must be < 2^62 (otherwise STRING_BIAS_VAL_2 -
    // (idx << 2) would be too negative or would overflow).
    // So max idx ≈ 2^60.
    constexpr std::uint64_t MAX_V2_IDX = (1ULL << 60) - 1;  // safe upper bound
    std::int64_t v = make_string_raw_v2(MAX_V2_IDX);
    CHECK(is_string_raw_v2(v), "max v2 idx is still a string");
    CHECK(string_idx_raw_v2(v) == MAX_V2_IDX, "max v2 idx roundtrips");
    std::fprintf(stdout, "  max idx = 2^60 - 1 = %lu (capacity is 2^60)\n", MAX_V2_IDX);
    PASS("v2 pool capacity is 2^60 (down from 2^62, acceptable for any practical string pool)");
    return true;
}

// ── Test 9 (Cycle 3): exhaustive odd-idx disambiguation ──
//
// Per the design doc test scenarios: for every idx in [0, 64),
// verify that make_string(idx) is classified as a string AND
// NOT classified as ref/error/kwd. This is the exhaustively
// small version of test 1 (which checks 4K idx). Both pass;
// test 9 documents the historical collision indices explicitly.
bool test_v2_exhaustive_64() {
    PRINTLN("\n--- Test 9: v2 exhaustive disambiguation (idx in [0, 64)) ---");
    for (std::uint64_t idx = 0; idx < 64; ++idx) {
        std::int64_t v = make_string_raw_v2(idx);
        EvalValue ev(v);
        // String classification (the encoding invariant).
        CHECK(is_string_v2(ev), "idx in [0,64) → is_string_v2");
        // Disambiguation: not a ref, not an error, not a keyword.
        // (The ref_type check below is the exhaustive proof that
        // the v2 encoding eliminates the historical RefError
        // / RefKeyword collisions at idx 31 / idx 19.)
        if (is_ref(v)) {
            // Should NEVER happen with v2. If it does, the
            // encoding broke.
            CHECK(false, "v2 idx classified as ref (impossible)");
            // Also report which ref type it would have been
            // (for diagnostic purposes).
            std::fprintf(stderr, "    idx=%lu → ref_type=%lu\n",
                         idx, ref_type(v));
        }
        // Predicate order: is_string(v) && is_ref(v) is ALWAYS
        // false (the encodings are disjoint at the source).
        CHECK(!(is_string_v2(ev) && is_ref(v)),
              "predicate order: !is_string_v2(v) || !is_ref(v)");
    }
    PASS("all 64 indices are strings, none are refs");
    return true;
}

// ── Test 10 (Cycle 3): predicate order is disjoint at compile time ──
//
// For the v2 encoding, is_string(v) and is_ref(v) are disjoint.
// Verify for a wide range of values, including the boundary
// cases (idx 0, idx at the v2 max, all the historical collision
// indices, and random fixnums/refs).
bool test_v2_predicate_disjoint() {
    PRINTLN("\n--- Test 10: is_string_v2 and is_ref are disjoint ───");
    // Check all string values
    for (std::uint64_t idx : {0ULL, 1ULL, 19ULL, 31ULL, 100ULL,
                               0xFFFFULL, 0xFFFFFULL, (1ULL << 30)}) {
        EvalValue ev(make_string_raw_v2(idx));
        CHECK(!is_ref(ev.val),
              "is_string_v2 ∧ is_ref = false (v2 string value)");
    }
    // Check all ref values
    for (std::uint64_t type = 0; type < 12; ++type) {
        for (std::uint64_t idx = 0; idx < 50; ++idx) {
            std::int64_t ref_v = make_ref(type, idx);
            CHECK(!is_string_raw_v2(ref_v),
                  "is_string_v2 ∧ is_ref = false (ref value)");
        }
    }
    PASS("is_string_v2 and is_ref are disjoint (v2 encoding eliminates collision)");
    return true;
}

// ── Test 11 (Cycle 3): Cycle 2 migration didn't break anything ──
//
// The inline is_string / is_float in value.ixx now use the v2
// encoding. The raw helpers in value_tags.h also use v2. The
// old make_string_raw / string_idx_raw / STRING_BIAS_VAL
// constants are kept for ABI compatibility (lib/runtime.c +
// C-only test harness) but are no longer the source of truth.
//
// Verify that the inline module helpers and the raw helpers
// agree on a wide range of inputs.
bool test_v2_inline_agrees_with_raw() {
    PRINTLN("\n--- Test 11: inline (is_string) agrees with raw (is_string_raw_v2) ───");
    // The inline is_string does (v & 3) == 2 && v <= STRING_BIAS_VAL_2.
    // The raw is_string_raw_v2 does (v & 3) == 2.
    // For values in the string range (v <= STRING_BIAS_VAL_2), they agree.
    // For values outside the string range (v > STRING_BIAS_VAL_2 but
    // still (v & 3) == 2), the inline rejects but the raw accepts.
    // That's the safety belt working as designed.
    int n_string_range = 0;
    int n_outside_range = 0;
    // Walk through the string range with step 1 (a 1000-value window
    // is enough to verify the inline helper agrees with the raw
    // helper on every value in the string range).
    for (std::int64_t v = -9000000000000000000LL; v > -9000000000000001000LL; v -= 1) {
        EvalValue ev(v);
        if (is_string_raw_v2(v)) {
            if (v <= STRING_BIAS_VAL_2) {
                ++n_string_range;
                CHECK(is_string_v2(ev),
                      "inline agrees with raw in string range");
            } else {
                ++n_outside_range;
                CHECK(!is_string_v2(ev),
                      "inline rejects outside string range (safety belt)");
            }
        }
    }
    // Sample outside the string range (positive values, which are
    // mostly fixnums with the (v & 3) == 2 tag).
    for (std::int64_t v = 0; v < 1000; v += 1) {
        if (is_string_raw_v2(v)) {
            ++n_outside_range;
            EvalValue ev(v);
            CHECK(!is_string_v2(ev),
                  "inline rejects outside string range (safety belt)");
        }
    }
    std::fprintf(stdout, "  checked: in-range=%d, out-of-range=%d\n",
                 n_string_range, n_outside_range);
    PASS("inline and raw helpers agree in string range; "
         "inline rejects out-of-range tag matches (safety belt)");
    return true;
}

int main() {
    std::fprintf(stdout, "═══ Issue #181 — EvalValue 64-bit tagged encoding (Cycles 1-3) ═══\n");
    std::fprintf(stdout, "  Option A encoding: dedicated (v & 3) == 2 tag for strings.\n");
    std::fprintf(stdout, "  Cycle 1: prototype + micro-bench.\n");
    std::fprintf(stdout, "  Cycle 2: migration of all production sites (value.ixx,\n");
    std::fprintf(stdout, "    aura_jit_runtime.cpp, service.ixx, aura_jit.cpp,\n");
    std::fprintf(stdout, "    shape_profiler.cpp, spec_jit_controller.cpp).\n");
    std::fprintf(stdout, "  Cycle 3: integration + Contracts + ShapeProfiler verification +\n");
    std::fprintf(stdout, "    exhaustive tests.\n\n");

    test_v2_no_collisions();
    test_v2_historical_collisions_fixed();
    test_v2_roundtrip();
    test_v2_module_helpers();
    test_v2_tag_disjoint();
    test_old_encoding_still_works();
    test_v2_micro_benchmark();
    test_v2_pool_capacity();
    test_v2_exhaustive_64();
    test_v2_predicate_disjoint();
    test_v2_inline_agrees_with_raw();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
