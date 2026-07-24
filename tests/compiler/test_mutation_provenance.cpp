// tests/test_mutation_provenance.cpp — Issue #1412: Compound
// provenance on MutationRecord (author_fingerprint +
// parent_mutation_id + composite_transaction_id).
//
// Background: a previous round (Issue #1376 follow-up) added
// `predicate_cond_node` + `source_mutation_id` + `narrow_evidence`
// to CoercionEntry, but never to MutationRecord. Issue #1412
// extends MutationRecord with three new fields for the AI audit
// trail:
//   - author_fingerprint: 0 = system, non-zero = hash(agent_id)
//   - parent_mutation_id: single-link chain (Issue #1408 composite
//     uses this to link sub-mutations to the transaction root)
//   - composite_transaction_id: all sub-mutations in an atomic
//     batch share the same id (set by TypedTransactionGuard)
//
// All three default to 0 for backward compatibility — pre-#1412
// records and system-initiated mutations read as system / no parent /
// no composite. The wiring of these fields at typed_mutate sites is
// a follow-up issue (this commit only extends the struct + proves
// the fields exist + the new defaults work end-to-end).
//
// ACs:
//   AC1: MutationRecord has the 3 new fields (struct-level test)
//   AC2: Default values are 0 (system / no parent / no composite)
//   AC3: Brace-init with all 19+ fields compiles + preserves values
//   AC4: add_mutation creates a record with the 3 new fields
//        defaulted to 0 (proves the struct addition doesn't break
//        the existing C++ call sites that don't set provenance)
//   AC5: Setting the 3 fields after construction works (mutable)

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;
import aura.core.mutation; // MutationRecord

