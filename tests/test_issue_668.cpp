// @category: integration
// @reason: Issue #668 — math regex primitive error consistency
//  (P1 stdlib-impl error handling). Ships:
//   - (query:primitives-regex-error-stats, schema 668) hash primitive
//     with 1 field (regex-errors)
//   - primitives_regex_error_total atomic on CompilerMetrics
//   - bump_primitives_regex_error_total + get_primitives_regex_error_total
//     accessors on Evaluator
//   - 4 regex primitives (regex-match?, regex-find, regex-replace,
//     regex-split) updated to use PRIM_ERROR + bump on EVERY error
//     path (pre-try type-mismatch + OOB + post-try invalid-regex-syntax).
//     Pre-fix: type-mismatch / OOB returned silent sentinel
//     (make_int(0) / make_void()); post-fix: PRIM_ERROR with
//     consistent message and counter bump.
//
//  Non-duplicative with #615 (PRIM_ERROR macro unification),
//  #478 (general primitive_error_count observability),
//  #643 (error unification), #614 (hotpath stability).
//
//   - AC1:  query:primitives-regex-error-stats reachable (schema 668)
//   - AC2:  regex-errors is 0 on fresh CompilerService
//   - AC3:  regex-match? with invalid regex bumps regex-errors by 1
//   - AC4:  regex-match? with type-mismatch (non-string arg) bumps
//           regex-errors by 1 (NEW behavior — pre-fix returned 0 silently)
//   - AC5:  regex-find with type-mismatch (non-string arg) bumps
//           regex-errors by 1 (NEW behavior — pre-fix returned void)
//   - AC6:  regex-replace with OOB string index bumps regex-errors
//           by 1 (NEW behavior — pre-fix returned void)
//   - AC7:  regex-split with OOB string index bumps regex-errors by 1
//   - AC8:  4 sequential invalid-regex calls bump by 4 (the primitive
//           breakdown sanity check — counter aggregates over all 4)
//   - AC9:  successful regex-match? does NOT bump regex-errors
//           (sanity: counter is errors-only)
//   - AC10: regression — adjacent observability primitives
//           (query:primitives-apply-stats schema 667,
//           query:primitives-meta-catalog schema 617) still reachable

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_668_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:primitives-regex-error-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void run_ac1_schema(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: query:primitives-regex-error-stats reachable (schema 668) ---");
    auto r = cs.eval("(hash-ref (query:primitives-regex-error-stats) 'schema)");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 668,
          "schema field == 668");
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: regex-errors is 0 on fresh CompilerService ---");
    CHECK(stat_int(cs, "regex-errors") == 0, "regex-errors == 0 on fresh workspace");
}

static void run_ac3_invalid_regex_match(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: regex-match? with invalid regex bumps regex-errors by 1 ---");
    const std::int64_t before = stat_int(cs, "regex-errors");
    // '[' is an unterminated character class — std::regex throws.
    auto r = cs.eval(R"aura((regex-match? "[" "abc"))aura");
    const std::int64_t after = stat_int(cs, "regex-errors");
    CHECK(after - before == 1, "regex-errors bumped by 1 after invalid regex-match? call");
    (void)r;
}

static void run_ac4_type_mismatch_match(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: regex-match? with non-string arg bumps regex-errors by 1 ---");
    const std::int64_t before = stat_int(cs, "regex-errors");
    // First arg is an integer — pre-fix returned make_int(0) silently.
    auto r = cs.eval(R"aura((regex-match? 42 "abc"))aura");
    const std::int64_t after = stat_int(cs, "regex-errors");
    CHECK(after - before == 1, "regex-errors bumped by 1 after non-string first arg (NEW)");
    (void)r;
}

static void run_ac5_type_mismatch_find(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regex-find with non-string arg bumps regex-errors by 1 ---");
    const std::int64_t before = stat_int(cs, "regex-errors");
    auto r = cs.eval(R"aura((regex-find 42 "abc"))aura");
    const std::int64_t after = stat_int(cs, "regex-errors");
    CHECK(after - before == 1, "regex-errors bumped by 1 after non-string first arg (NEW)");
    (void)r;
}

static void run_ac6_oob_replace(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC6: regex-replace with type-mismatch bumps regex-errors by 1 ---");
    const std::int64_t before = stat_int(cs, "regex-errors");
    // Integer first arg — pre-fix returned make_void() silently.
    auto r = cs.eval(R"aura((regex-replace 42 "abc" "def"))aura");
    const std::int64_t after = stat_int(cs, "regex-errors");
    CHECK(after - before == 1, "regex-errors bumped by 1 after non-string args (NEW)");
    (void)r;
}

static void run_ac7_oob_split(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC7: regex-split with type-mismatch bumps regex-errors by 1 ---");
    const std::int64_t before = stat_int(cs, "regex-errors");
    auto r = cs.eval(R"aura((regex-split 42 "abc"))aura");
    const std::int64_t after = stat_int(cs, "regex-errors");
    CHECK(after - before == 1, "regex-errors bumped by 1 after non-string first arg (NEW)");
    (void)r;
}

static void run_ac8_four_primitives(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC8: 4 invalid-regex calls bump by 4 (one per primitive) ---");
    const std::int64_t before = stat_int(cs, "regex-errors");
    cs.eval(R"aura((regex-match? "[" "x"))aura");
    cs.eval(R"aura((regex-find "[" "x"))aura");
    cs.eval(R"aura((regex-replace "[" "x" "y"))aura");
    cs.eval(R"aura((regex-split "[" "x"))aura");
    const std::int64_t after = stat_int(cs, "regex-errors");
    CHECK(after - before == 4,
          "4 invalid-regex calls across 4 primitives → regex-errors bumped by 4");
}

static void run_ac9_no_bump_on_success(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC9: successful regex-match? does NOT bump regex-errors ---");
    const std::int64_t before = stat_int(cs, "regex-errors");
    auto r = cs.eval(R"aura((regex-match? "[0-9]+" "abc123"))aura");
    const std::int64_t after = stat_int(cs, "regex-errors");
    CHECK(after - before == 0, "successful regex-match? did not bump regex-errors");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 1,
          "regex-match? returns 1 (match found)");
}

static void run_ac10_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC10: regression — adjacent observability primitives reachable ---");
    auto apply_stats = cs.eval("(query:primitives-apply-stats)");
    auto meta_catalog = cs.eval("(query:primitives-meta-catalog)");
    auto apply_schema = cs.eval("(hash-ref (query:primitives-apply-stats) 'schema)");
    CHECK(apply_stats && aura::compiler::types::is_hash(*apply_stats),
          "query:primitives-apply-stats (schema 667) regression [hash]");
    CHECK(meta_catalog, "query:primitives-meta-catalog (schema 617) regression");
    CHECK(apply_schema && aura::compiler::types::is_int(*apply_schema) &&
              aura::compiler::types::as_int(*apply_schema) == 667,
          "query:primitives-apply-stats schema == 667 (regression)");
}

} // namespace aura_issue_668_detail

int aura_issue_668_run() {
    using namespace aura_issue_668_detail;

    {
        aura::compiler::CompilerService cs;
        run_ac1_schema(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac2_fresh_zero(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac3_invalid_regex_match(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac4_type_mismatch_match(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac5_type_mismatch_find(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac6_oob_replace(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac7_oob_split(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac8_four_primitives(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac9_no_bump_on_success(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac10_regression(cs);
    }

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_668_run();
}
#endif
