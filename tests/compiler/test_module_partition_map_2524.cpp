// @category: unit
// @reason: Issue #2524 — split giant modules (pass_manager Phase C + partition map).
//
//   AC1: Measurable reduction OR clear partition map (pass_manager facade
//        < ~50 KB; pass_pipeline_core + pass_impls exist)
//   AC2: No public API renames — facade re-exports; pass types still imported
//        via aura.compiler.pass_manager
//   AC3: Representative hot path still builds (this TU + pipeline types usable)
//   AC4: Module dependency graph documented (pass_manager.ixx partition map)
//   AC5: Modules build; no circular import (core ← impls ← facade)

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.pass_manager;
import aura.compiler.pass_pipeline_core;
import aura.compiler.ir;

namespace {

using aura::compiler::ComputeKindWrap;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::DefineDirtyMaskView;
using aura::compiler::InlinePass;
using aura::compiler::kPassManagerFacadeIssue;
using aura::compiler::pass_pipeline_runs_total;
using aura::ir::IRModule;
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

static std::size_t file_bytes(const char* path) {
    return read_file(path).size();
}

// ── AC1: size reduction + partition map ──
static void ac1_size_and_map() {
    std::println("\n--- AC1: measurable shrink + partition map ---");
    const auto facade = file_bytes("src/compiler/pass_manager.ixx");
    const auto core = file_bytes("src/compiler/pass_pipeline_core.ixx");
    const auto impls = file_bytes("src/compiler/pass_impls.ixx");
    const auto map = read_file("src/compiler/pass_manager.ixx");
    std::println("  pass_manager.ixx (facade)     = {} bytes", facade);
    std::println("  pass_pipeline_core.ixx        = {} bytes", core);
    std::println("  pass_impls.ixx                = {} bytes", impls);

    CHECK(facade > 0 && core > 0 && impls > 0, "AC1: partition files present");
    // Facade must be a thin re-export (well under 150–200 KB target).
    CHECK(facade < 20'000, "AC1: pass_manager facade < 20 KB");
    // Pipeline core alone is the hot import surface for folds/metrics.
    CHECK(core < 150'000, "AC1: pass_pipeline_core < 150 KB");
    // Combined must still exceed pre-split bulk (impls hold the passes).
    CHECK(impls > 100'000, "AC1: pass_impls holds extracted pass bodies");
    // Pre-split was ~260 KB in one unit; facade is the primary import name.
    CHECK(facade + 50'000 < 260'000, "AC1: primary facade much smaller than pre-split");

    CHECK(!map.empty(), "AC1: partition map header exists on facade");
    CHECK(map.find("Issue #2524") != std::string::npos || map.find("#2524") != std::string::npos,
          "AC1: map cites #2524");
    CHECK(map.find("pass_pipeline_core") != std::string::npos, "AC1: map lists pipeline core");
    CHECK(map.find("pass_impls") != std::string::npos, "AC1: map lists pass_impls");
    CHECK(map.find("Phase A") != std::string::npos && map.find("Phase B") != std::string::npos,
          "AC1: map documents evaluator/ast phases");
    CHECK(kPassManagerFacadeIssue == 2524, "AC1: facade issue stamp");
}

// ── AC2: public API aliases via facade ──
static void ac2_public_api() {
    std::println("\n--- AC2: public API still via pass_manager ---");
    // Types must be reachable through import aura.compiler.pass_manager
    // (this TU imports pass_manager + core). Smoke-construct defaultable types.
    ComputeKindWrap ck;
    ConstantFoldingWrap cf;
    InlinePass inl;
    (void)ck;
    (void)cf;
    (void)inl;
    CHECK(true, "AC2: pass types constructible");

    // Metrics from pipeline core still visible.
    const auto runs0 = pass_pipeline_runs_total.load(std::memory_order_relaxed);
    (void)runs0;
    CHECK(true, "AC2: pipeline metrics accessible");

    // DefineDirtyMaskView lives in core, re-exported.
    DefineDirtyMaskView view;
    (void)view;
    CHECK(true, "AC2: DefineDirtyMaskView accessible");

    const auto facade = read_file("src/compiler/pass_manager.ixx");
    CHECK(facade.find("export import aura.compiler.pass_pipeline_core") != std::string::npos,
          "AC2: facade re-exports pipeline core");
    CHECK(facade.find("export import aura.compiler.pass_impls") != std::string::npos,
          "AC2: facade re-exports pass_impls");
    CHECK(facade.find("No public API renames") != std::string::npos ||
              facade.find("no API renames") != std::string::npos ||
              facade.find("keep") != std::string::npos,
          "AC2: facade documents stable import name");
}

// ── AC3: representative compile / runtime smoke ──
static void ac3_hot_path_smoke() {
    std::println("\n--- AC3: hot-path smoke (empty module pipeline types) ---");
    IRModule mod;
    ComputeKindWrap ck;
    ck.run(mod);
    CHECK(!ck.has_error(), "AC3: run on empty module");
    // Constant folding wrap default-constructible and name/error surface.
    ConstantFoldingWrap cf;
    (void)cf.name();
    CHECK(true, "AC3: representative types usable without full service");
}

// ── AC4: dependency graph documented ──
static void ac4_dep_graph_doc() {
    std::println("\n--- AC4: dependency graph documented ---");
    const auto map = read_file("src/compiler/pass_manager.ixx");
    CHECK(map.find("Module partition map") != std::string::npos ||
              map.find("Facade only re-exports") != std::string::npos,
          "AC4: dependency notes on facade");
    CHECK(map.find("pass_impls") != std::string::npos &&
              map.find("pass_pipeline_core") != std::string::npos,
          "AC4: core ← impls relationship");
    const auto cmake = read_file("cmake/AuraModules.cmake");
    CHECK(cmake.find("pass_pipeline_core.ixx") != std::string::npos, "AC4: CMake lists core");
    CHECK(cmake.find("pass_impls.ixx") != std::string::npos, "AC4: CMake lists impls");
    // Order: core before impls before facade
    const auto p_core = cmake.find("pass_pipeline_core.ixx");
    const auto p_impl = cmake.find("pass_impls.ixx");
    const auto p_facade = cmake.find("pass_manager.ixx");
    CHECK(p_core != std::string::npos && p_impl != std::string::npos &&
              p_facade != std::string::npos,
          "AC4: all three in CMake");
    CHECK(p_core < p_impl && p_impl < p_facade, "AC4: CMake order core < impls < facade");
}

// ── AC5: no circular imports (source contract) ──
static void ac5_no_cycles() {
    std::println("\n--- AC5: no circular import regressions ---");
    const auto core = read_file("src/compiler/pass_pipeline_core.ixx");
    const auto impls = read_file("src/compiler/pass_impls.ixx");
    const auto facade = read_file("src/compiler/pass_manager.ixx");
    // Real import edges only — match start-of-line import (skip comments).
    auto has_line_import = [](const std::string& text, std::string_view mod) {
        const std::string needle = std::string("import ") + std::string(mod);
        const std::string export_needle = std::string("export import ") + std::string(mod);
        std::size_t pos = 0;
        while (pos < text.size()) {
            const auto line_end = text.find('\n', pos);
            const auto line = text.substr(pos, line_end == std::string::npos ? std::string::npos
                                                                             : line_end - pos);
            pos = line_end == std::string::npos ? text.size() : line_end + 1;
            // Trim leading space
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                ++i;
            if (i < line.size() && line[i] == '/')
                continue; // comment
            const auto body = line.substr(i);
            if (body.starts_with(export_needle) || body.starts_with(needle))
                return true;
        }
        return false;
    };
    CHECK(!has_line_import(core, "aura.compiler.pass_impls"), "AC5: core does not import impls");
    CHECK(!has_line_import(core, "aura.compiler.pass_manager"), "AC5: core does not import facade");
    CHECK(core.find("export module aura.compiler.pass_pipeline_core") != std::string::npos,
          "AC5: core module name");
    CHECK(has_line_import(impls, "aura.compiler.pass_pipeline_core"), "AC5: impls imports core");
    CHECK(!has_line_import(impls, "aura.compiler.pass_manager"),
          "AC5: impls does not import facade (no cycle)");
    CHECK(facade.find("export module aura.compiler.pass_manager") != std::string::npos,
          "AC5: facade module name");
    CHECK(facade.find("Issue #2524") != std::string::npos ||
              facade.find("#2524") != std::string::npos,
          "AC5: facade cites #2524");
}

} // namespace

int run_test_module_partition_map_2524() {
    std::println("=== Issue #2524: module partition map + pass_manager Phase C ===");
    ac1_size_and_map();
    ac2_public_api();
    ac3_hot_path_smoke();
    ac4_dep_graph_doc();
    ac5_no_cycles();
    std::println("\n=== #2524: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_module_partition_map_2524();
}
#endif
