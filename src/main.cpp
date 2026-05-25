#include "compiler/aura_jit.h"
#include "messaging_bridge.h"

import std;
import aura.core;
import aura.compiler.service;
import aura.compiler.query;
import aura.compiler.lowering;
import aura.compiler.pass_manager;
import aura.compiler.ir_executor;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.core.type;
import aura.parser.parser;
import aura.compiler.ir;
import aura.diag;
import aura.compiler.cache;
import aura.compiler.value;

// C-linkage bridge to reflection-based --inspect
// (implemented in ir_reflect_serialize.cpp, compiled with -freflection)
extern "C" char* aura_inspect_ir_json(const void* mod, std::size_t* out_size);
extern "C" bool aura_emit_object_file(const void* mod, const char* path);
extern "C" bool aura_emit_native_file(const char* source, const char* out_path,
                                       const void* functions, unsigned int num_functions);
extern "C" void aura_set_prim_registration(const char* c_code);
extern "C" void aura_set_string_pool(const char** strings, unsigned int count);



// JSON pretty-printer (no external dependencies, safe for module TU)
static std::string prettify_json(const std::string& compact) {
    std::string out;
    out.reserve(compact.size() * 2);
    int indent = 0;
    bool in_string = false;
    bool escaped = false;

    for (std::size_t i = 0; i < compact.size(); ++i) {
        char c = compact[i];
        if (escaped) { escaped = false; out += c; continue; }
        if (c == '\\') { out += c; escaped = true; continue; }
        if (c == '"') { in_string = !in_string; out += c; continue; }
        if (in_string) { out += c; continue; }

        if (c == '{' || c == '[') {
            out += c; out += '\n';
            ++indent;
            out.append(static_cast<std::size_t>(indent * 2), ' ');
        } else if (c == '}' || c == ']') {
            out += '\n';
            --indent; if (indent < 0) indent = 0;
            out.append(static_cast<std::size_t>(indent * 2), ' ');
            out += c;
        } else if (c == ',') {
            out += c; out += '\n';
            out.append(static_cast<std::size_t>(indent * 2), ' ');
        } else if (c == ':') {
            out += ": ";
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            out += c;
        }
    }
    out += '\n';
    return out;
}

// Format helper: format EvalValue with access to string heap
static std::string fmt_val(const aura::compiler::types::EvalValue& v,
                           aura::compiler::CompilerService& cs) {
    return aura::compiler::format_value(v, &cs.evaluator().primitives().string_heap(),
                                        &cs.evaluator().pairs(), 0, &cs.evaluator().primitives());
}

