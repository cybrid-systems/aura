// @category: unit
// @reason: Issue #2196 — unified query:mutation-memory / query:blame-of
// Agent self-repair surface (mutation log + composite + dirty join).
//
//   AC1: Single EDSL query returns structured blame/memory for node
//        or mutation_id (no multi-stats scrape)
//   AC2: Composite txn children link via parent_mutation_id /
//        composite_transaction_id in the returned tree
//   AC3: After failed Guard rollback, status=RolledBack and
//        live-effects=0 (does not claim live effects)
//   AC4: Schema + metrics registered (schema-2196, mutation_memory_*)
//   AC5: Multi-step composite mutate → one query → Agent identifies
//        root mutation and affected StableNodeRefs / nodes

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:mutation-memory\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_q(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_mid(CompilerService& cs, std::int64_t mid, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:mutation-memory\" {}) \"{}\")", mid, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool setup_ws(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2) (define z 3)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

// ── AC1: single structured surface ──────────────────────────
static void ac1_single_query_surface() {
    std::println("\n--- AC1: single query:mutation-memory surface ---");
    CompilerService cs;
    CHECK(setup_ws(cs), "setup workspace");
    auto mr = cs.typed_mutate("(mutate:rebind \"x\" \"10\")");
    CHECK(mr.success, "typed_mutate rebind x");

    CHECK(href(cs, "schema-2196") == 2196, "schema-2196");
    CHECK(href(cs, "issue-2196") == 2196, "issue-2196");
    CHECK(href(cs, "mutation-memory-wired") == 1, "wired");
    CHECK(href(cs, "found") == 1, "found latest mutation");
    CHECK(href(cs, "mutation-id") > 0, "mutation-id set");
    CHECK(href(cs, "target-node") >= 0, "target-node present");
    CHECK(href(cs, "live-effects") == 1, "committed → live-effects=1");
    CHECK(href(cs, "status") == 0, "status Committed=0");
    CHECK(href(cs, "join-size") >= 1, "join-size >= 1");

    // Alias query:blame-of
    CHECK(href_q(cs, "query:blame-of", "schema-2196") == 2196, "blame-of alias schema");
    CHECK(href_q(cs, "query:blame-of", "found") == 1, "blame-of found");

    // By mutation-id
    const auto mid = href(cs, "mutation-id");
    CHECK(href_mid(cs, mid, "found") == 1, "lookup by mid found");
    CHECK(href_mid(cs, mid, "mutation-id") == mid, "mid round-trip");
    CHECK(href_mid(cs, mid + 999999, "found") == 0, "missing mid → found=0");
}

// ── AC2: composite parent / composite_transaction_id tree ───
static void ac2_composite_txn_links() {
    std::println("\n--- AC2: composite txn parent/composite links ---");
    CompilerService cs;
    CHECK(setup_ws(cs), "setup");
    std::array<std::string_view, 3> mutations = {
        "(mutate:rebind \"x\" \"11\")",
        "(mutate:rebind \"y\" \"22\")",
        "(mutate:rebind \"z\" \"33\")",
    };
    auto result = cs.typed_mutate_atomic(mutations);
    CHECK(result.success, "typed_mutate_atomic happy path");

    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace present");
    const auto log = ws->mutation_log_view();
    CHECK(!log.empty(), "mutation log non-empty");

    // Find a record that has composite + parent chain (later sub-mutation).
    const aura::ast::MutationRecord* child = nullptr;
    const aura::ast::MutationRecord* root = nullptr;
    for (std::size_t i = log.size(); i-- > 0;) {
        if (log[i].composite_transaction_id == 0)
            continue;
        if (log[i].parent_mutation_id != 0 && !child)
            child = &log[i];
        if (log[i].parent_mutation_id == 0 && !root)
            root = &log[i];
    }
    // Walk all for root of same composite as child
    if (child) {
        for (const auto& r : log) {
            if (r.composite_transaction_id == child->composite_transaction_id &&
                r.parent_mutation_id == 0) {
                root = &r;
                break;
            }
        }
    }
    CHECK(child != nullptr || root != nullptr, "composite provenance stamped on atomic batch");

    if (child) {
        const auto mid = static_cast<std::int64_t>(child->mutation_id);
        CHECK(href_mid(cs, mid, "found") == 1, "child found");
        CHECK(href_mid(cs, mid, "parent-mutation-id") ==
                  static_cast<std::int64_t>(child->parent_mutation_id),
              "parent-mutation-id matches record");
        CHECK(href_mid(cs, mid, "composite-transaction-id") ==
                  static_cast<std::int64_t>(child->composite_transaction_id),
              "composite-transaction-id matches");
        CHECK(href_mid(cs, mid, "composite-sibling-count") >= 2, "siblings in composite >= 2");
        CHECK(href_mid(cs, mid, "root-mutation-id") ==
                      static_cast<std::int64_t>(child->parent_mutation_id == 0
                                                    ? child->mutation_id
                                                    : child->parent_mutation_id) ||
                  href_mid(cs, mid, "root-mutation-id") > 0,
              "root-mutation-id resolved");
        CHECK(href_mid(cs, mid, "chain-depth") >= 1, "chain-depth >= 1");

        // Lookup by composite id returns a tree entry.
        auto r = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:mutation-memory\" 2 {}) \"found\")",
                        child->composite_transaction_id));
        CHECK(r.has_value() && is_int(*r) && as_int(*r) == 1, "lookup by composite mode found");
    } else if (root) {
        CHECK(href_mid(cs, static_cast<std::int64_t>(root->mutation_id),
                       "composite-transaction-id") ==
                  static_cast<std::int64_t>(root->composite_transaction_id),
              "root composite id");
        CHECK(href_mid(cs, static_cast<std::int64_t>(root->mutation_id),
                       "composite-sibling-count") >= 1,
              "at least self in sibling count");
    }
}

