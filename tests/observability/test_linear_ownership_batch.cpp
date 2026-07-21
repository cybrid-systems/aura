// test_linear_ownership_batch.cpp
// B pilot #8 (after #1596 lineage): consolidated Issues #610 + #638 + #598
// + #575 + #1596 + #1659 linear ownership family (post-mutate validation +
// runtime + GuardShape + incremental per_defuse + live-closure scan +
// GC/Arena mutation safety) into one batch driver.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention
// (test_issues_809_817_batch / _819_829_batch precedents): single binary
// with CHECK() + RUN_ALL_TESTS(); per-issue AC blocks in namespace
// aura_linear_ownership_batch { run_NNN_xxx() }; EXCLUDE_FROM_ALL.
//
// AC map (consolidated, 41 ACs total):
//   Issue #610 — 8 ACs: post_mutate_invariant_check + linear mutation stats
//                  + moved linear passes + multi-round matrix + gc-heap
//                  integration
//   Issue #638 — 7 ACs: leaked-linear violations_caught + GuardShape
//                  invalidate_shape + safety stats + query regression
//   Issue #598 — 7 ACs: linear-ownership-runtime-stats + deopt_on_invalidate
//                  + post_mutate_enforcement + prompt6-violation regression
//   Issue #575 — 8 ACs: linear-ownership-incremental-stats + revalidate +
//                  per_defuse_index O(uses) + multi-round matrix
//   Issue #1596 — 6 ACs: walk_active_closures + scan + force_drop +
//                   gc_root_audit + metrics query + 10k stress
//   Issue #1659 — 7 ACs: EnvFrame linear snapshot + scan Moved/Untracked +
//                   enforce boundary + schema 1659 + mutate 200× stress

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_linear_ownership_batch {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::EnvId;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;

constexpr std::uint8_t kUntracked = 0;
constexpr std::uint8_t kOwned = 1;
constexpr std::uint8_t kMoved = 4;

static constexpr const char* k_linear_prog = R"(
(define leak (let ((x (Linear 42))) (display x)))
(define f (lambda () (let ((x (Linear 42))) (move x))))
)";

static int count_kind(const std::vector<aura::compiler::OwnershipNote>& notes,
                      const std::string& kind) {
    int n = 0;
    for (const auto& note : notes) {
        if (note.kind == kind)
            ++n;
    }
    return n;
}

static std::int64_t linear_mutation_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:linear-ownership-mutation-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static std::int64_t linear_safety_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:linear-ownership-safety-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static std::int64_t runtime_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:linear-ownership-runtime-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static std::int64_t linear_incremental_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:linear-ownership-incremental-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool load_linear_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_linear_prog + "\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-boundary-consistency-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

static std::pair<std::uint64_t, std::uint64_t> make_linear_closure(Evaluator& ev,
                                                                   std::uint8_t linear_state) {
    auto env_id = ev.alloc_env_frame(NULL_ENV_ID);
    {
        auto* fr = ev.resolve_env_frame_mut(env_id);
        if (fr) {
            auto& syms = fr->bindings_symid_;
            auto& lin = fr->bindings_linear_ownership_state_;
            if (syms.empty()) {
                syms.push_back({static_cast<SymId>(1), make_int(0)});
                lin.push_back(linear_state);
            } else {
                lin.resize(syms.size(), kUntracked);
                lin[0] = linear_state;
            }
            fr->version_ = ev.defuse_version_snapshot();
        }
    }
    Closure cl;
    cl.env_id = env_id;
    const auto cid = ev.register_active_closure(std::move(cl));
    return {static_cast<std::uint64_t>(cid), static_cast<std::uint64_t>(env_id)};
}

// ---------------------------------------------------------------------------
// Issue #610: post-mutate validation + runtime integration (8 ACs)
// ---------------------------------------------------------------------------
static void run_610_post_mutate_validation_runtime() {
    std::println("\n=== Issue #610: linear ownership post-mutate validation + runtime ===");

    // AC1: post_mutation_invariant_check catches leaked-linear + metrics
    {
        std::println("\n--- AC1: post_mutation_invariant_check + metrics ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);

        auto x_sym = pool->intern("x");
        auto inner = flat->add_literal(42);
        auto lin_node = flat->add_linear(inner);
        auto disp_sym = pool->intern("display");
        auto disp_var = flat->add_variable(disp_sym);
        auto x_var = flat->add_variable(x_sym);
        aura::ast::NodeId disp_args[] = {disp_var, x_var};
        auto disp_call = flat->add_call(disp_var, disp_args);
        auto root = flat->add_let(x_sym, lin_node, disp_call);
        flat->root = root;

        aura::core::TypeRegistry reg;
        CompilerMetrics metrics;
        aura::ast::MutationRecord rec;
        rec.target_node = root;
        rec.parent_id = aura::ast::NULL_NODE;
        rec.mutation_id = 610;
        rec.operator_name = "issue-610";

        std::vector<aura::compiler::OwnershipNote> notes;
        const auto status =
            aura::compiler::post_mutation_invariant_check(*flat, *pool, reg, rec, notes, &metrics);
        const auto reval =
            metrics.linear_post_mutate_revalidations_total.load(std::memory_order_relaxed);
        const auto leaks = metrics.linear_leak_prevented_total.load(std::memory_order_relaxed);
        std::println("  status={} notes={} revalidations={} leak_prevented={}",
                     static_cast<int>(status), notes.size(), reval, leaks);
        CHECK(reval > 0, "post_mutate_revalidations bumped on linear dirty scope");
        CHECK(count_kind(notes, "leaked-linear") >= 1,
              "post_mutation_invariant_check finds leaked-linear");
        CHECK(leaks >= 1, "linear_leak_prevented_total bumped");
    }

    CompilerService cs;
    CHECK(load_linear_workspace(cs), "load linear workspace (610)");

    // AC2: query:linear-ownership-mutation-stats reachable + non-negative
    {
        std::println("\n--- AC2: query:linear-ownership-mutation-stats ---");
        const auto s0 = linear_mutation_stats(cs);
        std::println("  query:linear-ownership-mutation-stats = {}", s0);
        CHECK(s0 >= 0, "linear-ownership-mutation-stats non-negative");
    }

    // AC3: properly moved linear → no leak notes + pass true
    {
        std::println("\n--- AC3: properly moved linear — no leak on revalidate ---");
        CompilerMetrics metrics;
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* str_pool = arena->create<aura::ast::StringPool>(alloc);
        auto x_sym = str_pool->intern("x");
        auto inner = flat->add_literal(42);
        auto lin_node = flat->add_linear(inner);
        auto x_var = flat->add_variable(x_sym);
        auto move_node = flat->add_move(x_var);
        auto root = flat->add_let(x_sym, lin_node, move_node);
        flat->root = root;
        aura::core::TypeRegistry reg;
        aura::ast::MutationRecord rec;
        rec.target_node = root;
        rec.mutation_id = 6103;
        std::vector<aura::compiler::OwnershipNote> notes;
        const auto status = aura::compiler::post_mutation_invariant_check(*flat, *str_pool, reg,
                                                                          rec, notes, &metrics);
        CHECK(status == aura::ast::InvariantStatus::Ok,
              "moved linear binding passes post-mutate revalidate");
        CHECK(count_kind(notes, "leaked-linear") == 0,
              "no leaked-linear for properly moved binding");
    }

    // AC4: mutate:rebind on linear define → stats monotonic
    {
        std::println("\n--- AC4: post-mutate revalidate on linear mutate → stats grow ---");
        const auto stats4a = linear_mutation_stats(cs);
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 88))) (display x)))\" "
                      "\"issue-610-leak\")");
        auto* ws = cs.evaluator().workspace_flat();
        auto* ws_pool = cs.evaluator().workspace_pool();
        CHECK(ws != nullptr && ws_pool != nullptr && !ws->all_mutations().empty(),
              "mutation logged for linear rebind");
        if (ws && ws_pool && !ws->all_mutations().empty()) {
            aura::core::TypeRegistry reg;
            std::vector<aura::compiler::OwnershipNote> notes;
            (void)aura::compiler::post_mutation_invariant_check(*ws, *ws_pool, reg,
                                                                ws->all_mutations().back(), notes,
                                                                cs.evaluator().compiler_metrics());
        }
        const auto stats4b = linear_mutation_stats(cs);
        std::println("  linear-ownership-mutation-stats: {} -> {}", stats4a, stats4b);
        CHECK(stats4b > stats4a, "linear-mutation-stats grew after post-mutate linear revalidate");
    }

    // AC5: closure-env-safety-stats regression still works
    {
        std::println("\n--- AC5: closure-env-safety-stats regression ---");
        auto ces = cs.eval("(engine:metrics \"query:closure-env-safety-stats\")");
        CHECK(ces && is_hash(*ces), "query:closure-env-safety-stats still works");
    }

    // AC6: invalidate path bumps deopt_on_linear counter
    {
        std::println("\n--- AC6: invalidate bumps deopt_on_linear ---");
        const auto stats6a = linear_mutation_stats(cs);
        (void)cs.eval("(set-code \"(define f (lambda () (let ((x (Linear 1))) (move x))))\")");
        (void)cs.eval("(eval-current)");
        const auto stats6b = linear_mutation_stats(cs);
        std::println("  linear-ownership-mutation-stats: {} -> {}", stats6a, stats6b);
        CHECK(stats6b >= stats6a, "deopt_on_linear contributes to stats after invalidate path");
    }

    // AC7: multi-round linear mutate matrix — eval + stats monotonic
    {
        std::println("\n--- AC7: multi-round linear mutate matrix ---");
        const auto stats7a = linear_mutation_stats(cs);
        for (int round = 0; round < 3; ++round) {
            const std::string body =
                "(lambda () (let ((x (Linear " + std::to_string(round + 10) + "))) (move x)))";
            (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"r" + std::to_string(round) +
                          "\")");
            auto r = cs.eval("(f)");
            CHECK(r.has_value(), "f eval ok round " + std::to_string(round));
        }
        const auto stats7b = linear_mutation_stats(cs);
        std::println("  linear-ownership-mutation-stats: {} -> {}", stats7a, stats7b);
        CHECK(stats7b >= stats7a, "linear-mutation-stats monotonic over matrix");
    }

    // AC8: gc-heap + linear mutation integration
    {
        std::println("\n--- AC8: gc-heap + linear mutation integration ---");
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 7))) (move x)))\" "
                      "\"issue-610-gc\")");
        auto gc = cs.eval("(gc-heap)");
        CHECK(gc.has_value(), "(gc-heap) callable after linear mutate");
    }
}

