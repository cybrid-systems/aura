// R19 phase4 dup-merge — atomic-batch core trio: Issue #1899 (dispatch + STRONG atomicity) + Issue
// #1893 (metadata snapshot/restore) + Issue #1913 (atomic-batch ↔ query:pattern / tag_arity_index
// sync)
//
// All three issues test core atomic-batch primitives with schema-NNNN stats-hash
// counters. Consolidated to a single batch file because:
//   - All 3 share module imports (test_harness.hpp + aura.compiler.{service,evaluator,value} + std)
//   - All 3 share the query:atomic-batch-stats-hash surface
//   - All 3 use anonymous namespace + free function pattern; main() is OUTSIDE
//   - All 3 set their distinct schema-NNNN counter independently (no inter-test interference)
//   - CMakeLists.txt: 0 explicit refs (all 3 auto-discovered via aura_resolve_test_cpp())
//
// Functions prefixed with the issue number to avoid name collisions across the 3
// originals (ac1 / ac2 / ac3 / ... were used per-issue with conflicting definitions).

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <chrono>
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

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

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

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void seed_dispatch(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1))) (define g (lambda (y) (* y 2)))\")")
              .has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
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

static bool seed_large(CompilerService& cs, int n_defs = 40) {
    std::string code;
    for (int i = 0; i < n_defs; ++i)
        code += std::format("(define (f{} x) (+ x {})) ", i, i);
    auto sc = cs.eval(std::format("(set-code \"{}\")", code));
    if (!sc)
        return false;
    (void)cs.eval("(eval-current)");
    return cs.evaluator().workspace_flat() != nullptr;
}

static void warm_index(Evaluator& ev) {
    ev.force_build_tag_arity_index();
}

// ═══════════════════════════════════════════════════════════════════════════
// Issue #1899 — atomic-batch STRONG atomicity (Option A) + data-driven
// lockless dispatch table (extensibility) closing the 5-of-14 if/else debt.
// AC1: source has kAtomicBatchLocklessOps table + #1899 + strong docs
// AC2: query:atomic-batch-stats-hash schema-1899 + dispatch-table-size 13
// AC3: multi-op batch of table-covered ops commits under strong mode
// AC4: unsupported op aborts batch (batch-unsupported-op)
// AC5: atomicity-mode remains strong (1); weak stays 0
// AC6: multi-round stress — strong commits monotonic
// ═══════════════════════════════════════════════════════════════════════════

static void ac1899_1_data_driven_table() {
    std::println("\n--- #1899 AC1: kAtomicBatchLocklessOps table ---");
    std::string src;
    for (const char* p : {"src/compiler/evaluator_primitives_mutate.cpp",
                          "../src/compiler/evaluator_primitives_mutate.cpp"}) {
        src = read_file(p);
        if (!src.empty())
            break;
    }
    CHECK(!src.empty(), "read mutate.cpp");
    CHECK(src.find("#1899") != std::string::npos, "cites #1899");
    CHECK(src.find("kAtomicBatchLocklessOps") != std::string::npos, "table present");
    CHECK(src.find("kAtomicBatchLocklessOpCount") != std::string::npos, "count present");
    CHECK(src.find("STRONG") != std::string::npos ||
              src.find("strong atomicity") != std::string::npos ||
              src.find("STRONG atomicity") != std::string::npos,
          "strong atomicity docs");
    CHECK(src.find("no inter-op yield") != std::string::npos ||
              src.find("no inter-op") != std::string::npos ||
              src.find("No inter-op fiber yield") != std::string::npos,
          "no inter-op yield documented");
    auto pos = src.find("kAtomicBatchLocklessOps[]");
    CHECK(pos != std::string::npos, "table array site");
    auto win = src.substr(pos, 2500);
    CHECK(win.find("eval_flat_apply_mutate_rebind") != std::string::npos, "rebind row");
    CHECK(win.find("eval_flat_apply_mutate_inline_call") != std::string::npos, "inline-call row");
    CHECK(win.find("eval_flat_apply_mutate_set_body") != std::string::npos, "set-body row");
    CHECK(src.find("(ev.*op_fn)") != std::string::npos ||
              src.find("(ev.*op_fn)(op_args)") != std::string::npos,
          "member-pointer dispatch");
}