// JSON helper: wrap a string value for JSON (escape quotes and backslashes)
static std::string json_escape(std::string_view s) {
    std::string out;
    for (auto c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

// Minimal JSON parser for --serve protocol messages.
// Only supports flat objects: {"key":"value","key2":"value2"}
// Returns an empty map on parse failure.
static std::unordered_map<std::string, std::string> parse_json_command(std::string_view line) {
    std::unordered_map<std::string, std::string> result;
    if (line.empty())
        return result;

    auto p = line.data();
    auto end = p + line.size();

    // Skip whitespace and opening brace
    auto skip_ws = [&]() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    };

    skip_ws();
    if (p >= end || *p != '{')
        return result;
    ++p;

    while (p < end) {
        skip_ws();
        if (p >= end || *p == '}')
            break;

        // Parse key
        if (*p != '"')
            return result;
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
        if (p >= end)
            return result;
        ++p; // skip closing quote

        skip_ws();
        if (p >= end || *p != ':')
            return result;
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
                    if (*p == 'n')
                        value += '\n';
                    else if (*p == 't')
                        value += '\t';
                    else if (*p == 'r')
                        value += '\r';
                    else if (*p == '"')
                        value += '"';
                    else if (*p == '\\')
                        value += '\\';
                    else
                        value += *p;
                } else {
                    value += *p;
                }
                ++p;
            }
            if (p >= end)
                return result;
            ++p; // skip closing quote
        } else if (*p == 't' || *p == 'f' || *p == 'n') {
            // true / false / null
            while (p < end && (*p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\n' &&
                               *p != '\r')) {
                value += *p++;
            }
        } else {
            // Number (int or float)
            while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\n' &&
                   *p != '\r') {
                value += *p++;
            }
        }

        result[std::move(key)] = std::move(value);

        skip_ws();
        if (p >= end)
            break;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}')
            break;
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
        cs.set_session_id(active_session);
        aura::compiler::CompilerService::register_session(active_session, &cs);

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty())
                continue;

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
                    auto act_it = cmd.find("action");
                    auto action = (act_it != cmd.end()) ? act_it->second : std::string{};

                    if (action == "list") {
                        std::println("{{\"status\":\"ok\",\"sessions\":[");
                        bool first = true;
                        for (auto& [sn, _] : sessions) {
                            if (!first) std::println(",");
                            first = false;
                            std::print("  \"{}\"", json_escape(sn));
                        }
                        std::println("],\"active\":\"{}\"}}", json_escape(active_session));
                        continue;
                    }

                    if (action == "delete") {
                        auto name_it = cmd.find("name");
                        if (name_it == cmd.end() || name_it->second == "default") {
                            std::println("{{\"status\":\"error\",\"msg\":\"cannot delete default session\"}}");
                            continue;
                        }
                        auto& sname = name_it->second;
                        if (sessions.erase(sname)) {
                            if (active_session == sname) {
                                active_session = "default";
                            }
                            std::println("{{\"status\":\"deleted\",\"session\":\"{}\"}}", json_escape(sname));
                        } else {
                            std::println("{{\"status\":\"error\",\"msg\":\"session not found\"}}");
                        }
                        continue;
                    }

                    // Default: activate / create session
                    auto name_it = cmd.find("name");
                    if (name_it == cmd.end()) {
                        std::println("{{\"status\":\"error\",\"msg\":\"missing name field\"}}");
                        continue;
                    }
                    auto& sname = name_it->second;
                    auto real_name = (sname.find("new:") == 0) ? sname.substr(4) : sname;
                    auto [it, created] = sessions.try_emplace(real_name);
                    if (created) {
                        it->second.set_session_id(real_name);
                        aura::compiler::CompilerService::register_session(real_name, &it->second);
                    }
                    if (created)
                        std::println("{{\"status\":\"created\",\"session\":\"{}\"}}", json_escape(real_name));
                    else
                        std::println("{{\"status\":\"ok\",\"session\":\"{}\"}}", json_escape(real_name));
                    active_session = real_name;
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
                            std::println(
                                "{{\"status\":\"error\",\"msg\":\"missing name or code field\"}}");
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
                            if (!first)
                                std::println(",");
                            first = false;
                            std::print("  \"{}\"", json_escape(m));
                        }
                        std::println("]}}");
                    } else if (action == "stats") {
                        auto stats = cs.module_memory_stats();
                        std::println("{{\"status\":\"ok\",\"arenas\":[");
                        bool first = true;
                        for (auto& [name, s] : stats) {
                            if (!first)
                                std::println(",");
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
                aura::messaging::g_current_compiler_service = &cs;

                // Commands that don't need a code field
                if (type == "mutate") {
                    // {"cmd": "mutate", "op": "record-patch", "node": 0, "op-name": "replace-type",
                    // "summary": "change type"}
                    auto op_it = cmd.find("op");      // mutation op name
                    auto node_it = cmd.find("node");  // target node ID
                    auto val_it = cmd.find("value");  // for replace-value
                    auto on_it = cmd.find("op-name"); // for record-patch: recorded op name
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
                            sexpr =
                                std::format("(mutate:record-patch {} \"{}\" \"{}\")", node, on, s);
                        } else if (op_name == "mutate:replace-value") {
                            // (mutate:replace-value node new-value summary)
                            std::string v = val_it != cmd.end() ? val_it->second : "0";
                            std::string s = sum_it != cmd.end() ? sum_it->second : "";
                            sexpr = std::format("(mutate:replace-value {} {} \"{}\")", node, v, s);
                        } else if (op_name == "mutate:replace-type") {
                            // (mutate:replace-type node type-string)
                            std::string t = val_it != cmd.end() ? val_it->second : "Dyn";
                            sexpr = std::format("(mutate:replace-type {} \"{}\")", node, t);
                        } else {
                            // Generic: pass through with all fields
                            std::string v = val_it != cmd.end() ? val_it->second : "0";
                            std::string s = sum_it != cmd.end() ? sum_it->second : "";
                            sexpr = std::format("({} {} {} \"{}\")", op_name, node, v, s);
                        }
                        auto mut_result = cs.typed_mutate(sexpr);
                        if (mut_result.success) {
                            std::println("{{\"status\":\"ok\",\"mutation_id\":{}}}",
                                         mut_result.mutation_id);
                        } else {
                            std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                         json_escape(mut_result.error.empty() ? "mutation failed"
                                                                              : mut_result.error));
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
                                     result
                                         ? (is_bool(*result) ? (as_bool(*result) ? "true" : "false")
                                                             : "false")
                                         : "false");
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
                            if (!first)
                                std::println(",");
                            first = false;
                            std::print("  {{\"id\":{},\"ts\":{},\"node\":{},\"op\":\"{}\","
                                       "\"old_type\":\"{}\",\"new_type\":\"{}\","
                                       "\"summary\":\"{}\",\"status\":\"{}\"}}",
                                       e.mutation_id, e.timestamp_ms, e.target_node,
                                       json_escape(e.operator_name), json_escape(e.old_type),
                                       json_escape(e.new_type), json_escape(e.summary),
                                       json_escape(e.status));
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
                            std::string out = "{\"status\":\"ok\",\"config\":{\"" +
                                              json_escape(key) + "\":" + val_str + "}}";
                            std::println("{}", out);
                            continue;
                        }
                        std::println("{{\"status\":\"error\",\"msg\":\"unknown config key: {}\"}}",
                                     json_escape(key));
                    } else {
                        std::println(
                            "{{\"status\":\"error\",\"msg\":\"missing key or value fields\"}}");
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
                    } else if (type == "define") {
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
                    } else if (type == "exec") {
                        // Timeout-based eval: run in async future, wait up to 30s
                        int timeout_sec = 30;
                        auto timeout_it = cmd.find("timeout");
                        if (timeout_it != cmd.end()) {
                            try { timeout_sec = std::stoi(timeout_it->second); }
                            catch (...) {}
                        }
                        auto future = std::async(std::launch::async, [&]() {
                            return cs.exec_with_cache(code);
                        });
                        auto status = future.wait_for(std::chrono::seconds(timeout_sec));
                        if (status == std::future_status::timeout) {
                            std::println("{{\"status\":\"timeout\",\"msg\":\"exec timed out ({}s)\"}}",
                                         timeout_sec);
                        } else {
                            auto result = future.get();
                            if (result) {
                                try {
                                    auto& v = *result;
                                    if (is_closure(v)) {
                                        std::println("{{\"status\":\"closure\",\"value\":\"#<procedure>\"}}");
                                    } else {
                                        std::println("{{\"status\":\"ok\",\"value\":\"{}\"}}",
                                                     json_escape(fmt_val(v, cs)));
                                    }
                                } catch (const std::bad_alloc&) {
                                    std::println("{{\"status\":\"error\",\"msg\":\"out of memory\"}}");
                                }
                            } else {
                                auto& d = result.error();
                                std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                             json_escape(d.format()));
                            }
                        }
                    } else if (type == "redefine") {
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
                    } else if (type == "unparse") {
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
                    } else if (type == "write") {
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
                                auto w_src =
                                    aura::compiler::unparse_node(w_flat, w_pool, w_flat.root);
                                std::ofstream w_f(write_path);
                                if (w_f) {
                                    w_f << w_src << "\n";
                                    std::println("{{\"status\":\"ok\",\"file\":\"{}\"}}",
                                                 json_escape(write_path));
                                } else {
                                    std::println(
                                        "{{\"status\":\"error\",\"msg\":\"cannot write: {}\"}}",
                                        json_escape(write_path));
                                }
                            }
                        }
                    } else if (type == "query") {
                        // Eval code and return result (like exec, but simpler)
                        auto future = std::async(std::launch::async, [&]() {
                            return cs.exec_with_cache(code);
                        });
                        auto status = future.wait_for(std::chrono::seconds(30));
                        if (status == std::future_status::timeout) {
                            std::println("{{\"status\":\"timeout\",\"msg\":\"query timed out (30s)\"}}");
                        } else {
                            auto result = future.get();
                            if (result) {
                                auto& v = *result;
                                if (is_closure(v)) {
                                    std::println("{{\"status\":\"ok\",\"value\":\"#<procedure>\"}}");
                                } else {
                                    std::println("{{\"status\":\"ok\",\"value\":\"{}\"}}",
                                                 json_escape(fmt_val(v, cs)));
                                }
                            } else {
                                auto& d = result.error();
                                std::println("{{\"status\":\"error\",\"msg\":\"{}\"}}",
                                             json_escape(d.format()));
                            }
                        }
                    } else if (type == "mutate") {
                        // {"cmd": "mutate", "op": "replace-value", "node": 0, "value": 42,
                        // "summary": "change x"}
                        auto op_it = cmd.find("op");
                        auto node_it = cmd.find("node");
                        auto val_it = cmd.find("value");
                        auto sum_it = cmd.find("summary");
                        if (op_it == cmd.end() || node_it == cmd.end()) {
                            std::println("{{\"status\":\"error\",\"msg\":\"missing op or node\"}}");
                        } else {
                            auto node = std::stoll(node_it->second);
                            auto sexpr = std::format(
                                "({} {} {}{}{})", op_it->second, node,
                                val_it != cmd.end() ? val_it->second : "0",
                                sum_it != cmd.end() ? " \"" + sum_it->second + "\"" : " \"\"", "");
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
                    } else if (type == "rollback") {
                        // {"cmd": "rollback", "id": 1}
                        auto id_it = cmd.find("id");
                        if (id_it == cmd.end()) {
                            std::println("{{\"status\":\"error\",\"msg\":\"missing id\"}}");
                        } else {
                            auto sexpr = std::format("(rollback {})", id_it->second);
                            auto result = cs.eval_on_current(sexpr);
                            std::println("{{\"status\":\"ok\",\"rolled_back\":{}}}",
                                         result ? (is_bool(*result)
                                                       ? (as_bool(*result) ? "true" : "false")
                                                       : "false")
                                                : "false");
                        }
                    } else if (type == "mutation-log") {
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
                                if (!first)
                                    std::println(",");
                                first = false;
                                std::print("  {{\"id\":{},\"ts\":{},\"node\":{},\"op\":\"{}\","
                                           "\"old_type\":\"{}\",\"new_type\":\"{}\","
                                           "\"summary\":\"{}\",\"status\":\"{}\"}}",
                                           e.mutation_id, e.timestamp_ms, e.target_node,
                                           json_escape(e.operator_name), json_escape(e.old_type),
                                           json_escape(e.new_type), json_escape(e.summary),
                                           json_escape(e.status));
                            }
                            std::println("]}}");
                        }
                    } else {
                        std::println("{{\"status\":\"error\",\"msg\":\"unknown command: {}\"}}",
                                     json_escape(type));
                    }
                }
            } else {
                // ── Plain S-expression (backward compatible) ────────
                auto& cs = sessions[active_session];
                aura::messaging::g_current_compiler_service = &cs;
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
                    std::println("{{\"status\":\"ok\",\"value\":\"{}\"}}",
                                 json_escape(fmt_val(*r, cs)));
                } else {
                    auto& d = r.error();
                    std::println(
                        "{{\"status\":\"error\",\"kind\":{},\"msg\":\"{}\",\"node_id\":{}}}",
                        static_cast<int>(d.kind), json_escape(d.format()), d.node_id);

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
                            std::println("{{\"status\":\"fixed\",\"value\":\"{}\"}}",
                                         aura::compiler::types::format_value(*fixed));
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
            auto source = argv[2];
            auto result = cs.eval_ir(source);
            if (!result) {
                std::println(std::cerr, "error: {}", result.error().format_with_source(source));
                return 1;
            }
            if (!is_void(*result))
                std::println("{}", fmt_val(*result, cs));
        } else {
            std::ostringstream buf;
            buf << std::cin.rdbuf();
            auto source = buf.str();
            auto result = cs.eval_ir(source);
            if (!result) {
                std::println(std::cerr, "error: {}", result.error().format_with_source(source));
                return 1;
            }
            if (!is_void(*result))
                std::println("{}", fmt_val(*result, cs));
        }
        return 0;
    }

    // ── --inspect: evaluate and dump compiler state as JSON ───────
    // Usage: ./aura --inspect [mode] [cache-file]
    //   echo '(+ 1 2)' | ./aura --inspect
    //   ./aura --inspect cache-open out.abc
    if (argc > 1 && std::string_view(argv[1]) == "--inspect") {
        aura::compiler::CompilerService cs;
        std::string mode = (argc > 2) ? argv[2] : "ir";

        if (mode == "cache-open") {
            // Load from cache file instead of evaluating stdin
            if (argc < 4) {
                std::println(std::cerr, "usage: {} --inspect cache-open <file.abc>", argv[0]);
                return 1;
            }
            auto mc = aura::compiler::cache::open_cache(argv[3]);
            if (!mc.valid()) {
                std::println(std::cerr, "error: cannot open cache file {}", argv[3]);
                return 1;
            }
            std::println("cache: {} nodes, root={}", mc.size(), mc.root());
            if (mc.has_ir()) {
                aura::ir::IRModule cached_mod;
                for (auto& fn : mc.ir_functions())
                    cached_mod.add_function(fn);
                cached_mod.set_entry(mc.ir_entry());
                cached_mod.string_pool.assign(mc.ir_strings().begin(), mc.ir_strings().end());

                aura::compiler::ComputeKindWrap ck;
                aura::compiler::ArityWrap ar;
                aura::compiler::ConstantFoldingWrap cf;
                std::println(std::cerr, "PM: running {}->{}->{}", ck.name(), ar.name(), cf.name());
                ck.run(cached_mod);
                ar.run(cached_mod);
                cf.run(cached_mod);
                if (cf.folded_count() > 0)
                    std::println(std::cerr, "PM: folded {} instructions", cf.folded_count());

                std::size_t json_size = 0;
                char* json_data = aura_inspect_ir_json(&cached_mod, &json_size);
                std::string_view json_str(json_data, json_size);
                std::println("{}", prettify_json(std::string(json_str)));
                delete[] json_data;
            } else {
                std::println("no IR cache in {}", argv[3]);
            }
            return 0;
        }

        // Evaluate the input first (stdin)
        {
            std::ostringstream buf;
            buf << std::cin.rdbuf();
            auto input = buf.str();
            if (!input.empty()) {
                auto result = cs.eval_ir(input);
                if (!result) {
                    std::println(std::cerr, "error: {}", result.error().format());
                    return 1;
                }
            }
        }

        // Dump inspection output
        if (mode == "ir" || mode == "pretty" || mode == "all") {
            auto& last_mod = cs.last_ir_module();
            if (last_mod) {
                std::size_t json_size = 0;
                char* json_data = aura_inspect_ir_json(&*last_mod, &json_size);
                std::string_view json_str(json_data, json_size);
                if (mode == "pretty") {
                    std::println("{}", prettify_json(std::string(json_str)));
                } else {
                    std::println("{}", json_str);
                }
                delete[] json_data;
            } else {
                std::println(std::cerr, "inspect: no IR module available");
            }
        }

        if (mode == "closures" || mode == "all") {
            auto closures = cs.last_closures();
            std::println("closures: {}", closures.size());
            for (auto& c : closures) {
                std::println("  [{}] func[{}] '{}': env[{}]", c.id, c.func_id, c.func_name,
                             c.env.size());
            }
        }

        if (mode == "cache" || mode == "all") {
            std::println("cached functions: {}", cs.cached_function_count());
        }

        if (mode == "evaluator" || mode == "all") {
            std::print("{}", cs.inspect_env());
        }

        if (mode == "typecheck" || mode == "all") {
            aura::core::TypeRegistry treg;
            aura::compiler::TypeChecker tc(treg);
            aura::diag::DiagnosticCollector diag;
            auto& flat = const_cast<aura::ast::FlatAST&>(cs.last_flat());
            auto& pool = const_cast<aura::ast::StringPool&>(cs.last_pool());
            if (flat.root < flat.size()) {
                auto result = tc.infer_flat(flat, pool, flat.root, diag);
                std::println("typecheck result: {}", treg.format_type(result));
                std::println("nodes: {}", flat.size());
                // Dump type for each node that has one
                for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
                    auto tid = flat.type_id(nid);
                    if (tid > 0 && tid < treg.size()) {
                        auto ttype = aura::core::TypeId{tid, 1};
                        std::println("  node[{}]: {} (type_id={})", nid,
                                     treg.format_type(ttype), tid);
                    }
                }
                if (!diag.diagnostics().empty()) {
                    std::println("diagnostics:");
                    for (auto& d : diag.diagnostics())
                        std::println("  {}", d.format());
                }
            } else {
                std::println(std::cerr, "inspect: no AST available");
            }
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
        if (argc > 3) {
            input = argv[3];
        } else {
            std::getline(std::cin, input);
        }
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "parse error{}", pr.error.empty() ? "" : ": " + pr.error);
            return 1;
        }
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
        if (argc > 4) {
            input = argv[4];
        } else {
            std::getline(std::cin, input);
        }
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "parse error{}", pr.error.empty() ? "" : ": " + pr.error);
            return 1;
        }
        flat.root = pr.root;
        aura::core::TypeRegistry tr;
        flat.resolve_type_ids(tr, pool);
        aura::compiler::QueryEngine engine(flat, pool);
        aura::compiler::TransformEngine xform(flat, pool);
        auto result = xform.query_and_fix(engine, argv[2], argv[3]);
        std::println("transform: {} matches, {} patches, applied={}", result.match_count,
                     result.patch_count, result.applied);
        return result.applied ? 0 : 1;
    }

    // ── --typecheck: run compile-time type checking ────────────
    if (argc > 1 && std::string_view(argv[1]) == "--typecheck") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) {
            input = argv[2];
        } else {
            std::getline(std::cin, input);
        }
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
        if (argc > 2)
            input = argv[2];
        else
            std::getline(std::cin, input);
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "parse error{}", pr.error.empty() ? "" : ": " + pr.error);
            return 1;
        }
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
        if (eval)
            std::println("result: {}", aura::compiler::types::format_value(*eval));
        return 0;
    }

    // ── --fmt / --unparse: parse, format (reindent), output ─────
    // Usage: ./aura --fmt file.aura    → stdout
    //        ./aura --fmt -i file.aura  → in-place rewrite
    //        ./aura --fmt --check file.aura → CI mode (exit 1 if unformatted)
    if (argc > 1 &&
        (std::string_view(argv[1]) == "--fmt" || std::string_view(argv[1]) == "--unparse")) {
        bool in_place = false;
        bool check_only = false;
        std::string in_path, input;

        for (int i = 2; i < argc; ++i) {
            if (std::string_view(argv[i]) == "-i")
                in_place = true;
            else if (std::string_view(argv[i]) == "--check")
                check_only = true;
            else if (argv[i][0] != '-') {
                in_path = argv[i];
            }
        }

        if (!in_path.empty()) {
            // Read from file
            std::ifstream f(in_path);
            if (!f) {
                std::println(std::cerr, "error: cannot read {}", in_path);
                return 1;
            }
            input = std::string((std::istreambuf_iterator<char>(f)), {});
        } else {
            // Read from stdin
            std::getline(std::cin, input);
        }

        if (input.empty())
            return 1;

        aura::compiler::CompilerService cs;
        auto alloc = cs.arena().allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "parse error{}", pr.error.empty() ? "" : ": " + pr.error);
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
            if (!f) {
                std::println(std::cerr, "error: cannot write {}", in_path);
                return 1;
            }
            f << source;
            std::println(std::cerr, "formatted {}", in_path);
        } else if (!in_path.empty()) {
            std::ofstream f(in_path + ".fmt");
            if (!f) {
                std::println(std::cerr, "error: cannot write {}.fmt", in_path);
                return 1;
            }
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
        if (argc > 3) {
            input = argv[3];
        } else {
            std::getline(std::cin, input);
        }

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

        std::println(std::cerr, "cache written to {} ({} ir functions, {} ir strings)", argv[2],
                     ir_mod.functions.size(), ir_mod.string_pool.size());

        // Evaluate normally
        aura::compiler::CompilerService cs;
        auto result = cs.eval_ir(input);
        if (!result) {
            std::println(std::cerr, "eval error: {}", result.error().format());
            return 1;
        }
        if (!is_void(*result))
            if (!is_void(*result))
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
        std::println("cache: {} nodes, root={}, version=3 (O(1) resolve)", mc.size(), mc.root());
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
            std::println("  [{}] tag={} sym_id={} sym='{}' int={}", i, tag_int, nv.sym_id, sym,
                         nv.int_value);
        }
        // Show IR cache info
        if (mc.has_ir()) {
            std::println("IR cache: {} functions, {} strings", mc.ir_functions().size(),
                         mc.ir_strings().size());
            for (auto& fn : mc.ir_functions()) {
                std::println("  func[{}] '{}': {} blocks, {} params, {} locals, {} args", fn.id,
                             fn.name, fn.blocks.size(), fn.params.size(), fn.local_count,
                             fn.arg_count);
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
            if (std::string_view(argv[2]) == "no-inline")
                s.enable_inlining = false;
            if (std::string_view(argv[2]) == "specialize")
                s.enable_specialization = true;
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
        if (argc > 2) {
            input = argv[2];
        } else {
            std::getline(std::cin, input);
        }
        auto result = cs.hot_swap(input);
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().format_with_source(input));
            return 1;
        }
        std::println("{}", fmt_val(*result, cs));
        return 0;
    }

    // ── --jit: compile and execute via LLVM ORC JIT ─────────────
    if (argc > 1 && std::string_view(argv[1]) == "--jit") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) {
            input = argv[2];
        } else {
            std::getline(std::cin, input);
        }

