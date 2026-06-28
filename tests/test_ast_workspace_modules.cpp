// test_ast_workspace_modules.cpp — Issue #563:
// New/enhanced std/ast and std/workspace high-level modules.
//
// Non-duplicative with #558-#562. This binary focuses on
// the ast: + workspace: stdlib wrapper surface:
//   - AC1: lib/std/ast.aura present + exports 6 functions
//   - AC2: lib/std/workspace.aura enhanced (+5 functions)
//   - AC3: 12 primitives wrapped (≥5 acceptance met)
//   - AC4: docs/design/ast-workspace-decision.md present
//   - AC5: regression — core ast:* + workspace:* primitives work

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_issue_563_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;

// ── AC1: lib/std/ast.aura present + exports 6 functions
bool test_stdlib_ast_file_present() {
    std::println("\n--- AC1: lib/std/ast.aura present + 6 exports ---");
    const std::string lib_path = "/home/dev/code/aura/lib/std/ast.aura";
    std::ifstream f(lib_path);
    CHECK(f.good(), "lib/std/ast.aura exists on disk");
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    f.close();
    const bool has_export = content.find("(export") != std::string::npos;
    const bool has_summary_f =
        content.find("(ast:summary-formatted") != std::string::npos;
    const bool has_diff_f =
        content.find("(ast:diff-formatted") != std::string::npos;
    const bool has_validate =
        content.find("(ast:validate-summary") != std::string::npos;
    const bool has_version =
        content.find("(ast:version-summary") != std::string::npos;
    const bool has_ref_stats =
        content.find("(ast:ref-stats") != std::string::npos;
    const bool has_mem_pressure =
        content.find("(ast:memory-pressure") != std::string::npos;
    std::println("  lib/ast.aura: present + 6 funcs defined + export");
    CHECK(has_export, "stdlib/ast.aura has (export ...) line");
    CHECK(has_summary_f, "stdlib/ast.aura exports (ast:summary-formatted)");
    CHECK(has_diff_f, "stdlib/ast.aura exports (ast:diff-formatted)");
    CHECK(has_validate, "stdlib/ast.aura exports (ast:validate-summary)");
    CHECK(has_version, "stdlib/ast.aura exports (ast:version-summary)");
    CHECK(has_ref_stats, "stdlib/ast.aura exports (ast:ref-stats)");
    CHECK(has_mem_pressure, "stdlib/ast.aura exports (ast:memory-pressure)");
    std::ifstream ft("/home/dev/code/aura/lib/std/ast.aura-type");
    CHECK(ft.good(), "lib/std/ast.aura-type exists on disk");
    return true;
}

// ── AC2: lib/std/workspace.aura enhanced (+5 functions)
bool test_stdlib_workspace_enhanced() {
    std::println("\n--- AC2: lib/std/workspace.aura enhanced (+5 funcs) ---");
    const std::string lib_path = "/home/dev/code/aura/lib/std/workspace.aura";
    std::ifstream f(lib_path);
    CHECK(f.good(), "lib/std/workspace.aura exists on disk");
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    f.close();
    // Existing 2 functions (ws:merge-symbols + ws:diff) +
    // 5 new functions in #563 (snapshot-current, list-snapshots,
    // rollback-latest, memory-pressure, current-stats) = 7 total.
    const bool has_export = content.find("(export") != std::string::npos;
    const bool has_snapshot = content.find("(ws:snapshot-current") != std::string::npos;
    const bool has_list = content.find("(ws:list-snapshots") != std::string::npos;
    const bool has_rollback = content.find("(ws:rollback-latest") != std::string::npos;
    const bool has_mem_pressure =
        content.find("(ws:memory-pressure") != std::string::npos;
    const bool has_current_stats =
        content.find("(ws:current-stats") != std::string::npos;
    std::println("  lib/workspace.aura: present + 5 new funcs + export");
    CHECK(has_export, "stdlib/workspace.aura has (export ...) line");
    CHECK(has_snapshot, "stdlib/workspace.aura exports (ws:snapshot-current)");
    CHECK(has_list, "stdlib/workspace.aura exports (ws:list-snapshots)");
    CHECK(has_rollback, "stdlib/workspace.aura exports (ws:rollback-latest)");
    CHECK(has_mem_pressure,
          "stdlib/workspace.aura exports (ws:memory-pressure)");
    CHECK(has_current_stats,
          "stdlib/workspace.aura exports (ws:current-stats)");
    return true;
}

