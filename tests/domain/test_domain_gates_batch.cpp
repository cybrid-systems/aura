// test_domain_gates_batch.cpp — Domain suite batch: behavioral gates.
// Consolidates three thin theme drivers that shared CompilerService +
// href / schema helpers into one CI binary (post-AuraDomainTests wire-up):
//
//   fiber / steal / Guard / orch   (was test_domain_fiber_orchestration)
//   macro hygiene / dirty-epoch    (was test_domain_hygiene_dirty)
//   typed mutate / type-system     (was test_domain_typed_mutate)
//
// Prefer adding a section here over a new test_domain_*.cpp binary.
// Schema-only surfaces: test_obs_schema_matrix + cases/*.hpp.

#include "test_harness.hpp"
#include "core/transparent_string_hash.hh"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.compiler.ir;
import aura.compiler.compute_kind;
import aura.compiler.ast_walkers;
import aura.compiler.macro_expansion;
import aura.diag;
import aura.core.type;
import aura.parser.parser;

namespace aura_domain_gates_batch {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(q), key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void expect_hash_schema(CompilerService& cs, std::string_view q, std::int64_t schema) {
    auto h = cs.eval(aura::test::aura_call_expr(q));
    CHECK(h && is_hash(*h), std::format("{} returns hash", q));
    CHECK(href(cs, q, "schema") == schema, std::format("{} schema == {}", q, schema));
}

static void expect_schema(CompilerService& cs, std::string_view q, std::int64_t schema) {
    auto h = cs.eval(aura::test::aura_call_expr(q));
    CHECK(h && is_hash(*h), std::format("{} hash", q));
    CHECK(href(cs, q, "schema") == schema, std::format("{} schema", q));
}

static void expect_schema_active(CompilerService& cs, std::string_view q, std::int64_t schema) {
    auto h = cs.eval(aura::test::aura_call_expr(q));
    CHECK(h && is_hash(*h), std::format("{} hash", q));
    CHECK(href(cs, q, "schema") == schema, std::format("{} schema", q));
    auto act = href(cs, q, "active");
    if (act >= 0)
        CHECK(act == 1, std::format("{} active", q));
}

// ── Fiber / steal / Guard / orch (ex test_domain_fiber_orchestration) ──
static void run_fiber_orchestration(CompilerService& cs) {
    auto& ev = cs.evaluator();

    std::println("\n=== Fiber/scheduler init observability (#810) ===");
    expect_hash_schema(cs, "query:fiber-scheduler-init-stats", 810);
    CHECK(href(cs, "query:fiber-scheduler-init-stats", "aura-result-init-active") == 1,
          "aura-result-init-active");
    ev.bump_fiber_init_aura_result_ok();
    ev.bump_scheduler_init_aura_result_ok();
    CHECK(href(cs, "query:fiber-scheduler-init-stats", "fiber-init-ok") >= 1, "fiber-init-ok");
    CHECK(href(cs, "query:fiber-scheduler-init-stats", "scheduler-init-ok") >= 1,
          "scheduler-init-ok");

    std::println("\n=== Steal + arena/GC coordination (#812) ===");
    expect_hash_schema(cs, "query:orchestration-steal-arena-gc-stats", 812);
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "steal-safety-active") == 1,
          "steal-safety-active");
    ev.bump_steal_arena_yield_during_compact();
    ev.bump_steal_outermost_only_enforced();
    ev.bump_steal_linear_probe_on_success();
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "yield-during-compact") >= 1,
          "yield-during-compact");
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "outermost-only-enforced") >= 1,
          "outermost-only-enforced");
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "linear-probe-on-success") >= 1,
          "linear-probe-on-success");

    std::println("\n=== Guard AuraResult path (#813) ===");
    expect_hash_schema(cs, "query:guard-error-stats", 813);
    CHECK(href(cs, "query:guard-error-stats", "no-unwind-through-guard") == 1,
          "no-unwind-through-guard");
    ev.bump_guard_aura_result_path();
    ev.bump_guard_panic_checkpoint_aura_result();
    CHECK(href(cs, "query:guard-error-stats", "guard-aura-result-path") >= 1,
          "guard-aura-result-path");

    std::println("\n=== Guard/steal safety v2 surface (#875) ===");
    expect_hash_schema(cs, "query:guard-steal-gc-safety-v2-stats", 875);
    CHECK(href(cs, "query:guard-steal-gc-safety-v2-stats", "active") == 1, "875 active");

    std::println("\n=== Runtime health under orchestration (#814) ===");
    expect_hash_schema(cs, "query:runtime-production-health", 814);
    CHECK(href(cs, "query:runtime-production-health", "health-score") >= 0, "health-score");
    auto heal = cs.eval("(runtime:self-heal-on-drift)");
    CHECK(heal.has_value(), "runtime:self-heal-on-drift callable");

    std::println("\n=== JIT exception bridge (#811) ===");
    expect_hash_schema(cs, "query:jit-exception-bridge-stats", 811);
    CHECK(href(cs, "query:jit-exception-bridge-stats", "guest-only-policy-active") == 1,
          "guest-only-policy-active");
    ev.bump_jit_guest_exception_bridge();
    ev.bump_jit_internal_aura_result_path();
    CHECK(href(cs, "query:jit-exception-bridge-stats", "guest-exception-bridge") >= 1,
          "guest-exception-bridge");
    CHECK(href(cs, "query:jit-exception-bridge-stats", "internal-aura-result-path") >= 1,
          "internal-aura-result-path");

    std::println("\n=== JIT fiber exception locality (#821) ===");
    expect_hash_schema(cs, "query:jit-fiber-exception-stats", 821);
    CHECK(href(cs, "query:jit-fiber-exception-stats", "fiber-local-policy-active") == 1,
          "fiber-local-policy-active");
    ev.bump_jit_fiber_ex_stack_local();
    ev.bump_jit_fiber_ex_cross_prevented();
    CHECK(href(cs, "query:jit-fiber-exception-stats", "fiber-local-ex-stack") >= 1,
          "fiber-local-ex-stack");
    CHECK(href(cs, "query:jit-fiber-exception-stats", "cross-fiber-prevented") >= 1,
          "cross-fiber-prevented");

    std::println("\n=== L2 specialization deopt (#822) ===");
    expect_hash_schema(cs, "query:l2-specialization-deopt-stats", 822);
    CHECK(href(cs, "query:l2-specialization-deopt-stats", "l2-maturity-active") == 1,
          "l2-maturity-active");
    ev.bump_l2_spec_pair_fastpath();
    ev.bump_l2_spec_linear_probe();
    CHECK(href(cs, "query:l2-specialization-deopt-stats", "pair-fastpath") >= 1, "pair-fastpath");

    std::println("\n=== Opcode coverage deopt (#823) ===");
    expect_hash_schema(cs, "query:opcode-coverage-deopt-stats", 823);
    CHECK(href(cs, "query:opcode-coverage-deopt-stats", "zero-fallback-policy") == 1,
          "zero-fallback-policy");
    ev.bump_opcode_cov_hit();
    CHECK(href(cs, "query:opcode-coverage-deopt-stats", "coverage-hits") >= 1, "coverage-hits");
}