// Quick test: compile empty function and call it
#ifdef AURA_HAVE_LLVM
        auto result = cs.exec_jit(input);
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().format());
            return 1;
        }
        if (!is_void(*result))
            std::println("{}", fmt_val(*result, cs));
        return 0;
#else
        std::println(std::cerr, "JIT not available — rebuild with LLVM");
        return 1;
#endif
    }

    // ── --freeze: freeze workspace state to a snapshot file ──────
    // Usage: echo '(define (f x) (+ x 1))(display (f 41))' | ./aura --freeze out.abc
    // Then: echo '(current-source)' | ./aura --cache-open out.abc
    if (argc > 2 && std::string_view(argv[1]) == "--freeze") {
        aura::compiler::CompilerService cs;
        std::ostringstream buf;
        buf << std::cin.rdbuf();
        auto input = buf.str();
        if (input.empty()) {
            std::println(std::cerr, "error: no input");
            return 1;
        }
        // Evaluate first (populates workspace env)
        auto result = cs.eval(input);
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().format());
            return 1;
        }
        // Freeze: write source to snapshot file
        if (auto* sf = std::fopen(argv[2], "w")) {
            std::fprintf(sf, "%s", input.c_str());
            std::fclose(sf);
            std::println("frozen to {}", argv[2]);
        } else {
            std::println(std::cerr, "error: cannot write to {}", argv[2]);
            return 1;
        }
        return 0;
    }

    // ── --load: load and run a frozen snapshot ───────────────────
    // Usage: ./aura --load frozen.aura
    if (argc > 2 && std::string_view(argv[1]) == "--load") {
        std::ifstream f(argv[2]);
        if (!f) {
            std::println(std::cerr, "error: cannot open {}", argv[2]);
            return 1;
        }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        aura::compiler::CompilerService cs;
        auto result = cs.eval(content);
        if (!result) {
            std::println(std::cerr, "error: {}", result.error().format());
            return 1;
        }
        if (!is_void(*result))
            std::println("{}", fmt_val(*result, cs));
        return 0;
    }

    // ── --emit-binary: compile to standalone native binary ───────
    // Usage: echo '(+ 1 2)' | ./aura --emit-binary myapp
    //
    // Pipeline:
    //   1. Parse source → Lower to IR → Run pass manager
    //   2. Convert IRModule functions to FlatFunction array
    //   3. LLVM IR → .ll → llc -filetype=obj → .o for each function
    //   4. Link .o files with runtime.c → standalone binary
    //
    // Falls back to shell wrapper when LLVM is unavailable.
    if (argc > 2 && std::string_view(argv[1]) == "--emit-binary") {
        aura::compiler::CompilerService cs;
        std::string input;
        std::string out_path;

        // ── Collect input: file args or stdin ─────────────────────
        // Usage:
        //   echo '(+ 1 2)' | ./aura --emit-binary myapp         (stdin)
        //   ./aura --emit-binary '(+ 1 2)' myapp    (inline: expr,output)
        //   ./aura --emit-binary file1 file2 out    (file: sources...,output)
        //
        if (argc > 3) {
            // Decide mode: if argv[2] starts with '(', it's inline expression.
            // Otherwise, all args from argv[2..argc-2] are files, argv[argc-1] is output.
            if (argv[2][0] == '(') {
                // Inline mode: ./aura --emit-binary '(+ 1 2)' myapp
                input = argv[2];
                argc -= 1; argv += 1;  // shift so argv[2] = myapp
                out_path = argv[2];
            } else {
                // File mode: ./aura --emit-binary lib.aura main.aura out
                for (int i = 2; i < argc - 1; ++i) {
                    std::ifstream f(argv[i]);
                    if (!f) {
                        std::println(std::cerr, "error: cannot open '{}'", argv[i]);
                        return 1;
                    }
                    input += std::string((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
                    input += '\n';
                }
                out_path = argv[argc - 1];
            }
        } else {
            // Stdin mode: echo '(+ 1 2)' | ./aura --emit-binary myapp
            std::getline(std::cin, input);
            out_path = argv[2];
        }

        if (input.empty()) {
            std::println(std::cerr, "error: no input");
            return 1;
        }

        // ── Resolve import/require: prepend module source ───────────
        // Replace (import "std/name") / (require "std/name") with the
        // module's source code so defines are processed inline by eval_ir.
        // This bypasses cache_module's conservative FnCheck which rejects
        // named-let and local lambda bindings.
        {
            static const std::string std_root = "lib/std/";
            std::string resolved;
            std::size_t pos = 0;
            while (pos < input.size()) {
                // Look for (import "...") or (require "...")
                auto imp = input.find("(import \"", pos);
                auto req = input.find("(require \"", pos);
                auto found = (imp < req) ? imp : req;
                if (found == std::string::npos ||
                    (imp == std::string::npos && req == std::string::npos)) {
                    resolved += input.substr(pos);
                    break;
                }
                resolved += input.substr(pos, found - pos);
                // Find the closing quote and paren
                auto quote_start = input.find('"', found);
                auto quote_end = input.find('"', quote_start + 1);
                if (quote_start == std::string::npos || quote_end == std::string::npos) {
                    resolved += input.substr(found);
                    break;
                }
                auto module_name = input.substr(quote_start + 1, quote_end - quote_start - 1);
                // Map: "std/algorithm" → "lib/std/algorithm.aura"
                std::string module_path;
                if (module_name.starts_with("std/"))
                    module_path = std_root + module_name.substr(4) + ".aura";
                else
                    module_path = module_name + ".aura";
                // Read and prepend module source
                std::ifstream mf(module_path);
                if (mf) {
                    std::string mod_src((std::istreambuf_iterator<char>(mf)),
                                         std::istreambuf_iterator<char>());
                    // Remove (export ...) line — not needed for inline compilation
                    auto exp = mod_src.find("(export");
                    if (exp != std::string::npos) {
                        auto exp_end = mod_src.find(')', exp);
                        if (exp_end != std::string::npos)
                            mod_src = mod_src.substr(0, exp) + mod_src.substr(exp_end + 1);
                    }
                    resolved += mod_src;
                    resolved += '\n';
                } else {
                    std::println(std::cerr, "warning: cannot open module '{}'", module_path);
                }
                pos = quote_end + 1;
                auto close_paren = input.find(')', pos);
                if (close_paren != std::string::npos)
                    pos = close_paren + 1;
            }
            input = resolved;
        }

        // ── Step 1: Parse → Lower → Passes ──
        // eval_ir now includes pre_exec_requires for module support.
        auto eval_result = cs.eval_ir(input);
        if (!eval_result) {
            std::println(std::cerr, "error: {}", eval_result.error().format());
            return 1;
        }
        auto& mod = cs.last_ir_module();
        if (!mod || mod->functions.empty()) {
            std::println(std::cerr, "no IR module generated");
            return 1;
        }

        // ── Step 2: Convert IRModule functions to FlatFunction ────
        // We need to keep all backing storage alive for the FlatFunction array.
        // Each FlatFunction has pointers into our owned vectors.
        //
        // Storage layout:
        //   instr_pool[batch_i][fn_i][block_i]  = vector of FlatInstruction
        //   block_pool[batch_i][fn_i]           = vector of FlatBlock (points into instr_pool)
        //   name_pool[batch_i]                  = string (flat_fn.name points here)
        //   flat_fn_array                       = vector of FlatFunction (final array)

        std::size_t num_fns = mod->functions.size();
        std::vector<std::vector<std::vector<aura::jit::FlatInstruction>>> instr_pool;
        std::vector<std::vector<aura::jit::FlatBlock>> block_pool;
        std::deque<std::string> name_pool;  // deque: stable pointers on push_back
        std::vector<aura::jit::FlatFunction> flat_fn_array;
        flat_fn_array.reserve(num_fns);
        instr_pool.reserve(num_fns);
        block_pool.reserve(num_fns);

        for (auto& ir_fn : mod->functions) {
            instr_pool.emplace_back();
            block_pool.emplace_back();
            name_pool.push_back(ir_fn.name);

            auto& instrs = instr_pool.back();
            auto& blocks = block_pool.back();

            instrs.resize(ir_fn.blocks.size());
            blocks.resize(ir_fn.blocks.size());

            for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
                auto& block = ir_fn.blocks[bi];
                for (auto& inst : block.instructions) {
                    instrs[bi].push_back({static_cast<std::uint32_t>(inst.opcode),
                                          {inst.operands[0], inst.operands[1],
                                           inst.operands[2], inst.operands[3]}});
                }
                blocks[bi] = {block.id, instrs[bi].data(),
                              static_cast<std::uint32_t>(instrs[bi].size())};
            }

            flat_fn_array.push_back({
                name_pool.back().c_str(),
                ir_fn.entry_block,
                ir_fn.local_count,
                ir_fn.arg_count,
                blocks.data(),
                static_cast<std::uint32_t>(blocks.size()),
                nullptr,  // func_id_map not used for AOT
                0
            });
        }

        // ── Step 3: Generate primitive dispatch C code ───────────
        // Build per-slot dispatch functions with the correct ABI:
        // each function has signature (int64_t* args, int32_t argc)
        // matching the PrimFn type used in aura_closure_call.
        //
        // The generated code creates one static function per primitive
        // slot and registers each at its correct evaluator slot number.
        // The slot numbers are matched at compile time.
        {
            auto& prims = cs.evaluator().primitives();
            std::string code = "// Auto-generated by aura --emit-binary\n";
            code += "#include <stdint.h>\n#include <stddef.h>\n#include <stdio.h>\n\n";
            code += "typedef int64_t (*PF)(int64_t*,int32_t);\n";
            code += "void aura_register_primitive_fn(int64_t, int64_t);\n";
            code += "int64_t aura_alloc_pair(int64_t, int64_t);\n";
            code += "int64_t aura_pair_car(int64_t);\n";
            code += "int64_t aura_pair_cdr(int64_t);\n";
            code += "int64_t aura_closure_call(int64_t, int64_t*, int64_t);\n";
            code += "\n";

            // Generate one wrapper function for each known primitive
            for (std::size_t s = 0; s < prims.slot_count(); ++s) {
                auto n = prims.name_for_slot(s);
                if (n.empty()) continue;
                code += "static int64_t prim_" + std::to_string(s) + "(int64_t* a, int32_t c) {\n";
                if (n == "+")
                    code += "    int64_t r=0; for(int i=0;i<c;i++) r+=a[i]; return r;\n";
                else if (n == "-")
                    code += "    if(c<1)return 0; if(c==1)return -a[0]; int64_t r=a[0]; for(int i=1;i<c;i++) r-=a[i]; return r;\n";
                else if (n == "*")
                    code += "    int64_t r=1; for(int i=0;i<c;i++) r*=a[i]; return r;\n";
                else if (n == "/")
                    code += "    if(c<1)return 1; int64_t r=a[0]; for(int i=1;i<c;i++){if(a[i]==0){r=0;break;}r/=a[i];} return r;\n";
                else if (n == "=" || n == "eq?" || n == "eqv?" || n == "equal?")
                    code += "    for(int i=0;i<c-1;i++) if(a[i]!=a[i+1]) return 0; return 1;\n";
                else if (n == "<")
                    code += "    for(int i=0;i<c-1;i++) if(!(a[i]<a[i+1])) return 0; return 1;\n";
                else if (n == ">")
                    code += "    for(int i=0;i<c-1;i++) if(!(a[i]>a[i+1])) return 0; return 1;\n";
                else if (n == "<=")
                    code += "    for(int i=0;i<c-1;i++) if(!(a[i]<=a[i+1])) return 0; return 1;\n";
                else if (n == ">=")
                    code += "    for(int i=0;i<c-1;i++) if(!(a[i]>=a[i+1])) return 0; return 1;\n";
                else if (n == "not")
                    code += "    return (c<1||a[0]==0)?1:0;\n";
                else if (n == "append") {
                    // (append lst1 lst2) — build reversed copy of lst1, then
                    // attach lst2: result = lst1's elements followed by lst2
                    // Strategy: collect car values from lst1 in reverse,
                    // then cons them all with lst2 as tail.
                    code += "    if(c<1)return 0; int64_t lst1=c>0?a[0]:0; int64_t lst2=c>1?a[1]:0;\n";
                    code += "    // Collect car values in reverse order\n";
                    code += "    int64_t rev[1024]; int revc=0;\n";
                    code += "    while(lst1!=0&&lst1<0&&revc<1024){rev[revc++]=aura_pair_car(lst1); lst1=aura_pair_cdr(lst1);}\n";
                    code += "    // Build result: cons each reversed car onto lst2\n";
                    code += "    int64_t r=lst2;\n";
                    code += "    for(int i=revc-1;i>=0;i--) r=aura_alloc_pair(rev[i],r);\n";
                    code += "    return r;\n";
                }
                else if (n == "member") {
                    // (member key list) — find key in list, return tail
                    code += "    int64_t key=c>0?a[0]:0; int64_t lst=c>1?a[1]:0;\n";
                    code += "    while(lst!=0&&lst<0){if(aura_pair_car(lst)==key)return lst; lst=aura_pair_cdr(lst);}\n";
                    code += "    return 0;\n";
                }
                else if (n == "map") {
                    // (map fn list) — apply fn to each element, collect results
                    code += "    int64_t fn=c>0?a[0]:0; int64_t lst=c>1?a[1]:0;\n";
                    code += "    int64_t rev[1024]; int revc=0;\n";
                    code += "    int64_t args[8];\n";
                    code += "    while(lst!=0&&lst<0&&revc<1024){args[0]=aura_pair_car(lst); rev[revc++]=aura_closure_call(fn,args,1); lst=aura_pair_cdr(lst);}\n";
                    code += "    int64_t r=0; for(int i=revc-1;i>=0;i--) r=aura_alloc_pair(rev[i],r);\n";
                    code += "    return r;\n";
                }
                else if (n == "null?")
                    code += "    return (c<1||a[0]==0)?1:0;\n";
                else if (n == "pair?")
                    code += "    return (c>0&&a[0]<0)?1:0;\n";
                else if (n == "cons")
                    code += "    return aura_alloc_pair(c>0?a[0]:0,c>1?a[1]:0);\n";
                else if (n == "car")
                    code += "    return (c>0)?aura_pair_car(a[0]):0;\n";
                else if (n == "cdr")
                    code += "    return (c>0)?aura_pair_cdr(a[0]):0;\n";
                else if (n == "length") {
                    code += "    int64_t v=c>0?a[0]:0; int64_t n=0;\n";
                    code += "    while(v!=0&&v<0&&n<9999){n++;v=aura_pair_cdr(v);}\n";
                    code += "    return n;\n";
                }
                else if (n == "list-ref") {
                    code += "    int64_t v=c>0?a[0]:0; int64_t i=c>1?a[1]:0;\n";
                    code += "    while(i>0&&v!=0&&v<0){v=aura_pair_cdr(v);i--;}\n";
                    code += "    if(i==0&&v!=0&&v<0)return aura_pair_car(v); return 0;\n";
                }
                else if (n == "reverse") {
                    code += "    int64_t v=c>0?a[0]:0; int64_t r=0;\n";
                    code += "    while(v!=0&&v<0){r=aura_alloc_pair(aura_pair_car(v),r);v=aura_pair_cdr(v);}\n";
                    code += "    return r;\n";
                }
                else if (n == "list") {
                    // Build list from multiple args using aura_alloc_pair
                    code += "    int64_t r=0;\n";
                    code += "    for(int i=c-1;i>=0;i--) r=aura_alloc_pair(a[i],r);\n";
                    code += "    return r;\n";
                }
                else if (n == "display") {
                    code += "    if(c>0)printf(\"%ld\",(long)a[0]); fflush(stdout);\n";
                    code += "    return (c>0)?a[0]:0;\n";
                }
                else if (n == "filter") {
                    // (filter pred list) — keep elements where pred returns truthy
                    code += "    int64_t pred=c>0?a[0]:0; int64_t lst=c>1?a[1]:0;\n";
                    code += "    int64_t rev[1024]; int revc=0;\n";
                    code += "    int64_t args[8];\n";
                    code += "    while(lst!=0&&lst<0&&revc<1024){int64_t v=aura_pair_car(lst); args[0]=v; if(aura_closure_call(pred,args,1)!=0)rev[revc++]=v; lst=aura_pair_cdr(lst);}\n";
                    code += "    int64_t r=0; for(int i=revc-1;i>=0;i--) r=aura_alloc_pair(rev[i],r);\n";
                    code += "    return r;\n";
                }
                else if (n == "foldl") {
                    // (foldl fn init list) — left fold
                    code += "    int64_t fn=c>0?a[0]:0; int64_t acc=c>1?a[1]:0; int64_t lst=c>2?a[2]:0;\n";
                    code += "    int64_t args[8];\n";
                    code += "    while(lst!=0&&lst<0){args[0]=acc; args[1]=aura_pair_car(lst); acc=aura_closure_call(fn,args,2); lst=aura_pair_cdr(lst);}\n";
                    code += "    return acc;\n";
                }
                else
                    code += "    return (c>0)?a[0]:0;\n";
                code += "}\n\n";
            }

            // Constructor: register each primitive function at its slot
            code += "__attribute__((constructor)) void reg(void) {\n";
            for (std::size_t s = 0; s < prims.slot_count(); ++s) {
                auto n = prims.name_for_slot(s);
                if (!n.empty()) {
                    code += "  aura_register_primitive_fn(" + std::to_string(s) + ",(int64_t)prim_" + std::to_string(s) + ");\n";
                }
            }
            code += "}\n";
            aura_set_prim_registration(code.c_str());
        }

        // Set string pool for OpConstString
        {
            auto& pool = mod->string_pool;
            std::vector<const char*> raw_pool(pool.size());
            for (std::size_t i = 0; i < pool.size(); ++i)
                raw_pool[i] = pool[i].c_str();
            aura_set_string_pool(raw_pool.data(), static_cast<unsigned>(raw_pool.size()));
        }

        // ── Step 4: Compile via LLVM AOT pipeline ────────────────
        bool compiled = aura_emit_native_file(
            input.c_str(), out_path.c_str(),
            (const void*)flat_fn_array.data(),
            static_cast<unsigned int>(flat_fn_array.size()));

        // ── Step 4: Fallback → shell wrapper ────────────────────
        if (!compiled) {
            std::string self_path = std::string(argv[0]);
            {
                std::ofstream sf(out_path);
                sf << "#!/bin/bash\n";
                sf << "# Aura compiled binary (shell wrapper fallback)\n";
                sf << "exec " << self_path << " --load " << out_path << ".tmp.aura \"$@\"\n";
            }
            std::filesystem::permissions(out_path,
                std::filesystem::perms::owner_all |
                std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                std::filesystem::perms::others_read | std::filesystem::perms::others_exec);
            // Save source for --load
            {
                std::ofstream sf(out_path + ".tmp.aura");
                sf << input;
            }
            if (std::filesystem::exists(out_path)) {
                std::println("emitted binary: {} (shell wrapper)", out_path);
                return 0;
            }
            std::println(std::cerr, "error: cannot create binary");
            return 1;
        }

        std::println("emitted binary: {}", out_path);
        return 0;
    }

    // ── --inspect: eval with full runtime reflection dump ────────
    if (argc > 1 && std::string_view(argv[1]) == "--inspect") {
        aura::compiler::CompilerService cs;
        std::string input;
        if (argc > 2) {
            input = argv[2];
        } else {
            std::getline(std::cin, input);
        }

        cs.set_strategy({.enable_inlining = true,
                         .enable_specialization = false,
                         .max_unroll = 3,
                         .verbose_inspect = true});

        auto result = cs.eval_ir(input);

        if (!result) {
            std::println(std::cerr, "error: {}", result.error().format_with_source(input));
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
        if (argc > 2) {
            input = argv[2];
        } else {
            std::getline(std::cin, input);
        }

        auto result = cs.eval_ir(input);
        if (result) {
            std::println("result: {}", fmt_val(*result, cs));
        } else {
            std::println(std::cerr, "error: {}", result.error().format_with_source(input));
        }

        auto closures = cs.last_closures();
        auto cells = cs.last_cells();

        for (auto& c : closures) {
            std::println("closure [{}] = func[{}] '{}'", c.id, c.func_id, c.func_name);
            for (std::size_t i = 0; i < c.env.size(); ++i) {
                auto label =
                    i < c.func_free_vars.size() ? c.func_free_vars[i] : std::format("[{}]", i);
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
        if (argc > 2) {
            input = argv[2];
        } else {
            std::getline(std::cin, input);
        }

        auto result = cs.eval_ir(input);
        auto closures = cs.last_closures();
        auto cells = cs.last_cells();

        std::println("{{\"status\":\"{}\",\"result\":{},\"closures\":[", result ? "ok" : "error",
                     result ? fmt_val(*result, cs) : std::string("null"));

        for (std::size_t i = 0; i < closures.size(); ++i) {
            auto& c = closures[i];
            std::println("  {{\"id\":{},\"func_id\":{},\"func\":\"{}\",", c.id, c.func_id,
                         c.func_name);
            std::println("   \"env\":[");
            for (std::size_t j = 0; j < c.env.size(); ++j) {
                auto label =
                    j < c.func_free_vars.size() ? c.func_free_vars[j] : std::format("[{}]", j);
                std::println("    {{\"name\":\"{}\",\"value\":{}}}{}", label, fmt_val(c.env[j], cs),
                             j + 1 < c.env.size() ? "," : "");
            }
            std::println("   ]}}{}", i + 1 < closures.size() ? "," : "");
        }

        std::println("],\"cells\":[");
        for (std::size_t i = 0; i < cells.size(); ++i) {
            std::println("  {{\"id\":{},\"value\":{}}}{}", cells[i].id, fmt_val(cells[i].value, cs),
                         i + 1 < cells.size() ? "," : "");
        }
        std::println("]}}");
        return result ? 0 : 1;
    }

    // ── Normal REPL / pipe mode ─────────────────────
    aura::compiler::CompilerService cs;
    bool interactive = false;
    try {
        std::cin.sync();
        interactive = (argc == 1 && std::cin.peek() == std::char_traits<char>::eof());
    } catch (...) {
    }
    if (interactive) {
        std::println("Aura v0.2 — LLVM JIT / Sound Gradual Typing / C FFI");
        std::println("  (quit) to exit");

        // Multi-line REPL with paren balance tracking
        std::string input;
        int depth = 0;
        int history_index = 0;
        std::vector<std::string> history;
        constexpr int MAX_HISTORY = 50;

        while (true) {
            // Prompt
            if (depth == 0)
                std::print("> ");
            else
                for (int i = 0; i < depth && i < 8; ++i)
                    std::print(". ");
            std::cout.flush();

            std::string line;
            if (!std::getline(std::cin, line))
                break;

            if (line == "(quit)" || line == "(exit)")
                break;
            if (line.empty() && depth == 0)
                continue;
            if (line.empty()) {
                input += "\n";
                continue;
            }

            input += line;
            input += "\n";

            // Track paren balance
            bool in_str = false;
            for (auto c : line) {
                if (c == '"')
                    in_str = !in_str;
                if (!in_str) {
                    if (c == '(')
                        ++depth;
                    if (c == ')')
                        --depth;
                }
            }

            if (depth > 0)
                continue; // Keep reading

            // Complete expression — evaluate
            // Trim whitespace
            auto start = input.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) {
                input.clear();
                continue;
            }
            auto end = input.find_last_not_of(" \t\n\r");
            auto trimmed = input.substr(start, end - start + 1);

            // Add to history
            if (!trimmed.empty()) {
                history.push_back(trimmed);
                if (history.size() > MAX_HISTORY)
                    history.erase(history.begin());
            }

            auto r = cs.eval(trimmed);
            if (!r)
                std::println(std::cerr, "{}: error: {}", trimmed, r.error().format_with_source(trimmed));
            else if (!aura::compiler::types::is_void(*r))
                std::println("{}", fmt_val(*r, cs));

            input.clear();
            depth = 0;
        }
        std::println();
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
    if (all_input.empty()) {
        std::println(std::cerr, "usage: echo '(+ 1 2)' | ./aura");
        return 1;
    }

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
            if (depth > 0)
                current += ';';
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
                        if (end != std::string::npos)
                            trimmed = trimmed.substr(0, end + 1);
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
                    if (end != std::string::npos)
                        trimmed = trimmed.substr(0, end + 1);
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
            if (end != std::string::npos)
                trimmed = trimmed.substr(0, end + 1);
            if (!trimmed.empty() && depth == 0)
                exprs.push_back(trimmed);
            else if (!trimmed.empty())
                std::println(std::cerr, "warning: unbalanced parentheses in input");
        }
    }

    if (exprs.empty()) {
        std::println(std::cerr, "usage: echo '(+ 1 2)' | ./aura");
        return 1;
    }

    bool err = false;
    for (auto& e : exprs) {
        auto s = e.find_first_not_of(" \t\r\n");
        if (s == std::string::npos)
            continue;
        e = e.substr(s);
        auto r = cs.eval(e);
        if (!r) {
            std::println(std::cerr, "error: {}", r.error().format_with_source(e));
            err = true;
        } else if (&e == &exprs.back() && !is_void(*r))
            std::println("{}", fmt_val(*r, cs));
    }
    return err ? 1 : 0;
}
