// @category: integration
// @reason: tests the (seva:run-demo-with-metrics) primitive
//          + verifies docs/tutorial/file presence

// test_issue_446_seva_docs_metrics.cpp — Issue #446:
// SEVA docs + tutorial + standardized metrics collection.
//
// Full scope: docs/demos/seva.md (architecture +
// how-to-run + primitives) + tutorial walkthrough +
// metrics collection primitive + README update.
//
// Scope-limited close ships:
//   1. docs/demos/seva.md (architecture diagram + how-to +
//      primitives reference + metrics section)
//   2. demos/seva/TUTORIAL.md (step-by-step walkthrough
//      with expected output)
//   3. README.md (Demos section pointing to SEVA)
//   4. (seva:run-demo-with-metrics) primitive — Issue #446:
//      6-field hash with iterations / coverage-improvement /
//      human-intervention / mutation-success-rate /
//      mutations-total / active-strategy
//   5. (stats:count) 45 → 46
//
// Test cases:
//   AC1:  docs/demos/seva.md exists + has architecture diagram
//   AC2:  demos/seva/TUTORIAL.md exists + has expected output
//   AC3:  README.md links to SEVA demo
//   AC4:  seva:run-demo-with-metrics returns a hash
//   AC5:  6 fields present
//   AC6:  human-intervention-count == 0 (agent autonomy)
//   AC7:  mutation-success-rate-pct is in 0..100
//   AC8:  stats:count >= 46
//   AC9:  stats:list includes seva:run-demo-with-metrics
//   AC10: end-to-end metrics after a synthetic strategy cycle

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_446_detail {
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
    auto r = cs.eval(std::format("(hash-ref (seva:run-demo-with-metrics) '{}')", key));
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
// AC1: docs/demos/seva.md exists + has architecture diagram
// ═══════════════════════════════════════════════════════════
bool test_seva_doc_exists() {
    std::println("\n--- AC1: docs/demos/seva.md exists ---");
    const std::filesystem::path p = "docs/demos/seva.md";
    bool exists = std::filesystem::exists(p);
    CHECK(exists, "docs/demos/seva.md exists");
    if (exists) {
        auto content = read_file(p);
        CHECK(!content.empty(), "seva.md is non-empty");
        CHECK(content.find("Architecture") != std::string::npos
              || content.find("architecture") != std::string::npos,
              "seva.md has architecture section");
        CHECK(content.find("self-evolving") != std::string::npos
              || content.find("self-evolution") != std::string::npos,
              "seva.md describes self-evolution");
        CHECK(content.find("verify:parse-coverage-feedback") != std::string::npos,
              "seva.md documents verify:parse-coverage-feedback");
        CHECK(content.find("strategy") != std::string::npos,
              "seva.md documents strategy evolution");
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC2: TUTORIAL.md exists + has expected output
// ═══════════════════════════════════════════════════════════
bool test_tutorial_exists() {
    std::println("\n--- AC2: demos/seva/TUTORIAL.md exists ---");
    const std::filesystem::path p = "demos/seva/TUTORIAL.md";
    bool exists = std::filesystem::exists(p);
    CHECK(exists, "demos/seva/TUTORIAL.md exists");
    if (exists) {
        auto content = read_file(p);
        CHECK(!content.empty(), "TUTORIAL.md is non-empty");
        CHECK(content.find("Expected output") != std::string::npos,
              "TUTORIAL.md has Expected output section");
        CHECK(content.find("Step 1: set-code") != std::string::npos,
              "TUTORIAL.md shows demo steps");
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC3: README.md links to SEVA
// ═══════════════════════════════════════════════════════════
bool test_readme_seva_link() {
    std::println("\n--- AC3: README.md links to SEVA ---");
    auto content = read_file("README.md");
    CHECK(content.find("SEVA") != std::string::npos, "README.md mentions SEVA");
    CHECK(content.find("docs/demos/seva.md") != std::string::npos
          || content.find("demos/seva/TUTORIAL.md") != std::string::npos,
          "README.md links to SEVA docs");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC4: seva:run-demo-with-metrics returns a hash
// ═══════════════════════════════════════════════════════════
bool test_metrics_returns_hash() {
    std::println("\n--- AC4: seva:run-demo-with-metrics returns hash ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto r = run_on(cs, "(seva:run-demo-with-metrics)");
    bool ok = aura::compiler::types::is_hash(r);
    CHECK(ok, "seva:run-demo-with-metrics returns a hash");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC5: 6 fields present
// ═══════════════════════════════════════════════════════════
bool test_six_fields() {
    std::println("\n--- AC5: 6 fields present + non-negative ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    static const char* kFields[] = {
        "iterations-to-closure", "coverage-improvement",
        "human-intervention-count", "mutation-success-rate-pct",
        "mutations-total", "active-strategy",
    };
    int found = 0;
    for (auto* k : kFields) {
        // active-strategy is a string, others are ints.
        if (std::string(k) == "active-strategy") {
            auto r = run_on(cs, std::format("(hash-ref (seva:run-demo-with-metrics) '{}')", k));
            if (aura::compiler::types::is_string(r)) ++found;
        } else {
            auto v = hash_int(cs, k);
            if (v >= 0) ++found;
        }
    }
    CHECK(found == 6, "all 6 fields accessible");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC6: human-intervention-count == 0 (MVP agent runs autonomously)
// ═══════════════════════════════════════════════════════════
bool test_human_intervention_zero() {
    std::println("\n--- AC6: human-intervention-count == 0 ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto v = hash_int(cs, "human-intervention-count");
    CHECK(v == 0, "MVP: human-intervention-count == 0 (agent autonomous)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC7: mutation-success-rate-pct is in 0..100
// ═══════════════════════════════════════════════════════════
bool test_success_rate_bounded() {
    std::println("\n--- AC7: success-rate is 0..100 ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto v = hash_int(cs, "mutation-success-rate-pct");
    CHECK(v >= 0 && v <= 100, "mutation-success-rate-pct in 0..100");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC8: stats:count >= 46
// ═══════════════════════════════════════════════════════════
bool test_stats_count() {
    std::println("\n--- AC8: stats:count is up to date ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(cs, "(stats:count)");
    bool ok = aura::compiler::types::is_int(r) &&
              aura::compiler::types::as_int(r) >= 46;
    CHECK(ok, "stats:count >= 46 (was 45 in #445, now 46 in #446)");
    if (aura::compiler::types::is_int(r)) {
        std::println("    [stats:count = {}]", aura::compiler::types::as_int(r));
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC9: stats:list includes seva:run-demo-with-metrics
// ═══════════════════════════════════════════════════════════
bool test_stats_list_includes() {
    std::println("\n--- AC9: stats:list includes the new primitive ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(cs,
        "(letrec ((find? (lambda (needle hay) "
        "                (if (pair? hay) "
        "                    (if (string=? (car hay) needle) #t (find? needle (cdr hay))) "
        "                    #f)))) "
        "  (if (find? \"seva:run-demo-with-metrics\" (stats:list)) 1 0))");
    bool included = aura::compiler::types::is_int(r) &&
                    aura::compiler::types::as_int(r) == 1;
    CHECK(included, "stats:list includes seva:run-demo-with-metrics");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC10: end-to-end metrics after a synthetic strategy cycle
// ═══════════════════════════════════════════════════════════
bool test_synthetic_cycle() {
    std::println("\n--- AC10: end-to-end metrics cycle ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    // Drive 4 strategies with mixed success rates.
    run_on(cs, "(strategy:set-strategy \"coverage-greedy\")");
    run_on(cs, "(strategy:set-strategy \"coverage-greedy\")");
    run_on(cs, "(strategy:set-strategy \"coverage-greedy\")");
    run_on(cs, "(strategy:report-success \"coverage-greedy\")");
    run_on(cs, "(strategy:set-strategy \"bug-fix-priority\")");
    run_on(cs, "(strategy:report-success \"bug-fix-priority\")");
    run_on(cs, "(strategy:set-strategy \"minimal-mutation\")");
    auto mut_total = hash_int(cs, "mutations-total");
    auto success_rate = hash_int(cs, "mutation-success-rate-pct");
    auto human_int = hash_int(cs, "human-intervention-count");
    CHECK(mut_total >= 0, "mutations-total >= 0");
    CHECK(success_rate >= 0 && success_rate <= 100,
          "success-rate in 0..100 after synthetic cycle");
    CHECK(human_int == 0, "human-intervention stays 0 (autonomous)");
    return true;
}

}  // namespace aura_issue_446_detail

int main() {
    using namespace aura_issue_446_detail;
    std::println("═══ Issue #446 SEVA docs + metrics tests ═══");

    test_seva_doc_exists();
    test_tutorial_exists();
    test_readme_seva_link();
    test_metrics_returns_hash();
    test_six_fields();
    test_human_intervention_zero();
    test_success_rate_bounded();
    test_stats_count();
    test_stats_list_includes();
    test_synthetic_cycle();

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}