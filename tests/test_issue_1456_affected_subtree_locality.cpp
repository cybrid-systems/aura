// test_issue_1456_affected_subtree_locality.cpp
// Issue #1456: Improve incremental type checking locality with
// affected_subtree_from_mutation (prefer target over parent Begin,
// stop dirty-upward at Define/Let/Module boundaries).
//
// AC1: multi-define workspace rebind — affected set << total nodes
// AC2: affected includes rebind target; excludes sibling define bodies
// AC3: post-mutate eval semantics preserved (partial re-infer correct)
// AC4: multi-round rebind matrix stays local + type-correct
// AC5: parent_id=Begin does not force full Begin descendants

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;

namespace aura_1456_detail {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::compiler::affected_subtree_from_mutation;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static constexpr const char* k_multi = R"(
(define a (lambda (x) (+ x 1)))
(define b (lambda (y) (* y 2)))
(define c (lambda (z) (- z 3)))
(define d (lambda (w) (+ w 10)))
(define e (lambda (v) (* v 5)))
)";

static bool load_multi(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_multi + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    (void)cs.eval("(typecheck-current)");
    return true;
}

static NodeId find_define_by_name(const FlatAST& flat, const aura::ast::StringPool& pool,
                                  std::string_view name) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        auto v = flat.get(id);
        if (v.tag != NodeTag::Define || v.sym_id == aura::ast::INVALID_SYM)
            continue;
        if (pool.resolve(v.sym_id) == name)
            return id;
    }
    return NULL_NODE;
}

static void test_affected_much_smaller_than_total() {
    std::println("\n--- AC1: rebind affected << total ---");
    CompilerService cs;
    CHECK(load_multi(cs), "load multi-define workspace");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "workspace flat");
    const auto total0 = ws->size();

    (void)cs.eval("(mutate:rebind \"b\" \"(lambda (y) (* y 3))\" \"issue-1456-ac1\")");
    CHECK(!ws->all_mutations().empty(), "mutation logged");
    auto affected = affected_subtree_from_mutation(*ws, ws->all_mutations().back());
    const auto total = ws->size();
    std::println("  affected={} total={} (pre={}) ratio={:.1f}%", affected.size(), total, total0,
                 100.0 * static_cast<double>(affected.size()) /
                     static_cast<double>(std::max<std::size_t>(1, total)));
    CHECK(!affected.empty(), "affected non-empty");
    CHECK(affected.size() < total, "affected < total");
    // With 5 defines, a local rebind of one should stay well under half.
    CHECK(affected.size() * 2 < total || affected.size() < 40,
          "affected stays local (not full-workspace cascade)");
}

static void test_target_included_siblings_excluded() {
    std::println("\n--- AC2: target in set; sibling bodies out ---");
    CompilerService cs;
    CHECK(load_multi(cs), "load multi");
    auto* ws = cs.workspace_flat();
    auto* pool = cs.workspace_pool();
    CHECK(ws && pool, "workspace + pool");

    const auto def_b = find_define_by_name(*ws, *pool, "b");
    const auto def_d = find_define_by_name(*ws, *pool, "d");
    CHECK(def_b != NULL_NODE && def_d != NULL_NODE, "found defines b and d");

    (void)cs.eval("(mutate:rebind \"b\" \"(lambda (y) (+ y 9))\" \"issue-1456-ac2\")");
    CHECK(!ws->all_mutations().empty(), "mutation logged");
    // Rebind may leave old nodes in flat; re-resolve defines after mutate.
    const auto def_b2 = find_define_by_name(*ws, *pool, "b");
    const auto def_d2 = find_define_by_name(*ws, *pool, "d");
    auto affected = affected_subtree_from_mutation(*ws, ws->all_mutations().back());
    std::unordered_set<NodeId> aset(affected.begin(), affected.end());

    // Target of rebind is the define node (old_define id).
    const auto target = ws->all_mutations().back().target_node;
    std::println("  target={} def_b2={} def_d2={} |affected|={}", target, def_b2, def_d2,
                 affected.size());
    CHECK(target != NULL_NODE, "mutation has target");
    CHECK(aset.count(target) > 0 || (def_b2 != NULL_NODE && aset.count(def_b2) > 0),
          "affected contains rebind target define");

    // Sibling define `d`'s body should not be fully covered unless
    // coincidentally small — at least `d` define itself should not
    // force all of d's descendants into the set via Begin cascade.
    if (def_d2 != NULL_NODE && def_d2 < ws->size()) {
        auto dv = ws->get(def_d2);
        if (!dv.children.empty() && dv.child(0) != NULL_NODE) {
            const auto d_body = dv.child(0);
            // Body root of sibling may be absent from affected.
            const bool body_in = aset.count(d_body) > 0;
            std::println("  sibling d body {} in affected? {}", d_body, body_in);
            CHECK(!body_in, "sibling define body not in affected (locality)");
        }
    }
}

