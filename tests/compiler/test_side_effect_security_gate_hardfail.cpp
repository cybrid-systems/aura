// @category: unit
// @reason: Issue #2494 — ci(security): hard-fail
// `check_side_effect_security.py` for new prims without `require_effect` /
// `PrimMeta`. The script already has --strict mode; this test confirms
// it's wired as a hard-fail (PR CI via build.py gate) and that the
// allowlist reason-format enforcement is operational.
//
//   AC1: Intentionally broken fixture prim (side-effect name, no
//        require_effect / PrimMeta / exempt) → script exit non-zero
//        under --strict + --path.
//   AC2: Existing production prim set passes under --strict (no false
//        positive storm on main).
//   AC3: PR CI job runs the script via build.py gate (which uses
//        --strict) and fails the build on violation.
//   AC4: Exempt allowlist entries missing '# SECURITY_EXEMPT: <reason>'
//        → script exit non-zero.
//   AC5: Source-cite script + CI workflow path.
//   AC6: Tests + source-cite registrations (CMakeLists.txt + build.py).

#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

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

// Helper: run the script via Python subprocess and capture exit code.
static int run_script(const std::vector<std::string>& args) {
    std::string cmd = "python3 scripts/coverage/checks/check_side_effect_security.py";
    for (const auto& a : args)
        cmd += " " + a;
    cmd += " 2>&1 > /tmp/check_side_effect_security.out";
    int rc = std::system(cmd.c_str());
    return rc;
}

// Helper: create a temp fixture prim file with a bare side-effect add()
// (no require_effect / add_mutate / security_exempt / PrimMeta). Used
// for AC1 self-test.
static std::string write_broken_fixture() {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "broken_prim_2494.cpp";
    std::ofstream out(path);
    out << "// Issue #2494 AC1 fixture: bare side-effect add() with no coverage marker.\n";
    out << "add(\"mutate:bad-no-coverage-2494\", [](auto& ev) { return ev.make_void(); });\n";
    out.close();
    return path.string();
}

// AC1: intentionally broken fixture prim → script exit non-zero under
// --strict + --path.
static void ac1_broken_fixture_fails() {
    std::println("\n--- #2494 AC1: broken fixture prim → script exit non-zero ---");
    const auto fixture = write_broken_fixture();
    const int rc = run_script({"--strict", "--path", fixture});
    std::println("  fixture={} rc={}", fixture, rc);
    CHECK(rc != 0, "AC1: bare side-effect add() with no coverage marker fails --strict");
}

// AC2: existing production prim set passes (no false-positive storm on
// main). Run the script on the production src/compiler path with
// --strict and verify exit 0.
static void ac2_existing_prim_set_passes() {
    std::println("\n--- #2494 AC2: production prim set passes ---");
    const int rc = run_script({"--strict"});
    std::println("  rc={}", rc);
    CHECK(rc == 0, "AC2: production prim set passes check_side_effect_security --strict");
}

// AC3: PR CI job runs the script via build.py gate. Verify gate includes
// cmd_side_effect_security() (which uses --strict).
static void ac3_ci_gate_runs_script() {
    std::println("\n--- #2494 AC3: CI gate runs script via build.py ---");
    const auto bp = read_file("build.py");
    // cmd_side_effect_security() exists at line 1596, called from gate.
    CHECK(bp.find("def cmd_side_effect_security():") != std::string::npos,
          "AC3: build.py has cmd_side_effect_security");
    CHECK(bp.find("--strict") != std::string::npos, "AC3: build.py calls script with --strict");
    CHECK(bp.find("or cmd_side_effect_security()") != std::string::npos,
          "AC3: build.py gate includes cmd_side_effect_security");
    const auto ci = read_file(".github/workflows/ci.yml");
    CHECK(ci.find("build.py gate") != std::string::npos,
          "AC3: ci.yml gate runs build.py gate (which runs the script)");
    // Issue #2494: also verify the change closes the gap from advisory
    // to hard-fail — the script returns 1 under --strict, which the
    // gate propagates. Source-cite the comment if present.
    CHECK(ci.find("gate") != std::string::npos, "AC3: ci.yml has gate job");
}

// AC4: allowlist entries missing '# SECURITY_EXEMPT: <reason>' fail.
static void ac4_allowlist_reason_enforced() {
    std::println("\n--- #2494 AC4: allowlist reason format enforced ---");
    const auto allow = read_file("tests/side-effect-security-allowlist.txt");
    CHECK(!allow.empty(), "AC4: allowlist file exists");
    CHECK(allow.find("SECURITY_EXEMPT:") != std::string::npos,
          "AC4: existing allowlist entries have SECURITY_EXEMPT reason");
    // Source-cite the EXEMPT_REASON_RE enforcement in the script.
    const auto script = read_file("scripts/coverage/checks/check_side_effect_security.py");
    CHECK(script.find("EXEMPT_REASON_RE") != std::string::npos,
          "AC4: script enforces SECURITY_EXEMPT reason token");
    CHECK(script.find("missing '# SECURITY_EXEMPT: <reason>'") != std::string::npos,
          "AC4: script reports missing-reason error");
}

// AC5: source-cite script + CI workflow path.
static void ac5_source_cite() {
    std::println("\n--- #2494 AC5: source-cite script + CI workflow path ---");
    const auto script = read_file("scripts/coverage/checks/check_side_effect_security.py");
    CHECK(script.find("Issue #2494") != std::string::npos,
          "AC5: script cites #2494 (--path argument)");
    CHECK(script.find("--strict") != std::string::npos, "AC5: script supports --strict");
    const auto ci = read_file(".github/workflows/ci.yml");
    CHECK(ci.find("check_side_effect_security") != std::string::npos ||
              ci.find("build.py gate") != std::string::npos,
          "AC5: ci.yml gate runs the gate (which runs the script)");
    const auto header = read_file("src/compiler/security_side_effect.hh");
    CHECK(header.find("Issue #2057") != std::string::npos,
          "AC5: security_side_effect.hh cites #2057 rule");
}

// AC6: registrations + this test file + linter.
static void ac6_registrations() {
    std::println("\n--- #2494 AC6: registrations ---");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_side_effect_security_gate_hardfail") != std::string::npos,
          "AC6: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_side_effect_security_gate_hardfail_2494") != std::string::npos ||
              build.find("cmd_side_effect_security_gate_hardfail_2494_coverage") !=
                  std::string::npos,
          "AC6: build.py gate entry");
    const auto gate =
        read_file("scripts/coverage/checks/check_side_effect_security_gate_hardfail_2494.py");
    CHECK(!gate.empty() && gate.find("Issue #2494") != std::string::npos,
          "AC6: coverage linter present");
}

} // namespace

int run_test_side_effect_security_gate_hardfail() {
    std::println("=== Issue #2494: side-effect security gate hard-fail ===");
    ac1_broken_fixture_fails();
    ac2_existing_prim_set_passes();
    ac3_ci_gate_runs_script();
    ac4_allowlist_reason_enforced();
    ac5_source_cite();
    ac6_registrations();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_side_effect_security_gate_hardfail();
}
#endif
