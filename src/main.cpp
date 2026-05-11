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

// JSON helper: wrap a string value for JSON (escape quotes and backslashes)
static std::string json_escape(std::string_view s) {
    std::string out;
    for (auto c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') { out += "\\n"; }
        else { out += c; }
    }
    return out;
}

int main(int argc, char* argv[]) {
    // ── --serve: persistent JSON-line compile-fix loop ─────────
    // Each line of output is JSON. Agent reads with JSON.parse(line).
    // Messages: ok, error, fix, fixed, fix-fail
    if (argc > 1 && std::string_view(argv[1]) == "--serve") {
        aura::compiler::CompilerService cs;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;

            auto alloc = cs.arena().allocator();
            aura::ast::StringPool pool(alloc);
            aura::ast::FlatAST flat(alloc);

            auto pr = aura::parser::parse_to_flat(line, flat, pool);
            if (!pr.success || pr.root == aura::ast::NULL_NODE) {
                std::println("{{\"status\":\"parse-error\",\"input\":\"{}\"}}",
                             json_escape(line));
                continue;
            }
            flat.root = pr.root;

            auto r = cs.eval(line);
            if (r) {
                std::println("{{\"status\":\"ok\",\"value\":{}}}", *r);
            } else {
                auto& d = r.error();
                std::println("{{\"status\":\"error\",\"kind\":{},\"msg\":\"{}\",\"node_id\":{}}}",
                             static_cast<int>(d.kind),
                             json_escape(d.message), d.node_id);

                // Auto-fix
                aura::compiler::AutoFixEngine fixer(flat, pool);
                fixer.add_error_fix(d.kind);
                auto patches = fixer.run_all();
                if (patches > 0) {
                    std::println("{{\"status\":\"fix\",\"patches\":{}}}", patches);
                    auto mod = aura::compiler::lower_to_ir(flat, pool, cs.arena());
                    aura::compiler::Primitives prims;
                    aura::compiler::IRInterpreter interp(mod, prims);
                    auto fixed = interp.execute();
                    if (fixed) {
                        std::println("{{\"status\":\"fixed\",\"value\":{}}}", *fixed);
                    } else {
                        std::println("{{\"status\":\"fix-fail\",\"msg\":\"{}\"}}",
                                     json_escape(fixed.error().message));
                    }
                }
            }
        }
        return 0;
    }

    // ── --ir: lower to IR and execute ─────────────────────────────
    if (argc > 1 && std::string_view(argv[1]) == "--ir") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) { input = argv[2]; }
        else { std::getline(std::cin, input); }
        auto result = cs.eval_ir(input);
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().message);
            return 1;
        }
        std::println("{}", *result);
        return 0;
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
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) { std::println(std::cerr, "parse error"); return 1; }
        flat.root = pr.root;
        aura::compiler::QueryEngine engine(flat, pool);
        auto results = engine.query(argv[2]);
        std::println("query: {} matches", results.size());
        for (auto id : results)
            std::println("  node[{}]: tag={}", id, static_cast<int>(flat.get(id).tag));
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
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) { std::println(std::cerr, "parse error"); return 1; }
        flat.root = pr.root;
        aura::compiler::QueryEngine engine(flat, pool);
        aura::compiler::TransformEngine xform(flat, pool);
        auto result = xform.query_and_fix(engine, argv[2], argv[3]);
        std::println("transform: {} matches, {} patches, applied={}",
                     result.match_count, result.patch_count, result.applied);
        return result.applied ? 0 : 1;
    }

    // ── --auto-fix: run built-in optimization fixes ──────────────
    if (argc > 1 && std::string_view(argv[1]) == "--auto-fix") {
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        std::string input;
        if (argc > 2) input = argv[2];
        else std::getline(std::cin, input);
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) { std::println(std::cerr, "parse error"); return 1; }
        flat.root = pr.root;
        aura::compiler::AutoFixEngine fixer(flat, pool);
        fixer.add_default_rules();
        auto patches = fixer.run_all();
        std::println("auto-fix: {} patches applied", patches);
        auto mod = aura::compiler::lower_to_ir(flat, pool, arena);
        aura::compiler::Primitives prims;
        aura::compiler::IRInterpreter interp(mod, prims);
        auto eval = interp.execute();
        if (eval) std::println("result: {}", *eval);
        return 0;
    }

    // ── --abf: deserialize ABF binary and evaluate ────────────────
    if (argc > 1 && std::string_view(argv[1]) == "--abf") {
        std::vector<std::byte> data;
        std::array<char, 4096> buf;
        while (std::cin.read(buf.data(), buf.size()).gcount() > 0) {
            for (auto i = decltype(buf)::difference_type(0); i < std::cin.gcount(); ++i)
                data.push_back(static_cast<std::byte>(buf[i]));
        }
        if (data.empty()) { std::println(std::cerr, "aura --abf: no input"); return 1; }
        aura::compiler::CompilerService cs;
        aura::binary::ABFDeserializer des(cs.arena());
        try {
            auto* expr = des.deserialize(data);
            auto result = cs.evaluator().eval(expr);
            if (!result) { std::println(std::cerr, "eval error: {}", result.error().message); return 1; }
            std::println("{}", *result);
        } catch (const std::exception& e) { std::println(std::cerr, "error: {}", e.what()); return 1; }
        return 0;
    }

    // ── Normal REPL / pipe mode (tree-walker) ─────────────────────
    aura::compiler::CompilerService cs;
    bool interactive = false;
    try { std::cin.sync(); interactive = (argc == 1 && std::cin.peek() == std::char_traits<char>::eof()); }
    catch (...) {}
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

    std::vector<std::string> exprs;
    std::string line;
    while (std::getline(std::cin, line)) exprs.push_back(line);
    if (exprs.empty()) { std::println(std::cerr, "usage: echo '(+ 1 2)' | ./aura"); return 1; }
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