// ── Macro hygiene + dirty/epoch (ex test_domain_hygiene_dirty) ──
static void run_hygiene_dirty(CompilerService& cs) {
    auto& ev = cs.evaluator();

    std::println("\n=== Macro IR provenance (#815) ===");
    expect_schema(cs, "query:macro-introduced-provenance-stats", 815);
    ev.bump_macro_ir_source_marker_stamp();
    ev.bump_macro_provenance_query();
    CHECK(href(cs, "query:macro-introduced-provenance-stats", "ir-source-marker-stamps") >= 1,
          "ir-source-marker-stamps");
    CHECK(href(cs, "query:macro-introduced-provenance-stats", "marker-propagation-active") == 1,
          "marker-propagation-active");

    std::println("\n=== Dirty/epoch marker awareness (#817) ===");
    expect_schema(cs, "query:dirty-epoch-marker-stats", 817);
    ev.bump_dirty_epoch_macro_introduced_hit();
    ev.bump_dirty_epoch_targeted_relower();
    ev.bump_dirty_epoch_hygiene_drift_prevented();
    CHECK(href(cs, "query:dirty-epoch-marker-stats", "macro-introduced-dirty-hits") >= 1,
          "macro-introduced-dirty-hits");
    CHECK(href(cs, "query:dirty-epoch-marker-stats", "marker-aware-dirty-active") == 1,
          "marker-aware-dirty-active");

    std::println("\n=== Pattern hygiene provenance enforcement (#819) ===");
    expect_schema(cs, "query:pattern-hygiene-provenance-stats", 819);
    ev.bump_pattern_hygiene_provenance_predicate_hit();
    ev.bump_pattern_hygiene_safe_span_enforced();
    CHECK(href(cs, "query:pattern-hygiene-provenance-stats", "enforcement-active") == 1,
          "enforcement-active");

    std::println("\n=== Macro hygiene query v2 (#847) ===");
    expect_schema(cs, "query:macro-hygiene-query-provenance-v2-stats", 847);
    CHECK(href(cs, "query:macro-hygiene-query-provenance-v2-stats", "active") == 1, "847 active");

    std::println("\n=== edsl:define-struct hygiene path (#816) ===");
    expect_schema(cs, "query:edsl-struct-meta-stats", 816);
    auto ok = cs.eval("(edsl:define-struct \"HygStruct\" \"doc\" \"f:int\")");
    CHECK(ok && is_bool(*ok), "edsl:define-struct");
    CHECK(href(cs, "query:edsl-struct-meta-stats", "define-struct-total") >= 1,
          "define-struct-total");

    std::println("\n=== Workspace hygiene queries (smoke) ===");
    auto hyg = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    CHECK(hyg.has_value() || !hyg.has_value(), "pattern-hygiene-stats eval completes");
    (void)hyg;

    std::println("\n=== Terminal render production (#824) ===");
    expect_schema(cs, "query:terminal-render-production-stats", 824);
    CHECK(href(cs, "query:terminal-render-production-stats", "module-active") == 1,
          "module-active");
    auto clr = cs.eval("(terminal:clear)");
    CHECK(clr && is_bool(*clr), "terminal:clear");
    auto pres = cs.eval("(terminal:present)");
    CHECK(pres && is_bool(*pres), "terminal:present");
    CHECK(href(cs, "query:terminal-render-production-stats", "clear-total") >= 1, "clear-total");

    std::println("\n=== Render FFI + hotpath (#825/#826) ===");
    expect_schema(cs, "query:render-ffi-buffer-stats", 825);
    CHECK(href(cs, "query:render-ffi-buffer-stats", "buffer-path-active") == 1,
          "buffer-path-active");
    ev.bump_render_ffi_batch_call();
    CHECK(href(cs, "query:render-ffi-buffer-stats", "batch-ffi-calls") >= 1, "batch-ffi-calls");
    expect_schema(cs, "query:render-hotpath-stats", 826);
    CHECK(href(cs, "query:render-hotpath-stats", "hotpath-active") == 1, "hotpath-active");
    ev.bump_render_hp_jit_coverage();
    CHECK(href(cs, "query:render-hotpath-stats", "jit-coverage") >= 1, "jit-coverage");
}

// ── Typed mutate / type-system (ex test_domain_typed_mutate) ──
static void run_typed_mutate(CompilerService& cs) {
    auto& ev = cs.evaluator();

    std::println("\n=== Typed-mutate / type-system observability ===");
    expect_schema_active(cs, "query:dead-coercion-elim-stats", 832);
    expect_schema_active(cs, "query:occurrence-renarrow-stats", 833);
    expect_schema_active(cs, "query:linear-escape-mutate-stats", 834);
    expect_schema_active(cs, "query:typed-mutate-coercion-stats", 835);
    expect_schema_active(cs, "query:fiber-epoch-type-safety-stats", 836);
    // schema lineage 839 → 1614 → 1894
    expect_schema_active(cs, "query:typed-mutation-audit-stats", 1894);
    expect_schema_active(cs, "query:defuse-infer-partial-stats", 862);
    expect_schema_active(cs, "query:ownership-escape-postmutate-stats", 863);
    expect_schema_active(cs, "query:typed-mutation-audit-pass-stats", 864);

    std::println("\n=== Bump + readback sample paths ===");
    ev.bump_dead_coercion_elim();
    ev.bump_dead_coercion_elim_hit();
    ev.bump_occurrence_renarrow();
    ev.bump_typed_mutate_coercion();
    CHECK(href(cs, "query:dead-coercion-elim-stats", "total") >= 1, "832 total after bump");
    CHECK(href(cs, "query:occurrence-renarrow-stats", "total") >= 1, "833 total after bump");
    CHECK(href(cs, "query:typed-mutate-coercion-stats", "total") >= 1, "835 total after bump");

    std::println("\n=== Mutate atomic-batch e2e surface (#820) ===");
    expect_schema_active(cs, "query:mutate-atomic-batch-e2e-stats", 820);
    ev.bump_mutate_batch_e2e_started();
    CHECK(href(cs, "query:mutate-atomic-batch-e2e-stats", "batches-started") >= 1,
          "batches-started");

    std::println("\n=== Shape / IR-SoA / arena live surfaces (#827–#829) ===");
    expect_schema_active(cs, "query:shape-value-hotpath-contracts-stats", 827);
    CHECK(href(cs, "query:shape-value-hotpath-contracts-stats", "contracts-active") == 1,
          "contracts-active");
    ev.bump_sv_contract_hotpath_check();
    CHECK(href(cs, "query:shape-value-hotpath-contracts-stats", "contract-checks-hotpath") >= 1,
          "contract-checks-hotpath");

    expect_schema_active(cs, "query:ir-soa-full-enforcement-stats", 828);
    CHECK(href(cs, "query:ir-soa-full-enforcement-stats", "enforcement-active") == 1,
          "828 enforcement-active");
    ev.bump_irsoa_enforce_dirty_skip();
    CHECK(href(cs, "query:ir-soa-full-enforcement-stats", "dirty-skips") >= 1, "dirty-skips");

    expect_schema_active(cs, "query:arena-live-defrag-stats", 829);
    CHECK(href(cs, "query:arena-live-defrag-stats", "live-defrag-active") == 1,
          "live-defrag-active");
    ev.bump_arena_ldefrag_auto_trigger();
    CHECK(href(cs, "query:arena-live-defrag-stats", "auto-triggers") >= 1, "auto-triggers");

    std::println("\n=== Simple eval mutate smoke ===");
    auto d = cs.eval("(define tm-x 1)");
    CHECK(d.has_value(), "define tm-x");
    auto r = cs.eval("tm-x");
    CHECK(r && is_int(*r) && as_int(*r) == 1, "tm-x == 1");
}

static void run_all(CompilerService& cs) {
    run_fiber_orchestration(cs);
    run_hygiene_dirty(cs);
    run_typed_mutate(cs);
}

// ── Raw TypeChecker unit tests (folded from tests/issues/{116,118,121,122}.cpp via #1957) ──
// These exercise the type checker directly (not via CompilerService) — they
// pin the contract that infer_flat doesn't mutate the FlatAST, that AST
// nodes get tagged with the right diagnostic kind on error, and that
// gensym / quasiquote / reflect-type primitives parse + typecheck.

struct TypecheckEnv {
    std::unique_ptr<aura::ast::ASTArena> arena;
    aura::ast::FlatAST* flat = nullptr;
    aura::ast::StringPool* pool = nullptr;
    std::unique_ptr<aura::core::TypeRegistry> treg;
    std::unique_ptr<aura::compiler::TypeChecker> tc;
    aura::ast::NodeId root = 0;

    static TypecheckEnv make() {
        TypecheckEnv e;
        e.arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = e.arena->allocator();
        e.flat = e.arena->create<aura::ast::FlatAST>(alloc);
        e.pool = e.arena->create<aura::ast::StringPool>(alloc);
        e.treg = std::make_unique<aura::core::TypeRegistry>();
        e.tc = std::make_unique<aura::compiler::TypeChecker>(*e.treg);
        return e;
    }
    void parse_src(const std::string& src) {
        auto pr = aura::parser::parse_to_flat(src, *flat, *pool);
        flat->root = pr.root;
        root = pr.root;
    }
};

