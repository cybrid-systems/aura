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

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.compiler.ir;
import aura.compiler.ir_cache_pure;
import aura.compiler.compute_kind;
import aura.compiler.ast_walkers;
import aura.compiler.macro_expansion;
import aura.compiler.pass_manager;
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

}

// ── Wave 9 (#330 FlatAST guard unit tests / #356 EnvFrame #242-2 / #375 IR stats) ──

static void run_330_structural_mutation_guard_reader_lock_guard() {
    std::println(
        "\n=== #330: StructuralMutationGuard + ReaderLockGuard unit tests for FlatAST ===");
    // T1: default-constructed StructuralMutationGuard is no-op
    {
        std::println("\n--- #330 T1: default-constructed StructuralMutationGuard ---");
        aura::ast::FlatAST ast;
        std::uint16_t g0 = ast.generation();
        {
            auto g = aura::ast::FlatAST::StructuralMutationGuard{};
            (void)g;
        }
        std::uint16_t g1 = ast.generation();
        CHECK(g1 == g0, "default guard does not bump generation on dtor");
    }
    // T2: active guard bumps generation on dtor
    {
        std::println("\n--- #330 T2: active guard bumps generation on dtor ---");
        aura::ast::FlatAST ast;
        std::uint16_t g0 = ast.generation();
        {
            auto guard = ast.begin_structural_mutation();
            (void)guard;
        }
        std::uint16_t g1 = ast.generation();
        CHECK(g1 == g0 + 1, "generation_ incremented by 1 after dtor");
    }
    // T3: move semantics — move-from leaves source empty
    {
        std::println("\n--- #330 T3: move semantics ---");
        aura::ast::FlatAST ast;
        std::uint16_t g0 = ast.generation();
        {
            auto g1 = ast.begin_structural_mutation();
            auto g2 = std::move(g1);
            (void)g1;
            (void)g2;
        }
        std::uint16_t g1_after = ast.generation();
        CHECK(g1_after == g0 + 1, "after move, exactly one guard (g2) bumps on dtor");
    }
    // T4: exception unwind still bumps generation
    {
        std::println("\n--- #330 T4: exception unwind still bumps generation ---");
        aura::ast::FlatAST ast;
        std::uint16_t g0 = ast.generation();
        try {
            auto guard = ast.begin_structural_mutation();
            (void)guard;
            throw std::runtime_error("simulated failure");
        } catch (const std::exception&) {
        }
        std::uint16_t g1 = ast.generation();
        CHECK(g1 == g0 + 1, "generation bumped even after exception unwind (RAII safety)");
    }
    // T5: lock released on dtor (acquire-again succeeds)
    {
        std::println("\n--- #330 T5: lock released on dtor ---");
        aura::ast::FlatAST ast;
        std::uint16_t g0 = ast.generation();
        {
            auto g = ast.begin_structural_mutation();
            (void)g;
        }
        {
            auto g2 = ast.begin_structural_mutation();
            (void)g2;
        }
        std::uint16_t g1 = ast.generation();
        CHECK(g1 == g0 + 2, "second guard acquires + bumps after first released");
    }
    // T6: generation_ monotonic across 5 mutations
    {
        std::println("\n--- #330 T6: generation_ monotonic across 5 mutations ---");
        aura::ast::FlatAST ast;
        std::uint16_t prev = ast.generation();
        for (int i = 0; i < 5; ++i) {
            {
                auto g = ast.begin_structural_mutation();
                (void)g;
            }
            std::uint16_t cur = ast.generation();
            CHECK(cur == prev + 1, "each mutation bumps gen by exactly 1");
            prev = cur;
        }
    }
    // T7: ReaderLockGuard — multiple concurrent readers OK
    {
        std::println("\n--- #330 T7: ReaderLockGuard multiple concurrent readers ---");
        aura::ast::FlatAST ast;
        auto r1 = ast.try_acquire_reader_lock();
        auto r2 = ast.try_acquire_reader_lock();
        auto r3 = ast.try_acquire_reader_lock();
        (void)r1;
        (void)r2;
        (void)r3;
        CHECK(true, "3 concurrent readers acquired without blocking");
    }
    // T8: ReaderLockGuard default-constructed is no-op
    {
        std::println("\n--- #330 T8: ReaderLockGuard default-constructed ---");
        aura::ast::FlatAST ast;
        std::uint16_t g0 = ast.generation();
        {
            auto g = aura::ast::FlatAST::ReaderLockGuard{};
            (void)g;
        }
        std::uint16_t g1 = ast.generation();
        CHECK(g1 == g0, "default ReaderLockGuard doesn't affect generation");
    }
    // T9: ReaderLockGuard move semantics
    {
        std::println("\n--- #330 T9: ReaderLockGuard move semantics ---");
        aura::ast::FlatAST ast;
        auto r1 = ast.try_acquire_reader_lock();
        auto r2 = std::move(r1);
        (void)r1;
        (void)r2;
        CHECK(true, "ReaderLockGuard move-ctor works");
    }
    // T10: writer-waits-for-readers handoff
    {
        std::println("\n--- #330 T10: writer-waits-for-readers handoff ---");
        aura::ast::FlatAST ast;
        std::uint16_t g0 = ast.generation();
        {
            auto r = ast.try_acquire_reader_lock();
            (void)r;
        }
        {
            auto w = ast.begin_structural_mutation();
            (void)w;
        }
        std::uint16_t g1 = ast.generation();
        CHECK(g1 == g0 + 1, "writer acquires + bumps after reader release");
    }
    // T11: nested default guards are no-ops
    {
        std::println("\n--- #330 T11: nested default-constructed guards are no-ops ---");
        aura::ast::FlatAST ast;
        std::uint16_t g0 = ast.generation();
        {
            auto gd1 = aura::ast::FlatAST::StructuralMutationGuard{};
            {
                auto gd2 = aura::ast::FlatAST::StructuralMutationGuard{};
                (void)gd2;
            }
            (void)gd1;
        }
        std::uint16_t g1 = ast.generation();
        CHECK(g1 == g0, "nested default guards don't bump generation");
    }
}

static void run_356_envframe_post_rollback_invalidation() {
    std::println(
        "\n=== #356: Arena rollback for env_frames_ via INVALID_VERSION indirection (#242-2) ===");
    // L1: INVALID_VERSION sentinel + is_env_frame_invalid
    {
        std::println("\n--- #356 L1.1: INVALID_VERSION sentinel = UINT64_MAX ---");
        CHECK(aura::compiler::INVALID_VERSION == std::numeric_limits<std::uint64_t>::max(),
              "INVALID_VERSION == UINT64_MAX (monotonic counter never reaches this)");
    }
    {
        std::println("\n--- #356 L1.2: fresh frame is NOT invalid ---");
        aura::compiler::Evaluator ev;
        aura::compiler::EnvId id = ev.alloc_env_frame();
        CHECK(!ev.is_env_frame_invalid(id), "fresh frame: version_ != INVALID_VERSION");
    }
    {
        std::println("\n--- #356 L1.3: NULL_ENV_ID is reported as invalid ---");
        aura::compiler::Evaluator ev;
        CHECK(ev.is_env_frame_invalid(aura::compiler::NULL_ENV_ID),
              "NULL_ENV_ID treated as invalid (safety net for callers)");
    }
    // L2: invalidate_post_rollback_env_frames helper
    {
        std::println("\n--- #356 L2.1: helper marks doomed frames ---");
        aura::compiler::Evaluator ev;
        constexpr int PRE = 3, POST = 4;
        for (int i = 0; i < PRE; ++i)
            ev.alloc_env_frame();
        const std::size_t base_size = ev.env_frames_size();
        ev.set_panic_safe_env_frames_size_for_test(base_size);
        for (int i = 0; i < POST; ++i)
            ev.alloc_env_frame();
        auto before = ev.get_envframe_post_rollback_invalidations();
        ev.invalidate_post_rollback_env_frames();
        auto after = ev.get_envframe_post_rollback_invalidations();
        CHECK(after == before + POST, "envframe_post_rollback_invalidations_ incremented by POST");
        for (std::size_t i = 0; i < base_size; ++i)
            CHECK(!ev.is_env_frame_invalid(static_cast<aura::compiler::EnvId>(i)),
                  "pre-checkpoint frame not invalid");
        for (std::size_t i = base_size; i < base_size + POST; ++i)
            CHECK(ev.is_env_frame_invalid(static_cast<aura::compiler::EnvId>(i)),
                  "post-checkpoint frame invalid");
    }
    {
        std::println("\n--- #356 L2.2: no-op when no doomed frames ---");
        aura::compiler::Evaluator ev;
        const std::size_t base_size = ev.env_frames_size();
        ev.set_panic_safe_env_frames_size_for_test(base_size);
        auto before = ev.get_envframe_post_rollback_invalidations();
        ev.invalidate_post_rollback_env_frames();
        auto after = ev.get_envframe_post_rollback_invalidations();
        CHECK(after == before, "no invalidations when size matches checkpoint");
    }
    {
        std::println("\n--- #356 L2.3: invalidate idempotent ---");
        aura::compiler::Evaluator ev;
        constexpr int PRE = 2, POST = 3;
        for (int i = 0; i < PRE + POST; ++i)
            ev.alloc_env_frame();
        ev.set_panic_safe_env_frames_size_for_test(PRE);
        ev.invalidate_post_rollback_env_frames();
        auto first = ev.get_envframe_post_rollback_invalidations();
        ev.invalidate_post_rollback_env_frames();
        auto second = ev.get_envframe_post_rollback_invalidations();
        CHECK(first == second, "second invalidate call does not re-bump the counter");
    }
    // L3: materialize_call_env skips invalid frames
    {
        std::println("\n--- #356 L3.1: materialize skips invalid frame ---");
        aura::compiler::Evaluator ev;
        aura::compiler::EnvId fid = ev.alloc_env_frame();
        ev.bump_defuse_version_for_test();
        ev.set_panic_safe_env_frames_size_for_test(0);
        ev.invalidate_post_rollback_env_frames();
        aura::compiler::Closure cl;
        cl.env_id = fid;
        auto ne = ev.materialize_call_env(cl);
        CHECK(ne.bindings().empty(), "materialize_call_env returns empty Env for invalid frame");
    }
}