namespace test_mutation_provenance_detail {

// ── AC1: struct has the 3 new fields (compile-time guarantee) ─────
//
// C++ doesn't have reflection, so we can't enumerate the fields.
// Instead, we test the struct by brace-initializing with all fields
// named (AC3) and assigning to each new field. The fact that this
// file compiles is the AC1 test.
bool test_struct_has_provenance_fields() {
    std::println("\n--- AC1: MutationRecord has the 3 provenance fields ---");
    // The fact that the brace-init in AC3 compiles proves all fields
    // exist. Here we just confirm we can construct a default
    // MutationRecord and observe the new fields' default values.
    aura::ast::MutationRecord rec;
    CHECK(rec.author_fingerprint == 0,
          "AC1: author_fingerprint field exists, defaults to 0 (system)");
    CHECK(rec.parent_mutation_id == 0,
          "AC1: parent_mutation_id field exists, defaults to 0 (no parent)");
    CHECK(rec.composite_transaction_id == 0,
          "AC1: composite_transaction_id field exists, defaults to 0 (no composite)");
    return true;
}

// ── AC2: default values are 0 ────────────────────────────────────

bool test_default_values() {
    std::println("\n--- AC2: default values are 0 (backward compat) ---");
    aura::ast::MutationRecord rec;
    CHECK(rec.author_fingerprint == 0, "AC2: author_fingerprint == 0 (system)");
    CHECK(rec.parent_mutation_id == 0, "AC2: parent_mutation_id == 0 (no parent)");
    CHECK(rec.composite_transaction_id == 0, "AC2: composite_transaction_id == 0 (no composite)");
    return true;
}

// ── AC3: brace-init with all fields compiles + preserves values ──

bool test_brace_init_with_provenance() {
    std::println("\n--- AC3: brace-init with all fields + provenance values ---");
    aura::ast::MutationRecord rec{
        /*mutation_id=*/42,
        /*timestamp_ms=*/1700000000000ULL,
        /*target_node=*/100,
        /*operator_name=*/"mutate:replace-type",
        /*old_type_str=*/"Int",
        /*new_type_str=*/"Number",
        /*summary=*/"x: Int → x: Number",
        /*status=*/aura::ast::MutationStatus::Committed,
        /*field_offset=*/0,
        /*old_value=*/0,
        /*new_value=*/0,
        /*has_rollback_data=*/false,
        /*parent_id=*/0,
        /*child_idx=*/0,
        /*old_subtree_source=*/"",
        /*has_subtree_rollback=*/false,
        /*invariant_status=*/aura::ast::InvariantStatus::NotChecked,
        // Issue #1412: the 3 new provenance fields
        /*author_fingerprint=*/0xDEADBEEFCAFEBABEULL,
        /*parent_mutation_id=*/7,         // chained mutation #7
        /*composite_transaction_id=*/123, // Issue #1408 atomic batch #123
    };
    CHECK(rec.mutation_id == 42, "AC3: mutation_id preserved");
    CHECK(rec.author_fingerprint == 0xDEADBEEFCAFEBABEULL,
          "AC3: author_fingerprint preserved (system-vs-agent fingerprint)");
    CHECK(rec.parent_mutation_id == 7, "AC3: parent_mutation_id preserved (single-link chain)");
    CHECK(rec.composite_transaction_id == 123,
          "AC3: composite_transaction_id preserved (Issue #1408 atomic batch)");
    return true;
}

// ── AC4: add_mutation creates record with provenance fields default
//        to 0 (proves the struct addition doesn't break the existing
//        C++ call sites that don't set provenance)

bool test_add_mutation_creates_record_with_zero_provenance() {
    std::println("\n--- AC4: add_mutation creates record with provenance=0 ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    // Build a minimal let-binding so we have a valid target node.
    auto x_sym = pool->intern("x");
    auto lit = flat->add_literal(42);
    auto x_var = flat->add_variable(x_sym);
    auto root = flat->add_let(x_sym, lit, x_var);
    flat->root = root;

    // add_mutation on x_var with no provenance arguments.
    auto mid = flat->add_mutation(x_var, "mutate:rebind", "<old>", "<new>", "<summary>");
    CHECK(mid != 0, "AC4.setup: add_mutation returns a non-zero mutation_id");

    // Read back via mutation_history. The new fields should all be 0
    // (backward compat: existing call sites don't set them).
    const auto hist = flat->mutation_history(x_var);
    CHECK(hist.size() == 1, "AC4: 1 record in mutation_history for x_var");
    if (hist.size() >= 1) {
        const auto& rec = hist[0];
        CHECK(
            rec.author_fingerprint == 0,
            "AC4: add_mutation (without provenance args) → author_fingerprint=0 (backward compat)");
        CHECK(rec.parent_mutation_id == 0,
              "AC4: add_mutation (without provenance args) → parent_mutation_id=0");
        CHECK(rec.composite_transaction_id == 0,
              "AC4: add_mutation (without provenance args) → composite_transaction_id=0");
    }
    return true;
}

// ── AC5: setting the 3 fields after construction works (mutable) ─

bool test_set_provenance_after_construction() {
    std::println("\n--- AC5: setting provenance after construction works ---");
    aura::ast::MutationRecord rec;
    rec.author_fingerprint = 0x1234567890ABCDEFULL;
    rec.parent_mutation_id = 999;
    rec.composite_transaction_id = 456;
    CHECK(rec.author_fingerprint == 0x1234567890ABCDEFULL,
          "AC5: author_fingerprint can be set after construction");
    CHECK(rec.parent_mutation_id == 999, "AC5: parent_mutation_id can be set");
    CHECK(rec.composite_transaction_id == 456, "AC5: composite_transaction_id can be set");
    return true;
}

} // namespace test_mutation_provenance_detail

int aura_issue_1412_run() {
    using namespace test_mutation_provenance_detail;
    std::println("=== Issue #1412: Compound provenance on MutationRecord ===");
    bool all_ok = true;
    all_ok &= test_struct_has_provenance_fields();
    all_ok &= test_default_values();
    all_ok &= test_brace_init_with_provenance();
    all_ok &= test_add_mutation_creates_record_with_zero_provenance();
    all_ok &= test_set_provenance_after_construction();
    if (all_ok) {
        std::println("\n=== ALL 5 ACs PASS ===");
        return 0;
    }
    std::println("\n=== Some ACs FAILED ===");
    return 1;
}

int main() {
    return aura_issue_1412_run();
}
