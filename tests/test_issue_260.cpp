// @category: integration
// @reason: uses TypeChecker + post_mutation_invariant_check

// test_issue_260.cpp — Issue #260: nested ADT exhaustiveness +
// mutation-aware occurrence typing robustness.
//
// AC:
//   1. analyze_match_exhaustiveness detects missing ctors (ADT match)
//   2. post_mutation_invariant_check emits MissingConstructorInNestedMatch
//   3. OwnershipNote carries source_mutation_id + blame from MutationRecord
//   4. MatchClauseInfo.exhaustiveness_checked + subject_type_id set after check

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <print>
#include <string>
#include <vector>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.diag;
import aura.parser.parser;

namespace aura_issue_260_detail {

struct TcCtx {
    std::unique_ptr<aura::ast::ASTArena> arena;
    aura::ast::FlatAST* flat = nullptr;
    aura::ast::StringPool* pool = nullptr;
    std::unique_ptr<aura::core::TypeRegistry> reg;
    aura::ast::NodeId root = aura::ast::NULL_NODE;
};

static TcCtx typecheck_src(const std::string& src) {
    TcCtx ctx;
    ctx.arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = ctx.arena->allocator();
    ctx.flat = ctx.arena->create<aura::ast::FlatAST>(alloc);
    ctx.pool = ctx.arena->create<aura::ast::StringPool>(alloc);
    auto pr = aura::parser::parse_to_flat(src, *ctx.flat, *ctx.pool);
    ctx.flat->root = pr.root;
    ctx.root = pr.root;
    if (!pr.success)
        return ctx;
    ctx.reg = std::make_unique<aura::core::TypeRegistry>();
    aura::compiler::TypeChecker tc(*ctx.reg);
    aura::diag::DiagnosticCollector diag;
    tc.infer_flat(*ctx.flat, *ctx.pool, pr.root, diag);
    return ctx;
}

static aura::ast::NodeId find_incomplete_match_let(const aura::ast::FlatAST& flat,
                                                 const aura::ast::StringPool& pool,
                                                 aura::core::TypeRegistry& reg) {
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.has_match_info(id))
            continue;
        auto missing = aura::compiler::analyze_match_exhaustiveness(flat, pool, reg, id);
        if (!missing.empty())
            return id;
    }
    return aura::ast::NULL_NODE;
}

static const std::string k_incomplete_color =
    "(begin (define-type (Color) (Red) (Green) (Blue)) "
    "  (let ((x Red)) (match x ((Red) 1) ((Green) 2))))";

static const std::string k_complete_color =
    "(begin (define-type (Color) (Red) (Green) (Blue)) "
    "  (let ((x Red)) (match x ((Red) 1) ((Green) 2) ((Blue) 3))))";

bool test_analyze_match_missing_ctor() {
    std::println("\n--- Test 1: analyze_match_exhaustiveness ---");
    auto ctx = typecheck_src(k_incomplete_color);
    auto incomplete = find_incomplete_match_let(*ctx.flat, *ctx.pool, *ctx.reg);
    CHECK(incomplete != aura::ast::NULL_NODE, "found incomplete match let");
    if (incomplete == aura::ast::NULL_NODE)
        return false;
    auto missing = aura::compiler::analyze_match_exhaustiveness(*ctx.flat, *ctx.pool, *ctx.reg,
                                                                incomplete);
    CHECK(!missing.empty(), "match reports missing constructors");
    bool has_blue = false;
    for (auto& m : missing) {
        if (m == "Blue")
            has_blue = true;
    }
    CHECK(has_blue, "missing constructor is Blue");
    if (auto* mi = ctx.flat->get_match_info(incomplete)) {
        CHECK(mi->exhaustiveness_checked, "exhaustiveness_checked flag set");
        CHECK(mi->subject_type_id > 0, "subject_type_id cached on MatchClauseInfo");
    }
    return true;
}

bool test_post_mutation_missing_ctor_note() {
    std::println("\n--- Test 2: post_mutation MissingConstructorInNestedMatch ---");
    auto ctx = typecheck_src(k_incomplete_color);
    auto incomplete = find_incomplete_match_let(*ctx.flat, *ctx.pool, *ctx.reg);
    CHECK(incomplete != aura::ast::NULL_NODE, "found incomplete match let");
    if (incomplete == aura::ast::NULL_NODE)
        return false;

    ctx.flat->mark_dirty(incomplete);
    aura::ast::MutationRecord rec;
    rec.mutation_id = 42;
    rec.target_node = incomplete;
    rec.parent_id = ctx.flat->parent_of(incomplete);
    rec.operator_name = "mutate:replace-children";

    std::vector<aura::compiler::OwnershipNote> notes;
    auto st = aura::compiler::post_mutation_invariant_check(*ctx.flat, *ctx.pool, *ctx.reg, rec,
                                                            notes);
    bool found = false;
    for (auto& n : notes) {
        if (n.kind == "MissingConstructorInNestedMatch") {
            found = true;
            CHECK(n.source_mutation_id.has_value() && *n.source_mutation_id == 42,
                  "note links mutation_id");
            CHECK(n.blame.has_value() && n.blame->annotation_src == "mutate:replace-children",
                  "note blame operator");
        }
    }
    CHECK(found, "MissingConstructorInNestedMatch emitted");
    CHECK(st == aura::ast::InvariantStatus::Warnings, "status is Warnings");
    return true;
}

bool test_complete_match_no_missing_note() {
    std::println("\n--- Test 3: complete match — no missing ctor note ---");
    auto ctx = typecheck_src(k_complete_color);
    auto incomplete = find_incomplete_match_let(*ctx.flat, *ctx.pool, *ctx.reg);
    CHECK(incomplete == aura::ast::NULL_NODE, "complete match has no incomplete lets");
    return true;
}

int run_tests() {
    std::println("═══ Issue #260 (ADT exhaustiveness + mutation re-check) ═══\n");
    test_analyze_match_missing_ctor();
    test_post_mutation_missing_ctor_note();
    test_complete_match_no_missing_note();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_260_detail

int aura_issue_260_run() { return aura_issue_260_detail::run_tests(); }