// ── AC3: RolledBack → live-effects=0 ────────────────────────
static void ac3_rollback_no_live_effects() {
    std::println("\n--- AC3: RolledBack status → live-effects=0 ---");
    CompilerService cs;
    CHECK(setup_ws(cs), "setup");

    // 1) Direct rollback leaves status=RolledBack in the audit log.
    auto mr = cs.typed_mutate("(mutate:rebind \"x\" \"55\")");
    CHECK(mr.success, "committed rebind for rollback");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr && !ws->mutation_log_view().empty(), "ws + log");
    // Prefer MutationResult.mutation_id; fall back to log.back() when 0.
    std::uint64_t mid_u = mr.mutation_id;
    if (mid_u == 0)
        mid_u = ws->mutation_log_view().back().mutation_id;
    CHECK(mid_u != 0, "resolved non-zero mutation_id");
    CHECK(ws->rollback(mid_u), "flat.rollback marks RolledBack");
    {
        const auto mid = static_cast<std::int64_t>(mid_u);
        CHECK(href_mid(cs, mid, "found") == 1, "rolled-back record still queryable");
        CHECK(href_mid(cs, mid, "status") == 1, "status=RolledBack");
        CHECK(href_mid(cs, mid, "live-effects") == 0, "AC3: live-effects=0");
        CHECK(href_mid(cs, mid, "safe-to-remutate") == 1, "safe-to-remutate after rollback");
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "metrics");
        CHECK(m->mutation_memory_rolled_back_total.load() >= 1, "rolled_back metric bumps");
    }

    // 2) Failed Guard (atomic abort) does not grow committed count.
    const std::size_t committed_before = ws->committed_mutation_count();
    std::array<std::string_view, 3> mutations = {
        "(mutate:rebind \"x\" \"100\")",
        "(mutate:rebind \"y\" \"200\")",
        "(mutate:rebind \"z\"   ", // parse fail → RAII rollback
    };
    auto result = cs.typed_mutate_atomic(mutations);
    CHECK(!result.success, "atomic abort expected");
    CHECK(ws->committed_mutation_count() == committed_before,
          "failed Guard: no new committed mutations (no live effects claimed)");
}

