// evaluator_primitives_module.cpp — P0 step 24: module? / module-get / module-keys / use /
// load-module / import aura.compiler.evaluator module partition; registered via
// evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "core/runtime_config.hh" // Issue #5374: AURA_MOD_TIMING knob

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function_ref<void(std::string, PrimFn)>;

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

std::string import_module_label(std::string_view path) {
    std::string s(path);
    if (s.size() >= 5 && s.ends_with(".aura"))
        s.erase(s.size() - 5);
    const auto slash_std = s.rfind("/std/");
    if (slash_std != std::string::npos)
        return s.substr(slash_std + 1);
    const auto rel = s.rfind("std/");
    if (rel != std::string::npos && (rel == 0 || s[rel - 1] == '/'))
        return s.substr(rel);
    return s;
}

} // namespace aura::compiler::primitives_detail

namespace aura::compiler {

namespace {
    // Issue #5238: these stdlib exports deliberately wrap a core primitive with
    // the same public contract (or restore it per test case). They are not the
    // silent contract change the warning exists to catch, so keep them quiet.
    [[nodiscard]] bool is_known_core_wrapper(std::string_view module, std::string_view name) {
        if (module == "std/string")
            return name == "string-join";
        if (module == "std/math")
            return name == "gcd" || name == "lcm";
        if (module == "std/test")
            return name == "check" || name == "check=";
        // std/evo (#5004) owns the value-based strategy layer: it deliberately
        // re-defines the four names the engine's strategy primitives held, so
        // loading it is not a silent contract swap. Without this entry every
        // module that needs the experience layer printed the #5238 warning.
        if (module == "std/evo")
            return name == "strategy:active" || name == "strategy:set-strategy" ||
                   name == "strategy:report-success" || name == "strategy:escalate";
        return false;
    }
} // namespace

void Evaluator::forget_import_bind_origin(const Env* env) noexcept {
    if (env != nullptr)
        import_bind_origin_.erase(env);
}

void Evaluator::warn_import_overwrite(Env* env, const std::string& name,
                                      const types::EvalValue& val, std::string_view module_path) {
    if (env == nullptr || name.empty() || suppress_import_warnings_ ||
        require_failure_is_diagnostic_)
        return;
    const std::string label = primitives_detail::import_module_label(module_path);
    // Bindings are cells. A re-export may be a fresh cell holding the same
    // closure (module-get) or the original cell (import). Compare the value
    // inside the cell so only a distinct definition warns.
    auto unwrap = [this](types::EvalValue v) {
        if (types::is_cell(v)) {
            const auto ci = types::as_cell_id(v);
            if (ci < cells_.size())
                return cells_[ci];
        }
        return v;
    };
    std::optional<types::EvalValue> local;
    {
        // Issue #5374: O(1) local probe. The old site copied the whole
        // binding vector per injected name and reverse-scanned the copy
        // (O(n²) for a module export set).
        local = env->lookup_local_binding(name);
    }
    auto& by_name = import_bind_origin_[env];
    const auto found = by_name.find(name);
    const bool tracked = found != by_name.end();
    const auto incoming = unwrap(val);
    const bool same_installed = tracked && local && unwrap(*local) == unwrap(found->second.value);
    if (tracked && local && found->second.module != label && same_installed &&
        unwrap(*local) != incoming) {
        std::println(std::cerr,
                     "warning: {} was already defined by {}; {}'s definition will overwrite it.",
                     name, found->second.module, label);
    }
    // Issue #5238: a module export must not silently replace a core primitive
    // with a different contract. Re-exporting the primitive value itself
    // (`(define floor floor)`) is not an override and stays quiet.
    if (env->lookup_primitive(name) && !types::is_primitive(incoming) &&
        !is_known_core_wrapper(label, name) && (!tracked || found->second.module != label) &&
        (!local || unwrap(*local) != incoming)) {
        std::println(std::cerr,
                     "warning: {} is a core primitive; {}'s definition will overwrite it.", name,
                     label);
    }
    // A true re-export is the same object. Keep the module that first
    // installed it so a later distinct definition names that module.
    // Snapshot the inner value. A later define in the importing module
    // updates the cell in place; storing the cell would make that
    // definition look like the original re-export (#5306).
    const bool same_object =
        tracked && local && found->second.module != label && unwrap(*local) == incoming;
    if (!same_object) {
        ImportBindOrigin origin;
        origin.module = label;
        origin.value = incoming;
        if (tracked)
            found->second = std::move(origin);
        else
            by_name.emplace(name, std::move(origin));
    }
}

} // namespace aura::compiler

