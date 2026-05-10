import std;
import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_interpreter;
import aura.compiler.compute_kind;
import aura.compiler.arity;

// Run compute-kind analysis on the top function and return a summary string
std::string check_compute_kind(aura::compiler::LoweringPass& lowering,
                                const std::string& input) {
    aura::ast::ASTArena arena;
    aura::parser::Parser parser(arena);
    auto pr = parser.parse(input);
    if (!pr.root) return "parse_fail";

    auto mod = lowering.lower(pr.root);
    auto& top_func = mod.entry();

    aura::compiler::ComputeKindAnalysis analysis;
    auto result = analysis.analyze(top_func);
    if (!result.valid) return "invalid";

    // Build a compact string per instruction: K=Known, U=Unknown
    std::string summary;
    for (std::size_t bi = 0; bi < result.per_block_inst_kind.size(); ++bi) {
        if (bi > 0) summary += '|';
        for (auto k : result.per_block_inst_kind[bi]) {
            summary += (k == aura::compiler::ComputeKind::Known) ? 'K' : 'U';
        }
    }
    return summary;
}

int main() {
    aura::ast::ASTArena arena;
    aura::compiler::Evaluator evaluator;
    evaluator.set_arena(&arena);
    aura::compiler::LoweringPass lowering(arena);

    // Test cases: (input, expected)
    struct Test { std::string input; std::int64_t expected; };
    Test tests[] = {
        // Literals and arithmetic
        {"42", 42},
        {"(+ 1 2)", 3},
        {"(* 2 3)", 6},
        {"(+ 1 (* 2 3))", 7},
        {"(- 5 3)", 2},
        {"(/ 6 2)", 3},

        // Comparisons
        {"(= 1 1)", 1},
        {"(= 1 2)", 0},
        {"(< 1 2)", 1},
        {"(> 2 1)", 1},

        // Conditionals
        {"(if 1 42 0)", 42},
        {"(if 0 0 99)", 99},
        {"(if (> 3 2) 1 0)", 1},

        // Let bindings
        {"(let ((x 10)) x)", 10},
        {"(let ((x 10)) (let ((y 20)) (+ x y)))", 30},

        // Lambda + application (tree-walker style closures)
        {"((lambda (x) (* x 2)) 5)", 10},
        {"((lambda (x y) (+ x y)) 3 4)", 7},

        // Closure with free variable
        {"(let ((x 10)) ((lambda (y) (+ x y)) 5))", 15},

        // Nested let + lambda
        {"(let ((x 2)) (let ((f (lambda (y) (* x y)))) (f 3)))", 6},
    };

    int passed = 0, failed = 0;
    for (auto& t : tests) {
        arena.reset();
        aura::parser::Parser parser(arena);
        auto pr = parser.parse(t.input);
        if (!pr.success || !pr.root) {
            std::cerr << "PARSE FAIL: " << t.input << std::endl;
            ++failed; continue;
        }

        // Lower to IR
        auto ir_mod = lowering.lower(pr.root);

        // Execute via IR interpreter
        aura::compiler::IRInterpreter ir_interp(ir_mod, evaluator.primitives());
        auto result = ir_interp.execute();

        if (result.success && result.int_value == t.expected) {
            ++passed;
        } else {
            std::cerr << "FAIL: " << t.input;
            if (!result.success) std::cerr << " error: " << result.error;
            else std::cerr << " got " << result.int_value << " expected " << t.expected;
            std::cerr << std::endl;
            ++failed;
        }
    }

    std::println("IR test: {}/{} passed", passed, passed + failed);

    if (failed > 0) return 1;

    // ── L2.3: compute-kind analysis tests ─────────────────────
    struct CKTest { std::string input; std::string desc; };
    CKTest ck_tests[] = {
        {"42", "literal"},
        {"(+ 1 2)", "add_const"},
        {"(let ((x 10)) x)", "let_known"},
        {"((lambda (x) (* x 2)) 5)", "lambda_call"},
    };

    int ck_passed = 0, ck_failed = 0;
    for (auto& t : ck_tests) {
        auto summary = check_compute_kind(lowering, t.input);
        // Verify: first N-1 instructions (everything except Return) should contain at least one K
        // and the last char should be 'U' (Return is always Unknown in our analysis)
        if (summary.empty()) {
            std::cerr << "CK FAIL: " << t.desc << " empty summary" << std::endl;
            ++ck_failed;
        } else if (summary.back() == 'U' && summary.find('K') != std::string::npos) {
            ++ck_passed;
            std::println("CK OK:  {} → {} (\"{}\")", t.desc, summary, t.input);
        } else {
            std::cerr << "CK FAIL: " << t.desc << " summary='" << summary << "'" << std::endl;
            ++ck_failed;
        }
    }

    std::println("Compute-kind test: {}/{}/{} passed/failed/total",
                 ck_passed, ck_failed, ck_passed + ck_failed);

    // ── L2.4: arity checking tests ───────────────────────────
    aura::compiler::ArityChecker arity_checker;

    struct ArityTest { std::string input; bool expect_error; std::string desc; };
    ArityTest arity_tests[] = {
        // Correct arity: (lambda (x) ...) called with 1 arg → OK
        {"((lambda (x) (* x 2)) 5)", false, "correct_1arg"},
        // Correct arity: (lambda (x y) ...) called with 2 args → OK
        {"((lambda (x y) (+ x y)) 3 4)", false, "correct_2arg"},
        // Wrong arity: (lambda (x) ...) called with 2 args → error
        {"((lambda (x) x) 1 2)", true, "wrong_arity_too_many"},
        // Wrong arity: (lambda (x y) ...) called with 1 arg → error
        {"((lambda (x y) (+ x y)) 5)", true, "wrong_arity_too_few"},
        // Zero-arg lambda: correct
        {"((lambda () 42))", false, "zero_arg_ok"},
    };

    int arity_passed = 0, arity_failed = 0;
    for (auto& t : arity_tests) {
        arena.reset();
        aura::parser::Parser parser(arena);
        auto pr = parser.parse(t.input);
        if (!pr.root) { std::cerr << "PARSE FAIL: " << t.input << std::endl; ++arity_failed; continue; }

        auto mod = lowering.lower(pr.root);
        auto result = arity_checker.check(mod);

        bool got_error = result.has_error;
        if (got_error == t.expect_error) {
            ++arity_passed;
            if (got_error) {
                std::println("ARITY OK: {} → {} (expected error: {})",
                             t.desc, result.diagnostics[0].message, t.input);
            } else {
                std::println("ARITY OK: {} → no error (\"{}\")", t.desc, t.input);
            }
        } else {
            std::cerr << "ARITY FAIL: " << t.desc << " (" << t.input << ")\n";
            if (got_error) {
                for (auto& d : result.diagnostics)
                    std::cerr << "  error: " << d.message << std::endl;
            } else {
                std::cerr << "  expected error but got none" << std::endl;
            }
            ++arity_failed;
        }
    }

    std::println("Arity test: {}/{}/{} passed/failed/total",
                 arity_passed, arity_failed, arity_passed + arity_failed);
    return (failed + ck_failed + arity_failed) > 0 ? 1 : 0;
}
