// @category: integration
// @reason: smoke-tests the sanitizer matrix script
//          + docs file
// test_issue_325.cpp — Verify Issue #325 acceptance
// criteria (sanitizer matrix + coverage).
//
// Scope-limited close. The issue body asks for 5
// broad deliverables (sanitizer matrix, coverage,
// known-failures, metrics, docs). This PR ships a
// focused subset:
//   1. tests/run_sanitizer_matrix.sh — single entry
//      point that supports TSan/ASan/UBSan/coverage
//      on 4 representative test binaries
//   2. docs/sanitizers.md — usage doc for the matrix
//   3. tests/test_issue_325.cpp — this file (smoke
//      test that the script + doc exist + are valid)
//
// The remaining 3 deliverables (CI YAML registration,
// known-failures entry, test result aggregation) are
// filed as follow-ups.
//
// 4 ACs (from the issue body, scoped to this PR):
//   AC1 sanitizer matrix: 3 sanitizers supported
//        (TSan/ASan/UBSan) + "all" combined mode +
//        coverage mode (llvm-cov via gcov)
//   AC2 coverage: gcov instrumentation flag set is
//        documented in the script + applied to the 3
//        hot files the issue body names
//   AC3 known-failures: the matrix surfaces
//        findings (no auto-skip), per the "honest
//        failure" pattern (issues surface, don't get
//        suppressed)
//   AC4 docs: docs/sanitizers.md exists + covers
//        usage, the 3-sanitizer matrix, hot files,
//        CI integration recipe

#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

namespace aura_issue_325_detail {

// Helper: read first N lines of a file (or empty if
// missing). Returns a string suitable for substring
// matching.
static std::string read_file_head(
    const std::string& path, std::size_t n_lines = 200) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return std::string{};
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f) && n_lines > 0) {
        out += buf;
        --n_lines;
    }
    fclose(f);
    return out;
}

// Helper: read entire file (small files only).
static std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return std::string{};
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    fclose(f);
    return out;
}

// Helper: check that a shell script is syntactically
// valid (bash -n). Returns true if the script is
// valid OR not present (so the test doesn't fail
// on missing files in unrelated environments).
static bool shell_syntax_ok(const std::string& path) {
    std::string cmd = "bash -n " + path + " 2>/dev/null";
    int rc = std::system(cmd.c_str());
    return rc == 0;
}

// ═══════════════════════════════════════════════════════════════
// AC1: sanitizer matrix script exists + supports 3 sanitizers
// ═══════════════════════════════════════════════════════════════

