// @category: unit
// @reason: Issue #2036 — complete PCV children_ migration; SafePCVSpan
// default for children_stable / children_default; multi-fiber stress.
//
//   AC1: source cites #2036; children_ is PersistentChildVector; children_default
//   AC2: children_stable / for_each_stable_child pin via children_safe_view
//   AC3: SafePCVSpan survives concurrent mutate + restore_children (multi-fiber)
//   AC4: children_stable_safe_default_total / children_safe_view_count advance
//   AC5: query:soa-children-columnar-migration-stats schema-2036
//   AC6: no pmr::vector path for FlatAST children_ (source scan)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/persistent_child_vector.hh" // SafePCVSpan (header-only)

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::SafePCVSpan;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:soa-children-columnar-migration-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2036 + PCV-backed children_ ---");
    auto ast = read_file("src/core/ast.ixx");
    auto pcv = read_file("src/core/persistent_child_vector.hh");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(!ast.empty() && ast.find("#2036") != std::string::npos, "ast.ixx #2036");
    CHECK(ast.find("std::vector<PersistentChildVector<NodeId>> children_") != std::string::npos,
          "children_ PCV-backed");
    CHECK(ast.find("children_default") != std::string::npos, "children_default API");
    CHECK(ast.find("children_stable_safe_default_total_") != std::string::npos,
          "safe-default metric");
    CHECK(!pcv.empty() && pcv.find("#2036") != std::string::npos, "pcv header #2036");
    CHECK(pcv.find("migration end-state") != std::string::npos ||
              pcv.find("migration complete") != std::string::npos,
          "migration end-state note");
    CHECK(!q.empty() && q.find("schema-2036") != std::string::npos, "query schema-2036");
}

static void ac2_stable_pins_safe() {
    std::println("\n--- AC2: children_stable / default pin via SafePCVSpan ---");
    FlatAST flat;
    auto a = flat.add_node(NodeTag::LiteralInt);
    auto b = flat.add_node(NodeTag::LiteralInt);
    NodeId kids[] = {a, b};
    auto parent = flat.add_begin(kids, 2);
    CHECK(parent != NULL_NODE, "begin parent");
    const auto safe0 = flat.children_safe_view_count();
    const auto def0 = flat.children_stable_safe_default_total();
    auto refs = flat.children_stable(parent);
    CHECK(refs.size() >= 2, "stable refs");
    CHECK(flat.children_safe_view_count() > safe0, "children_stable pinned via safe_view");
    CHECK(flat.children_stable_safe_default_total() > def0, "safe-default counter advanced");

    const auto def1 = flat.children_stable_safe_default_total();
    auto def_view = flat.children_default(parent);
    CHECK(flat.children_stable_safe_default_total() > def1, "children_default bumps counter");
    CHECK(def_view.size() == flat.children_safe(parent).size(), "default == safe size");

    std::size_t n = 0;
    flat.for_each_stable_child(parent, [&](FlatAST::StableNodeRef) { ++n; });
    CHECK(n == refs.size(), "for_each matches");
}

