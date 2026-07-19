// test_issue_501_phase2.cpp — Issue #501 Phase 2:
// Apply Mutator concept to mutation strategy classes
// (scope-limited close).
//
// Verifies the strategy classes in aura.core.mutators
// (ReplaceChildMutator / InsertChildMutator /
// RemoveChildMutator / NoOpMutator) satisfy the Mutator
// concept (compile-time), the generic apply_mutation<>()
// template dispatches through the concept, and the
// apply_by_kind / apply_by_name runtime tag dispatchers
// route correctly with proper error propagation.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <print>

import aura.core.mutators;
import aura.core.ast;
import aura.core.error;
import aura.core.concepts;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println("  PASS: {}", msg);                                                       \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {}", msg);                                                       \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                                        \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            ++g_passed;                                                                            \
            std::println("  PASS: {}  ({} == {})", msg, _a, _b);                                   \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {}  ({} != {})", msg, _a, _b);                                   \
        }                                                                                          \
    } while (0)

// Helper: build a 2-child let node. add_let sets children[0]
// (val) and children[1] (body). Returns the let id.
static aura::ast::NodeId make_let_2(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                    const char* name, std::int64_t val,
                                    aura::ast::NodeId body = aura::ast::NULL_NODE) {
    auto name_sym = pool.intern(name);
    auto val_node = flat.add_literal(val);
    auto id = flat.add_let(name_sym, val_node, body);
    flat.root = id;
    return id;
}

// ── AC1: Strategy classes satisfy Mutator concept ─────────
bool test_strategy_classes_satisfy_mutator() {
    std::println("\n--- AC1: strategy classes satisfy Mutator<FlatAST> ---");
    using aura::ast::FlatAST;
    using aura::ast::mutators::InsertChildMutator;
    using aura::ast::mutators::NoOpMutator;
    using aura::ast::mutators::RemoveChildMutator;
    using aura::ast::mutators::ReplaceChildMutator;

    static_assert(aura::core::Mutator<ReplaceChildMutator, FlatAST>,
                  "ReplaceChildMutator must satisfy Mutator<FlatAST>");
    static_assert(aura::core::Mutator<InsertChildMutator, FlatAST>,
                  "InsertChildMutator must satisfy Mutator<FlatAST>");
    static_assert(aura::core::Mutator<RemoveChildMutator, FlatAST>,
                  "RemoveChildMutator must satisfy Mutator<FlatAST>");
    static_assert(aura::core::Mutator<NoOpMutator, FlatAST>,
                  "NoOpMutator must satisfy Mutator<FlatAST>");
    CHECK(true, "static_asserts: all 4 strategies satisfy Mutator<FlatAST>");
    return true;
}

// ── AC2: ReplaceChildMutator happy path ──────────────────
bool test_replace_child_success() {
    std::println("\n--- AC2: ReplaceChildMutator happy path ---");
    using namespace aura::ast;
    using namespace aura::ast::mutators;

    FlatAST flat;
    StringPool pool;
    auto let_id = make_let_2(flat, pool, "a", 1);

    const auto kids_before = flat.children(let_id);
    CHECK_EQ(kids_before.size(), 2u, "let has 2 children before replace");
    CHECK(kids_before[0] != NULL_NODE, "child[0] is the val node (not NULL)");

    ReplaceChildMutator r{/*index*/ 0, /*new_child*/ NULL_NODE};
    auto result = apply_mutation(flat, let_id, r);
    CHECK(result.has_value(), "apply_mutation returned success");
    CHECK_EQ(result.value(), let_id, "result is the target node id");

    const auto& kids_after = flat.children(let_id);
    CHECK_EQ(kids_after.size(), 2u, "let still has 2 children (size unchanged)");
    CHECK_EQ(kids_after[0], NULL_NODE, "child[0] replaced with NULL_NODE");
    return true;
}