namespace aura::compiler::primitives_detail {

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
        auto result = ev.modules_[mod_idx]->lookup(std::string(ev.string_at(name_idx)));
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
            auto sidx = ev.string_heap_.mint(it->first);
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
        // Issue #2653: own the path under alloc_storage_lock_ so concurrent
        // string_heap_ reallocate cannot UAF the path ref mid-load
        // (LLM pad / prompt reuse class of H10). Empty paths reach
        // load_module_file for structured refuse (not silent void).
        return ev.load_module_file(ev.copy_string_heap_at(idx));
    });

    add("load-module", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        // Issue #2653: snapshot path (owned) before load_module_file.
        return ev.load_module_file(ev.copy_string_heap_at(idx));
    });

    add("import", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        // Issue #2653: own path + optional prefix (no bare string_heap_ refs).
        auto path = ev.copy_string_heap_at(idx);

        // Optional prefix: (import "path" "prefix:")
        std::string prefix;
        if (a.size() > 1 && is_string(a[1]))
            prefix = ev.copy_string_heap_at(as_string_idx(a[1]));

        // Load module (cached, isolated env).
        // Issue #2570: surface load failure as first-class error (not void)
        // so half-loaded modules are never injected and nested (require)
        // failures are visible to callers / outer module load.
        auto mod_val = ev.load_module_file(path);
        if (is_error(mod_val) && !is_string(mod_val))
            return mod_val;
        if (!is_module(mod_val)) {
            // #5241: a module that did not load is ModuleError at creation.
            return ev.make_typed_error(std::string("module-load-failed: ") + path,
                                       aura::diag::ErrorKind::ModuleError);
        }
        auto mod_idx = as_module_idx(mod_val);
        if (mod_idx >= ev.modules_.size()) {
            return ev.make_typed_error(std::string("module-load-failed: bad index for ") + path,
                                       aura::diag::ErrorKind::ModuleError);
        }

        // Issue #2566: inject into active require target (module env under
        // nested load) so free-vars resolve inside that module's closures.
        // Fall back to top_ for top-level (import)/(require). Always also
        // inject into top_ when the target is a nested module so top-level
        // scripts that later call exported helpers still see the same names
        // (parity with historical top_-only injection for std requires).
        auto* loaded_env = ev.modules_[mod_idx];
        Env* inject = ev.require_inject_env_ ? ev.require_inject_env_ : &ev.top_;
        // Issue #5306: a binding this module only imported is a re-export.
        // Warn and record the module that installed that same value, not
        // the file that required it. A later define in this module stores
        // a different value, so the require path stays the provider.
        auto unwrap_bind = [&ev](EvalValue v) {
            if (is_cell(v)) {
                const auto ci = as_cell_id(v);
                if (ci < ev.cells_.size())
                    return ev.cells_[ci];
            }
            return v;
        };
        auto provider_for = [&](const std::string& name, const EvalValue& val) -> std::string {
            const auto env_it = ev.import_bind_origin_.find(loaded_env);
            if (env_it == ev.import_bind_origin_.end())
                return path;
            const auto name_it = env_it->second.find(name);
            if (name_it == env_it->second.end())
                return path;
            if (unwrap_bind(name_it->second.value) != unwrap_bind(val))
                return path;
            return name_it->second.module;
        };
        // Issue #5374: the injection binds a whole export set at once.
        // Sealing each name separately (Env::bind → note_workspace_binding)
        // walked the same captured env frame once per name — O(n²) for a
        // module export set (a 4 000-name set cost ~1.5 s). The set is
        // bound with the per-bind seal off and the published values are
        // sealed in ONE walk afterwards, which marks the same values.
        std::vector<EvalValue> published;
        published.reserve(loaded_env->bindings().size() * (prefix.empty() ? 1u : 2u));
        auto inject_one = [&](const std::string& name, const EvalValue& val) {
            // Issue #5198: warn when this require overwrites a name a
            // different module already installed here. The bind still
            // happens. The top_ mirror of a nested require is not a second
            // warning (the caller's require is the direct inject below).
            ev.warn_import_overwrite(inject, name, val, provider_for(name, val));
            inject->bind(name, val, /*seal=*/false);
            // Nested module load: also publish on top_ so top-level after
            // (require "m" all:) can still resolve shared std names if the
            // nested require was the first introduction (e.g. only m required
            // std/mutate). Skip double-bind when inject is already top_.
            if (inject != &ev.top_)
                ev.top_.bind(name, val, /*seal=*/false);
            published.push_back(val);
        };
        const bool inj_timing = ::aura::config::flag(::aura::config::Key::AURA_MOD_TIMING);
        const auto inj_t0 = std::chrono::steady_clock::now();
        std::size_t inj_n = 0;
        if (prefix.empty()) {
            // No prefix: inject as-is (backward compat)
            for (auto& [name, val] : loaded_env->bindings()) {
                inject_one(name, val);
                ++inj_n;
            }
        } else {
            // Prefix injection: bind both prefix:name and bare name for each export
            for (auto& [name, val] : loaded_env->bindings()) {
                auto prefixed = prefix + name;
                // Env::bind copies key strings — do not intern prefixed
                // names into the workspace string heap (no psid capture).
                // Issue #1488 / #1693: dead intern leftover polluted the heap;
                // bind uses the stack std::string directly.
                inject_one(prefixed, val);
                // Also bind bare name (no prefix) so tree-walker can find it
                inject_one(name, val);
                ++inj_n;
            }
        }
        // One seal for the whole injected set (Issue #5374).
        ev.seal_workspace_values(published);
        if (inj_timing) {
            // Issue #5374: the injection cost is the second half of a
            // require's cost (the module load itself is the first half).
            const auto inj_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - inj_t0)
                                    .count();
            std::println(std::cerr, "[inj-timing] {} binds={} inject={}us", path, inj_n, inj_us);
        }
        return make_bool(true);
    });
}

} // namespace aura::compiler::primitives_detail