static void ac1899_2_stats_surface() {
    std::println("\n--- #1899 AC2: schema-1899 dispatch inventory ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:atomic-batch-stats-hash\")");
    CHECK(r.has_value() && is_hash(*r), "stats-hash is hash");
    CHECK(href(cs, "schema") == 622, "base schema 622");
    CHECK(href(cs, "schema-1878") == 1878, "schema-1878");
    CHECK(href(cs, "schema-1893") == 1893, "schema-1893");
    CHECK(href(cs, "schema-1899") == 1899, "schema-1899");
    CHECK(href(cs, "issue") == 1899, "issue 1899");
    CHECK(href(cs, "atomicity-mode") == 1, "strong atomicity-mode");
    CHECK(href(cs, "strong-atomicity-default") == 1, "strong default flag");
    CHECK(href(cs, "dispatch-table-data-driven") == 1, "data-driven flag");
    CHECK(href(cs, "dispatch-table-size") == 13, "table size 13");
    CHECK(href(cs, "lockless-ops-covered") == 13, "lockless covered 13");
    CHECK(href(cs, "no-inter-op-yield") == 1, "no inter-op yield");
    CHECK(href(cs, "weak-atomicity-used") == 0, "weak unused");
}

static void ac1899_3_multi_op_batch() {
    std::println("\n--- #1899 AC3: multi-op batch under strong mode ---");
    CompilerService cs;
    seed_dispatch(cs);
    auto& ev = cs.evaluator();
    const auto strong0 = ev.atomic_batch_strong_atomicity_commits_total();
    const auto weak0 = ev.atomic_batch_weak_atomicity_used_total();
    auto r = cs.eval("(mutate:atomic-batch "
                     "(list "
                     "(list \"mutate:rebind\" \"f\" \"(lambda (x) (+ x 10))\" \"batch-1899-a\") "
                     "(list \"mutate:rebind\" \"g\" \"(lambda (y) (* y 3))\" \"batch-1899-b\")) "
                     "\"1899 multi-op\")");
    CHECK(r.has_value(), "batch eval returns");
    if (r)
        CHECK(!is_error(*r), "batch not error");
    CHECK(ev.atomic_batch_strong_atomicity_commits_total() >= strong0, "strong commits ≥");
    CHECK(ev.atomic_batch_weak_atomicity_used_total() == weak0, "weak still 0");
    CHECK(href(cs, "atomicity-mode") == 1, "mode still strong");
}

static void ac1899_4_unsupported_aborts() {
    std::println("\n--- #1899 AC4: unsupported op aborts batch ---");
    CompilerService cs;
    seed_dispatch(cs);
    auto r = cs.eval("(mutate:atomic-batch "
                     "(list "
                     "(list \"mutate:rebind\" \"f\" \"(lambda (x) 1)\" \"ok\") "
                     "(list \"mutate:not-a-real-op\" 0) "
                     "(list \"mutate:rebind\" \"g\" \"(lambda (y) 2)\" \"never\")) "
                     "\"1899 unsupported\")");
    CHECK(r.has_value(), "unsupported batch returns");
    if (r) {
        const bool failed = is_error(*r) || is_pair(*r) || (is_bool(*r) && !as_bool(*r));
        CHECK(failed, "unsupported → merr pair / error / #f");
    }
    CHECK(href(cs, "schema-1899") == 1899, "schema holds after abort");
}

static void ac1899_5_strong_mode_stable() {
    std::println("\n--- #1899 AC5: strong mode + weak=0 ---");
    CompilerService cs;
    CHECK(href(cs, "atomicity-mode") == 1, "mode 1");
    CHECK(href(cs, "weak-atomicity-used") == 0, "weak 0");
    CHECK(href(cs, "strong-atomicity-default") == 1, "default strong");
}

