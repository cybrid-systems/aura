// @category: integration
// @reason: uses CompilerService to verify SEVA goal primitives
//          + OpenClaw integration layer

// test_issue_445_openclaw_integration.cpp — Issue #445:
// OpenClaw / LLM Agent integration layer for natural-
// language-driven SEVA goals.
//
// Full scope: high-level SEVA goal primitives + safety
// guardrails + OpenClaw skill/plugin + integration
// contract docs. Scope-limited close ships:
//
//   1. 4 new Aura primitives wrapping the SEVA loop:
//     - seva:achieve-coverage name target-pct → hash
//       with gap analysis (drives the agent's loop)
//     - seva:fix-reset-bugs → hash with verify-dirty
//       breakdown by reason (assertion/coverage/sva/cex)
//     - seva:generate-regression → string with a
//       regression script skeleton
//     - seva:approve-mutation id flag → bool (safety
//       gate; flag "force" / "auto" approved, others
//       denied in MVP)
//
//   2. (query:seva-audit-log) — Issue #445: the agent's
//      audit trail. Returns a hash with mutation totals
//      + verify-dirty + auto-evolve counters.
//
//   3. demos/seva/openclaw-skill/seva_skill.py — example
//      OpenClaw skill showing how to drive the SEVA
//      loop from an external agent (natural-language
//      goal parser + AuraServer transport + goal drivers).
//
//   4. (stats:count) 44 → 45.
//
// Test cases:
//   AC1:  seva:achieve-coverage returns a hash
//   AC2:  seva:achieve-coverage gap == 0 on clean workspace
//   AC3:  seva:fix-reset-bugs returns a hash
//   AC4:  seva:generate-regression returns a string
//   AC5:  seva:approve-mutation "force" → #t
//   AC6:  seva:approve-mutation "auto" → #t
//   AC7:  seva:approve-mutation "deny" → #f
//   AC8:  query:seva-audit-log returns a hash with 4 fields
//   AC9:  stats:list includes query:seva-audit-log
//   AC10: stats:count >= 45
//   AC11: end-to-end — agent reads coverage, applies
//         strategy, queries audit log, observes movement
//   AC12: openclaw-skill Python file exists

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_445_detail {
static int g_passed = 0;
static int g_failed = 0;

static aura::compiler::types::EvalValue run_on(aura::compiler::CompilerService& cs,
                                                std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return aura::compiler::types::make_void();
    }
    return *r;
}

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:seva-audit-log) '{}')", key));
    if (!r) return -1;
    if (!aura::compiler::types::is_int(*r)) return -1;
    return aura::compiler::types::as_int(*r);
}

static std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f) return std::string();
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println(std::cout, "  PASS: {}", msg); } \
    else      { ++g_failed; std::println(std::cout, "  FAIL: {}", msg); } \
} while (0)

