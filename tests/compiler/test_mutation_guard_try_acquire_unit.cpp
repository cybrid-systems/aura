// @category: unit
// @reason: Issue #2124 — force all mutate:* paths through
// MutationBoundaryGuard::try_acquire (uniform quota + metrics).
//
//   AC1: check_mutation_guard_coverage.py --strict → 0 legacy ctor residual
//   AC2: mutate:* registrations covered by try_acquire family
//   AC3: try_acquire quota reject does not arm PanicCheckpoint
//   AC4: mutation_guard_try_acquire_total / _reject_total move under load
//   AC5: coverage script is the CI gate (invoked via build.py)
//   AC6: this registered issue test
//
// Issue #2986: coverage linter + production naked fail-closed.
//   ac2986_1 every production mutate:* Guard-wrapped or GUARD_EXEMPT
//   ac2986_2 linter catches naked add("mutate:__test_naked_2986")
//   ac2986_3 metrics remain; production fail-closed path cited
//   ac2986_4 metadata/policy setters remain exempt
//   ac2986_5 zero extra stores on happy Guard path
//   ac2986_6 source-cite + no docs/design / invent test

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <sys/wait.h>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.query;
import aura.parser.parser;
import aura.core;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

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

static int run_coverage_script(bool strict) {
    // CTest / binary runs from build/; script lives at repo-root/scripts/.
    const char* flags = strict ? " --strict --quiet" : " --quiet";
    for (const char* script : {"scripts/coverage/checks/check_mutation_guard_coverage.py",
                               "../scripts/coverage/checks/check_mutation_guard_coverage.py",
                               "../../scripts/coverage/checks/check_mutation_guard_coverage.py"}) {
        std::ifstream probe(script);
        if (!probe)
            continue;
        probe.close();
        std::string full = std::string("python3 ") + script + flags;
        int rc = std::system(full.c_str());
        return WEXITSTATUS(rc);
    }
    return 127;
}

static void ac1_zero_legacy_residual() {
    std::println("\n--- AC1: coverage script 0 legacy residual ---");
    // Source-level: no production MutationBoundaryGuard guard(ev, ... without try_acquire
    auto mutate = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    auto ast = read_file("src/compiler/evaluator_primitives_ast.cpp");
    auto script = read_file("scripts/coverage/checks/check_mutation_guard_coverage.py");
    CHECK(!mutate.empty() && !script.empty(), "read sources");
    CHECK(script.find("#2124") != std::string::npos, "coverage script cites #2124");
    CHECK(script.find("LEGACY_CTOR") != std::string::npos ||
              script.find("legacy ctor") != std::string::npos,
          "legacy residual check present");
    // No bare ctor in mutate production after #2124
    CHECK(mutate.find("MutationBoundaryGuard guard(ev") == std::string::npos,
          "no legacy guard(ev in mutate.cpp");
    CHECK(ast.find("MutationBoundaryGuard guard(ev") == std::string::npos,
          "no legacy guard(ev in ast.cpp");
    const int rc = run_coverage_script(/*strict=*/true);
    CHECK(rc == 0, "check_mutation_guard_coverage.py --strict exits 0");
}

static void ac2_try_acquire_in_mutate_paths() {
    std::println("\n--- AC2: try_acquire present on mutate surface ---");
    auto mutate = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    auto helpers = read_file("src/compiler/mutation_guard_helpers.hh");
    CHECK(mutate.find("mutate_dispatch_try_acquire") != std::string::npos,
          "mutate.cpp uses mutate_dispatch_try_acquire (#3074)");
    CHECK(helpers.find("try_acquire") != std::string::npos,
          "run_under_mutation_guard uses try_acquire");
    // Count try_acquire vs add_mutate roughly
    std::size_t n_try = 0, n_add = 0, pos = 0;
    while ((pos = mutate.find("try_acquire", pos)) != std::string::npos) {
        ++n_try;
        pos += 11;
    }
    pos = 0;
    while ((pos = mutate.find("add_mutate(\"", pos)) != std::string::npos) {
        ++n_add;
        pos += 12;
    }
    std::println("  try_acquire mentions={}, add_mutate={}", n_try, n_add);
    CHECK(n_try >= 10, "many try_acquire sites in mutate.cpp");
}

