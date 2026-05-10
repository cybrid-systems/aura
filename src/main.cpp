#include <iostream>
#include <string>

import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;

int main(int argc, char* argv[]) {
    std::string input;
    if (argc > 1) {
        input = argv[1];
    } else {
        std::string line;
        while (std::getline(std::cin, line)) {
            input += line;
        }
    }

    if (input.empty()) {
        std::cerr << "usage: echo '42' | ./aura" << std::endl;
        return 1;
    }

    aura::ast::ASTArena arena;
    aura::parser::Parser parser(arena);
    auto parse_result = parser.parse(input);

    if (!parse_result.success || !parse_result.root) {
        std::cerr << "parse error: " << parse_result.error << std::endl;
        return 1;
    }

    aura::compiler::Evaluator evaluator;
    evaluator.set_arena(&arena);
    auto result = evaluator.eval(parse_result.root);

    if (!result.success) {
        std::cerr << "eval error: " << result.error << std::endl;
        return 1;
    }

    std::cout << result.int_value << std::endl;
    return 0;
}