// ── AC3: ReplaceChildMutator invalid target ──────────────
bool test_replace_child_invalid_target() {
    std::println("\n--- AC3: ReplaceChildMutator invalid target ---");
    using namespace aura::ast;
    using namespace aura::ast::mutators;

    FlatAST flat;
    ReplaceChildMutator r{0, NULL_NODE};
    auto result = apply_mutation(flat, /*target*/ NULL_NODE, r);
    CHECK(!result.has_value(), "invalid target returns AuraError");
    CHECK_EQ(static_cast<int>(result.error().kind),
             static_cast<int>(aura::core::AuraErrorKind::MutationInvalidTarget),
             "error kind is MutationInvalidTarget");
    CHECK(result.error().message.find("ReplaceChildMutator") != std::string::npos,
          "error message names the strategy");
    return true;
}

// ── AC4: ReplaceChildMutator index out of range ─────────
bool test_replace_child_out_of_range() {
    std::println("\n--- AC4: ReplaceChildMutator index out of range ---");
    using namespace aura::ast;
    using namespace aura::ast::mutators;

    FlatAST flat;
    StringPool pool;
    auto let_id = make_let_2(flat, pool, "a", 1);

    ReplaceChildMutator r{/*index*/ 5, NULL_NODE};
    auto result = apply_mutation(flat, let_id, r);
    CHECK(!result.has_value(), "out-of-range index returns AuraError");
    CHECK_EQ(static_cast<int>(result.error().kind),
             static_cast<int>(aura::core::AuraErrorKind::MutationOutOfRange),
             "error kind is MutationOutOfRange");
    CHECK(result.error().message.find("index") != std::string::npos,
          "error message mentions index");
    return true;
}

// ── AC5: InsertChildMutator happy path ───────────────────
bool test_insert_child_happy_path() {
    std::println("\n--- AC5: InsertChildMutator happy path ---");
    using namespace aura::ast;
    using namespace aura::ast::mutators;

    FlatAST flat;
    StringPool pool;
    auto let_id = make_let_2(flat, pool, "a", 1);
    auto original_first = flat.children(let_id)[0];

    auto two = flat.add_literal(2);
    InsertChildMutator ins{/*index*/ 0, two};
    auto result = apply_mutation(flat, let_id, ins);
    CHECK(result.has_value(), "insert succeeds");
    const auto& kids = flat.children(let_id);
    CHECK_EQ(kids.size(), 3u, "let now has 3 children");
    CHECK_EQ(kids[0], two, "child[0] is the inserted two");
    CHECK_EQ(kids[1], original_first, "child[1] is the original val (shifted)");
    return true;
}

// ── AC6: RemoveChildMutator happy path ───────────────────
bool test_remove_child_happy_path() {
    std::println("\n--- AC6: RemoveChildMutator happy path ---");
    using namespace aura::ast;
    using namespace aura::ast::mutators;

    FlatAST flat;
    StringPool pool;
    auto let_id = make_let_2(flat, pool, "a", 1);
    const auto kids_before = flat.children(let_id);
    auto original_body = kids_before[1];

    RemoveChildMutator rem{/*index*/ 0};
    auto result = apply_mutation(flat, let_id, rem);
    CHECK(result.has_value(), "remove succeeds");
    const auto& kids = flat.children(let_id);
    CHECK_EQ(kids.size(), 1u, "let now has 1 child");
    CHECK_EQ(kids[0], original_body, "remaining child is the original body");
    return true;
}

// ── AC7: NoOpMutator identity ────────────────────────────
bool test_no_op_mutator() {
    std::println("\n--- AC7: NoOpMutator identity ---");
    using namespace aura::ast;
    using namespace aura::ast::mutators;

    FlatAST flat;
    StringPool pool;
    auto let_id = make_let_2(flat, pool, "a", 1);
    const auto kids_before = flat.children(let_id);

    NoOpMutator noop;
    auto result = apply_mutation(flat, let_id, noop);
    CHECK(result.has_value(), "noop returns success");
    CHECK_EQ(result.value(), let_id, "noop returns the target unchanged");
    CHECK_EQ(flat.children(let_id).size(), kids_before.size(), "noop doesn't modify child count");
    CHECK_EQ(flat.children(let_id)[0], kids_before[0], "noop doesn't modify child[0]");
    return true;
}