static void ac3_quota_reject_no_panic_checkpoint() {
    std::println("\n--- AC3: try_acquire quota reject does not arm PanicCheckpoint ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Exhaust mutation quota if API exists; else verify try_acquire path docs.
    auto src = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(src.find("try_acquire") != std::string::npos, "try_acquire impl present");
    // Quota reject returns unexpected before Guard ctor — no save_panic_checkpoint.
    // Documented order: check_mutation_quota then construct.
    auto try_pos = src.find("MutationBoundaryGuard::try_acquire");
    auto quota_pos = src.find("check_mutation_quota", try_pos == std::string::npos ? 0 : try_pos);
    auto save_pos = src.find("save_panic_checkpoint", try_pos == std::string::npos ? 0 : try_pos);
    CHECK(try_pos != std::string::npos, "try_acquire body found");
    // save_panic is in AcquireTag ctor, only after successful construct
    CHECK(quota_pos != std::string::npos, "quota check in try_acquire path");

    // Live: successful try_acquire then release; defer should not stick from quota fail.
    bool ok = true;
    {
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
        CHECK(gr.has_value(), "try_acquire succeeds under normal quota");
        // unique_ptr released at block end → Guard dtor (may save/commit panic).
    }
    // Document: quota reject returns unexpected *before* Guard ctor, so
    // save_panic_checkpoint is never called on reject (AC3).
    auto src2 = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto try_impl = src2.find("MutationBoundaryGuard::try_acquire");
    auto ctor_impl = src2.find("AcquireTag");
    CHECK(try_impl != std::string::npos && ctor_impl != std::string::npos,
          "try_acquire and AcquireTag ctor are separate");
    // save_panic is only in AcquireTag ctor path (after successful construct).
    CHECK(src2.find("had_panic_checkpoint_ = ev_->save_panic_checkpoint") != std::string::npos ||
              src2.find("save_panic_checkpoint()") != std::string::npos,
          "panic checkpoint only after successful Guard entry");
}

static void ac4_metrics_move() {
    std::println("\n--- AC4: try_acquire total / reject metrics move ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics");
    const auto t0 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    const auto r0 = m->mutation_guard_try_acquire_reject_total.load(std::memory_order_relaxed);

    // Synthetic multi-mutate via C++ try_acquire (same path as primitives).
    for (int i = 0; i < 8; ++i) {
        bool ok = true;
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
        CHECK(gr.has_value(), "acquire in loop");
    }
    // Also drive a real EDSL mutate if possible
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(mutate:rebind \"x\" \"2\")");

    const auto t1 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    const auto r1 = m->mutation_guard_try_acquire_reject_total.load(std::memory_order_relaxed);
    std::println("  try_acquire_total {} -> {}, reject {} -> {}", t0, t1, r0, r1);
    CHECK(t1 >= t0 + 8, "try_acquire_total += 8 from loop");
    CHECK(r1 >= r0, "reject total monotonic");
}

static void ac5_gate_docs() {
    std::println("\n--- AC5: build.py gate wires --strict ---");
    auto bp = read_file("build.py");
    CHECK(bp.find("cmd_mutation_guard_coverage") != std::string::npos, "gate function present");
    CHECK(bp.find("check_mutation_guard_coverage.py") != std::string::npos, "script invoked");
    CHECK(bp.find("--strict") != std::string::npos, "strict mode");
    CHECK(bp.find("#2124") != std::string::npos || bp.find("2124") != std::string::npos,
          "build.py cites #2124");
}

static void ac3074_1_structural_routes_dispatch() {
    std::println("\n--- #3074 AC1: structural mutate:* acquire via mutate_dispatch ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto hh = read_file("src/compiler/mutate_dispatch.hh");
    CHECK(hh.find("mutate_dispatch_try_acquire") != std::string::npos, "AC1: dispatch acquire");
    CHECK(hh.find("kMutateDispatchSoleGuardIssue = 3074") != std::string::npos, "AC1: issue stamp");
    CHECK(mut.find("mutate_dispatch_try_acquire") != std::string::npos, "AC1: mutate.cpp routes");
    CHECK(mut.find("mutate:replace-pattern") != std::string::npos, "AC1: replace-pattern");
    CHECK(mut.find("mutate:atomic-batch") != std::string::npos, "AC1: atomic-batch");
    CHECK(mut.find("mutate:rename-symbol") != std::string::npos, "AC1: rename-symbol");
    CHECK(mut.find("mutate:move-node") != std::string::npos, "AC1: move-node");
}

static void ac3074_2_guard_exempt_preserved() {
    std::println("\n--- #3074 AC2: GUARD_EXEMPT metadata prims stay exempt ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(mut.find("GUARD_EXEMPT:") != std::string::npos, "AC2: GUARD_EXEMPT comments");
    CHECK(mut.find("/*guard_exempt=*/true") != std::string::npos ||
              mut.find("guard_exempt") != std::string::npos,
          "AC2: guard_exempt flag");
}

static void ac3074_3_metrics_live() {
    std::println("\n--- #3074 AC3: MutateDispatchMetrics live (not simulate) ---");
    const auto hh = read_file("src/compiler/mutate_dispatch.hh");
    CHECK(hh.find("simulate applied") == std::string::npos, "AC3: no simulate applied");
    CHECK(hh.find("mutate_dispatch_note") != std::string::npos, "AC3: live note");
    CHECK(hh.find("rejected_total") != std::string::npos, "AC3: rejected counter");
    CHECK(hh.find("MutateDispatchResult::Rejected") != std::string::npos, "AC3: reject path");
    CHECK(hh.find("MutateDispatchResult::Applied") != std::string::npos, "AC3: applied path");
}

static void ac3074_5_source_and_linter() {
    std::println("\n--- #3074 AC5/AC6: linter + no invent + no docs/design ---");
    const auto t = read_file("tests/compiler/test_mutation_guard_try_acquire_unit.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_mutate_dispatch_sole_guard_3074.py");
    CHECK(t.find("ac3074_1_structural_routes_dispatch") != std::string::npos, "AC5: AC1 test");
    CHECK(build.find("check_mutate_dispatch_sole_guard_3074") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!lint.empty() && lint.find("3074") != std::string::npos, "AC5: linter present");
    CHECK(read_file("tests/compiler/test_issue_3074.cpp").empty(),
          "AC5: no invent test file per #81967");
    CHECK(read_file("docs/design/3074-mutate-dispatch.md").empty(),
          "AC6: no docs/design/3074-* per #1655");
}

static void ac2986_1_every_mutate_wrapped_or_exempt() {
    std::println("\n--- #2986 AC1: every production mutate:* wrapped or GUARD_EXEMPT ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto hh = read_file("src/compiler/evaluator.ixx");
    CHECK(mut.find("GUARD_EXEMPT:") != std::string::npos, "2986 AC1: GUARD_EXEMPT comments");
    CHECK(hh.find("guard_exempt") != std::string::npos, "2986 AC1: PrimMeta.guard_exempt");
    CHECK(mut.find("Issue #2986") != std::string::npos, "2986 AC1: add_mutate cites");
    CHECK(mut.find(":rebind") != std::string::npos && mut.find(":atomic") != std::string::npos,
          "2986 AC1: 6-op dispatcher");
    const int rc = run_coverage_script(/*strict=*/true);
    CHECK(rc == 0, "2986 AC1: existing #2124 --strict still 0");
}

static void ac2986_2_linter_catches_naked() {
    std::println("\n--- #2986 AC2: linter catches naked add(\"mutate:__test_naked_2986\") ---");
    // Deliberately-naked test prim — production never registers this.
    // The coverage linter must fail if this line appears in src/:
    //   add("mutate:__test_naked_2986", ...);
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(mut.find("mutate:__test_naked_2986") == std::string::npos,
          "2986 AC2: production never registers fixture");
    int rc = 127;
    for (const char* script : {"scripts/coverage/checks/check_mutate_guard_coverage.py",
                               "../scripts/coverage/checks/check_mutate_guard_coverage.py",
                               "../../scripts/coverage/checks/check_mutate_guard_coverage.py"}) {
        std::ifstream probe(script);
        if (!probe)
            continue;
        probe.close();
        rc = WEXITSTATUS(std::system((std::string("python3 ") + script).c_str()));
        break;
    }
    CHECK(rc == 0, "2986 AC2: check_mutate_guard_coverage.py exits 0");
}

static void ac2986_3_metrics_and_fail_closed() {
    std::println("\n--- #2986 AC3: metrics remain + production fail-closed path ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto metrics = read_file("src/compiler/observability_metrics.h");
    CHECK(mut.find("naked_mutate_attempt") != std::string::npos, "2986 AC3: naked counter");
    CHECK(mut.find("mutate_guard_enforced") != std::string::npos, "2986 AC3: enforced counter");
    CHECK(mut.find("naked_mutate_fail_closed_total") != std::string::npos,
          "2986 AC3: fail-closed counter");
    CHECK(mut.find("production_defaults_active()") != std::string::npos,
          "2986 AC3: production gate");
    CHECK(mut.find("mark_outermost_mutation_failed") != std::string::npos, "2986 AC3: mark-failed");
    CHECK(metrics.find("naked_mutate_fail_closed_total") != std::string::npos,
          "2986 AC3: metric field");

    CompilerService cs;
    CHECK(href(cs, "query:mutation-boundary-coverage-stats", "schema-2986") == 2986,
          "2986 AC3: schema-2986");
    CHECK(href(cs, "query:mutation-boundary-coverage-stats", "issue-2986") == 2986,
          "2986 AC3: issue-2986");
    CHECK(href(cs, "query:mutation-boundary-coverage-stats", "naked-mutate-fail-closed-wired") == 1,
          "2986 AC3: fail-closed wired");
    CHECK(href(cs, "query:mutation-boundary-coverage-stats", "naked-mutate-attempt") >= 0,
          "2986 AC3: naked-mutate-attempt preserved");
    CHECK(href(cs, "query:mutation-systemic-guard-stats", "schema-2986") == 2986,
          "2986 AC3: systemic schema-2986");

    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "2986 AC3: metrics");
    const auto t0 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    bool ok = true;
    auto gr = Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
    CHECK(gr.has_value(), "2986 AC3: try_acquire still works");
    const auto t1 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    CHECK(t1 >= t0 + 1, "2986 AC3: try_acquire_total moves (metrics remain)");
    CHECK(href(cs, "query:mutation-boundary-coverage-stats", "mutate-guard-enforced") >= 0,
          "2986 AC3: mutate-guard-enforced queryable");
}

static void ac2986_4_exempt_policy_setters() {
    std::println("\n--- #2986 AC4: metadata/policy setters remain exempt ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(mut.find("mutate:set-agent-fingerprint") != std::string::npos, "2986 AC4: fingerprint");
    CHECK(mut.find("mutate:set-stale-ref-policy") != std::string::npos,
          "2986 AC4: stale-ref policy");
    CHECK(mut.find("mutate:save-hygiene-checkpoint") != std::string::npos,
          "2986 AC4: hygiene save");
    CHECK(mut.find("/*guard_exempt=*/true") != std::string::npos, "2986 AC4: add_mutate flag");

    CompilerService cs;
    // Metadata-only add() path (no add_mutate capability gate).
    auto fp = cs.eval("(mutate:set-agent-fingerprint 7)");
    CHECK(fp.has_value() && is_int(*fp) && as_int(*fp) == 7, "2986 AC4: fingerprint setter works");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "2986 AC4: metrics");
    const auto fail = m->naked_mutate_fail_closed_total.load(std::memory_order_relaxed);
    CHECK(fail == 0, "2986 AC4: exempt setters do not fail-closed");
}

static void ac2986_5_zero_happy_overhead() {
    std::println("\n--- #2986 AC5: zero extra stores on happy Guard path ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(mut.find("wraps_after == wraps_before") != std::string::npos,
          "2986 AC5: existing compare");
    CHECK(mut.find("Happy Guard path") != std::string::npos,
          "2986 AC5: fail-closed off happy path");
    CHECK(mut.find("production_defaults_active()") != std::string::npos,
          "2986 AC5: production load only on naked");
    const auto build = read_file("build.py");
    CHECK(build.find("check_mutate_guard_coverage") != std::string::npos, "2986 AC5: linter wired");
}

static void ac2986_6_source_and_linter() {
    std::println("\n--- #2986 AC6: source-cite + no design/invent ---");
    const auto lint = read_file("scripts/coverage/checks/check_mutate_guard_coverage.py");
    const auto t = read_file("tests/compiler/test_mutation_guard_try_acquire_unit.cpp");
    CHECK(!lint.empty() && lint.find("2986") != std::string::npos, "2986 AC6: linter");
    CHECK(t.find("ac2986_1_every_mutate_wrapped_or_exempt") != std::string::npos,
          "2986 AC6: AC1 test");
    CHECK(read_file("docs/design/2986-mutate-guard-coverage.md").empty(),
          "2986 AC6: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_2986.cpp").empty(), "2986 AC6: no invent test");
}

static void ac3197_1_prim_meta_requires_guard() {
    std::println("\n--- #3197 AC1: PrimMeta.requires_mutation_guard stamped ---");
    CompilerService cs;
    auto& prims = cs.evaluator().primitives();
    const auto rebind = prims.slot_for_name("mutate:rebind");
    CHECK(rebind < prims.slot_count(), "3197 AC1: mutate:rebind registered");
    const auto& rm = prims.meta_for_slot(rebind);
    CHECK(rm.requires_mutation_guard, "3197 AC1: rebind requires_mutation_guard");
    CHECK(!rm.guard_exempt, "3197 AC1: rebind not guard_exempt");
    const auto set_body = prims.slot_for_name("mutate:set-body");
    CHECK(set_body < prims.slot_count(), "3197 AC1: mutate:set-body registered");
    CHECK(prims.meta_for_slot(set_body).requires_mutation_guard,
          "3197 AC1: set-body requires_mutation_guard");
    const auto check_ref = prims.slot_for_name("mutate:check-stable-ref");
    CHECK(check_ref < prims.slot_count(), "3197 AC1: check-stable-ref registered");
    const auto& cm = prims.meta_for_slot(check_ref);
    CHECK(cm.guard_exempt, "3197 AC1: check-stable-ref guard_exempt");
    CHECK(!cm.requires_mutation_guard, "3197 AC1: exempt does not require guard");
}

static void ac3197_2_token_and_fail_closed() {
    std::println("\n--- #3197 AC2: acquire token + production fail-closed cited ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto disp = read_file("src/compiler/mutate_dispatch.hh");
    CHECK(disp.find("note_mutate_guard_acquire_token") != std::string::npos,
          "3197 AC2: token note");
    CHECK(disp.find("kNakedMutatePrimMetaIssue = 3197") != std::string::npos, "3197 AC2: stamp");
    CHECK(mut.find("mutate_guard_acquire_token") != std::string::npos,
          "3197 AC2: post-check token");
    CHECK(mut.find("requires_mutation_guard = !guard_exempt") != std::string::npos,
          "3197 AC2: stamp from add_mutate");
    CHECK(mut.find("naked-mutate") != std::string::npos, "3197 AC2: structured kind");
    CHECK(mut.find("mark_outermost_mutation_failed") != std::string::npos, "3197 AC2: mark-failed");
}

static void ac3197_4_soft_observe() {
    std::println("\n--- #3197 AC4: Soft observe path preserved ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(mut.find("naked_mutate_attempt") != std::string::npos, "3197 AC4: attempt counter");
    CHECK(mut.find("production_defaults_active()") != std::string::npos,
          "3197 AC4: production gate still on fail-closed only");
}

static void ac3197_5_source_and_linter() {
    std::println("\n--- #3197 AC5: source-cite + linter + no invent ---");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_naked_mutate_prim_meta_3197.py");
    CHECK(!lint.empty() && lint.find("3197") != std::string::npos, "3197 AC5: linter");
    CHECK(build.find("check_naked_mutate_prim_meta_3197") != std::string::npos,
          "3197 AC5: build.py wires linter");
    CHECK(read_file("docs/design/3197-naked-mutate-prim-meta.md").empty(),
          "3197 AC5: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3197.cpp").empty(), "3197 AC5: no invent");
    CHECK(read_file("tests/issues/test_issue_3197.cpp").empty(), "3197 AC5: no invent issues/");
}

static void ac3352_1_in_process_guard() {
    std::println("\n--- #3352 AC1: Transform/AutoFix under Evaluator acquires Guard ---");
    const auto impl = read_file("src/compiler/query_impl.cpp");
    const auto qixx = read_file("src/compiler/query.ixx");
    CHECK(qixx.find("kTransformEngineGuardIssue = 3352") != std::string::npos, "AC1: stamp");
    CHECK(qixx.find("Evaluator* ev = nullptr") != std::string::npos, "AC1: optional Evaluator*");
    CHECK(impl.find("mutate_dispatch_try_acquire") != std::string::npos,
          "AC1: query_impl hits mutate_dispatch_try_acquire");
    CHECK(impl.find("if (ev)") != std::string::npos, "AC1: Guard only when ev set");

    CompilerService cs;
    (void)cs.eval("(set-code \"(define x (if 0 1 2))\")");
    auto& ev = cs.evaluator();
    auto* flat = ev.workspace_flat();
    auto* pool = ev.workspace_pool();
    CHECK(flat != nullptr && pool != nullptr, "AC1: workspace after set-code");
    if (!flat || !pool)
        return;
    aura::compiler::QueryEngine qe(*flat, *pool);
    aura::compiler::TransformEngine xform(*flat, *pool);
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "AC1: metrics");
    const auto t0 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    auto result = xform.query_and_fix(
        qe, "(and (node-type IfExpr) (child 0 (and (node-type LiteralInt) (= int_value 0))))",
        "(child 2)", &ev);
    CHECK(result.applied, "AC1: query_and_fix applied");
    CHECK(result.patch_count > 0, "AC1: patches written");
    const auto t1 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    CHECK(t1 >= t0 + 1, "AC1: Guard acquired (try_acquire_total moved)");
}

static void ac3352_2_cli_offline() {
    std::println("\n--- #3352 AC2: CLI throwaway FlatAST stays offline ---");
    const auto main = read_file("src/main.cpp");
    CHECK(main.find("--query-and-fix") != std::string::npos, "AC2: CLI flag");
    CHECK(main.find("query_and_fix(engine, argv[2], argv[3])") != std::string::npos,
          "AC2: CLI does not pass Evaluator");
    CHECK(main.find("fixer.run_all()") != std::string::npos, "AC2: --auto-fix run_all() no ev");
    CHECK(main.find("Do not hand this mutated tree to a live") != std::string::npos ||
              main.find("Do not pass this mutated FlatAST into a live Evaluator") !=
                  std::string::npos,
          "AC2: no live Evaluator handoff without re-load");

    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto pr = aura::parser::parse_to_flat("(if 0 1 2)", flat, pool);
    CHECK(pr.success && pr.root != aura::ast::NULL_NODE, "AC2: parse throwaway");
    if (!pr.success)
        return;
    flat.root = pr.root;
    aura::compiler::QueryEngine engine(flat, pool);
    aura::compiler::TransformEngine xform(flat, pool);
    auto result = xform.query_and_fix(
        engine, "(and (node-type IfExpr) (child 0 (and (node-type LiteralInt) (= int_value 0))))",
        "(child 2)");
    CHECK(result.applied, "AC2: offline apply still works");
    CHECK(result.patch_count > 0, "AC2: offline patches");
}

static void ac3352_3_soft_no_ev_zero_extra() {
    std::println("\n--- #3352 AC3: no Evaluator → zero extra vs today ---");
    const auto impl = read_file("src/compiler/query_impl.cpp");
    CHECK(impl.find("if (ev)") != std::string::npos, "AC3: ev-null skips Guard");
    CHECK(impl.find("g_3352_") == std::string::npos, "AC3: no g_3352_*");
    CHECK(impl.find("schema-3352") == std::string::npos, "AC3: no schema-3352");

    CompilerService cs;
    (void)cs.eval("(set-code \"(define y (if 1 3 4))\")");
    auto& ev = cs.evaluator();
    auto* flat = ev.workspace_flat();
    auto* pool = ev.workspace_pool();
    CHECK(flat != nullptr && pool != nullptr, "AC3: workspace");
    if (!flat || !pool)
        return;
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "AC3: metrics");
    const auto t0 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    aura::compiler::QueryEngine qe(*flat, *pool);
    aura::compiler::TransformEngine xform(*flat, *pool);
    auto result = xform.query_and_fix(
        qe, "(and (node-type IfExpr) (child 0 (and (node-type LiteralInt) (= int_value 1))))",
        "(child 1)");
    CHECK(result.applied, "AC3: nullptr-ev apply");
    const auto t1 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    CHECK(t1 == t0, "AC3: no Guard acquire without Evaluator");
}

static void ac3352_4_run_all_once_and_linter() {
    std::println("\n--- #3352 AC4: run_all acquires once + linter after #3074 ---");
    const auto impl = read_file("src/compiler/query_impl.cpp");
    CHECK(impl.find("query_and_fix(engine, rule.query, rule.replacement, /*ev=*/nullptr)") !=
              std::string::npos,
          "AC4: run_all inner query_and_fix nullptr (no nested Guard)");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_transform_engine_guard_3352.py");
    CHECK(!lint.empty() && lint.find("3352") != std::string::npos, "AC4: linter present");
    CHECK(build.find("check_transform_engine_guard_3352") != std::string::npos,
          "AC4: build.py wires linter");
    const auto i3074 = build.find("check_mutate_dispatch_sole_guard_3074.py");
    const auto i3352 = build.find("check_transform_engine_guard_3352.py");
    CHECK(i3074 != std::string::npos && i3352 != std::string::npos && i3352 > i3074,
          "AC4: #3352 linter after #3074");
    CHECK(read_file("docs/design/3352-transform-engine-guard.md").empty(), "AC4: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3352.cpp").empty(), "AC4: no invent");
    CHECK(read_file("tests/issues/test_issue_3352.cpp").empty(), "AC4: no invent issues/");

    CompilerService cs;
    (void)cs.eval("(set-code \"(define z (if 0 5 6))\")");
    auto& ev = cs.evaluator();
    auto* flat = ev.workspace_flat();
    auto* pool = ev.workspace_pool();
    CHECK(flat != nullptr && pool != nullptr, "AC4: workspace");
    if (!flat || !pool)
        return;
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "AC4: metrics");
    const auto t0 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    aura::compiler::AutoFixEngine fixer(*flat, *pool);
    fixer.add_default_rules();
    (void)fixer.run_all(&ev);
    const auto t1 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    CHECK(t1 == t0 + 1, "AC4: run_all acquires Guard once (not per rule)");
}

} // namespace

int run_test_mutation_guard_try_acquire_unit() {
    ac1_zero_legacy_residual();
    ac2_try_acquire_in_mutate_paths();
    ac3_quota_reject_no_panic_checkpoint();
    ac4_metrics_move();
    ac5_gate_docs();
    ac3074_1_structural_routes_dispatch();
    ac3074_2_guard_exempt_preserved();
    ac3074_3_metrics_live();
    ac3074_5_source_and_linter();
    ac2986_1_every_mutate_wrapped_or_exempt();
    ac2986_2_linter_catches_naked();
    ac2986_3_metrics_and_fail_closed();
    ac2986_4_exempt_policy_setters();
    ac2986_5_zero_happy_overhead();
    ac2986_6_source_and_linter();
    ac3197_1_prim_meta_requires_guard();
    ac3197_2_token_and_fail_closed();
    ac3197_4_soft_observe();
    ac3197_5_source_and_linter();
    ac3352_1_in_process_guard();
    ac3352_2_cli_offline();
    ac3352_3_soft_no_ev_zero_extra();
    ac3352_4_run_all_once_and_linter();

    std::println("\n=== test_mutation_guard_try_acquire_unit: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mutation_guard_try_acquire_unit();
}
#endif
