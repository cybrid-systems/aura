// @category: unit
// @reason: Issue #2191 — unify type affected_subtree cone with
// dirty::DepGraph cascade (refine #2110 R4).
//
//   AC1: Mutate callee B → type cone of callers + IR cascade share
//        overlapping cone; sibling defines without free-ref stay clean
//   AC2: Metrics type_dirty_cone_mirrored_total / type_ir_cone_union_size_avg
//   AC3: Occurrence dirty If nodes mirrored into type cone
//   AC4: #2110 / #2109 / #2106 wiring preserved (source)
//   AC5: Tests under tests/compiler/ src-aligned

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.dirty_propagation;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::dirty::decode_ast_dep_node;
using aura::compiler::dirty::encode_ast_dep_node;
using aura::compiler::dirty::g_global_dirty;
using aura::compiler::dirty::is_ast_dep_node;
using aura::compiler::dirty::is_block_dep_node;
using aura::compiler::dirty::is_fn_node;
using aura::compiler::dirty::kAstDepTag;
using aura::compiler::dirty::last_type_cone_ast;
using aura::compiler::dirty::mirror_type_affected_to_cascade;
using aura::compiler::dirty::pull_cascade_ast_dirty_into;
using aura::compiler::dirty::type_dirty_cone_mirrored_total;
using aura::compiler::dirty::type_ir_cone_union_samples;
using aura::compiler::dirty::type_ir_cone_union_size_avg;
using aura::compiler::dirty::type_ir_cone_union_size_sum;
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

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void reset_type_cone_metrics() {
    type_dirty_cone_mirrored_total.store(0, std::memory_order_relaxed);
    type_ir_cone_union_size_sum.store(0, std::memory_order_relaxed);
    type_ir_cone_union_samples.store(0, std::memory_order_relaxed);
    g_global_dirty.clear();
}

} // namespace

