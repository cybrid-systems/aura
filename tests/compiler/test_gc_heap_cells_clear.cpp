// @category: unit
// @reason: Issue #2486 — gc-heap fallback clears ev.cells_ (stale cell
//          data must not survive a "stronger" heap reset).
//
//   AC1: after seeding cells + gc-heap, cells().size() == 0
//   AC2: pairs/string also reset; cells stay 0 after second seed path
//   AC3: source cites #2486 + cells_.clear in fallback
//   AC4: gate wiring

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::compiler::types::make_int;
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

// Seed cells via public cells() mutator (no top-level (cell) prim).
static void seed_cells(CompilerService& cs, std::size_t n) {
    auto& cells = cs.evaluator().cells();
    for (std::size_t i = 0; i < n; ++i)
        cells.push_back(make_int(static_cast<std::int64_t>(i + 1)));
}

// AC1: seed cells then gc-heap → cells empty
static void ac1_cells_cleared() {
    std::println("\n--- #2486 AC1: cells_ cleared after gc-heap ---");
    CompilerService cs;
    seed_cells(cs, 3);
    CHECK(cs.evaluator().cells().size() == 3, "AC1: cells seeded");
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value() && is_bool(*r) && as_bool(*r), "AC1: gc-heap returns #t");
    CHECK(cs.evaluator().cells().size() == 0, "AC1: cells().size() == 0 after gc-heap");
    CHECK(cs.evaluator().pairs().empty(), "AC1: pairs empty after gc-heap");
}

// AC2: re-seed after gc-heap starts at empty; second gc-heap zeros again
static void ac2_fresh_after_reset() {
    std::println("\n--- #2486 AC2: post-gc reseed is fresh ---");
    CompilerService cs;
    seed_cells(cs, 5);
    CHECK(cs.evaluator().cells().size() == 5, "AC2: pre-gc cells");
    (void)cs.eval("(gc-heap)");
    CHECK(cs.evaluator().cells().size() == 0, "AC2: cells:0 after first gc-heap");
    seed_cells(cs, 2);
    CHECK(cs.evaluator().cells().size() == 2, "AC2: reseed 2 cells (no stale tail)");
    (void)cs.eval("(gc-heap)");
    CHECK(cs.evaluator().cells().size() == 0, "AC2: cells:0 after second gc-heap");
}

// AC3: source
static void ac3_source() {
    std::println("\n--- #2486 AC3: source contracts ---");
    auto src = read_file("src/compiler/evaluator_primitives_memory.cpp");
    CHECK(!src.empty(), "AC3: read memory primitives");
    CHECK(src.find("Issue #2486") != std::string::npos, "AC3: cites #2486");
    auto pos = src.find("add(\"gc-heap\"");
    CHECK(pos != std::string::npos, "AC3: gc-heap present");
    if (pos != std::string::npos) {
        auto win = src.substr(pos, 5000);
        CHECK(win.find("cells_.clear()") != std::string::npos, "AC3: cells_.clear()");
        CHECK(win.find("cells_.shrink_to_fit()") != std::string::npos,
              "AC3: cells_.shrink_to_fit()");
        CHECK(win.find("string_heap_.clear()") != std::string::npos, "AC3: string_heap clear");
        CHECK(win.find("pairs_.clear()") != std::string::npos, "AC3: pairs clear");
        CHECK(win.find("vector_heap_.clear()") != std::string::npos, "AC3: vector_heap clear");
        CHECK(win.find("opaque_heap_.clear()") != std::string::npos, "AC3: opaque_heap clear");
    }
}

// AC4: gate
static void ac4_gate() {
    std::println("\n--- #2486 AC4: gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    CHECK(build.find("check_gc_heap_cells_clear_2486") != std::string::npos,
          "AC4: check script in build.py");
    CHECK(build.find("cmd_gc_heap_cells_clear_coverage") != std::string::npos, "AC4: coverage cmd");
    CHECK(cmake.find("test_gc_heap_cells_clear") != std::string::npos, "AC4: cmake test");
    CHECK(!read_file("scripts/coverage/checks/check_gc_heap_cells_clear_2486.py").empty(),
          "AC4: check script exists");
}

} // namespace

int run_test_gc_heap_cells_clear() {
    std::println("=== Issue #2486: gc-heap clears cells_ ===");
    ac1_cells_cleared();
    ac2_fresh_after_reset();
    ac3_source();
    ac4_gate();
    std::println("\n=== #2486 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_gc_heap_cells_clear();
}
#endif