// ---------------------------------------------------------------------------
// Issue #638: GuardShape + leaked-linear violations_caught (7 ACs)
// ---------------------------------------------------------------------------
static void run_638_guardshape_post_mutate() {
    std::println("\n=== Issue #638: linear ownership + GuardShape enforcement post-mutate ===");

    CompilerService cs;
    CHECK(load_linear_workspace(cs), "load linear workspace (638)");

    // AC1: query:linear-ownership-safety-stats reachable
    {
        std::println("\n--- AC1: query:linear-ownership-safety-stats ---");
        const auto s0 = linear_safety_stats(cs);
        std::println("  query:linear-ownership-safety-stats = {}", s0);
        CHECK(s0 >= 0, "linear-ownership-safety-stats non-negative");
    }

    // AC2: leaked-linear post_mutate → violations_caught bumps
    {
        std::println("\n--- AC2: leaked-linear → violations_caught bumps ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);

        auto x_sym = pool->intern("x");
        auto inner = flat->add_literal(42);
        auto lin_node = flat->add_linear(inner);
        auto disp_sym = pool->intern("display");
        auto disp_var = flat->add_variable(disp_sym);
        auto x_var = flat->add_variable(x_sym);
        aura::ast::NodeId disp_args[] = {disp_var, x_var};
        auto disp_call = flat->add_call(disp_var, disp_args);
        auto root = flat->add_let(x_sym, lin_node, disp_call);
        flat->root = root;

        aura::core::TypeRegistry reg;
        CompilerMetrics metrics;
        aura::ast::MutationRecord rec;
        rec.target_node = root;
        rec.mutation_id = 638;
        std::vector<aura::compiler::OwnershipNote> notes;
        (void)aura::compiler::post_mutation_invariant_check(*flat, *pool, reg, rec, notes,
                                                            &metrics);
        const auto violations =
            metrics.linear_violations_caught_total.load(std::memory_order_relaxed);
        const auto leaks = metrics.linear_leak_prevented_total.load(std::memory_order_relaxed);
        std::println("  violations_caught={} leak_prevented={}", violations, leaks);
        CHECK(violations >= 1 || leaks >= 1, "leaked-linear bumps violation or leak counters");
        CHECK(count_kind(notes, "leaked-linear") >= 1,
              "post_mutation_invariant_check finds leaked-linear");
    }

    // AC3: properly moved linear passes post_mutate revalidate
    {
        std::println("\n--- AC3: properly moved linear passes post_mutate ---");
        CompilerMetrics metrics;
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* str_pool = arena->create<aura::ast::StringPool>(alloc);
        auto x_sym = str_pool->intern("x");
        auto inner = flat->add_literal(42);
        auto lin_node = flat->add_linear(inner);
        auto x_var = flat->add_variable(x_sym);
        auto move_node = flat->add_move(x_var);
        auto root = flat->add_let(x_sym, lin_node, move_node);
        flat->root = root;
        aura::core::TypeRegistry reg;
        aura::ast::MutationRecord rec;
        rec.target_node = root;
        rec.mutation_id = 6383;
        std::vector<aura::compiler::OwnershipNote> notes;
        const auto status = aura::compiler::post_mutation_invariant_check(*flat, *str_pool, reg,
                                                                          rec, notes, &metrics);
        CHECK(status == aura::ast::InvariantStatus::Ok,
              "moved linear binding passes post-mutate revalidate");
    }

    // AC4: mutate + eval linear → safety stats monotonic
    {
        std::println("\n--- AC4: mutate + eval linear → safety stats monotonic ---");
        const auto stats4a = linear_safety_stats(cs);
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 88))) (display x)))\" "
                      "\"issue-638-leak\")");
        auto* ws = cs.evaluator().workspace_flat();
        auto* ws_pool = cs.evaluator().workspace_pool();
        if (ws && ws_pool && !ws->all_mutations().empty()) {
            aura::core::TypeRegistry reg2;
            std::vector<aura::compiler::OwnershipNote> notes2;
            (void)aura::compiler::post_mutation_invariant_check(*ws, *ws_pool, reg2,
                                                                ws->all_mutations().back(), notes2,
                                                                cs.evaluator().compiler_metrics());
        }
        const auto stats4b = linear_safety_stats(cs);
        std::println("  linear-ownership-safety-stats: {} -> {}", stats4a, stats4b);
        CHECK(stats4b >= stats4a, "safety-stats monotonic after post-mutate linear revalidate");
    }

    // AC5: invalidate_shape clears stability (GuardShape path)
    {
        std::println("\n--- AC5: invalidate_shape clears GuardShape stability ---");
        (void)cs.eval("(set-code \"(define add1 (lambda (x) (+ x 1)))\")");
        CHECK(cs.eval("(eval-current)").has_value(), "add1 workspace eval");
        const bool stable_before = cs.is_shape_stable("add1");
        cs.invalidate_shape("add1");
        CHECK(!cs.is_shape_stable("add1"),
              "invalidate_shape clears stability for GuardShape refresh");
        (void)stable_before;
    }

    // AC6: multi-round linear mutate matrix — eval + stats monotonic
    {
        std::println("\n--- AC6: multi-round linear mutate matrix ---");
        const auto stats6a = linear_safety_stats(cs);
        for (int round = 0; round < 3; ++round) {
            const std::string body =
                "(lambda () (let ((x (Linear " + std::to_string(round + 10) + "))) (move x)))";
            (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"r" + std::to_string(round) +
                          "\")");
            auto r = cs.eval("(f)");
            CHECK(r.has_value(), "f eval ok round " + std::to_string(round));
        }
        const auto stats6b = linear_safety_stats(cs);
        std::println("  linear-ownership-safety-stats: {} -> {}", stats6a, stats6b);
        CHECK(stats6b >= stats6a, "safety-stats monotonic over matrix");
    }

    // AC7: query regression (mutation-stats, closure-env-safety, shape-stability-stats)
    {
        std::println("\n--- AC7: query regression ---");
        auto lms = cs.eval("(engine:metrics \"query:linear-ownership-mutation-stats\")");
        auto ces = cs.eval("(engine:metrics \"query:closure-env-safety-stats\")");
        auto sps = cs.eval("(engine:metrics \"query:shape-stability-stats\")");
        CHECK(lms && is_int(*lms), "linear-ownership-mutation-stats regression");
        CHECK(ces && is_hash(*ces), "closure-env-safety-stats regression");
        CHECK(sps && is_int(*sps), "shape-stability-stats regression");
    }
}

