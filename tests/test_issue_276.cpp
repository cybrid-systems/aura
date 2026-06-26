// @category: integration
// @reason: WorkspaceTree cross-layer StableNodeRef resolution


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.parser.parser;

namespace aura_issue_276_detail {

bool test_remap_table_identity() {
    std::println("\n--- AC1: NodeIdRemapTable identity remap ---");
    aura::ast::mutation::NodeIdRemapTable table;
    table.reset_identity(0, 1, 128);
    CHECK(table.resolve_from_parent(7) == 7, "identity from-parent");
    CHECK(table.resolve_to_parent(7) == 7, "identity to-parent");
    table.record_parent_local(7, 9);
    CHECK(table.resolve_from_parent(7) == 9, "explicit parent->local remap");
    return true;
}

bool test_workspace_cow_initializes_remap() {
    std::println("\n--- AC2: COW clone initializes per-layer remap ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    for (int i = 0; i < 20; ++i)
        (void)flat.add_literal(i);

    aura::compiler::WorkspaceTree tree;
    aura::compiler::WorkspaceNode root;
    root.is_root = true;
    root.has_own_flat = true;
    root.flat = &flat;
    root.pool = &pool;
    tree.nodes_.push_back(std::move(root));

    auto child = tree.create_child("exp", 0, &flat, &pool);
    CHECK(tree.ensure_local_flat(child), "lazy COW succeeds");
    const auto& node = tree.nodes_[child];
    CHECK(node.has_own_flat, "child owns flat after COW");
    CHECK(node.remap.parent_layer() == 0, "remap parent layer is 0");
    CHECK(node.remap.cow_epoch() == node.cow_epoch, "remap epoch synced");
    CHECK(node.remap.node_count() == node.flat->size(), "remap sized to flat");
    return true;
}

bool test_cross_layer_resolve_validity_rate() {
    std::println("\n--- AC3: cross-layer resolve validity >= 99% ---");
    std::ostringstream src;
    src << "(begin";
    constexpr int kTotal = 100;
    for (int i = 0; i < kTotal; ++i)
        src << " (define v" << i << " " << i << ")";
    src << ")";

    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto pr = aura::parser::parse_to_flat(src.str(), flat, pool);
    CHECK(pr.success, "parse 100-define workspace");
    if (!pr.success)
        return false;
    flat.root = pr.root;

    aura::compiler::WorkspaceTree tree;
    aura::compiler::WorkspaceNode root;
    root.is_root = true;
    root.has_own_flat = true;
    root.flat = &flat;
    root.pool = &pool;
    tree.nodes_.push_back(std::move(root));

    std::vector<aura::ast::FlatAST::StableNodeRef> captured;
    captured.reserve(kTotal);
    for (aura::ast::NodeId id = 1; id < flat.size(); ++id) {
        if (flat.get(id).tag == aura::ast::NodeTag::Define)
            captured.push_back(flat.make_ref(id));
    }
    CHECK(captured.size() >= static_cast<std::size_t>(kTotal), "captured at least 100 define refs");

    auto child = tree.create_child("mut-child", 0, &flat, &pool);
    CHECK(tree.ensure_local_flat(child), "child COW for mutate");
    auto* child_flat = tree.nodes_[child].flat;
    CHECK(child_flat != nullptr, "child flat allocated");

    // Simulate a single structural mutation generation bump in the child layer.
    child_flat->bump_generation();

    std::size_t resolved = 0;
    for (const auto& ref : captured) {
        if (tree.resolve_stable_ref(0, ref, child).has_value())
            ++resolved;
    }

    const auto pct = captured.empty() ? 0.0
                                      : (100.0 * static_cast<double>(resolved) /
                                         static_cast<double>(captured.size()));
    std::println("  resolved {}/{} ({:.1f}%)", resolved, captured.size(), pct);
    CHECK(pct >= 99.0, "cross-layer StableNodeRef validity >= 99%");
    return pct >= 99.0;
}

static bool run_bool(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    return r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r);
}

bool test_workspace_resolve_primitive() {
    std::println("\n--- AC4: workspace:resolve-stable-ref primitive ---");
    aura::compiler::CompilerService cs;
    CHECK(run_bool(cs, "(begin (set-code \"(define x 1) (define y 2)\") "
                       "(workspace:create \"child\") "
                       "(workspace:switch 1) "
                       "(mutate:rebind \"x\" \"(quote 9)\" \"mut\") "
                       "(workspace:switch 0))"),
          "setup parent/child with mutation");

    auto r = cs.eval(
        "(begin "
        "  (define rx (ast:stable-ref 1)) "
        "  (workspace:switch 1) "
        "  (define resolved (workspace:resolve-stable-ref 0 (car rx) (cdr rx))) "
        "  (if resolved 1 0))");
    CHECK(r.has_value(), "resolve primitive eval succeeded");
    if (!r || !aura::compiler::types::is_int(*r)) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::as_int(*r) == 1,
          "workspace:resolve-stable-ref returns resolved pair for sibling define");
    return true;
}

int run_tests() {
    std::println("Issue #276 — cross-layer NodeId remap / StableNodeRef\n");
    bool ok = true;
    ok = aura_issue_276_detail::test_remap_table_identity() && ok;
    ok = aura_issue_276_detail::test_workspace_cow_initializes_remap() && ok;
    ok = aura_issue_276_detail::test_cross_layer_resolve_validity_rate() && ok;
    ok = aura_issue_276_detail::test_workspace_resolve_primitive() && ok;
    std::println("\n{} passed, {} failed", g_passed, g_failed);
    return ok ? 0 : 1;
}

} // namespace aura_issue_276_detail

int aura_issue_276_run() { return aura_issue_276_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_276_run(); }
#endif