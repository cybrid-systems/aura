#include <iostream>
#include <string>
#include <memory>
#include <unistd.h>
#include <cstdio>

import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;

int main(int argc, char* argv[]) {
    aura::ast::ASTArena arena;
    aura::compiler::Evaluator evaluator;
    evaluator.set_arena(&arena);

    // REPL mode
    if (argc == 1 && isatty(fileno(stdin))) {
        std::cout << "Aura v0.1" << std::endl;
        std::string line;
        while (std::cout << "> ", std::getline(std::cin, line)) {
            if (line.empty()) continue;
            aura::parser::Parser parser(arena);
            auto pr = parser.parse(line);
            if (!pr.success || !pr.root) {
                std::cerr << "parse error: " << pr.error << std::endl;
                continue;
            }
            auto result = evaluator.eval(pr.root);
            if (!result.success)
                std::cerr << "eval error: " << result.error << std::endl;
            else
                std::cout << result.int_value << std::endl;
        }
        return 0;
    }

    // Pipe mode: read all, split by line
    std::vector<std::string> expressions;
    std::string line;
    while (std::getline(std::cin, line))
        expressions.push_back(line);

    if (expressions.empty()) {
        std::cout << "usage: echo '(+ 1 2)' | ./aura" << std::endl;
        return 1;
    }

    bool any_error = false;
    for (auto& expr : expressions) {
        // Trim
        size_t s = expr.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        expr = expr.substr(s);

        aura::parser::Parser parser(arena);
        auto pr = parser.parse(expr);
        if (!pr.success || !pr.root) {
            std::cerr << "parse error: " << pr.error << std::endl;
            any_error = true;
            continue;
        }
        auto result = evaluator.eval(pr.root);
        if (!result.success) {
            std::cerr << "eval error: " << result.error << std::endl;
            any_error = true;
        } else {
            std::cout << result.int_value << std::endl;
        }
    }

    return any_error ? 1 : 0;
}