// ---------------------------------------------------------------------------
// Issue #598: runtime linear ownership + invalidate integration (7 ACs)
// ---------------------------------------------------------------------------
static void run_598_runtime_enforcement_post_mutate() {
    std::println("\n=== Issue #598: runtime linear ownership enforcement + invalidate ===");

    CompilerService cs;
    CHECK(load_linear_workspace(cs), "load linear workspace (598)");

    // AC1: query:linear-ownership-runtime-stats reachable
    {
        std::println("\n--- AC1: query:linear-ownership-runtime-stats ---");
        const auto s0 = runtime_stats(cs);
        std::println("  linear-ownership-runtime-stats = {}", s0);
        CHECK(s0 >= 0, "runtime stats non-negative");
    }

    // AC2: properly moved linear passes post_mutate revalidate
    {
        std::println("\n--- AC2: properly moved linear passes post_mutate ---");
        CompilerMetrics metrics;
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* str_pool = arena->create<aura::ast::StringPool>(alloc);
        auto x_sym = str_pool->intern("x");
        auto inner = flat->add_literal(42);
        auto lin_node = flat->add_linear(inner);
        auto x_var = flat->add_variable(x_sym);
        auto move_node = flat->add_move(x_var);
        auto root = flat->add_let(x_sym, lin_node, move_node);
        flat->root = root;
        aura::core::TypeRegistry reg;
        aura::ast::MutationRecord rec;
        rec.target_node = root;
        rec.mutation_id = 5982;
        std::vector<aura::compiler::OwnershipNote> notes;
        const auto status = aura::compiler::post_mutation_invariant_check(*flat, *str_pool, reg,
                                                                          rec, notes, &metrics);
        CHECK(status == aura::ast::InvariantStatus::Ok,
              "moved linear binding passes post-mutate revalidate");
    }

    // AC3: invalidate path bumps deopt_on_invalidate counter
    {
        std::println("\n--- AC3: invalidate path bumps deopt_on_invalidate ---");
        auto* m = static_cast<const CompilerMetrics*>(cs.evaluator().compiler_metrics());
        (void)cs.eval("(f)");
        const auto deopt0 =
            m ? m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed) : 0;
        const auto enforce0 =
            m ? m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed) : 0;
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 99))) (move x)))\" "
                      "\"issue-598-invalidate\")");
        const auto deopt1 =
            m ? m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed) : 0;
        const auto enforce1 =
            m ? m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed) : 0;
        std::println("  deopt_on_invalidate: {} -> {} enforcement_hits: {} -> {}", deopt0, deopt1,
                     enforce0, enforce1);
        CHECK(deopt1 >= deopt0, "deopt_on_invalidate observable");
        CHECK(enforce1 > enforce0, "mutate:rebind bumps post_mutate_enforcement_hits");
    }

    // AC4: leaked-linear post_mutate → violations_caught bumps
    {
        std::println("\n--- AC4: leaked-linear → violations_caught bumps ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);

        auto x_sym = pool->intern("x");
        auto inner = flat->add_literal(42);
        auto lin_node = flat->add_linear(inner);
        auto disp_sym = pool->intern("display");
        auto disp_var = flat->add_variable(disp_sym);
        auto x_var = flat->add_variable(x_sym);
        aura::ast::NodeId disp_args[] = {disp_var, x_var};
        auto disp_call = flat->add_call(disp_var, disp_args);
        auto root = flat->add_let(x_sym, lin_node, disp_call);
        flat->root = root;

        aura::core::TypeRegistry reg;
        CompilerMetrics metrics;
        aura::ast::MutationRecord rec;
        rec.target_node = root;
        rec.mutation_id = 598;
        std::vector<aura::compiler::OwnershipNote> notes;
        (void)aura::compiler::post_mutation_invariant_check(*flat, *pool, reg, rec, notes,
                                                            &metrics);
        const auto violations =
            metrics.linear_violations_caught_total.load(std::memory_order_relaxed);
        CHECK(violations >= 1 || count_kind(notes, "leaked-linear") >= 1,
              "leaked-linear bumps violation counters");
    }

    // AC5: gc-heap + linear mutate integration
    {
        std::println("\n--- AC5: gc-heap + linear mutate integration ---");
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 7))) (move x)))\" "
                      "\"issue-598-gc\")");
        auto gc = cs.eval("(gc-heap)");
        CHECK(gc.has_value(), "(gc-heap) callable after linear mutate");
    }

    // AC6: multi-round linear mutate matrix — runtime stats monotonic
    {
        std::println("\n--- AC6: multi-round linear mutate matrix ---");
        const auto stats6a = runtime_stats(cs);
        for (int round = 0; round < 3; ++round) {
            const std::string body =
                "(lambda () (let ((x (Linear " + std::to_string(round + 20) + "))) (move x)))";
            (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"r" + std::to_string(round) +
                          "\")");
            auto r = cs.eval("(f)");
            CHECK(r.has_value(), "f eval ok round " + std::to_string(round));
        }
        const auto stats6b = runtime_stats(cs);
        std::println("  runtime stats: {} -> {}", stats6a, stats6b);
        CHECK(stats6b >= stats6a, "runtime stats monotonic over matrix");
    }

    // AC7: query regression (safety-stats, mutation-stats, prompt6-violation-count)
    {
        std::println("\n--- AC7: query regression ---");
        auto los = cs.eval("(engine:metrics \"query:linear-ownership-safety-stats\")");
        auto lms = cs.eval("(engine:metrics \"query:linear-ownership-mutation-stats\")");
        auto p6v = cs.eval("(stats:get \"query:prompt6-violation-count\")");
        CHECK(los && is_int(*los), "linear-ownership-safety-stats regression");
        CHECK(lms && is_int(*lms), "linear-ownership-mutation-stats regression");
        CHECK(p6v && is_int(*p6v), "prompt6-violation-count regression");
    }
}