// ── AC4: schema + metrics + source surface ──────────────────
static void ac4_schema_metrics() {
    std::println("\n--- AC4: schema + metrics + catalog ---");
    CompilerService cs;
    CHECK(setup_ws(cs), "setup");
    (void)cs.typed_mutate("(mutate:rebind \"x\" \"7\")");

    CHECK(href(cs, "schema-2196") == 2196, "schema-2196");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics ptr");
    CHECK(m->mutation_memory_query_total.load() >= 1, "mutation_memory_query_total");
    CHECK(m->mutation_memory_found_total.load() >= 1, "mutation_memory_found_total");
    CHECK(m->mutation_memory_join_size_last.load() >= 1, "join_size_last");

    auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    CHECK(fields.find("mutation_memory_query_total") != std::string::npos, "fields query total");
    CHECK(fields.find("mutation_memory_join_size_last") != std::string::npos, "fields join size");
    CHECK(fields.find("mutation_memory_found_total") != std::string::npos, "fields found");
    CHECK(fields.find("mutation_memory_rolled_back_total") != std::string::npos,
          "fields rolled_back");

    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:mutation-memory") != std::string::npos, "register mutation-memory");
    CHECK(q.find("query:blame-of") != std::string::npos, "register blame-of alias");
    CHECK(q.find("schema-2196") != std::string::npos, "schema key in source");
    CHECK(q.find("register_stats_impl") != std::string::npos, "stats catalog path");

    auto obs = read_file("src/compiler/observability_metrics.h");
    CHECK(obs.find("mutation_memory_query_total") != std::string::npos, "obs metrics");
    CHECK(obs.find("Issue #2196") != std::string::npos, "cites #2196");
}

// ── AC5: multi-step composite → Agent identifies root + nodes ─
static void ac5_agent_closed_loop() {
    std::println("\n--- AC5: multi-step composite → root + affected nodes ---");
    CompilerService cs;
    CHECK(setup_ws(cs), "setup");

    std::array<std::string_view, 3> mutations = {
        "(mutate:rebind \"x\" \"41\")",
        "(mutate:rebind \"y\" \"42\")",
        "(mutate:rebind \"z\" \"43\")",
    };
    auto result = cs.typed_mutate_atomic(mutations);
    CHECK(result.success, "atomic multi-mutate");

    // One query — Agent identifies latest + chain without multi-stats.
    CHECK(href(cs, "found") == 1, "one-query found");
    const auto mid = href(cs, "mutation-id");
    const auto root = href(cs, "root-mutation-id");
    const auto composite = href(cs, "composite-transaction-id");
    const auto target = href(cs, "target-node");
    const auto affected0 = href(cs, "affected-node-0");
    const auto siblings = href(cs, "composite-sibling-count");

    CHECK(mid > 0, "Agent sees mutation-id");
    CHECK(root > 0, "Agent sees root-mutation-id");
    CHECK(target >= 0, "Agent sees target-node / StableNodeRef id");
    CHECK(affected0 >= 0, "affected-node-0 sample");
    // Composite batch should join multiple records when provenance stamped.
    if (composite > 0) {
        CHECK(siblings >= 1, "composite siblings visible");
        // Agent can re-query by composite id alone.
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:mutation-memory\" 2 {}) \"root-mutation-id\")",
            composite));
        CHECK(r.has_value() && is_int(*r) && as_int(*r) > 0, "Agent re-query by composite → root");
    }

    // By-node mode: last mutation on target node.
    if (target >= 0) {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:mutation-memory\" 1 {}) \"mutation-id\")", target));
        CHECK(r.has_value() && is_int(*r) && as_int(*r) > 0, "by-node lookup works");
    }

    // Parity smoke: mutation-provenance still works; memory has more keys.
    CHECK(href(cs, "author-fingerprint") >= 0, "author field present");
    CHECK(href(cs, "dirty-now") >= 0, "dirty join present");
    CHECK(href(cs, "workspace-gen") >= 0, "workspace gen present");
}

} // namespace

int main() {
    std::println("=== Issue #2196: query:mutation-memory / blame surface ===");
    ac1_single_query_surface();
    ac2_composite_txn_links();
    ac3_rollback_no_live_effects();
    ac4_schema_metrics();
    ac5_agent_closed_loop();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
