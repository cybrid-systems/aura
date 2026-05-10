import <iostream>;
import <string>;

import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;

int main(int argc, char* argv[]) {
    std::string input;
    if (argc > 1 && std::string_view(argv[1]) != "--eval") {
        input = argv[1];
    } else {
        std::string line;
        while (std::getline(std::cin, line)) input += line;
    }

    if (input.empty()) {
        std::cerr << "usage: echo '42' | ./aura" << std::endl;
        return 1;
    }

    aura::ast::ASTArena arena;
    aura::parser::Parser parser(arena);
    auto pr = parser.parse(input);

    if (!pr.success || !pr.root) {
        std::cerr << "parse error: " << pr.error << std::endl;
        return 1;
    }

    aura::compiler::Evaluator ev;
    auto result = ev.eval(pr.root);

    if (!result.success) {
        std::cerr << "eval error: " << result.error << std::endl;
        return 1;
    }

    std::cout << result.int_value << std::endl;
    return 0;
}