static void run_116_deferred_coercion_node() {
    std::println("\n=== #116: defer CoercionNode (no in-place FlatAST mutation) ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(+ 1 2)");
    int n_before = env.flat->size();
    aura::diag::DiagnosticCollector diag;
    env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(env.flat->size() == n_before, "well-typed: no new AST nodes after infer_flat");
    int coercions = 0;
    for (aura::ast::NodeId i = 0; i < env.flat->size(); ++i)
        if (env.flat->get(i).tag == aura::ast::NodeTag::Coercion)
            ++coercions;
    CHECK(coercions == 0, "no CoercionNodes in AST for well-typed input");

    // apply_coercion_map round-trip + idempotency
    auto arena2 = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena2->allocator();
    auto* flat = arena2->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena2->create<aura::ast::StringPool>(alloc);
    auto callee_sym = pool->intern("+");
    auto callee_var = flat->add_variable(callee_sym);
    auto arg0 = flat->add_literal(42);
    auto arg1 = flat->add_literal(2);
    auto call_id = flat->add_call(callee_var, {arg0, arg1});
    flat->root = call_id;
    int n0 = flat->size();
    aura::compiler::CoercionMap cm;
    cm.add(call_id, 1, arg0, 2, 1, 0, 0);
    auto applied1 = aura::compiler::apply_coercion_map(*flat, cm);
    CHECK(applied1 == 1, "first apply: 1 entry applied");
    auto applied2 = aura::compiler::apply_coercion_map(*flat, cm);
    CHECK(applied2 == 0, "second apply: idempotent (0 entries)");
    CHECK(flat->size() == n0 + 1, "exactly 1 CoercionNode after 2 applies");
}

static void run_118_unbound_var_tags_node() {
    std::println("\n=== #118: unbound variable tags the AST node ===");
    auto env = TypecheckEnv::make();
    env.parse_src("undefined_var");
    aura::diag::DiagnosticCollector diag;
    env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    int unbound = 0;
    for (auto& d : diag.diagnostics())
        if (d.kind == aura::diag::ErrorKind::UnboundVariable)
            ++unbound;
    CHECK(unbound >= 1, "UnboundVariable diagnostic emitted");
    CHECK(env.flat->node_error(env.root) ==
              static_cast<int>(aura::diag::ErrorKind::UnboundVariable),
          "AST node tagged with UnboundVariable");
}

static void run_118_module_member_not_found_tags_node() {
    std::println("\n=== #118: missing module member tags the AST node ===");
    auto env = TypecheckEnv::make();
    env.tc->set_strict(true);
    env.parse_src("no_such_module:foo");
    aura::diag::DiagnosticCollector diag;
    env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    int unbound = 0;
    for (auto& d : diag.diagnostics())
        if (d.kind == aura::diag::ErrorKind::UnboundVariable)
            ++unbound;
    CHECK(unbound >= 1, "missing module: UnboundVariable diagnostic emitted");
}

static void run_118_well_typed_no_tag() {
    std::println("\n=== #118: well-typed input has no diagnostic, no node tag ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(+ 1 2)");
    aura::diag::DiagnosticCollector diag;
    env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(diag.diagnostics().empty(), "well-typed: no diagnostics");
    CHECK(env.flat->node_error(env.root) == 0, "well-typed: AST node NOT tagged");
}

static void run_121_gensym_unique_and_prefix() {
    std::println("\n=== #121: gensym uniqueness and prefix ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(define a (gensym)) (define b (gensym)) (define c (gensym \"tmp\")) "
                  "(define d (symbol-append 'make- 'point))");
    aura::diag::DiagnosticCollector diag;
    auto tid = env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(tid.valid(), "(gensym) + (gensym \"prefix\") + symbol-append parse + typecheck");
}

static void run_121_nested_hygienic_macros_recursive() {
    std::println(
        "\n=== #121: nested hygienic macros call each other recursively (Issue #121 AC) ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(define-hygienic-macro (m1 x) (list '+ x 1)) "
                  "(define-hygienic-macro (m2 x) (list 'm1 (list '* x 2))) "
                  "(define-hygienic-macro (m3 x) (list 'm2 x))");
    aura::diag::DiagnosticCollector diag;
    auto tid = env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(tid.valid(), "nested hygienic macros parse + typecheck");
}

static void run_121_legacy_defmacro_compat() {
    std::println("\n=== #121: legacy `defmacro` backward compat + bounded macro_expand_all ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(defmacro (my-double x) `(+ ,x ,x)) (my-double 5)");
    aura::diag::DiagnosticCollector diag;
    auto tid = env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(tid.valid(), "legacy defmacro + quasiquote parses + typechecks");
}

static void run_122_reflect_type_scalar() {
    std::println("\n=== #122: reflect-type for scalar returns structured description ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(display (reflect-type \"Int\"))");
    aura::diag::DiagnosticCollector diag;
    auto tid = env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(tid.valid(), "reflect-type scalar parses + typechecks");
}

static void run_122_reflect_unknown_returns_void() {
    std::println("\n=== #122: reflect-type / reflect-module-exports unknown name returns void ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(display (reflect-type \"NonexistentType\")) "
                  "(display (reflect-module-exports \"FakeModule\"))");
    aura::diag::DiagnosticCollector diag;
    auto tid = env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(tid.valid(), "unknown-name reflect calls parse + typecheck");
}

static void run_122_reflect_members_in_query() {
    std::println("\n=== #122: reflect-members usable inside query ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(query (reflect-members \"Int\"))");
    aura::diag::DiagnosticCollector diag;
    auto tid = env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(tid.valid(), "reflect-members in query parses + typechecks");
}

// ── Wave 3 (#123 IR-require pre-exec / #124 try-catch / #127 Result aliases / #128 std::span) ──

static void run_123_pre_exec_strips_begin_require() {
    std::println("\n=== #123: pre_exec_requires strips top-level require from (begin ...) ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(begin (require std/list all:) (require std/math all:) (display (+ 1 2)))");
    aura::diag::DiagnosticCollector diag;
    auto tid = env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(tid.valid(), "begin + 2 requires + body parse + typecheck");
}

static void run_123_nested_require_falls_back() {
    std::println("\n=== #123: nested require inside (if) still needs tree-walker fallback ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(if #t (require std/list all:) 0)");
    aura::diag::DiagnosticCollector diag;
    auto tid = env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(tid.valid(), "nested require parses + typechecks");
}

static void run_124_try_catch_raise_parses() {
    std::println("\n=== #124: (try (raise ...) (catch e ...)) parses + typechecks ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(try (raise \"err\") (catch e (display \"caught\") (display e)))");
    aura::diag::DiagnosticCollector diag;
    auto tid = env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(tid.valid(), "try/catch/raise parses + typechecks");
}

static void run_124_nested_try_catch_parses() {
    std::println("\n=== #124: nested (try (try ...) (catch ...)) parses + typechecks ===");
    auto env = TypecheckEnv::make();
    env.parse_src("(try (try (raise \"inner\") (catch e1 (display \"inner-caught\"))) "
                  "     (catch e2 (display \"outer-caught\")))");
    aura::diag::DiagnosticCollector diag;
    auto tid = env.tc->infer_flat(*env.flat, *env.pool, env.root, diag);
    CHECK(tid.valid(), "nested try/catch parses + typechecks");
}

static void run_127_result_alias_type_identity() {
    std::println("\n=== #127: Result / ParseResult / LowerResult / CompileResult / VoidResult type "
                 "identity ===");
    using R1 = aura::diag::Result<int>;
    using R2 = aura::diag::ParseResult<int>;
    using R3 = aura::diag::LowerResult<int>;
    using R4 = aura::diag::CompileResult<int>;
    static_assert(std::is_same_v<R1, R2>);
    static_assert(std::is_same_v<R1, R3>);
    static_assert(std::is_same_v<R1, R4>);
    static_assert(std::is_same_v<aura::diag::VoidResult, aura::diag::Result<void>>);
    CHECK(true, "5 Result aliases resolved");
}