// ── AC8: Generic dispatch through lvalue + rvalue ────────
bool test_generic_dispatch_lvalue_and_rvalue() {
    std::println("\n--- AC8: apply_mutation accepts lvalue + rvalue ---");
    using namespace aura::ast;
    using namespace aura::ast::mutators;

    FlatAST flat;
    StringPool pool;
    auto let_id = make_let_2(flat, pool, "a", 1);

    auto r1 = apply_mutation(flat, let_id, ReplaceChildMutator{0, NULL_NODE});
    CHECK(r1.has_value(), "rvalue ReplaceChildMutator dispatches");

    // Use a fresh node for the lvalue test (generation was bumped
    // by mark_dirty_upward in r1).
    auto let_id2 = make_let_2(flat, pool, "b", 2);
    NoOpMutator noop;
    auto r2 = apply_mutation(flat, let_id2, noop);
    CHECK(r2.has_value(), "lvalue NoOpMutator dispatches");
    return true;
}

// ── AC9: Monadic chaining with AuraResult ────────────────
bool test_monadic_chaining() {
    std::println("\n--- AC9: AuraResult monadic chaining ---");
    using namespace aura::ast;
    using namespace aura::ast::mutators;

    FlatAST flat;
    StringPool pool;
    auto let_id = make_let_2(flat, pool, "a", 1);
    auto two = flat.add_literal(2);

    // Happy path: chain succeeds.
    auto chained_ok = apply_mutation(flat, let_id, ReplaceChildMutator{0, two})
                          .and_then([&](NodeId id) -> aura::core::AuraResult<NodeId> {
                              return apply_mutation(flat, id, NoOpMutator{});
                          });
    CHECK(chained_ok.has_value(), "happy-path chain returns success");
    CHECK_EQ(chained_ok.value(), let_id, "chain returns the original target");

    // Failure path: bad index on first step short-circuits.
    // Use a freshly-built let to avoid generation invalidation.
    auto let_id_err = make_let_2(flat, pool, "b", 2);
    auto chained_err = apply_mutation(flat, let_id_err, ReplaceChildMutator{99, NULL_NODE})
                           .and_then([&](NodeId /*id*/) -> aura::core::AuraResult<NodeId> {
                               return aura::core::AuraResult<NodeId>{std::in_place, NULL_NODE};
                           });
    CHECK(!chained_err.has_value(), "monadic chain short-circuits on first error");
    CHECK_EQ(static_cast<int>(chained_err.error().kind),
             static_cast<int>(aura::core::AuraErrorKind::MutationOutOfRange),
             "preserved error kind is MutationOutOfRange");
    return true;
}

// ── AC10: StrategyKind enum + kind_name() ──────────────
bool test_strategy_kind_enum() {
    std::println("\n--- AC10: StrategyKind + kind_name ---");
    using namespace aura::ast::mutators;

    CHECK_EQ(static_cast<int>(StrategyKind::NoOp), 0, "NoOp = 0");
    CHECK_EQ(static_cast<int>(StrategyKind::ReplaceChild), 1, "ReplaceChild = 1");
    CHECK_EQ(static_cast<int>(StrategyKind::InsertChild), 2, "InsertChild = 2");
    CHECK_EQ(static_cast<int>(StrategyKind::RemoveChild), 3, "RemoveChild = 3");

    auto n = kind_name(StrategyKind::NoOp);
    auto r = kind_name(StrategyKind::ReplaceChild);
    auto i = kind_name(StrategyKind::InsertChild);
    auto x = kind_name(StrategyKind::RemoveChild);

    CHECK_EQ(n, std::string_view("no-op"), "kind_name(NoOp)");
    CHECK_EQ(r, std::string_view("replace-child"), "kind_name(ReplaceChild)");
    CHECK_EQ(i, std::string_view("insert-child"), "kind_name(InsertChild)");
    CHECK_EQ(x, std::string_view("remove-child"), "kind_name(RemoveChild)");
    return true;
}