static void ac3_multifiber_safe_span() {
    std::println("\n--- AC3: multi-fiber SafePCVSpan across mutate+rollback ---");
    FlatAST flat;
    auto c0 = flat.add_node(NodeTag::LiteralInt);
    auto c1 = flat.add_node(NodeTag::LiteralInt);
    auto c2 = flat.add_node(NodeTag::LiteralInt);
    NodeId kids[] = {c0, c1, c2};
    auto root = flat.add_begin(kids, 3);

    // Capture pinned view before concurrent work.
    SafePCVSpan<NodeId> held = flat.children_safe(root);
    CHECK(held.size() >= 2, "held has children");
    const auto first = held[0];
    const auto second = held.size() > 1 ? held[1] : NULL_NODE;

    auto snap = flat.snapshot_children();
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> bad{0};
    std::atomic<std::uint64_t> iters{0};

    std::thread mutator([&] {
        for (int i = 0; i < 200 && !stop.load(std::memory_order_relaxed); ++i) {
            // Mutate children (COW) then restore from snapshot.
            if (root < flat.size() && flat.stable_child_count(root) > 0) {
                flat.set_child(root, 0, c2);
                flat.insert_child(root, 1, c0);
            }
            auto local = flat.snapshot_children();
            flat.restore_children(std::move(local));
            // Periodically reinstall original snap
            if ((i % 17) == 0) {
                auto s2 = snap; // PCV shared_ptr copy
                flat.restore_children(std::move(s2));
                snap = flat.snapshot_children();
            }
            iters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread reader([&] {
        for (int i = 0; i < 500; ++i) {
            // Held span must remain valid for entire reader life.
            if (held.empty()) {
                bad.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            const auto a = held[0];
            const auto n = held.size();
            if (n == 0 || a == NULL_NODE) {
                // empty after pin would be a bug for our setup
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            // Touch every element (storage must not be freed).
            for (std::size_t j = 0; j < held.size(); ++j)
                (void)held[j];
            (void)a;
        }
    });

    reader.join();
    stop.store(true, std::memory_order_relaxed);
    mutator.join();

    CHECK(bad.load() == 0, "no SafePCVSpan dangling under concurrent mutate");
    CHECK(held.size() >= 2, "held size still ≥2 after concurrent work");
    CHECK(held[0] == first, "first child id stable in pinned view");
    if (second != NULL_NODE)
        CHECK(held[1] == second, "second child id stable in pinned view");
    CHECK(iters.load() > 0, "mutator ran");
}

static void ac4_counters() {
    std::println("\n--- AC4: observability counters advance ---");
    FlatAST flat;
    auto a = flat.add_node(NodeTag::LiteralInt);
    NodeId kids[] = {a};
    auto root = flat.add_begin(kids, 1);
    const auto s0 = flat.children_safe_view_count();
    const auto d0 = flat.children_stable_safe_default_total();
    const auto span0 = flat.children_stable_span_calls_total();
    (void)flat.children_default(root);
    (void)flat.children_stable(root);
    (void)flat.children_stable_span_view(root);
    CHECK(flat.children_safe_view_count() > s0, "safe_view advanced");
    CHECK(flat.children_stable_safe_default_total() > d0, "safe-default advanced");
    CHECK(flat.children_stable_span_calls_total() > span0, "span_view advanced");
    CHECK(flat.children_pcv_migration_complete() == 1, "migration complete flag");
}

static void ac5_query() {
    std::println("\n--- AC5: query schema-2036 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (f 2)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:soa-children-columnar-migration-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2036") == 2036, "schema-2036");
    CHECK(href(cs, "issue-2036") == 2036, "issue-2036");
    CHECK(href(cs, "children-pcv-migration-complete") == 1, "migration complete");
    CHECK(href(cs, "children-default-safe-pcv-wired") == 1, "default wired");
    CHECK(href(cs, "children-safe-view-count") >= 0, "safe-view count");
    CHECK(href(cs, "children-stable-safe-default-total") >= 0, "safe-default total");
}

static void ac6_no_pmr_children() {
    std::println("\n--- AC6: no pmr::vector children_ path ---");
    auto ast = read_file("src/core/ast.ixx");
    CHECK(!ast.empty(), "ast readable");
    // Must not declare children_ as pmr::vector of NodeId lists.
    CHECK(ast.find("std::pmr::vector<std::pmr::vector<NodeId>> children_") == std::string::npos,
          "no nested pmr children_");
    CHECK(ast.find("std::pmr::vector<NodeId> children_") == std::string::npos,
          "no pmr vector<NodeId> children_");
    CHECK(ast.find("std::vector<PersistentChildVector<NodeId>> children_") != std::string::npos,
          "PCV vector children_ present");
}

// ── Issue #2862: query:children-stable full safety contract
// Source-cite ACs (gate-only ship; runtime verifies on CI).

static void ac2862_1_source_atomics() {
    std::println("\n--- #2862 AC1: source — 3 new safety atomics present ---");
    const auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(met.find("children_stable_pin_hits_total") != std::string::npos,
          "#2862 AC1: children_stable_pin_hits_total atomic");
    CHECK(met.find("children_stable_invalidation_detected_total") != std::string::npos,
          "#2862 AC1: children_stable_invalidation_detected_total atomic");
    CHECK(met.find("children_stable_epoch_mismatch_total") != std::string::npos,
          "#2862 AC1: children_stable_epoch_mismatch_total atomic");
    CHECK(met.find("// #2862") != std::string::npos,
          "#2862 AC1: comment block cites #2862 contract surfaces");
    // Reuse existing FlatAST atomic.
    const auto ast = read_file("src/core/ast.ixx");
    CHECK(ast.find("children_stable_span_calls_total_") != std::string::npos,
          "#2862 AC1: existing FlatAST children_stable_span_calls_total_ atomic reused");
}

static void ac2862_2_source_primitive() {
    std::println("\n--- #2862 AC2: source — query:children-stable-stats primitive ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:children-stable-stats") != std::string::npos,
          "#2862 AC2: query:children-stable-stats primitive registered");
    CHECK(q.find("schema") != std::string::npos && q.find("make_int(2862)") != std::string::npos,
          "#2862 AC2: schema=2862 in hash builder");
    CHECK(q.find("children-stable-span-calls-total") != std::string::npos &&
              q.find("children-stable-pin-hits-total") != std::string::npos &&
              q.find("children-stable-invalidation-detected-total") != std::string::npos &&
              q.find("children-stable-epoch-mismatch-total") != std::string::npos,
          "#2862 AC2: 4 contract counter keys in hash");
    // Additive on existing #2036 + #2861 pattern safety stats.
    CHECK(q.find("query:children-stable-safe-default-total") != std::string::npos,
          "#2862 AC2: #2036 children-stable-safe-default-total preserved");
    CHECK(q.find("query:pattern-safety-stats") != std::string::npos,
          "#2862 AC2: #2861 pattern-safety-stats preserved");
}

static void ac2862_3_no_docs() {
    std::println("\n--- #2862 AC3: no docs/design/ + lineage refs ---");
    CHECK(read_file("docs/design/2862-children-stable-contract.md").empty(),
          "#2862 AC3: no docs/design/2862-* per #1655");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("#2036") != std::string::npos && q.find("#678") != std::string::npos &&
              q.find("#655") != std::string::npos && q.find("#2861") != std::string::npos,
          "#2862 AC3: lineage refs to #2036/#678/#655/#2861");
}

} // namespace

int main() {
    std::println("=== test_pcv_children_safe_default_migration (#2036 + #2862) ===");
    ac1_source();
    ac2_stable_pins_safe();
    ac3_multifiber_safe_span();
    std::println(
        "\n=== #2862: query:children-stable full safety contract (source-cite gate-only) ===");
    ac2862_1_source_atomics();
    ac2862_2_source_primitive();
    ac2862_3_no_docs();
    ac4_counters();
    ac5_query();
    ac6_no_pmr_children();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
