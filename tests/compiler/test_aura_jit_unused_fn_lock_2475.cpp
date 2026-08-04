// @category: unit
// @reason: Issue #2475 — remove unused default-constructed fn_lock in
//          AuraJIT::Impl::compile(); document real locking strategy.
//
//   AC1: fn_lock removed from compile()
//   AC2: comments reflect compile_mtx_ global + fn_compile_mtx_ cache-only
//   AC3: cache lookup still uses shared_lock(fn_compile_mtx_)
//   AC4: source cites #2475
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

// Extract AuraJIT::Impl::compile body (from ScalarFn compile to a later
// method bound). Size is large; use a generous window.
static std::string compile_body(const std::string& src) {
    const auto pos = src.find("ScalarFn compile(const FlatFunction");
    if (pos == std::string::npos)
        return {};
    // Cover comments + cache lookup + publish site (~4k is enough for start;
    // also search whole file for patterns that must not reappear).
    return src.substr(pos, 4500);
}

static void ac1_fn_lock_removed() {
    std::println("\n--- #2475 AC1: fn_lock removed ---");
    auto src = read_file("src/compiler/aura_jit.cpp");
    CHECK(!src.empty(), "AC1: read aura_jit.cpp");
    auto body = compile_body(src);
    CHECK(!body.empty(), "AC1: compile() found");
    // Dead unique_lock variable must be gone (comment may still say "fn_lock").
    CHECK(body.find("unique_lock<std::shared_mutex> fn_lock") == std::string::npos,
          "AC1: no default-constructed unique_lock fn_lock");
    CHECK(body.find("std::unique_lock") == std::string::npos ||
              body.find("fn_lock;") == std::string::npos,
          "AC1: no fn_lock; declaration");
    // Must still lock compile_mtx_ and use fn_compile_mtx_ for cache.
    CHECK(body.find("compile_mtx_") != std::string::npos, "AC1: compile_mtx_ held");
    CHECK(body.find("fn_compile_mtx_") != std::string::npos, "AC1: fn_compile_mtx_ used");
}

static void ac2_comments() {
    std::println("\n--- #2475 AC2: comments match actual locking ---");
    auto src = read_file("src/compiler/aura_jit.cpp");
    auto body = compile_body(src);
    CHECK(body.find("Issue #2475") != std::string::npos, "AC2: cites #2475 in compile()");
    // Misleading "run in parallel" claim near fn_lock must be gone.
    CHECK(body.find("Two threads compiling DIFFERENT functions run in parallel") ==
              std::string::npos,
          "AC2: removed parallel-compile claim");
    // Document global serializer + cache-only role.
    CHECK(body.find("global serializer") != std::string::npos ||
              body.find("compile_mtx_ is the global") != std::string::npos ||
              body.find("serialize") != std::string::npos,
          "AC2: documents global serialize");
    CHECK(body.find("cache") != std::string::npos, "AC2: documents cache role of fn_compile_mtx_");
}

static void ac3_shared_lookup() {
    std::println("\n--- #2475 AC3: shared_lock cache lookup retained ---");
    auto src = read_file("src/compiler/aura_jit.cpp");
    auto body = compile_body(src);
    CHECK(body.find("shared_lock") != std::string::npos, "AC3: shared_lock present");
    CHECK(body.find("compile_fns_") != std::string::npos, "AC3: compile_fns_ cache lookup");
    // Publish still unique_locks fn_compile_mtx_ (later in function).
    const auto pub = src.find("compile_fns_[std::string(fn.name)]");
    CHECK(pub != std::string::npos, "AC3: cache publish present");
    if (pub != std::string::npos) {
        // Look back a bit for unique_lock
        const auto win_start = pub > 400 ? pub - 400 : 0;
        auto win = src.substr(win_start, 500);
        CHECK(win.find("unique_lock") != std::string::npos, "AC3: unique_lock on publish");
        CHECK(win.find("fn_compile_mtx_") != std::string::npos,
              "AC3: publish under fn_compile_mtx_");
    }
}

static void ac4_cite() {
    std::println("\n--- #2475 AC4: source cite ---");
    auto src = read_file("src/compiler/aura_jit.cpp");
    CHECK(src.find("Issue #2475") != std::string::npos, "AC4: cites #2475");
    CHECK(src.find("removed unused") != std::string::npos ||
              src.find("fn_lock") == std::string::npos ||
              src.find("default-constructed") != std::string::npos,
          "AC4: documents removal");
    // Member comment also fixed
    CHECK(src.find("does not enable parallel LLVM compiles") != std::string::npos ||
              src.find("cache map access only") != std::string::npos,
          "AC4: member comment corrected");
}

static void ac5_gate() {
    std::println("\n--- #2475 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    auto script = read_file("scripts/coverage/checks/check_aura_jit_unused_fn_lock_2475.py");
    CHECK(build.find("check_aura_jit_unused_fn_lock_2475") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_aura_jit_unused_fn_lock_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_aura_jit_unused_fn_lock_2475") != std::string::npos, "AC5: cmake test");
    CHECK(!script.empty() && script.find("2475") != std::string::npos, "AC5: check script exists");
}

} // namespace

int main() {
    std::println("=== Issue #2475: remove unused fn_lock in AuraJIT::compile ===");
    ac1_fn_lock_removed();
    ac2_comments();
    ac3_shared_lookup();
    ac4_cite();
    ac5_gate();
    std::println("\n=== #2475 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