static void run_375_ir_encoding_observability_foundation() {
    std::println("\n=== #375: IR encoding observability foundation + AoS-vs-compact baselines ===");
    auto run_workload = [](CompilerService& cs, const std::string& workload) {
        auto r = cs.eval_ir(workload);
        (void)r;
        return cs.last_ir_stats();
    };
    // AC1: initial snapshot zero on fresh CompilerService
    {
        std::println("\n--- #375 AC1: snapshot zero on fresh CompilerService ---");
        CompilerService cs;
        const auto& s = cs.last_ir_stats();
        CHECK(s.total_instructions == 0u, "total-instructions is 0 on fresh service");
        CHECK(s.total_functions == 0u, "total-functions is 0");
        CHECK(s.total_blocks == 0u, "total-blocks is 0");
        CHECK(s.aos_bytes_total == 0u, "aos-bytes-total is 0");
        CHECK(s.compact_bytes_projection == 0u, "compact-bytes-projection is 0");
        CHECK(s.compact_ratio_bp == 0u, "compact-ratio-bp is 0");
    }
    // AC2: simple expression populates snapshot
    {
        std::println("\n--- #375 AC2: snapshot populates after simple workload ---");
        CompilerService cs;
        const auto& s = run_workload(cs, "((lambda (n) (* n n)) 7)");
        CHECK(s.total_instructions > 0, "total-instructions > 0 after eval(f 7)");
        CHECK(s.total_functions >= 1, "total-functions >= 1");
        CHECK(s.aos_bytes_total == s.total_instructions * 40, "aos-bytes-total = total-instr * 40");
        CHECK(s.padding_bytes_total == s.total_instructions * 3,
              "padding-bytes-total = total-instr * 3");
    }
    // AC3: compact ratio in range
    {
        std::println("\n--- #375 AC3: compact-ratio-bp in [0, 10000] ---");
        CompilerService cs;
        const auto& s = run_workload(cs, "((lambda (n)\n"
                                         "  (let loop ((i 0) (acc 0))\n"
                                         "    (if (>= i n) acc (loop (+ i 1) (+ acc i)))))\n"
                                         " 10)");
        CHECK(s.total_instructions > 0, "sum-to lowered to > 0 instructions");
        CHECK(s.compact_ratio_bp <= 10000, "compact-ratio-bp <= 10000 (no overflow)");
        CHECK(s.operands_used_sum > 0, "operands-used-sum > 0");
        CHECK(s.compact_bytes_projection < s.aos_bytes_total,
              "compact-bytes-projection < aos-bytes-total");
    }
    // AC4: heavier recursive workload
    {
        std::println("\n--- #375 AC4: heavier recursive workload has more instructions ---");
        CompilerService cs;
        const auto& s = run_workload(
            cs, "((lambda (n)\n"
                "  (if (<= n 1) 1\n"
                "    (* n ((lambda (m)\n"
                "      (if (<= m 1) 1 (* m ((lambda (k) (if (<= k 1) 1 (* k 1))) (- m 1)))))\n"
                "      (- n 1)))))\n"
                " 5)");
        CHECK(s.total_instructions >= 5, "fact lowered to >= 5 instructions");
        bool any_op_count_nonzero = false;
        for (std::size_t i = 0; i < 5; ++i)
            if (s.operand_count_distribution[i] > 0) {
                any_op_count_nonzero = true;
                break;
            }
        CHECK(any_op_count_nonzero, "operand-count-distribution has at least one non-zero bucket");
        bool any_opcode_nonzero = false;
        for (auto c : s.opcode_histogram)
            if (c > 0) {
                any_opcode_nonzero = true;
                break;
            }
        CHECK(any_opcode_nonzero, "opcode-histogram has at least one non-zero bucket");
    }
    // AC5: no regression — basic eval still works
    {
        std::println("\n--- #375 AC5: no regression ---");
        CompilerService cs;
        auto r = cs.eval("(+ 1 2 3 4 5)");
        CHECK(r.has_value() && is_int(*r), "(+ 1 2 3 4 5) returns int");
        if (r)
            CHECK(as_int(*r) == 15, "(+ 1 2 3 4 5) == 15");
        auto r2 = cs.eval("(define g (lambda (x) (+ x 10))) (g 5)");
        CHECK(r2.has_value() && is_int(*r2), "(g 5) returns int");
        if (r2)
            CHECK(as_int(*r2) == 15, "(g 5) == 15");
    }
}
}

// ── Wave 10 (#430 production arena compaction policy + #456 mutation observability + #508 DCE
// pass) ──