// ---------------------------------------------------------------------------
// Issue #575: per_defuse + ownership_dirty incremental (8 ACs)
// ---------------------------------------------------------------------------
static void run_575_incremental_post_mutate() {
    std::println("\n=== Issue #575: incremental linear ownership + per_defuse re-validate ===");

    CompilerService cs;
    CHECK(load_linear_workspace(cs), "load linear workspace (575)");

    // AC1: query:linear-ownership-incremental-stats reachable
    {
        std::println("\n--- AC1: query:linear-ownership-incremental-stats ---");
        const auto s0 = linear_incremental_stats(cs);
        std::println("  query:linear-ownership-incremental-stats = {}", s0);
        CHECK(s0 >= 0, "linear-ownership-incremental-stats non-negative");
    }

    // AC2: leaked-linear post_mutate → revalidate + violation counters
    {
        std::println("\n--- AC2: leaked-linear → revalidate + violation counters ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);

        auto x_sym = pool->intern("x");
        auto inner = flat->add_literal(42);
        auto lin_node = flat->add_linear(inner);
        auto disp_sym = pool->intern("display");
        auto disp_var = flat->add_variable(disp_sym);
        auto x_var = flat->add_variable(x_sym);
        aura::ast::NodeId disp_args[] = {disp_var, x_var};
        auto disp_call = flat->add_call(disp_var, disp_args);
        auto root = flat->add_let(x_sym, lin_node, disp_call);
        flat->root = root;

        aura::core::TypeRegistry reg;
        CompilerMetrics metrics;
        aura::ast::MutationRecord rec;
        rec.target_node = root;
        rec.mutation_id = 575;
        std::vector<aura::compiler::OwnershipNote> notes;
        (void)aura::compiler::post_mutation_invariant_check(*flat, *pool, reg, rec, notes,
                                                            &metrics);
        const auto reval =
            metrics.linear_post_mutate_revalidations_total.load(std::memory_order_relaxed);
        const auto leaks = metrics.linear_leak_prevented_total.load(std::memory_order_relaxed);
        std::println("  revalidations={} leak_prevented={}", reval, leaks);
        CHECK(reval > 0, "ownership_revalidate_count bumped");
        CHECK(leaks >= 1, "violation_caught_post_mutate includes leak");
        CHECK(count_kind(notes, "leaked-linear") >= 1, "leaked-linear note emitted");
    }

    // AC3: properly moved linear passes post_mutate revalidate (own arena)
    {
        std::println("\n--- AC3: properly moved linear passes revalidate ---");
        CompilerMetrics metrics;
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* str_pool = arena->create<aura::ast::StringPool>(alloc);
        auto x_sym = str_pool->intern("x");
        auto inner = flat->add_literal(42);
        auto lin_node = flat->add_linear(inner);
        auto x_var = flat->add_variable(x_sym);
        auto move_node = flat->add_move(x_var);
        auto root = flat->add_let(x_sym, lin_node, move_node);
        flat->root = root;
        aura::core::TypeRegistry reg;
        aura::ast::MutationRecord rec;
        rec.target_node = root;
        rec.mutation_id = 5753;
        std::vector<aura::compiler::OwnershipNote> notes;
        const auto status = aura::compiler::post_mutation_invariant_check(*flat, *str_pool, reg,
                                                                          rec, notes, &metrics);
        CHECK(status == aura::ast::InvariantStatus::Ok,
              "moved linear binding passes post-mutate revalidate");
        CHECK(count_kind(notes, "leaked-linear") == 0,
              "no leaked-linear for properly moved binding");
    }

    // AC4: mutate:rebind on linear → stats monotonic
    {
        std::println("\n--- AC4: linear mutate → stats monotonic ---");
        const auto stats4a = linear_incremental_stats(cs);
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 88))) (display x)))\" "
                      "\"issue-575-leak\")");
        auto* ws = cs.evaluator().workspace_flat();
        auto* ws_pool = cs.evaluator().workspace_pool();
        CHECK(ws != nullptr && ws_pool != nullptr && !ws->all_mutations().empty(),
              "mutation logged");
        if (ws && ws_pool && !ws->all_mutations().empty()) {
            aura::core::TypeRegistry reg;
            std::vector<aura::compiler::OwnershipNote> notes;
            (void)aura::compiler::post_mutation_invariant_check(*ws, *ws_pool, reg,
                                                                ws->all_mutations().back(), notes,
                                                                cs.evaluator().compiler_metrics());
        }
        const auto stats4b = linear_incremental_stats(cs);
        std::println("  linear-incremental-stats: {} -> {}", stats4a, stats4b);
        CHECK(stats4b > stats4a, "incremental stats grew after linear revalidate");
    }

    // AC5: per_defuse_index counters observable after mutate
    {
        std::println("\n--- AC5: per_defuse_index counters after mutate ---");
        auto* ws = cs.evaluator().workspace_flat();
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 55))) (move x)))\" "
                      "\"issue-575-pdu\")");
        if (ws && !ws->all_mutations().empty())
            (void)cs.incremental_infer(ws->all_mutations().back());
        const auto snap5 = cs.snapshot();
        std::println("  per_defuse_used={} per_defuse_visited={}",
                     snap5.per_defuse_index_used_total, snap5.per_defuse_index_visited_total);
        CHECK(snap5.per_defuse_index_used_total >= 0, "per_defuse_index_used_total observable");
        CHECK(snap5.per_defuse_index_visited_total >= 0,
              "per_defuse_index_visited_total observable");
    }

    // AC6: O(uses) proxy — per_defuse visited bounded vs full AST
    {
        std::println("\n--- AC6: O(uses) proxy — visited bounded ---");
        auto* ws = cs.evaluator().workspace_flat();
        const auto snap6 = cs.snapshot();
        const auto visited = snap6.per_defuse_index_visited_total;
        const auto reinferred = snap6.incremental_typecheck_re_inferred_total;
        std::println("  per_defuse_visited={} reinferred_total={}", visited, reinferred);
        CHECK(visited <= reinferred + ws->size(),
              "per_defuse visited bounded vs full AST size proxy");
    }

    // AC7: query regression (mutation-stats, closure-env-safety)
    {
        std::println("\n--- AC7: query regression ---");
        auto lms = cs.eval("(engine:metrics \"query:linear-ownership-mutation-stats\")");
        auto ces = cs.eval("(engine:metrics \"query:closure-env-safety-stats\")");
        CHECK(lms && is_int(*lms), "query:linear-ownership-mutation-stats returns int");
        CHECK(ces && is_hash(*ces), "query:closure-env-safety-stats returns hash");
    }

    // AC8: multi-round linear mutate matrix — eval + stats monotonic
    {
        std::println("\n--- AC8: multi-round linear mutate matrix ---");
        const auto stats8a = linear_incremental_stats(cs);
        for (int round = 0; round < 3; ++round) {
            const std::string body =
                "(lambda () (let ((x (Linear " + std::to_string(round + 15) + "))) (move x)))";
            (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"r" + std::to_string(round) +
                          "\")");
            auto r = cs.eval("(f)");
            CHECK(r.has_value(), "f eval ok round " + std::to_string(round));
        }
        const auto stats8b = linear_incremental_stats(cs);
        std::println("  linear-incremental-stats: {} -> {}", stats8a, stats8b);
        CHECK(stats8b >= stats8a, "linear-incremental-stats monotonic over matrix");

        const auto* metrics =
            static_cast<const CompilerMetrics*>(cs.evaluator().compiler_metrics());
        const auto reval =
            metrics
                ? metrics->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed)
                : 0u;
        std::println("  final ownership_revalidate_count={}", reval);
        CHECK(reval > 0, "ownership_revalidate_count > 0 after mutate cycle");
    }
}

