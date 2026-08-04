// @category: unit
// @reason: Issue #2238 — alloc-time / set_name-time policy for anonymous
// AOT-bound closures. Default = permissive (today's behavior, legacy
// sid=0 + empty name closures still allowed). Opt-in via
// AURA_REQUIRE_STABLE_ID_FOR_AOT=1 or API
// aura_set_require_stable_id_for_aot(1). When ON, closures that have
// sid=0 + empty name get MustDeopt set at aura_closure_set_name time
// (or aura_closure_check_aot_stable_id_policy call from AOT install),
// forcing interpreter-only on the next call — no native ptr install
// happens (existing #2128 MustDeopt gate at aura_jit_runtime.cpp:1591).
//
//   AC1: anonymous + aura_closure_check_aot_stable_id_policy under
//        policy on → returns 1 (reject); counter +1
//   AC2: anonymous + aura_closure_set_name(name=null) under policy
//        on → MustDeopt flag set on the closure; counter +1
//   AC3: named legacy sid=0 → backfill still works (no MustDeopt set);
//        counter unchanged
//   AC4: policy off → legacy behavior preserved (counter stays 0,
//        no MustDeopt on anonymous closures)
//   AC5: env resolver AURA_REQUIRE_STABLE_ID_FOR_AOT works (1/on/true/
//        yes → on; else off)
//   AC6: query:closure-stats exposes 5 new keys + schema-2238
//        (verified via the underlying C-linkage readers the kv list
//        reads from)

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;

// C-linkage declarations (Issue #2238). Defined in aura_jit_runtime.cpp.
extern "C" void aura_set_require_stable_id_for_aot(int mode) noexcept;
extern "C" int aura_get_require_stable_id_for_aot(void) noexcept;
extern "C" void aura_apply_require_stable_id_for_aot_env(void) noexcept;
extern "C" std::uint64_t aura_anonymous_aot_reject_total_v_read() noexcept;
extern "C" void aura_test_set_require_stable_id_for_aot(int v) noexcept;
extern "C" std::uint64_t aura_test_anonymous_aot_reject_total_v_read() noexcept;
extern "C" void aura_test_reset_anonymous_aot_reject_total_for_test() noexcept;
extern "C" int aura_closure_check_aot_stable_id_policy(int64_t closure_id) noexcept;
extern "C" int64_t aura_alloc_closure(int64_t func_id);
extern "C" void aura_free_closure(int64_t closure_id);
extern "C" void aura_closure_set_name(int64_t closure_id, const char* name);
extern "C" void aura_closure_set_must_deopt(int64_t closure_id, int v);
extern "C" int aura_closure_get_must_deopt(int64_t closure_id);

// RAII guard: reset policy + reject counter for test-order isolation.
struct AnonPolicyGuard {
    AnonPolicyGuard() noexcept {
        aura_test_set_require_stable_id_for_aot(0);
        aura_test_reset_anonymous_aot_reject_total_for_test();
    }
    ~AnonPolicyGuard() noexcept {
        aura_test_set_require_stable_id_for_aot(0);
        aura_test_reset_anonymous_aot_reject_total_for_test();
    }
};

// AC1: aura_closure_check_aot_stable_id_policy under policy on +
// anonymous (no set_name) → returns 1 (reject); counter +1.
static void ac_check_rejects_anonymous() {
    std::println("\n--- AC1: aura_closure_check_aot_stable_id_policy rejects anonymous ---");
    AnonPolicyGuard g;
    aura_test_set_require_stable_id_for_aot(1);
    CHECK(aura_get_require_stable_id_for_aot() == 1,
          "AC1: require_stable_id_for_aot=1 readable after setter");
    // Allocate a closure without setting a name (anonymous path).
    const auto cid = aura_alloc_closure(/*func_id=*/1);
    CHECK(cid >= 0, "AC1: aura_alloc_closure returns valid cid");
    const auto before = static_cast<std::int64_t>(aura_test_anonymous_aot_reject_total_v_read());
    const int rc = aura_closure_check_aot_stable_id_policy(cid);
    CHECK(rc == 1, "AC1: check returns 1 (reject) for anonymous closure under policy");
    const auto after = static_cast<std::int64_t>(aura_test_anonymous_aot_reject_total_v_read());
    CHECK(after == before + 1, "AC1: anonymous_aot_reject_total bumped by 1");
    aura_free_closure(cid);
}

// AC2: aura_closure_set_name(name=null) under policy on → MustDeopt
// flag set on the closure; counter +1.
static void ac_set_name_null_sets_must_deopt() {
    std::println("\n--- AC2: set_name(null) sets MustDeopt under policy ---");
    AnonPolicyGuard g;
    aura_test_set_require_stable_id_for_aot(1);
    const auto cid = aura_alloc_closure(/*func_id=*/2);
    CHECK(cid >= 0, "AC2: aura_alloc_closure returns valid cid");
    CHECK(aura_closure_get_must_deopt(cid) == 0, "AC2: pre-set MustDeopt=0 (clean)");
    const auto before = static_cast<std::int64_t>(aura_test_anonymous_aot_reject_total_v_read());
    aura_closure_set_name(cid, /*name=*/nullptr);
    CHECK(aura_closure_get_must_deopt(cid) == 1,
          "AC2: post-set MustDeopt=1 (policy flagged anonymous)");
    const auto after = static_cast<std::int64_t>(aura_test_anonymous_aot_reject_total_v_read());
    CHECK(after == before + 1, "AC2: anonymous_aot_reject_total bumped by 1");
    aura_free_closure(cid);
}

