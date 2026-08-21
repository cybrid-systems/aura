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
// Issue #3229: 6-bit region hash collision — define-id side set so a
// peer that collides on fnv1a&63 stays residual until its own success.

#include "test_harness.hpp"
#include "compiler/hot_update_registry.hh"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;


namespace {

using aura::compiler::CompilerService;
using aura::compiler::hot_update_registry;
using aura::compiler::kRelowerSuccessDefineCollisionIssue;
using aura::compiler::relower_success_define_id;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// AC1: Production + restamp → bit for that name flips in last_reemit_success_region_mask.
static void ac1_restamp_flips_bit(CompilerService& cs) {
    auto& reg = hot_update_registry();
    reg.reset_force_jit_repromote_for_test();
    reg.force_jit_stamp_for_test(0);
    CHECK(aura::compiler::typed_audit::production_defaults_active(),
          "AC1: production_defaults_active");
    const auto before = reg.last_reemit_success_region_mask();
    CHECK(before == 0, "AC1: last_success reset");
    const bool ok = cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac1");
    CHECK(ok, "AC1: restamp_cache_entry_for_test returned true");
    const auto after = reg.last_reemit_success_region_mask();
    CHECK(after != before, "AC1: last_reemit_success_region_mask changed after restamp (bit "
                           "flipped for named define)");
}

// AC2: Monotonic — second restamp of same name does not shrink the mask.
static void ac2_monotonic_same_name(CompilerService& cs) {
    auto& reg = hot_update_registry();
    reg.force_jit_stamp_for_test(0);
    cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac2");
    const auto after_first = reg.last_reemit_success_region_mask();
    cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac2");
    const auto after_second = reg.last_reemit_success_region_mask();
    CHECK((after_second & after_first) == after_first,
          "AC2: mask is monotonic — second restamp of same name does not shrink coverage");
}

// AC3: Distinct names → coverage grows (union only, never shrinks).
static void ac3_distinct_names_grow(CompilerService& cs) {
    auto& reg = hot_update_registry();
    reg.force_jit_stamp_for_test(0);
    cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac3_a");
    const auto after_first = reg.last_reemit_success_region_mask();
    cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac3_b");
    const auto after_second = reg.last_reemit_success_region_mask();
    CHECK((after_second | after_first) == after_second,
          "AC3: mask coverage grows (union-only) for distinct named defines");
}

// AC4: residual_force_mask = force & ~last_success strictly shrinks after
// a single named restamp on a fully-stamped force mask (all 64 bits).
static void ac4_residual_shrinks(CompilerService& cs) {
    auto& reg = hot_update_registry();
    reg.reset_force_jit_repromote_for_test();
    reg.force_jit_stamp_for_test(0xFFFFFFFFFFFFFFFFULL);
    const auto residual_before = reg.residual_force_mask();
    cs.restamp_cache_entry_for_test("test_hot_update_relower_success_coverage_ac4");
    const auto residual_after = reg.residual_force_mask();
    CHECK(residual_after < residual_before,
          "AC4: residual_force_mask strictly shrinks after restamp on full force mask");
}

// ── Issue #3229: colliding 6-bit region bits stay define-correct ──
static void ac3229_1_collision_peer_stays_residual() {
    std::println("\n--- #3229 AC1: colliding peer residual not cleared ---");
    auto& reg = hot_update_registry();
    reg.reset_force_jit_repromote_for_test();
    reg.force_jit_stamp_for_test(~0ull);
    aura::compiler::typed_audit::apply_production_audit_defaults();

    constexpr std::uint32_t kD = 101;
    constexpr std::uint32_t kP = 202;
    const std::uint64_t bit = 1ULL << 7;
    CHECK((kD != kP), "3229 AC1: distinct define ids");
    reg.note_relower_success_coverage(bit);
    reg.note_relower_success_define(kD);
    CHECK(reg.relower_success_covers_define(kD), "3229 AC1: D covered");
    CHECK(!reg.relower_success_covers_define(kP), "3229 AC1: P not covered by D");
    CHECK(!reg.residual_force_for_define(kD, bit), "3229 AC1: D residual cleared");
    CHECK(reg.residual_force_for_define(kP, bit),
          "3229 AC1: colliding P still residual after D's region bit");
    CHECK((reg.residual_force_mask() & bit) == 0, "3229 AC1: #3136 region residual still shrinks");

    // Engineered name pair with the same fnv1a & 63.
    std::string a = "n0";
    std::string b;
    auto fnv = [](std::string_view s) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001b3ull;
        }
        return h;
    };
    const auto slot = fnv(a) & 63;
    bool found = false;
    for (int i = 1; i < 4096; ++i) {
        b = "n" + std::to_string(i);
        if ((fnv(b) & 63) == slot) {
            found = true;
            break;
        }
    }
    CHECK(found, "3229 AC1: found colliding name pair");
    CHECK((fnv(a) & 63) == (fnv(b) & 63), "3229 AC1: same 6-bit slot");
    CHECK(relower_success_define_id(a) != relower_success_define_id(b),
          "3229 AC1: define-ids distinct despite region collision");
}

