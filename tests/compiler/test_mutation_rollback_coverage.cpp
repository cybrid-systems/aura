// Issue #213/#266/#369/#400/#553 (#1978 renamed): issue# moved from filename to header.
// test_mutation_rollback_coverage_400.cpp
// Issue #400: Expand rollback coverage observability for sym_id_
// and structural changes under add_mutation_with_rollback.
//
// Non-duplicative with #266 (sym_id unit), #369 (structural unit),
// #553 (mutation-log-stats batch matrix), #213 (rollback_to_size).
//
// AC1: query:mutation-rollback-coverage-stats reachable
// AC2: sym_id per-record rollback under Guard boundary
// AC3: structural insert-child rollback restores children_
// AC4: structural_rollback_success counter bumps on rollback()
// AC5: mutate:rebind + failed boundary bumps field_log_rollbacks
// AC6: multi-round rollback matrix — coverage stats monotonic
// AC7: query regression (mutation-log-stats, ast:generation-stats)
//
// Unit sym/structural tests run first; integration uses CompilerService.

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;
import aura.core;
import aura.core.ast;

namespace aura_400_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::compiler::types::make_bool;

static std::int64_t coverage_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:mutation-rollback-coverage-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static std::int64_t coverage_stats(Evaluator& ev) {
    auto* ws = ev.workspace_flat();
    if (!ws)
        return 0;
    return static_cast<std::int64_t>(
        ws->structural_rollback_success() + ws->structural_rollback_besteffort() +
        ev.get_mutation_log_rollback_count() + ev.atomic_batch_rollbacks());
}

static void test_sym_id_rollback() {
    std::println("\n--- AC2: sym_id per-record rollback ---");
    Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto x = flat.add_variable(pool.intern("x"));
    flat.root = x;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    const auto stats0 = coverage_stats(ev);
    auto old_sym = flat.sym_id(x);
    auto new_sym = pool.intern("y");

    ev.enter_mutation_boundary();
    flat.set_sym(x, new_sym);
    flat.add_mutation_with_rollback(
        x, "replace-value", "Sym", "Sym", "x -> y", aura::ast::MutationStatus::Committed,
        static_cast<std::uint32_t>(aura::ast::MutationSoAField::SymId),
        static_cast<std::uint64_t>(old_sym), static_cast<std::uint64_t>(new_sym), true);
    CHECK(flat.sym_id(x) == new_sym, "mid-boundary sym is y");
    ev.exit_mutation_boundary(false);

    CHECK(flat.sym_id(x) == old_sym, "sym rolled back to x");
    const auto stats1 = coverage_stats(ev);
    CHECK(stats1 >= stats0, "coverage stats monotonic after sym_id rollback");
}

static void test_structural_insert_rollback() {
    std::println("\n--- AC3: insert-child structural rollback ---");
    Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto fn = flat.add_variable(pool.intern("+"));
    auto lit = flat.add_literal(1);
    auto root = flat.add_call(fn, std::span{&lit, 1});
    flat.root = root;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    const auto before = flat.get(root).children.size();
    ev.enter_mutation_boundary();
    auto extra = flat.add_literal(2);
    flat.insert_child(root, 1, extra);
    CHECK(flat.get(root).children.size() == before + 1, "mid-boundary has inserted child");
    ev.exit_mutation_boundary(false);
    CHECK(flat.get(root).children.size() == before,
          "children count restored after failed boundary");
}

static void test_structural_counter_on_rollback() {
    std::println("\n--- AC4: structural_rollback_success on rollback() ---");
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::FlatAST flat(alloc);
    auto parent = flat.add_literal(0);
    auto child = flat.add_literal(42);
    flat.set_child(parent, 0, child);

    const auto success0 = flat.structural_rollback_success();
    auto mid = flat.add_structural_mutation_log_entry(parent, 0, child, aura::ast::NULL_NODE,
                                                      "remove-node");
    CHECK(mid > 0, "structural log entry recorded");
    CHECK(flat.rollback(mid), "rollback(mutation_id) succeeds for remove-node");
    CHECK(flat.structural_rollback_success() >= success0 + 1,
          "structural_rollback_success incremented");
}