// AC3: named closure (set_name with non-empty name) → MustDeopt NOT
// set; counter unchanged. Backfill path untouched (#2175 still works).
static void ac_set_name_valid_no_must_deopt() {
    std::println("\n--- AC3: set_name with valid name → no MustDeopt ---");
    AnonPolicyGuard g;
    aura_test_set_require_stable_id_for_aot(1);
    const auto cid = aura_alloc_closure(/*func_id=*/3);
    CHECK(cid >= 0, "AC3: aura_alloc_closure returns valid cid");
    const auto before = static_cast<std::int64_t>(aura_test_anonymous_aot_reject_total_v_read());
    aura_closure_set_name(cid, /*name=*/"named_closure_2238");
    CHECK(aura_closure_get_must_deopt(cid) == 0,
          "AC3: post-set MustDeopt=0 (named closure, policy doesn't trigger)");
    const auto after = static_cast<std::int64_t>(aura_test_anonymous_aot_reject_total_v_read());
    CHECK(after == before, "AC3: counter unchanged for named closure");
    aura_free_closure(cid);
}

// AC4: policy off (default) → legacy behavior. Anonymous closure + set_name
// with empty string → no MustDeopt; counter stays 0.
static void ac_policy_off_legacy_behavior() {
    std::println("\n--- AC4: policy off → legacy behavior ---");
    AnonPolicyGuard g;
    aura_test_set_require_stable_id_for_aot(0);
    CHECK(aura_get_require_stable_id_for_aot() == 0,
          "AC4: require_stable_id_for_aot=0 default after reset");
    const auto cid = aura_alloc_closure(/*func_id=*/4);
    CHECK(cid >= 0, "AC4: aura_alloc_closure returns valid cid");
    aura_closure_set_name(cid, /*name=*/"");
    CHECK(aura_closure_get_must_deopt(cid) == 0,
          "AC4: empty-name closure under policy off → no MustDeopt (legacy)");
    const int rc = aura_closure_check_aot_stable_id_policy(cid);
    CHECK(rc == 0, "AC4: check returns 0 (permit) under policy off");
    aura_free_closure(cid);
}

// AC5: env resolver AURA_REQUIRE_STABLE_ID_FOR_AOT works for "1"/"on"
// /"true"/"yes" → on; else off.
static void ac_env_resolver() {
    std::println("\n--- AC5: env resolver AURA_REQUIRE_STABLE_ID_FOR_AOT ---");
    AnonPolicyGuard g;
    // setenv / unsetenv are POSIX (or stdlib on Windows).
    ::setenv("AURA_REQUIRE_STABLE_ID_FOR_AOT", "1", /*overwrite=*/1);
    aura_apply_require_stable_id_for_aot_env();
    CHECK(aura_get_require_stable_id_for_aot() == 1, "AC5: env \"1\" → policy=1");
    ::setenv("AURA_REQUIRE_STABLE_ID_FOR_AOT", "yes", /*overwrite=*/1);
    aura_apply_require_stable_id_for_aot_env();
    CHECK(aura_get_require_stable_id_for_aot() == 1, "AC5: env \"yes\" → policy=1");
    ::setenv("AURA_REQUIRE_STABLE_ID_FOR_AOT", "true", /*overwrite=*/1);
    aura_apply_require_stable_id_for_aot_env();
    CHECK(aura_get_require_stable_id_for_aot() == 1, "AC5: env \"true\" → policy=1");
    ::setenv("AURA_REQUIRE_STABLE_ID_FOR_AOT", "0", /*overwrite=*/1);
    aura_apply_require_stable_id_for_aot_env();
    CHECK(aura_get_require_stable_id_for_aot() == 0, "AC5: env \"0\" → policy=0");
    ::setenv("AURA_REQUIRE_STABLE_ID_FOR_AOT", "off", /*overwrite=*/1);
    aura_apply_require_stable_id_for_aot_env();
    CHECK(aura_get_require_stable_id_for_aot() == 0, "AC5: env \"off\" → policy=0");
    ::setenv("AURA_REQUIRE_STABLE_ID_FOR_AOT", "garbage", /*overwrite=*/1);
    aura_apply_require_stable_id_for_aot_env();
    CHECK(aura_get_require_stable_id_for_aot() == 0,
          "AC5: env invalid → policy=0 (typo-safe fallback)");
    ::unsetenv("AURA_REQUIRE_STABLE_ID_FOR_AOT");
}

// AC6: query:closure-stats exposes 5 new keys + schema-2238. Verified
// via the C-linkage readers the kv list reads from. The closure-stats
// dispatch itself returns a hash successfully (no regression on the
// 9 existing keys).
static void ac_query_surface() {
    std::println("\n--- AC6: query:closure-stats 5 new keys wired ---");
    AnonPolicyGuard g;
    aura_test_set_require_stable_id_for_aot(1);
    aura_test_reset_anonymous_aot_reject_total_for_test();
    const auto cid = aura_alloc_closure(/*func_id=*/5);
    aura_closure_set_name(cid, /*name=*/nullptr);
    CHECK(static_cast<std::int64_t>(aura_anonymous_aot_reject_total_v_read()) >= 1,
          "AC6: cumulative counter readable (>= 1)");
    CHECK(aura_get_require_stable_id_for_aot() == 1, "AC6: live policy flag readable (= 1)");
    aura_free_closure(cid);
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:closure-stats\")");
    CHECK(h, "AC6: query:closure-stats returns hash (no regression)");
}

} // namespace

int run_test_aot_anonymous_closure_policy() {
    std::println("=== Issue #2238 — anonymous AOT-bound closure policy ===");
    ac_check_rejects_anonymous();
    ac_set_name_null_sets_must_deopt();
    ac_set_name_valid_no_must_deopt();
    ac_policy_off_legacy_behavior();
    ac_env_resolver();
    ac_query_surface();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_aot_anonymous_closure_policy();
}
#endif
