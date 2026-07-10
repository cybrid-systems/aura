// evaluator_module_loader.cpp — P1-e: module path resolution, load, and GC
// aura.compiler.evaluator module partition.

module;

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;
import aura.parser.parser;

namespace aura::compiler {

using types::EvalValue;
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

// ── Module path resolution ──────────────────────────────────
// Issue #967: heap realpath/getcwd (no 4096 stack cap); skip empty AURA_PATH.
std::string Evaluator::resolve_module_path(const std::string& path) const {
    auto try_load = [](const std::string& full) -> std::optional<std::string> {
        for (auto candidate : {full, full + ".aura"}) {
            struct stat st;
            if (::stat(candidate.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            // realpath(NULL) allocates; free after copy. Long paths no longer
            // silently fall back to non-canonical form due to 4096 stack limit.
            if (char* real = ::realpath(candidate.c_str(), nullptr)) {
                std::string out(real);
                ::free(real);
                return out;
            }
            return candidate;
        }
        return std::nullopt;
    };

    if (!path.empty() && path[0] == '/') {
        auto hit = try_load(path);
        if (hit)
            return *hit;
        return {};
    }

    // Search CWD first
    {
        if (char* cwd = ::getcwd(nullptr, 0)) {
            auto hit = try_load(std::string(cwd) + "/" + path);
            ::free(cwd);
            if (hit)
                return *hit;
        }
    }

    // Search AURA_PATH (skip empty entries from "::" or leading/trailing ':')
    auto* env = ::getenv("AURA_PATH");
    if (env) {
        std::string aura_path(env);
        std::size_t start = 0, end;
        while ((end = aura_path.find(':', start)) != std::string::npos) {
            auto dir = aura_path.substr(start, end - start);
            if (!dir.empty()) {
                auto hit = try_load(dir + "/" + path);
                if (hit)
                    return *hit;
            }
            start = end + 1;
        }
        if (start < aura_path.size()) {
            auto dir = aura_path.substr(start);
            if (!dir.empty()) {
                auto hit = try_load(dir + "/" + path);
                if (hit)
                    return *hit;
            }
        }
    }

    // Auto-discover: try ../lib/ and ./lib/ (relative to executable / CWD)
    {
        // Try ../lib/ (common for build/aura → lib/ layout)
        auto hit = try_load("../lib/" + path);
        if (hit)
            return *hit;
    }
    {
        // Try ./lib/ (cwd-relative)
        auto hit = try_load("./lib/" + path);
        if (hit)
            return *hit;
    }

    return {};
}

// ── Load module file, return module object ────────────────
types::EvalValue Evaluator::load_module_file(const std::string& path) {
    // 1. Resolve path
    auto resolved = resolve_module_path(path);
    if (resolved.empty()) {
        std::println(std::cerr, "load_module_file: cannot resolve '{}'", path);
        return types::make_void();
    }

    // 2. Check cache
    auto cache_it = module_cache_.find(resolved);
    if (cache_it != module_cache_.end()) {
        return types::make_module(cache_it->second);
    }

    // 3. Circular dependency detection
    if (loading_stack_.count(resolved)) {
        auto eidx = string_heap_.size();
        string_heap_.push_back("circular dependency: " + resolved);
        return types::make_void();
    }
    loading_stack_.insert(resolved);

    // 4. Read file
    struct stat st;
    if (::stat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        loading_stack_.erase(resolved);
        return types::make_void();
    }
    std::ifstream f(resolved);
    if (!f) {
        loading_stack_.erase(resolved);
        return types::make_void();
    }
    std::string content((std::istreambuf_iterator<char>(f)), {});
    if (content.empty()) {
        loading_stack_.erase(resolved);
        return types::make_void();
    }

    // 5. Parse
    if (!arena_) {
        loading_stack_.erase(resolved);
        std::println(std::cerr, "load_module_file: no arena");
        return types::make_void();
    }
    // Per-module arena: StringPool / FlatAST / mod_env all live here so the
    // entire module (incl. closures that reference its FlatAST/Pool) can be
    // freed in O(1) by reset_module(resolved). The default 8MB initial size
    // matches ASTArena's main arena budget; can be tuned per module later.
    auto& mod_arena = arena_group_->module_arena(resolved);
    auto alloc = mod_arena.allocator();
    auto* pool_ptr = mod_arena.create<aura::ast::StringPool>(alloc);
    auto* flat_ptr = mod_arena.create<aura::ast::FlatAST>(alloc);
    auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        loading_stack_.erase(resolved);
        std::println(std::cerr, "load_module_file: parse error for {}", resolved);
        if (!pr.error.empty())
            std::println(std::cerr, "  {}", pr.error);
        // Free the partial allocation — module was never inserted into cache.
        arena_group_->reset_module(resolved);
        return types::make_void();
    }
    flat_ptr->root = pr.root;

    // 6. Create isolated module env (child of top_ for primitive access)
    // Arena-allocate in the per-module arena so closures captured during
    // module eval stay valid for the module's lifetime.
    auto* mod_env = mod_arena.create<Env>(&top_);
    mod_env->set_primitives(&primitives_);
    // Issue #232 follow-up: register the module env in env_frames_ so
    // closures captured during module eval (e.g., the lambda in
    // `(define (f x) ...)`) can walk the parent chain via SoA when
    // materialize_call_env creates a fresh env for the call. Without
    // this, the closure body can't see bindings from the surrounding
    // scope (e.g., from a nested `(require "other-module" all:)`).
    //
    // Pass top_.parent_id() (the env_id of top_'s frame, which is
    // the root frame index) as the explicit parent. This makes the
    // new frame's parent_id_ point to top_'s frame so the SoA walk
    // can find top_'s bindings (which is where the require primitive
    // injects the required module's exports).
    EnvId top_id = top_.parent_id();
    if (top_id == NULL_ENV_ID)
        top_id = 0; // top_ is at index 0
    EnvId mod_id = alloc_env_frame_from_env(*mod_env, top_id);
    const_cast<Env*>(mod_env)->set_parent_id(mod_id);


    // 7. Clear any stale export set from previous module loads
    if (current_export_set_)
        current_export_set_->clear();

    // 8. Evaluate module in its own env
    auto expanded = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);
    auto result = eval_flat(*flat_ptr, *pool_ptr, expanded, *mod_env);

    // 9. Apply export filtering: if (export ...) was declared, remove unexported bindings
    if (current_export_set_ && !current_export_set_->empty()) {
        auto& bindings = mod_env->bindings();
        for (auto it = bindings.begin(); it != bindings.end();) {
            if (!current_export_set_->count(it->first)) {
                it = bindings.erase(it);
            } else {
                ++it;
            }
        }
        current_export_set_->clear();
    }

    // 10. Store module (pointer to arena-allocated env — persists for the
    // module's lifetime, freed by gc_module(resolved)).
    auto mod_idx = modules_.size();
    modules_.push_back(mod_env);
    module_cache_[resolved] = mod_idx;
    module_arena_ptrs_[resolved] = &mod_arena;
    string_heap_.push_back(resolved);
    module_names_.push_back(resolved);

    // 10b. IR caching callback (registered by CompilerService)
    if (module_loaded_cb_) {
        module_loaded_cb_(content, resolved);
    }

    // 10c. 自动加载 .aura-type 类型签名文件
    // 检查 {module}.aura 同目录下是否有 {module}.aura-type 文件
    auto type_sig_path = resolved;
    if (type_sig_path.size() > 5) {
        auto dot = type_sig_path.rfind('.');
        if (dot != std::string::npos)
            type_sig_path = type_sig_path.substr(0, dot) + ".aura-type";
    }
    struct stat st2;
    if (::stat(type_sig_path.c_str(), &st2) == 0 && S_ISREG(st2.st_mode)) {
        std::ifstream tf(type_sig_path);
        if (tf) {
            std::string line;
            while (std::getline(tf, line)) {
                // 格式: "name: param1 param2 -> rettype"
                // 例如 "add: Int Int -> Int"
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
                if (!name.empty() && !ret_str.empty()) {
                    declared_type_sigs_[name] = {.type_str = params_str + "|" + ret_str,
                                                 .module_file = resolved,
                                                 .resolved = false};
                }
            }
        }
    }

    // 10d. 为没有 .aura-type 签名的导出函数注册 Any 级签名
    // 这样 typecheck-current 至少知道这些函数存在，不会报 unbound variable。
    // 如果有 .aura-type 签名，优先使用（已在 10c 中注册）。
    for (auto& [fname, fval] : mod_env->bindings()) {
        if (declared_type_sigs_.find(fname) != declared_type_sigs_.end())
            continue; // 已有 .aura-type 签名
        // Module bindings are stored as cells (define creates mutable cells).
        // Unwrap cell to get the actual closure value.
        types::EvalValue actual = fval;
        if (types::is_cell(actual)) {
            auto cid = types::as_cell_id(actual);
            if (cid < cells_.size())
                actual = cells_[cid];
        }
        if (types::is_closure(actual)) {
            auto cid = types::as_closure_id(actual);
            std::string param_str;
            auto cit = closures_.find(cid);
            if (cit != closures_.end() && !cit->second.params.empty()) {
                for (std::size_t pi = 0; pi < cit->second.params.size(); ++pi) {
                    if (pi > 0)
                        param_str += " ";
                    param_str += "Any";
                }
                param_str += " ";
            }
            declared_type_sigs_[fname] = {
                .type_str = param_str + "|Any", .module_file = resolved, .resolved = false};
        }
    }

    loading_stack_.erase(resolved);
    return types::make_module(mod_idx);
}

// Free a module's per-module arena and all closures it owns. Removes the
// module from modules_ / module_cache_ / module_names_. After the call,
// module lookup by name returns void. Caller must ensure no in-flight
// calls into the module (the language runtime is single-threaded per
// Evaluator, so this is the caller's responsibility).
bool Evaluator::gc_module(const std::string& path) {
    auto cache_it = module_cache_.find(path);
    if (cache_it == module_cache_.end())
        return false;
    auto mod_idx = cache_it->second;
    if (mod_idx >= modules_.size())
        return false;

    // Erase closures whose owner_arena matches the module's arena.
    auto arena_it = module_arena_ptrs_.find(path);
    if (arena_it != module_arena_ptrs_.end() && arena_it->second) {
        auto* owner = arena_it->second;
        for (auto it = closures_.begin(); it != closures_.end();) {
            if (it->second.owner_arena == owner)
                it = closures_.erase(it);
            else
                ++it;
        }
    }

    // Reset the module arena. ASTArena v4 (#131) now runs ~Env() on
    // every arena-allocated object as part of reset(), so we no
    // longer need to call env->~Env() manually here — that was the
    // pre-#131 band-aid and would now double-destroy.
    arena_group_->reset_module(path);

    // Clear the module slot. Swap-with-last keeps indices stable for
    // any other live modules; update module_cache_ and
    // functor_instance_cache_ accordingly.
    auto last = modules_.size() - 1;
    if (mod_idx != last) {
        modules_[mod_idx] = modules_[last];
        for (auto& [p, idx] : module_cache_) {
            if (idx == last) {
                module_cache_[p] = mod_idx;
                break;
            }
        }
        for (auto& [key, idx] : functor_instance_cache_) {
            if (idx == last) {
                functor_instance_cache_[key] = mod_idx;
                break;
            }
        }
    }
    modules_.pop_back();
    module_names_.pop_back();
    module_cache_.erase(cache_it);
    module_arena_ptrs_.erase(path);
    // Also remove from functor instance cache if present (the
    // swap-with-last above already fixed indices for survivors; the
    // removed entry's key is the path itself for functor instances).
    auto fic_it = functor_instance_cache_.find(path);
    if (fic_it != functor_instance_cache_.end())
        functor_instance_cache_.erase(fic_it);

    return true;
}

} // namespace aura::compiler