// ═══════════════════════════════════════════════════════════
// AC1: seva:achieve-coverage returns a hash
// ═══════════════════════════════════════════════════════════
bool test_achieve_coverage_returns_hash() {
    std::println("\n--- AC1: seva:achieve-coverage returns hash ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto r = run_on(cs, "(seva:achieve-coverage \"FIFO\" 95)");
    bool ok = aura::compiler::types::is_hash(r);
    CHECK(ok, "seva:achieve-coverage returns a hash");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC2: gap == 0 on clean workspace
// ═══════════════════════════════════════════════════════════
bool test_gap_zero_on_clean() {
    std::println("\n--- AC2: gap == 0 on clean workspace ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto r = run_on(cs, "(hash-ref (seva:achieve-coverage \"FIFO\" 95) 'gap)");
    bool ok = aura::compiler::types::is_int(r) &&
              aura::compiler::types::as_int(r) == 0;
    CHECK(ok, "clean workspace: gap == 0");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC3: seva:fix-reset-bugs returns a hash
// ═══════════════════════════════════════════════════════════
bool test_fix_reset_bugs_returns_hash() {
    std::println("\n--- AC3: seva:fix-reset-bugs returns hash ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto r = run_on(cs, "(seva:fix-reset-bugs)");
    bool ok = aura::compiler::types::is_hash(r);
    CHECK(ok, "seva:fix-reset-bugs returns a hash");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC4: seva:generate-regression returns a string
// ═══════════════════════════════════════════════════════════
bool test_generate_regression_returns_string() {
    std::println("\n--- AC4: seva:generate-regression returns string ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(cs, "(seva:generate-regression)");
    bool ok = aura::compiler::types::is_string(r);
    CHECK(ok, "seva:generate-regression returns a string");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC5-AC7: approve-mutation flag semantics
// ═══════════════════════════════════════════════════════════
bool test_approve_mutation_flags() {
    std::println("\n--- AC5-AC7: approve-mutation flag semantics ---");
    aura::compiler::CompilerService cs;
    auto r_force = run_on(cs, "(seva:approve-mutation 0 \"force\")");
    auto r_auto = run_on(cs, "(seva:approve-mutation 0 \"auto\")");
    auto r_deny = run_on(cs, "(seva:approve-mutation 0 \"deny\")");
    CHECK(aura::compiler::types::is_bool(r_force) &&
          aura::compiler::types::as_bool(r_force),
          "approve-mutation \"force\" → #t");
    CHECK(aura::compiler::types::is_bool(r_auto) &&
          aura::compiler::types::as_bool(r_auto),
          "approve-mutation \"auto\" → #t");
    CHECK(aura::compiler::types::is_bool(r_deny) &&
          !aura::compiler::types::as_bool(r_deny),
          "approve-mutation \"deny\" → #f");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC8: query:seva-audit-log returns 4 fields
// ═══════════════════════════════════════════════════════════
bool test_seva_audit_log_fields() {
    std::println("\n--- AC8: query:seva-audit-log fields ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    static const char* kFields[] = {
        "mutations-total", "verify-dirty-total",
        "auto-evolve-cycles", "auto-evolve-fixed",
    };
    int found = 0;
    for (auto* k : kFields) {
        auto v = hash_int(cs, k);
        if (v >= 0) ++found;
    }
    CHECK(found == 4, "all 4 audit-log fields present + non-negative");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC9: stats:list includes the new primitive
// ═══════════════════════════════════════════════════════════
bool test_stats_list_includes() {
    std::println("\n--- AC9: stats:list includes query:seva-audit-log ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(cs,
        "(letrec ((find? (lambda (needle hay) "
        "                (if (pair? hay) "
        "                    (if (string=? (car hay) needle) #t (find? needle (cdr hay))) "
        "                    #f)))) "
        "  (if (find? \"query:seva-audit-log\" (stats:list)) 1 0))");
    bool included = aura::compiler::types::is_int(r) &&
                    aura::compiler::types::as_int(r) == 1;
    CHECK(included, "stats:list includes query:seva-audit-log");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC10: stats:count >= 45
// ═══════════════════════════════════════════════════════════
bool test_stats_count() {
    std::println("\n--- AC10: stats:count is up to date ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(cs, "(stats:count)");
    bool ok = aura::compiler::types::is_int(r) &&
              aura::compiler::types::as_int(r) >= 45;
    CHECK(ok, "stats:count >= 45 (was 44 in #444, now 45 in #445)");
    if (aura::compiler::types::is_int(r)) {
        std::println("    [stats:count = {}]", aura::compiler::types::as_int(r));
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC11: end-to-end loop
// ═══════════════════════════════════════════════════════════
bool test_end_to_end_loop() {
    std::println("\n--- AC11: end-to-end agent loop ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    // Step 1: read coverage gap
    auto gap = run_on(cs,
        "(hash-ref (seva:achieve-coverage \"FIFO\" 95) 'gap)");
    // Step 2: apply strategy
    run_on(cs, "(strategy:set-strategy \"coverage-greedy\")");
    run_on(cs, "(strategy:report-success \"coverage-greedy\")");
    // Step 3: query audit log
    auto audit = hash_int(cs, "mutations-total");
    // Step 4: read readiness
    auto dbr = run_on(cs,
        "(hash-ref (query:edsl-readiness) 'dirty-block-rate)");
    CHECK(aura::compiler::types::is_int(gap), "step 1: gap is an int");
    CHECK(audit >= 0, "step 3: audit log mutations-total >= 0");
    CHECK(aura::compiler::types::is_int(dbr), "step 4: dirty-block-rate is an int");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC12: openclaw-skill Python file exists
// ═══════════════════════════════════════════════════════════
bool test_openclaw_skill_exists() {
    std::println("\n--- AC12: openclaw-skill file exists ---");
    const std::filesystem::path p = "demos/seva/openclaw-skill/seva_skill.py";
    bool exists = std::filesystem::exists(p);
    CHECK(exists, "demos/seva/openclaw-skill/seva_skill.py exists");
    if (exists) {
        auto content = read_file(p);
        CHECK(!content.empty(), "seva_skill.py is non-empty");
        CHECK(content.find("AuraServer") != std::string::npos,
              "seva_skill.py defines AuraServer");
        CHECK(content.find("parse_goal") != std::string::npos,
              "seva_skill.py has parse_goal");
        CHECK(content.find("seva:achieve-coverage") != std::string::npos,
              "seva_skill.py calls seva:achieve-coverage");
        CHECK(content.find("query:seva-audit-log") != std::string::npos,
              "seva_skill.py calls query:seva-audit-log");
    }
    return true;
}

}  // namespace aura_issue_445_detail

int main() {
    using namespace aura_issue_445_detail;
    std::println("═══ Issue #445 OpenClaw integration tests ═══");

    test_achieve_coverage_returns_hash();
    test_gap_zero_on_clean();
    test_fix_reset_bugs_returns_hash();
    test_generate_regression_returns_string();
    test_approve_mutation_flags();
    test_seva_audit_log_fields();
    test_stats_list_includes();
    test_stats_count();
    test_end_to_end_loop();
    test_openclaw_skill_exists();

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}