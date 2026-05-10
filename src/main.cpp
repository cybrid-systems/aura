import std;
import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;
import aura.binary.abf_deserializer;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_interpreter;

int main(int argc, char* argv[]) {
    // Check for --abf mode: read ABF binary from stdin, deserialize, evaluate
    if (argc > 1 && std::string_view(argv[1]) == "--abf") {
        // Read binary data from stdin
        std::vector<std::byte> data;
        std::array<char, 4096> buf;
        while (std::cin.read(buf.data(), buf.size()).gcount() > 0) {
            auto n = std::cin.gcount();
            for (auto it = buf.begin(); it != buf.begin() + n; ++it)
                data.push_back(static_cast<std::byte>(*it));
        }
        // Read remaining bytes after last block
        if (std::cin.gcount() > 0) {
            for (std::streamsize i = 0; i < std::cin.gcount(); ++i)
                data.push_back(static_cast<std::byte>(buf[static_cast<std::size_t>(i)]));
        }

        if (data.empty()) {
            std::cerr << "aura --abf: no input" << std::endl;
            return 1;
        }

        aura::ast::ASTArena arena;
        aura::binary::ABFDeserializer des(arena);
        aura::compiler::Evaluator evaluator;
        evaluator.set_arena(&arena);

        try {
            auto* expr = des.deserialize(data);
            auto result = evaluator.eval(expr);
            if (!result.success) {
                std::cerr << "eval error: " << result.error << std::endl;
                return 1;
            }
            std::cout << result.int_value << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << std::endl;
            return 1;
        }
        return 0;
    }

    // Normal REPL/pipe mode
    aura::ast::ASTArena arena;
    aura::compiler::Evaluator evaluator;
    evaluator.set_arena(&arena);

    // Check if stdin is interactive (REPL mode)
    bool interactive = false;
    try {
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
