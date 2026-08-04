// @category: unit
// @reason: Issue #2477 — emit_object fail-closed deprecation
//          (no misleading true after writing .ir).
//
//   AC1: emit_object returns false
//   AC2: does not create out_path or out_path+".ir"
//   AC3: source/docs cite deprecation + emit_native_object
//   AC4: no non-test production callers of emit_object
//   AC5: gate wiring

#include "test_harness.hpp"

#include "compiler/aura_jit.h"

#include <cstdio>
#include <fstream>
#include <print>
#include <string>
#include <vector>

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

static bool file_exists(const std::string& path) {
    std::ifstream in(path);
    return static_cast<bool>(in);
}

static void ac1_returns_false() {
    std::println("\n--- #2477 AC1: emit_object returns false ---");
    const auto ok = aura::jit::emit_object("; dummy ir\n", "/tmp/aura_2477_emit_object.o");
    CHECK(ok == false, "AC1: returns false");
}

static void ac2_no_files() {
    std::println("\n--- #2477 AC2: no .o or .ir written ---");
    const std::string base = "/tmp/aura_2477_no_write";
    const std::string o_path = base + ".o";
    const std::string ir_path = o_path + ".ir"; // old bug wrote out_path+".ir"
    // Clean any leftovers
    std::remove(o_path.c_str());
    std::remove(ir_path.c_str());
    std::remove((base + ".ir").c_str());

    (void)aura::jit::emit_object("define @f() {}\n", o_path);
    CHECK(!file_exists(o_path), "AC2: no .o at out_path");
    CHECK(!file_exists(ir_path), "AC2: no out_path.ir side file");
    CHECK(!file_exists(o_path + ".ir"), "AC2: no .o.ir either");
}

static void ac3_source_cite() {
    std::println("\n--- #2477 AC3: source deprecation contract ---");
    auto cpp = read_file("src/compiler/aura_jit.cpp");
    auto hdr = read_file("src/compiler/aura_jit.h");
    CHECK(!cpp.empty(), "AC3: read aura_jit.cpp");
    CHECK(cpp.find("Issue #2477") != std::string::npos, "AC3: cites #2477");
    CHECK(cpp.find("emit_object: deprecated, use emit_native_object instead") != std::string::npos,
          "AC3: stderr deprecation message");
    // Both LLVM and no-LLVM stubs return false
    auto pos = cpp.find("bool emit_object(const std::string");
    CHECK(pos != std::string::npos, "AC3: emit_object defined");
    if (pos != std::string::npos) {
        auto win = cpp.substr(pos, 600);
        CHECK(win.find("return false") != std::string::npos, "AC3: returns false in body");
        CHECK(win.find("return true") == std::string::npos, "AC3: no return true");
        CHECK(win.find(".ir") == std::string::npos ||
                  win.find("out_path + \".ir\"") == std::string::npos,
              "AC3: no .ir fopen write path");
    }
    CHECK(hdr.find("2477") != std::string::npos || hdr.find("Deprecated") != std::string::npos,
          "AC3: header documents deprecation");
    CHECK(hdr.find("emit_native_object") != std::string::npos, "AC3: header points to native API");
}

static void ac4_no_production_callers() {
    std::println("\n--- #2477 AC4: no production emit_object callers ---");
    // Grep-style: scan known call sites in source tree via reading key files.
    // Production should use emit_native_object only.
    auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    auto main_cpp = read_file("src/main.cpp");
    // Must not call aura::jit::emit_object( in bridge/main
    CHECK(bridge.find("emit_object(") == std::string::npos ||
              bridge.find("emit_native_object") != std::string::npos,
          "AC4: bridge uses native path");
    // Harder: bridge should not call emit_object without native prefix
    // Scan for "jit::emit_object" or "aura::jit::emit_object"
    CHECK(bridge.find("jit::emit_object(") == std::string::npos,
          "AC4: no jit::emit_object in bridge");
    CHECK(main_cpp.find("jit::emit_object(") == std::string::npos,
          "AC4: no jit::emit_object in main");
    // Supported path present
    CHECK(bridge.find("emit_native_object") != std::string::npos,
          "AC4: bridge has emit_native_object");
}

static void ac5_gate() {
    std::println("\n--- #2477 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    auto script = read_file("scripts/coverage/checks/check_emit_object_deprecated_2477.py");
    CHECK(build.find("check_emit_object_deprecated_2477") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_emit_object_deprecated_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_emit_object_deprecated_2477") != std::string::npos, "AC5: cmake test");
    CHECK(!script.empty() && script.find("2477") != std::string::npos, "AC5: check script exists");
}

} // namespace

int run_test_emit_object_deprecated_2477() {
    std::println("=== Issue #2477: emit_object fail-closed deprecation ===");
    ac1_returns_false();
    ac2_no_files();
    ac3_source_cite();
    ac4_no_production_callers();
    ac5_gate();
    std::println("\n=== #2477 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_emit_object_deprecated_2477();
}
#endif
