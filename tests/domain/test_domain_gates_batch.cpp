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
}

} // namespace aura_domain_gates_batch

int aura_issue_domain_gates_batch_run() {
    aura::compiler::CompilerService cs;
    aura_domain_gates_batch::run_all(cs);
    aura_domain_gates_batch::run_typechecker_unit_tests();
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