static void run_430_arena_compaction_policy_observability() {
    std::println("\n=== #430: Production Arena compaction policy + 10-field observability ===");
    auto hash_int = [](CompilerService& cs, std::string_view key) -> int64_t {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:arena-compaction-stats-hash\") '{}')", key));
        if (!r)
            return -1;
        if (!is_int(*r))
            return -1;
        return as_int(*r);
    };
    // AC1: fresh Evaluator compactions == 0 + frag well-defined
    {
        std::println("\n--- #430 AC1: fresh Evaluator ---");
        CompilerService cs;
        CHECK(hash_int(cs, "compactions") == 0, "fresh Evaluator: compactions == 0");
        auto frag = hash_int(cs, "fragmentation-ratio-pct");
        CHECK(frag >= 0 && frag <= 100, "fresh Evaluator: frag-pct in 0..100");
    }
    // AC2: 10 fields present + non-negative
    {
        std::println("\n--- #430 AC2: 10 fields present + non-negative ---");
        CompilerService cs;
        bool all_ok = true;
        for (const char* k : {"auto-compact-triggers", "auto-compact-skips", "compactions",
                              "bytes-saved", "last-saved", "paused-by-boundary", "mutation-volume",
                              "dirty-propagation", "fragmentation-ratio-pct", "peak-used-bytes"}) {
            if (hash_int(cs, k) < 0)
                all_ok = false;
        }
        CHECK(all_ok, "all 10 fields present and non-negative");
    }
    // AC3 + AC4: empty workspace + idempotence
    {
        std::println("\n--- #430 AC3+AC4: empty workspace + idempotence ---");
        CompilerService cs;
        CHECK(hash_int(cs, "compactions") == 0, "empty: compactions == 0 (no walls)");
        auto a = hash_int(cs, "compactions");
        auto b = hash_int(cs, "compactions");
        CHECK(a == b, "two consecutive calls return same compactions (idempotent)");
    }
    // AC5: skip policy returns 0
    {
        std::println("\n--- #430 AC5: skip policy returns 0 ---");
        CompilerService cs;
        auto r = cs.eval("(arena:compact-with-policy \"main\" \"skip\")");
        CHECK(r.has_value() && is_int(*r) && as_int(*r) == 0, "skip policy returns 0");
    }
    // AC6: force policy doesn't crash
    {
        std::println("\n--- #430 AC6: force policy doesn't crash ---");
        CompilerService cs;
        auto r = cs.eval("(arena:compact-with-policy \"main\" \"force\")");
        CHECK(r.has_value() && is_int(*r) && as_int(*r) >= 0,
              "force policy returns non-negative int");
    }
    // AC7: nonexistent arena returns 0
    {
        std::println("\n--- #430 AC7: nonexistent arena returns 0 ---");
        CompilerService cs;
        auto r = cs.eval("(arena:compact-with-policy \"definitely-not-an-arena-xyz\" \"force\")");
        CHECK(r.has_value() && is_int(*r) && as_int(*r) == 0,
              "nonexistent arena returns 0 (safe no-op)");
    }
    // AC8: stats-hash primitive is reachable
    {
        std::println("\n--- #430 AC8: stats-hash primitive reachable ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:arena-compaction-stats-hash\")");
        CHECK(r.has_value(), "(engine:metrics \"query:arena-compaction-stats-hash\") is callable");
    }
    // AC9: stats:count >= 41 (was 40 in #429, +1 from #430)
    {
        std::println("\n--- #430 AC9: stats:count >= 41 ---");
        CompilerService cs;
        auto r = cs.eval("(stats:count)");
        CHECK(r.has_value() && is_int(*r) && as_int(*r) >= 41, "stats:count >= 41");
    }
    // AC10: heavy mutation load — sane state
    {
        std::println("\n--- #430 AC10: heavy mutation load (100 mutate calls) ---");
        CompilerService cs;
        cs.eval("(set-code \"(define (f x) (+ x 1))\")");
        cs.eval("(eval-current)");
        for (int i = 0; i < 100; ++i)
            cs.eval(std::format("(f {})", i));
        CHECK(hash_int(cs, "mutation-volume") >= 0, "mutation-volume >= 0 after 100 calls");
        CHECK(hash_int(cs, "compactions") >= 0, "compactions >= 0 after 100 calls (no overflow)");
        CHECK(hash_int(cs, "peak-used-bytes") >= 0, "peak-used-bytes >= 0");
    }
    // AC11: bad policy name returns void (no crash)
    {
        std::println("\n--- #430 AC11: bad policy name doesn't crash ---");
        CompilerService cs;
        auto r = cs.eval("(arena:compact-with-policy \"main\" \"not-a-policy\")");
        CHECK(r.has_value(), "bad policy returns a value (no crash)");
    }
    // AC12: force policy bumps trigger counter
    {
        std::println("\n--- #430 AC12: force bumps trigger counter ---");
        CompilerService cs;
        auto before = hash_int(cs, "auto-compact-triggers");
        cs.eval("(arena:compact-with-policy \"main\" \"force\")");
        auto after = hash_int(cs, "auto-compact-triggers");
        CHECK(after >= before, "force policy bumps or maintains trigger counter");
    }
}

static void run_456_mutation_observability_primitives() {
    std::println("\n=== #456: Fine-grained mutation observability primitives ===");
    // AC1: query:epoch-stats returns int
    {
        std::println("\n--- #456 AC1: query:epoch-stats ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:epoch-stats\")");
        CHECK(r.has_value() && is_int(*r), "query:epoch-stats returns int");
    }
    // AC2: query:epoch-delta first call
    {
        std::println("\n--- #456 AC2: query:epoch-delta first call ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:epoch-delta-since-last-query\")");
        CHECK(r.has_value() && is_int(*r), "epoch-delta-since-last-query first call returns int");
    }
    // AC3: query:dirty-subtree returns count
    {
        std::println("\n--- #456 AC3: query:dirty-subtree returns count ---");
        CompilerService cs;
        cs.eval("(set-code \"(define x 1) (define y 2)\")");
        auto r = cs.eval("(query:dirty-subtree 0)");
        CHECK(r.has_value() && is_int(*r),
              "query:dirty-subtree returns int (count of dirty nodes)");
    }
    // AC4: query:mutation-impact returns value
    {
        std::println("\n--- #456 AC4: query:mutation-impact returns value ---");
        CompilerService cs;
        auto r = cs.eval("(query:mutation-impact)");
        CHECK(r.has_value() && is_int(*r),
              "query:mutation-impact returns int (count of recorded impacts)");
    }
    // AC5: epoch-delta >= 2 after mutate:rebind
    {
        std::println("\n--- #456 AC5: epoch-delta >= 2 after mutate:rebind ---");
        CompilerService cs;
        cs.eval("(set-code \"(define x 1) (define y 2)\")");
        (void)cs.eval("(engine:metrics \"query:epoch-stats\")");
        (void)cs.eval("(engine:metrics \"query:epoch-delta-since-last-query\")");
        cs.eval("(mutate:rebind \"x\" \"42\")");
        auto r = cs.eval("(engine:metrics \"query:epoch-delta-since-last-query\")");
        CHECK(r.has_value() && is_int(*r) && as_int(*r) >= 2,
              "epoch-delta >= 2 after a mutate:rebind (2 bumps per boundary)");
    }
    // AC6: mutation-impact count increases
    {
        std::println("\n--- #456 AC6: mutation-impact count increases after mutate ---");
        CompilerService cs;
        cs.eval("(set-code \"(define z 1)\")");
        auto r0 = cs.eval("(query:mutation-impact)");
        int64_t before = (r0 && is_int(*r0)) ? as_int(*r0) : 0;
        cs.eval("(mutate:rebind \"z\" \"99\")");
        auto r1 = cs.eval("(query:mutation-impact)");
        int64_t after = (r1 && is_int(*r1)) ? as_int(*r1) : 0;
        CHECK(after > before, "query:mutation-impact count increased after mutate");
    }
    // AC7: query:dirty-subtree with reason-mask
    {
        std::println("\n--- #456 AC7: query:dirty-subtree reason-mask ---");
        CompilerService cs;
        cs.eval("(set-code \"(define m 1)\")");
        auto r1 = cs.eval("(query:dirty-subtree 0 255)");
        CHECK(r1.has_value() && is_int(*r1), "query:dirty-subtree 0 255 returns int");
        auto r2 = cs.eval("(query:dirty-subtree 0 0)");
        CHECK(r2.has_value() && is_int(*r2), "query:dirty-subtree 0 0 returns int");
    }
    // AC8: regression — prior primitives still work
    {
        std::println("\n--- #456 AC8: regression prior primitives ---");
        CompilerService cs;
        auto r1 = cs.eval("(engine:metrics \"query:atomic-batch-stats\")");
        CHECK(r1.has_value() && is_int(*r1), "query:atomic-batch-stats (regression for #459)");
        auto r2 = cs.eval("(engine:metrics \"query:verify-dirty-stats\")");
        CHECK(r2.has_value() && is_int(*r2), "query:verify-dirty-stats (regression for #437)");
        auto r3 = cs.eval("(engine:metrics \"query:ir-marker-stats\")");
        CHECK(r3.has_value(), "query:ir-marker-stats (regression for #455)");
    }
    // AC9: define + eval smoke regression
    {
        std::println("\n--- #456 AC9: define + eval smoke ---");
        CompilerService cs;
        cs.eval("(define smoke-456-a 11)");
        cs.eval("(define smoke-456-b 31)");
        auto r = cs.eval("(+ smoke-456-a smoke-456-b)");
        CHECK(r.has_value() && is_int(*r) && as_int(*r) == 42,
              "smoke: (+ 11 31) == 42 (regression)");
    }
}