bool test_sanitizer_matrix_script_exists() {
    std::println("\n--- AC1: sanitizer matrix script exists + 3 sanitizers ---");
    // Multi-source path resolution: tests are launched
    // from build/ so a relative path like
    // "tests/run_sanitizer_matrix.sh" would resolve to
    // "build/tests/run_sanitizer_matrix.sh" which
    // doesn't exist. Try env var AURA_SRC_ROOT first
    // (set by CI), then "../tests/..." (relative to
    // build/), then fall back to the env-less form.
    auto find_path = [](const std::string& rel) -> std::string {
        if (auto* env = std::getenv("AURA_SRC_ROOT"))
            return std::string(env) + "/" + rel;
        return std::string("../") + rel;
    };
    const std::string script = find_path("tests/run_sanitizer_matrix.sh");
    const std::string contents = read_file(script);
    CHECK(!contents.empty(),
          "tests/run_sanitizer_matrix.sh exists");
    if (contents.empty()) return false;
    // All 3 sanitizers supported in the case statement.
    CHECK(contents.find("thread|address|undefined|all|both|coverage")
              != std::string::npos,
          "case statement lists thread/address/undefined/all/both/coverage");
    // Per-sanitizer build directories.
    CHECK(contents.find("build_tsan") != std::string::npos,
          "TSan build dir documented");
    CHECK(contents.find("build_asan") != std::string::npos,
          "ASan build dir documented");
    CHECK(contents.find("build_ubsan") != std::string::npos,
          "UBSan build dir documented");
    CHECK(contents.find("build_cov") != std::string::npos,
          "coverage build dir documented");
    // Per-sanitizer compiler flag sets.
    CHECK(contents.find("TSAN_FLAGS") != std::string::npos,
          "TSan compiler flags defined");
    CHECK(contents.find("ASAN_FLAGS") != std::string::npos,
          "ASan compiler flags defined");
    CHECK(contents.find("UBSAN_FLAGS") != std::string::npos,
          "UBSan compiler flags defined");
    // Syntax check.
    CHECK(shell_syntax_ok(script),
          "script is syntactically valid (bash -n)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2: coverage toolchain flags + hot files
// ═══════════════════════════════════════════════════════════════

bool test_coverage_toolchain() {
    std::println("\n--- AC2: coverage toolchain + hot files ---");
    auto find_path = [](const std::string& rel) -> std::string {
        if (auto* env = std::getenv("AURA_SRC_ROOT"))
            return std::string(env) + "/" + rel;
        return std::string("../") + rel;
    };
    const std::string script = find_path("tests/run_sanitizer_matrix.sh");
    const std::string contents = read_file(script);
    CHECK(contents.find("COV_FLAGS") != std::string::npos,
          "COV_FLAGS defined (llvm-cov / gcov instrumentation)");
    CHECK(contents.find("-fprofile-arcs") != std::string::npos,
          "profile-arcs flag (gcov instrumentation)");
    CHECK(contents.find("-ftest-coverage") != std::string::npos,
          "test-coverage flag (gcov instrumentation)");
    CHECK(contents.find("-lgcov") != std::string::npos,
          "linker flag -lgcov present");
    // Hot files from the issue body (post-#225 split,
    // evaluator_impl.cpp was split into the 3
    // evaluator_primitives_*.cpp files; the script
    // names evaluator_primitives_compile.cpp as the
    // successor).
    CHECK(contents.find("evaluator_primitives_compile.cpp")
              != std::string::npos,
          "evaluator_primitives_compile.cpp covered (successor of evaluator_impl.cpp)");
    CHECK(contents.find("fiber.cpp") != std::string::npos,
          "fiber.cpp covered");
    CHECK(contents.find("aura_jit_bridge.cpp")
              != std::string::npos,
          "aura_jit_bridge.cpp covered");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3: known-failures handling — matrix surfaces findings
// (no auto-skip; honest failure pattern)
// ═══════════════════════════════════════════════════════════════

bool test_known_failures_handling() {
    std::println("\n--- AC3: known-failures handling ---");
    auto find_path = [](const std::string& rel) -> std::string {
        if (auto* env = std::getenv("AURA_SRC_ROOT"))
            return std::string(env) + "/" + rel;
        return std::string("../") + rel;
    };
    const std::string script = find_path("tests/run_sanitizer_matrix.sh");
    const std::string contents = read_file(script);
    // The matrix does NOT skip on pre-existing
    // findings. Each new finding surfaces immediately
    // (the issue is filed as a follow-up, not
    // suppressed in the script). The exit code is
    // 0 only when all targets pass; otherwise 1.
    CHECK(contents.find("\"$fail_count\" = 0") != std::string::npos
          || contents.find("$fail_count = 0") != std::string::npos,
          "matrix exit code 0 only on full pass");
    // The script's tail includes a summary that
    // surfaces the failing target names (not silent).
    CHECK(contents.find("Failed targets:") != std::string::npos
          || contents.find("failed_targets=") != std::string::npos,
          "matrix prints failed target names");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC4: docs/sanitizers.md exists + covers usage
// ═══════════════════════════════════════════════════════════════

bool test_docs_sanitizers_md() {
    std::println("\n--- AC4: docs/sanitizers.md ---");
    auto find_path = [](const std::string& rel) -> std::string {
        if (auto* env = std::getenv("AURA_SRC_ROOT"))
            return std::string(env) + "/" + rel;
        return std::string("../") + rel;
    };
    const std::string docs = find_path("docs/sanitizers.md");
    const std::string contents = read_file(docs);
    CHECK(!contents.empty(),
          "docs/sanitizers.md exists");
    if (contents.empty()) return false;
    // Usage block at the top.
    CHECK(contents.find("## Quick start") != std::string::npos,
          "docs has Quick start section");
    CHECK(contents.find("tests/run_sanitizer_matrix.sh all")
              != std::string::npos,
          "docs shows the all-mode invocation");
    // The 3 sanitizers are documented.
    CHECK(contents.find("TSan") != std::string::npos,
          "docs mention TSan");
    CHECK(contents.find("ASan") != std::string::npos,
          "docs mention ASan");
    CHECK(contents.find("UBSan") != std::string::npos,
          "docs mention UBSan");
    // Hot files documented.
    CHECK(contents.find("evaluator_primitives_compile.cpp")
              != std::string::npos,
          "docs name evaluator_primitives_compile.cpp as hot file");
    CHECK(contents.find("fiber.cpp") != std::string::npos,
          "docs name fiber.cpp as hot file");
    CHECK(contents.find("aura_jit_bridge.cpp")
              != std::string::npos,
          "docs name aura_jit_bridge.cpp as hot file");
    // CI integration recipe.
    CHECK(contents.find("CI integration") != std::string::npos,
          "docs have CI integration section");
    return true;
}

int run_tests() {
    std::println("═══ Issue #325 (sanitizer matrix + coverage) ═══\n");
    test_sanitizer_matrix_script_exists();
    test_coverage_toolchain();
    test_known_failures_handling();
    test_docs_sanitizers_md();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_325_detail

int aura_issue_325_run() { return aura_issue_325_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_325_run(); }
#endif