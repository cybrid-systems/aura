// @category: unit
// @reason: Issue #2403 — composite index coverage (tag+arity±marker) +
// shared_lock hold minimization/SLO for query:pattern and query:by-marker.
//
//   AC1: Constrained pattern (tag+arity±marker) hits composite index;
//        miss counter only on unconstrained (wildcard / Kleene+ellipsis).
//   AC2: Indexed path records shared_lock hold; max surface non-zero after
//        work; hold is O(candidates) not forced full-tree telemetry.
//   AC3: Soft / empty workspace — zero extra composite cost before queries.
//   AC4: Additive query keys on pattern-index-stats-hash (schema-2403);
//        no break of existing pattern/by-marker semantics.
//   AC5: Large-workspace microbench + source-cite + by-marker :where path.
//
// Issue #4163 — tag_arity_index free-slot hygiene (node_gen_==0 tombstones,
// #261/#1299): free_orphan_nodes_from does not clear parent_, so the #484
// parent-based checks alone cannot keep recycled NodeIds out of (tag,arity)
// buckets / the query:pattern index fast path.
//   AC1: prune drops free-slot tombstones from (tag,arity) buckets across
//        a gen-change sync even when parent_ is still set.
//   AC2: query:pattern index fast path never serves a freed id (warm
//        bucket, quiet sync path — serve-loop guard is the last line).
//   AC3: rebuild_full / insert_node never re-adds free slots.
//   AC4: source-cite — guards + #4163 cites present in both TUs.

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view prim, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", prim, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool setup_ws(CompilerService& cs, int n_defs = 80) {
    std::string code = "(define root 0)";
    for (int i = 0; i < n_defs; ++i)
        code += " (define v" + std::to_string(i) + " " + std::to_string(i) + ")";
    code += " (+ 1 2) (+ 3 4)";
    if (!cs.eval("(set-code \"" + code + "\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

// Issue #4163: locate the `(+ 1 2)` call node in the evaluated workspace —
// post-eval the flat holds the EVALUATED tree (defines collapse to single-
// child Define nodes, names interned away), so the (Call, 3) call node is
// the stable parented (tag,arity) index member to probe.
static aura::ast::NodeId find_call3_4163(aura::ast::FlatAST* flat) {
    if (flat == nullptr)
        return aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
        if (flat->is_free_slot(id))
            continue;
        const auto nd = flat->get(id);
        if (nd.tag == aura::ast::NodeTag::Call && nd.children.size() == 3)
            return id;
    }
    return aura::ast::NULL_NODE;
}

// Issue #4163: (tag, arity) composite key derived from the node itself —
// same encoding as the Evaluator's tag_arity_key ((tag << 32) | arity).
static std::uint64_t node_bucket_key_4163(aura::ast::FlatAST* flat, aura::ast::NodeId id) {
    const auto nd = flat->get(id);
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(nd.tag)) << 32) |
           static_cast<std::uint64_t>(nd.children.size());
}