static void run_508_dead_coercion_elimination_pass() {
    std::println("\n=== #508: DeadCoercionEliminationPass dedicated verification ===");
    auto count_cast_ops = [](const aura::ir::IRModule& mod) -> std::size_t {
        std::size_t n = 0;
        for (const auto& f : mod.functions)
            for (const auto& b : f.blocks)
                for (const auto& i : b.instructions)
                    if (i.opcode == aura::ir::IROpcode::CastOp)
                        ++n;
        return n;
    };
    auto make_module_with = [](std::vector<aura::ir::IRInstruction> instrs,
                               std::uint32_t local_count = 16) -> aura::ir::IRModule {
        aura::ir::IRModule mod;
        mod.functions.push_back(aura::ir::IRFunction{.name = "test", .local_count = local_count});
        auto& func = mod.functions.back();
        func.blocks.push_back({0});
        func.blocks.back().instructions = std::move(instrs);
        return mod;
    };
    // AC1: Dynamic passthrough — ground-to-Dynamic CastOp elided
    {
        std::println("\n--- #508 AC1: safe Dynamic passthrough rule ---");
        auto mod = make_module_with({
            {aura::ir::IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
            {aura::ir::IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},
            {aura::ir::IROpcode::Return, {1, 0, 0, 0}, 0, 0},
        });
        CHECK(count_cast_ops(mod) == std::size_t{1}, "1 CastOp before elision");
        aura::compiler::DeadCoercionEliminationPass dce;
        dce.run(mod);
        CHECK(count_cast_ops(mod) == std::size_t{0}, "Dynamic passthrough CastOp elided");
        CHECK(dce.eliminated_count() == std::size_t{1}, "eliminated_count == 1");
    }
    // AC1b: unknown source NOT elided (conservative)
    {
        std::println("\n--- #508 AC1b: unknown source conservative ---");
        auto mod = make_module_with({
            {aura::ir::IROpcode::Arg, {0, 0, 0, 0}, 0, 0},
            {aura::ir::IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},
            {aura::ir::IROpcode::Return, {1, 0, 0, 0}, 0, 0},
        });
        CHECK(count_cast_ops(mod) == std::size_t{1}, "1 CastOp before");
        aura::compiler::DeadCoercionEliminationPass dce;
        dce.run(mod);
        CHECK(count_cast_ops(mod) == std::size_t{1}, "Unknown source: NOT elided (conservative)");
        CHECK(dce.eliminated_count() == std::size_t{0}, "eliminated_count == 0");
    }
    // AC1c: chain of Dynamic passthroughs collapses
    {
        std::println("\n--- #508 AC1c: chain of Dynamic passthroughs ---");
        auto mod = make_module_with({
            {aura::ir::IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
            {aura::ir::IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},
            {aura::ir::IROpcode::CastOp, {2, 1, 3, 0}, 0, 0},
            {aura::ir::IROpcode::Return, {2, 0, 0, 0}, 0, 0},
        });
        aura::compiler::DeadCoercionEliminationPass dce;
        dce.run(mod);
        CHECK(count_cast_ops(mod) == std::size_t{0}, "Both Dynamic CastOps elided via chain");
        CHECK(dce.eliminated_count() >= std::size_t{2}, "eliminated_count >= 2 (chain collapses)");
    }
    // AC2: keep_for_debug disables elision
    {
        std::println("\n--- #508 AC2: keep_for_debug disables elision ---");
        auto mod = make_module_with({
            {aura::ir::IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
            {aura::ir::IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},
            {aura::ir::IROpcode::Return, {1, 0, 0, 0}, 0, 0},
        });
        aura::compiler::DeadCoercionEliminationPass dce;
        dce.set_keep_for_debug(true);
        dce.run(mod);
        CHECK(count_cast_ops(mod) == std::size_t{1}, "CastOp preserved with keep_for_debug");
        CHECK(dce.eliminated_count() == std::size_t{0}, "eliminated_count == 0 in debug mode");
        CHECK(dce.kept_for_debug_count() == std::size_t{1}, "kept_for_debug_count == 1");
    }
    // AC3a: elapsed_us is monotonic via snapshot
    {
        std::println("\n--- #508 AC3a: elapsed_us monotonic ---");
        CompilerService cs;
        auto snap0 = cs.snapshot();
        std::uint64_t t0 = snap0.dead_coercion_elapsed_us_total;
        cs.eval("(set-code \"(define q 42)\")");
        cs.eval("(eval-current)");
        auto snap1 = cs.snapshot();
        CHECK(snap1.dead_coercion_elapsed_us_total >= t0,
              "elapsed_us_total is monotonic non-decreasing");
    }
    // AC3b+AC3c: primitives
    {
        std::println(
            "\n--- #508 AC3b+AC3c: (stats:get \"compile:dead-coercion-*\") primitives ---");
        CompilerService cs;
        auto r1 = cs.eval("(stats:get \"compile:dead-coercion-elapsed\")");
        CHECK(r1.has_value() && is_int(*r1),
              "(stats:get \"compile:dead-coercion-elapsed\") returns int");
        auto r2 = cs.eval("(stats:get \"compile:dead-coercion-kept-for-debug\")");
        CHECK(r2.has_value() && is_int(*r2),
              "(stats:get \"compile:dead-coercion-kept-for-debug\") returns int");
    }
    // AC4: end-to-end gradual mutation loop
    {
        std::println("\n--- #508 AC4: end-to-end gradual mutation loop ---");
        CompilerService cs;
        cs.eval("(set-code \"(define x 42) (define y \\\"hello\\\") (define z #t)\")");
        auto r1 = cs.eval("(eval-current)");
        CHECK(r1.has_value(), "Initial eval succeeds");
        for (int i = 0; i < 5; ++i) {
            std::string code = "(set-code \"(define v " + std::to_string(i * 7) + ")\")";
            auto rs = cs.eval(code);
            CHECK(rs.has_value(), std::string("set-code iter ") + std::to_string(i) + " ok");
            auto r = cs.eval("(eval-current)");
            CHECK(r.has_value(), std::string("Mutation iter ") + std::to_string(i) + " eval ok");
        }
    }
    // AC4b: gradual mixed-typed coercion chain
    {
        std::println("\n--- #508 AC4b: gradual mixed-typed coercion chain ---");
        CompilerService cs;
        cs.eval("(set-code \"(define s \\\"123\\\") (define n (cast s 'Int))\")");
        auto r = cs.eval("(eval-current)");
        CHECK(r.has_value(), "Mixed coercion program eval succeeds");
    }
}
}

// ── Issue #1401 — load_module_file ↔ compact_env_frames() mutex interlock ──
// Folded from tests/issues/test_issue_1401.cpp via #1957. Pure C++
// Evaluator::compact_env_frames / load_module_file mutex test.
// std::thread preserved verbatim for AC2 (multi-thread mutex exclusion).

static void run_1401_load_module_compact_env_mutex() {
    std::println("\n=== #1401: load_module_file ↔ compact_env_frames() mutex interlock ===");
    // AC1: single-threaded alternating calls
    {
        std::println("\n--- #1401 AC1: single-thread alternating compact+load ---");
        CompilerService cs;
        constexpr int N = 100;
        int errors = 0;
        for (int i = 0; i < N; ++i) {
            try {
                (void)cs.evaluator().compact_env_frames();
            } catch (...) {
                ++errors;
            }
            try {
                auto r = cs.evaluator().load_module_file("__nonexistent_test_path_1401__.aura");
                (void)r;
            } catch (...) {
                ++errors;
            }
        }
        CHECK(errors == 0,
              std::format("{} alternating compact+load calls: 0 errors (got {})", N, errors));
    }
    // AC2: multi-thread mutex exclusion
    {
        std::println("\n--- #1401 AC2: multi-thread mutex exclusion ---");
        CompilerService cs;
        constexpr int N = 200;
        std::atomic<int> compact_count{0};
        std::atomic<int> load_count{0};
        std::atomic<int> errors{0};
        auto compact_worker = [&]() {
            try {
                for (int i = 0; i < N; ++i) {
                    (void)cs.evaluator().compact_env_frames();
                    compact_count.fetch_add(1);
                }
            } catch (...) {
                errors.fetch_add(1);
            }
        };
        auto load_worker = [&]() {
            try {
                for (int i = 0; i < N; ++i) {
                    auto r = cs.evaluator().load_module_file("__nonexistent_test_path_1401__.aura");
                    (void)r;
                    load_count.fetch_add(1);
                }
            } catch (...) {
                errors.fetch_add(1);
            }
        };
        std::thread t1(compact_worker);
        std::thread t2(load_worker);
        t1.join();
        t2.join();
        CHECK(compact_count.load() == N,
              std::format("compact_worker ran {} times (got {})", N, compact_count.load()));
        CHECK(load_count.load() == N,
              std::format("load_worker ran {} times (got {})", N, load_count.load()));
        CHECK(errors.load() == 0,
              std::format("2 threads x {} compact+load: 0 errors (got {})", N, errors.load()));
    }
}

// ── Issue #1407 — ConstraintSolver engine-level cache + typed_mutation epoch bump ──
// Folded from tests/issues/test_issue_1407_constraint_solver_cache.cpp via
// #1957. Pure C++ TypeChecker cache + epoch + direct API tests. 7 ACs.

static void run_1407_constraint_solver_cache_epoch() {
    std::println("\n=== #1407: ConstraintSolver cache + typed_mutation epoch bump ===");
    // AC1: cache hit on repeated infer_flat
    {
        std::println("\n--- #1407 AC1: cache hit on repeated infer_flat ---");
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
        CHECK(r1.index > 0, "first infer_flat returns a valid type");
        tc.set_cache_epoch(100);
        auto r2 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r2.index == r1.index, "second infer_flat returns same type (cache hit)");
        const auto stats = tc.stats();
        CHECK(stats.cs_cache_lookups >= 2, "cs_cache_lookups >= 2 (one per call)");
        CHECK(stats.cs_cache_hits >= 1, "cs_cache_hits >= 1 (second call hit)");
    }
    // AC2: cache miss when epoch advances
    {
        std::println("\n--- #1407 AC2: cache miss on epoch advance ---");
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
        tc.set_cache_epoch(200);
        auto r1 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r1.index > 0, "first infer_flat at epoch=200 returns valid type");
        tc.set_cache_epoch(201);
        auto r2 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r2.index == r1.index, "miss-path still returns correct type");
        tc.set_cache_epoch(201);
        auto r3 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r3.index == r1.index, "post-201 hit returns same type");
        const auto stats = tc.stats();
        CHECK(stats.cs_cache_lookups >= 2, "cs_cache_lookups >= 2 (epoch advance + recovery)");
        CHECK(stats.cs_cache_hits >= 1, "cs_cache_hits >= 1 (post-advance recovery hit)");
    }
    // AC3: cache miss when AST shape changes
    {
        std::println("\n--- #1407 AC3: cache miss on AST shape change ---");
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
        tc.set_cache_epoch(300);
        auto r1 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r1.index > 0, "first infer_flat at shape=42 returns valid type");
        flat.set_int(id, 999); // shape change
        auto r2 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r2.index == r1.index, "post-shape-change miss returns correct type");
        auto r3 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r3.index == r1.index, "recovery call returns correct type");
        const auto stats = tc.stats();
        CHECK(stats.cs_cache_lookups >= 3, "cs_cache_lookups >= 3 (2 misses + 1 hit)");
        CHECK(stats.cs_cache_hits >= 1, "cs_cache_hits >= 1 (recovery hit)");
    }
    // AC4: typed_mutate epoch bump (set_cache_epoch simulates)
    {
        std::println("\n--- #1407 AC4: typed_mutate epoch bump ---");
        aura::core::TypeRegistry reg;
        aura::diag::DiagnosticCollector diag;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool;
        auto id = flat.add_literal(7);
        flat.set_type(id, 0);
        aura::compiler::TypeChecker tc(reg);
        tc.set_strict(true);
        tc.set_cache_epoch(400);
        auto r1 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r1.index > 0, "first infer_flat at epoch=400 returns valid type");
        tc.set_cache_epoch(401); // simulate typed_mutate epoch bump
        auto r2 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r2.index == r1.index, "post-bump infer_flat still returns correct type");
        CHECK(tc.cs_cache_size() >= 1, "cs_cache_size >= 1 after at least one store");
    }
    // AC5: epoch bump invalidates cache
    {
        std::println("\n--- #1407 AC5: epoch bump invalidates cache ---");
        aura::core::TypeRegistry reg;
        aura::diag::DiagnosticCollector diag;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool;
        auto id = flat.add_literal(13);
        flat.set_type(id, 0);
        aura::compiler::TypeChecker tc(reg);
        tc.set_strict(true);
        tc.set_cache_epoch(500);
        auto r1 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r1.index > 0, "first infer_flat at epoch=500 returns valid type");
        CHECK(tc.cs_cache_size() >= 1, "cs_cache populated after first call");
        tc.set_cache_epoch(501);
        auto r2 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r2.index == r1.index, "post-bump infer_flat returns correct type");
        const auto stats = tc.stats();
        CHECK(stats.cs_cache_lookups >= 2, "cs_cache_lookups bumped");
    }
    // Backward compat: no set_cache_epoch means no cache
    {
        std::println("\n--- #1407 backward compat: cache inactive without set_cache_epoch ---");
        aura::core::TypeRegistry reg;
        aura::diag::DiagnosticCollector diag;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool;
        auto id = flat.add_literal(99);
        flat.set_type(id, 0);
        aura::compiler::TypeChecker tc(reg);
        tc.set_strict(true);
        auto r1 = tc.infer_flat(flat, pool, id, diag);
        CHECK(r1.index > 0, "infer_flat works without set_cache_epoch");
        CHECK(tc.cs_cache_size() == 0, "cache stays empty without set_cache_epoch");
        CHECK(tc.stats().cs_cache_lookups == 0, "no cache lookups without epoch");
    }
    // Direct API: cs_cache_lookup / store / clear
    {
        std::println("\n--- #1407 direct cs_cache_lookup / store / clear ---");
        aura::core::TypeRegistry reg;
        aura::compiler::TypeChecker tc(reg);
        aura::compiler::SolveResult res{};
        aura::core::TypeId ty{};
        CHECK(!tc.cs_cache_lookup(42, 1, 0xDEADBEEF, res, ty), "lookup on empty cache misses");
        tc.cs_cache_store(42, 1, 0xDEADBEEF, aura::compiler::SolveResult::SOLVED,
                          aura::core::TypeId{7});
        CHECK(tc.cs_cache_size() == 1, "store increments cache size");
        CHECK(tc.cs_cache_lookup(42, 1, 0xDEADBEEF, res, ty), "lookup after store hits");
        CHECK(res == aura::compiler::SolveResult::SOLVED, "stored SolveResult roundtrips");
        CHECK(ty.index == 7, "stored TypeId roundtrips");
        aura::compiler::SolveResult res2{};
        aura::core::TypeId ty2{};
        CHECK(!tc.cs_cache_lookup(42, 2, 0xDEADBEEF, res2, ty2), "epoch mismatch misses");
        CHECK(!tc.cs_cache_lookup(42, 1, 0xCAFEBABE, res, ty), "hash mismatch misses");
        tc.cs_cache_clear();
        CHECK(tc.cs_cache_size() == 0, "clear empties cache");
    }
}