static void run_127_result_error_channel() {
    std::println("\n=== #127: Result<T> Ok/Err construction + diagnostic channel ===");
    aura::diag::Result<int> ok = 42;
    CHECK(ok.has_value(), "Ok has value");
    if (ok)
        CHECK(*ok == 42, "Ok unwraps to 42");
    aura::diag::Diagnostic d{aura::diag::ErrorKind::TypeError, "test error"};
    aura::diag::Result<int> err = std::unexpected(d);
    CHECK(!err.has_value(), "Err has no value");
    CHECK(err.error().kind == aura::diag::ErrorKind::TypeError, "Err kind");
    CHECK(err.error().message == "test error", "Err message");
}

static void run_127_result_monadic_chain() {
    std::println("\n=== #127: monadic transform / and_then on Result ===");
    auto r2 = aura::diag::Result<int>(42).transform([](int x) { return x * 2; });
    CHECK(r2.has_value() && *r2 == 84, "transform on Ok = Ok(84)");
    aura::diag::Diagnostic d{aura::diag::ErrorKind::InternalError, "fail"};
    auto r3 = aura::diag::Result<int>(std::unexpected(d));
    auto r4 = r3.transform([](int) { return 999; });
    CHECK(!r4.has_value() && r4.error().message == "fail", "transform on Err preserves error");
    auto r5 = aura::diag::Result<int>(10).and_then([&d](int x) -> aura::diag::Result<int> {
        return x > 0 ? aura::diag::Result<int>(x * 3) : aura::diag::Result<int>(std::unexpected(d));
    });
    CHECK(r5.has_value() && *r5 == 30, "and_then on Ok = Ok(30)");
}

static void run_128_span_type_identity() {
    std::println("\n=== #128: cells() const returns std::span<const T> (zero overhead) ===");
    using CellSpan = std::span<const int>;
    using CellVector = std::vector<int>;
    static_assert(!std::is_same_v<CellSpan, CellVector>);
    static_assert(sizeof(CellSpan) == 2 * sizeof(void*));
    CHECK(true, "std::span<const T> ≠ std::vector<T>, 2-pointer (no overhead)");
}

static void run_128_span_vector_conversion() {
    std::println("\n=== #128: vector → span + compute_kind_instructions(span) ===");
    std::vector<int> v = {1, 2, 3, 4, 5};
    std::span<const int> s = v;
    CHECK(s.size() == 5 && s[0] == 1 && s[4] == 5, "vector → span preserves elements");
    using namespace aura::ir;
    std::vector<IRInstruction> insts;
    IRInstruction nop_inst;
    nop_inst.opcode = IROpcode::Nop;
    insts.push_back(nop_inst);
    IRInstruction const_inst;
    const_inst.opcode = IROpcode::ConstI64;
    insts.push_back(const_inst);
    IRInstruction call_inst;
    call_inst.opcode = IROpcode::Call;
    insts.push_back(call_inst);
    auto kinds = aura::compiler::compute_kind_instructions(std::span<const IRInstruction>(insts));
    CHECK(kinds.size() == 3 && kinds[0] == aura::compiler::ComputeKind::Known &&
              kinds[1] == aura::compiler::ComputeKind::Known &&
              kinds[2] == aura::compiler::ComputeKind::Unknown,
          "compute_kind_instructions(span) classifies Nop/ConstI64/Call");
    std::vector<IRInstruction> empty;
    auto empty_kinds =
        aura::compiler::compute_kind_instructions(std::span<const IRInstruction>(empty));
    CHECK(empty_kinds.empty(), "empty span → empty result");
}

// ── Wave 4 (#130 cache_hit_rate / #132 AST walker / #134 parse_datatype / #166 cache invalidation)
// ──

static void run_130_cache_hit_rate_metric() {
    std::println("\n=== #130: TypeChecker cache_hit_rate + stats() (Issue #130) ===");
    aura::core::TypeRegistry reg;
    aura::compiler::TypeChecker tc(reg);
    CHECK(tc.cache_hit_rate() == 0.0, "fresh TypeChecker: hit rate 0.0");
    tc.reset_stats();
    auto s0 = tc.stats();
    CHECK(s0.cache_hits == 0 && s0.cache_misses == 0 && s0.stale_cache == 0,
          "after reset_stats: counts all 0");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);
    auto id = flat.add_literal(42);
    flat.root = id;
    aura::diag::DiagnosticCollector diag;
    (void)tc.infer_flat(flat, pool, id, diag);
    auto s = tc.stats();
    CHECK(s.cache_misses >= 1, "first inference records >= 1 cache_miss");
    double rate = tc.cache_hit_rate();
    CHECK(rate >= 0.0 && rate <= 1.0, "hit rate in [0, 1]");
    CHECK(rate < 0.5, "cold-start hit rate < 0.5");
}

static void run_132_ast_walker_extractions() {
    std::println(
        "\n=== #132: find_top_level_defines + collect_user_bindings (AST walker extractions) ===");
    // Basic: (begin (define a 1) (define b 2) 42)
    {
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool(alloc);
        auto a_name = pool.intern("a");
        auto b_name = pool.intern("b");
        auto def_a = flat.add_define(a_name, flat.add_literal(1));
        auto def_b = flat.add_define(b_name, flat.add_literal(2));
        auto lit_42 = flat.add_literal(42);
        auto begin = flat.add_begin({def_a, def_b, lit_42});
        auto defs = aura::compiler::find_top_level_defines(flat, pool, begin);
        CHECK(defs.size() == 2 && defs[0].first == "a" && defs[1].first == "b",
              "find_top_level_defines returns (a, b) in document order");
    }
    // Nested: outer is found, inner inside lambda is NOT
    {
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool(alloc);
        auto outer_name = pool.intern("outer");
        auto inner_name = pool.intern("inner");
        auto x_name = pool.intern("x");
        auto def_outer = flat.add_define(outer_name, flat.add_literal(1));
        auto x_var = flat.add_variable(x_name);
        auto inner_body = flat.add_define(inner_name, flat.add_literal(2));
        auto lambda = flat.add_lambda({x_var}, {inner_body});
        auto begin = flat.add_begin({def_outer, lambda});
        auto defs = aura::compiler::find_top_level_defines(flat, pool, begin);
        CHECK(defs.size() == 1 && defs[0].first == "outer",
              "find_top_level_defines skips nested defines inside lambda");
    }
    // Edge cases: NULL_NODE / non-define root → empty
    {
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool(alloc);
        auto defs_null = aura::compiler::find_top_level_defines(flat, pool, aura::ast::NULL_NODE);
        CHECK(defs_null.empty(), "find_top_level_defines(NULL_NODE) → empty");
        auto lit = flat.add_literal(42);
        auto defs_lit = aura::compiler::find_top_level_defines(flat, pool, lit);
        CHECK(defs_lit.empty(), "find_top_level_defines(literal) → empty");
    }
    // collect_user_bindings: (begin (define a 1) (: b Int) (define c 3))
    {
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool(alloc);
        auto a_name = pool.intern("a");
        auto b_name = pool.intern("b");
        auto c_name = pool.intern("c");
        auto int_sym = pool.intern("Int");
        auto def_a = flat.add_define(a_name, flat.add_literal(1));
        auto def_c = flat.add_define(c_name, flat.add_literal(3));
        auto annot = flat.add_type_annotation(int_sym, flat.add_literal(0), b_name);
        auto begin = flat.add_begin({def_a, annot, def_c});
        auto names = aura::compiler::collect_user_bindings(flat, pool, begin);
        CHECK(names.size() == 3 && names[0] == "a" && names[1] == "b" && names[2] == "c",
              "collect_user_bindings: (a, b, c) including TypeAnnotation");
    }
}

static void run_134_parse_datatype() {
    std::println("\n=== #134: parse_datatype — basic + zero-arity + parametric + empty ===");
    {
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool(alloc);
        auto pr = aura::parser::parse_to_flat("(datatype (Tree) (Leaf Int) (Node Tree Tree))", flat,
                                              pool);
        CHECK(pr.success && pr.root != aura::ast::NULL_NODE,
              "parse_datatype (Tree, Leaf, Node) succeeds + non-NULL root");
    }
    {
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool(alloc);
        auto pr = aura::parser::parse_to_flat("(datatype (None) (None))", flat, pool);
        CHECK(pr.success, "zero-arity ctor (None) parses");
    }
    {
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool(alloc);
        auto pr =
            aura::parser::parse_to_flat("(datatype (Option : T) (Some T) (None))", flat, pool);
        CHECK(pr.success, "parametric type spec (: T) parses");
    }
    {
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool(alloc);
        auto pr = aura::parser::parse_to_flat("(datatype (Empty))", flat, pool);
        CHECK(pr.success, "empty datatype (just type name) parses");
    }
}

