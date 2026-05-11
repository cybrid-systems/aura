import std;
import aura.core;
import aura.compiler.service;
import aura.binary.abf_deserializer;

int main(int argc, char* argv[]) {
    // ── --ir: lower to IR and execute ─────────────────────────────
    if (argc > 1 && std::string_view(argv[1]) == "--ir") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) {
            input = argv[2];
        } else {
            std::getline(std::cin, input);
        }
        auto result = cs.eval_ir(input);
        if (!result.success) {
            std::println(std::cerr, "error: {}", result.error);
            return 1;
        }
        std::println("{}", result.int_value);
        return 0;
    }

    // ── --abf: deserialize ABF binary and evaluate ────────────────
    if (argc > 1 && std::string_view(argv[1]) == "--abf") {
        std::vector<std::byte> data;
        std::array<char, 4096> buf;
        while (std::cin.read(buf.data(), buf.size()).gcount() > 0) {
            auto n = std::cin.gcount();
            for (auto it = buf.begin(); it != buf.begin() + n; ++it)
                data.push_back(static_cast<std::byte>(*it));
        }
        if (std::cin.gcount() > 0) {
            for (std::streamsize i = 0; i < std::cin.gcount(); ++i)
                data.push_back(static_cast<std::byte>(buf[static_cast<std::size_t>(i)]));
        }
        if (data.empty()) {
            std::println(std::cerr, "aura --abf: no input");
            return 1;
        }

        aura::compiler::CompilerService cs;
        aura::binary::ABFDeserializer des(cs.arena());
        try {
            auto* expr = des.deserialize(data);
            auto result = cs.evaluator().eval(expr);
            if (!result.success) {
                std::println(std::cerr, "eval error: {}", result.error);
                return 1;
            }
            std::println("{}", result.int_value);
        } catch (const std::exception& e) {
            std::println(std::cerr, "error: {}", e.what());
            return 1;
        }
        return 0;
    }

    // ── Normal REPL / pipe mode (tree-walker) ─────────────────────
    aura::compiler::CompilerService cs;

    bool interactive = false;
    try {
        std::cin.sync();
        interactive = (argc == 1 && std::cin.peek() == std::char_traits<char>::eof());
    } catch (...) {}

    if (interactive) {
        std::println("Aura v0.1");
        std::string line;
        while (std::print("> "), std::getline(std::cin, line)) {
            if (line.empty()) continue;
            auto r = cs.eval(line);
            if (!r.success) std::println(std::cerr, "error: {}", r.error);
            else std::println("{}", r.int_value);
            cs.reset();  // fresh arena for next REPL expression
        }
        return 0;
    }

    // Pipe mode: read lines from stdin
    std::vector<std::string> exprs;
    std::string line;
    while (std::getline(std::cin, line)) exprs.push_back(line);
    if (exprs.empty()) {
        std::println(std::cerr, "usage: echo '(+ 1 2)' | ./aura");
        return 1;
    }

    bool err = false;
    for (auto& e : exprs) {
        auto s = e.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        e = e.substr(s);
        auto r = cs.eval(e);
        if (!r.success) { std::println(std::cerr, "error: {}", r.error); err = true; }
        else std::println("{}", r.int_value);
        cs.reset();
    }
    return err ? 1 : 0;
}