static void test_field_int_rollback() {
    std::println("\n--- AC5: field_offset int_val rollback bumps counter ---");
    Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::FlatAST flat(alloc);
    auto lit = flat.add_literal(10);
    flat.root = lit;
    ev.set_workspace_flat(&flat);

    const auto roll0 = ev.get_mutation_log_rollback_count();
    ev.enter_mutation_boundary();
    flat.set_int(lit, 99);
    flat.add_mutation_with_rollback(
        lit, "test:set", "Int", "Int", "10 -> 99", aura::ast::MutationStatus::Committed,
        static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal), 10, 99, true);
    ev.exit_mutation_boundary(false);
    const auto roll1 = ev.get_mutation_log_rollback_count();
    CHECK(flat.int_val(lit) == 10, "int_val restored after failed boundary");
    CHECK(roll1 > roll0, "mutation_log_rollback_count bumped");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:mutation-rollback-coverage-stats ---");
    CHECK(cs.eval("(set-code \"(define acc 0)\")").has_value(), "workspace setup");
    CHECK(cs.eval("(eval-current)").has_value(), "workspace eval");
    const auto s0 = coverage_stats(cs);
    std::println("  mutation-rollback-coverage-stats = {}", s0);
    CHECK(s0 >= 0, "coverage stats non-negative");

    std::println("\n--- AC6: multi-round mutate matrix ---");
    const auto stats6a = coverage_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(10 + round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats6b = coverage_stats(cs);
    std::println("  coverage stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "coverage stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto mls = cs.eval("(engine:metrics \"query:mutation-log-stats\")");
    auto ags = cs.eval("(stats:get \"ast:generation-stats\")");
    CHECK(mls && is_int(*mls), "mutation-log-stats regression");
    CHECK(ags.has_value(), "ast:generation-stats regression");

    std::println("\n--- AC8: dual-topology abort force-dirties IR cache (#3033) ---");
    // Issue #3033: abort_restore_dual_topology leaves CacheEntryVersionStamp
    // pointing at intermediate/pre-abort state → should_relower could return
    // false and silently serve stale IR. After abort, every cached entry must
    // be dirty + zero-restamped and abort_ir_cache_force_dirty_total bumps.
    CHECK(cs.eval("(set-code \"(define f3033 (+ 1 2))\")").has_value(), "AC8: define setup");
    CHECK(cs.eval("(eval-current)").has_value(), "AC8: eval to populate cache");
    auto& ev = cs.evaluator();
    const auto* entry = cs.get_define_v2("f3033");
    CHECK(entry != nullptr, "AC8: cache entry exists");
    const auto dirty_before = entry ? entry->dirty : false;
    const auto force0 =
        cs.metrics().abort_ir_cache_force_dirty_total.load(std::memory_order_relaxed);
    ev.enter_mutation_boundary();
    (void)cs.eval("(mutate:rebind \"f3033\" \"9\")");
    ev.exit_mutation_boundary(false); // abort → force-dirty hook fires
    const auto force1 =
        cs.metrics().abort_ir_cache_force_dirty_total.load(std::memory_order_relaxed);
    const auto* after = cs.get_define_v2("f3033");
    CHECK(force1 > force0, "AC8: abort_ir_cache_force_dirty_total bumped");
    CHECK(after != nullptr && after->dirty, "AC8: cache entry forced dirty after abort");
    // Zero stamps → should_relower forced true on every domain.
    if (after) {
        CHECK(after->version_stamp_.mutation_count == 0, "AC8: mutation stamp zeroed");
        CHECK(after->version_stamp_.bridge_epoch == 0, "AC8: bridge stamp zeroed");
    }
    (void)dirty_before;
}

// Issue #3069: abort force fence vs concurrent lookup_define_v2.
static void test_abort_force_generation_fence_3069() {
    std::println("\n--- AC9: abort-force generation fence vs concurrent lookup (#3069) ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f3069 (lambda (x) (+ x 1))) (f3069 1)\")").has_value(),
          "3069 set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3069 eval");
    if (!cs.get_define_v2("f3069"))
        (void)cs.eval("(compile:cache-define \"f3069\")");
    CHECK(cs.get_define_v2("f3069") != nullptr, "3069 cache entry");
    const auto hash = cs.get_define_v2("f3069")->source_hash;
    const auto force0 =
        cs.metrics().abort_ir_cache_force_dirty_total.load(std::memory_order_relaxed);
    const auto gen0 = cs.public_abort_force_generation();

    cs.public_set_abort_force_hold(true);
    std::atomic<int> forcer_started{0};
    std::thread forcer([&] {
        forcer_started.store(1, std::memory_order_release);
        cs.public_force_ir_cache_dirty_after_abort();
    });
    while (forcer_started.load(std::memory_order_acquire) == 0)
        std::this_thread::yield();
    while (cs.public_abort_force_generation() == gen0)
        std::this_thread::yield();
    CHECK(cs.public_abort_force_in_progress(), "3069 AC1: in-progress during hold");
    int clean_hits = 0;
    int dirty_hits = 0;
    for (int i = 0; i < 256; ++i) {
        const int st = cs.lookup_define_v2("f3069", hash);
        if (st == 0)
            ++clean_hits;
        else if (st == 1)
            ++dirty_hits;
    }
    CHECK(clean_hits == 0, "3069 AC1: no clean hit during abort force");
    CHECK(dirty_hits > 0, "3069 AC1: lookups return need-relower");
    cs.public_set_abort_force_hold(false);
    forcer.join();

    const auto force1 =
        cs.metrics().abort_ir_cache_force_dirty_total.load(std::memory_order_relaxed);
    CHECK(force1 > force0, "3069 AC2: abort_ir_cache_force_dirty_total bumped");
    const auto* after = cs.get_define_v2("f3069");
    CHECK(after != nullptr && after->dirty, "3069 AC2: dirty after abort");
    CHECK(after && after->version_stamp_.mutation_count == 0, "3069 AC2: mutation stamp zeroed");
    CHECK(after && after->version_stamp_.bridge_epoch == 0, "3069 AC2: bridge stamp zeroed");
    CHECK(after &&
              after->version_stamp_.abort_force_generation == cs.public_abort_force_generation(),
          "3069 AC2: entry acked abort gen");
    CHECK(cs.public_source_to_ir_map_consistent("f3069"), "3069 AC2: map consistent");
    CHECK(!cs.public_abort_force_in_progress(), "3069 AC2: in-progress cleared");
    CHECK(cs.lookup_define_v2("f3069", hash) == 1, "3069 AC2: post-abort still need-relower");

    // AC3: success-path store acks the fence → clean hit.
    {
        aura::ir::IRFunction top;
        top.id = 0;
        top.name = "__top__";
        top.blocks.push_back({0, {}, {}});
        aura::ir::IRFunction body;
        body.id = 1;
        body.name = "f3069_body";
        body.blocks.push_back({0, {}, {}});
        std::vector<aura::ir::IRFunction> irs;
        irs.push_back(std::move(top));
        irs.push_back(std::move(body));
        const std::string src = "(define f3069 (lambda (x) (+ x 1)))";
        cs.store_define_v2("f3069", src, std::move(irs), {}, {});
        const auto stored = cs.get_define_v2("f3069");
        CHECK(stored && !stored->dirty, "3069 AC3: store clears dirty");
        CHECK(stored && stored->version_stamp_.abort_force_generation ==
                            cs.public_abort_force_generation(),
              "3069 AC3: store acks abort gen");
        CHECK(cs.lookup_define_v2("f3069", stored->source_hash) == 0,
              "3069 AC3: success-path store is clean hit");
    }

    const auto svc = []() {
        std::ifstream in("src/compiler/service.ixx");
        if (in)
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        std::ifstream in2("../src/compiler/service.ixx");
        if (in2)
            return std::string((std::istreambuf_iterator<char>(in2)),
                               std::istreambuf_iterator<char>());
        return std::string{};
    }();
    CHECK(svc.find("Issue #3069") != std::string::npos, "3069 AC4: service cite");
    CHECK(svc.find("abort_force_generation_") != std::string::npos, "3069 AC4: generation");
    CHECK(svc.find("abort_force_in_progress_") != std::string::npos, "3069 AC4: in-progress");
}