// ── AC11: Concept rejects a non-Mutator shape ───────────
struct BadStrategyNoApply {
    int x = 0;
};
struct BadStrategyWrongReturn {
    int apply(aura::ast::FlatAST&, aura::ast::NodeId) { return 0; }
};

bool test_concept_rejects_bad_shapes() {
    std::println("\n--- AC11: Mutator concept rejects bad shapes ---");
    using aura::ast::FlatAST;
    static_assert(!aura::core::Mutator<BadStrategyNoApply, FlatAST>,
                  "strategy without apply() must NOT satisfy Mutator");
    static_assert(!aura::core::Mutator<BadStrategyWrongReturn, FlatAST>,
                  "strategy returning int must NOT satisfy Mutator");
    CHECK(true, "static_asserts: bad shapes rejected");
    return true;
}

// ── AC12: apply_by_kind dispatch ────────────────────────
//
// Drive every StrategyKind through the runtime tag dispatcher
// and verify each routes to the right strategy.
bool test_apply_by_kind() {
    std::println("\n--- AC12: apply_by_kind tag dispatch ---");
    using namespace aura::ast;
    using namespace aura::ast::mutators;

    // NoOp: target unchanged.
    {
        FlatAST flat;
        StringPool pool;
        auto let_id = make_let_2(flat, pool, "a", 1);
        const auto kids_before = flat.children(let_id).size();
        auto r = apply_by_kind(flat, let_id, StrategyKind::NoOp, {});
        CHECK(r.has_value(), "NoOp returns success");
        CHECK_EQ(flat.children(let_id).size(), kids_before, "NoOp didn't mutate");
    }

    // ReplaceChild: child[0] -> NULL_NODE.
    {
        FlatAST flat;
        StringPool pool;
        auto let_id = make_let_2(flat, pool, "a", 1);
        auto r = apply_by_kind(flat, let_id, StrategyKind::ReplaceChild,
                               StrategyParams{/*index*/ 0, /*new_child*/ NULL_NODE});
        CHECK(r.has_value(), "ReplaceChild returns success");
        CHECK_EQ(flat.children(let_id)[0], NULL_NODE, "child[0] replaced");
    }

    // InsertChild: insert at index 0.
    {
        FlatAST flat;
        StringPool pool;
        auto let_id = make_let_2(flat, pool, "a", 1);
        auto two = flat.add_literal(2);
        auto r = apply_by_kind(flat, let_id, StrategyKind::InsertChild,
                               StrategyParams{/*index*/ 0, /*new_child*/ two});
        CHECK(r.has_value(), "InsertChild returns success");
        CHECK_EQ(flat.children(let_id).size(), 3u, "child count grew to 3");
        CHECK_EQ(flat.children(let_id)[0], two, "child[0] is the inserted two");
    }

    // RemoveChild: erase child[0].
    {
        FlatAST flat;
        StringPool pool;
        auto let_id = make_let_2(flat, pool, "a", 1);
        const auto expected_remaining = flat.children(let_id)[1];
        auto r =
            apply_by_kind(flat, let_id, StrategyKind::RemoveChild, StrategyParams{/*index*/ 0});
        CHECK(r.has_value(), "RemoveChild returns success");
        CHECK_EQ(flat.children(let_id).size(), 1u, "child count shrank to 1");
        CHECK_EQ(flat.children(let_id)[0], expected_remaining,
                 "remaining child is the original body");
    }

    // Error path: out-of-range index propagates from strategy.
    {
        FlatAST flat;
        StringPool pool;
        auto let_id = make_let_2(flat, pool, "a", 1);
        auto r = apply_by_kind(flat, let_id, StrategyKind::ReplaceChild,
                               StrategyParams{/*index*/ 99, /*new_child*/ NULL_NODE});
        CHECK(!r.has_value(), "bad index returns AuraError");
        CHECK_EQ(static_cast<int>(r.error().kind),
                 static_cast<int>(aura::core::AuraErrorKind::MutationOutOfRange),
                 "error kind propagated from strategy");
    }
    return true;
}

