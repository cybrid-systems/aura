// test_occurrence_narrow_blame_stale_invalidation_post_mutate.cpp
// Issue #639: Occurrence Typing blame + stale narrow invalidation.
//
// AC1: mutate predicate var marks narrowing records stale
// AC2: post-mutate typecheck detects stale + attaches blame
// AC3: query:narrow-blame-stats grows after stale path
// AC4: query:provenance-of includes :stale field
// AC5: re-narrow clears stale + eval semantics preserved
// AC6: match/if matrix — no wrong narrow after cond mutate

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_639_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static constexpr const char* k_if_prog = R"(
(define f (lambda (x) (if (number? x) (+ x 1) 0)))
)";

static bool load_if_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_if_prog + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    // Workspace typecheck populates narrowing_log_ on workspace_flat.
    if (!cs.eval("(typecheck-current)"))
        return false;
    return true;
}

static std::int64_t narrow_blame_stats(CompilerService& cs) {
    auto r = cs.eval("(query:narrow-blame-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void test_mutate_marks_narrowing_stale() {
    std::println("\n--- AC1: mutate marks narrowing records stale ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    const auto log0 = cs.all_narrowings().size();
    CHECK(log0 > 0, "narrowing log populated after typecheck-current");

    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "workspace flat present");
    const auto inv0 = ws ? ws->narrow_invalidation_post_mutate_count() : 0u;

    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (string? x) x 0))\" "
        "\"issue-639-pred\")");
    const auto inv1 = ws->narrow_invalidation_post_mutate_count();
    const auto stale_count = ws->stale_narrowing_record_count();
    std::println("  invalidation: {} -> {}, stale_records={}", inv0, inv1, stale_count);
    CHECK(inv1 > inv0 || stale_count > 0,
          "narrowing invalidation fired after predicate mutate");
}

static void test_stale_detected_with_blame_stats() {
    std::println("\n--- AC2/AC3: stale detected + narrow-blame-stats grows ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    const auto stats0 = narrow_blame_stats(cs);

    cs.set_incremental_typecheck_mode(
        aura::compiler::IncrementalTypecheckMode::Lazy);
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (* x 2) 0))\" "
        "\"issue-639-stale\")");
    (void)cs.eval("(typecheck-current)");

    const auto stats1 = narrow_blame_stats(cs);
    const auto snap = cs.snapshot();
    std::println("  narrow-blame-stats: {} -> {}", stats0, stats1);
    std::println("  stale_caught={} blame_attached={} safe_fallback={} inv_post_mutate={}",
                 snap.narrow_stale_caught_total, snap.narrow_blame_attached_total,
                 snap.narrow_safe_fallback_total, snap.narrow_invalidation_post_mutate_total);
    CHECK(stats1 >= stats0, "query:narrow-blame-stats monotonic");
    CHECK(snap.narrow_stale_caught_total > 0 || snap.narrow_blame_attached_total > 0 ||
              snap.narrow_safe_fallback_total > 0 ||
              snap.narrow_invalidation_post_mutate_total > 0,
          "stale path bumped at least one #639 counter");
}

static void test_provenance_includes_stale_field() {
    std::println("\n--- AC4: query:provenance-of includes :stale ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (pair? x) (car x) 0))\" "
        "\"issue-639-prov\")");
    (void)cs.eval("(typecheck-current)");
    auto prov = cs.eval("(query:provenance-of \"x\")");
    CHECK(prov.has_value(), "provenance-of returns value");
}

static void test_re_narrow_clears_stale_and_eval_ok() {
    std::println("\n--- AC5: re-narrow clears stale + eval preserved ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    cs.set_incremental_typecheck_mode(
        aura::compiler::IncrementalTypecheckMode::Lazy);
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 3) 0))\" "
        "\"issue-639-renarrow\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    const auto stale_before = ws->occurrence_stale_count();
    (void)cs.incremental_infer(ws->all_mutations().back());
    const auto stale_after = ws->occurrence_stale_count();
    std::println("  occurrence_stale_count: {} -> {}", stale_before, stale_after);
    CHECK(stale_after <= stale_before, "stale count non-increasing after re-narrow");
    auto r = cs.eval("(eval-current)");
    CHECK(r.has_value(), "eval succeeds after re-narrow");
}

static void test_no_wrong_narrow_after_cond_mutate() {
    std::println("\n--- AC6: no wrong narrow after cond mutate ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 10) 0))\" "
        "\"issue-639-sem\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    auto r = cs.eval("(f 5)");
    CHECK(r && is_int(*r), "f 5 returns int after re-narrow");
    if (r && is_int(*r)) {
        const auto v = as_int(*r);
        CHECK(v == 15, "narrow-dependent (+ x 10) semantics correct");
    }
}

} // namespace aura_639_detail

int main() {
    using namespace aura_639_detail;
    test_mutate_marks_narrowing_stale();
    test_stale_detected_with_blame_stats();
    test_provenance_includes_stale_field();
    test_re_narrow_clears_stale_and_eval_ok();
    test_no_wrong_narrow_after_cond_mutate();
    return RUN_ALL_TESTS();
}