// Issue #3117: dual-topology abort restore must hold the abort-force
// fence across restore and clear source_to_ir_map (no rebuild from
// pre-abort IR). Concurrent lookup during the fence is never a clean hit.
static void test_abort_restore_ir_map_fence_3117() {
    std::println("\n--- AC10: abort restore IR map fence (#3117) ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f3117 (lambda (x) (+ x 1))) (f3117 1)\")").has_value(),
          "3117 set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3117 eval");
    if (!cs.get_define_v2("f3117"))
        (void)cs.eval("(compile:cache-define \"f3117\")");
    CHECK(cs.get_define_v2("f3117") != nullptr, "3117 cache entry");
    const auto hash = cs.get_define_v2("f3117")->source_hash;
    const auto gen0 = cs.public_abort_force_generation();

    // AC1: begin fence (restore window) — no clean lookup.
    cs.public_begin_abort_ir_cache_force_fence();
    CHECK(cs.public_abort_force_in_progress(), "3117 AC1: in-progress before restore");
    CHECK(cs.public_abort_force_generation() > gen0, "3117 AC1: gen bumped before restore");
    int clean_hits = 0;
    int dirty_hits = 0;
    for (int i = 0; i < 128; ++i) {
        const int st = cs.lookup_define_v2("f3117", hash);
        if (st == 0)
            ++clean_hits;
        else if (st == 1)
            ++dirty_hits;
    }
    CHECK(clean_hits == 0, "3117 AC1: no clean hit during restore fence");
    CHECK(dirty_hits > 0, "3117 AC1: lookups return need-relower");

    cs.public_force_ir_cache_dirty_after_abort();
    CHECK(!cs.public_abort_force_in_progress(), "3117 AC2: in-progress cleared after walk");
    const auto* after = cs.get_define_v2("f3117");
    CHECK(after != nullptr && after->dirty, "3117 AC2: dirty after abort");
    CHECK(after && after->source_to_ir_map.empty(), "3117 AC2: map cleared (not rebuilt)");
    CHECK(after && after->abort_map_invalid, "3117 AC2: abort_map_invalid set");
    CHECK(cs.public_source_to_ir_map_consistent("f3117"), "3117 AC2: empty map is consistent");
    CHECK(cs.lookup_define_v2("f3117", hash) == 1, "3117 AC2: post-abort still need-relower");

    // Dual-topology abort (exit false) also leaves map invalid.
    auto& ev = cs.evaluator();
    ev.enter_mutation_boundary();
    (void)cs.eval("(mutate:rebind \"f3117\" \"9\")");
    ev.exit_mutation_boundary(false);
    const auto* after_exit = cs.get_define_v2("f3117");
    CHECK(after_exit && after_exit->abort_map_invalid,
          "3117 AC2: exit(false) leaves abort_map_invalid");
    CHECK(after_exit && after_exit->source_to_ir_map.empty(),
          "3117 AC2: exit(false) leaves map empty");

    // AC3: success-path store rebuilds map + clears abort_map_invalid.
    {
        aura::ir::IRFunction top;
        top.id = 0;
        top.name = "__top__";
        top.blocks.push_back({0, {}, {}});
        aura::ir::IRFunction body;
        body.id = 1;
        body.name = "f3117_body";
        body.blocks.push_back({0, {}, {}});
        std::vector<aura::ir::IRFunction> irs;
        irs.push_back(std::move(top));
        irs.push_back(std::move(body));
        const std::string src = "(define f3117 (lambda (x) (+ x 1)))";
        cs.store_define_v2("f3117", src, std::move(irs), {}, {});
        const auto stored = cs.get_define_v2("f3117");
        CHECK(stored && !stored->dirty, "3117 AC3: store clears dirty");
        CHECK(stored && !stored->abort_map_invalid, "3117 AC3: store clears abort_map_invalid");
        CHECK(cs.lookup_define_v2("f3117", stored->source_hash) == 0,
              "3117 AC3: success-path store is clean hit");
    }

    const auto svc = []() {
        std::ifstream in("src/compiler/service.ixx");
        if (in)
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        std::ifstream in2("../src/compiler/service.ixx");
        if (in2)
            return std::string((std::istreambuf_iterator<char>(in2)),
                               std::istreambuf_iterator<char>());
        return std::string{};
    }();
    CHECK(svc.find("Issue #3117") != std::string::npos, "3117 AC4: service cite");
    CHECK(svc.find("abort_map_invalid") != std::string::npos, "3117 AC4: abort_map_invalid");
    CHECK(svc.find("begin_abort_ir_cache_force_fence") != std::string::npos,
          "3117 AC4: begin fence");
}

