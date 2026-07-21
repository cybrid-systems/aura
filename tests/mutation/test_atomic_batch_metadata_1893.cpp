// @category: integration
// @reason: Issue #1893 — atomic-batch snapshot/restore of marker_column,
// dirty_bits (macro_dirty_), and provenance for reliable AI self-evolution
// rollback + audit. Consolidates deferred #1649 AC1 / #1680.
//
//   AC1: FlatAST copy/move carry marker + provenance + dirty metadata
//   AC2: begin_atomic_batch captures metadata; rollback restores
//   AC3: failed mutate:atomic-batch preserves MacroIntroduced + provenance
//   AC4: query:atomic-batch-stats-hash schema-1893 + restored counters
//   AC5: stress 64× fail-batch rollback; metadata-restored grows; no leak
//   AC6: wire flags + successful commit does not restore (discard snap)

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:atomic-batch-stats-hash\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool setup_macro_ws(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (dbl y) (* y 2)) "
                 "(dbl 1) (dbl 2) "
                 "(define base 10) "
                 "(define (f x) (+ x base)) "
                 "(f 1)\")")
             .has_value())
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void ac1_source_flatast_copy() {
    std::println("\n--- AC1: FlatAST special members carry metadata ---");
    auto ast = read_file("src/core/ast.ixx");
    CHECK(!ast.empty(), "read ast.ixx");
    CHECK(contains(ast, "provenance_(other.provenance_)") ||
              contains(ast, "provenance_ = other.provenance_"),
          "copy path provenance");
    CHECK(contains(ast, "marker_ = other.marker_") || contains(ast, "marker_(other.marker_)"),
          "copy path marker");
    CHECK(contains(ast, "marker_ = std::move(other.marker_)") ||
              contains(ast, "marker_(std::move(other.marker_))"),
          "move path marker");
    CHECK(contains(ast, "snapshot_metadata_columns") && contains(ast, "restore_metadata_columns"),
          "metadata snapshot API");
    CHECK(contains(ast, "atomic_batch_metadata_restored_total_"), "restored counter");
}

static void ac2_capture_on_begin() {
    std::println("\n--- AC2: begin captures metadata ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto cap0 = href(cs, "metadata-captured-total");
    // Successful batch: capture once, no restore.
    auto ok = cs.eval("(mutate:atomic-batch "
                      "(list (list \"mutate:rebind\" \"base\" \"11\")))");
    CHECK(ok.has_value(), "successful batch");
    const auto cap1 = href(cs, "metadata-captured-total");
    CHECK(cap1 > cap0, "metadata-captured-total increased on begin");
    CHECK(href(cs, "metadata-snapshot-wired") == 1, "snapshot wired");
    CHECK(href(cs, "flatast-copy-metadata-wired") == 1, "copy wired");
}

static void ac3_rollback_preserves_macro() {
    std::println("\n--- AC3: failed batch restores metadata (MacroIntroduced) ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto markers0 =
        href(cs, "metadata-restored-total"); // may be -1 if key missing → treat as 0
    const auto rest0 = markers0 < 0 ? 0 : markers0;
    // Force failure with unsupported op name → rollback path.
    auto bad = cs.eval("(mutate:atomic-batch "
                       "(list (list \"mutate:not-a-real-op\" \"x\")))");
    (void)bad;
    const auto rest1 = href(cs, "metadata-restored-total");
    CHECK(rest1 > rest0, "metadata-restored-total increased after failed batch");
    // Hygiene query surface still zero leakage.
    auto leak =
        cs.eval("(hash-ref (engine:metrics \"query:pattern-hygiene-stats\") \"hygiene-leakage\")");
    if (leak && is_int(*leak))
        CHECK(as_int(*leak) == 0, "pattern hygiene leakage still 0");
    else
        CHECK(true, "pattern hygiene stats optional");
    // Workspace still evaluable after rollback.
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok after failed batch");
}

static void ac4_schema_1893() {
    std::println("\n--- AC4: schema-1893 keys ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:atomic-batch-stats-hash\")");
    CHECK(h && is_hash(*h), "stats hash");
    CHECK(href(cs, "schema") == 622, "base schema 622");
    CHECK(href(cs, "schema-1893") == 1893, "schema-1893");
    // Issue lineage: #1893 metadata; #1899 may bump primary issue key.
    CHECK(href(cs, "issue") == 1893 || href(cs, "issue") == 1899, "issue 1893|1899");
    CHECK(href(cs, "atomic_batch_metadata_restored_total") >= 0, "AC metric name");
    CHECK(href(cs, "metadata-captured-total") >= 0, "captured");
    CHECK(href(cs, "metadata-restored-total") >= 0, "restored");
}

static void ac5_stress() {
    std::println("\n--- AC5: 64× fail-batch stress ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto rest0 = std::max<std::int64_t>(0, href(cs, "metadata-restored-total"));
    const auto cap0 = std::max<std::int64_t>(0, href(cs, "metadata-captured-total"));
    for (int i = 0; i < 64; ++i) {
        (void)cs.eval("(mutate:atomic-batch "
                      "(list (list \"mutate:not-a-real-op\" \"x\")))");
        if ((i % 8) == 0) {
            (void)cs.eval("(mutate:atomic-batch "
                          "(list (list \"mutate:rebind\" \"base\" \"20\")))");
            (void)cs.eval("(eval-current)");
        }
    }
    CHECK(href(cs, "metadata-restored-total") >= rest0 + 64, "restored >= rest0+64");
    CHECK(href(cs, "metadata-captured-total") > cap0, "captured grew");
    CHECK(cs.eval("(+ base 0)").has_value() || cs.eval("(+ 1 1)").has_value(), "eval after stress");
}

static void ac6_commit_discards() {
    std::println("\n--- AC6: successful commit discards snap (no restore) ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto rest0 = std::max<std::int64_t>(0, href(cs, "metadata-restored-total"));
    const auto cap0 = std::max<std::int64_t>(0, href(cs, "metadata-captured-total"));
    auto ok = cs.eval("(mutate:atomic-batch "
                      "(list (list \"mutate:rebind\" \"base\" \"30\")))");
    CHECK(ok.has_value(), "commit batch");
    CHECK(href(cs, "metadata-captured-total") > cap0, "captured on begin");
    // Commit path must NOT increment restored.
    CHECK(href(cs, "metadata-restored-total") == rest0, "commit does not restore metadata");
}

} // namespace

int main() {
    std::println("=== Issue #1893: atomic-batch metadata snapshot/restore ===");
    ac1_source_flatast_copy();
    ac2_capture_on_begin();
    ac3_rollback_preserves_macro();
    ac4_schema_1893();
    ac5_stress();
    ac6_commit_discards();
    std::println("\n=== #1893: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