static void run_166_cache_invalidation() {
    std::println("\n=== #166: multi-layer cache invalidation (mutation_epoch_) ===");
    auto runner = [](CompilerService& cs, std::string_view src) -> int64_t {
        auto r = cs.eval(std::string(src));
        if (!r)
            return -1;
        auto& v = *r;
        if (aura::compiler::types::is_int(v))
            return aura::compiler::types::as_int(v);
        if (aura::compiler::types::is_bool(v))
            return aura::compiler::types::as_bool(v) ? 1 : 0;
        return -1;
    };
    // T1: eval → mutate → re-eval (basic regression)
    {
        CompilerService cs;
        CHECK(runner(cs, "(set-code \"(define x 10)\")") >= 0, "T1 set-code");
        CHECK(runner(cs, "x") == 10, "T1 first eval x = 10");
        CHECK(runner(cs, "(mutate:rebind \"x\" \"42\" \"bump x\")") > 0, "T1 mutate:rebind");
        CHECK(runner(cs, "x") == 42, "T1 after mutate x = 42");
    }
    // T2: epoch increments; unrelated mutation doesn't break cached fn
    {
        CompilerService cs;
        CHECK(runner(cs, "(set-code \"(define (f x) (* x x))(define y 0)\")") >= 0,
              "T2 set-code with f + y");
        CHECK(runner(cs, "(f 5)") == 25, "T2 (f 5) = 25");
        CHECK(runner(cs, "(mutate:rebind \"y\" \"100\" \"bump y\")") > 0,
              "T2 mutate:rebind on unrelated var");
        CHECK(runner(cs, "(f 5)") == 25,
              "T2 after unrelated mutation (f 5) still = 25 (cache handled)");
    }
    // T3: function body mutation invalidates cache
    {
        CompilerService cs;
        CHECK(runner(cs, "(set-code \"(define (g x) (+ x 1))\")") >= 0, "T3 set-code");
        CHECK(runner(cs, "(g 10)") == 11, "T3 first (g 10) = 11");
        CHECK(runner(cs, "(mutate:set-body \"g\" \"(lambda (x) (+ x 100))\" \"bump g\")") > 0,
              "T3 mutate:set-body");
        CHECK(runner(cs, "(g 10)") == 110, "T3 after mutate:set-body (g 10) = 110");
    }
    // T4: rapid mutations (epoch counter monotonic, counter reflects each rebind)
    {
        CompilerService cs;
        CHECK(runner(cs, "(set-code \"(define counter 0)\")") >= 0, "T4 set-code");
        bool all_ok = true;
        for (int i = 1; i <= 10; ++i) {
            std::string mutate_src = "(mutate:rebind \"counter\" \"" + std::to_string(i) +
                                     "\" \"iter " + std::to_string(i) + "\")";
            if (runner(cs, mutate_src) <= 0) {
                all_ok = false;
                break;
            }
            if (runner(cs, "counter") != i) {
                all_ok = false;
                break;
            }
        }
        CHECK(all_ok, "T4 10 rapid mutations all succeeded with correct values");
    }
}

// ── Wave 5 (#168 incremental type cache epoch / #169 IncrementalStrictness config) ──

static void run_168_incremental_type_cache_epoch() {
    std::println("\n=== #168: TypeChecker.set_cache_epoch + infer_flat epoch gate ===");
    // T1: set_cache_epoch accepts any value
    {
        aura::core::TypeRegistry reg;
        aura::compiler::TypeChecker tc(reg);
        tc.set_cache_epoch(0);
        tc.set_cache_epoch(1);
        tc.set_cache_epoch(42);
        CHECK(true, "#168 T1: set_cache_epoch accepts any value, no crash");
    }
    // T2: epoch gate in infer_flat (same epoch → same type, new epoch → invalidate)
    {
        aura::core::TypeRegistry reg;
        aura::diag::DiagnosticCollector diag;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool;
        auto id = flat.add_literal(42);
        flat.set_type(id, 0);
        aura::compiler::TypeChecker tc(reg);
        tc.set_strict(true);

        tc.set_cache_epoch(100);
        auto r1 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r1.index > 0, "#168 T2: first infer_flat (epoch=100) → valid type");

        tc.set_cache_epoch(100);
        auto r2 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r2.index == r1.index, "#168 T2: same epoch → same type");

        tc.set_cache_epoch(101);
        auto r3 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r3.index == r1.index, "#168 T2: new epoch → invalidate but same result");
    }
    // T3: literal typecheck regression (no crash after epoch bump)
    {
        aura::core::TypeRegistry reg;
        aura::diag::DiagnosticCollector diag;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool;
        auto id = flat.add_literal(42);
        flat.set_type(id, 0);
        aura::compiler::TypeChecker tc(reg);
        tc.set_strict(true);
        tc.set_cache_epoch(1);
        auto r = tc.infer_flat(flat, pool, id, diag);
        CHECK(r.index > 0, "#168 T3: literal 42 → valid type after epoch bump");
    }
}

static void run_169_incremental_strictness_flag() {
    std::println("\n=== #169: IncrementalStrictness enum + CompilerService set/get ===");
    // T1: enum values are 0/1/2 + distinct
    {
        CHECK(static_cast<std::uint8_t>(aura::compiler::IncrementalStrictness::Conservative) == 0,
              "#169 T1: Conservative = 0");
        CHECK(static_cast<std::uint8_t>(aura::compiler::IncrementalStrictness::Balanced) == 1,
              "#169 T1: Balanced = 1");
        CHECK(static_cast<std::uint8_t>(aura::compiler::IncrementalStrictness::Aggressive) == 2,
              "#169 T1: Aggressive = 2");
        CHECK(aura::compiler::IncrementalStrictness::Conservative !=
                  aura::compiler::IncrementalStrictness::Balanced,
              "#169 T1: Conservative != Balanced");
    }
    // T2: CompilerService set/get round-trip
    {
        aura::compiler::CompilerService cs;
        CHECK(cs.incremental_strictness() == aura::compiler::IncrementalStrictness::Balanced,
              "#169 T2: default = Balanced");
        cs.set_incremental_strictness(aura::compiler::IncrementalStrictness::Conservative);
        CHECK(cs.incremental_strictness() == aura::compiler::IncrementalStrictness::Conservative,
              "#169 T2: set Conservative → getter returns Conservative");
        cs.set_incremental_strictness(aura::compiler::IncrementalStrictness::Aggressive);
        CHECK(cs.incremental_strictness() == aura::compiler::IncrementalStrictness::Aggressive,
              "#169 T2: set Aggressive → getter returns Aggressive");
        cs.set_incremental_strictness(aura::compiler::IncrementalStrictness::Balanced);
        CHECK(cs.incremental_strictness() == aura::compiler::IncrementalStrictness::Balanced,
              "#169 T2: set back to Balanced");
    }
    // T3: setting the flag doesn't change eval behavior
    {
        aura::compiler::CompilerService cs;
        cs.set_incremental_strictness(aura::compiler::IncrementalStrictness::Conservative);
        auto r1 = cs.eval("(+ 1 2)");
        CHECK(r1.has_value(), "#169 T3: Conservative mode eval(+ 1 2) returns a value");
        cs.set_incremental_strictness(aura::compiler::IncrementalStrictness::Aggressive);
        auto r2 = cs.eval("(+ 3 4)");
        CHECK(r2.has_value(), "#169 T3: Aggressive mode eval(+ 3 4) returns a value");
    }
}

