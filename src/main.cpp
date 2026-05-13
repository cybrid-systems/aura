import std;
import aura.core;
import aura.compiler.service;
import aura.compiler.query;
import aura.compiler.lowering;
import aura.compiler.ir_interpreter;
import aura.compiler.frontend;
import aura.core.ast_flat;
import aura.core.ast_pool;
import aura.core.type;
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
            aura::core::TypeRegistry tr;
            flat.resolve_type_ids(tr, pool);

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
        aura::core::TypeRegistry tr;
        flat.resolve_type_ids(tr, pool);
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
        aura::core::TypeRegistry tr;
        flat.resolve_type_ids(tr, pool);
        aura::compiler::QueryEngine engine(flat, pool);
        aura::compiler::TransformEngine xform(flat, pool);
        auto result = xform.query_and_fix(engine, argv[2], argv[3]);
        std::println("transform: {} matches, {} patches, applied={}",
                     result.match_count, result.patch_count, result.applied);
        return result.applied ? 0 : 1;
    }

    // ── --typecheck: run compile-time type checking ────────────
    if (argc > 1 && std::string_view(argv[1]) == "--typecheck") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) { input = argv[2]; }
        else { std::getline(std::cin, input); }
        auto result = cs.typecheck(input);
        std::print("{}", result);
        return result.find("diagnostics:") == std::string::npos ? 0 : 1;
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
        aura::core::TypeRegistry tr;
        flat.resolve_type_ids(tr, pool);
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

    // ── --strategy: set eval strategy before executing ─────────────
    // Usage: echo '(let ...)' | ./aura --strategy 'max_unroll=5' --ir
    if (argc > 1 && std::string_view(argv[1]) == "--strategy") {
        // Note: strategy parsing is simple placeholder
        aura::compiler::CompilerService cs;
        if (argc > 2) {
            aura::compiler::EvalStrategy s;
            if (std::string_view(argv[2]) == "no-inline") s.enable_inlining = false;
            if (std::string_view(argv[2]) == "specialize") s.enable_specialization = true;
            cs.set_strategy(s);
        }
        // Forward remaining args would re-dispatch to --ir
        // (not yet implemented — full strategy CLI / config passthrough pending)
        // Re-dispatch to --ir with the modified CompilerService
        // For now, fall through to --ir
        // This is a stub — full strategy CLI will come with proper arg parsing
        std::println(std::cerr, "strategy set, re-run with --ir or --inspect");
        return 0;
    }

    // ── --inspect: eval with full runtime reflection dump ────────
    if (argc > 1 && std::string_view(argv[1]) == "--inspect") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) { input = argv[2]; }
        else { std::getline(std::cin, input); }

        cs.set_strategy({.enable_inlining = true, .enable_specialization = false,
                         .max_unroll = 3, .verbose_inspect = true});

        auto result = cs.eval_ir(input);

        if (!result) {
            std::println(std::cerr, "error: {}", result.error().message);
        } else {
            std::println("result: {}", *result);
        }

        // ── Environment dump ─────────────────────────────────────
        auto closures = cs.last_closures();
        auto cells = cs.last_cells();

        std::println("┌─ closures ({})", closures.size());
        for (auto& c : closures) {
            std::println("├ [{}] func[{}] '{}'", c.id, c.func_id, c.func_name);
            // Free variables (captured from enclosing scope)
            if (!c.func_free_vars.empty()) {
                std::println("│   free-vars:");
                for (std::size_t i = 0; i < c.func_free_vars.size() && i < c.env.size(); ++i) {
                    std::println("│     {} : {}", c.func_free_vars[i], c.env[i]);
                }
            }
            // Parameters
            if (!c.func_params.empty()) {
                std::println("│   params:");
                for (auto& p : c.func_params) {
                    std::println("│     {}", p);
                }
            }
            // Remaining env (non-free-var values)
            if (c.env.size() > c.func_free_vars.size()) {
                std::println("│   extra env slots:");
                for (std::size_t i = c.func_free_vars.size(); i < c.env.size(); ++i) {
                    std::println("│     [{}] = {}", i, c.env[i]);
                }
            }
        }

        std::println("┌─ cells ({})", cells.size());
        for (auto& c : cells) {
            std::println("├ [{}] = {}", c.id, c.value);
        }

        return result ? 0 : 1;
    }

    // ── --env: compact cell/closure state dump ───────────────────
    if (argc > 1 && std::string_view(argv[1]) == "--env") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) { input = argv[2]; }
        else { std::getline(std::cin, input); }

        auto result = cs.eval_ir(input);
        if (result) {
            std::println("result: {}", *result);
        } else {
            std::println(std::cerr, "error: {}", result.error().message);
        }

        auto closures = cs.last_closures();
        auto cells = cs.last_cells();

        for (auto& c : closures) {
            std::println("closure [{}] = func[{}] '{}'",
                         c.id, c.func_id, c.func_name);
            for (std::size_t i = 0; i < c.env.size(); ++i) {
                auto label = i < c.func_free_vars.size()
                           ? c.func_free_vars[i] : std::format("[{}]", i);
                std::println("  {} = {}", label, c.env[i]);
            }
        }

        for (auto& c : cells) {
            std::println("cell [{}] = {}", c.id, c.value);
        }

        return result ? 0 : 1;
    }

    // ── --env-json: machine-readable JSON env dump ────────────────
    if (argc > 1 && std::string_view(argv[1]) == "--env-json") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) { input = argv[2]; }
        else { std::getline(std::cin, input); }

        auto result = cs.eval_ir(input);
        auto closures = cs.last_closures();
        auto cells = cs.last_cells();

        std::println("{{\"status\":\"{}\",\"result\":{},\"closures\":[",
                     result ? "ok" : "error",
                     result ? std::to_string(*result) : std::string("null"));

        for (std::size_t i = 0; i < closures.size(); ++i) {
            auto& c = closures[i];
            std::println("  {{\"id\":{},\"func_id\":{},\"func\":\"{}\",",
                         c.id, c.func_id, c.func_name);
            std::println("   \"env\":[");
            for (std::size_t j = 0; j < c.env.size(); ++j) {
                auto label = j < c.func_free_vars.size()
                           ? c.func_free_vars[j] : std::format("[{}]", j);
                std::println("    {{\"name\":\"{}\",\"value\":{}}}{}",
                             label, c.env[j],
                             j + 1 < c.env.size() ? "," : "");
            }
            std::println("   ]}}{}", i + 1 < closures.size() ? "," : "");
        }

        std::println("],\"cells\":[");
        for (std::size_t i = 0; i < cells.size(); ++i) {
            std::println("  {{\"id\":{},\"value\":{}}}{}",
                         cells[i].id, cells[i].value,
                         i + 1 < cells.size() ? "," : "");
        }
        std::println("]}}");
        return result ? 0 : 1;
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