// ── AC13: apply_by_name string-tagged dispatch ───────────
//
// Verify kind_from_name is the inverse of kind_name, and
// apply_by_name routes correctly. Unknown names return
// MutationInvalidField.
bool test_apply_by_name() {
    std::println("\n--- AC13: apply_by_name string dispatch ---");
    using namespace aura::ast::mutators;

    // Round-trip: every kind_name must parse back via kind_from_name.
    for (auto k : {StrategyKind::NoOp, StrategyKind::ReplaceChild, StrategyKind::InsertChild,
                   StrategyKind::RemoveChild}) {
        auto name = kind_name(k);
        auto parsed = kind_from_name(name);
        CHECK(parsed.has_value(), "kind_from_name parses its own kind_name");
        if (parsed) {
            CHECK_EQ(static_cast<int>(*parsed), static_cast<int>(k), "round-trip preserves kind");
        }
    }

    // Unknown name returns nullopt.
    auto bogus = kind_from_name("nonexistent-strategy");
    CHECK(!bogus.has_value(), "kind_from_name rejects unknown names");

    // apply_by_name routes through.
    {
        using namespace aura::ast;
        FlatAST flat;
        StringPool pool;
        auto let_id = make_let_2(flat, pool, "a", 1);
        auto r = apply_by_name(flat, let_id, "replace-child", StrategyParams{0, NULL_NODE});
        CHECK(r.has_value(), "apply_by_name('replace-child') succeeds");
        CHECK_EQ(flat.children(let_id)[0], NULL_NODE, "child[0] replaced via name");
    }

    // apply_by_name unknown returns MutationInvalidField.
    {
        using namespace aura::ast;
        FlatAST flat;
        StringPool pool;
        auto let_id = make_let_2(flat, pool, "a", 1);
        auto r = apply_by_name(flat, let_id, "explode-and-die", {});
        CHECK(!r.has_value(), "unknown name returns AuraError");
        CHECK_EQ(static_cast<int>(r.error().kind),
                 static_cast<int>(aura::core::AuraErrorKind::MutationInvalidField),
                 "error kind is MutationInvalidField");
    }
    return true;
}

int main() {
    std::println("═══ Issue #501 Phase 2 verification ═══\n");
    std::println("AC #1: strategy classes satisfy Mutator");
    test_strategy_classes_satisfy_mutator();
    std::println("\nAC #2: ReplaceChildMutator happy path");
    test_replace_child_success();
    std::println("\nAC #3: ReplaceChildMutator invalid target");
    test_replace_child_invalid_target();
    std::println("\nAC #4: ReplaceChildMutator out-of-range index");
    test_replace_child_out_of_range();
    std::println("\nAC #5: InsertChildMutator happy path");
    test_insert_child_happy_path();
    std::println("\nAC #6: RemoveChildMutator happy path");
    test_remove_child_happy_path();
    std::println("\nAC #7: NoOpMutator identity");
    test_no_op_mutator();
    std::println("\nAC #8: generic dispatch lvalue + rvalue");
    test_generic_dispatch_lvalue_and_rvalue();
    std::println("\nAC #9: monadic chaining");
    test_monadic_chaining();
    std::println("\nAC #10: StrategyKind + kind_name");
    test_strategy_kind_enum();
    std::println("\nAC #11: concept rejects bad shapes");
    test_concept_rejects_bad_shapes();
    std::println("\nAC #12: apply_by_kind tag dispatch");
    test_apply_by_kind();
    std::println("\nAC #13: apply_by_name string dispatch");
    test_apply_by_name();

    std::println("\n════════════════════════════════════════");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}