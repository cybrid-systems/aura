// @category: integration
// @reason: ADT/match exhaustiveness post-mutation reliability (#612)
//
// test_adt_match_exhaust_post_mutate_reliability.cpp — Issue #612:
// DefineType variant changes + match exhaustiveness re-check after
// mutation. Non-duplicative with #260 (base machinery) and #341
// (occurrence narrowing stats).

#include "test_harness.hpp"

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;
import aura.diag;
import aura.parser.parser;

namespace aura_612_detail {

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

static aura::ast::NodeId find_match_let(const aura::ast::FlatAST& flat) {
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (flat.has_match_info(id))
            return id;
    }
    return aura::ast::NULL_NODE;
}

static aura::ast::NodeId find_define_type_color(const aura::ast::FlatAST& flat,
                                                const aura::ast::StringPool& pool) {
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (flat.get(id).tag != aura::ast::NodeTag::DefineType)
            continue;
        if (std::string(pool.resolve(flat.get(id).sym_id)) == "Color")
            return id;
    }
    return aura::ast::NULL_NODE;
}

static bool missing_has(const std::vector<std::string>& missing, const char* ctor) {
    return std::find(missing.begin(), missing.end(), ctor) != missing.end();
}

// AC1: stale TypeRegistry (missing new ctor) fixed by refresh
bool test_stale_registry_detects_added_variant() {
    std::println("\n--- AC1: refresh ADT registry after variant add ---");
    const std::string src =
        "(begin (define-type (Color) (Red) (Green) (Blue) (Yellow)) "
        "  (let ((x Red)) (match x ((Red) 1) ((Green) 2) ((Blue) 3))))";
    auto ctx = typecheck_src(src);
    auto match_id = find_match_let(*ctx.flat);
    auto def_id = find_define_type_color(*ctx.flat, *ctx.pool);
    CHECK(match_id != aura::ast::NULL_NODE, "found match let");
    CHECK(def_id != aura::ast::NULL_NODE, "found Color DefineType");
    if (match_id == aura::ast::NULL_NODE || def_id == aura::ast::NULL_NODE)
        return false;

    auto tid = ctx.reg->lookup_type("Color");
    CHECK(tid.valid(), "Color type registered");
    // Simulate stale registry: only RGB, AST has Yellow too.
    ctx.reg->register_adt_constructors(tid, {"Red", "Green", "Blue"});
    auto missing_stale = aura::compiler::analyze_match_exhaustiveness(
        *ctx.flat, *ctx.pool, *ctx.reg, match_id);
    CHECK(missing_stale.empty(),
          "stale registry hides missing Yellow (pre-refresh baseline)");

    std::vector<aura::ast::NodeId> dirty = {def_id};
    aura::compiler::refresh_adt_constructors_for_dirty_define_types(
        *ctx.flat, *ctx.pool, *ctx.reg, dirty, nullptr);

    auto missing = aura::compiler::analyze_match_exhaustiveness(
        *ctx.flat, *ctx.pool, *ctx.reg, match_id);
    CHECK(!missing.empty(), "post-refresh reports non-exhaustive match");
    CHECK(missing_has(missing, "Yellow"), "missing constructor is Yellow");
    return true;
}

