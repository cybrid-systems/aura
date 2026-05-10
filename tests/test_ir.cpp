import std;
import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_interpreter;

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
    return failed ? 1 : 0;
}
