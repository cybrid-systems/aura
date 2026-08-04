// @category: unit
// @reason: Issue #2391 — validate_post_restore must detect SoA column
// size drift (int_val_/sym_id_/node_gen_/… vs size()), not only gen/parent.
//
//   AC1: sym_id_ size != tag_.size() → PostRestoreReport size-mismatch
//   AC2: happy-path healthy FlatAST → zero SoA size violations
//   AC3: this test + source-cite + gate
//   AC4: no abort / clean unit path (ASan/TSAN via normal gate)

#include "test_harness.hpp"

#include <fstream>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::ast::PostRestoreReport;
using aura::ast::SymId;
using aura::ast::SyntaxMarker;
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

static bool has_soa_mismatch(const std::vector<FlatAST::ValidationError>& errors,
                             std::string_view col_hint = {}) {
    for (const auto& e : errors) {
        if (e.message.find("SoA column") == std::string::npos)
            continue;
        if (col_hint.empty() || e.message.find(col_hint) != std::string::npos)
            return true;
    }
    return false;
}

// ── AC1: SoA drift via truncated sym_id restore ──
static void ac1_soa_sym_id_drift() {
    std::println("\n--- #2391 AC1: sym_id_ size drift reported ---");
    FlatAST flat;
    (void)flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    (void)flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    (void)flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    CHECK(flat.size() == 3, "AC1: three nodes");

    // Truncate sym_id_ only (public restore path used by rename rollback).
    auto snap = flat.snapshot_sym_id();
    CHECK(snap.size() == 3, "AC1: snap size 3");
    snap.pop_back();
    CHECK(snap.size() == 2, "AC1: truncated snap size 2");
    flat.restore_sym_id(std::move(snap));

    std::vector<FlatAST::ValidationError> errors;
    const auto report = flat.validate_post_restore(&errors);
    std::println("  violations={} errors={}", report.violations, errors.size());
    for (const auto& e : errors)
        std::println("    node={} msg={}", e.node, e.message);

    CHECK(report.violations > 0, "AC1: violations > 0 on SoA drift");
    CHECK(has_soa_mismatch(errors, "sym_id_"), "AC1: SoA size-mismatch mentions sym_id_");
    // Message shape: SoA column 'sym_id_' size N != size() M
    bool shape_ok = false;
    for (const auto& e : errors) {
        if (e.message.find("sym_id_") != std::string::npos &&
            e.message.find("!= size()") != std::string::npos) {
            shape_ok = true;
            break;
        }
    }
    CHECK(shape_ok, "AC1: message has 'size N != size() M' shape");
}

// ── AC1b: metadata column drift (restore_metadata does not pad) ──
static void ac1b_metadata_drift() {
    std::println("\n--- #2391 AC1b: marker_/dirty_ size drift reported ---");
    FlatAST flat;
    (void)flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    (void)flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    auto meta = flat.snapshot_metadata_columns();
    CHECK(meta.marker.size() == 2, "AC1b: meta snap size 2");
    meta.marker.pop_back();
    meta.dirty.pop_back();
    // Leave provenance/macro_dirty full — mixed drift still reports.
    flat.restore_metadata_columns(std::move(meta));

    std::vector<FlatAST::ValidationError> errors;
    const auto report = flat.validate_post_restore(&errors);
    std::println("  violations={}", report.violations);
    CHECK(report.violations > 0, "AC1b: violations on metadata SoA drift");
    CHECK(has_soa_mismatch(errors, "marker_") || has_soa_mismatch(errors, "dirty_"),
          "AC1b: reports marker_ or dirty_ SoA mismatch");
}

// ── AC2: happy path no false positive ──
static void ac2_happy_path() {
    std::println("\n--- #2391 AC2: healthy FlatAST zero SoA violations ---");
    FlatAST flat;
    for (int i = 0; i < 8; ++i)
        (void)flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    // Add a small tree edge if API allows.
    const auto a = flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    const auto b = flat.add_node(NodeTag::Variable, SyntaxMarker::User);
    (void)a;
    (void)b;

    std::vector<FlatAST::ValidationError> errors;
    const auto report = flat.validate_post_restore(&errors);
    std::println("  healthy violations={} live={} free={}", report.violations, report.live_nodes,
                 report.free_slots);
    CHECK(report.violations == 0, "AC2: healthy workspace zero violations");
    CHECK(!has_soa_mismatch(errors), "AC2: no SoA size-mismatch on happy path");
    CHECK(report.live_nodes == flat.size(), "AC2: live_nodes == size()");
}

// ── AC3/AC4: source-cite + gate ──
static void ac3_source_and_gate() {
    std::println("\n--- #2391 AC3: source-cite + gate ---");
    const auto impl = read_file("src/core/ast_impl.cpp");
    const auto ixx = read_file("src/core/ast.ixx");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto linter =
        read_file("scripts/coverage/checks/check_validate_post_restore_soa_2391.py");

    CHECK(impl.find("Issue #2391") != std::string::npos, "AC3: cites #2391 in ast_impl");
    CHECK(impl.find("record_size_mismatch") != std::string::npos ||
              impl.find("SoA column") != std::string::npos,
          "AC3: SoA size check present");
    CHECK(impl.find("int_val_") != std::string::npos, "AC3: checks int_val_");
    CHECK(impl.find("sym_id_") != std::string::npos, "AC3: checks sym_id_");
    CHECK(impl.find("node_gen_") != std::string::npos, "AC3: checks node_gen_");
    CHECK(impl.find("type_id_") != std::string::npos, "AC3: checks type_id_");
    CHECK(ixx.find("#2391") != std::string::npos || ixx.find("2391") != std::string::npos,
          "AC3: ast.ixx documents #2391");
    CHECK(cmake.find("test_validate_post_restore_soa") != std::string::npos, "AC3: CMake");
    CHECK(build.find("check_validate_post_restore_soa_2391") != std::string::npos ||
              build.find("cmd_validate_post_restore_soa_coverage") != std::string::npos,
          "AC3: build.py gate");
    CHECK(!linter.empty(), "AC3: coverage linter present");
    (void)SymId{}; // keep type visible if needed
}

} // namespace

int run_test_validate_post_restore_soa() {
    std::println("=== Issue #2391: validate_post_restore SoA size cross-check ===");
    ac1_soa_sym_id_drift();
    ac1b_metadata_drift();
    ac2_happy_path();
    ac3_source_and_gate();
    std::println("\n=== #2391 results: passed={} failed={} ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_validate_post_restore_soa();
}
#endif
