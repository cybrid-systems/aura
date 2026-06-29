// @category: integration
// @reason: pure C++ — FlatAST structural mutation + mutation_log
// test_issue_314.cpp — Verify Issue #314 acceptance criteria
// ("feat(mutate): route new SV nodes through mutation_log_ +
//  mark_dirty_upward + generation bump").
//
// Scope-limited close. The issue body asks for Interface /
// Modport structural mutations (set_child, insert_child) to
// automatically route through the safety path:
//   1. mutation_log_ record (re-use MutationRecord)
//   2. mark_dirty_upward + generation bump
//   3. (optional) mark_dirty_verification_upward
//
// After inspection, the existing path is already tag-
// agnostic: `set_child_locked` + `insert_child_locked` +
// `remove_child_locked` all funnel through
// `add_mutation_child_op` → `add_mutation_with_rollback`,
// which:
//   - records to `mutation_log_`
//   - calls `mark_dirty_upward(node)` for the parent
//   - the public wrappers `set_child` / `insert_child` /
//     `remove_child` instantiate a `StructuralMutationGuard`
//     whose dtor bumps `generation_`
//   - sets `has_rollback_data = true` so the rollback path
//     is set up
//
// The new NodeTag values from #310 (Interface = 0x1B,
// Modport = 0x1C) are picked up automatically — the
// infrastructure is generic over NodeTag. This test is the
// confirmation.
//
// On mark_dirty_verification_upward specifically:
//   The #313 helper is for verification-feedback events
//   (coverage holes, assert failures). It's NOT meant to
//   fire on every structural mutation — that would
//   conflate "I changed the AST" with "I observed a
//   verification failure". Callers that need it should
//   invoke it explicitly after the mutation. We document
//   this in the close comment.
//
// ACs covered:
//   AC1 mutation_log_ 有完整记录
//        (after set_child on an Interface child, the log
//         has a "structural-set-child" record with the
//         right (parent, child_idx, old/new child ids))
//   AC2 dirty 正确向上传播
//        (after the mutation, parent + ancestors are
//         marked dirty via mark_dirty_upward)
//   AC3 rollback 机制可用
//        (has_rollback_data is true on the record + the
//         log entry's old/new_value carry the child
//         NodeIds for rollback reconstruction)
//   AC4 generation_ 正确 bump
//        (the StructuralMutationGuard bumps generation_;
//         captured before + after)


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;

namespace aura_issue_314_detail {
#define CHECK_EQ_LOCAL(a, b, msg) do { \
    auto _a = (a); auto _b = (b); \
    if (!(_a == _b)) { \
        std::println("  FAIL: {} (got {} expected {} line {})", msg, _a, _b, __LINE__); \
        ++g_failed; \
    } else { \
        std::println("  PASS: {}", msg); \
        ++g_passed; \
    } \
} while (0)

// ═══════════════════════════════════════════════════════════════
// AC1: Interface structural mutation → mutation_log_ record
// ═══════════════════════════════════════════════════════════════