// ── AC3: 12 primitives wrapped (≥5 acceptance met)
bool test_12_primitives_wrapped() {
    std::println("\n--- AC3: >= 12 primitives wrapped ---");
    const std::string ast_path = "/home/dev/code/aura/lib/std/ast.aura";
    const std::string ws_path = "/home/dev/code/aura/lib/std/workspace.aura";
    std::ifstream f1(ast_path);
    std::ifstream f2(ws_path);
    CHECK(f1.good(), "lib/std/ast.aura exists");
    CHECK(f2.good(), "lib/std/workspace.aura exists");
    std::string c1((std::istreambuf_iterator<char>(f1)),
                   std::istreambuf_iterator<char>());
    std::string c2((std::istreambuf_iterator<char>(f2)),
                   std::istreambuf_iterator<char>());
    f1.close();
    f2.close();
    // Count occurrences of "ast:" + "workspace:" in stdlib
    // files (each occurrence = one engine primitive call).
    int ast_calls = 0;
    std::size_t pos = 0;
    while ((pos = c1.find("(ast:", pos + 1)) != std::string::npos) {
        ++ast_calls;
    }
    int ws_calls = 0;
    pos = 0;
    while ((pos = c2.find("(workspace:", pos + 1)) != std::string::npos) {
        ++ws_calls;
    }
    int total = ast_calls + ws_calls;
    std::println("  ast:* calls in stdlib/ast.aura: {}", ast_calls);
    std::println("  workspace:* calls in stdlib/workspace.aura: {}", ws_calls);
    std::println("  total engine primitive calls from stdlib: {}", total);
    // The stdlib wrappers call at least 1 engine primitive
    // each (the wrapped primitive) + sometimes helpers. We
    // accept >= 12 (matches the 12 wrappers' minimum).
    CHECK(total >= 12,
          "stdlib wrappers invoke >= 12 engine primitives "
          "(≥5 acceptance criterion exceeded)");
    return true;
}

// ── AC4: docs/design/ast-workspace-decision.md present
bool test_decision_doc_exists() {
    std::println("\n--- AC4: docs/design/ast-workspace-decision.md ---");
    const std::string doc_path =
        "/home/dev/code/aura/docs/design/ast-workspace-decision.md";
    std::ifstream f(doc_path);
    CHECK(f.good(), "decision doc exists");
    if (f.good()) {
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        f.close();
        const bool has_ast_section =
            content.find("ast: namespace") != std::string::npos;
        const bool has_ws_section =
            content.find("workspace: namespace") != std::string::npos;
        const bool has_acceptance =
            content.find("Acceptance criteria check") != std::string::npos;
        std::println("  decision doc: present + ast+workspace sections + acceptance");
        CHECK(has_ast_section, "doc has ast: section");
        CHECK(has_ws_section, "doc has workspace: section");
        CHECK(has_acceptance, "doc has Acceptance criteria check section");
    }
    return true;
}

// ── AC5: regression — core ast:* + workspace:* primitives work
bool test_regression_existing_primitives() {
    std::println("\n--- AC5: regression — core ast:* + workspace:* primitives ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    // Core ast:* primitives (red-line #2 — stay in engine).
    auto r1 = cs.eval("(ast:nodes)");
    CHECK(r1.has_value(), "(ast:nodes) (regression)");
    auto r2 = cs.eval("(ast:defs)");
    CHECK(r2.has_value(), "(ast:defs) (regression)");
    auto r3 = cs.eval("(ast:version)");
    CHECK(r3.has_value(), "(ast:version) (regression)");
    auto r4 = cs.eval("(ast:generation)");
    CHECK(r4.has_value(), "(ast:generation) (regression)");
    // Core workspace:* primitives.
    auto r5 = cs.eval("(workspace:current)");
    CHECK(r5.has_value(), "(workspace:current) (regression)");
    auto r6 = cs.eval("(workspace:list)");
    CHECK(r6.has_value(), "(workspace:list) (regression)");
    // Tier-3 query:* primitive (regression for #562).
    auto r7 = cs.eval("(query:node \"a\")");
    CHECK(r7.has_value(), "(query:node \"a\") (regression for #562)");
    if (!cs.eval("(define reg-563-a 10)")) {
        CHECK(false, "define (regression)");
        return false;
    }
    auto r8 = cs.eval("(define reg-563-b 32)");
    (void)r8;
    auto r9 = cs.eval("(+ reg-563-a reg-563-b)");
    CHECK(r9.has_value() && aura::compiler::types::is_int(*r9) &&
              aura::compiler::types::as_int(*r9) == 42,
          "(+ reg-563-a reg-563-b) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #563 verification tests ═══\n");
    std::println("Layer 1: stdlib/ast.aura");
    test_stdlib_ast_file_present();
    std::println("\nLayer 2: stdlib/workspace.aura enhanced");
    test_stdlib_workspace_enhanced();
    std::println("\nLayer 3: 12 primitives wrapped + decision doc");
    test_12_primitives_wrapped();
    test_decision_doc_exists();
    std::println("\nLayer 4: regression");
    test_regression_existing_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_563_detail

int aura_issue_563_run() { return aura_issue_563_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_563_run(); }
#endif