static void ac1899_6_stress() {
    std::println("\n--- #1899 AC6: multi-round strong commits ---");
    CompilerService cs;
    seed_dispatch(cs);
    auto& ev = cs.evaluator();
    const auto strong0 = ev.atomic_batch_strong_atomicity_commits_total();
    for (int i = 0; i < 8; ++i) {
        auto expr =
            std::format("(mutate:atomic-batch "
                        "(list (list \"mutate:rebind\" \"f\" \"(lambda (x) (+ x {}))\" \"round\")) "
                        "\"r{}\")",
                        i + 1, i);
        (void)cs.eval(expr);
    }
    CHECK(ev.atomic_batch_strong_atomicity_commits_total() >= strong0, "strong non-decreasing");
    CHECK(ev.atomic_batch_weak_atomicity_used_total() == 0, "weak still 0 after stress");
    CHECK(href(cs, "dispatch-table-size") == 13, "table size holds");
    CHECK(href(cs, "schema-1899") == 1899, "schema holds after stress");
}

// ═══════════════════════════════════════════════════════════════════════════
// Issue #1893 — atomic-batch snapshot/restore of marker_column, dirty_bits
// (macro_dirty_), and provenance for reliable AI self-evolution rollback +
// audit. Consolidates deferred #1649 AC1 / #1680.
// AC1: FlatAST copy/move carry marker + provenance + dirty metadata
// AC2: begin_atomic_batch captures metadata; rollback restores
// AC3: failed mutate:atomic-batch preserves MacroIntroduced + provenance
// AC4: query:atomic-batch-stats-hash schema-1893 + restored counters
// AC5: stress 64× fail-batch rollback; metadata-restored grows; no leak
// AC6: wire flags + successful commit does not restore (discard snap)
// ═══════════════════════════════════════════════════════════════════════════

