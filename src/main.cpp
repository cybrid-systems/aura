import std;
import aura.core;
import aura.compiler.service;
import aura.compiler.query;
import aura.compiler.lowering;
import aura.compiler.pass_manager;
import aura.compiler.ir_executor;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.type;
import aura.parser.parser;
import aura.compiler.ir;
import aura.compiler.cache;

// Format helper: format EvalValue with access to string heap
static std::string fmt_val(const aura::compiler::types::EvalValue& v,
                           aura::compiler::CompilerService& cs) {
    return aura::compiler::format_value(v, &cs.evaluator().primitives().string_heap(),
                                         &cs.evaluator().pairs(), 0,
                                         &cs.evaluator().primitives());
}

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

// Minimal JSON parser for --serve protocol messages.
// Only supports flat objects: {"key":"value","key2":"value2"}
// Returns an empty map on parse failure.
static std::unordered_map<std::string, std::string> parse_json_command(std::string_view line) {
    std::unordered_map<std::string, std::string> result;
    if (line.empty()) return result;

    auto p = line.data();
    auto end = p + line.size();

    // Skip whitespace and opening brace
    auto skip_ws = [&]() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    };

    skip_ws();
    if (p >= end || *p != '{') return result;
    ++p;

    while (p < end) {
        skip_ws();
        if (p >= end || *p == '}') break;

        // Parse key
        if (*p != '"') return result;
        ++p;
        std::string key;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                key += *p;
            } else {
                key += *p;
            }
            ++p;
        }
        if (p >= end) return result;
        ++p; // skip closing quote

        skip_ws();
        if (p >= end || *p != ':') return result;
        ++p;

        skip_ws();
        // Parse value (string or number/bool/null)
        std::string value;
        if (*p == '"') {
            // Quoted string
            ++p;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) {
                    ++p;
                    if (*p == 'n') value += '\n';
                    else if (*p == 't') value += '\t';
                    else if (*p == 'r') value += '\r';
                    else if (*p == '"') value += '"';
                    else if (*p == '\\') value += '\\';
                    else value += *p;
                } else {
                    value += *p;
                }
                ++p;
            }
            if (p >= end) return result;
            ++p; // skip closing quote
        } else if (*p == 't' || *p == 'f' || *p == 'n') {
            // true / false / null
            while (p < end && (*p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')) {
                value += *p++;
            }
        } else {
            // Number (int or float)
            while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                value += *p++;
            }
        }

        result[std::move(key)] = std::move(value);

        skip_ws();
        if (p >= end) break;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}') break;
        // Unexpected character
        return result;
    }

    return result;
}

