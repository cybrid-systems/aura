// @category: unit
// @reason: Issue #3136 — relower-success-path bitmap coherence. After any
// successful relower that restamps the IR cache entry (store_ir_cache_v2 /
// partial peel / per-fn partial / cascade-reemit / test path), the producer
// stamps the just-restamped define's region bit into
// HotUpdateRegistry::last_reemit_success_region_mask_ so the existing
// `residual_force_mask() = force & ~last_success` shrinks for the covered
// region. Closes the success-path authority split between IR cache stamp and
// registry residual force (orthogonal to #3129 entry-path completion).
//
//   AC1: restamp_cache_entry_for_test(name) flips the bit for that name
//        under production defaults (test path runs with probe on).
//   AC2: Same name twice → mask monotonic (bit stays set; no shrink).
//   AC3: Distinct names → coverage grows (union only).
//   AC4: residual_force_mask() strictly shrinks after a single named
//        restamp on a fully-stamped force mask (all 64 bits).
//   AC5: Soft / Off zero-cost verification is at the source level — see
//        scripts/check_relower_success_coverage_3136.py which asserts
//        each call site has the inline `aura_production_defaults_active
//        _probe() != 0` gate before note_relower_success_coverage.

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

extern "C" {
std::uint64_t aura_hot_update_force_jit_stamp_for_test(std::uint64_t mask);
std::uint64_t aura_hot_update_residual_force_mask(void);
std::uint64_t aura_hot_update_last_reemit_success_region_mask(void);
}

namespace {

using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;

// AC1: Production + restamp → bit for that name flips in last_reemit_success_region_mask.
static void ac1_restamp_flips_bit(CompilerService& cs) {
    aura_hot_update_force_jit_stamp_for_test(0);
    const auto before = aura_hot_update_last_reemit_success_region_mask();
    const bool ok = cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac1");
    CHECK(ok, "AC1: restamp_cache_entry_for_test returned true");
    const auto after = aura_hot_update_last_reemit_success_region_mask();
    CHECK(after != before, "AC1: last_reemit_success_region_mask changed after restamp (bit "
                           "flipped for named define)");
}

// AC2: Monotonic — second restamp of same name does not shrink the mask.
static void ac2_monotonic_same_name(CompilerService& cs) {
    aura_hot_update_force_jit_stamp_for_test(0);
    cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac2");
    const auto after_first = aura_hot_update_last_reemit_success_region_mask();
    cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac2");
    const auto after_second = aura_hot_update_last_reemit_success_region_mask();
    CHECK((after_second & after_first) == after_first,
          "AC2: mask is monotonic — second restamp of same name does not shrink coverage");
}

// AC3: Distinct names → coverage grows (union only, never shrinks).
static void ac3_distinct_names_grow(CompilerService& cs) {
    aura_hot_update_force_jit_stamp_for_test(0);
    cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac3_a");
    const auto after_first = aura_hot_update_last_reemit_success_region_mask();
    cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac3_b");
    const auto after_second = aura_hot_update_last_reemit_success_region_mask();
    CHECK((after_second | after_first) == after_second,
          "AC3: mask coverage grows (union-only) for distinct named defines");
}

// AC4: residual_force_mask = force & ~last_success strictly shrinks after
// a single named restamp on a fully-stamped force mask (all 64 bits).
static void ac4_residual_shrinks(CompilerService& cs) {
    aura_hot_update_force_jit_stamp_for_test(0xFFFFFFFFFFFFFFFFULL);
    const auto residual_before = aura_hot_update_residual_force_mask();
    cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac4");
    const auto residual_after = aura_hot_update_residual_force_mask();
    CHECK(residual_after < residual_before,
          "AC4: residual_force_mask strictly shrinks after restamp on full force mask");
}

} // namespace

int run_test_hot_update_relower_success_coverage() {
    CompilerService cs;
    std::print("[test_hot_update_relower_success_coverage] running 4 ACs\n");

    ac1_restamp_flips_bit(cs);
    ac2_monotonic_same_name(cs);
    ac3_distinct_names_grow(cs);
    ac4_residual_shrinks(cs);

    std::print("[test_hot_update_relower_success_coverage] passed={} failed={}\n", g_passed,
               g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hot_update_relower_success_coverage();
}
#endif
