import std;
import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_interpreter;

int main() {
    aura::ast::ASTArena arena;
    aura::parser::Parser parser(arena);
    aura::compiler::Evaluator evaluator;
    evaluator.set_arena(&arena);

    // Test cases: (input, expected)
    struct Test { std::string input; std::int64_t expected; };
    Test tests[] = {
        {"42", 42},
        {"(+ 1 2)", 3},
        {"(* 2 3)", 6},
        {"(if 1 42 0)", 42},
        {"(if 0 0 99)", 99},
        {"(+ 1 (* 2 3))", 7},
    };

    int passed = 0, failed = 0;
    for (auto& t : tests) {
        auto pr = parser.parse(t.input);
        if (!pr.success || !pr.root) {
            std::cerr << "PARSE FAIL: " << t.input << std::endl;
            ++failed; continue;
        }

        // Lower to IR
        aura::compiler::LoweringPass lowering(arena);
        auto ir_func = lowering.lower(pr.root);

        // Execute via IR interpreter
        aura::compiler::IRInterpreter ir_interp(evaluator.top_env(), *evaluator.primitives_for_ir());
        auto result = ir_interp.execute(ir_func);

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
