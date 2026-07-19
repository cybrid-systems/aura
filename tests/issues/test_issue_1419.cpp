// @category: unit
// @reason: pure C++ FlatAST MutationRecord provenance; no CompilerService
//
// test_issue_1419.cpp — Issue #1419: Compound provenance wire-up
// on MutationRecord (author_fingerprint + parent_mutation_id +
// composite_transaction_id).
//
// Background: #1412 added the 3 fields with defaults=0. This issue
// wires them at add_mutation sites via FlatAST provenance context,
// TypedTransactionGuard (composite + parent chain), Evaluator
// agent fingerprint, query:mutation-provenance, and
// MutationLogEntry surface.
//
// ACs:
//   AC1: FlatAST context stamps author/parent/composite on add_mutation
//   AC2: unset context → provenance stays 0 (backward compat)
//   AC3: shared composite_transaction_id across multiple mutations
//   AC4: parent chain — first root (parent=0), subsequent link to first
//   AC5: create_mutation_record / create_subtree stamp params
//   AC6: MutationLogEntry fields exist on C++ surface (struct layout)

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.core.mutation;

namespace test_issue_1419_detail {

// ── AC1: context stamps into add_mutation ───────────────────────

bool test_ac1_context_stamps_on_add_mutation() {
    std::println("\n--- AC1: FlatAST provenance context stamps add_mutation ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    auto x_sym = pool->intern("x");
    auto lit = flat->add_literal(42);
    auto x_var = flat->add_variable(x_sym);
    auto root = flat->add_let(x_sym, lit, x_var);
    flat->root = root;

    flat->set_mutation_author_fingerprint(0xA11CE7F00DULL);
    flat->set_mutation_parent_mutation_id(7);
    flat->set_mutation_composite_transaction_id(99);

    auto mid = flat->add_mutation(x_var, "mutate:rebind", "Int", "Number", "test");
    CHECK(mid != 0, "AC1.setup: add_mutation returns non-zero id");

    const auto hist = flat->mutation_history(x_var);
    CHECK(hist.size() == 1, "AC1: 1 record");
    if (hist.size() >= 1) {
        CHECK(hist[0].author_fingerprint == 0xA11CE7F00DULL,
              "AC1: author_fingerprint stamped from context");
        CHECK(hist[0].parent_mutation_id == 7, "AC1: parent_mutation_id stamped from context");
        CHECK(hist[0].composite_transaction_id == 99,
              "AC1: composite_transaction_id stamped from context");
    }
    return true;
}

// ── AC2: default 0 when context unset ───────────────────────────

bool test_ac2_defaults_zero() {
    std::println("\n--- AC2: unset context → provenance = 0 ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    auto lit = flat->add_literal(1);
    auto mid = flat->add_mutation(lit, "mutate:replace-type", "a", "b", "c");
    CHECK(mid != 0, "AC2.setup: mutation id");

    const auto& log = flat->all_mutations();
    CHECK(!log.empty(), "AC2: log non-empty");
    if (!log.empty()) {
        CHECK(log.back().author_fingerprint == 0, "AC2: author default 0 (system)");
        CHECK(log.back().parent_mutation_id == 0, "AC2: parent default 0");
        CHECK(log.back().composite_transaction_id == 0, "AC2: composite default 0");
    }
    return true;
}

// ── AC3: shared composite across batch ──────────────────────────

bool test_ac3_shared_composite() {
    std::println("\n--- AC3: shared composite_transaction_id ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    auto a = flat->add_literal(1);
    auto b = flat->add_literal(2);
    auto c = flat->add_literal(3);

    const std::uint64_t tx = 4242;
    flat->set_mutation_author_fingerprint(0xBEEF);
    flat->set_mutation_composite_transaction_id(tx);

    flat->add_mutation(a, "m1", "", "", "one");
    flat->add_mutation(b, "m2", "", "", "two");
    flat->add_mutation(c, "m3", "", "", "three");

    const auto& log = flat->all_mutations();
    CHECK(log.size() == 3, "AC3: 3 mutations");
    int shared = 0;
    for (const auto& rec : log) {
        if (rec.composite_transaction_id == tx)
            ++shared;
        CHECK(rec.author_fingerprint == 0xBEEF, "AC3: author shared across batch");
    }
    CHECK(shared == 3, "AC3: all 3 share composite_transaction_id");
    return true;
}

// ── AC4: parent chain ───────────────────────────────────────────

bool test_ac4_parent_chain() {
    std::println("\n--- AC4: parent chain (first root, rest link to first) ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    auto a = flat->add_literal(10);
    auto b = flat->add_literal(20);
    auto c = flat->add_literal(30);

    flat->set_mutation_composite_transaction_id(777);
    flat->set_mutation_parent_mutation_id(0); // root
    auto mid1 = flat->add_mutation(a, "root-op", "", "", "root");
    flat->set_mutation_parent_mutation_id(mid1); // subsequent → first
    auto mid2 = flat->add_mutation(b, "child-op", "", "", "child1");
    auto mid3 = flat->add_mutation(c, "child-op", "", "", "child2");

    const auto& log = flat->all_mutations();
    CHECK(log.size() == 3, "AC4: 3 mutations");
    // Find by id
    const aura::ast::MutationRecord* r1 = nullptr;
    const aura::ast::MutationRecord* r2 = nullptr;
    const aura::ast::MutationRecord* r3 = nullptr;
    for (const auto& rec : log) {
        if (rec.mutation_id == mid1)
            r1 = &rec;
        if (rec.mutation_id == mid2)
            r2 = &rec;
        if (rec.mutation_id == mid3)
            r3 = &rec;
    }
    CHECK(r1 && r2 && r3, "AC4: all three records found");
    if (r1 && r2 && r3) {
        CHECK(r1->parent_mutation_id == 0, "AC4: first mutation is root (parent=0)");
        CHECK(r2->parent_mutation_id == mid1, "AC4: second links to first");
        CHECK(r3->parent_mutation_id == mid1, "AC4: third links to first");
        CHECK(r1->composite_transaction_id == 777 && r2->composite_transaction_id == 777 &&
                  r3->composite_transaction_id == 777,
              "AC4: all share composite id");
    }
    return true;
}

// ── AC5: create_mutation_record params ──────────────────────────

bool test_ac5_create_record_params() {
    std::println("\n--- AC5: create_mutation_record stamps provenance params ---");
    auto rec = aura::ast::mutation::create_mutation_record({
        .mutation_id = 1,
        .target_node = 5,
        .operator_name = "op",
        .old_type_str = "A",
        .new_type_str = "B",
        .summary = "s",
        .author_fingerprint = 0x111,
        .parent_mutation_id = 2,
        .composite_transaction_id = 3,
    });
    CHECK(rec.author_fingerprint == 0x111, "AC5: create_mutation_record author");
    CHECK(rec.parent_mutation_id == 2, "AC5: create_mutation_record parent");
    CHECK(rec.composite_transaction_id == 3, "AC5: create_mutation_record composite");

    auto sub = aura::ast::mutation::create_subtree_mutation_record({
        .mutation_id = 9,
        .target_node = 1,
        .parent_id = 0,
        .child_idx = 0,
        .old_subtree_source = "(old)",
        .operator_name = "replace-subtree",
        .summary = "sub",
        .author_fingerprint = 0x222,
        .parent_mutation_id = 8,
        .composite_transaction_id = 7,
    });
    CHECK(sub.author_fingerprint == 0x222, "AC5: create_subtree author");
    CHECK(sub.parent_mutation_id == 8, "AC5: create_subtree parent");
    CHECK(sub.composite_transaction_id == 7, "AC5: create_subtree composite");
    return true;
}

// ── AC6: accessors round-trip ───────────────────────────────────

bool test_ac6_accessors() {
    std::println("\n--- AC6: FlatAST provenance accessors round-trip ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);

    CHECK(flat->mutation_author_fingerprint() == 0, "AC6: default author 0");
    CHECK(flat->mutation_parent_mutation_id() == 0, "AC6: default parent 0");
    CHECK(flat->mutation_composite_transaction_id() == 0, "AC6: default composite 0");

    flat->set_mutation_author_fingerprint(42);
    flat->set_mutation_parent_mutation_id(43);
    flat->set_mutation_composite_transaction_id(44);
    CHECK(flat->mutation_author_fingerprint() == 42, "AC6: author round-trip");
    CHECK(flat->mutation_parent_mutation_id() == 43, "AC6: parent round-trip");
    CHECK(flat->mutation_composite_transaction_id() == 44, "AC6: composite round-trip");
    return true;
}

} // namespace test_issue_1419_detail

int aura_issue_1419_run() {
    using namespace test_issue_1419_detail;
    std::println("=== Issue #1419: Compound provenance wire-up ===");
    bool all_ok = true;
    all_ok &= test_ac1_context_stamps_on_add_mutation();
    all_ok &= test_ac2_defaults_zero();
    all_ok &= test_ac3_shared_composite();
    all_ok &= test_ac4_parent_chain();
    all_ok &= test_ac5_create_record_params();
    all_ok &= test_ac6_accessors();
    if (all_ok && g_failed == 0) {
        std::println("\n=== ALL ACs PASS ===");
        return 0;
    }
    std::println("\n=== Some ACs FAILED (g_failed={}) ===", g_failed);
    return 1;
}

int main() {
    return aura_issue_1419_run();
}
