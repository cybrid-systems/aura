// tests/test_bidirectional_annotation.cpp — Issue #1413: True
// bidirectional type checking (Synth + Check) — annotation
// mismatch detection at compile time.
//
// Background: bidirectional check-mode plumbing is already wired
// in src/compiler/type_checker_impl.cpp:3087+ (Issue #384 first
// slice). check_flat_call at line 4606 does
// `synthesize_flat_call` → `cs_.consistent_unify` against the
// expected type from the call-site annotation. Mismatch reports
// a TypeError via diag_ with blame on the caller.
//
// This test is a contract test — it exercises the full
// CompilerService pipeline (cs.eval) which triggers
// Infer_flat → bidirectional check → mismatch detection. The
// bidirectional_mode_ flag is true by default (line 667 of
// type_checker.ixx), so the check is active out of the box.
//
// ACs:
//   AC1: (let ((x : Integer 1)) (+ x 2)) typechecks OK
//   AC2: (let ((x : Integer "hello")) (+ x 2)) gets a TypeError
//   AC3: bidirectional check survives across the function body —
//        synth of 1 (Int) ⊆ annotation : Integer passes, but
//        synth of "hello" (String) vs annotation : Integer fails
//   AC4: backward compat — code without annotations still
//        typechecks (the bidirectional check is opt-in per binding
//        via TypeAnnotation presence)
//   AC5 (#2992): (: x Int "hello") emits Warning in
//        default non-strict Balanced mode (program still typechecks)
//   AC6 (#2992): Dynamic ~ T stays silent
//   AC7 (#2992): Int ↔ Float stays silent
//   AC8 (#2992): permissive knob silences Int vs String
//
// Note: the top-level `infer_flat_bidirectional` orchestration
// proposed in the issue body is a follow-up. This test
// exercises the existing per-call / per-lambda check pass that
// already enforces the contract.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <fstream>
#include <iterator>

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.service;
import aura.compiler.coercion_map;
import aura.diag;
import aura.parser.parser;

namespace test_bidirectional_annotation_detail {

// Helper: run a piece of code through the full CompilerService
// pipeline. Returns true if eval succeeded (no TypeError
// diagnostic), false otherwise.
bool run_eval(aura::compiler::CompilerService& cs, const std::string& code) {
    // set-code initializes workspace + parses; eval-current runs
    // the full type-check + interpret pipeline.
    auto set_r = cs.eval("(set-code \"" + code + "\")");
    if (!set_r.has_value())
        return false;
    auto eval_r = cs.eval("(eval-current)");
    return eval_r.has_value();
}

} // namespace test_bidirectional_annotation_detail

