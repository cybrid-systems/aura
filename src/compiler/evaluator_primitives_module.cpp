// evaluator_primitives_module.cpp — P0 step 24: module? / module-get / module-keys / use /
// load-module / import aura.compiler.evaluator module partition; registered via
// evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

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

void register_module_primitives(PrimRegistrar add, Evaluator& ev) {

    // (module? v) — Check if value is a module object
    add("module?", [](const auto& a) { return make_bool(!a.empty() && is_module(a[0])); });

    // (module-get mod name) — Get a binding from a module by symbol name.
    // Phase 2.5.0 commit 6: route through lookup_by_intern
    // (SymId-first) with canonical_pool() so the intern happens
    // against the long-lived workspace pool. The env's own
    // pool_ (set via set_pool for closures that captured a non-
    // canonical pool) is the fallback — see Env::lookup_by_intern
    // for the full resolution chain. Observable behavior
    // matches Env::lookup(name) when no binding is found.
    //
    // Issue #231 fix: don't pass canonical_pool() here. The
    // module's bindings are interned in the module's own
    // pool (per-module arena), NOT in the workspace pool.
    // Different pools = different SymIds = lookup_by_symid
    // misses the binding. Use the legacy string-based lookup
    // directly, which walks bindings_ (string-keyed) without
    // needing a SymId. The same applies to module-keys.
    add("module-get", [&ev](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_module(a[0]) || !is_string(a[1]))
            return make_void();
        auto mod_idx = as_module_idx(a[0]);
        auto name_idx = as_string_idx(a[1]);
        if (mod_idx >= ev.modules_.size() || name_idx >= ev.string_heap_.size())
            return make_void();
        auto result = ev.modules_[mod_idx]->lookup(std::string(ev.string_heap_[name_idx]));
        if (!result)
            return make_void();
        // Issue #229 fix: deref cell sentinel
        if (is_cell(*result)) {
            auto ci = as_cell_id(*result);
            if (ci < ev.cells_.size())
                return ev.cells_[ci];
        }
        return *result;
    });

    // (module-keys mod) — List all exported binding names from a module
    add("module-keys", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_module(a[0]))
            return make_void();
        auto mod_idx = as_module_idx(a[0]);
        if (mod_idx >= ev.modules_.size())
            return make_void();
        EvalValue result = make_void();
        auto& bindings = ev.modules_[mod_idx]->bindings();
        for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(it->first);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    // (use path) — Load module, return module object (no env injection)
    add("use", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        return ev.load_module_file(ev.string_heap_[idx]);
    });

    add("load-module", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        return ev.load_module_file(ev.string_heap_[idx]);
    });

    add("import", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& path = ev.string_heap_[idx];

        // Optional prefix: (import "path" "prefix:")
        std::string prefix;
        if (a.size() > 1 && is_string(a[1])) {
            auto pidx = as_string_idx(a[1]);
            if (pidx < ev.string_heap_.size())
                prefix = ev.string_heap_[pidx];
        }

        // Load module (cached, isolated env)
        auto mod_val = ev.load_module_file(path);
        if (!is_module(mod_val))
            return make_void();
        auto mod_idx = as_module_idx(mod_val);
        if (mod_idx >= ev.modules_.size())
            return make_void();

        // Inject all bindings into top_ env
        auto* mod_env = ev.modules_[mod_idx];
        if (prefix.empty()) {
            // No prefix: inject as-is (backward compat)
            for (auto& [name, val] : mod_env->bindings()) {
                ev.top_.bind(name, val);
            }
        } else {
            // Prefix injection: bind both prefix:name and bare name for each export
            for (auto& [name, val] : mod_env->bindings()) {
                auto prefixed = prefix + name;
                // Inter the prefixed name into the workspace pool
                auto psid = ev.string_heap_.size();
                ev.string_heap_.push_back(prefixed);
                // Bind in top env with prefix
                ev.top_.bind(prefixed, val);
                // Also bind bare name (no prefix) so tree-walker can find it
                ev.top_.bind(name, val);
            }
        }
        return make_bool(true);
    });
}

} // namespace aura::compiler::primitives_detail