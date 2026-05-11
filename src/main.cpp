import std;
import aura.core;
import aura.compiler.service;
import aura.compiler.query;
import aura.compiler.lowering;
import aura.compiler.ir_interpreter;
import aura.compiler.frontend;
import aura.core.ast_flat;
import aura.core.ast_pool;
import aura.parser.parser;
import aura.binary.abf_deserializer;

int main(int argc, char* argv[]) {
    // ── --auto-fix: run built-in optimization fixes ──────────────
    if (argc > 1 && std::string_view(argv[1]) == "--auto-fix") {
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);

        std::string input;
        if (argc > 2) { input = argv[2]; }
        else { std::getline(std::cin, input); }

        auto pr = aura::parser::parse(input, arena);
        if (!pr.success || !pr.root) {
            std::println(std::cerr, "parse error");
            return 1;
        }
        flat.root = aura::ast::flatten_to_flat(pr.root, flat, pool);

        aura::compiler::AutoFixEngine fixer(flat, pool);
        fixer.add_default_rules();
        auto patches = fixer.run_all();

        std::println("auto-fix: {} patches applied", patches);

        // Re-compile and show result
        auto mod = aura::compiler::lower_to_ir(flat, pool, arena);
        aura::compiler::Primitives prims;
        aura::compiler::IRInterpreter interp(mod, prims);
        auto eval = interp.execute();
        if (eval) std::println("result: {}", *eval);
        return 0;
    }

    // ── --query-and-fix: query + transform on parsed AST ─────────
    if (argc > 3 && std::string_view(argv[1]) == "--query-and-fix") {
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);

        std::string input;
        if (argc > 4) { input = argv[4]; }
        else { std::getline(std::cin, input); }

        auto pr = aura::parser::parse(input, arena);
        if (!pr.success || !pr.root) {
            std::println(std::cerr, "parse error");
            return 1;
        }
        flat.root = aura::ast::flatten_to_flat(pr.root, flat, pool);

        aura::compiler::QueryEngine engine(flat, pool);
        aura::compiler::TransformEngine xform(flat, pool);
        auto result = xform.query_and_fix(engine, argv[2], argv[3]);

        std::println("transform: {} matches, {} patches, applied={}",
                     result.match_count, result.patch_count, result.applied);
        if (!result.error.empty())
            std::println(std::cerr, "  error: {}", result.error);
        return result.applied ? 0 : 1;
    }

    // ── --query: run AuraQuery on parsed AST ─────────────────────
    if (argc > 2 && std::string_view(argv[1]) == "--query") {
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);

        std::string input;
        if (argc > 3) { input = argv[3]; }
        else { std::getline(std::cin, input); }

        auto pr = aura::parser::parse(input, arena);
        if (!pr.success || !pr.root) {
            std::println(std::cerr, "parse error");
            return 1;
        }
        flat.root = aura::ast::flatten_to_flat(pr.root, flat, pool);

        aura::compiler::QueryEngine engine(flat, pool);
        auto results = engine.query(argv[2]);

        std::println("query: {} matches", results.size());
        for (auto id : results) {
            auto v = flat.get(id);
            std::println("  node[{}]: tag={}", id, static_cast<int>(v.tag));
        }
        return 0;
    }

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
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().message);
            return 1;
        }
        std::println("{}", *result);
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
            if (!result) {
                std::println(std::cerr, "eval error: {}", result.error().message);
                return 1;
            }
            std::println("{}", *result);
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
            if (!r) std::println(std::cerr, "error: {}", r.error().message);
            else std::println("{}", *r);
            cs.reset();
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
        if (!r) { std::println(std::cerr, "error: {}", r.error().message); err = true; }
        else std::println("{}", *r);
        cs.reset();
    }
    return err ? 1 : 0;
}