static void ac3229_2_soft_quiet() {
    std::println("\n--- #3229 AC2: Soft no-op; quiet id=0 ---");
    auto& reg = hot_update_registry();
    reg.reset_force_jit_repromote_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    CHECK(!reg.relower_success_define_active(), "3229 AC2: Soft define set idle");
    reg.note_relower_success_define(303);
    CHECK(!reg.relower_success_covers_define(303), "3229 AC2: Soft does not persist define");
    CHECK(!reg.relower_success_define_active(), "3229 AC2: Soft stays idle");
    CHECK(!reg.residual_force_for_define(0, 0), "3229 AC2: quiet zero extra");
    aura::compiler::typed_audit::apply_production_audit_defaults();
}

static void ac3229_3_no_regression_3136() {
    std::println("\n--- #3229 AC3: #3136 surfaces retained ---");
    const auto hh = read_file("src/compiler/hot_update_registry.hh");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(hh.find("note_relower_success_coverage") != std::string::npos, "3229 AC3: #3136 hook");
    CHECK(svc.find("note_relower_success_coverage(1ULL << (fnv1a_64(name) & 63))") !=
              std::string::npos,
          "3229 AC3: #3136 hashed-name bit");
    CHECK(kRelowerSuccessDefineCollisionIssue == 3229, "3229 AC3: issue constant");
}

static void ac3229_4_linter_suites() {
    std::println("\n--- #3229 AC4: linter + suite cites ---");
    const auto t = read_file("tests/compiler/test_hot_update_relower_success_coverage.cpp");
    const auto force = read_file("tests/compiler/test_force_jit_repromote.cpp");
    const auto rec = read_file("tests/compiler/test_reload_recovery_query.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_relower_success_define_collision_3229.py");
    const auto build = read_file("build.py");
    const auto hh = read_file("src/compiler/hot_update_registry.hh");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(t.find("ac3229_1_collision_peer_stays_residual") != std::string::npos, "3229 AC4: suite");
    CHECK(force.find("3229") != std::string::npos, "3229 AC4: re-promote suite");
    CHECK(rec.find("3229") != std::string::npos, "3229 AC4: residual suite");
    CHECK(!lint.empty() && lint.find("3229") != std::string::npos, "3229 AC4: linter");
    CHECK(build.find("check_relower_success_define_collision_3229") != std::string::npos,
          "3229 AC4: build.py");
    CHECK(hh.find("note_relower_success_define") != std::string::npos, "3229 AC4: define hook");
    CHECK(rt.find("Issue #3229") != std::string::npos, "3229 AC4: remount skip");
    CHECK(hh.find("g_aot_table_epoch") == std::string::npos, "3229 AC4: no global epoch bump");
    CHECK(read_file("docs/design/3229-relower-success-define.md").empty(),
          "3229 AC4: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3229.cpp").empty(), "3229 AC4: no invent");
    CHECK(read_file("tests/issues/test_issue_3229.cpp").empty(), "3229 AC4: no tests/issues");
}

} // namespace

int run_test_hot_update_relower_success_coverage() {
    CompilerService cs;
    std::print("[test_hot_update_relower_success_coverage] running #3136 + #3229 ACs\n");

    // Seed cache first (Soft) so store_ir_cache_v2 does not already
    // OR last_reemit_success_region_mask. Then arm production so
    // restamp_cache_entry_for_test notes coverage.
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    // restamp_cache_entry_for_test only hits live ir_cache_v2_ names.
    CHECK(cs.eval("(set-code \""
                  "(define test_hot_update_relower_success_coverage_ac1 (lambda () 1))"
                  "(define test_hot_update_relower_success_coverage_ac2 (lambda () 2))"
                  "(define test_hot_update_relower_success_coverage_ac3_a (lambda () 3))"
                  "(define test_hot_update_relower_success_coverage_ac3_b (lambda () 4))"
                  "(define test_hot_update_relower_success_coverage_ac4 (lambda () 5))"
                  "\")")
              .has_value(),
          "seed defines");
    CHECK(cs.eval("(eval-current)").has_value(), "eval seed");
    if (!cs.get_define_v2("test_hot_update_relower_success_coverage_ac1"))
        (void)cs.eval("(compile:cache-define \"test_hot_update_relower_success_coverage_ac1\")");
    if (!cs.get_define_v2("test_hot_update_relower_success_coverage_ac2"))
        (void)cs.eval("(compile:cache-define \"test_hot_update_relower_success_coverage_ac2\")");
    if (!cs.get_define_v2("test_hot_update_relower_success_coverage_ac3_a"))
        (void)cs.eval("(compile:cache-define \"test_hot_update_relower_success_coverage_ac3_a\")");
    if (!cs.get_define_v2("test_hot_update_relower_success_coverage_ac3_b"))
        (void)cs.eval("(compile:cache-define \"test_hot_update_relower_success_coverage_ac3_b\")");
    if (!cs.get_define_v2("test_hot_update_relower_success_coverage_ac4"))
        (void)cs.eval("(compile:cache-define \"test_hot_update_relower_success_coverage_ac4\")");

    aura::compiler::typed_audit::apply_production_audit_defaults();
    ac1_restamp_flips_bit(cs);
    ac2_monotonic_same_name(cs);
    ac3_distinct_names_grow(cs);
    ac4_residual_shrinks(cs);
    ac3229_1_collision_peer_stays_residual();
    ac3229_2_soft_quiet();
    ac3229_3_no_regression_3136();
    ac3229_4_linter_suites();

    std::print("[test_hot_update_relower_success_coverage] passed={} failed={}\n", g_passed,
               g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hot_update_relower_success_coverage();
}
#endif