int aura_issue_1413_run() {
    using namespace test_bidirectional_annotation_detail;
    std::println("=== Issue #1413: True bidirectional type checking ===");

    // ── AC1: (let ((x : Integer 1)) (+ x 2)) typechecks OK ──
    //
    // annotation : Integer expects Int, synth of 1 is Int,
    // consistent_unify passes, body synth Int, ⊆ annotation.
    {
        std::println("\n--- AC1: (let ((x : Integer 1)) (+ x 2)) typechecks ---");
        aura::compiler::CompilerService cs;
        const bool ok = run_eval(cs, "(let ((x : Integer 1)) (+ x 2))");
        CHECK(ok, "AC1: annotated binding with matching type typechecks OK");
    }

    // ── AC2: (let ((x : Integer "hello")) (+ x 2)) gets TypeError ──
    //
    // annotation : Integer expects Int, synth of "hello" is String,
    // consistent_unify fails, is_coercible is false, TypeError
    // reported, eval returns no value.
    {
        std::println("\n--- AC2: (let ((x : Integer \\\"hello\\\")) (+ x 2)) rejected ---");
        aura::compiler::CompilerService cs;
        const bool ok = run_eval(cs, "(let ((x : Integer \"hello\")) (+ x 2))");
        CHECK(!ok, "AC2: annotated binding with mismatched type rejected (TypeError)");
    }

    // ── AC3: bidirectional check across function body ──
    //
    // annotation : Integer on the let, body (+ x 2) is Int (1 + 2
    // are both Int), ⊆ Integer. Positive path. The AC1 case
    // already covers this, but we make it explicit.
    {
        std::println("\n--- AC3: bidirectional check across function body ---");
        aura::compiler::CompilerService cs;
        const bool ok = run_eval(cs, "(let ((x : Integer 100)) "
                                     "  (+ x 200))");
        CHECK(ok, "AC3: synth of body within annotation type is accepted");
    }

    // ── AC4: backward compat — no annotations still typechecks ──
    //
    // Plain let without annotation — bidirectional_mode_ is on but
    // no expected type flows in, so the check is a no-op.
    {
        std::println("\n--- AC4: no annotations still typechecks ---");
        aura::compiler::CompilerService cs;
        const bool ok = run_eval(cs, "(let ((x 1)) (+ x 2))");
        CHECK(ok, "AC4: plain let (no annotation) typechecks — backward compat");
    }

    // ── #2992 helpers ──
    auto infer_code = [](const std::string& code, aura::compiler::GradualPermissiveness gp,
                         bool strict, aura::diag::DiagnosticCollector& diag) {
        aura::core::TypeRegistry reg;
        aura::compiler::TypeChecker tc(reg);
        tc.set_strict(strict);
        tc.set_gradual_permissiveness(gp);
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(code, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return;
        flat.root = pr.root;
        (void)tc.infer_flat(flat, pool, pr.root, diag);
    };
    auto has_kind_msg = [](const aura::diag::DiagnosticCollector& diag, aura::diag::ErrorKind kind,
                           std::string_view needle) {
        for (const auto& d : diag.diagnostics()) {
            if (d.kind == kind && d.message.find(needle) != std::string::npos)
                return true;
        }
        return false;
    };

    // ── AC5 (#2992): Int vs String → Warning in default non-strict ──
    {
        std::println("\n--- AC5 (#2992): Int vs String Warning (balanced) ---");
        aura::diag::DiagnosticCollector diag;
        infer_code("(: x Int \"hello\")", aura::compiler::GradualPermissiveness::Balanced,
                   /*strict=*/false, diag);
        CHECK(has_kind_msg(diag, aura::diag::ErrorKind::Warning, "incompatible ground types"),
              "ac2992_1_int_string_warning: Int vs String produces Warning in default non-strict");
    }

    // ── AC6 (#2992): Dynamic ~ T stays permissive ──
    {
        std::println("\n--- AC6 (#2992): Dynamic ~ T silent ---");
        aura::diag::DiagnosticCollector diag;
        infer_code("(: x Any \"hello\")", aura::compiler::GradualPermissiveness::Balanced,
                   /*strict=*/false, diag);
        CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::Warning, "incompatible ground types"),
              "ac2992_2_dynamic_permissive: Dynamic boundary stays fully permissive");
    }

    // ── AC7 (#2992): Int ↔ Float stays quiet ──
    {
        std::println("\n--- AC7 (#2992): Int ↔ Float silent ---");
        aura::diag::DiagnosticCollector diag;
        infer_code("(: x Float 1)", aura::compiler::GradualPermissiveness::Balanced,
                   /*strict=*/false, diag);
        CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::Warning, "incompatible ground types"),
              "ac2992_3_numeric_quiet: intentional numeric coercion stays quiet");
    }

    // ── AC8 (#2992): permissive silences Int vs String ──
    {
        std::println("\n--- AC8 (#2992): permissive silences Int vs String ---");
        aura::diag::DiagnosticCollector diag;
        infer_code("(: x Int \"hello\")", aura::compiler::GradualPermissiveness::Permissive,
                   /*strict=*/false, diag);
        CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::Warning, "incompatible ground types"),
              "ac2992_4_permissive_silent: permissive knob restores legacy silent ground "
              "consistency");
        CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "incompatible ground types"),
              "AC8b: permissive does not elevate to TypeError");
    }

    // ── #3202: Production + Strict hard-reject unify boolean ──
    {
        using aura::compiler::ConstraintSystem;
        using aura::compiler::GradualPermissiveness;
        using aura::compiler::kProductionStrictGroundUnifyIssue;
        using aura::compiler::TypeChecker;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::core::TypeRegistry;

        struct ProdScope {
            ProdScope() { apply_production_audit_defaults(); }
            ~ProdScope() { apply_dev_audit_defaults(); }
        };

        CHECK(kProductionStrictGroundUnifyIssue == 3202, "ac3202_4_source_and_linter");

        // AC1: Production + Strict → Int~String unify false
        {
            std::println("\n--- AC9 (#3202): Production+Strict unify hard-reject ---");
            ProdScope prod;
            TypeRegistry reg;
            ConstraintSystem cs(reg);
            cs.set_unify_gradual_mode(GradualPermissiveness::Strict);
            CHECK(!cs.consistent_unify(reg.int_type(), reg.string_type()),
                  "ac3202_1_prod_strict_unify_false: Int~String hard-reject");
            CHECK(!cs.consistent_unify(reg.bool_type(), reg.string_type()),
                  "ac3202_1b: Bool~String hard-reject");
        }

        // AC1 check-site: TypeError + no CastOp
        {
            std::println("\n--- AC9b (#3202): Production+Strict TypeError, no CastOp ---");
            ProdScope prod;
            TypeRegistry reg;
            TypeChecker tc(reg);
            tc.set_strict(false);
            tc.set_gradual_permissiveness(GradualPermissiveness::Strict);
            aura::diag::DiagnosticCollector diag;
            aura::ast::ASTArena arena;
            auto alloc = arena.allocator();
            aura::ast::StringPool pool(alloc);
            aura::ast::FlatAST flat(alloc);
            auto pr = aura::parser::parse_to_flat("(: x Int \"hello\")", flat, pool);
            CHECK(pr.success && pr.root != aura::ast::NULL_NODE, "ac3202_1 parse");
            if (pr.success && pr.root != aura::ast::NULL_NODE) {
                flat.root = pr.root;
                (void)tc.infer_flat(flat, pool, pr.root, diag);
            }
            CHECK(has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch") ||
                      has_kind_msg(diag, aura::diag::ErrorKind::TypeError,
                                   "incompatible ground types"),
                  "ac3202_1_typeerror: Production+Strict TypeError at check site");
            CHECK(tc.last_coercions().empty(), "ac3202_1_no_castop: no CastOp emitted");
        }

        // AC2: Soft / balanced / permissive keep unify true
        {
            std::println("\n--- AC10 (#3202): Soft Strict unify stays true ---");
            apply_dev_audit_defaults();
            TypeRegistry reg;
            ConstraintSystem cs(reg);
            cs.set_unify_gradual_mode(GradualPermissiveness::Strict);
            CHECK(cs.consistent_unify(reg.int_type(), reg.string_type()),
                  "ac3202_2_soft_strict_unify_true");
            cs.set_unify_gradual_mode(GradualPermissiveness::Balanced);
            CHECK(cs.consistent_unify(reg.int_type(), reg.string_type()),
                  "ac3202_2_soft_balanced_unify_true");
            cs.set_unify_gradual_mode(GradualPermissiveness::Permissive);
            CHECK(cs.consistent_unify(reg.int_type(), reg.string_type()),
                  "ac3202_2_soft_permissive_unify_true");
        }
        {
            std::println("\n--- AC10b (#3202): Production+Balanced unify stays true ---");
            ProdScope prod;
            TypeRegistry reg;
            ConstraintSystem cs(reg);
            cs.set_unify_gradual_mode(GradualPermissiveness::Balanced);
            CHECK(cs.consistent_unify(reg.int_type(), reg.string_type()),
                  "ac3202_2_prod_balanced_unify_true");
        }

        // AC3: Int ↔ Float stays permissive; Dynamic ~ T hard-rejects under
        // the production face (Issue #3622 residual — Soft stays permissive).
        {
            std::println("\n--- AC11 (#3202+#3622): Dynamic~T rejects, Int↔Float stays true ---");
            ProdScope prod;
            TypeRegistry reg;
            ConstraintSystem cs(reg);
            cs.set_unify_gradual_mode(GradualPermissiveness::Strict);
            auto fl = reg.lookup_type("Float");
            CHECK(!cs.consistent_unify(reg.dynamic_type(), reg.string_type()),
                  "ac3202_3_dynamic_permissive");
            CHECK(cs.consistent_unify(reg.int_type(), fl), "ac3202_3_numeric_int_float");
            CHECK(cs.consistent_unify(fl, reg.int_type()), "ac3202_3_numeric_float_int");
        }

        apply_dev_audit_defaults();
    }

    // ── #3430: production_defaults forces Strict without set_strict ──
    {
        using aura::compiler::CompilerMetrics;
        using aura::compiler::ConstraintSystem;
        using aura::compiler::GradualPermissiveness;
        using aura::compiler::kProductionDefaultsForceStrictUnifyIssue;
        using aura::compiler::TypeChecker;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::core::TypeRegistry;

        struct ProdScope {
            ProdScope() { apply_production_audit_defaults(); }
            ~ProdScope() { apply_dev_audit_defaults(); }
        };

        CHECK(kProductionDefaultsForceStrictUnifyIssue == 3430, "ac3430_5_stamp");

        {
            std::println("\n--- #3430 AC1: Production + no set_strict → Int~String reject ---");
            ProdScope prod;
            TypeRegistry reg;
            TypeChecker tc(reg);
            CHECK(!tc.is_strict(), "ac3430_1_strict_flag_still_false");
            CHECK(tc.effective_gradual_permissiveness() == GradualPermissiveness::Strict,
                  "ac3430_1_effective_strict_without_set_strict");
            CompilerMetrics metrics{};
            tc.set_metrics(&metrics);
            aura::diag::DiagnosticCollector diag;
            aura::ast::ASTArena arena;
            auto alloc = arena.allocator();
            aura::ast::StringPool pool(alloc);
            aura::ast::FlatAST flat(alloc);
            auto pr = aura::parser::parse_to_flat("(: x Int \"hello\")", flat, pool);
            CHECK(pr.success && pr.root != aura::ast::NULL_NODE, "ac3430_1 parse");
            if (pr.success && pr.root != aura::ast::NULL_NODE) {
                flat.root = pr.root;
                (void)tc.infer_flat(flat, pool, pr.root, diag);
            }
            CHECK(has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch") ||
                      has_kind_msg(diag, aura::diag::ErrorKind::TypeError,
                                   "incompatible ground types"),
                  "ac3430_1_typeerror");
            CHECK(tc.last_coercions().empty(), "ac3430_1_no_castop");
            CHECK(metrics.gradual_ground_incompatible_error_total.load() > 0,
                  "ac3430_1_error_counter");
        }

        {
            std::println("\n--- #3430 AC2: infer_flat_partial first pass uses Strict ---");
            const auto impl = [] {
                std::ifstream in("src/compiler/type_checker_impl.cpp");
                if (!in) {
                    in.open("../src/compiler/type_checker_impl.cpp");
                }
                if (!in) {
                    in.open("../../src/compiler/type_checker_impl.cpp");
                }
                return std::string((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            }();
            const auto ixx = [] {
                std::ifstream in("src/compiler/type_checker.ixx");
                if (!in)
                    in.open("../src/compiler/type_checker.ixx");
                if (!in)
                    in.open("../../src/compiler/type_checker.ixx");
                return std::string((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            }();
            CHECK(impl.find("TypeChecker::infer_flat_partial") != std::string::npos,
                  "ac3430_2_partial_entry");
            CHECK(impl.find("engine.set_gradual_permissiveness(gradual_permissiveness_)") !=
                      std::string::npos,
                  "ac3430_2_partial_plumbs_knob");
            CHECK(ixx.find("if (aura::compiler::typed_audit::production_defaults_active())") !=
                          std::string::npos &&
                      ixx.find("return GradualPermissiveness::Strict;") != std::string::npos,
                  "ac3430_2_effective_ssot");
            ProdScope prod;
            TypeRegistry reg;
            TypeChecker tc(reg);
            CHECK(tc.effective_gradual_permissiveness() == GradualPermissiveness::Strict,
                  "ac3430_2_live_effective_strict");
        }

        {
            std::println("\n--- #3430 AC3: Soft Balanced Warning path + Int↔Float ---");
            apply_dev_audit_defaults();
            TypeRegistry reg;
            TypeChecker tc(reg);
            CHECK(!aura::compiler::typed_audit::production_defaults_active(), "ac3430_3_soft");
            CHECK(tc.effective_gradual_permissiveness() == tc.gradual_permissiveness() ||
                      tc.is_strict(),
                  "ac3430_3_soft_follows_knob");
            ConstraintSystem cs(reg);
            cs.set_unify_gradual_mode(GradualPermissiveness::Balanced);
            CHECK(cs.consistent_unify(reg.int_type(), reg.string_type()),
                  "ac3430_3_soft_balanced_unify_true");
            auto fl = reg.lookup_type("Float");
            CHECK(cs.consistent_unify(reg.int_type(), fl), "ac3430_3_numeric_int_float");
            CHECK(cs.consistent_unify(fl, reg.int_type()), "ac3430_3_numeric_float_int");
        }

        {
            std::println("\n--- #3430 AC4: Dynamic~T and Linear+Dynamic reject (#117) ---");
            ProdScope prod;
            TypeRegistry reg;
            ConstraintSystem cs(reg);
            cs.set_unify_gradual_mode(GradualPermissiveness::Strict);
            // Issue #3622: Dynamic ~ T hard-rejects under the production face
            // (supersedes the #3430 dynamic-permissive accept).
            CHECK(!cs.consistent_unify(reg.dynamic_type(), reg.string_type()),
                  "ac3430_4_dynamic_permissive");
            auto lin = reg.register_linear(reg.int_type());
            CHECK(!cs.consistent_unify(reg.dynamic_type(), lin), "ac3430_4_linear_dynamic_reject");
            CHECK(!cs.consistent_unify(lin, reg.dynamic_type()), "ac3430_4_dynamic_linear_reject");
        }

        {
            std::println("\n--- #3430 AC5: no invent / docs ---");
            std::ifstream d("docs/design/3430-production-defaults-force-strict.md");
            CHECK(!d.good(), "ac3430_5_no_docs");
            std::ifstream t("tests/issues/test_issue_3430.cpp");
            CHECK(!t.good(), "ac3430_5_no_invent");
            std::ifstream t2("tests/compiler/test_issue_3430.cpp");
            CHECK(!t2.good(), "ac3430_5_no_compiler_invent");
        }

        apply_dev_audit_defaults();
    }

    // ── #3698: check_flat Let/Define annotated value ---
    {
        using aura::compiler::kCheckFlatLetDefineAnnotationIssue;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;

        struct ProdScope {
            ProdScope() { apply_production_audit_defaults(); }
            ~ProdScope() { apply_dev_audit_defaults(); }
        };

        CHECK(kCheckFlatLetDefineAnnotationIssue == 3698, "3698 stamp");

        {
            std::println("\n--- #3698 AC1: production annotated let mismatch TypeError ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            infer_code("(check (letrec ((x (check \"hi\" : Int))) 1) : Int)",
                       aura::compiler::GradualPermissiveness::Strict, /*strict=*/false, diag);
            CHECK(has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch") ||
                      has_kind_msg(diag, aura::diag::ErrorKind::TypeError,
                                   "incompatible ground types"),
                  "3698 AC1: production letrec annotated mismatch TypeError");
        }
        {
            std::println("\n--- #3698 AC2: production define body mismatch TypeError ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            infer_code("(check (define d3698 \"hi\") : Int)",
                       aura::compiler::GradualPermissiveness::Strict, /*strict=*/false, diag);
            CHECK(has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch") ||
                      has_kind_msg(diag, aura::diag::ErrorKind::TypeError,
                                   "incompatible ground types"),
                  "3698 AC2: production define value vs Int TypeError");
        }
        {
            std::println("\n--- #3698 AC3: unannotated let still infers ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            infer_code("(let ((x 1)) (+ x 2))", aura::compiler::GradualPermissiveness::Strict,
                       /*strict=*/false, diag);
            CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch"),
                  "3698 AC3: unannotated let no TypeError");
        }
        {
            std::println("\n--- #3698 AC5: Soft unannotated no extra TypeError ---");
            apply_dev_audit_defaults();
            aura::diag::DiagnosticCollector diag;
            infer_code("(let ((x \"hi\")) x)", aura::compiler::GradualPermissiveness::Balanced,
                       /*strict=*/false, diag);
            CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch"),
                  "3698 AC5: Soft unannotated let no extra TypeError");
        }
        apply_dev_audit_defaults();
    }

    // ── #3871: unannotated LetRec check_flat walks the value ──
    {
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;

        struct ProdScope {
            ProdScope() { apply_production_audit_defaults(); }
            ~ProdScope() { apply_dev_audit_defaults(); }
        };

        {
            std::println("\n--- #3871 AC1: unannotated letrec value error reported ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            aura::core::TypeRegistry reg;
            aura::compiler::InferenceEngine eng(reg, diag);
            eng.set_strict(true);
            eng.set_gradual_permissiveness(aura::compiler::GradualPermissiveness::Strict);
            aura::ast::ASTArena arena;
            auto alloc = arena.allocator();
            aura::ast::StringPool pool(alloc);
            aura::ast::FlatAST flat(alloc);
            // Discriminator: the error lives ONLY inside the unannotated
            // letrec value — (set! g "hi") against the outer g : Integer
            // fires the #3407 synthesize-Set ground mismatch, which requires
            // the value to actually be walked (the #3871 else arm).
            auto pr = aura::parser::parse_to_flat(
                "(let ((g 0)) (letrec ((x (begin (set! g \"hi\") 1))) 1))", flat, pool);
            CHECK(pr.success && pr.root != aura::ast::NULL_NODE, "3871 AC1: parse ok");
            if (pr.success && pr.root != aura::ast::NULL_NODE) {
                flat.root = pr.root;
                (void)eng.check_flat(flat, pool, pr.root, reg.int_type());
                CHECK(has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch") ||
                          has_kind_msg(diag, aura::diag::ErrorKind::TypeError,
                                       "incompatible ground types"),
                      "3871 AC1: letrec unannotated value walked (was skipped)");
            }
        }
        {
            std::println("\n--- #3871 AC2: well-typed letrec check still green ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            aura::core::TypeRegistry reg;
            aura::compiler::InferenceEngine eng(reg, diag);
            eng.set_strict(true);
            eng.set_gradual_permissiveness(aura::compiler::GradualPermissiveness::Strict);
            aura::ast::ASTArena arena;
            auto alloc = arena.allocator();
            aura::ast::StringPool pool(alloc);
            aura::ast::FlatAST flat(alloc);
            auto pr = aura::parser::parse_to_flat("(letrec ((x (+ 1 1))) x)", flat, pool);
            CHECK(pr.success && pr.root != aura::ast::NULL_NODE, "3871 AC2: parse ok");
            if (pr.success && pr.root != aura::ast::NULL_NODE) {
                flat.root = pr.root;
                (void)eng.check_flat(flat, pool, pr.root, reg.int_type());
                CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch"),
                      "3871 AC2: no false positive on letrec check");
            }
        }
        {
            std::println("\n--- #3871 AC3: letrec synthesize path unchanged ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            infer_code("(letrec ((x 1)) (+ x 2))", aura::compiler::GradualPermissiveness::Strict,
                       /*strict=*/false, diag);
            CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch"),
                  "3871 AC3: letrec synthesize/infer unchanged");
        }
        {
            std::println("\n--- #3871 AC4: source-cite + no invent ---");
            std::ifstream tci("src/compiler/type_checker_impl.cpp");
            CHECK(tci.good(), "3871 AC4: open type_checker_impl.cpp");
            std::string src((std::istreambuf_iterator<char>(tci)),
                            std::istreambuf_iterator<char>());
            const auto check_pos = src.find("InferenceEngine::check_flat(");
            CHECK(check_pos != std::string::npos, "3871 AC4: check_flat present");
            const auto check_after = src.substr(check_pos);
            const auto let_pos = check_after.find("NodeTag::Let || v.tag == NodeTag::LetRec");
            CHECK(let_pos != std::string::npos, "3871 AC4: Let/LetRec branch");
            const auto begin_pos = check_after.find("NodeTag::Begin", let_pos);
            const auto let_branch = check_after.substr(
                let_pos, begin_pos == std::string::npos ? std::string::npos : begin_pos - let_pos);
            CHECK(let_branch.find("Issue #3871") != std::string::npos, "3871 AC4: cites #3871");
            CHECK(let_branch.find("} else if (!is_rec)") == std::string::npos,
                  "3871 AC4: !is_rec guard dropped");
            CHECK(let_branch.find("consistent_unify(rec_fwd, val_type)") != std::string::npos,
                  "3871 AC4: rec forward unify present");
            std::ifstream d("docs/design/3871-letrec-check-synth.md");
            CHECK(!d.good(), "3871 AC4: no docs/design");
            std::ifstream t("tests/compiler/test_issue_3871.cpp");
            CHECK(!t.good(), "3871 AC4: no invent");
        }
        apply_dev_audit_defaults();
    }

    // ── #3700: Production Quote walks children ---
    {
        using aura::compiler::kBidirectionalQuoteWalkIssue;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;

        struct ProdScope {
            ProdScope() { apply_production_audit_defaults(); }
            ~ProdScope() { apply_dev_audit_defaults(); }
        };

        CHECK(kBidirectionalQuoteWalkIssue == 3700, "3700 stamp");
        {
            std::println("\n--- #3700 AC1: production quoted (+ 1 \"x\") TypeError ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            infer_code("(quote ((lambda ((x : Int)) x) \"hi\"))",
                       aura::compiler::GradualPermissiveness::Strict, /*strict=*/false, diag);
            CHECK(has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch") ||
                      has_kind_msg(diag, aura::diag::ErrorKind::TypeError,
                                   "incompatible ground types") ||
                      has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "argument"),
                  "3700 AC1: inner Call Int~String TypeError");
        }
        {
            std::println("\n--- #3700 AC2: Quote in expected Int TypeError ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            infer_code("(check (quote 1) : Int)", aura::compiler::GradualPermissiveness::Strict,
                       /*strict=*/false, diag);
            CHECK(has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch") ||
                      has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "Quote"),
                  "3700 AC2: Quote vs Int TypeError");
        }
        {
            std::println("\n--- #3700 AC3: Soft Quote no extra TypeError ---");
            apply_dev_audit_defaults();
            aura::diag::DiagnosticCollector diag;
            infer_code("(quote ((lambda ((x : Int)) x) \"hi\"))",
                       aura::compiler::GradualPermissiveness::Balanced, /*strict=*/false, diag);
            CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "type mismatch"),
                  "3700 AC3: Soft Quote children not required");
        }
        apply_dev_audit_defaults();
    }

    // ── #3921: display accepts String/Bool under production (no Any spam) ──
    {
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;

        struct ProdScope {
            ProdScope() { apply_production_audit_defaults(); }
            ~ProdScope() { apply_dev_audit_defaults(); }
        };

        {
            std::println("\n--- #3921 AC1: production (display \"hello\") no expected Any ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            infer_code("(display \"hello\")", aura::compiler::GradualPermissiveness::Strict,
                       /*strict=*/false, diag);
            CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "expected Any"),
                  "3921 AC1: display String no expected Any");
        }
        {
            std::println("\n--- #3921 AC2: production (display #t) no expected Any ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            infer_code("(display #t)", aura::compiler::GradualPermissiveness::Strict,
                       /*strict=*/false, diag);
            CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "expected Any"),
                  "3921 AC2: display Bool no expected Any");
        }
        {
            std::println("\n--- #3921 AC2b: production display of set! Any cell ---");
            ProdScope prod;
            aura::diag::DiagnosticCollector diag;
            infer_code("(let ((x #f)) (begin (set! x \"hi\") (display x)))",
                       aura::compiler::GradualPermissiveness::Strict, /*strict=*/false, diag);
            CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "expected Any") &&
                      !has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "expected __t"),
                  "3921 AC2b: display Any-typed cell no type error");
        }
        {
            std::println("\n--- #3921 AC3: Soft display String still silent ---");
            apply_dev_audit_defaults();
            aura::diag::DiagnosticCollector diag;
            infer_code("(display \"hello\")", aura::compiler::GradualPermissiveness::Balanced,
                       /*strict=*/false, diag);
            CHECK(!has_kind_msg(diag, aura::diag::ErrorKind::TypeError, "expected Any"),
                  "3921 AC3: Soft display String no type error");
        }
        {
            std::println("\n--- #3921 AC4: source-cite poly display ---");
            std::ifstream tci("src/compiler/type_checker_impl.cpp");
            std::string src((std::istreambuf_iterator<char>(tci)),
                            std::istreambuf_iterator<char>());
            CHECK(src.find("#3921") != std::string::npos, "3921 AC4: cites #3921");
            CHECK(src.find("register_poly_primitive(\"display\"") != std::string::npos,
                  "3921 AC4: display is ∀a. a → Void");
            std::ifstream d("docs/design/3921-display-any.md");
            CHECK(!d.good(), "3921 AC4: no docs/design");
            std::ifstream t("tests/compiler/test_issue_3921.cpp");
            CHECK(!t.good(), "3921 AC4: no invent");
        }
        apply_dev_audit_defaults();
    }

    if (g_failed == 0) {
        std::println("\n=== ALL ACs PASS ===");
        return 0;
    }
    std::println("\n=== {} ACs FAILED ===", g_failed);
    return 1;
}

int main() {
    return aura_issue_1413_run();
}
