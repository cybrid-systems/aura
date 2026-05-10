import std;
import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;

int main(int argc, char* argv[]) {
    aura::ast::ASTArena arena;
    aura::compiler::Evaluator evaluator;
    evaluator.set_arena(&arena);

    // Check if stdin is interactive (REPL mode)
    bool interactive = false;
    try {
        // In a real application, use isatty; for now, REPL if no args and no piped input
        std::cin.sync();
        interactive = (argc == 1 && std::cin.peek() == std::char_traits<char>::eof());
    } catch (...) {}

    if (interactive) {
        std::println("Aura v0.1");
        std::string line;
        while (std::cout << "> ", std::getline(std::cin, line)) {
            if (line.empty()) continue;
            aura::parser::Parser parser(arena);
            auto pr = parser.parse(line);
            if (!pr.success || !pr.root) { std::cerr << "parse error\n"; continue; }
            auto r = evaluator.eval(pr.root);
            if (!r.success) std::cerr << "eval error: " << r.error << std::endl;
            else std::cout << r.int_value << std::endl;
        }
        return 0;
    }

    std::vector<std::string> exprs;
    std::string line;
    while (std::getline(std::cin, line)) exprs.push_back(line);
    if (exprs.empty()) { std::println("usage: echo '(+ 1 2)' | ./aura"); return 1; }

    bool err = false;
    for (auto& e : exprs) {
        auto s = e.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        e = e.substr(s);
        aura::parser::Parser parser(arena);
        auto pr = parser.parse(e);
        if (!pr.success || !pr.root) { std::cerr << "parse error\n"; err = true; continue; }
        auto r = evaluator.eval(pr.root);
        if (!r.success) { std::cerr << "eval error: " << r.error << std::endl; err = true; }
        else std::cout << r.int_value << std::endl;
    }
    return err ? 1 : 0;
}