int main(int argc, char* argv[]) {
    // ── --serve: persistent JSON-line compile-fix loop ─────────
    // Each line of output is JSON. Agent reads with JSON.parse(line).
    // Messages: ok, error, fix, fixed, fix-fail
    if (argc > 1 && std::string_view(argv[1]) == "--serve") {
        // ── Multi-session ────────────────────────────────────
        std::unordered_map<std::string, aura::compiler::CompilerService> sessions;
        std::string active_session = "default";
        sessions.try_emplace(active_session);
        auto& cs = sessions[active_session];

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;

            auto trimmed = line;
            auto first_non_space = trimmed.find_first_not_of(" \t");
            if (first_non_space != std::string::npos && trimmed[first_non_space] == '{') {
                // ── JSON command protocol ──────────────────────────
                auto json_input = trimmed.substr(first_non_space);
                auto cmd = parse_json_command(json_input);
                if (cmd.empty()) {
                    std::println("{{\"status\":\"parse-error\",\"msg\":\"invalid JSON command\"}}");
                    continue;
                }
                auto cmd_type = cmd.find("cmd");
                if (cmd_type == cmd.end()) {
                    std::println("{{\"status\":\"error\",\"msg\":\"missing cmd field\"}}");
                    continue;
                }
                auto& type = cmd_type->second;

                // Session management
                if (type == "session") {
                    auto name_it = cmd.find("name");
                    if (name_it == cmd.end()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing name field\"}}");
                        continue;
                    }
                    auto& sname = name_it->second;
                    auto real_name = (sname.find("new:") == 0) ? sname.substr(4) : sname;
                    if (!sessions.count(real_name)) {
                        sessions.try_emplace(real_name);
                        std::println("{{\"status\":\"created\",\"session\":\"{}\"}}", real_name);
                    } else {
                        std::println("{{\"status\":\"ok\",\"session\":\"{}\"}}", real_name);
                    }
                    active_session = (sname.find("new:") == 0) ? sname.substr(4) : sname;
                    continue;
                }

                // Module management (ArenaGroup)
                if (type == "module") {
                    auto action_it = cmd.find("action");
                    auto name_it = cmd.find("name");
                    if (action_it == cmd.end()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing action field\"}}");
                        continue;
                    }
                    auto& action = action_it->second;

                    if (action == "compile") {
                        auto code_it = cmd.find("code");
                        if (name_it == cmd.end() || code_it == cmd.end()) {
                            std::println("{{\"status\":\"error\",\"msg\":\"missing name or code field\"}}");
                            continue;
                        }
                        auto result = cs.compile_module(name_it->second, code_it->second);
                        if (result) {
                            std::println("{{\"status\":\"ok\",\"module\":\"{}\"}}",
                                        json_escape(name_it->second));
                        } else {
                            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                        json_escape(result.error().message));
                        }
                    } else if (action == "unload") {
                        if (name_it == cmd.end()) {
                            std::println("{{\"status\":\"error\",\"msg\":\"missing name field\"}}");
                            continue;
                        }
                        cs.unload_module(name_it->second);
                        std::println("{{\"status\":\"ok\",\"unloaded\":\"{}\"}}",
                                    json_escape(name_it->second));
                    } else if (action == "reload") {
                        if (name_it == cmd.end()) {
                            std::println("{{\"status\":\"error\",\"msg\":\"missing name field\"}}");
                            continue;
                        }
                        auto result = cs.reload_module(name_it->second);
                        if (result) {
                            std::println("{{\"status\":\"ok\",\"reloaded\":\"{}\"}}",
                                        json_escape(name_it->second));
                        } else {
                            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                        json_escape(result.error().message));
                        }
                    } else if (action == "list") {
                        auto modules = cs.loaded_modules();
                        std::println("{{\"status\":\"ok\",\"modules\":[");
                        bool first = true;
                        for (auto& m : modules) {
                            if (!first) std::println(",");
                            first = false;
                            std::print("  \"{}\"", json_escape(m));
                        }
                        std::println("]}}");
                    } else if (action == "stats") {
                        auto stats = cs.module_memory_stats();
                        std::println("{{\"status\":\"ok\",\"arenas\":[");
                        bool first = true;
                        for (auto& [name, s] : stats) {
                            if (!first) std::println(",");
                            first = false;
                            std::print("  {{\"name\":\"{}\",\"used\":{},\"capacity\":{}}}",
                                      json_escape(name), s.used, s.capacity);
                        }
                        std::println("]}}");
                    } else {
                        std::println("{{\"status\":\"error\",\"msg\":\"unknown action: {}\"}}",
                                    json_escape(action));
                    }
                    continue;
                }

                // Look up the current session
                auto& cs = sessions[active_session];

                // Commands that don't need a code field
                if (type == "mutate") {
                    // {"cmd": "mutate", "op": "record-patch", "node": 0, "op-name": "replace-type", "summary": "change type"}
                    auto op_it = cmd.find("op");          // mutation op name
                    auto node_it = cmd.find("node");       // target node ID
                    auto val_it = cmd.find("value");       // for replace-value
                    auto on_it = cmd.find("op-name");      // for record-patch: recorded op name
                    auto sum_it = cmd.find("summary");
                    if (op_it == cmd.end() || node_it == cmd.end()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing op or node\"}}");
                    } else {
                        auto op_name = op_it->second;
                        auto node = std::stoll(node_it->second);
                        std::string sexpr;
                        if (op_name == "mutate:record-patch") {
                            // (mutate:record-patch node op-name summary)
                            std::string on = on_it != cmd.end() ? on_it->second : "patch";
                            std::string s = sum_it != cmd.end() ? sum_it->second : "";
                            sexpr = std::format("(mutate:record-patch {} \"{}\" \"{}\")",
                                                node, on, s);
                        } else if (op_name == "mutate:replace-value") {
                            // (mutate:replace-value node new-value summary)
                            std::string v = val_it != cmd.end() ? val_it->second : "0";
                            std::string s = sum_it != cmd.end() ? sum_it->second : "";
                            sexpr = std::format("(mutate:replace-value {} {} \"{}\")",
                                                node, v, s);
                        } else if (op_name == "mutate:replace-type") {
                            // (mutate:replace-type node type-string)
                            std::string t = val_it != cmd.end() ? val_it->second : "Dyn";
                            sexpr = std::format("(mutate:replace-type {} \"{}\")", node, t);
                        } else {
                            // Generic: pass through with all fields
                            std::string v = val_it != cmd.end() ? val_it->second : "0";
                            std::string s = sum_it != cmd.end() ? sum_it->second : "";
                            sexpr = std::format("({} {} {} \"{}\")",
                                                op_name, node, v, s);
                        }
                        auto mut_result = cs.typed_mutate(sexpr);
                        if (mut_result.success) {
                            std::println("{{\"status\":\"ok\",\"mutation_id\":{}}}",
                                        mut_result.mutation_id);
                        } else {
                            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                        json_escape(mut_result.error.empty()
                                            ? "mutation failed" : mut_result.error));
                        }
                    }
                    continue;
                }
                if (type == "rollback") {
                    // {"cmd": "rollback", "id": 1}
                    auto id_it = cmd.find("id");
                    if (id_it == cmd.end()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing id\"}}");
                    } else {
                        auto sexpr = std::format("(rollback {})", id_it->second);
                        auto result = cs.eval_on_current(sexpr);
                        std::println("{{\"status\":\"ok\",\"rolled_back\":{}}}",
                                    result ? (is_bool(*result) ? (as_bool(*result) ? "true" : "false") : "false") : "false");
                    }
                    continue;
                }
                if (type == "mutation-log") {
                    // {"cmd": "mutation-log", "node": 0}
                    auto node_it = cmd.find("node");
                    if (node_it == cmd.end()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing node\"}}");
                    } else {
                        auto node = static_cast<aura::ast::NodeId>(std::stoul(node_it->second));
                        auto entries = cs.query_mutation_log(node);
                        std::println("{{\"status\":\"ok\",\"log\":[");
                        bool first = true;
                        for (auto& e : entries) {
                            if (!first) std::println(",");
                            first = false;
                            std::print("  {{\"id\":{},\"ts\":{},\"node\":{},\"op\":\"{}\","
                                       "\"old_type\":\"{}\",\"new_type\":\"{}\","
                                       "\"summary\":\"{}\",\"status\":\"{}\"}}",
                                       e.mutation_id, e.timestamp_ms, e.target_node,
                                       json_escape(e.operator_name),
                                       json_escape(e.old_type), json_escape(e.new_type),
                                       json_escape(e.summary), json_escape(e.status));
                        }
                        std::println("]}}");
                    }
                    continue;
                }

                // ── Configuration commands ─────────────────────────────
                if (type == "config") {
                    auto key_it = cmd.find("key");
                    auto val_it = cmd.find("value");
                    if (key_it != cmd.end() && val_it != cmd.end()) {
                        auto& key = key_it->second;
                        if (key == "strict") {
                            cs.set_strict_mode(val_it->second == "true" || val_it->second == "1");
                            auto val_str = val_it->second == "true" ? "true" : "false";
                            std::string out = "{\"status\":\"ok\",\"config\":{\""
                                + json_escape(key) + "\":" + val_str + "}}";
                            std::println("{}", out);
                            continue;
                        }
                        std::println("{{\"status\":\"error\",\"msg\":\"unknown config key: {}\"}}",
                                    json_escape(key));
                    } else {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing key or value fields\"}}");
                    }
                    continue;
                }

                {
                    auto code_it = cmd.find("code");
                    if (code_it == cmd.end()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing code field\"}}");
                        continue;
                    }
                    auto& code = code_it->second;

                if (type == "defmacro") {
                    auto result = cs.define_function(code);
                    if (result) {
                        auto name = cmd.count("name") ? cmd["name"] : "<macro>";
                        std::println("{{\"status\":\"defined\",\"name\":\"{}\"}}",
                                     json_escape(name));
                    } else {
                        auto& d = result.error();
                        std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                     json_escape(d.format()));
                    }
                }
                else if (type == "define") {
                    auto result = cs.define_function(code);
                    if (result) {
                        auto name = cmd.count("name") ? cmd["name"] : "<lambda>";
                        std::println("{{\"status\":\"defined\",\"name\":\"{}\"}}",
                                     json_escape(name));
                    } else {
                        auto& d = result.error();
                        std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                     json_escape(d.format()));
                    }
                }
                else if (type == "exec") {
                    auto result = cs.exec_with_cache(code);
                    if (result) {
                        std::println("{{\"status\":\"ok\",\"value\":\"{}\"}}",
                                     json_escape(fmt_val(*result, cs)));
                    } else {
                        auto& d = result.error();
                        std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                     json_escape(d.format()));
                    }
                }
                else if (type == "redefine") {
                    auto name = cmd.count("name") ? cmd["name"] : "<lambda>";
                    auto result = cs.define_function(code);
                    if (result) {
                        std::println("{{\"status\":\"redefined\",\"name\":\"{}\"}}",
                                     json_escape(name));
                    } else {
                        auto& d = result.error();
                        std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                     json_escape(d.format()));
                    }
                }
                else if (type == "unparse") {
                    auto alloc = cs.arena().allocator();
                    aura::ast::StringPool pool(alloc);
                    aura::ast::FlatAST flat(alloc);
                    auto ux_pr = aura::parser::parse_to_flat(code, flat, pool);
                    if (!ux_pr.success) {
                        std::println("{{\"status\":\"error\",\"msg\":\"parse error\"}}");
                    } else {
                        flat.root = ux_pr.root;
                        auto src = aura::compiler::unparse_node(flat, pool, flat.root);
                        std::println("{{\"status\":\"ok\",\"source\":\"{}\"}}",
                                     json_escape(src));
                    }
                }
                else if (type == "write") {
                    auto write_path = cmd.count("file") ? cmd["file"] : "";
                    if (write_path.empty()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing file field\"}}");
                    } else {
                        auto w_alloc = cs.arena().allocator();
                        aura::ast::StringPool w_pool(w_alloc);
                        aura::ast::FlatAST w_flat(w_alloc);
                        auto w_pr = aura::parser::parse_to_flat(code, w_flat, w_pool);
                        if (!w_pr.success) {
                            std::println("{{\"status\":\"error\",\"msg\":\"parse error\"}}");
                        } else {
                            w_flat.root = w_pr.root;
                            auto w_src = aura::compiler::unparse_node(w_flat, w_pool, w_flat.root);
                            std::ofstream w_f(write_path);
                            if (w_f) {
                                w_f << w_src << "\n";
                                std::println("{{\"status\":\"ok\",\"file\":\"{}\"}}",
                                             json_escape(write_path));
                            } else {
                                std::println("{{\"status\":\"error\",\"msg\":\"cannot write: {}\"}}",
                                             json_escape(write_path));
                            }
                        }
                    }
                }
                else if (type == "mutate") {
                    // {"cmd": "mutate", "op": "replace-value", "node": 0, "value": 42, "summary": "change x"}
                    auto op_it = cmd.find("op");
                    auto node_it = cmd.find("node");
                    auto val_it = cmd.find("value");
                    auto sum_it = cmd.find("summary");
                    if (op_it == cmd.end() || node_it == cmd.end()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing op or node\"}}");
                    } else {
                        auto node = std::stoll(node_it->second);
                        auto sexpr = std::format("({} {} {}{}{})",
                            op_it->second, node,
                            val_it != cmd.end() ? val_it->second : "0",
                            sum_it != cmd.end() ? " \"" + sum_it->second + "\"" : " \"\"",
                            "");
                        // Evaluate mutation against the persistent AST
                        auto mut_result = cs.typed_mutate(sexpr);
                        if (mut_result.success) {
                            std::println("{{\"status\":\"ok\",\"mutation_id\":{}}}",
                                        mut_result.mutation_id);
                        } else {
                            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                        json_escape(mut_result.error));
                        }
                    }
                }
                else if (type == "rollback") {
                    // {"cmd": "rollback", "id": 1}
                    auto id_it = cmd.find("id");
                    if (id_it == cmd.end()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing id\"}}");
                    } else {
                        auto sexpr = std::format("(rollback {})", id_it->second);
                        auto result = cs.eval_on_current(sexpr);
                        std::println("{{\"status\":\"ok\",\"rolled_back\":{}}}",
                                    result ? (is_bool(*result) ? (as_bool(*result) ? "true" : "false") : "false") : "false");
                    }
                }
                else if (type == "mutation-log") {
                    // {"cmd": "mutation-log", "node": 0}
                    auto node_it = cmd.find("node");
                    if (node_it == cmd.end()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing node\"}}");
                    } else {
                        auto node = static_cast<aura::ast::NodeId>(std::stoul(node_it->second));
                        auto entries = cs.query_mutation_log(node);
                        std::println("{{\"status\":\"ok\",\"log\":[");
                        bool first = true;
                        for (auto& e : entries) {
                            if (!first) std::println(",");
                            first = false;
                            std::print("  {{\"id\":{},\"ts\":{},\"node\":{},\"op\":\"{}\","
                                       "\"old_type\":\"{}\",\"new_type\":\"{}\","
                                       "\"summary\":\"{}\",\"status\":\"{}\"}}",
                                       e.mutation_id, e.timestamp_ms, e.target_node,
                                       json_escape(e.operator_name),
                                       json_escape(e.old_type), json_escape(e.new_type),
                                       json_escape(e.summary), json_escape(e.status));
                        }
                        std::println("]}}");
                    }
                }
                else {
                    std::println("{{\"status\":\"error\",\"msg\":\"unknown command: {}\"}}",
                                 json_escape(type));
                }
            } } else {
                // ── Plain S-expression (backward compatible) ────────
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
                    std::println("{{\"status\":\"ok\",\"value\":\"{}\"}}", json_escape(fmt_val(*r, cs)));
                } else {
                    auto& d = r.error();
                    std::println("{{\"status\":\"error\",\"kind\":{},\"msg\":\"{}\",\"node_id\":{}}}",
                                 static_cast<int>(d.kind),
                                 json_escape(d.format()), d.node_id);

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
                            std::println("{{\"status\":\"fixed\",\"value\":\"{}\"}}", aura::compiler::types::format_value(*fixed));
                        } else {
                            std::println("{{\"status\":\"fix-fail\",\"msg\":\"{}\"}}",
                                         json_escape(fixed.error().message));
                        }
                    }
                }
            }
        }
        return 0;
    }

    // ── --ir: lower to IR and execute ─────────────────────────────
    if (argc > 1 && std::string_view(argv[1]) == "--ir") {
        aura::compiler::CompilerService cs;
        if (argc > 2) {
            auto result = cs.eval_ir(argv[2]);
            if (!result) {
                std::println(std::cerr, "error: {}", result.error().format());
                return 1;
            }
            std::println("{}", fmt_val(*result, cs));
        } else {
            std::ostringstream buf;
            buf << std::cin.rdbuf();
            auto result = cs.eval_ir(buf.str());
            if (!result) {
                std::println(std::cerr, "error: {}", result.error().format());
                return 1;
            }
            std::println("{}", fmt_val(*result, cs));
        }
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
        if (eval) std::println("result: {}", aura::compiler::types::format_value(*eval));
        return 0;
    }

    // ── --fmt / --unparse: parse, format (reindent), output ─────
    // Usage: ./aura --fmt file.aura    → stdout
    //        ./aura --fmt -i file.aura  → in-place rewrite
    //        ./aura --fmt --check file.aura → CI mode (exit 1 if unformatted)
    if (argc > 1 && (std::string_view(argv[1]) == "--fmt" || std::string_view(argv[1]) == "--unparse")) {
        bool in_place = false;
        bool check_only = false;
        std::string in_path, input;

        for (int i = 2; i < argc; ++i) {
            if (std::string_view(argv[i]) == "-i") in_place = true;
            else if (std::string_view(argv[i]) == "--check") check_only = true;
            else if (argv[i][0] != '-') { in_path = argv[i]; }
        }

        if (!in_path.empty()) {
            // Read from file
            std::ifstream f(in_path);
            if (!f) { std::println(std::cerr, "error: cannot read {}", in_path); return 1; }
            input = std::string((std::istreambuf_iterator<char>(f)), {});
        } else {
            // Read from stdin
            std::getline(std::cin, input);
        }

        if (input.empty()) return 1;

        aura::compiler::CompilerService cs;
        auto alloc = cs.arena().allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "parse error");
            return 1;
        }
        flat.root = pr.root;
        auto source = aura::compiler::unparse_node(flat, pool, flat.root);
        source += "\n";

        if (check_only) {
            if (source == input) {
                std::println(std::cerr, "formatted OK");
                return 0;
            }
            std::println(std::cerr, "needs formatting");
            return 1;
        }

        if (in_place && !in_path.empty()) {
            std::ofstream f(in_path);
            if (!f) { std::println(std::cerr, "error: cannot write {}", in_path); return 1; }
            f << source;
            std::println(std::cerr, "formatted {}", in_path);
        } else if (!in_path.empty()) {
            std::ofstream f(in_path + ".fmt");
            if (!f) { std::println(std::cerr, "error: cannot write {}.fmt", in_path); return 1; }
            f << source;
            std::println(std::cerr, "written to {}.fmt", in_path);
        } else {
            std::print("{}", source);
        }
        return 0;
    }

    // ── --write: write stdin content to file ──────────────────────
    // Usage: ./aura --write <file>
    if (argc > 2 && std::string_view(argv[1]) == "--write") {
        std::string input;
        std::getline(std::cin, input);
        std::ofstream f(argv[2]);
        if (!f) {
            std::println(std::cerr, "error: cannot write {}", argv[2]);
            return 1;
        }
        f << input << "\n";
        std::println(std::cerr, "written to {}", argv[2]);
        return 0;
    }

    // ── --cache: parse stdin and write FlatAST+StringPool+IR cache file ─
    // Usage: echo '(+ 1 2)' | ./aura --cache out.abc
    // Parses stdin to FlatAST, lowers to IR, writes cache (with IR), then evaluates.
    if (argc > 2 && std::string_view(argv[1]) == "--cache") {
        std::string input;
        if (argc > 3) { input = argv[3]; }
        else { std::getline(std::cin, input); }

        if (input.empty()) {
            std::println(std::cerr, "error: empty input");
            return 1;
        }

        aura::ast::ASTArena arena;
        aura::ast::StringPool pool(arena.allocator());
        aura::ast::FlatAST flat(arena.allocator());
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (pr.error.size()) {
            std::println(std::cerr, "parse error: {}", pr.error);
            return 1;
        }
        flat.root = pr.root;

        // Lower to IR and run passes
        auto ir_mod = aura::compiler::lower_to_ir(flat, pool, arena);
        aura::compiler::ComputeKindWrap ck;
        aura::compiler::ArityWrap ar;
        aura::compiler::ConstantFoldingWrap cf;
        ck.run(ir_mod);
        ar.run(ir_mod);
        cf.run(ir_mod);

        // Write cache with IR
        if (!aura::compiler::cache::write_cache(argv[2], flat, pool, pr.root, 0, &ir_mod)) {
            std::println(std::cerr, "error: cannot write cache file {}", argv[2]);
            return 1;
        }

        std::println(std::cerr, "cache written to {} ({} ir functions, {} ir strings)",
                     argv[2], ir_mod.functions.size(), ir_mod.string_pool.size());

        // Evaluate normally
        aura::compiler::CompilerService cs;
        auto result = cs.eval_ir(input);
        if (!result) {
            std::println(std::cerr, "eval error: {}", result.error().format());
            return 1;
        }
        std::println("{}", fmt_val(*result, cs));
        return 0;
    }

    // ── --cache-open: load and inspect a cache file ───────────────
    // Usage: ./aura --cache-open out.abc
    // Prints cache stats (AST + IR if available).
    if (argc > 2 && std::string_view(argv[1]) == "--cache-open") {
        auto mc = aura::compiler::cache::open_cache(argv[2]);
        if (!mc.valid()) {
            std::println(std::cerr, "error: cannot open cache file {}", argv[2]);
            return 1;
        }
        std::println("cache: {} nodes, root={}, version=3 (O(1) resolve)",
                     mc.size(), mc.root());
        // Show first few nodes with sym resolution
        auto show_n = std::min<std::size_t>(mc.size(), 6);
        for (std::uint32_t i = 0; i < show_n; ++i) {
            auto nv = mc.get(i);
            auto tag_int = static_cast<int>(nv.tag);
            std::string sym;
            if (nv.sym_id == 0xFFFFFFFFu) {
                sym = "(none)";
            } else {
                auto sv = mc.resolve(nv.sym_id);
                sym = sv.empty() ? "(unresolved)" : std::string(sv);
            }
            std::println("  [{}] tag={} sym_id={} sym='{}' int={}", i, tag_int, nv.sym_id, sym, nv.int_value);
        }
        // Show IR cache info
        if (mc.has_ir()) {
            std::println("IR cache: {} functions, {} strings",
                         mc.ir_functions().size(), mc.ir_strings().size());
            for (auto& fn : mc.ir_functions()) {
                std::println("  func[{}] '{}': {} blocks, {} params, {} locals, {} args",
                             fn.id, fn.name, fn.blocks.size(),
                             fn.params.size(), fn.local_count, fn.arg_count);
            }
            // Build IRModule from cached functions
            aura::ir::IRModule cached_mod;
            for (auto& fn : mc.ir_functions())
                cached_mod.add_function(fn);
            cached_mod.set_entry(mc.ir_entry());
            cached_mod.string_pool.assign(mc.ir_strings().begin(), mc.ir_strings().end());

            // Run passes
            aura::compiler::ComputeKindWrap ck;
            aura::compiler::ArityWrap ar;
            aura::compiler::ConstantFoldingWrap cf;
            std::println(std::cerr, "PM: running {}->{}->{}", ck.name(), ar.name(), cf.name());
            ck.run(cached_mod);
            ar.run(cached_mod);
            cf.run(cached_mod);

            if (ar.has_error()) {
                std::println(std::cerr, "arity check failed from cache");
                return 1;
            }

            if (cf.folded_count() > 0)
                std::println(std::cerr, "PM: folded {} instructions", cf.folded_count());

            // Execute
            aura::compiler::CompilerService cs_tmp;
            aura::compiler::IRInterpreter interp(cached_mod, cs_tmp.evaluator().primitives());
            auto result = interp.execute();
            if (!result) {
                std::println(std::cerr, "error: {}", result.error().format());
                return 1;
            }
            std::println("{}", fmt_val(*result, cs_tmp));
            return 0;
        } else {
            std::println("no IR cache available \u2014 use --cache instead");
            return 1;
        }
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

    // ── --hot-swap: replace function in cached IR module with new code ─
    // First call seeds the cache like --ir; subsequent calls hot-swap.
    if (argc > 1 && std::string_view(argv[1]) == "--hot-swap") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) { input = argv[2]; }
        else { std::getline(std::cin, input); }
        auto result = cs.hot_swap(input);
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().format());
            return 1;
        }
        std::println("{}", fmt_val(*result, cs));
        return 0;
    }

    // ── --jit: compile and execute via LLVM ORC JIT ─────────────
    if (argc > 1 && std::string_view(argv[1]) == "--jit") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) { input = argv[2]; }
        else { std::getline(std::cin, input); }

        // Quick test: compile empty function and call it
        #ifdef AURA_HAVE_LLVM
        auto result = cs.exec_jit(input);
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().format());
            return 1;
        }
        std::println("{}", fmt_val(*result, cs));
        return 0;
        #else
        std::println(std::cerr, "JIT not available — rebuild with LLVM");
        return 1;
        #endif
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
            std::println(std::cerr, "error: {}", result.error().format());
        } else {
            std::println("result: {}", fmt_val(*result, cs));
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
                    std::println("│     {} : {}", c.func_free_vars[i], fmt_val(c.env[i], cs));
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
                    std::println("│     [{}] = {}", i, fmt_val(c.env[i], cs));
                }
            }
        }

        std::println("┌─ cells ({})", cells.size());
        for (auto& c : cells) {
            std::println("├ [{}] = {}", c.id, fmt_val(c.value, cs));
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
            std::println("result: {}", fmt_val(*result, cs));
        } else {
            std::println(std::cerr, "error: {}", result.error().format());
        }

        auto closures = cs.last_closures();
        auto cells = cs.last_cells();

        for (auto& c : closures) {
            std::println("closure [{}] = func[{}] '{}'",
                         c.id, c.func_id, c.func_name);
            for (std::size_t i = 0; i < c.env.size(); ++i) {
                auto label = i < c.func_free_vars.size()
                           ? c.func_free_vars[i] : std::format("[{}]", i);
                std::println("  {} = {}", label, fmt_val(c.env[i], cs));
            }
        }

        for (auto& c : cells) {
            std::println("cell [{}] = {}", c.id, fmt_val(c.value, cs));
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
                     result ? fmt_val(*result, cs) : std::string("null"));

        for (std::size_t i = 0; i < closures.size(); ++i) {
            auto& c = closures[i];
            std::println("  {{\"id\":{},\"func_id\":{},\"func\":\"{}\",",
                         c.id, c.func_id, c.func_name);
            std::println("   \"env\":[");
            for (std::size_t j = 0; j < c.env.size(); ++j) {
                auto label = j < c.func_free_vars.size()
                           ? c.func_free_vars[j] : std::format("[{}]", j);
                std::println("    {{\"name\":\"{}\",\"value\":{}}}{}",
                             label, fmt_val(c.env[j], cs),
                             j + 1 < c.env.size() ? "," : "");
            }
            std::println("   ]}}{}", i + 1 < closures.size() ? "," : "");
        }

        std::println("],\"cells\":[");
        for (std::size_t i = 0; i < cells.size(); ++i) {
            std::println("  {{\"id\":{},\"value\":{}}}{}",
                         cells[i].id, fmt_val(cells[i].value, cs),
                         i + 1 < cells.size() ? "," : "");
        }
        std::println("]}}");
        return result ? 0 : 1;
    }

    // ── Normal REPL / pipe mode ─────────────────────
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
            if (!r) std::println(std::cerr, "error: {}", r.error().format());
            else std::println("{}", fmt_val(*r, cs));
        }
        return 0;
    }

    // ── Pipe mode: read all input, split into complete S-expressions ──
    // Join all lines to support multi-line expressions
    std::string all_input;
    {
        std::ostringstream buf;
        buf << std::cin.rdbuf();
        all_input = buf.str();
    }
    if (all_input.empty()) { std::println(std::cerr, "usage: echo '(+ 1 2)' | ./aura"); return 1; }

    // Split into balanced S-expressions by tracking paren balance
    // Also track string literals to avoid counting parens inside strings
    std::vector<std::string> exprs;
    std::string current;
    int depth = 0;
    bool in_string = false;

    for (std::size_t i = 0; i < all_input.size(); ++i) {
        auto c = all_input[i];

        if (in_string) {
            current += c;
            if (c == '\\' && i + 1 < all_input.size()) {
                // Skip escaped character
                ++i;
                current += all_input[i];
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == ';') {
            // Line comment: skip to end of line
            if (depth > 0) current += ';';
            while (i + 1 < all_input.size() && all_input[i + 1] != '\n') {
                ++i;
            }
            continue;
        }

        if (c == '"') {
            current += c;
            in_string = true;
            continue;
        }

        if (c == '(' || c == '[') {
            if (depth == 0 && !current.empty()) {
                // Prefix characters (' ` ,) before ( form a single expression
                // e.g. '(+ 1 2), `(1 ,x 3), ,(+ 1 2)
                bool is_prefix = false;
                for (auto pc : current) {
                    if (pc == '\'' || pc == '`' || pc == ',') {
                        is_prefix = true;
                        break;
                    }
                    if (!std::isspace(static_cast<unsigned char>(pc))) {
                        is_prefix = false;
                        break;
                    }
                }
                if (!is_prefix) {
                    // Start of a new expression while we had non-whitespace leftover?
                    // Push current as complete
                    auto trimmed = current;
                    auto pos = trimmed.find_first_not_of(" \t\r\n");
                    if (pos != std::string::npos) {
                        trimmed = trimmed.substr(pos);
                        auto end = trimmed.find_last_not_of(" \t\r\n");
                        if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);
                        exprs.push_back(trimmed);
                    }
                    current.clear();
                }
            }
            current += c;
            ++depth;
            continue;
        }

        if ((c == ')' || c == ']') && depth > 0) {
            current += c;
            --depth;
            if (depth == 0) {
                // Complete expression found
                auto trimmed = current;
                auto pos = trimmed.find_first_not_of(" \t\r\n");
                if (pos != std::string::npos) {
                    trimmed = trimmed.substr(pos);
                    auto end = trimmed.find_last_not_of(" \t\r\n");
                    if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);
                    exprs.push_back(trimmed);
                }
                current.clear();
            }
            continue;
        }

        current += c;
    }

    // If there's leftover text, try it as a complete expression
    if (!current.empty()) {
        auto trimmed = current;
        auto pos = trimmed.find_first_not_of(" \t\r\n");
        if (pos != std::string::npos) {
            trimmed = trimmed.substr(pos);
            auto end = trimmed.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);
            if (!trimmed.empty() && depth == 0)
                exprs.push_back(trimmed);
            else if (!trimmed.empty())
                std::println(std::cerr, "warning: unbalanced parentheses in input");
        }
    }

    if (exprs.empty()) { std::println(std::cerr, "usage: echo '(+ 1 2)' | ./aura"); return 1; }

    bool err = false;
    for (auto& e : exprs) {
        auto s = e.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        e = e.substr(s);
        auto r = cs.eval(e);
        if (!r) { std::println(std::cerr, "error: {}", r.error().format()); err = true; }
        else if (&e == &exprs.back() && !is_void(*r)) std::println("{}", fmt_val(*r, cs));
    }
    return err ? 1 : 0;
}