// AC2: post_mutation_invariant_check emits note after refresh path
bool test_post_mutation_missing_ctor_after_refresh() {
    std::println("\n--- AC2: post_mutation MissingConstructor after ADT refresh ---");
    const std::string src =
        "(begin (define-type (Color) (Red) (Green) (Blue) (Yellow)) "
        "  (let ((x Red)) (match x ((Red) 1) ((Green) 2) ((Blue) 3))))";
    auto ctx = typecheck_src(src);
    auto def_id = find_define_type_color(*ctx.flat, *ctx.pool);
    auto match_id = find_match_let(*ctx.flat);
    CHECK(def_id != aura::ast::NULL_NODE && match_id != aura::ast::NULL_NODE,
          "Color define + match present");
    if (def_id == aura::ast::NULL_NODE)
        return false;

    ctx.flat->mark_dirty(def_id);
    ctx.flat->mark_dirty(match_id);

    aura::ast::MutationRecord rec;
    rec.mutation_id = 612;
    rec.target_node = def_id;
    rec.parent_id = ctx.flat->parent_of(def_id);
    rec.operator_name = "mutate:replace-subtree";

    std::vector<aura::compiler::OwnershipNote> notes;
    auto st = aura::compiler::post_mutation_invariant_check(
        *ctx.flat, *ctx.pool, *ctx.reg, rec, notes, nullptr);
    bool found = false;
    for (auto& n : notes) {
        if (n.kind == "MissingConstructorInNestedMatch") {
            found = true;
            CHECK(n.message.find("Yellow") != std::string::npos,
                  "note mentions missing Yellow");
        }
    }
    CHECK(found, "MissingConstructorInNestedMatch after DefineType mutate");
    CHECK(st == aura::ast::InvariantStatus::Warnings, "status is Warnings");
    return true;
}

// AC3: complete match stays complete after refresh
bool test_complete_match_stable_after_refresh() {
    std::println("\n--- AC3: complete match stable after ADT refresh ---");
    const std::string src =
        "(begin (define-type (Color) (Red) (Green) (Blue)) "
        "  (let ((x Red)) (match x ((Red) 1) ((Green) 2) ((Blue) 3))))";
    auto ctx = typecheck_src(src);
    auto def_id = find_define_type_color(*ctx.flat, *ctx.pool);
    auto match_id = find_match_let(*ctx.flat);
    CHECK(def_id != aura::ast::NULL_NODE && match_id != aura::ast::NULL_NODE,
          "define + match present");

    std::vector<aura::ast::NodeId> dirty = {def_id, match_id};
    aura::compiler::refresh_adt_constructors_for_dirty_define_types(
        *ctx.flat, *ctx.pool, *ctx.reg, dirty, nullptr);
    auto missing = aura::compiler::analyze_match_exhaustiveness(
        *ctx.flat, *ctx.pool, *ctx.reg, match_id);
    CHECK(missing.empty(), "complete match remains exhaustive post-refresh");
    return true;
}

// AC4: query:adt-match-exhaust-stats primitive wired
bool test_query_adt_match_exhaust_stats() {
    std::println("\n--- AC4: (query:adt-match-exhaust-stats) primitive ---");
    aura::compiler::CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(query:adt-match-exhaust-stats)");
    CHECK(r.has_value() && aura::compiler::types::is_int(*r),
          "(query:adt-match-exhaust-stats) returns int");
    if (r && aura::compiler::types::is_int(*r))
        CHECK(aura::compiler::types::as_int(*r) >= 0, "stats sum >= 0");
    return true;
}

// AC5: Evaluator ADT sync hook reachable (no crash under set-code)
bool test_evaluator_adt_sync_hook() {
    std::println("\n--- AC5: Evaluator::sync_workspace_adt_registry hook ---");
    aura::compiler::CompilerService cs;
    (void)cs.eval("(set-code \"(define-type (Color) (Red) (Green) (Blue))\")");
    (void)cs.eval("(eval-current)");
    cs.evaluator().sync_workspace_adt_registry();
    CHECK(true, "sync_workspace_adt_registry completes without crash");
    return true;
}

int run_tests() {
    std::println("═══ Issue #612 — ADT match exhaust post-mutate reliability ═══\n");
    test_stale_registry_detects_added_variant();
    test_post_mutation_missing_ctor_after_refresh();
    test_complete_match_stable_after_refresh();
    test_query_adt_match_exhaust_stats();
    test_evaluator_adt_sync_hook();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

}  // namespace aura_612_detail

int aura_issue_612_run() { return aura_612_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_612_run(); }
#endif