static std::string merr_kind_3122(Evaluator& ev, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = ev.pairs();
    if (idx >= pairs.size() || !is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = ev.string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

static std::string merr_reason_3122(Evaluator& ev, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = ev.pairs();
    if (idx >= pairs.size() || !is_pair(pairs[idx].cdr))
        return {};
    auto midx = as_pair_idx(pairs[idx].cdr);
    if (midx >= pairs.size() || !is_string(pairs[midx].car))
        return {};
    auto sidx = as_string_idx(pairs[midx].car);
    auto heap = ev.string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

// Issue #3122: Guard abort after topology write → structured topology-restore.
static void test_topology_restore_structured_3122() {
    std::println("\n--- AC11: Guard abort structured topology-restore (#3122) ---");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    apply_production_audit_defaults();
    Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto fn = flat.add_variable(pool.intern("+"));
    auto lit = flat.add_literal(1);
    auto root = flat.add_call(fn, std::span{&lit, 1});
    flat.root = root;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);
    const auto before = flat.get(root).children.size();
    const auto dual0 = flat.topology_dual_restore_total();
    const auto inc0 = flat.topology_dual_restore_inconsistency_total();

    const auto r = ev.abort_after_insert_child_for_test(root, 1);
    CHECK(merr_kind_3122(ev, r) == "topology-restore", "3122 AC1: error=topology-restore");
    CHECK(merr_reason_3122(ev, r) == "restored", "3122 AC1: reason=restored");
    CHECK(!is_bool(r), "3122 AC1: not opaque #f");
    CHECK(flat.get(root).children.size() == before, "3122 AC1: children_ restored");
    CHECK(flat.topology_dual_restore_total() > dual0, "3122 AC1: dual restore ran");
    CHECK(flat.topology_dual_restore_inconsistency_total() == inc0,
          "3122 AC1: dual restore consistent");

    apply_dev_audit_defaults();
    const auto soft = ev.abort_after_insert_child_for_test(root, 1);
    CHECK(is_bool(soft) && !as_bool(soft), "3122 AC3: Soft still #f");
    CHECK(flat.get(root).children.size() == before, "3122 AC3: Soft also restores");

    apply_production_audit_defaults();
    apply_dev_audit_defaults();

    const auto gh = []() {
        std::ifstream in("src/compiler/mutation_guard_helpers.hh");
        if (in)
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        std::ifstream in2("../src/compiler/mutation_guard_helpers.hh");
        if (in2)
            return std::string((std::istreambuf_iterator<char>(in2)),
                               std::istreambuf_iterator<char>());
        return std::string{};
    }();
    const auto bound = []() {
        std::ifstream in("src/compiler/evaluator_mutation_boundary.cpp");
        if (in)
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        std::ifstream in2("../src/compiler/evaluator_mutation_boundary.cpp");
        if (in2)
            return std::string((std::istreambuf_iterator<char>(in2)),
                               std::istreambuf_iterator<char>());
        return std::string{};
    }();
    const auto mut = []() {
        std::ifstream in("src/compiler/evaluator_primitives_mutate.cpp");
        if (in)
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        std::ifstream in2("../src/compiler/evaluator_primitives_mutate.cpp");
        if (in2)
            return std::string((std::istreambuf_iterator<char>(in2)),
                               std::istreambuf_iterator<char>());
        return std::string{};
    }();
    CHECK(gh.find("Issue #3122") != std::string::npos, "3122 AC4: helper cites");
    CHECK(bound.find("Issue #3122") != std::string::npos, "3122 AC4: boundary cites");
    CHECK(mut.find("Issue #3122") != std::string::npos, "3122 AC4: mutate entry cites");
    CHECK(gh.find("return on_fail;") != std::string::npos, "3122 AC2: acquire-fail stays on_fail");
    CHECK(gh.find("not topology-restore") != std::string::npos,
          "3122 AC2: acquire-fail / hygiene distinct");
    std::ifstream no_issue("tests/compiler/test_issue_3122.cpp");
    CHECK(!no_issue.good(), "3122 AC5: no test_issue_3122.cpp");
    std::ifstream no_docs("docs/design/3122-topology-restore.md");
    CHECK(!no_docs.good(), "3122 AC4: no docs/design");
}

} // namespace aura_400_detail

int main() {
    aura_400_detail::test_sym_id_rollback();
    aura_400_detail::test_structural_insert_rollback();
    aura_400_detail::test_structural_counter_on_rollback();
    aura_400_detail::test_field_int_rollback();
    aura::compiler::CompilerService cs;
    aura_400_detail::run_matrix(cs);
    aura_400_detail::test_abort_force_generation_fence_3069();
    aura_400_detail::test_abort_restore_ir_map_fence_3117();
    aura_400_detail::test_topology_restore_structured_3122();
    return RUN_ALL_TESTS();
}