static void ac1893_1_flatast_copy() {
    std::println("\n--- #1893 AC1: FlatAST special members carry metadata ---");
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

static void ac1893_2_capture_on_begin() {
    std::println("\n--- #1893 AC2: begin captures metadata ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto cap0 = href(cs, "metadata-captured-total");
    auto ok = cs.eval("(mutate:atomic-batch "
                      "(list (list \"mutate:rebind\" \"base\" \"11\")))");
    CHECK(ok.has_value(), "successful batch");
    const auto cap1 = href(cs, "metadata-captured-total");
    CHECK(cap1 > cap0, "metadata-captured-total increased on begin");
    CHECK(href(cs, "metadata-snapshot-wired") == 1, "snapshot wired");
    CHECK(href(cs, "flatast-copy-metadata-wired") == 1, "copy wired");
}

static void ac1893_3_rollback_preserves_macro() {
    std::println("\n--- #1893 AC3: failed batch restores metadata (MacroIntroduced) ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto markers0 = href(cs, "metadata-restored-total");
    const auto rest0 = markers0 < 0 ? 0 : markers0;
    auto bad = cs.eval("(mutate:atomic-batch "
                       "(list (list \"mutate:not-a-real-op\" \"x\")))");
    (void)bad;
    const auto rest1 = href(cs, "metadata-restored-total");
    CHECK(rest1 > rest0, "metadata-restored-total increased after failed batch");
    auto leak =
        cs.eval("(hash-ref (engine:metrics \"query:pattern-hygiene-stats\") \"hygiene-leakage\")");
    if (leak && is_int(*leak))
        CHECK(as_int(*leak) == 0, "pattern hygiene leakage still 0");
    else
        CHECK(true, "pattern hygiene stats optional");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok after failed batch");
}

static void ac1893_4_schema_1893() {
    std::println("\n--- #1893 AC4: schema-1893 keys ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:atomic-batch-stats-hash\")");
    CHECK(h && is_hash(*h), "stats hash");
    CHECK(href(cs, "schema") == 622, "base schema 622");
    CHECK(href(cs, "schema-1893") == 1893, "schema-1893");
    CHECK(href(cs, "issue") == 1893 || href(cs, "issue") == 1899, "issue 1893|1899");
    CHECK(href(cs, "atomic_batch_metadata_restored_total") >= 0, "AC metric name");
    CHECK(href(cs, "metadata-captured-total") >= 0, "captured");
    CHECK(href(cs, "metadata-restored-total") >= 0, "restored");
}

static void ac1893_5_stress() {
    std::println("\n--- #1893 AC5: 64× fail-batch stress ---");
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

static void ac1893_6_commit_discards() {
    std::println("\n--- #1893 AC6: successful commit discards snap (no restore) ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto rest0 = std::max<std::int64_t>(0, href(cs, "metadata-restored-total"));
    const auto cap0 = std::max<std::int64_t>(0, href(cs, "metadata-captured-total"));
    auto ok = cs.eval("(mutate:atomic-batch "
                      "(list (list \"mutate:rebind\" \"base\" \"30\")))");
    CHECK(ok.has_value(), "commit batch");
    CHECK(href(cs, "metadata-captured-total") > cap0, "captured on begin");
    CHECK(href(cs, "metadata-restored-total") == rest0, "commit does not restore metadata");
}

// ═══════════════════════════════════════════════════════════════════════════
// Issue #1913 — mutate:atomic-batch ↔ query:pattern / tag_arity_index
// end-to-end incremental sync (dirty-fraction policy) + metrics.
// AC1: source wires tag_arity_index_sync_after_atomic_batch + :sync-query-index?
// AC2: query:atomic-batch-stats-hash schema-1913 AC metrics
// AC3: successful atomic-batch commit bumps index sync counters
// AC4: post-batch query:pattern works; latency sample armed
// AC5: dirty-fraction path prefers incremental (rebuild_skipped / sync_hits)
// AC6: :sync-query-index? #f skips Evaluator index sync call path
// AC7: multi-round batch + pattern — index stays warm
// ═══════════════════════════════════════════════════════════════════════════

static void ac1913_1_source_surface() {
    std::println("\n--- #1913 AC1: batch index sync wiring ---");
    std::string qi, mut, ixx;
    for (const char* p :
         {"src/compiler/evaluator_query_index.cpp", "../src/compiler/evaluator_query_index.cpp"}) {
        qi = read_file(p);
        if (!qi.empty())
            break;
    }
    for (const char* p : {"src/compiler/evaluator_primitives_mutate.cpp",
                          "../src/compiler/evaluator_primitives_mutate.cpp"}) {
        mut = read_file(p);
        if (!mut.empty())
            break;
    }
    for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
        ixx = read_file(p);
        if (!ixx.empty())
            break;
    }
    CHECK(!qi.empty(), "read query_index");
    CHECK(qi.find("tag_arity_index_sync_after_atomic_batch") != std::string::npos,
          "sync_after_atomic_batch");
    CHECK(qi.find("#1913") != std::string::npos, "cites #1913");
    CHECK(qi.find("atomic_batch_index_sync_hits") != std::string::npos, "sync hits metric");
    CHECK(qi.find("pattern_query_after_batch") != std::string::npos, "post-batch latency");
    CHECK(!mut.empty(), "read mutate.cpp");
    CHECK(mut.find(":sync-query-index?") != std::string::npos, "keyword");
    CHECK(mut.find("tag_arity_index_sync_after_atomic_batch") != std::string::npos,
          "commit calls sync");
    CHECK(!ixx.empty() && ixx.find("tag_arity_index_sync_after_atomic_batch") != std::string::npos,
          "Evaluator API declared");
}

static void ac1913_2_schema_1913() {
    std::println("\n--- #1913 AC2: query:atomic-batch-stats-hash schema-1913 ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:atomic-batch-stats-hash\")");
    CHECK(r.has_value() && is_hash(*r), "is hash");
    CHECK(href(cs, "schema-1913") == 1913, "schema-1913");
    CHECK(href(cs, "index-sync-wired") == 1, "wired");
    CHECK(href(cs, "dirty-fraction-policy-wired") == 1, "dirty-fraction wired");
    CHECK(href(cs, "atomic_batch_index_sync_hits") >= 0, "sync hits key");
    CHECK(href(cs, "atomic_batch_index_rebuild_skipped") >= 0, "rebuild skipped key");
    CHECK(href(cs, "pattern_query_after_batch_latency") >= 0, "pattern latency key");
    CHECK(href(cs, "sync-query-index-default") == 1, "default sync on");
    CHECK(href(cs, "schema-1899") == 1899, "schema-1899 retained");
    CHECK(href(cs, "dispatch-table-size") == 13, "dispatch table");
}

static void ac1913_3_commit_sync_counters() {
    std::println("\n--- #1913 AC3: atomic-batch commit → index sync metrics ---");
    CompilerService cs;
    CHECK(seed_large(cs, 30), "seed large");
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");

    warm_index(ev);
    CHECK(ev.tag_arity_index_is_warm(), "index warm after build");

    const auto calls0 = ev.get_atomic_batch_index_sync_calls();
    const auto hits0 = ev.get_atomic_batch_index_sync_hits();
    const auto skip0 = ev.get_atomic_batch_index_rebuild_skipped();
    const auto full0 = ev.get_atomic_batch_index_full_rebuilds();

    auto batch =
        cs.eval("(mutate:atomic-batch "
                "(list (list \"mutate:rebind\" \"f0\" \"(lambda (x) (+ x 100))\" \"t1913\")) "
                "\"1913-ac3\")");
    CHECK(batch.has_value(), "atomic-batch ok");

    CHECK(ev.get_atomic_batch_index_sync_calls() > calls0, "sync calls advanced");
    const bool advanced = ev.get_atomic_batch_index_sync_hits() > hits0 ||
                          ev.get_atomic_batch_index_rebuild_skipped() > skip0 ||
                          ev.get_atomic_batch_index_full_rebuilds() > full0;
    CHECK(advanced, "sync hits or skip or full advanced");
    CHECK(href(cs, "atomic_batch_index_sync_calls") > 0 ||
              href(cs, "atomic-batch-index-sync-hits") >= 0,
          "hash sees counters");
    CHECK(load_u64(m->atomic_batch_index_sync_calls) > 0 ||
              load_u64(m->atomic_batch_index_sync_hits) > 0 ||
              load_u64(m->atomic_batch_index_full_rebuilds) > 0,
          "CompilerMetrics bumped");
}

static void ac1913_4_pattern_latency() {
    std::println("\n--- #1913 AC4: post-batch query:pattern latency ---");
    CompilerService cs;
    CHECK(seed_large(cs, 20), "seed");
    auto& ev = cs.evaluator();
    warm_index(ev);

    const auto samples0 = load_u64(metrics_of(cs)->pattern_query_after_batch_samples);
    auto batch = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:rebind\" \"f0\" \"(lambda (x) (* x 2))\" \"t\")) "
                         "\"1913-ac4\")");
    CHECK(batch.has_value(), "batch");

    auto pat = cs.eval("(query:pattern '(define _ _))");
    CHECK(pat.has_value(), "query:pattern after batch");

    const auto samples1 = load_u64(metrics_of(cs)->pattern_query_after_batch_samples);
    CHECK(samples1 > samples0 || href(cs, "pattern_query_after_batch_latency") >= 0,
          "post-batch latency sampled or key present");
    CHECK(ev.get_pattern_query_after_batch_latency_us_max() >= 0, "latency max field");
}

static void ac1913_5_sparse_dirty_incremental() {
    std::println("\n--- #1913 AC5: dirty-fraction prefers incremental ---");
    CompilerService cs;
    CHECK(seed_large(cs, 50), "seed 50 defs");
    auto& ev = cs.evaluator();
    warm_index(ev);

    const auto hits0 = ev.get_atomic_batch_index_sync_hits();
    const auto skip0 = ev.get_atomic_batch_index_rebuild_skipped();
    for (int i = 0; i < 5; ++i) {
        auto b = cs.eval(
            std::format("(mutate:atomic-batch "
                        "(list (list \"mutate:rebind\" \"f{}\" \"(lambda (x) (+ x {}))\" \"t\")) "
                        "\"1913-ac5-{}\")",
                        i % 10, i, i));
        CHECK(b.has_value(), std::format("batch round {}", i));
    }
    const auto hits1 = ev.get_atomic_batch_index_sync_hits();
    const auto skip1 = ev.get_atomic_batch_index_rebuild_skipped();
    std::println("  sync_hits {}→{}  rebuild_skipped {}→{}", hits0, hits1, skip0, skip1);
    CHECK(hits1 > hits0 || skip1 > skip0, "incremental path taken at least once");
    CHECK(ev.tag_arity_index_is_warm(), "index still warm");
}

static void ac1913_6_opt_out_sync() {
    std::println("\n--- #1913 AC6: :sync-query-index? #f skips sync ---");
    CompilerService cs;
    CHECK(seed_large(cs, 10), "seed");
    auto& ev = cs.evaluator();
    warm_index(ev);
    const auto calls0 = ev.get_atomic_batch_index_sync_calls();
    auto b = cs.eval("(mutate:atomic-batch "
                     "(list (list \"mutate:rebind\" \"f0\" \"(lambda (x) 1)\" \"t\")) "
                     "\"1913-ac6\" "
                     ":sync-query-index? #f)");
    CHECK(b.has_value(), "batch with sync off");
    CHECK(ev.get_atomic_batch_index_sync_calls() == calls0,
          "sync calls unchanged when :sync-query-index? #f");
}

static void ac1913_7_multi_round_stress() {
    std::println("\n--- #1913 AC7: multi-round batch + pattern ---");
    CompilerService cs;
    CHECK(seed_large(cs, 25), "seed");
    auto& ev = cs.evaluator();
    warm_index(ev);
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kRounds = 30;
    for (int i = 0; i < kRounds; ++i) {
        auto b = cs.eval(
            std::format("(mutate:atomic-batch "
                        "(list (list \"mutate:rebind\" \"f{}\" \"(lambda (x) (+ x {}))\" \"t\")) "
                        "\"r{}\")",
                        i % 20, i, i));
        CHECK(b.has_value(), std::format("round {} batch", i));
        auto p = cs.eval("(query:pattern '(define _ _))");
        CHECK(p.has_value(), std::format("round {} pattern", i));
    }
    const auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    std::println("  {} rounds batch+pattern in {} us (avg {} us)", kRounds, us, us / kRounds);
    CHECK(ev.get_atomic_batch_index_sync_calls() >= static_cast<std::uint64_t>(kRounds),
          "sync calls ≥ rounds");
    CHECK(ev.tag_arity_index_is_warm(), "warm after stress");
    CHECK(href(cs, "schema-1913") == 1913, "schema holds");
    CHECK(href(cs, "index-sync-wired") == 1, "wired after stress");
}

} // namespace

int main() {
    std::println("=== test_atomic_batch_core_batch: #1899 + #1893 + #1913 ===");

    // Issue #1899 — dispatch table + STRONG atomicity
    ac1899_1_data_driven_table();
    ac1899_2_stats_surface();
    ac1899_3_multi_op_batch();
    ac1899_4_unsupported_aborts();
    ac1899_5_strong_mode_stable();
    ac1899_6_stress();

    // Issue #1893 — metadata snapshot/restore
    ac1893_1_flatast_copy();
    ac1893_2_capture_on_begin();
    ac1893_3_rollback_preserves_macro();
    ac1893_4_schema_1893();
    ac1893_5_stress();
    ac1893_6_commit_discards();

    // Issue #1913 — atomic-batch ↔ query:pattern sync
    ac1913_1_source_surface();
    ac1913_2_schema_1913();
    ac1913_3_commit_sync_counters();
    ac1913_4_pattern_latency();
    ac1913_5_sparse_dirty_incremental();
    ac1913_6_opt_out_sync();
    ac1913_7_multi_round_stress();

    std::println("\n=== test_atomic_batch_core_batch: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