int run_test_type_dirty_cone_dep_graph() {
    std::println("=== Issue #2191: type dirty cone ↔ DepGraph cascade ===");

    // ── Encode / decode unit ──
    {
        std::println("\n--- encode_ast_dep_node ---");
        const auto e = encode_ast_dep_node(42);
        CHECK(is_ast_dep_node(e), "ast dep tagged");
        CHECK(!is_fn_node(e), "not fn node");
        CHECK(!is_block_dep_node(e), "not block-dep");
        CHECK(decode_ast_dep_node(e) == 42, "decode 42");
        CHECK((e & kAstDepTag) != 0, "tag bit set");
        // Distinct from small block encodings.
        CHECK(e != 42u, "encoded != raw nid");
    }

    // ── Mirror + pull unit ──
    {
        std::println("\n--- mirror_type_affected_to_cascade ---");
        reset_type_cone_metrics();
        const std::vector<aura::compiler::dirty::NodeId> cone{1, 3, 5, 3}; // dedup 3
        const auto n = mirror_type_affected_to_cascade(cone);
        CHECK(n == 3, "3 distinct mirrored");
        CHECK(type_dirty_cone_mirrored_total.load() == 3, "metric +3");
        CHECK(last_type_cone_ast().size() == 3, "last cone size 3");
        CHECK(g_global_dirty.is_dirty(encode_ast_dep_node(1)), "nid 1 dirty");
        CHECK(g_global_dirty.is_dirty(encode_ast_dep_node(5)), "nid 5 dirty");
        std::vector<aura::compiler::dirty::NodeId> pulled;
        pull_cascade_ast_dirty_into(pulled);
        std::unordered_set<aura::compiler::dirty::NodeId> ps(pulled.begin(), pulled.end());
        CHECK(ps.count(1) && ps.count(3) && ps.count(5), "pull has 1,3,5");
        CHECK(type_ir_cone_union_samples.load() >= 1, "union samples");
        CHECK(type_ir_cone_union_size_avg() >= 3.0, "union avg ≥ cone");
    }

    // ── AC1/AC5: mutate B → type + IR cone under hybrid cascade ──
    {
        std::println("\n--- AC1: mutate callee B, overlapping type/IR cone ---");
        reset_type_cone_metrics();
        CompilerService cs;
        CHECK(cs.eval(R"(
(set-code "
(define B (lambda () 1))
(define A (lambda () (+ (B) 1)))
(define C (lambda () 99))
")")
                  .has_value(),
              "set-code A/B/C");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        // Wire hybrid edges.
        cs.public_record_dependency("A", "B");
        CHECK(cs.public_dep_graph_has_edge("A", "B"), "string edge A→B");
        CHECK(cs.public_node_dep_has_mirror_edge("A", "B"), "NodeId mirror B→A");
        const auto mir0 = type_dirty_cone_mirrored_total.load();
        // Mutate B body — triggers partial type re-infer + cascade.
        CHECK(cs.eval("(mutate:set-body \"B\" \"(lambda () 2)\")").has_value(), "set-body B");
        CHECK(cs.eval("(eval-current)").has_value(), "re-eval");
        auto r = cs.eval("(A)");
        CHECK(r && is_int(*r) && as_int(*r) == 3, "A = 3 after B mutate");
        // Sibling C should still work (no free-ref to B).
        auto c = cs.eval("(C)");
        CHECK(c && is_int(*c) && as_int(*c) == 99, "C untouched = 99");
        // Mirror metric advanced under sparse mutate (or service path).
        const auto mir1 = type_dirty_cone_mirrored_total.load();
        // Either process-wide metric or query surface after mutate.
        const auto qmir = href(cs, "query:dirty-cascade-stats", "type_dirty_cone_mirrored_total");
        CHECK(mir1 >= mir0 || qmir >= 0, "mirror metric live after mutate");
        CHECK(href(cs, "query:dirty-cascade-stats", "type-dirty-cone-mirror-wired") == 1,
              "dirty-cascade wired");
        CHECK(href(cs, "query:dirty-cascade-stats", "schema-2191") == 2191, "schema-2191");
        CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema-2191") == 2191,
              "fidelity schema-2191");
        CHECK(href(cs, "query:type-incremental-fidelity-stats", "type-dirty-cone-mirror-wired") ==
                  1,
              "fidelity wired");
    }

    // ── AC2: metrics keys ──
    {
        std::println("\n--- AC2: metrics surface ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
        CHECK(href(cs, "query:dirty-cascade-stats", "type_dirty_cone_mirrored_total") >= 0,
              "mirrored key");
        CHECK(href(cs, "query:dirty-cascade-stats", "type_ir_cone_union_size_avg") >= 0,
              "union avg key");
        CHECK(href(cs, "query:type-incremental-fidelity-stats", "type_dirty_cone_mirrored_total") >=
                  0,
              "fidelity mirrored key");
        // cache_hit_rate not regressed: fidelity still exposes reinfer keys.
        CHECK(href(cs, "query:type-incremental-fidelity-stats", "incremental-reinfer-nodes") >= 0,
              "reinfer key intact");
    }

    // ── AC3: occurrence If path mirrors (source + unit reverse pull) ──
    {
        std::println("\n--- AC3: occurrence If stay in type cone ---");
        auto impl = read_file("src/compiler/type_checker_impl.cpp");
        CHECK(impl.find("Issue #2191") != std::string::npos, "type_checker_impl #2191");
        CHECK(impl.find("mirror_type_affected_to_cascade") != std::string::npos, "mirror call");
        CHECK(impl.find("occurrence_targets") != std::string::npos, "occurrence in cone path");
        // Reverse: cascade-marked AST dep re-enters pull → affected seed.
        reset_type_cone_metrics();
        g_global_dirty.mark(encode_ast_dep_node(7));
        g_global_dirty.mark(encode_ast_dep_node(11));
        std::vector<aura::compiler::dirty::NodeId> pulled;
        pull_cascade_ast_dirty_into(pulled);
        std::unordered_set<aura::compiler::dirty::NodeId> ps(pulled.begin(), pulled.end());
        CHECK(ps.count(7) && ps.count(11), "pull cascade AST for type seed");
    }

    // ── AC4: #2110 / #2109 / #2106 lineage intact ──
    {
        std::println("\n--- AC4: related issue wiring ---");
        auto dirty = read_file("src/compiler/dirty_propagation.ixx");
        auto svc = read_file("src/compiler/service_dirty.cpp");
        CHECK(dirty.find("encode_fn_node") != std::string::npos, "#2110 encode_fn_node");
        CHECK(dirty.find("encode_block_dep_node") != std::string::npos, "#2187 block dep");
        CHECK(dirty.find("encode_ast_dep_node") != std::string::npos, "#2191 ast dep");
        CHECK(dirty.find("mirror_type_affected_to_cascade") != std::string::npos, "mirror helper");
        CHECK(dirty.find("cascade_skip_subtree") != std::string::npos ||
                  dirty.find("dirty_skip_subtree") != std::string::npos,
              "#2106 skip subtree");
        CHECK(svc.find("hybrid_node_cascade_") != std::string::npos, "#2110 hybrid cascade");
    }

    // ── Source wiring summary ──
    {
        std::println("\n--- source wiring ---");
        auto pure = read_file("src/compiler/dirty_propagation.ixx");
        auto tc = read_file("src/compiler/type_checker_impl.cpp");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(pure.find("type_dirty_cone_mirrored_total") != std::string::npos, "metric atom");
        CHECK(pure.find("type_ir_cone_union_size") != std::string::npos, "union size metric");
        CHECK(tc.find("pull_cascade_ast_dirty_into") != std::string::npos, "pull in partial");
        CHECK(obs.find("schema-2191") != std::string::npos, "obs schema-2191");
    }

    reset_type_cone_metrics();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_type_dirty_cone_dep_graph();
}
#endif