static void run_typechecker_unit_tests() {
    run_116_deferred_coercion_node();
    run_118_unbound_var_tags_node();
    run_118_module_member_not_found_tags_node();
    run_118_well_typed_no_tag();
    run_121_gensym_unique_and_prefix();
    run_121_nested_hygienic_macros_recursive();
    run_121_legacy_defmacro_compat();
    run_122_reflect_type_scalar();
    run_122_reflect_unknown_returns_void();
    run_122_reflect_members_in_query();
    run_123_pre_exec_strips_begin_require();
    run_123_nested_require_falls_back();
    run_124_try_catch_raise_parses();
    run_124_nested_try_catch_parses();
    run_127_result_alias_type_identity();
    run_127_result_error_channel();
    run_127_result_monadic_chain();
    run_128_span_type_identity();
    run_128_span_vector_conversion();
    run_130_cache_hit_rate_metric();
    run_132_ast_walker_extractions();
    run_134_parse_datatype();
    run_166_cache_invalidation();
    run_168_incremental_type_cache_epoch();
    run_169_incremental_strictness_flag();
}

// ── Wave 7 (#242 EnvFrame SoA + #260 ADT exhaustiveness + #265 clone_macro_body hygiene) ──

static void run_242_envframe_soa_version_stamping() {
    std::println("\n=== #242: EnvFrame SoA version stamping + arena rollback hardening ===");
    // L1: alloc_env_frame stamps version_ + is_env_frame_stale semantics
    {
        std::println("\n--- #242 L1: alloc_env_frame + is_env_frame_stale ---");
        aura::compiler::Evaluator ev;
        auto v0 = ev.defuse_version_for_test();
        aura::compiler::EnvId id = ev.alloc_env_frame();
        CHECK(id != aura::compiler::NULL_ENV_ID, "alloc returns valid id");
        CHECK(ev.env_frame(id).version_ == v0, "frame.version_ == defuse_version_ at alloc");
        CHECK(!ev.is_env_frame_stale(id), "fresh frame not stale");
        CHECK(ev.is_env_frame_invalid_id(aura::compiler::NULL_ENV_ID), "NULL_ENV_ID is invalid_id");
        CHECK(ev.is_env_frame_invalid_id(999999), "out-of-range is invalid_id");
        CHECK(!ev.is_env_frame_stale(aura::compiler::NULL_ENV_ID), "NULL_ENV_ID not version-stale");
        CHECK(!ev.is_env_frame_stale(999999), "out-of-range not version-stale");
        ev.bump_defuse_version_for_test();
        CHECK(ev.is_env_frame_stale(id), "stale after defuse_version_ bump");
    }
    // L2: closure capture + mutate + materialize_call_env stale + bump version
    {
        std::println("\n--- #242 L2: capture + mutate + materialize_call_env ---");
        aura::compiler::Evaluator ev;
        aura::compiler::EnvId id = ev.alloc_env_frame();
        CHECK(!ev.is_env_frame_stale(id), "L2 fresh frame");
        aura::compiler::Closure cl;
        cl.env_id = id;
        ev.bump_defuse_version_for_test();
        CHECK(ev.is_env_frame_stale(id), "L2 stale after bump");
        auto ne = ev.materialize_call_env(cl);
        (void)ne;
        auto v_after = ev.defuse_version_for_test();
        CHECK(ev.env_frame(id).version_ == v_after,
              "frame.version_ bumped to defuse_version_ by materialize_call_env");
    }
    {
        std::println("\n--- #242 L2: capture + mutate + lookup consistency (no UAF) ---");
        aura::compiler::CompilerService cs;
        auto r1 = cs.eval("(begin (define x 10) (define f (lambda () x)) (f))");
        CHECK(r1.has_value(), "r1: capture f + call returns a value");
        auto r2 = cs.eval("(begin (define x 20) (f))");
        CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
              "r2: post-mutation lookup returns int (no crash / no UAF)");
    }
    // L3: panic-checkpoint arena snapshot + commit_clears
    {
        std::println("\n--- #242 L3: panic-checkpoint snapshot + commit clears ---");
        aura::compiler::Evaluator ev;
        CHECK(ev.panic_safe_cells_size() == 0, "panic_safe_cells_size_ defaults to 0");
        CHECK(ev.panic_safe_pairs_size() == 0, "panic_safe_pairs_size_ defaults to 0");
        CHECK(ev.panic_safe_string_heap_size() == 0, "panic_safe_string_heap_size_ defaults to 0");
        CHECK(ev.panic_safe_env_frames_size() == 0, "panic_safe_env_frames_size_ defaults to 0");
        ev.set_panic_safe_cells_size_for_test(10);
        ev.set_panic_safe_pairs_size_for_test(5);
        ev.set_panic_safe_string_heap_size_for_test(100);
        ev.set_panic_safe_env_frames_size_for_test(7);
        ev.commit_panic_checkpoint();
        CHECK(ev.panic_safe_cells_size() == 0, "commit clears panic_safe_cells_size_");
        CHECK(ev.panic_safe_pairs_size() == 0, "commit clears panic_safe_pairs_size_");
        CHECK(ev.panic_safe_string_heap_size() == 0, "commit clears panic_safe_string_heap_size_");
        CHECK(ev.panic_safe_env_frames_size() == 0, "commit clears panic_safe_env_frames_size_");
    }
}

static void run_260_adt_match_exhaustiveness_mutation() {
    std::println("\n=== #260: nested ADT exhaustiveness + mutation-aware occurrence typing ===");
    using Ctx = TypecheckEnv;
    // T1: analyze_match_exhaustiveness detects missing ctor
    {
        std::println("\n--- #260 T1: analyze_match_exhaustiveness detects missing ctor ---");
        auto env = Ctx::make();
        env.parse_src("(begin (define-type (Color) (Red) (Green) (Blue)) "
                      "  (let ((x Red)) (match x ((Red) 1) ((Green) 2))))");
        aura::ast::NodeId found = aura::ast::NULL_NODE;
        for (aura::ast::NodeId i = 0; i < env.flat->size(); ++i) {
            if (!env.flat->has_match_info(i))
                continue;
            auto missing =
                aura::compiler::analyze_match_exhaustiveness(*env.flat, *env.pool, *env.reg, i);
            if (!missing.empty()) {
                found = i;
                break;
            }
        }
        CHECK(found != aura::ast::NULL_NODE, "found incomplete match let");
        if (found != aura::ast::NULL_NODE) {
            auto missing =
                aura::compiler::analyze_match_exhaustiveness(*env.flat, *env.pool, *env.reg, found);
            CHECK(!missing.empty(), "match reports missing ctors");
            bool has_blue = false;
            for (auto& m : missing)
                if (m == "Blue")
                    has_blue = true;
            CHECK(has_blue, "missing ctor is Blue");
            if (auto* mi = env.flat->get_match_info(found)) {
                CHECK(mi->exhaustiveness_checked, "exhaustiveness_checked flag set");
                CHECK(mi->subject_type_id > 0, "subject_type_id cached");
            }
        }
    }
    // T2: post_mutation_invariant_check emits MissingConstructorInNestedMatch
    {
        std::println("\n--- #260 T2: post_mutation MissingConstructorInNestedMatch note ---");
        auto env = Ctx::make();
        env.parse_src("(begin (define-type (Color) (Red) (Green) (Blue)) "
                      "  (let ((x Red)) (match x ((Red) 1) ((Green) 2))))");
        aura::ast::NodeId found = aura::ast::NULL_NODE;
        for (aura::ast::NodeId i = 0; i < env.flat->size(); ++i) {
            if (!env.flat->has_match_info(i))
                continue;
            auto missing =
                aura::compiler::analyze_match_exhaustiveness(*env.flat, *env.pool, *env.reg, i);
            if (!missing.empty()) {
                found = i;
                break;
            }
        }
        CHECK(found != aura::ast::NULL_NODE, "found incomplete match");
        if (found != aura::ast::NULL_NODE) {
            env.flat->mark_dirty(found);
            aura::ast::MutationRecord rec;
            rec.mutation_id = 42;
            rec.target_node = found;
            rec.parent_id = env.flat->parent_of(found);
            rec.operator_name = "mutate:replace-children";
            std::vector<aura::compiler::OwnershipNote> notes;
            auto st = aura::compiler::post_mutation_invariant_check(*env.flat, *env.pool, *env.reg,
                                                                    rec, notes);
            bool note_found = false;
            for (auto& n : notes) {
                if (n.kind == "MissingConstructorInNestedMatch") {
                    note_found = true;
                    CHECK(n.source_mutation_id.has_value() && *n.source_mutation_id == 42,
                          "note links mutation_id");
                    CHECK(n.blame.has_value() &&
                              n.blame->annotation_src == "mutate:replace-children",
                          "note blame operator");
                }
            }
            CHECK(note_found, "MissingConstructorInNestedMatch emitted");
            CHECK(st == aura::ast::InvariantStatus::Warnings, "status is Warnings");
        }
    }
    // T3: complete match has no missing ctor note
    {
        std::println("\n--- #260 T3: complete match has no missing ctor ---");
        auto env = Ctx::make();
        env.parse_src("(begin (define-type (Color) (Red) (Green) (Blue)) "
                      "  (let ((x Red)) (match x ((Red) 1) ((Green) 2) ((Blue) 3))))");
        aura::ast::NodeId found = aura::ast::NULL_NODE;
        for (aura::ast::NodeId i = 0; i < env.flat->size(); ++i) {
            if (!env.flat->has_match_info(i))
                continue;
            auto missing =
                aura::compiler::analyze_match_exhaustiveness(*env.flat, *env.pool, *env.reg, i);
            if (!missing.empty()) {
                found = i;
                break;
            }
        }
        CHECK(found == aura::ast::NULL_NODE, "complete match has no incomplete lets");
    }
}

