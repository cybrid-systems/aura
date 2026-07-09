// @category: integration
// @reason: tests (query:ci-reproducibility-stats) primitive for Issue #675

// test_issue_675.cpp — Issue #675: Build/CI reproducibility observability.
//
// Scope-limited close ships the METRICS + SCRIPT layer:
//   - (query:ci-reproducibility-stats) 5-field hash primitive
//   - scripts/ci_reproducibility.py dual-build verifier
//   - scripts/gen_sbom.py CycloneDX SBOM generator
//   - scripts/security_scan.sh + .github/dependabot.yml
//   - CI jobs: ubsan-smoke, security-scan, reproducible-build
//
// Test cases:
//   AC1: query:ci-reproducibility-stats returns a hash
//   AC2: 5 fields present
//   AC3: sanitizer-mode defaults to "none" on normal build
//   AC4: reproducible-flags-active follows SOURCE_DATE_EPOCH
//   AC5: stats:list includes query:ci-reproducibility-stats
//   AC6: stats:count >= 51

#include <cstdlib>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_675_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:ci-reproducibility-stats) '{}')", key));
    if (!r)
        return -1;
    if (!aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::string hash_string(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:ci-reproducibility-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_string(*r))
        return {};
    return cs.evaluator().string_heap()[aura::compiler::types::as_string_idx(*r)];
}

static bool hash_bool(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:ci-reproducibility-stats) '{}')", key));
    return r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r);
}

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

} // namespace aura_issue_675_detail

int aura_issue_675_run() {
    using namespace aura_issue_675_detail;

    std::println("=== Issue #675: CI reproducibility stats ===");

    aura::compiler::CompilerService cs;

    // AC1: returns a hash
    {
        std::println("\n--- AC1: query:ci-reproducibility-stats returns hash ---");
        auto r = run_on(cs, "(query:ci-reproducibility-stats)");
        CHECK(aura::compiler::types::is_hash(r), "returns a hash");
    }

    // AC2: 5 fields
    {
        std::println("\n--- AC2: five fields present ---");
        CHECK(hash_int(cs, "source-date-epoch") >= 0, "source-date-epoch field");
        CHECK(!hash_string(cs, "build-type").empty(), "build-type field");
        CHECK(!hash_string(cs, "sanitizer-mode").empty(), "sanitizer-mode field");
        auto repro =
            cs.eval("(hash-ref (query:ci-reproducibility-stats) 'reproducible-flags-active)");
        CHECK(repro && aura::compiler::types::is_bool(*repro), "reproducible-flags-active field");
        auto ccache = cs.eval("(hash-ref (query:ci-reproducibility-stats) 'ccache-disabled)");
        CHECK(ccache && aura::compiler::types::is_bool(*ccache), "ccache-disabled field");
    }

    // AC3: sanitizer-mode "none" on normal build
    {
        std::println("\n--- AC3: sanitizer-mode is none on normal build ---");
        auto mode = hash_string(cs, "sanitizer-mode");
        CHECK(mode == "none", std::format("sanitizer-mode == none (got {})", mode));
    }

    // AC4: SOURCE_DATE_EPOCH drives reproducible-flags-active
    {
        std::println("\n--- AC4: reproducible-flags-active follows SOURCE_DATE_EPOCH ---");
        const char* old = std::getenv("SOURCE_DATE_EPOCH");
        std::string saved = old ? old : "";
#if defined(_WIN32)
        _putenv_s("SOURCE_DATE_EPOCH", "1704067200");
#else
        setenv("SOURCE_DATE_EPOCH", "1704067200", 1);
#endif
        CHECK(hash_bool(cs, "reproducible-flags-active"),
              "reproducible-flags-active #t with epoch set");
        CHECK(hash_int(cs, "source-date-epoch") == 1704067200, "source-date-epoch == 1704067200");
#if defined(_WIN32)
        if (saved.empty())
            _putenv_s("SOURCE_DATE_EPOCH", "");
        else
            _putenv_s("SOURCE_DATE_EPOCH", saved.c_str());
#else
        if (saved.empty())
            unsetenv("SOURCE_DATE_EPOCH");
        else
            setenv("SOURCE_DATE_EPOCH", saved.c_str(), 1);
#endif
    }

    // AC5: stats:list includes primitive
    {
        std::println("\n--- AC5: stats:list includes query:ci-reproducibility-stats ---");
        auto r = run_on(
            cs, "(letrec ((find? (lambda (needle hay) "
                "                (if (pair? hay) "
                "                    (if (string=? (car hay) needle) #t (find? needle (cdr hay))) "
                "                    #f)))) "
                "  (if (find? \"query:ci-reproducibility-stats\" (stats:list)) 1 0))");
        CHECK(aura::compiler::types::is_int(r) && aura::compiler::types::as_int(r) == 1,
              "stats:list includes query:ci-reproducibility-stats");
    }

    // AC6: stats:count >= 51
    {
        std::println("\n--- AC6: stats:count >= 51 ---");
        auto r = run_on(cs, "(stats:count)");
        const auto n = aura::compiler::types::is_int(r) ? aura::compiler::types::as_int(r) : 0;
        CHECK(n >= 51, std::format("stats:count >= 51 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_675_run();
}
#endif