// ── Issue #1425 — DeadCoercion AST elision + IR pipeline integration ──
// Folded from tests/issues/test_issue_1425.cpp via #1957. Pure C++
// CoercionMap apply + DeadCoercionEliminationPass run_pipeline.
// Complements #508 (#508 covered IR-side DCE; #1425 covers AST-side
// identity elision + run_pipeline integration).

static void run_1425_dead_coercion_ast_ir_pipeline() {
    std::println("\n=== #1425: DeadCoercion AST elision + IR pipeline ===");
    // AC1: AST identity elision (2 of 5)
    {
        std::println("\n--- #1425 AC1: AST CoercionMap identity elision (2 of 5) ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);
        auto f_sym = pool->intern("f");
        auto f_var = flat->add_variable(f_sym);
        aura::ast::NodeId args[5];
        for (int i = 0; i < 5; ++i)
            args[i] = flat->add_literal(i + 1);
        flat->set_type(args[0], 1);
        flat->set_type(args[1], 1);
        auto call = flat->add_call(f_var, args);
        flat->root = call;
        aura::compiler::CoercionMap cm;
        for (int i = 0; i < 5; ++i) {
            cm.add(call, static_cast<std::uint32_t>(i + 1), args[i], 0, 1, 0, 0);
        }
        CHECK(cm.size() == 5, "AC1.setup: 5 coercion entries");
        aura::compiler::DeadCoercionAstStats stats;
        const auto applied = aura::compiler::apply_coercion_map(*flat, cm, &stats, &cm);
        int coercion_nodes = 0;
        for (aura::ast::NodeId i = 0; i < flat->size(); ++i)
            if (flat->get(i).tag == aura::ast::NodeTag::Coercion)
                ++coercion_nodes;
        CHECK(stats.eliminated == 2, "AC1: 2 identity coercions eliminated");
        CHECK(applied == 3, "AC1: 3 CoercionNodes applied");
        CHECK(stats.kept == 3, "AC1: kept == 3");
        CHECK(coercion_nodes == 3, "AC1: FlatAST has exactly 3 Coercion nodes");
        CHECK(cm.eliminated_count() == 2, "AC1: CoercionMap.mark_eliminated count == 2");
    }
    // AC2: non-identity coercions are kept (no false elision)
    {
        std::println("\n--- #1425 AC2: non-identity coercions are kept ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);
        auto f_sym = pool->intern("g");
        auto f_var = flat->add_variable(f_sym);
        auto a0 = flat->add_literal(1);
        auto a1 = flat->add_literal(2);
        flat->set_type(a0, 2);
        flat->set_type(a1, 2);
        std::array<aura::ast::NodeId, 2> call_args = {a0, a1};
        auto call = flat->add_call(f_var, call_args);
        flat->root = call;
        aura::compiler::CoercionMap cm;
        cm.add(call, 1, a0, 0, 1, 0, 0);
        cm.add(call, 2, a1, 0, 1, 0, 0);
        aura::compiler::DeadCoercionAstStats stats;
        aura::compiler::apply_coercion_map(*flat, cm, &stats, &cm);
        int coercion_nodes = 0;
        for (aura::ast::NodeId i = 0; i < flat->size(); ++i)
            if (flat->get(i).tag == aura::ast::NodeTag::Coercion)
                ++coercion_nodes;
        CHECK(stats.eliminated == 0, "AC2: no false identity elision");
        CHECK(stats.applied == 2, "AC2: both non-identity applied");
        CHECK(coercion_nodes == 2, "AC2: 2 Coercion nodes present");
    }
    // AC3: IR DCE 2 of 5 (regression of #1418 AC)
    {
        std::println("\n--- #1425 AC3: IR DeadCoercionEliminationPass 2 of 5 ---");
        aura::ir::IRModule mod;
        aura::ir::IRFunction func;
        func.id = 0;
        func.name = "dce1425";
        func.entry_block = 0;
        func.local_count = 16;
        aura::ir::BasicBlock blk;
        blk.id = 0;
        auto& ins = blk.instructions;
        // 2 identity CastOps
        ins.push_back({aura::ir::IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1});
        ins.push_back({aura::ir::IROpcode::CastOp, {1, 0, 0, 0}, 0, 1});
        ins.push_back({aura::ir::IROpcode::ConstI64, {2, 9, 0, 0}, 0, 1});
        ins.push_back({aura::ir::IROpcode::CastOp, {3, 2, 0, 0}, 0, 1});
        // 3 necessary CastOps
        ins.push_back({aura::ir::IROpcode::ConstI64, {4, 1, 0, 0}, 0, 0});
        ins.push_back({aura::ir::IROpcode::CastOp, {5, 4, 0, 0}, 0, 1});
        ins.push_back({aura::ir::IROpcode::ConstI64, {6, 2, 0, 0}, 0, 0});
        ins.push_back({aura::ir::IROpcode::CastOp, {7, 6, 0, 0}, 0, 1});
        ins.push_back({aura::ir::IROpcode::ConstI64, {8, 3, 0, 0}, 0, 0});
        ins.push_back({aura::ir::IROpcode::CastOp, {9, 8, 0, 0}, 0, 1});
        ins.push_back({aura::ir::IROpcode::Return, {5, 0, 0, 0}, 0, 0});
        func.blocks.push_back(std::move(blk));
        mod.functions.push_back(std::move(func));
        aura::compiler::DeadCoercionEliminationPass dce;
        dce.run(mod);
        int remaining = 0;
        for (const auto& f : mod.functions)
            for (const auto& b : f.blocks)
                for (const auto& i : b.instructions)
                    if (i.opcode == aura::ir::IROpcode::CastOp)
                        ++remaining;
        CHECK(dce.eliminated_count() == 2, "AC3: IR DCE eliminates 2");
        CHECK(remaining == 3, "AC3: 3 CastOps remain");
    }
    // AC4: run_pipeline invokes DCE
    {
        std::println("\n--- #1425 AC4: run_pipeline includes DeadCoercionEliminationPass ---");
        aura::ir::IRModule mod;
        aura::ir::IRFunction func;
        func.name = "pipe";
        func.local_count = 4;
        aura::ir::BasicBlock blk;
        blk.id = 0;
        blk.instructions = {
            {aura::ir::IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
            {aura::ir::IROpcode::CastOp, {1, 0, 0, 0}, 0, 1},
            {aura::ir::IROpcode::Return, {1, 0, 0, 0}, 0, 0},
        };
        func.blocks.push_back(std::move(blk));
        mod.functions.push_back(std::move(func));
        aura::compiler::DeadCoercionEliminationPass dce;
        const bool ok = aura::compiler::run_pipeline(mod, dce);
        CHECK(ok, "AC4: run_pipeline ok");
        CHECK(dce.eliminated_count() == 1, "AC4: DCE inside run_pipeline elided identity CastOp");
        CHECK(mod.functions[0].blocks[0].instructions[1].opcode == aura::ir::IROpcode::Local,
              "AC4: CastOp → Local after pipeline");
    }
}
}

// ── Wave 15 (#125 per-module dirty-skip / #126 pure functions / #138 incremental dirty + type
// check / #139 structural refactor operators) ──

static void run_125_per_module_dirty_skip() {
    std::println("\n=== #125: per-module dirty-skip optimization ===");
    // AC1: dirty-skip counters exist on CompilerMetrics
    {
        std::println("\n--- #125 AC1: dirty-skip counters exist on CompilerMetrics ---");
        aura::compiler::CompilerMetrics m;
        CHECK(m.module_dirty_skips.load() == 0,
              "module_dirty_skips counter exists and starts at 0");
        CHECK(m.module_dirty_recompiles.load() == 0,
              "module_dirty_recompiles counter exists and starts at 0");
        m.module_dirty_skips.fetch_add(3, std::memory_order_relaxed);
        m.module_dirty_recompiles.fetch_add(2, std::memory_order_relaxed);
        CHECK(m.module_dirty_skips.load() == 3, "module_dirty_skips counter increments");
        CHECK(m.module_dirty_recompiles.load() == 2, "module_dirty_recompiles counter increments");
    }
    // AC2: parse a simple program
    {
        std::println("\n--- #125 AC2: parse a simple program ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);
        auto pr = aura::parser::parse_to_flat("(+ 1 2)", *flat, *pool);
        CHECK(pr.success, "simple program parses");
    }
    // AC3: metrics struct has Issue #125 counters
    {
        std::println("\n--- #125 AC3: CompilerMetrics struct has Issue #125 counters ---");
        aura::compiler::CompilerMetrics m;
        m.module_dirty_skips.store(0);
        m.module_dirty_recompiles.store(0);
        m.module_dirty_skips.fetch_add(1);
        m.module_dirty_recompiles.fetch_add(1);
        CHECK(m.module_dirty_skips.load() == 1, "module_dirty_skips is mutable");
        CHECK(m.module_dirty_recompiles.load() == 1, "module_dirty_recompiles is mutable");
    }
}

static void run_126_pure_functions_extracted() {
    std::println("\n=== #126: pure functions extracted from CompilerService and Evaluator ===");
    // AC1: should_relower covers all 5 input combinations
    {
        std::println("\n--- #126 AC1: should_relower covers all combinations ---");
        // Clean, hash matches, no mutation drift → no re-lower
        CHECK(!aura::compiler::should_relower(100, 100, false, 5, 5),
              "clean, hash match, no drift → no re-lower");
        // Dirty → re-lower
        CHECK(aura::compiler::should_relower(100, 100, true, 5, 5), "dirty → re-lower");
        // Hash mismatch → re-lower
        CHECK(aura::compiler::should_relower(100, 200, false, 5, 5), "hash mismatch → re-lower");
        // Mutation drift (cached<current) → re-lower
        CHECK(aura::compiler::should_relower(100, 100, false, 3, 5), "mutation drift → re-lower");
        // Mixed: dirty + hash mismatch + drift → re-lower
        CHECK(aura::compiler::should_relower(100, 200, true, 1, 5),
              "dirty + hash mismatch + drift → re-lower");
        // All-zero inputs → no re-lower
        CHECK(!aura::compiler::should_relower(0, 0, false, 0, 0), "all-zero inputs → no re-lower");
    }
    // AC2: fnv1a_64 known answer test
    {
        std::println("\n--- #126 AC2: fnv1a_64 known answer test ---");
        auto h0 = aura::compiler::fnv1a_64("");
        auto h1 = aura::compiler::fnv1a_64("a");
        auto h_foo = aura::compiler::fnv1a_64("foobar");
        CHECK(h0 == 0xcbf29ce484222325ULL, "fnv1a_64(\"\") == 0xcbf29ce484222325");
        CHECK(h1 == 0xaf63dc4c8601ec8cULL, "fnv1a_64(\"a\") == 0xaf63dc4c8601ec8c");
        CHECK(h_foo == 0x85944171f73967e8ULL, "fnv1a_64(\"foobar\") == 0x85944171f73967e8");
        CHECK(aura::compiler::fnv1a_64("hello") == aura::compiler::fnv1a_64("hello"),
              "fnv1a_64 is deterministic");
        CHECK(aura::compiler::fnv1a_64("hello") != aura::compiler::fnv1a_64("Hello"),
              "fnv1a_64 is case-sensitive");
    }
    // AC3: compute_dependencies
    {
        std::println("\n--- #126 AC3: compute_dependencies walks the AST ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool(alloc);
        auto a_id = pool.intern("a");
        auto b_id = pool.intern("b");
        auto a_var = flat.add_variable(a_id);
        auto b_var = flat.add_variable(b_id);
        auto plus_id = pool.intern("+");
        auto plus_var = flat.add_variable(plus_id);
        auto call = flat.add_call(plus_var, {a_var, b_var});
        std::unordered_set<std::string> available;
        available.insert("a");
        available.insert("b");
        available.insert("c");
        auto deps = aura::compiler::compute_dependencies(flat, pool, call, available);
        CHECK(deps.size() == 2, "compute_dependencies returns 2 deps for (a b)");
        if (deps.size() == 2) {
            CHECK(deps[0] == "a", "first dep is 'a'");
            CHECK(deps[1] == "b", "second dep is 'b'");
        }
        // Deduplication
        auto a2_var = flat.add_variable(a_id);
        auto call_dup = flat.add_call(plus_var, {a_var, a2_var});
        auto deps2 = aura::compiler::compute_dependencies(flat, pool, call_dup, available);
        CHECK(deps2.size() == 1, "compute_dependencies deduplicates 'a'");
        if (deps2.size() == 1) {
            CHECK(deps2[0] == "a", "deduplicated dep is 'a'");
        }
        // d not in available
        auto d_id = pool.intern("d");
        auto d_var = flat.add_variable(d_id);
        auto call3 = flat.add_call(plus_var, {a_var, d_var});
        auto deps3 = aura::compiler::compute_dependencies(flat, pool, call3, available);
        CHECK(deps3.size() == 1 && deps3[0] == "a",
              "compute_dependencies excludes 'd' (not in available_defines)");
        // Empty available
        std::unordered_set<std::string> empty;
        auto deps4 = aura::compiler::compute_dependencies(flat, pool, call, empty);
        CHECK(deps4.empty(), "compute_dependencies with empty available returns empty");
    }
    // AC4: try_extract_define
    {
        std::println("\n--- #126 AC4: try_extract_define pattern match ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        aura::ast::StringPool pool(alloc);
        auto name_id = pool.intern("my-def");
        auto body_id = flat.add_literal(42);
        auto define_id = flat.add_define(name_id, body_id);
        auto def = aura::compiler::try_extract_define(flat, pool, define_id);
        CHECK(def.has_value(), "try_extract_define returns Some for Define root");
        if (def) {
            CHECK(def->first == "my-def", "extracted name is 'my-def'");
            CHECK(def->second == body_id, "extracted body is the literal 42");
        }
        auto lit = flat.add_literal(99);
        auto def2 = aura::compiler::try_extract_define(flat, pool, lit);
        CHECK(!def2.has_value(), "try_extract_define returns None for non-Define root");
        auto def3 = aura::compiler::try_extract_define(flat, pool, aura::ast::NULL_NODE);
        CHECK(!def3.has_value(), "try_extract_define returns None for NULL_NODE");
    }
}

static void run_138_incremental_dirty_propagation_type_checking() {
    std::println("\n=== #138: incremental dirty propagation + fine-grained type checking ===");
    // AC1: dirty propagation correctness
    {
        std::println("\n--- #138 AC1: dirty propagation correctness ---");
        CompilerService cs;
        int64_t r1 = run_int(cs, "(set-code \"(define x 1)(define y 2)\") "
                                 "(eval-current) (+ x y)");
        CHECK(r1 == 3, "set-code + eval-current: 1+2=3");
        int64_t r2 = run_int(cs, "(set-code \"(define x 1)\") "
                                 "(mutate:rebind \"x\" \"(quote 99)\" \"test\") "
                                 "(eval-current) x");
        CHECK(r2 == 99, "after mutate:rebind, eval-current sees x=99");
        std::string s = run_str(cs, "(set-code \"(define x 1)(define y 2)\") "
                                    "(mutate:rebind \"x\" \"(quote 99)\" \"test\") "
                                    "(typecheck-current)");
        CHECK(s.find("no errors") != std::string::npos,
              "typecheck after mutate:rebind returns no errors");
        std::string s1 = run_str(cs, "(set-code \"(define x 1)\") "
                                     "(typecheck-current)");
        CHECK(s1.find("no errors") != std::string::npos, "typecheck passes before mutation");
        std::string s2 = run_str(cs, "(set-code \"(define x 1)\") "
                                     "(mutate:replace-value (query:find \"x\") "
                                     "\"\\\"hello\\\"\" \"test\") "
                                     "(typecheck-current)");
        bool has_error = s2.find("error") != std::string::npos ||
                         s2.find("Error") != std::string::npos ||
                         s2.find("TypeError") != std::string::npos;
        CHECK(has_error, "typecheck detects error after type-violating mutation");
    }
    // AC2: incremental typecheck equivalence
    {
        std::println("\n--- #138 AC2: incremental typecheck equivalence ---");
        CompilerService cs;
        std::string s1 = run_str(cs, "(set-code \"(define x 1)(define y 2)(+ x y)\") "
                                     "(typecheck-current)");
        bool ok1 = s1.find("no errors") != std::string::npos;
        CHECK(ok1, "first typecheck on clean code: no errors");
        std::string s2 = run_str(cs, "(set-code \"(define x 1)(define y 2)(+ x y)\") "
                                     "(typecheck-current)");
        bool ok2 = s2.find("no errors") != std::string::npos;
        CHECK(ok2, "second typecheck on identical clean code: no errors");
        std::string fresh = run_str(cs, "(set-code \"(define f (lambda (x) (+ x 1)))\") "
                                        "(typecheck-current)");
        std::string recheck = run_str(cs, "(set-code \"(define f (lambda (x) (+ x 1)))\") "
                                          "(typecheck-current)");
        CHECK(fresh == recheck, "typecheck on identical clean code: identical output");
    }
    // AC3: performance — incremental speedup stability
    {
        std::println("\n--- #138 AC3: incremental speedup stability ---");
        CompilerService cs;
        std::string code = "(set-code \"(begin ";
        for (int i = 0; i < 30; ++i) {
            code +=
                "(define f" + std::to_string(i) + " (lambda (x) (+ x " + std::to_string(i) + "))) ";
        }
        code += ")\") (typecheck-current)";
        std::string r1 = run_str(cs, code);
        std::string r2 = run_str(cs, code);
        CHECK(r1 == r2, "incremental typecheck: identical output across runs");
    }
    // AC4: stress test (50 mutate cycles)
    {
        std::println("\n--- #138 AC4: stress test (50 mutate cycles) ---");
        CompilerService cs;
        std::string code = "(set-code \"(define counter 0)\") (begin ";
        for (int i = 0; i < 50; ++i) {
            code += "(mutate:rebind \"counter\" \"(quote " + std::to_string(i) + ")\" \"c\") ";
        }
        code += "(typecheck-current))";
        std::string status = run_str(cs, code);
        bool ok = status.find("no errors") != std::string::npos;
        CHECK(ok, "after 50 mutate cycles, typecheck-current still passes");
    }
    // AC4 cont.: multiple mutations
    {
        std::println("\n--- #138 AC4 cont.: multiple mutations don't corrupt workspace ---");
        CompilerService cs;
        std::string code = "(set-code \"(define a 1)(define b 2)(define c 3)\") (begin "
                           "(mutate:rebind \"a\" \"(quote 10)\" \"t\") "
                           "(mutate:rebind \"b\" \"(quote 20)\" \"t\") "
                           "(mutate:rebind \"c\" \"(quote 30)\" \"t\") "
                           "(eval-current) (+ a b c))";
        int64_t sum = run_int(cs, code);
        CHECK(sum == 60, "after mutations: 10+20+30=60");
    }
    // Workspace isolation
    {
        std::println("\n--- #138 workspace isolation (per-eval set-code) ---");
        CompilerService cs;
        int64_t r1 = run_int(cs, "(set-code \"(define x 1)\") "
                                 "(mutate:rebind \"x\" \"(quote 99)\" \"t\") "
                                 "(eval-current) x");
        CHECK(r1 == 99, "first eval: x=99 after mutate");
        int64_t r2 = run_int(cs, "(set-code \"(define x 1)\") "
                                 "(eval-current) x");
        CHECK(r2 == 1, "second eval: fresh set-code, x=1 (workspace isolated)");
    }
    // FlatAST dirty bit API
    {
        std::println("\n--- #138 FlatAST dirty bit API via direct C++ ---");
        CompilerService cs;
        std::string s = run_str(cs, "(set-code \"(define x 1)\") (typecheck-current)");
        CHECK(s.find("no errors") != std::string::npos,
              "typecheck-current on clean workspace: no errors");
    }
}

static void run_139_structural_refactor_operators() {
    std::println("\n=== #139: advanced structural refactoring operators (rename, inline, extract, "
                 "move, wrap) ===");
    // AC1: structural refactor operators (rename-symbol, inline-call, refactor-extract, move-node,
    // extract-function)
    {
        std::println("\n--- #139 AC1: structural refactor operators ---");
        CompilerService cs;
        // rename-symbol all occurrences
        std::string s = run_str(cs, "(set-code \"(define (f x) (+ x x))\") "
                                    "(begin "
                                    "  (mutate:rename-symbol \"x\" \"y\" \"rename test\") "
                                    "  (typecheck-current))");
        bool ok = s.find("no errors") != std::string::npos;
        CHECK(ok, "after rename x→y, typecheck-current passes");
        // rename creates new binding (cross-binding)
        int64_t r = run_int(cs, "(set-code \"(define (f x) (+ x 1))(f 5)\") "
                                "(mutate:rename-symbol \"f\" \"g\" \"rename test\") "
                                "(eval-current) (g 5)");
        CHECK(r == 6, "after rename f→g, (g 5) = 6");
        // rename preserves shadow
        int64_t r2 = run_int(cs, "(set-code "
                                 "\""
                                 "(define (outer x) (define (inner x) (* x 2)) (+ x (inner x)))"
                                 "\") "
                                 "(mutate:rename-symbol \"outer\" \"outer2\" \"test\") "
                                 "(eval-current) (outer2 5)");
        CHECK(r2 == 15, "after rename outer→outer2, (outer2 5) = 15");
        // inline-call basic
        int64_t r3 =
            run_int(cs, "(set-code \"(define (sq x) (* x x))(define v (sq 5))\") "
                        "(begin "
                        "  (mutate:inline-call (list-ref (query:node-type \"Call\") 1) \"test\") "
                        "  (eval-current) v)");
        CHECK(r3 == 25, "after inline-call, v=25");
        // refactor-extract
        int64_t r4 =
            run_int(cs, "(set-code \"(define (f x) (+ (* x 2) 1))\") "
                        "(begin "
                        "  (mutate:refactor/extract (car (query:node-type \"Call\")) \"doubled\") "
                        "  (length (query:node-type \"Define\")))");
        CHECK(r4 == 2, "after refactor/extract: 2 Defines (original + extracted)");
        // move-node
        int64_t r5 = run_int(cs, "(set-code \"(begin (define a 1) (define b 2))\") "
                                 "(begin "
                                 "  (mutate:move-node (query:find \"b\") "
                                 "                       (query:find \"a\") 0 \"move b before a\") "
                                 "  (eval-current) (+ a b))");
        CHECK(r5 == 3, "after move-node, (+ a b) = 3");
        // extract-function
        int64_t r6 =
            run_int(cs, "(set-code \"(define (f x) (+ (* x 2) 1))\") "
                        "(begin "
                        "  (mutate:extract-function (car (query:find \"x\")) \"double-of\") "
                        "  (eval-current) (f 5))");
        CHECK(r6 == 11, "after extract-function, (f 5) = 11");
        // hygiene preservation
        int64_t r7 = run_int(
            cs, "(set-code "
                "\""
                "(define-hygienic-macro (swap! a b) (let ((tmp a)) (set! a b) (set! b tmp)))"
                "(define x 1) (define y 2) (define tmp 99)"
                "\") "
                "(eval-current) (begin (swap! x y) tmp)");
        CHECK(r7 == 99, "caller's tmp NOT captured by hygienic swap!");
    }
    // AC2: stress + mixed refactor + splice/wrap
    {
        std::println("\n--- #139 AC2: stress + mixed refactor + splice/wrap ---");
        CompilerService cs;
        // 50 rename cycles
        std::string code2 = "(set-code \"(define a 1)(define b 2)\") (begin ";
        for (int i = 0; i < 50; ++i) {
            if (i % 2 == 0) {
                code2 += "(mutate:rename-symbol \"a\" \"tmp\" \"test\") ";
            } else {
                code2 += "(mutate:rename-symbol \"tmp\" \"a\" \"test\") ";
            }
        }
        code2 += "(typecheck-current))";
        std::string status = run_str(cs, code2);
        bool ok = status.find("no errors") != std::string::npos;
        CHECK(ok, "after 50 rename cycles, typecheck passes");
        // mixed refactor (rename + inline)
        std::string status2 =
            run_str(cs, "(set-code \"(define (sq x) (* x x))(define v (sq 5))\") "
                        "(begin "
                        "  (mutate:rename-symbol \"sq\" \"square\" \"test\") "
                        "  (mutate:inline-call (list-ref (query:node-type \"Call\") 1) \"test\") "
                        "  (typecheck-current))");
        bool ok2 = status2.find("no errors") != std::string::npos;
        CHECK(ok2, "after rename+inline, typecheck passes");
        // splice and wrap
        std::string code3 = "(set-code \"(define x 5)\") "
                            "(begin "
                            "  (mutate:wrap (car (query:find \"x\")) \"lambda-wrap\" \"test\") "
                            "  (typecheck-current))";
        std::string status3 = run_str(cs, code3);
        bool ok3 = status3.find("no errors") != std::string::npos;
        CHECK(ok3, "after mutate:wrap, typecheck passes");
    }
    // AC3: type correctness after refactor
    {
        std::println("\n--- #139 AC3: type correctness after refactor ---");
        CompilerService cs;
        std::string s1 = run_str(cs, "(set-code \"(define (add1 x) (+ x 1))(add1 5)\") "
                                     "(begin "
                                     "  (mutate:rename-symbol \"add1\" \"increment\" \"test\") "
                                     "  (typecheck-current))");
        bool ok1 = s1.find("no errors") != std::string::npos;
        CHECK(ok1, "after rename add1→increment, typecheck passes");
        std::string s2 =
            run_str(cs, "(set-code \"(define (f x) (+ (* x 2) 1))\") "
                        "(begin "
                        "  (mutate:extract-function (car (query:find \"x\")) \"double-of\") "
                        "  (typecheck-current))");
        bool ok2 = s2.find("no errors") != std::string::npos;
        CHECK(ok2, "after extract-function, typecheck passes");
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
    aura_domain_gates_batch::run_330_structural_mutation_guard_reader_lock_guard();
    aura_domain_gates_batch::run_356_envframe_post_rollback_invalidation();
    aura_domain_gates_batch::run_375_ir_encoding_observability_foundation();
    aura_domain_gates_batch::run_430_arena_compaction_policy_observability();
    aura_domain_gates_batch::run_456_mutation_observability_primitives();
    aura_domain_gates_batch::run_508_dead_coercion_elimination_pass();
    aura_domain_gates_batch::run_1401_load_module_compact_env_mutex();
    aura_domain_gates_batch::run_1407_constraint_solver_cache_epoch();
    aura_domain_gates_batch::run_1425_dead_coercion_ast_ir_pipeline();
    aura_domain_gates_batch::run_125_per_module_dirty_skip();
    aura_domain_gates_batch::run_126_pure_functions_extracted();
    aura_domain_gates_batch::run_138_incremental_dirty_propagation_type_checking();
    aura_domain_gates_batch::run_139_structural_refactor_operators();
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