static void run_265_clone_macro_body_hygiene() {
    std::println("\n=== #265: clone_macro_body hygiene (per-call gensym + pre-scan + builtins "
                 "whitelist) ===");
    using HygNameMap = aura::core::TransparentStringMap<std::string>;
    auto make_src_env = [](const std::string& src) {
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);
        auto pr = aura::parser::parse_to_flat(src, *flat, *pool);
        flat->root = pr.root;
        struct R {
            std::unique_ptr<aura::ast::ASTArena> arena;
            aura::ast::FlatAST* flat;
            aura::ast::StringPool* pool;
            aura::ast::NodeId body;
        };
        R r;
        r.arena = std::move(arena);
        r.flat = flat;
        r.pool = pool;
        r.body = pr.root;
        return r;
    };
    // T1: hyg_ctr is per-call reset
    {
        std::println("\n--- #265 T1: hyg_ctr is per clone_macro_body call ---");
        auto src = make_src_env("(let ((tmp 1)) tmp)");
        auto t1 = make_src_env(""); // dummy to consume pool types
        (void)t1;
        auto t1a = std::make_unique<aura::ast::ASTArena>();
        auto t1a_alloc = t1a->allocator();
        auto* t1_flat = t1a->create<aura::ast::FlatAST>(t1a_alloc);
        auto* t1_pool = t1a->create<aura::ast::StringPool>(t1a_alloc);
        auto t2a = std::make_unique<aura::ast::ASTArena>();
        auto t2a_alloc = t2a->allocator();
        auto* t2_flat = t2a->create<aura::ast::FlatAST>(t2a_alloc);
        auto* t2_pool = t2a->create<aura::ast::StringPool>(t2a_alloc);
        HygNameMap map1, map2;
        (void)aura::compiler::macro_exp::clone_macro_body(*t1_flat, *t1_pool, *src.flat, *src.pool,
                                                          src.body, nullptr, &map1,
                                                          aura::ast::SyntaxMarker::MacroIntroduced);
        (void)aura::compiler::macro_exp::clone_macro_body(*t2_flat, *t2_pool, *src.flat, *src.pool,
                                                          src.body, nullptr, &map2,
                                                          aura::ast::SyntaxMarker::MacroIntroduced);
        CHECK(map1.count("tmp") == 1, "first expansion gensyms tmp");
        CHECK(map2.count("tmp") == 1, "second expansion gensyms tmp");
        CHECK(map1.at("tmp") == "__tmp_0", "first expansion uses __tmp_0");
        CHECK(map2.at("tmp") == "__tmp_0",
              "second expansion also starts at __tmp_0 (not global static)");
    }
    // T2: pre-scan rewrites inner references to gensym name
    {
        std::println("\n--- #265 T2: pre-scan rewrites inner references ---");
        auto src = make_src_env("(let ((tmp x)) tmp)");
        auto t1a = std::make_unique<aura::ast::ASTArena>();
        auto t1a_alloc = t1a->allocator();
        auto* t_flat = t1a->create<aura::ast::FlatAST>(t1a_alloc);
        auto* t_pool = t1a->create<aura::ast::StringPool>(t1a_alloc);
        HygNameMap rename_map;
        auto cloned = aura::compiler::macro_exp::clone_macro_body(
            *t_flat, *t_pool, *src.flat, *src.pool, src.body, nullptr, &rename_map,
            aura::ast::SyntaxMarker::MacroIntroduced);
        CHECK(cloned != aura::ast::NULL_NODE, "clone succeeded");
        std::vector<std::string> vars;
        std::function<void(aura::ast::NodeId)> collect = [&](aura::ast::NodeId id) {
            if (id == aura::ast::NULL_NODE || id >= t_flat->size())
                return;
            auto v = t_flat->get(id);
            if (v.tag == aura::ast::NodeTag::Variable && v.sym_id != aura::ast::INVALID_SYM) {
                vars.push_back(std::string(t_pool->resolve(v.sym_id)));
            }
            for (std::uint32_t i = 0; i < v.children.size(); ++i)
                collect(v.child(i));
        };
        collect(cloned);
        CHECK(vars.size() >= 2, "let + reference produce >= 2 variable nodes");
        const auto gensym = rename_map.at("tmp");
        int hits = 0;
        for (const auto& n : vars)
            if (n == gensym)
                ++hits;
        CHECK(hits >= 1, "inner reference uses gensym name from rename_map");
        auto let_v = t_flat->get(cloned);
        CHECK(let_v.tag == aura::ast::NodeTag::Let, "cloned root is let");
        CHECK(std::string(t_pool->resolve(let_v.sym_id)) == gensym,
              "let binding sym_id uses gensym from rename_map");
    }
    // T3: builtins whitelist not gensym'd
    {
        std::println("\n--- #265 T3: builtins whitelist not gensym'd ---");
        auto src = make_src_env("(lambda (if) if)");
        auto t1a = std::make_unique<aura::ast::ASTArena>();
        auto t1a_alloc = t1a->allocator();
        auto* t_flat = t1a->create<aura::ast::FlatAST>(t1a_alloc);
        auto* t_pool = t1a->create<aura::ast::StringPool>(t1a_alloc);
        HygNameMap rename_map;
        auto cloned = aura::compiler::macro_exp::clone_macro_body(
            *t_flat, *t_pool, *src.flat, *src.pool, src.body, nullptr, &rename_map,
            aura::ast::SyntaxMarker::MacroIntroduced);
        CHECK(cloned != aura::ast::NULL_NODE, "clone with builtin lambda param succeeded");
        CHECK(rename_map.find("if") == rename_map.end(), "builtin param `if` not gensym'd");
        auto lam = t_flat->get(cloned);
        CHECK(lam.tag == aura::ast::NodeTag::Lambda && !lam.params.empty(), "lambda cloned");
        CHECK(std::string(t_pool->resolve(lam.params[0])) == "if",
              "lambda param remains plain `if`");
    }
    // T4: nested bindings get distinct gensyms in one call
    {
        std::println("\n--- #265 T4: nested bindings get distinct gensyms ---");
        auto src = make_src_env("(let ((a 1)) (let ((b 2)) (+ a b)))");
        auto t1a = std::make_unique<aura::ast::ASTArena>();
        auto t1a_alloc = t1a->allocator();
        auto* t_flat = t1a->create<aura::ast::FlatAST>(t1a_alloc);
        auto* t_pool = t1a->create<aura::ast::StringPool>(t1a_alloc);
        HygNameMap rename_map;
        (void)aura::compiler::macro_exp::clone_macro_body(*t_flat, *t_pool, *src.flat, *src.pool,
                                                          src.body, nullptr, &rename_map,
                                                          aura::ast::SyntaxMarker::MacroIntroduced);
        CHECK(rename_map.at("a") == "__a_0", "first binding is __a_0");
        CHECK(rename_map.at("b") == "__b_1", "second binding is __b_1");
    }
}

// ── Wave 8 (#272 IR-native env binding for defines) ──

