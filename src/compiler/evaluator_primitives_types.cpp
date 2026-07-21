// evaluator_primitives_types.cpp — P0 step 22: declare-type / module-sig / hot-swap primitives
// aura.compiler.evaluator module partition; registered via evaluator_ctor.cpp.

module;

#include <sys/stat.h>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.parser.parser;
import aura.diag;
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

void register_type_primitives(PrimRegistrar add, Evaluator& ev) {

    // 示例: (declare-type add "Int Int" "Int") → add: (Int, Int) -> Int
    // 这些签名在 typecheck-current 时注入到类型环境中。
    add("declare-type", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_string(a[1]) || !is_string(a[2]))
            return make_bool(false);
        auto name_idx = as_string_idx(a[0]);
        auto params_idx = as_string_idx(a[1]);
        auto ret_idx = as_string_idx(a[2]);
        if (name_idx >= ev.string_heap_.size() || params_idx >= ev.string_heap_.size() ||
            ret_idx >= ev.string_heap_.size())
            return make_bool(false);
        ev.declared_type_sigs_[ev.string_heap_[name_idx]] = {
            .type_str = ev.string_heap_[params_idx] + "|" + ev.string_heap_[ret_idx],
            .module_file = "",
            .resolved = false};
        return make_bool(true);
    });

    // (generate-type-sigs "module-path") — 类型推断并生成 .aura-type 文件
    // 解析模块文件，对其中每个 export 的函数进行类型推断，
    // 生成同名的 .aura-type 签名文件。
    // 示例: (generate-type-sigs "helper.aura") → helper.aura-type
    add("generate-type-sigs", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto path = ev.resolve_module_path(ev.string_heap_[idx]);
        if (path.empty()) {
            std::println(std::cerr, "generate-type-sigs: cannot resolve '{}'",
                         ev.string_heap_[idx]);
            return make_bool(false);
        }

        // 读取并解析模块文件
        std::ifstream f(path);
        if (!f) {
            std::println(std::cerr, "generate-type-sigs: cannot open '{}'", path);
            return make_bool(false);
        }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty())
            return make_bool(false);

        aura::ast::ASTArena local_arena;
        auto alloc = local_arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(content, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "generate-type-sigs: parse error");
            return make_bool(false);
        }
        flat.root = pr.root;

        // 类型推断
        aura::core::TypeRegistry treg;
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;
        tc.infer_flat(flat, pool, flat.root, diag);

        // 扫描所有 Define 节点，收集名称
        // 如果模块有 (export ...) 声明，只输出 export 的函数
        std::unordered_set<std::string> export_set;
        for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Export) {
                for (auto cid : nv.children) {
                    auto cv = flat.get(cid);
                    if (cv.tag == aura::ast::NodeTag::Variable)
                        export_set.insert(std::string(pool.resolve(cv.sym_id)));
                }
            }
        }

        std::vector<std::string> fn_names;
        std::unordered_map<std::string, aura::ast::NodeId, aura::core::TransparentStringHash,
                           std::equal_to<>>
            define_map;
        for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Define) {
                auto name = std::string(pool.resolve(nv.sym_id));
                define_map[name] = nid;
                if (export_set.empty() || export_set.count(name))
                    fn_names.push_back(name);
            }
        }

        // 生成类型签名文件
        auto type_sig_path = path;
        {
            auto dot_pos = type_sig_path.rfind('.');
            if (dot_pos != std::string::npos)
                type_sig_path = type_sig_path.substr(0, dot_pos) + ".aura-type";
        }

        std::ofstream of(type_sig_path);
        if (!of) {
            std::println(std::cerr, "generate-type-sigs: cannot write '{}'", type_sig_path);
            return make_bool(false);
        }

        std::function<std::string(std::uint32_t)> type_name_for =
            [&](std::uint32_t tid) -> std::string {
            auto t = aura::core::TypeId{tid, 1};
            auto tag = treg.tag_of(t);
            switch (tag) {
                case aura::core::TypeTag::INT:
                    return "Int";
                case aura::core::TypeTag::BOOL:
                    return "Bool";
                case aura::core::TypeTag::STRING:
                    return "String";
                case aura::core::TypeTag::FLOAT:
                    return "Float";
                case aura::core::TypeTag::VOID:
                    return "Void";
                case aura::core::TypeTag::FUNC: {
                    if (auto* ft = treg.func_of(t)) {
                        std::string s;
                        for (auto& a : ft->args) {
                            if (!s.empty())
                                s += " ";
                            s += type_name_for(a.index);
                        }
                        s += " -> " + type_name_for(ft->ret.index);
                        return s;
                    }
                    return "Any";
                }
                default:
                    return "Any";
            }
        };

        std::size_t written = 0;
        for (auto& name : fn_names) {
            auto it = define_map.find(name);
            if (it == define_map.end())
                continue;
            auto def_v = flat.get(it->second);
            if (!def_v.children.empty()) {
                auto val_id = def_v.child(0);
                // 对 define 的值显式进行类型推断（define 自身不遍历子节点）
                // 使用 tc 在同一个 TypeRegistry 中推断类型
                auto val_type = tc.infer_flat(flat, pool, val_id, diag);
                if (val_type.valid() && val_type.index != 0) {
                    of << name << ": " << type_name_for(val_type.index) << "\n";
                    ++written;
                }
            }
        }

        // 写入成功后，失效模块缓存强制下次 require 重新加载
        // 这样 .aura-type 文件才能在后续的 require 中被读取。
        // pre_exec_requires 可能在 generate-type-sigs 之前加载了模块。
        auto cache_it = ev.module_cache_.find(path);
        if (cache_it != ev.module_cache_.end()) {
            ev.module_cache_.erase(cache_it);
        }

        std::println(std::cerr, "generate-type-sigs: wrote {} types to '{}'", written,
                     type_sig_path);
        return make_bool(written > 0);
    });

    // (check-module-signature "module-path")
    // 加载模块，对其每个 define 的函数进行类型推断，
    // 然后与 .aura-type 中的声明签名进行比对。
    // 输出不一致的诊断结果（不修改文件）。
    add("check-module-signature", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto path = ev.resolve_module_path(ev.string_heap_[idx]);
        if (path.empty()) {
            std::println(std::cerr, "check-module-signature: cannot resolve '{}'",
                         ev.string_heap_[idx]);
            return make_bool(false);
        }

        // 读取并解析模块文件
        std::ifstream f(path);
        if (!f) {
            std::println(std::cerr, "check-module-signature: cannot open '{}'", path);
            return make_bool(false);
        }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty())
            return make_bool(false);

        aura::ast::ASTArena local_arena;
        auto alloc = local_arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(content, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "check-module-signature: parse error");
            return make_bool(false);
        }
        flat.root = pr.root;

        // 类型推断：对每个 define 的值进行类型推断
        struct FnInfo {
            std::string name;
            std::string inferred_type;
        };
        std::vector<FnInfo> fn_infos;

        aura::core::TypeRegistry treg;
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;
        tc.infer_flat(flat, pool, flat.root, diag);

        for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Define) {
                auto name = std::string(pool.resolve(nv.sym_id));
                if (!nv.children.empty()) {
                    auto val_id = nv.child(0);
                    auto val_type = tc.infer_flat(flat, pool, val_id, diag);
                    if (val_type.valid() && val_type.index != 0) {
                        fn_infos.push_back({name, treg.format_type(val_type)});
                    }
                }
            }
        }

        // 读取 .aura-type 文件
        auto sig_path = path;
        {
            auto dot = sig_path.rfind('.');
            if (dot != std::string::npos)
                sig_path = sig_path.substr(0, dot) + ".aura-type";
        }

        struct SigDecl {
            std::string name;
            std::string decl_type;
        };
        std::vector<SigDecl> sig_decls;

        struct stat st;
        if (::stat(sig_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            std::ifstream tf(sig_path);
            if (tf) {
                std::string line;
                while (std::getline(tf, line)) {
                    auto colon = line.find(':');
                    if (colon == std::string::npos)
                        continue;
                    auto arrow = line.find("->", colon);
                    if (arrow == std::string::npos)
                        continue;
                    auto name = line.substr(0, colon);
                    name.erase(name.find_last_not_of(" \t\r") + 1);
                    auto params_str = line.substr(colon + 1, arrow - colon - 1);
                    params_str.erase(0, params_str.find_first_not_of(" \t\r"));
                    params_str.erase(params_str.find_last_not_of(" \t\r") + 1);
                    auto ret_str = line.substr(arrow + 2);
                    ret_str.erase(0, ret_str.find_first_not_of(" \t\r"));
                    ret_str.erase(ret_str.find_last_not_of(" \t\r\n") + 1);
                    sig_decls.push_back({name, params_str + " -> " + ret_str});
                }
            }
        }

        // 比对：每个 decl 必须在 inferred 中找到匹配的
        // 类型字符串精确匹配（eg: "Int Int -> Int"）
        std::size_t matched = 0, mismatched = 0, missing = 0;
        for (auto& sd : sig_decls) {
            bool found = false;
            bool match = false;
            for (auto& fi : fn_infos) {
                if (fi.name == sd.name) {
                    found = true;
                    // treg.format_type 返回 "(Int Int -> Int)"，sd.decl_type 是 "Int Int -> Int"
                    // 标准化比较：去掉 format_type 中的括号
                    std::string fmt = fi.inferred_type;
                    // format_type 像 "(Int Int -> Int)"，去掉首尾括号
                    if (fmt.size() >= 2 && fmt.front() == '(' && fmt.back() == ')')
                        fmt = fmt.substr(1, fmt.size() - 2);
                    // 替换类型变量 __tN 为 Any（未标注类型时推断出 "__t0 -> __t0"）
                    for (auto ci = fmt.find("__t"); ci != std::string::npos;
                         ci = fmt.find("__t", ci)) {
                        auto end = ci + 3;
                        while (end < fmt.size() && (std::isalnum(fmt[end]) || fmt[end] == '_'))
                            ++end;
                        fmt.replace(ci, end - ci, "Any");
                        ci += 3;
                    }
                    if (fmt == sd.decl_type) {
                        match = true;
                    }
                    break;
                }
            }
            if (!found) {
                std::println(std::cerr, "  MISSING '{}' in module (declared but not defined)",
                             sd.name);
                ++missing;
            } else if (!match) {
                // 查找推断的类型字符串
                std::string inferred_fmt;
                for (auto& fi2 : fn_infos) {
                    if (fi2.name == sd.name) {
                        auto f = fi2.inferred_type;
                        if (f.size() >= 2 && f.front() == '(' && f.back() == ')')
                            f = f.substr(1, f.size() - 2);
                        // 替换类型变量 __tN 为 Any
                        std::string clean;
                        for (std::size_t ci = 0; ci < f.size(); ++ci) {
                            if (f[ci] == '_' && ci + 3 < f.size() && f[ci + 1] == '_' &&
                                f[ci + 2] == 't') {
                                clean += "Any";
                                while (ci < f.size() && (std::isalnum(f[ci]) || f[ci] == '_'))
                                    ++ci;
                                --ci;
                            } else {
                                clean += f[ci];
                            }
                        }
                        inferred_fmt = clean;
                        break;
                    }
                }
                std::println(std::cerr, "  MISMATCH '{}': declared '{}', inferred '{}'", sd.name,
                             sd.decl_type, inferred_fmt);
                ++mismatched;
            } else {
                ++matched;
            }
        }

        std::println(std::cerr, "check-module-signature: {}/{}/{} matched/mismatched/missing",
                     matched, mismatched, missing);
        return make_bool(matched > 0 || (mismatched == 0 && missing == 0));
    });
}