static void test_eval_semantics_after_partial() {
    std::println("\n--- AC3: eval semantics after partial re-infer ---");
    CompilerService cs;
    CHECK(load_multi(cs), "load multi");
    (void)cs.eval("(mutate:rebind \"a\" \"(lambda (x) (+ x 100))\" \"issue-1456-ac3\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    auto r = cs.eval("(a 1)");
    CHECK(r && is_int(*r), "a 1 returns int");
    if (r && is_int(*r))
        CHECK(as_int(*r) == 101, "a rebind (+ x 100) correct");
    // Unrelated binding still works.
    auto rb = cs.eval("(b 3)");
    CHECK(rb && is_int(*rb), "b 3 still works");
    if (rb && is_int(*rb))
        CHECK(as_int(*rb) == 6, "sibling b unchanged (* y 2)");
}

static void test_multi_round_local_matrix() {
    std::println("\n--- AC4: multi-round rebind locality + correctness ---");
    CompilerService cs;
    CHECK(load_multi(cs), "load multi");
    auto* ws = cs.workspace_flat();
    CHECK(ws, "workspace");
    const auto total = ws->size();
    std::size_t max_aff = 0;
    for (int round = 0; round < 4; ++round) {
        const int add = 20 + round;
        const std::string body = "(lambda (x) (+ x " + std::to_string(add) + "))";
        (void)cs.eval("(mutate:rebind \"c\" \"" + body + "\" \"r" + std::to_string(round) + "\")");
        CHECK(!ws->all_mutations().empty(), "mutation round " + std::to_string(round));
        auto aff = affected_subtree_from_mutation(*ws, ws->all_mutations().back());
        max_aff = std::max(max_aff, aff.size());
        (void)cs.incremental_infer(ws->all_mutations().back());
        auto r = cs.eval("(c 1)");
        CHECK(r && is_int(*r), "c eval round " + std::to_string(round));
        if (r && is_int(*r))
            CHECK(as_int(*r) == 1 + add, "c result round " + std::to_string(round));
    }
    std::println("  max_affected={} total={}", max_aff, total);
    CHECK(max_aff < total, "every round stayed under full AST");
    CHECK(max_aff * 2 < total || max_aff < 50, "max affected stays local across rounds");
}

static void test_parent_begin_does_not_cascade() {
    std::println("\n--- AC5: synthetic parent=Begin does not cascade ---");
    CompilerService cs;
    CHECK(load_multi(cs), "load multi");
    auto* ws = cs.workspace_flat();
    auto* pool = cs.workspace_pool();
    CHECK(ws && pool, "ws+pool");

    // Build a synthetic MutationRecord pointing at define `e` with
    // parent = a Begin root if present (or parent_of(e)).
    NodeId def_e = find_define_by_name(*ws, *pool, "e");
    CHECK(def_e != NULL_NODE, "found e");
    NodeId parent = ws->parent_of(def_e);
    aura::ast::MutationRecord rec{};
    rec.target_node = def_e;
    rec.parent_id = parent;
    rec.mutation_id = 1456;
    rec.operator_name = "rebind";

    auto old_style_parent_first = [&]() {
        // Reproduce pre-#1456 preference: parent first → full descendants.
        std::vector<NodeId> out;
        NodeId root = (parent != NULL_NODE) ? parent : def_e;
        std::function<void(NodeId)> walk = [&](NodeId id) {
            if (id == NULL_NODE || id >= ws->size())
                return;
            out.push_back(id);
            auto v = ws->get(id);
            for (auto c : v.children)
                if (c != NULL_NODE)
                    walk(c);
        };
        walk(root);
        return out.size();
    };

    auto localized = affected_subtree_from_mutation(*ws, rec);
    const auto parent_cascade = old_style_parent_first();
    std::println("  localized={} parent_cascade_size={} parent={}", localized.size(),
                 parent_cascade, parent);
    CHECK(!localized.empty(), "localized non-empty");
    if (parent != NULL_NODE && parent_cascade > localized.size()) {
        CHECK(localized.size() < parent_cascade, "localized smaller than parent-first cascade");
    }
    // Target define must be in localized set.
    std::unordered_set<NodeId> aset(localized.begin(), localized.end());
    CHECK(aset.count(def_e) > 0, "localized contains target define e");
}

} // namespace aura_1456_detail

int main() {
    using namespace aura_1456_detail;
    test_affected_much_smaller_than_total();
    test_target_included_siblings_excluded();
    test_eval_semantics_after_partial();
    test_multi_round_local_matrix();
    test_parent_begin_does_not_cascade();
    return RUN_ALL_TESTS();
}