bool test_interface_set_child_logs_to_mutation_log() {
    std::println("\n--- AC1: Interface set_child logs to mutation_log_ ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    // Build a 2-level tree: root → interface → [signal1, signal2]
    auto sig1 = flat.add_variable(pool.intern("data"));
    auto sig2 = flat.add_variable(pool.intern("valid"));
    std::vector<NodeId> initial_body{sig1};
    auto iface = flat.add_interface(pool.intern("Bus"),
                                    std::span<const NodeId>(initial_body));
    auto root = flat.add_begin({iface});
    flat.root = root;
    // Pre-condition: mutation log is empty.
    CHECK_EQ_LOCAL(flat.mutation_count(), std::size_t{0},
                   "mutation_log_ starts empty (pre-mutation)");
    CHECK_EQ_LOCAL(flat.generation(), std::uint16_t{1},
                   "generation_ starts at 1 (pre-mutation)");
    // Mutate: replace signal1 (idx 0) with signal2.
    flat.set_child(iface, 0, sig2);
    // AC1: mutation log has 1 record.
    CHECK_EQ_LOCAL(flat.mutation_count(), std::size_t{1},
                   "Interface set_child creates 1 mutation log entry");
    // The record has the right shape.
    auto records = flat.all_mutations();
    CHECK_EQ_LOCAL(records[0].target_node, iface,
                   "mutation target == interface id");
    CHECK_EQ_LOCAL(records[0].field_offset, std::uint32_t{0},
                   "mutation child_idx == 0");
    CHECK_EQ_LOCAL(records[0].old_value, static_cast<std::uint64_t>(sig1),
                   "mutation old_value == sig1");
    CHECK_EQ_LOCAL(records[0].new_value, static_cast<std::uint64_t>(sig2),
                   "mutation new_value == sig2");
    CHECK(records[0].has_rollback_data,
          "mutation record has rollback data set (AC3)");
    CHECK(records[0].operator_name.find("structural-set-child")
              != std::string::npos,
          "mutation op_name is 'structural-set-child'");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC1 (cont): Modport structural mutation → mutation_log_ record
// ═══════════════════════════════════════════════════════════════

bool test_modport_insert_child_logs_to_mutation_log() {
    std::println("\n--- AC1 (cont): Modport insert_child logs to mutation_log_ ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    // Build an Interface containing a nested Modport with 0 ports.
    // (Modports are leaves per the kNodeMeta definition, so
    //  set_child at the modport level is the canonical
    //  structural mutation; but for the test we'll mutate
    //  the Interface body's modport slot.)
    auto port1 = pool.intern("data");
    std::vector<SymId> mp_ports{port1};
    auto mp = flat.add_modport(pool.intern("master"),
                               std::span<const SymId>(mp_ports));
    std::vector<NodeId> initial_body{mp};
    auto iface = flat.add_interface(pool.intern("Bus"),
                                    std::span<const NodeId>(initial_body));
    flat.root = flat.add_begin({iface});
    CHECK_EQ_LOCAL(flat.mutation_count(), std::size_t{0},
                   "pre-mutation log empty for Interface→Modport scenario");
    // Insert a new Variable as a sibling of the modport.
    auto new_sig = flat.add_variable(pool.intern("valid"));
    flat.insert_child(iface, 1, new_sig);
    CHECK_EQ_LOCAL(flat.mutation_count(), std::size_t{1},
                   "Interface insert_child (modport context) logs mutation");
    auto records = flat.all_mutations();
    CHECK_EQ_LOCAL(records[0].field_offset, std::uint32_t{1},
                   "insert mutation child_idx == 1 (append position)");
    CHECK_EQ_LOCAL(records[0].new_value, static_cast<std::uint64_t>(new_sig),
                   "insert mutation new_value == new_sig");
    CHECK(records[0].has_rollback_data,
          "insert mutation has rollback data");
    CHECK(records[0].operator_name.find("structural-insert-child")
              != std::string::npos,
          "insert mutation op_name is 'structural-insert-child'");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2: dirty propagation upward on Interface mutation
// ═══════════════════════════════════════════════════════════════

bool test_interface_mutation_marks_dirty_upward() {
    std::println("\n--- AC2: Interface mutation marks parent + ancestors dirty ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto sig = flat.add_variable(pool.intern("data"));
    std::vector<NodeId> body{sig};
    auto iface = flat.add_interface(pool.intern("Bus"),
                                    std::span<const NodeId>(body));
    auto root = flat.add_begin({iface});
    flat.root = root;
    // Pre-mutation: nothing is dirty.
    CHECK_EQ_LOCAL(flat.is_dirty(iface), false,
                   "interface starts NOT dirty");
    CHECK_EQ_LOCAL(flat.is_dirty(root), false,
                   "root starts NOT dirty");
    // Mutate via remove_child (replaces target child with NULL).
    flat.remove_child(iface, 0);
    // AC2: parent (interface) + root are now dirty via
    // mark_dirty_upward propagation.
    CHECK_EQ_LOCAL(flat.is_dirty(iface), true,
                   "interface is dirty after mutation (mark_dirty_upward)");
    CHECK_EQ_LOCAL(flat.is_dirty(root), true,
                   "root is dirty after mutation (mark_dirty_upward BFS)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3 + AC4: rollback data available + generation bumped
// ═══════════════════════════════════════════════════════════════

bool test_mutation_rollback_and_generation() {
    std::println("\n--- AC3+AC4: rollback + generation bump on Interface mutation ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto sig = flat.add_variable(pool.intern("data"));
    std::vector<NodeId> body{sig};
    auto iface = flat.add_interface(pool.intern("Bus"),
                                    std::span<const NodeId>(body));
    flat.root = flat.add_begin({iface});
    const auto gen_before = flat.generation();
    const auto count_before = flat.mutation_count();
    // Mutate.
    flat.set_child(iface, 0, flat.add_variable(pool.intern("valid")));
    // AC4: generation bumped by StructuralMutationGuard.
    CHECK(flat.generation() > gen_before,
          "generation_ bumped after mutation (StructuralMutationGuard)");
    CHECK_EQ_LOCAL(flat.mutation_count() - count_before, std::size_t{1},
                   "mutation count incremented by 1");
    // AC3: rollback data is set + old/new_value preserved.
    auto records = flat.all_mutations();
    auto& rec = records.back();
    CHECK(rec.has_rollback_data,
          "rollback data set (AC3 — caller can restore via snapshot)");
    CHECK(rec.old_value != rec.new_value,
          "old_value != new_value (the roll-forward is non-trivial)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC (bonus): mark_dirty_verification_upward is NOT triggered
// (verification is a separate concern from structural mutation)
// ═══════════════════════════════════════════════════════════════

bool test_verification_dirty_not_auto_triggered() {
    std::println("\n--- AC (bonus): verification-dirty is NOT auto-triggered ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto sig = flat.add_variable(pool.intern("data"));
    std::vector<NodeId> body{sig};
    auto iface = flat.add_interface(pool.intern("Bus"),
                                    std::span<const NodeId>(body));
    flat.root = flat.add_begin({iface});
    // Pure structural mutation — verification channel should
    // stay clean. (Per the close comment, callers that need
    // verification-dirty should call mark_dirty_verification_
    // upward explicitly; mixing the two channels in auto-
    // routing would conflate concerns.)
    flat.set_child(iface, 0, flat.add_variable(pool.intern("valid")));
    CHECK_EQ_LOCAL(flat.is_verification_dirty(iface), false,
                   "verification-dirty NOT auto-triggered by structural mutate");
    return true;
}

int run_tests() {
    std::println("═══ Issue #314 (mutate routing for SV nodes) ═══\n");
    test_interface_set_child_logs_to_mutation_log();
    test_modport_insert_child_logs_to_mutation_log();
    test_interface_mutation_marks_dirty_upward();
    test_mutation_rollback_and_generation();
    test_verification_dirty_not_auto_triggered();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_314_detail

int aura_issue_314_run() { return aura_issue_314_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_314_run(); }
#endif