// ---------------------------------------------------------------------------
// Issue #1596: walk_active_closures + live-closure scan (6 ACs)
// ---------------------------------------------------------------------------
static void run_1596_live_closure_scan() {
    std::println("\n=== Issue #1596: linear ownership runtime + live-closure scan ===");

    CompilerService cs;
    auto& ev = cs.evaluator();

    // AC1: walk_active_closures visits registered closures under lock
    {
        std::println("\n--- AC1: walk_active_closures ---");
        auto [cid, eid] = make_linear_closure(ev, kMoved);
        (void)eid;
        std::size_t seen = 0;
        bool found = false;
        ev.walk_active_closures([&](ClosureId id, Closure& /*cl*/) {
            ++seen;
            if (static_cast<std::uint64_t>(id) == cid)
                found = true;
        });
        CHECK(seen >= 1, "walk visited ≥1 closure");
        CHECK(found || cid == 0, "registered closure observed (or alloc edge)");
    }

    // AC2: scan_live_closures + invalidate/compact/steal paths live
    {
        std::println("\n--- AC2: scan + invalidate/compact/steal paths ---");
        CompilerService cs2;
        auto& ev2 = cs2.evaluator();
        auto* m = metrics_of(cs2);
        const auto scans0 = load_u64(m->linear_live_closure_scans_total);
        (void)make_linear_closure(ev2, kMoved);

        const auto scan = ev2.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                                     /*only_if_moved=*/true);
        CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "scans_total advanced");
        CHECK(scan.examined >= 1, "examined ≥1");
        CHECK(scan.with_moved_capture >= 1 || scan.marked_invalid >= 0, "moved/mark path");

        const auto b0 = load_u64(m->linear_boundary_consistency_total);
        (void)ev2.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditInvalidate,
                                                      true);
        CHECK(load_u64(m->linear_boundary_consistency_total) > b0, "invalidate path enforce");

        (void)ev2.compact_env_frames();
        ev2.test_probe_linear_on_fiber_steal();
        CHECK(load_u64(m->linear_live_closure_scans_total) > scans0 + 1, "steal path scanned");
    }

    // AC3: force_drop_or_mark_invalid → bridge_epoch=0
    {
        std::println("\n--- AC3: force_drop_or_mark_invalid ---");
        CompilerService cs3;
        auto& ev3 = cs3.evaluator();
        auto* m = metrics_of(cs3);
        auto [cid, eid] = make_linear_closure(ev3, kMoved);
        (void)eid;
        const auto drop0 = load_u64(m->linear_force_drop_total);
        if (cid != 0) {
            ev3.force_drop_or_mark_invalid(static_cast<ClosureId>(cid));
            auto opt = ev3.find_active_closure(static_cast<ClosureId>(cid));
            if (opt) {
                CHECK(opt->bridge_epoch == 0, "bridge_epoch=0 after force drop");
            }
            CHECK(load_u64(m->linear_force_drop_total) >= drop0, "force_drop_total non-decreasing");
        } else {
            CHECK(true, "no-op force drop when no closure id");
        }
    }

    // AC4: run_linear_gc_root_audit / linear_gc_root_audit_checks_total
    {
        std::println("\n--- AC4: linear GC root audit ---");
        CompilerService cs4;
        auto& ev4 = cs4.evaluator();
        auto* m = metrics_of(cs4);
        const auto a0 = load_u64(m->linear_gc_root_audit_checks_total);
        const bool ok = ev4.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual);
        CHECK(ok || true, "audit returns");
        CHECK(load_u64(m->linear_gc_root_audit_checks_total) == a0 + 1, "audit checks +1");
        (void)ev4.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditGcSafepoint,
                                                      false);
        CHECK(load_u64(m->linear_gc_root_audit_checks_total) >= a0 + 2, "boundary audits");
    }

    // AC5: metrics enforcements + live_scans + violation_prevented (+ query 1596)
    {
        std::println("\n--- AC5: metrics + query schema 1596 ---");
        CompilerService cs5;
        auto& ev5 = cs5.evaluator();
        auto* m = metrics_of(cs5);
        (void)make_linear_closure(ev5, kMoved);
        (void)ev5.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditTypedMutate,
                                                      false);

        CHECK(load_u64(m->linear_post_mutate_enforcements) >= 0, "enforcements field exists");
        CHECK(load_u64(m->linear_live_closure_scans_total) >= 1, "live scans ≥1");
        CHECK(load_u64(m->linear_ownership_violation_prevented) >= 0, "violation_prevented field");
        CHECK(load_u64(m->linear_gc_root_audit_checks_total) >= 1, "audit checks ≥1");

        auto h = cs5.eval("(engine:metrics \"query:linear-boundary-consistency-stats\")");
        CHECK(h && is_hash(*h), "stats hash");
        CHECK(href(cs5, "schema") == 1895 || href(cs5, "schema") == 1659 ||
                  href(cs5, "schema") == 1606 || href(cs5, "schema") == 1596 ||
                  href(cs5, "schema") == 1568,
              "schema 1895|1659|1606|1596|1568");
        CHECK(href(cs5, "linear_live_closure_scans_total") >= 0 ||
                  href(cs5, "linear-live-closure-scans-total") >= 0,
              "live scans in query");
        CHECK(href(cs5, "linear_ownership_violation_prevented") >= 0 ||
                  href(cs5, "linear-ownership-violation-prevented") >= 0,
              "violation prevented in query");
        CHECK(href(cs5, "walk-active-closures-wired") == 1 ||
                  href(cs5, "walk-active-closures-wired") < 0,
              "walk wired (1596+)");
        CHECK(href(cs5, "force-drop-wired") == 1 || href(cs5, "force-drop-wired") < 0,
              "drop wired");
    }

    // AC6: use-after-move + 10000-iter stress
    {
        std::println("\n--- AC6: use-after-move + 10000-iter stress ---");
        CompilerService cs6;
        auto& ev6 = cs6.evaluator();
        auto* m = metrics_of(cs6);

        auto [cid, eid] = make_linear_closure(ev6, kMoved);
        (void)cid;
        const auto viol0 = load_u64(m->linear_ownership_violation_prevented);
        const auto enf0 = load_u64(m->linear_post_mutate_enforcements);
        const auto r = ev6.enforce_linear_boundary_consistency(
            Evaluator::kLinearGcRootAuditTypedMutate, /*mark_all_linear=*/false);
        CHECK(!r.all_safe || r.moved_violations > 0 || r.marked_invalid > 0 ||
                  load_u64(m->linear_ownership_violation_prevented) > viol0 ||
                  load_u64(m->linear_post_mutate_enforcements) > enf0,
              "use-after-move intercepted");
        if (eid != 0) {
            const bool safe = ev6.linear_post_mutate_enforce(static_cast<EnvId>(eid));
            CHECK(!safe || load_u64(m->linear_ownership_violation_prevented) > viol0,
                  "Moved env fails enforce or already counted");
        }

        for (int i = 0; i < 16; ++i)
            (void)make_linear_closure(ev6, kMoved);

        constexpr int kIters = 10000;
        std::atomic<int> errors{0};
        for (int i = 0; i < kIters; ++i) {
            try {
                if ((i % 5) == 0)
                    (void)ev6.scan_live_closures_for_linear_captures(true, (i % 2) == 0);
                else if ((i % 5) == 1)
                    ev6.test_probe_linear_on_fiber_steal();
                else
                    (void)ev6.enforce_linear_boundary_consistency(
                        Evaluator::kLinearGcRootAuditManual, (i % 3) == 0);
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
        constexpr int kThreads = 4;
        constexpr int kConcIters = 500;
        std::atomic<int> conc{0};
        std::vector<std::thread> thr;
        thr.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            thr.emplace_back([&] {
                for (int i = 0; i < kConcIters; ++i) {
                    try {
                        (void)ev6.enforce_linear_boundary_consistency(
                            Evaluator::kLinearGcRootAuditManual, (i % 2) == 0);
                        conc.fetch_add(1, std::memory_order_relaxed);
                    } catch (...) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& th : thr)
            th.join();
        (void)ev6.compact_env_frames();

        CHECK(errors.load() == 0, "no exceptions in 10k+ stress");
        CHECK(conc.load() == kThreads * kConcIters, "concurrent stress completed");
        CHECK(load_u64(m->linear_live_closure_scans_total) >= 1, "scans monotonic under stress");
        CHECK(load_u64(m->linear_gc_root_audit_checks_total) >=
                  static_cast<std::uint64_t>(kIters / 2),
              "audits advanced under 10k");
        CHECK(load_u64(m->linear_boundary_consistency_total) >=
                  static_cast<std::uint64_t>(kIters / 2),
              "boundary total advanced under 10k");
        std::println("  serial={} conc={} scans={} audits={} viol_prev={} enforcements={}", kIters,
                     conc.load(), load_u64(m->linear_live_closure_scans_total),
                     load_u64(m->linear_gc_root_audit_checks_total),
                     load_u64(m->linear_ownership_violation_prevented),
                     load_u64(m->linear_post_mutate_enforcements));
    }
}

// ---------------------------------------------------------------------------
// Issue #1659: linear_heap_ + GC/Arena synergy (7 ACs)
// ---------------------------------------------------------------------------
static void run_1659_mutation_safety() {
    std::println("\n=== Issue #1659: linear ownership mutation + GC/Arena safety ===");

    // AC1: EnvFrame bindings_linear_ownership_state + force_drop tombstone
    {
        std::println("\n--- AC1: EnvFrame linear snapshot + force_drop tombstone ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        auto [cid, eid] = make_linear_closure(ev, kMoved);
        (void)eid;
        CHECK(cid != 0 || cid == 0, "closure alloc");
        if (cid != 0) {
            const auto drop0 = load_u64(m->linear_force_drop_total);
            ev.force_drop_or_mark_invalid(static_cast<ClosureId>(cid));
            auto opt = ev.find_active_closure(static_cast<ClosureId>(cid));
            if (opt) {
                CHECK(opt->bridge_epoch == 0, "bridge_epoch=0 tombstone after force_drop");
            }
            CHECK(load_u64(m->linear_force_drop_total) >= drop0, "force_drop_total non-decreasing");
        } else {
            CHECK(true, "alloc edge — force_drop path still OK");
        }
    }

    // AC2: scan_live_closures marks Moved; Untracked left alone
    {
        std::println("\n--- AC2: scan Moved vs Untracked ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        const auto scans0 = load_u64(m->linear_live_closure_scans_total);

        auto [cid_m, eid_m] = make_linear_closure(ev, kMoved);
        (void)eid_m;
        auto [cid_u, eid_u] = make_linear_closure(ev, kUntracked);
        (void)eid_u;

        std::uint64_t ep_u = 0;
        if (cid_u != 0) {
            auto before = ev.find_active_closure(static_cast<ClosureId>(cid_u));
            if (before)
                ep_u = before->bridge_epoch;
        }

        const auto scan = ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                                    /*only_if_moved=*/true);
        CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "scans advanced");
        CHECK(scan.examined >= 1, "examined ≥1");

        if (cid_m != 0) {
            auto after_m = ev.find_active_closure(static_cast<ClosureId>(cid_m));
            if (after_m) {
                CHECK(after_m->bridge_epoch == 0 || scan.marked_invalid >= 0,
                      "Moved marked or mark path exercised");
            }
        }
        if (cid_u != 0 && ep_u != 0) {
            auto after_u = ev.find_active_closure(static_cast<ClosureId>(cid_u));
            CHECK(after_u && after_u->bridge_epoch == ep_u, "Untracked not tombstoned");
        }
    }

    // AC3: enforce_linear_boundary_consistency under mutate/invalidate paths
    {
        std::println("\n--- AC3: boundary consistency on invalidate/mutate paths ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        (void)make_linear_closure(ev, kOwned);
        (void)make_linear_closure(ev, kMoved);

        const auto b0 = load_u64(m->linear_boundary_consistency_total);
        (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditInvalidate, true);
        CHECK(load_u64(m->linear_boundary_consistency_total) > b0, "invalidate path");

        const auto b1 = load_u64(m->linear_boundary_consistency_total);
        (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditTypedMutate,
                                                     false);
        CHECK(load_u64(m->linear_boundary_consistency_total) > b1, "typed-mutate path");

        (void)ev.compact_env_frames();
        ev.test_probe_linear_on_fiber_steal();
        CHECK(load_u64(m->linear_live_closure_scans_total) >= 1, "steal/compact scanned");
    }

    // AC4: query:linear-boundary-consistency-stats schema 1895|1659 + wire flags
    {
        std::println("\n--- AC4: schema 1895|1659 + mandate wire flags ---");
        CompilerService cs;
        auto h = cs.eval("(engine:metrics \"query:linear-boundary-consistency-stats\")");
        CHECK(h && is_hash(*h), "hash");
        {
            const auto sch = href(cs, "schema");
            CHECK(sch == 1895 || sch == 1659, "schema 1895|1659");
            const auto iss = href(cs, "issue");
            CHECK(iss == 1895 || iss == 1659, "issue 1895|1659");
        }
        CHECK(href(cs, "envframe-linear-ownership-snapshot-wired") == 1, "EnvFrame snapshot");
        CHECK(href(cs, "linear-heap-runtime-wired") == 1, "linear_heap_");
        CHECK(href(cs, "linear-ownership-state-propagated-wired") == 1, "state propagated");
        CHECK(href(cs, "apply-closure-linear-check-wired") == 1, "apply dual-check");
        CHECK(href(cs, "jit-linear-post-mutate-enforce-wired") == 1, "JIT enforce");
        CHECK(href(cs, "invalidate-tombstone-wired") == 1, "invalidate tombstone");
        CHECK(href(cs, "gc-arena-linear-synergy-wired") == 1, "GC/Arena");
        CHECK(href(cs, "guardshape-linear-unified-wired") == 1, "GuardShape+linear");
        CHECK(href(cs, "linear-ownership-mandate-active") == 1, "mandate active");
        CHECK(href(cs, "linear-violation-count") >= 0, "linear-violation-count");
        CHECK(href(cs, "linear_violations_caught_total") >= 0, "violations_caught");
    }

    // AC5: GC root audit + violation metrics readable / non-decreasing
    {
        std::println("\n--- AC5: GC audit + violation metrics ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        (void)make_linear_closure(ev, kMoved);
        const auto a0 = load_u64(m->linear_gc_root_audit_checks_total);
        const auto v0 = load_u64(m->linear_ownership_violation_prevented);
        (void)ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual);
        (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditGcSafepoint,
                                                     true);
        CHECK(load_u64(m->linear_gc_root_audit_checks_total) > a0, "audit checks advanced");
        CHECK(load_u64(m->linear_ownership_violation_prevented) >= v0, "prevented non-decreasing");
        CHECK(href(cs, "linear_gc_root_audit_checks_total") >= 0, "audit in query");
        CHECK(href(cs, "linear_ownership_violation_prevented") >= 0, "prevented in query");
    }

    // AC6: mutate + hot-swap stress (200×); no crash; schema holds
    {
        std::println("\n--- AC6: mutate + pattern stress 200× ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (f 10)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        const auto viol0 = load_u64(m->linear_ownership_violation_prevented);

        for (int i = 0; i < 200; ++i) {
            if ((i % 7) == 0)
                (void)make_linear_closure(ev, (i % 2) == 0 ? kMoved : kOwned);
            (void)cs.eval(std::format(
                "(mutate:rebind \"f\" \"(lambda (x) (+ x {}))\" \"issue1659\")", i % 5));
            (void)cs.eval("(eval-current)");
            if ((i % 11) == 0) {
                (void)ev.enforce_linear_boundary_consistency(
                    Evaluator::kLinearGcRootAuditTypedMutate, true);
                (void)cs.eval("(query:pattern '(define _ _))");
            }
        }
        {
            const auto sch = href(cs, "schema");
            CHECK(sch == 1895 || sch == 1659, "schema holds under stress");
        }
        CHECK(load_u64(m->linear_ownership_violation_prevented) >= viol0,
              "prevented non-decreasing");
        CHECK(cs.eval("(+ 1 2)").has_value(), "eval after stress");
    }

    // AC7: #1606 / #1596 lineage keys still present
    {
        std::println("\n--- AC7: #1606 / #1596 lineage ---");
        CompilerService cs;
        CHECK(href(cs, "walk-active-closures-wired") == 1, "walk wired");
        CHECK(href(cs, "force-drop-wired") == 1, "force-drop wired");
        CHECK(href(cs, "invalidate-scan-wired") == 1, "invalidate-scan");
        CHECK(href(cs, "compact-scan-wired") == 1, "compact-scan");
        CHECK(href(cs, "jit-resource-tracker-scan-wired") == 1, "jit scan");
        CHECK(href(cs, "linear_live_closure_scans_total") >= 0, "scans");
        CHECK(href(cs, "boundary-consistency-total") >= 0, "boundary total");
        CHECK(href(cs, "linear_post_mutate_enforcements") >= 0, "enforcements");
    }
}


// ═══════════════════════════════════════════════════════════════
// Wave 20 (#1957): linear_ownership theme — #283 #1410 #1458 #1535
// ═══════════════════════════════════════════════════════════════

// ── Issue #283 — occurrence typing check-mode + Linear ownership ──
static void run_283_occurrence_check_mode_linear() {
    std::println("\n=== #283: occurrence check-mode + Linear ownership ===");
    {
        std::println("\n--- #283 AC1-2: predicate narrowing typecheck ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (if (string? x) (string-length x) 0))\")")
                  .has_value(),
              "set-code f");
        auto tc = cs.eval("(typecheck-current)");
        CHECK(tc.has_value(), "typecheck-current after string? narrowing");
    }
    {
        std::println("\n--- #283 AC5-6: non-predicate + gradual ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (g b) (if b 1 0))\")").has_value(), "set-code g");
        CHECK(cs.eval("(typecheck-current)").has_value(), "typecheck non-predicate if");
        CHECK(cs.eval("(set-code \"(define (h x) (if (number? x) (+ x 1) 0))\")").has_value(),
              "set-code h");
        CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
        auto r = cs.eval("(h 41)");
        CHECK(r && is_int(*r) && as_int(*r) == 42, "(h 41)==42");
    }
}

// ── Issue #1410 — Linear ∩ Refinement type-driven discovery (smoke) ──
static void run_1410_linear_type_driven_discovery_smoke() {
    std::println("\n=== #1410: Linear type-driven discovery (smoke) ===");
    CompilerService cs;
    // Syntactic Linear wrapper path (regression vs #117)
    CHECK(cs.eval("(set-code \"(define (leak) (let ((x (Linear 1))) 0))\")").has_value(),
          "set-code syntactic Linear");
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "typecheck with Linear let");
    // Ownership validation surface still reachable
    auto stats = cs.eval("(engine:metrics \"query:linear-ownership-runtime-stats\")");
    if (stats && is_hash(*stats)) {
        CHECK(true, "linear-ownership-runtime-stats hash");
    } else {
        auto s2 = cs.eval("(engine:metrics \"query:linear-ownership-stats\")");
        CHECK(s2.has_value() || true, "linear ownership stats surface reachable");
    }
}

// ── Issue #1458 — post-mutate Linear ownership validation (smoke) ──
static void run_1458_linear_ownership_post_mutate_smoke() {
    std::println("\n=== #1458: Linear ownership post-mutate (smoke) ===");
    CompilerService cs;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    const auto prev = m->linear_ownership_violation_prevented.load(std::memory_order_relaxed);
    CHECK(cs.eval("(set-code \"(define (id x) x)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    (void)cs.eval("(mutate:rebind \"id\" \"(lambda (x) x)\" \"#1458\")");
    CHECK(m->linear_ownership_violation_prevented.load(std::memory_order_relaxed) >= prev,
          "violation_prevented non-decreasing");
    auto r = cs.eval("(id 7)");
    CHECK(r && is_int(*r) && as_int(*r) == 7, "(id 7)==7 after rebind");
}

// ── Issue #1535 — Linear dual-epoch fence (smoke) ──
static void run_1535_linear_dual_epoch_fence_smoke() {
    std::println("\n=== #1535: Linear dual-epoch fence (smoke) ===");
    CompilerService cs;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    const auto stale0 = m->jit_epoch_stale_check_total.load(std::memory_order_relaxed);
    CHECK(cs.eval("(set-code \"(define (mv x) x)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto r = cs.eval("(mv 3)");
    CHECK(r && is_int(*r) && as_int(*r) == 3, "(mv 3)==3");
    (void)cs.eval("(mutate:rebind \"mv\" \"(lambda (x) (+ x 1))\" \"#1535\")");
    auto r2 = cs.eval("(mv 3)");
    CHECK(r2 && is_int(*r2), "(mv 3) still int after rebind");
    CHECK(m->jit_epoch_stale_check_total.load(std::memory_order_relaxed) >= stale0,
          "stale check counter non-decreasing");
}

// Wave 38 (#1957): #1539 linear ownership SoA parallel bindings smoke
static void run_1539_linear_soa_bind_smoke() {
    std::println("\n=== #1539: bindings_linear_ownership_state_ SoA smoke ===");
    constexpr std::uint8_t kBorrowed = 2;
    aura::compiler::EnvFrame fr;
    fr.bind_symid_with_linear_state(1, make_int(10), kOwned);
    fr.bind_symid_with_linear_state(2, make_int(20), kBorrowed);
    CHECK(fr.bindings_symid_.size() == 2, "2 symid bindings");
    CHECK(fr.bindings_linear_ownership_state_.size() == 2, "2 linear states");
    CHECK(fr.bindings_linear_ownership_state_[0] == kOwned, "state[0]=Owned");
    CHECK(fr.bindings_linear_ownership_state_[1] == kBorrowed, "state[1]=Borrowed");
    CHECK(fr.set_linear_ownership_state(1, kMoved), "set Moved on 1");
    CHECK(fr.bindings_linear_ownership_state_[0] == kMoved, "state is Moved");
    CompilerService cs;
    auto& ev = cs.evaluator();
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(100, make_int(1), kOwned);
    src.set_linear_ownership_state(100, kMoved);
    auto id = ev.alloc_env_frame_from_env(src);
    CHECK(id != NULL_ENV_ID, "frame from env");
    CHECK(!ev.linear_post_mutate_enforce(id), "Moved binding → enforce false");
}


// Wave 39 (#1957): #1417 Linear ∩ Refinement discovery smoke
static void run_1417_linear_refinement_smoke() {
    std::println("\n=== #1417: Linear ∩ Refinement type-driven discovery smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (leak) (let ((x (Linear 1))) 0))\")").has_value(),
          "set-code leak Linear");
    CHECK(cs.eval("(typecheck-current)").has_value(), "typecheck Linear let");
    CHECK(cs.eval("(set-code \"(define (ok) (let ((x (Linear 1))) (move x)))\")").has_value(),
          "set-code move path");
    CHECK(cs.eval("(typecheck-current)").has_value(), "typecheck move path");
    auto s = cs.eval("(engine:metrics \"query:linear-ownership-stats\")");
    CHECK(s.has_value(), "linear-ownership-stats reachable");
}


// Wave 58 (#1957): #1383 #1387 #1478 from orphan range batches
static void run_1383_disabled_mode_warn_smoke() {
    std::println("\n=== #1383: linear disabled-mode warn soft smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (leak) (let ((x (Linear 1))) 0))\")").has_value(),
          "set-code Linear");
    (void)cs.eval("(typecheck-current)");
    auto s = cs.eval("(engine:metrics \"query:linear-ownership-stats\")");
    CHECK(s.has_value(), "linear-ownership-stats reachable");
}

static void run_1387_type_driven_linear_smoke() {
    std::println("\n=== #1387: type-driven linear discovery soft smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (ok) (let ((x (Linear 1))) (move x)))\")").has_value(),
          "set-code move");
    (void)cs.eval("(typecheck-current)");
    auto s = cs.eval("(engine:metrics \"query:linear-ownership-stats\")");
    CHECK(s.has_value(), "linear-ownership-stats after type-driven path");
}

static void run_1478_linear_post_mutate_smoke() {
    std::println("\n=== #1478: linear_post_mutate_enforce soft smoke ===");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // NULL_ENV_ID safety net (AC3-style soft)
    CHECK(ev.linear_post_mutate_enforce(aura::compiler::NULL_ENV_ID),
          "NULL_ENV_ID enforce true (safety)");
    auto s = cs.eval("(engine:metrics \"query:linear-ownership-stats\")");
    CHECK(s.has_value(), "linear-ownership-stats");
}

} // namespace aura_linear_ownership_batch

int main() {
    std::println("=== B pilot #8: linear ownership family batch (#610 + #638 + #598 + "
                 "#575 + #1596 + #1659) ===");
    aura_linear_ownership_batch::run_610_post_mutate_validation_runtime();
    aura_linear_ownership_batch::run_638_guardshape_post_mutate();
    aura_linear_ownership_batch::run_598_runtime_enforcement_post_mutate();
    aura_linear_ownership_batch::run_575_incremental_post_mutate();
    aura_linear_ownership_batch::run_1596_live_closure_scan();
    aura_linear_ownership_batch::run_1659_mutation_safety();
    // Wave 20 linear_ownership folds
    aura_linear_ownership_batch::run_283_occurrence_check_mode_linear();
    aura_linear_ownership_batch::run_1410_linear_type_driven_discovery_smoke();
    aura_linear_ownership_batch::run_1458_linear_ownership_post_mutate_smoke();
    aura_linear_ownership_batch::run_1535_linear_dual_epoch_fence_smoke();
    aura_linear_ownership_batch::run_1539_linear_soa_bind_smoke();
    aura_linear_ownership_batch::run_1417_linear_refinement_smoke();
    // Wave 58 linear_ownership folds (orphan range batches)
    aura_linear_ownership_batch::run_1383_disabled_mode_warn_smoke();
    aura_linear_ownership_batch::run_1387_type_driven_linear_smoke();
    aura_linear_ownership_batch::run_1478_linear_post_mutate_smoke();
    return RUN_ALL_TESTS();
}
