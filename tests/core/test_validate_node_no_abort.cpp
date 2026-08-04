// @category: unit
// @reason: Issue #2390 — FlatAST::validate_node must not hard-abort on
// !is_valid; report / throw so post-restore recovery stays non-crashing.
//
//   AC1: validate_post_restore with corrupt gen returns PostRestoreReport
//        (violations listed), does not abort
//   AC2: validate_node(id, fail_on_error=true) on invalid id throws logic_error
//   AC3: validate_node(id, fail_on_error=false) on invalid id returns error string
//   AC4: TSAN/ASan clean (no abort; exercised by this unit path)
//   AC5: this test + source-cite + gate

#include "test_harness.hpp"

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::ast::PostRestoreReport;
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

// ── AC1: corrupt gen → post-restore report, no abort ──
static void ac1_post_restore_corrupt_reports() {
    std::println("\n--- #2390 AC1: validate_post_restore corrupt gen reports ---");
    FlatAST flat;
    const auto id0 = flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    const auto id1 = flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    CHECK(flat.is_valid(id0), "AC1: id0 valid before bump");
    CHECK(flat.is_valid(id1), "AC1: id1 valid before bump");

    // Bump global generation without restamping node_gen_ → both slots
    // are neither live (gen match) nor tombstone (gen==0).
    flat.bump_generation();
    CHECK(!flat.is_valid(id0), "AC1: id0 invalid after gen bump without restamp");
    CHECK(!flat.restamp_lazy_align_enabled(), "AC1: lazy-align off by default");

    std::vector<FlatAST::ValidationError> errors;
    PostRestoreReport report;
    // Must not abort (pre-#2390 abort was in validate_node; post-restore
    // has its own path — still verify no crash + violations listed).
    report = flat.validate_post_restore(&errors);
    std::println("  post-restore violations={} live={} free={} gen={} errors={}", report.violations,
                 report.live_nodes, report.free_slots, report.generation, errors.size());
    CHECK(report.violations > 0, "AC1: PostRestoreReport.violations > 0");
    CHECK(report.generation == flat.generation(), "AC1: report generation matches");
    CHECK(!errors.empty(), "AC1: errors vector populated");
    bool found_gen_msg = false;
    for (const auto& e : errors) {
        if (e.message.find("neither live nor tombstone") != std::string::npos ||
            e.message.find("generation") != std::string::npos) {
            found_gen_msg = true;
            break;
        }
    }
    CHECK(found_gen_msg, "AC1: error message mentions gen mismatch / neither live");
}

// ── AC2: fail_on_error=true throws ──
static void ac2_validate_node_throws() {
    std::println("\n--- #2390 AC2: validate_node(fail_on_error=true) throws ---");
    FlatAST flat;
    const auto id = flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    flat.bump_generation();
    CHECK(!flat.is_valid(id), "AC2: id invalid");

    bool threw = false;
    try {
        (void)flat.validate_node(id, /*fail_on_error=*/true);
    } catch (const std::logic_error& e) {
        threw = true;
        std::println("  logic_error: {}", e.what());
        CHECK(std::string_view(e.what()).find("not valid") != std::string_view::npos,
              "AC2: message says not valid");
    } catch (...) {
        CHECK(false, "AC2: unexpected exception type (want std::logic_error)");
    }
    CHECK(threw, "AC2: threw std::logic_error");

    // Out-of-range id also throws (not abort).
    threw = false;
    try {
        (void)flat.validate_node(static_cast<aura::ast::NodeId>(flat.size() + 100),
                                 /*fail_on_error=*/true);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw, "AC2: out-of-range id throws (not abort)");
}

// ── AC3: fail_on_error=false returns error string ──
static void ac3_validate_node_returns_msg() {
    std::println("\n--- #2390 AC3: validate_node(fail_on_error=false) returns msg ---");
    FlatAST flat;
    const auto id = flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    flat.bump_generation();

    const auto msg = flat.validate_node(id, /*fail_on_error=*/false);
    std::println("  msg={}", msg);
    CHECK(!msg.empty(), "AC3: non-empty error string");
    CHECK(msg.find("not valid") != std::string::npos, "AC3: mentions not valid");

    // validate_all_nodes(false) must also not abort over free/stale slots.
    const auto freed = flat.free_orphan_nodes_from(0);
    CHECK(freed >= 1, "AC3: freed at least one slot");
    const auto violations = flat.validate_all_nodes(/*fail_on_error=*/false);
    std::println("  validate_all_nodes violations={}", violations);
    CHECK(violations >= 1, "AC3: validate_all_nodes reports free slots without abort");
}

// ── AC4: valid path still clean ──
static void ac4_valid_path_clean() {
    std::println("\n--- #2390 AC4: valid node still validates clean ---");
    FlatAST flat;
    const auto id = flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    CHECK(flat.is_valid(id), "AC4: fresh id valid");
    const auto msg = flat.validate_node(id, /*fail_on_error=*/false);
    CHECK(msg.empty(), "AC4: valid node returns empty string");
    auto report = flat.validate_post_restore(nullptr);
    CHECK(report.violations == 0, "AC4: healthy workspace zero violations");
    CHECK(report.live_nodes >= 1, "AC4: live_nodes counted");
}

// ── AC5: source-cite + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- #2390 AC5: source-cite + gate ---");
    const auto impl = read_file("src/core/ast_impl.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto linter = read_file("scripts/coverage/checks/check_validate_node_no_abort_2390.py");

    CHECK(impl.find("Issue #2390") != std::string::npos, "AC5: cites #2390");
    CHECK(impl.find("std::abort()") == std::string::npos ||
              impl.find("if (!is_valid(id))\n        std::abort()") == std::string::npos,
          "AC5: no hard-abort on !is_valid");
    // Positive: throw / return path present.
    CHECK(impl.find("node ID is not valid") != std::string::npos,
          "AC5: error message for invalid id");
    CHECK(impl.find("fail_on_error") != std::string::npos, "AC5: respects fail_on_error");
    CHECK(impl.find("throw std::logic_error") != std::string::npos, "AC5: throws logic_error");
    CHECK(cmake.find("test_validate_node_no_abort") != std::string::npos, "AC5: CMake");
    CHECK(build.find("check_validate_node_no_abort_2390") != std::string::npos ||
              build.find("cmd_validate_node_no_abort_coverage") != std::string::npos,
          "AC5: build.py gate");
    CHECK(!linter.empty(), "AC5: coverage linter present");
}

} // namespace

int run_test_validate_node_no_abort() {
    std::println("=== Issue #2390: validate_node no hard-abort on !is_valid ===");
    ac1_post_restore_corrupt_reports();
    ac2_validate_node_throws();
    ac3_validate_node_returns_msg();
    ac4_valid_path_clean();
    ac5_source_and_gate();
    std::println("\n=== #2390 results: passed={} failed={} ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_validate_node_no_abort();
}
#endif