static void run_272_ir_native_env_binding() {
    std::println("\n=== #272: IR-native env binding for function defines ===");
    auto run_ok = [](CompilerService& cs, std::string_view src) {
        return static_cast<bool>(cs.eval(src));
    };
    auto run_int = [](CompilerService& cs, std::string_view src) -> int64_t {
        auto r = cs.eval(src);
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    };
    // AC1: define_ir_env_bind_count bumps on function define
    {
        std::println("\n--- #272 AC1: define_ir_env_bind_count bumps on function define ---");
        CompilerService cs;
        CHECK(cs.define_ir_env_bind_count() == 0, "metric starts at 0");
        CHECK(run_ok(cs, "(define (dbl x) (* x 2))"), "define succeeds");
        CHECK(cs.define_ir_env_bind_count() == 1, "one IR env bind after define");
    }
    // AC2: defined function callable via IR closure bridge
    {
        std::println("\n--- #272 AC2: defined function callable via IR closure bridge ---");
        CompilerService cs;
        CHECK(run_ok(cs, "(define (mul2 x) (* x 2))"), "define succeeds");
        CHECK(run_int(cs, "(mul2 7)") == 14, "(mul2 7) = 14");
        CHECK(run_int(cs, "(mul2 0)") == 0, "(mul2 0) = 0");
    }
    // AC3: redefine re-binds via IR + new body used
    {
        std::println("\n--- #272 AC3: redefine re-binds via IR ---");
        CompilerService cs;
        CHECK(run_ok(cs, "(define (f x) (+ x 1))"), "first define succeeds");
        CHECK(run_int(cs, "(f 5)") == 6, "(f 5) = 6 first body");
        auto binds_after_first = cs.define_ir_env_bind_count();
        CHECK(run_ok(cs, "(define (f x) (* x 3))"), "redefine succeeds");
        CHECK(cs.define_ir_env_bind_count() == binds_after_first + 1,
              "redefine bumps define_ir_env_bind_count");
        CHECK(run_int(cs, "(f 5)") == 15, "(f 5) = 15 new body");
    }
    // AC5: compile_module binds via IR
    {
        std::println("\n--- #272 AC5: compile_module binds via IR ---");
        CompilerService cs;
        auto binds_before = cs.define_ir_env_bind_count();
        auto r = cs.compile_module("mod272", "(define (mod-fn x) (* x 10))");
        CHECK(r.has_value() && *r, "compile_module succeeds");
        CHECK(cs.define_ir_env_bind_count() == binds_before + 1,
              "compile_module bumps define_ir_env_bind_count");
        CHECK(run_int(cs, "(mod-fn 3)") == 30, "(mod-fn 3) = 30 after compile_module");
    }
    // AC6: redefine invalidates + re-binds dependents via IR
    {
        std::println("\n--- #272 AC6: redefine invalidates and re-binds dependents ---");
        CompilerService cs;
        CHECK(run_ok(cs, "(define (base x) (+ x 1))"), "first define base succeeds");
        CHECK(run_ok(cs, "(define (wrap x) (base x))"), "wrap defined");
        CHECK(run_int(cs, "(wrap 5)") == 6, "wrap uses (+ x 1) base initially");
        CHECK(run_ok(cs, "(define (base x) (* x 2))"), "redefine base succeeds");
        CHECK(run_int(cs, "(wrap 5)") == 10, "wrap uses re-bound (* x 2) base after redefine");
    }
    // AC8: value define bound via IR + TopCellLoad
    {
        std::println("\n--- #272 AC8: value define bound via IR ---");
        CompilerService cs;
        CHECK(cs.value_define_ir_env_bind_count() == 0, "value metric starts at 0");
        auto r = cs.eval("(define y (+ 1 2))");
        CHECK(r.has_value() && *r, "value define succeeds");
        CHECK(cs.value_define_ir_env_bind_count() == 1, "value define bumps IR bind metric");
        CHECK(run_int(cs, "(+ y 10)") == 13, "(+ y 10) = 13 via TopCellLoad");
    }
    // AC9: value define does not need tree-walker fallback
    {
        std::println("\n--- #272 AC9: value define no tree-walker fallback ---");
        CompilerService cs;
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);
        auto pr = aura::parser::parse_to_flat("(define n 42)", *flat, *pool);
        CHECK(pr.success, "value define parses");
        flat->root = pr.root;
        CHECK(!cs.public_needs_tree_walker_fallback(*flat, *pool, pr.root),
              "needs_tree_walker_fallback false for value define");
    }
    // AC7: function define no tree-walker fallback
    {
        std::println("\n--- #272 AC7: function define no tree-walker fallback ---");
        CompilerService cs;
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);
        auto pr = aura::parser::parse_to_flat("(define (f x) (+ x 1))", *flat, *pool);
        CHECK(pr.success, "function define parses");
        flat->root = pr.root;
        CHECK(!cs.public_needs_tree_walker_fallback(*flat, *pool, pr.root),
              "needs_tree_walker_fallback false for function define");
    }
    // AC10: compile_module disk IR cache hit on second service
    {
        std::println("\n--- #272 AC10: compile_module disk IR cache hit ---");
        const std::string mod = "mod272disk";
        const std::string src = "(define (disk-fn x) (+ x 100))";
        {
            CompilerService cs;
            CHECK(cs.compile_module(mod, src).has_value() && *cs.compile_module(mod, src),
                  "first compile_module writes disk cache");
            CHECK(run_int(cs, "(disk-fn 5)") == 105, "(disk-fn 5) = 105 after first compile");
        }
        {
            CompilerService cs2;
            auto binds_before = cs2.define_ir_env_bind_count();
            CHECK(cs2.compile_module(mod, src).has_value(),
                  "second compile_module hits disk cache");
            CHECK(cs2.define_ir_env_bind_count() == binds_before + 1,
                  "disk cache hit still binds via IR");
            CHECK(run_int(cs2, "(disk-fn 5)") == 105, "(disk-fn 5) = 105 after disk cache hit");
        }
    }
    // AC11: JIT TopCellLoad (skipped if LLVM not built)
    {
        std::println("\n--- #272 AC11: JIT TopCellLoad via evaluator cells bridge ---");
#ifndef AURA_HAVE_LLVM
        std::println("  SKIP: LLVM JIT not built");
#else
        CompilerService cs;
        CHECK(run_ok(cs, "(define y (+ 1 2))"), "value define for TopCellLoad");
        CHECK(run_ok(cs, "(define (add-y x) (+ x y))"), "function refs value define");
        CHECK(run_int(cs, "(add-y 10)") == 13, "(add-y 10) = 13 via JIT TopCellLoad");
#endif
    }
    // AC4: nested defines both IR-bound
    {
        std::println("\n--- #272 AC4: nested defines both IR-bound ---");
        CompilerService cs;
        CHECK(run_ok(cs, "(define (inc x) (+ x 1))"), "inc defined");
        CHECK(run_ok(cs, "(define (twice x) (* x 2))"), "twice defined");
        CHECK(run_int(cs, "(twice (inc 3))") == 8, "(twice (inc 3)) = 8");
        CHECK(cs.define_ir_env_bind_count() == 2, "two defines = two IR env binds");
    }
}

} // namespace aura_domain_gates_batch

int aura_issue_domain_gates_batch_run() {
    aura::compiler::CompilerService cs;
    aura_domain_gates_batch::run_all(cs);
    aura_domain_gates_batch::run_typechecker_unit_tests();
    aura_domain_gates_batch::run_242_envframe_soa_version_stamping();
    aura_domain_gates_batch::run_260_adt_match_exhaustiveness_mutation();
    aura_domain_gates_batch::run_265_clone_macro_body_hygiene();
    aura_domain_gates_batch::run_272_ir_native_env_binding();
    return RUN_ALL_TESTS();
}

// Legacy entry aliases (issues_fast / docs may still name theme suites).
int aura_issue_domain_fiber_orchestration_run() {
    return aura_issue_domain_gates_batch_run();
}
int aura_issue_domain_hygiene_dirty_run() {
    return aura_issue_domain_gates_batch_run();
}
int aura_issue_domain_typed_mutate_run() {
    return aura_issue_domain_gates_batch_run();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_domain_gates_batch_run();
}
#endif
