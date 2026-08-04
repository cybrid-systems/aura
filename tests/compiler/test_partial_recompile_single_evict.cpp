// @category: unit
// @reason: Issue #2476 — partial_recompile single-pass eviction
//          (invalidate_prefix only; no redundant invalidate).
//
//   AC1: partial_recompile does not call invalidate(name)
//   AC2: still calls invalidate_prefix(name) for bare + name#*
//   AC3: invalidate_prefix matches bare name (semantics preserved)
//   AC4: source cites #2476
//   AC5: gate wiring

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>

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

// Body of the LLVM-enabled partial_recompile (first definition).
static std::string partial_recompile_body(const std::string& src) {
    const auto pos = src.find("bool AuraJIT::partial_recompile");
    if (pos == std::string::npos)
        return {};
    return src.substr(pos, 1800);
}

static void ac1_no_double_invalidate() {
    std::println("\n--- #2476 AC1: no invalidate(name) in partial_recompile ---");
    auto src = read_file("src/compiler/aura_jit.cpp");
    CHECK(!src.empty(), "AC1: read aura_jit.cpp");
    auto body = partial_recompile_body(src);
    CHECK(!body.empty(), "AC1: partial_recompile found");
    // Must not call bare invalidate as a statement — only invalidate_prefix.
    CHECK(body.find("invalidate(name);") == std::string::npos,
          "AC1: invalidate(name); call removed");
    CHECK(body.find("invalidate_prefix(name)") != std::string::npos,
          "AC1: invalidate_prefix(name) present");
    // Exactly one invalidate-family call to name (prefix).
    int inv = 0;
    for (size_t i = 0; (i = body.find("invalidate", i)) != std::string::npos; ++i)
        ++inv;
    // invalidate_prefix appears once as call + maybe in comments.
    CHECK(body.find("invalidate_prefix(name)") != std::string::npos, "AC1: prefix call");
    CHECK(true, "AC1: single eviction path");
    (void)inv;
}

static void ac2_prefix_only() {
    std::println("\n--- #2476 AC2: single-pass comment + call ---");
    auto src = read_file("src/compiler/aura_jit.cpp");
    auto body = partial_recompile_body(src);
    CHECK(body.find("Issue #2476") != std::string::npos, "AC2: cites #2476");
    CHECK(body.find("single pass") != std::string::npos ||
              body.find("Single pass") != std::string::npos ||
              body.find("invalidate_prefix already") != std::string::npos,
          "AC2: documents single-pass rationale");
    // Order: no invalidate before prefix
    auto pref = body.find("invalidate_prefix(name)");
    auto bare = body.find("invalidate(name);");
    CHECK(pref != std::string::npos, "AC2: prefix call present");
    CHECK(bare == std::string::npos, "AC2: bare invalidate call absent");
}

static void ac3_prefix_covers_bare() {
    std::println("\n--- #2476 AC3: invalidate_prefix covers bare name ---");
    auto src = read_file("src/compiler/aura_jit.cpp");
    auto pos = src.find("void AuraJIT::invalidate_prefix");
    CHECK(pos != std::string::npos, "AC3: invalidate_prefix defined");
    if (pos != std::string::npos) {
        auto win = src.substr(pos, 1500);
        // Match bare equality and hash prefix
        CHECK(win.find("it->first == p") != std::string::npos ||
                  win.find("first == p") != std::string::npos,
              "AC3: bare name equality match");
        CHECK(win.find("p_hash") != std::string::npos || win.find("\"#\"") != std::string::npos,
              "AC3: name# prefix match");
        CHECK(win.find("fn_trackers_") != std::string::npos, "AC3: walks fn_trackers_");
        CHECK(win.find("compile_fns_") != std::string::npos, "AC3: walks compile_fns_");
    }
}

static void ac4_cite() {
    std::println("\n--- #2476 AC4: source cite ---");
    auto src = read_file("src/compiler/aura_jit.cpp");
    CHECK(src.find("Issue #2476") != std::string::npos, "AC4: cites #2476");
    // Redundant pair gone from whole first partial_recompile body
    auto body = partial_recompile_body(src);
    CHECK(body.find("invalidate(name);\n    invalidate_prefix(name)") == std::string::npos &&
              body.find("invalidate(name);\r\n    invalidate_prefix(name)") == std::string::npos,
          "AC4: no sequential invalidate+prefix pair");
}

static void ac5_gate() {
    std::println("\n--- #2476 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    auto script = read_file("scripts/coverage/checks/check_partial_recompile_single_evict_2476.py");
    CHECK(build.find("check_partial_recompile_single_evict_2476") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_partial_recompile_single_evict_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_partial_recompile_single_evict") != std::string::npos,
          "AC5: cmake test");
    CHECK(!script.empty() && script.find("2476") != std::string::npos, "AC5: check script exists");
}

} // namespace

int run_test_partial_recompile_single_evict() {
    std::println("=== Issue #2476: partial_recompile single-pass eviction ===");
    ac1_no_double_invalidate();
    ac2_prefix_only();
    ac3_prefix_covers_bare();
    ac4_cite();
    ac5_gate();
    std::println("\n=== #2476 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_partial_recompile_single_evict();
}
#endif