void register_hot_swap_primitives(PrimRegistrar add, Evaluator& ev) {

    // ═══════════════════════════════════════════════════════════════
    // (hot-swap:fn "name" "new-source") → #t / #f
    // Replaces the body of an existing function while keeping its id.
    // Closures referencing the function will use the new code on next call.
    // Requires the CompilerService to have set a hot-swap callback.
    add("hot-swap:fn", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() != 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto ni = as_string_idx(a[0]);
        auto si = as_string_idx(a[1]);
        if (ni >= ev.string_heap_.size() || si >= ev.string_heap_.size())
            return make_bool(false);
        if (!ev.hot_swap_fn_) {
            // No callback set — hot-swap not available in this context
            return make_bool(false);
        }
        const std::string& name = ev.string_heap_[ni];
        const std::string& new_source = ev.string_heap_[si];
        bool ok = false;
        try {
            ok = ev.hot_swap_fn_(name, new_source);
        } catch (...) {
            // [SILENCE-PRIM-#615] hot_swap_fn_ is a user-supplied
            // callback; swallowing ensures a misbehaving callback
            // can't crash the host. ok=false is the documented
            // failure signal returned to the caller.
            ok = false;
        }
        // Issue #1637: closed-loop panic checkpoint restore on hot-swap
        // deopt. Even when the hot-swap itself fails (ok=false), a
        // pending panic checkpoint may still be left from a prior
        // mutation, so we unconditionally run the restore — the
        // underlying method early-returns when no checkpoint is pending
        // (no-op + negligible cost on the steady-state hot-swap path).
        // Wired through restore_panic_checkpoint_on_hot_swap_if_needed
        // in evaluator_workspace_tree.cpp (truncate env_frames +
        // env_generation bump + invalidate_post_rollback_env_frames +
        // walk_active_closures bridge refresh + clear_panic_checkpoint).
        ev.restore_panic_checkpoint_on_hot_swap_if_needed();
        return make_bool(ok);
    });
}

} // namespace aura::compiler::primitives_detail