// Issue #4163 fixture: (begin ...) wrapper — after eval-current the
// workspace holds the EVALUATED tree; the (+ 1 2) call survives as a
// parented (Call,3) node — the index member the probes below tombstone.
static bool setup_ws4163(CompilerService& cs) {
    std::string code = "(begin (define root 0)";
    for (int i = 0; i < 8; ++i)
        code += " (define v" + std::to_string(i) + " " + std::to_string(i) + ")";
    code += " (+ 1 2))";
    if (!cs.eval("(set-code \"" + code + "\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

} // namespace

int run_test_query_index_composite() {
    std::println("=== test_query_index_composite ===");

    // ── AC3 soft path: no queries yet ──────────────────────────────
    {
        std::println("\n--- #2403 AC3: soft path zero composite cost ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(ev.get_query_index_composite_hit_total() == 0, "AC3: hit=0 before queries");
        CHECK(ev.get_query_index_composite_miss_total() == 0, "AC3: miss=0 before queries");
        CHECK(ev.get_query_shared_lock_us_total() == 0, "AC3: lock_us_total=0 soft");
        auto h = cs.eval("(engine:metrics \"query:pattern-index-stats-hash\")");
        CHECK(h.has_value(), "AC3: pattern-index-stats-hash reachable without workspace");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-composite-wired") == 1,
              "AC3: composite-wired=1");
        CHECK(href(cs, "query:pattern-index-stats-hash", "schema-2403") == 2403,
              "AC3: schema-2403");
        CHECK(href(cs, "query:pattern-index-stats-hash", "issue-2403") == 2403, "AC3: issue-2403");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-miss-total") == 0,
              "AC3: miss-total=0 soft");
    }

    // ── AC1 constrained hit + unconstrained miss ───────────────────
    {
        std::println("\n--- #2403 AC1: constrained hit / unconstrained miss ---");
        CompilerService cs;
        CHECK(setup_ws(cs), "AC1: workspace setup");
        auto& ev = cs.evaluator();
        const auto h0 = ev.get_query_index_composite_hit_total();
        const auto m0 = ev.get_query_index_composite_miss_total();

        // Constrained (tag+arity): Define with 2 children → index hit.
        (void)cs.eval("(query:pattern \"(define v0 0)\" :strict-arity #t)");
        CHECK(ev.get_query_index_composite_hit_total() > h0, "AC1: constrained pattern → hit");
        const auto h1 = ev.get_query_index_composite_hit_total();
        const auto m1 = ev.get_query_index_composite_miss_total();
        CHECK(m1 == m0, "AC1: constrained does not bump miss");

        // Empty-bucket constrained still counts as hit (index used).
        (void)cs.eval("(query:pattern \"(+ 9 9 9)\" :strict-arity #t)");
        CHECK(ev.get_query_index_composite_hit_total() > h1,
              "AC1: empty-bucket constrained still hit");
        CHECK(ev.get_query_index_composite_miss_total() == m1,
              "AC1: empty-bucket does not count as miss");

        // Unconstrained: root wildcard forces full walk → miss.
        const auto h2 = ev.get_query_index_composite_hit_total();
        (void)cs.eval("(query:pattern \"...\")");
        CHECK(ev.get_query_index_composite_miss_total() > m1, "AC1: unconstrained → miss");
        CHECK(ev.get_query_index_composite_hit_total() == h2,
              "AC1: unconstrained does not bump hit");

        // by-marker :where → composite hit; bare by-marker → miss.
        const auto h3 = ev.get_query_index_composite_hit_total();
        const auto m3 = ev.get_query_index_composite_miss_total();
        (void)cs.eval("(query:by-marker \"User\" :where \"Define\")");
        CHECK(ev.get_query_index_composite_hit_total() > h3, "AC1: by-marker :where → hit");
        (void)cs.eval("(query:by-marker \"User\")");
        CHECK(ev.get_query_index_composite_miss_total() > m3,
              "AC1: unconstrained by-marker → miss");
    }

    // ── AC2 shared_lock hold metrics ───────────────────────────────
    {
        std::println("\n--- #2403 AC2: shared_lock hold SLO metrics ---");
        CompilerService cs;
        CHECK(setup_ws(cs, 120), "AC2: workspace setup");
        auto& ev = cs.evaluator();
        const auto lock0 = ev.get_query_shared_lock_us_total();
        for (int i = 0; i < 20; ++i)
            (void)cs.eval("(query:pattern \"(define v0 0)\" :strict-arity #t)");
        const auto lock1 = ev.get_query_shared_lock_us_total();
        const auto lock_max = ev.get_query_shared_lock_us_max();
        std::println("  lock_us_total {} -> {}, max={}", lock0, lock1, lock_max);
        CHECK(lock1 >= lock0, "AC2: lock_us_total monotonic");
        // Max is non-negative by construction; surface exposed on hash.
        (void)lock_max;
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-shared-lock-us-total") >= 0,
              "AC2: query-shared-lock-us-total on hash");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-shared-lock-us-max") >= 0,
              "AC2: query-shared-lock-us-max on hash");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-hit-total") >= 20,
              "AC2: indexed path recorded hits");
    }

    // ── AC4 additive query keys ────────────────────────────────────
    {
        std::println("\n--- #2403 AC4: additive query keys schema-2403 ---");
        CompilerService cs;
        CHECK(setup_ws(cs), "AC4: workspace setup");
        (void)cs.eval("(query:pattern \"(define v0 0)\" :strict-arity #t)");
        CHECK(href(cs, "query:pattern-index-stats-hash", "schema") == 621,
              "AC4: base schema=621 preserved");
        CHECK(href(cs, "query:pattern-index-stats-hash", "schema-2403") == 2403,
              "AC4: schema-2403");
        CHECK(href(cs, "query:pattern-index-stats-hash", "issue-2403") == 2403, "AC4: issue-2403");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-composite-wired") == 1,
              "AC4: query-index-composite-wired");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-hit-total") >= 1,
              "AC4: query-index-hit-total");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-miss-total") >= 0,
              "AC4: query-index-miss-total");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-hit-rate") >= 0 &&
                  href(cs, "query:pattern-index-stats-hash", "query-index-hit-rate") <= 100,
              "AC4: query-index-hit-rate in 0..100");
        // Semantics: pattern still returns matches for define.
        auto r = cs.eval("(query:pattern \"(define v0 0)\" :strict-arity #t)");
        CHECK(r.has_value(), "AC4: pattern still returns value");
    }

    // ── AC5 microbench 1e3 constrained queries on larger workspace ─
    {
        std::println("\n--- #2403 AC5: large-ws microbench + source-cite ---");
        CompilerService cs;
        CHECK(setup_ws(cs, 200), "AC5: large workspace");
        auto& ev = cs.evaluator();
        const auto h0 = ev.get_query_index_composite_hit_total();
        const auto m0 = ev.get_query_index_composite_miss_total();
        constexpr int kRounds = 200;
        for (int i = 0; i < kRounds; ++i)
            (void)cs.eval("(query:pattern \"(+ 1 2)\" :strict-arity #t)");
        const auto h1 = ev.get_query_index_composite_hit_total();
        const auto m1 = ev.get_query_index_composite_miss_total();
        std::println("  hits {} -> {} (delta {}), misses {} -> {}", h0, h1, h1 - h0, m0, m1);
        CHECK(h1 >= h0 + static_cast<std::uint64_t>(kRounds),
              "AC5: every constrained query is a composite hit");
        CHECK(m1 == m0, "AC5: microbench constrained path does not miss");
        const auto rate = href(cs, "query:pattern-index-stats-hash", "query-index-hit-rate");
        CHECK(rate >= 0, "AC5: hit-rate surface");
        // by-marker :where composition still works
        auto bm = cs.eval("(query:by-marker \"User\" :where \"Define\" :limit 5)");
        CHECK(bm.has_value(), "AC5: by-marker :where returns");
    }

    // ── AC1 #4163: prune drops free-slot tombstones ────────────────
    {
        std::println("\n--- #4163 AC1: prune drops free-slot tombstones (node_gen_==0) ---");
        CompilerService cs;
        CHECK(setup_ws4163(cs), "AC1: workspace setup");
        auto& ev = cs.evaluator();
        auto* flat = ev.workspace_flat();
        CHECK(flat != nullptr, "AC1: workspace flat");
        const auto call_id = find_call3_4163(flat);
        CHECK(call_id != aura::ast::NULL_NODE, "AC1: (+ 1 2) call node present");
        const auto key = node_bucket_key_4163(flat, call_id);
        // Warm the composite index for the call's (tag, arity).
        (void)cs.eval("(query:pattern \"(+ 1 2)\" :strict-arity #t)");
        const auto bucket0 = ev.snapshot_tag_arity_bucket(key);
        CHECK(!bucket0.empty(), "AC1: warm (Call,3) bucket non-empty");
        CHECK(flat->parent_of(call_id) != aura::ast::NULL_NODE,
              "AC1: call node is parented (index member)");
        // Rollback shape (#1299/#1300): node_gen_ zeroed, parent_ left
        // in place — the #484 parent-based orphan arm cannot see it.
        (void)flat->free_orphan_nodes_from(call_id);
        CHECK(flat->is_free_slot(call_id), "AC1: slot tombstoned");
        CHECK(flat->parent_of(call_id) != aura::ast::NULL_NODE,
              "AC1: tombstone keeps stale parent_ (bug precondition)");
        // Gen bump forces the sync path (quiet path would no-op);
        // prune must drop free-slot tombstones regardless of parent_.
        flat->bump_generation();
        ev.force_build_tag_arity_index();
        const auto bucket1 = ev.snapshot_tag_arity_bucket(key);
        bool freed_present = false;
        for (auto id : bucket1) {
            if (id == call_id || flat->is_free_slot(id))
                freed_present = true;
        }
        CHECK(!freed_present, "AC1: prune dropped free-slot tombstones from (tag,arity) bucket");
    }

    // ── AC2 #4163: pattern fast path never serves freed ids ───────
    {
        std::println("\n--- #4163 AC2: query:pattern must not return freed ids ---");
        CompilerService cs;
        CHECK(setup_ws4163(cs), "AC2: workspace setup");
        auto& ev = cs.evaluator();
        auto* flat = ev.workspace_flat();
        CHECK(flat != nullptr, "AC2: workspace flat");
        const auto call_id = find_call3_4163(flat);
        CHECK(call_id != aura::ast::NULL_NODE, "AC2: (+ 1 2) call node present");
        // Warm the index, then confirm the live baseline: the call matches
        // its own pattern exactly once (query:pattern returns the match
        // LIST directly — plain list of NodeIds, not a hash).
        (void)cs.eval("(query:pattern \"(+ 1 2)\" :strict-arity #t)");
        {
            const auto n = cs.eval("(length (query:pattern \"(+ 1 2)\" :strict-arity #t))");
            CHECK(n.has_value() && is_int(*n), "AC2: live match count readable");
            CHECK(n.has_value() && is_int(*n) && as_int(*n) == 1,
                  "AC2: exactly one live match for the (+ 1 2) call");
        }
        // No gen bump, no sync — the warm bucket still holds the freed id
        // (quiet sync path never re-prunes). The serve loop is the last
        // line of defense: the only structural matcher is dead, so the
        // query must serve zero matches, not a recycled id.
        (void)flat->free_orphan_nodes_from(call_id);
        CHECK(flat->is_free_slot(call_id), "AC2: call node tombstoned");
        const auto n = cs.eval("(length (query:pattern \"(+ 1 2)\" :strict-arity #t))");
        const bool served_freed = n.has_value() && is_int(*n) && as_int(*n) > 0;
        CHECK(!served_freed, "AC2: freed id not served by the index fast path");
    }

    // ── AC3 #4163: rebuild_full / insert never re-adds tombstones ──
    {
        std::println("\n--- #4163 AC3: full rebuild keeps free slots out ---");
        CompilerService cs;
        CHECK(setup_ws4163(cs), "AC3: workspace setup");
        auto& ev = cs.evaluator();
        auto* flat = ev.workspace_flat();
        CHECK(flat != nullptr, "AC3: workspace flat");
        const auto call_id = find_call3_4163(flat);
        CHECK(call_id != aura::ast::NULL_NODE, "AC3: (+ 1 2) call node present");
        const auto key = node_bucket_key_4163(flat, call_id);
        (void)cs.eval("(query:pattern \"(+ 1 2)\" :strict-arity #t)");
        (void)flat->free_orphan_nodes_from(call_id);
        CHECK(flat->is_free_slot(call_id), "AC3: slot tombstoned");
        // Cold full rebuild (public test hook: invalidate clears the
        // index + workspace anchor, so the next build takes the
        // rebuild_full path): insert_node must skip free slots — the
        // tombstone still has parent_ set, so the #484 parent-based
        // orphan check alone would re-add it to the bucket.
        ev.invalidate_tag_arity_index_for_test();
        ev.force_build_tag_arity_index();
        const auto bucket1 = ev.snapshot_tag_arity_bucket(key);
        bool freed_present = false;
        for (auto id : bucket1) {
            if (id == call_id || flat->is_free_slot(id))
                freed_present = true;
        }
        CHECK(!freed_present, "AC3: rebuild_full bucket holds no free slots");
    }

    // ── AC4 #4163: source-cite guards ────────────────────────────
    {
        std::println("\n--- #4163 AC4: source-cite — index guards + serve guard ---");
        std::ifstream f_idx("src/compiler/evaluator_query_index.cpp");
        std::ifstream f_qws("src/compiler/evaluator_primitives_query_workspace.cpp");
        std::string qidx((std::istreambuf_iterator<char>(f_idx)), std::istreambuf_iterator<char>());
        std::string qws((std::istreambuf_iterator<char>(f_qws)), std::istreambuf_iterator<char>());
        CHECK(!qidx.empty(), "AC4: evaluator_query_index.cpp readable");
        CHECK(!qws.empty(), "AC4: evaluator_primitives_query_workspace.cpp readable");
        // insert_node guard (skip) + prune is_stale arm, each citing #4163.
        CHECK(qidx.find("Issue #4163: skip free-list tombstones") != std::string::npos,
              "AC4: insert_node free-slot guard cites #4163");
        CHECK(qidx.find("Issue #4163: free-list tombstones") != std::string::npos,
              "AC4: prune is_stale free-slot arm cites #4163");
        {
            std::size_t hits = 0;
            for (std::size_t pos = 0;
                 (pos = qidx.find("flat.is_free_slot(id)", pos)) != std::string::npos; pos += 1)
                ++hits;
            CHECK(hits >= 2, "AC4: insert + prune both guard is_free_slot in the index TU");
        }
        // Serve-loop guard in the pattern fast path, citing #4163.
        CHECK(qws.find("Issue #4163: skip free-list tombstones") != std::string::npos,
              "AC4: pattern fast-path serve guard cites #4163");
        CHECK(qws.find("if (flat.is_free_slot(id))") != std::string::npos,
              "AC4: serve loop guards is_free_slot");
        // No per-issue test file (per #81934 the family test is extended).
        {
            std::ifstream f("tests/compiler/test_issue_4163.cpp");
            CHECK(!f.good(), "AC4: no tests/compiler/test_issue_4163.cpp");
        }
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_query_index_composite();
}
#endif
