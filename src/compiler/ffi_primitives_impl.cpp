// ffi_primitives_impl.cpp — Implementation of the FFI
// primitives extracted from evaluator partition TUs
// (Issue #131).
//
// The C FFI state (loaded libraries, registered C
// functions) was previously file-scope statics in
// the monolithic evaluator TU. Now it's per-FFIRuntime, allowing
// multiple evaluators with independent FFI state.

module;

#include <dlfcn.h>
#include "compiler/ffi_hot_path.hh"
#include "core/gc_hooks.h" // Issue #2005: ffi_pin_defer_active + arm/release
#include "renderer/render_ffi.hh"
#include "stdlib/render_ffi.hh"

module aura.compiler.ffi_primitives;

import std;
import aura.compiler.value;
import aura.core.lifetime_pin; // Issue #2005: (ffi:pin-buffer)

// (Issue #131 / #1354 / #1560).
//
// The C FFI state is per-FFIRuntime. Issue #1354 wraps c-* with
// FfiRenderHotpathGuard so render-loop FFI participates in deopt / arena soft-gate.
// Issue #1560: real batch dispatch via ffi_hot_path + stdlib/render_ffi.

namespace aura::compiler {
using EvalValue = types::EvalValue;
using namespace aura::compiler::types;
using aura::renderer::ffi::FfiRenderHotpathGuard;
using aura::renderer::ffi::render_ffi_registry;

void FFIRuntime::register_primitives(RegisterFn add, std::pmr::vector<std::string>* string_heap,
                                     std::vector<void*>* opaque_heap,
                                     std::array<std::uint64_t, 16>* coverage_counters) {
    auto* sh = string_heap;
    auto* oh = opaque_heap;

    add("c-load", [this, sh, coverage_counters](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (coverage_counters)
            (*coverage_counters)[8]++;
        if (a.empty() || !types::is_string(a[0]))
            return make_int(0);
        auto path = (*sh)[types::as_string_idx(a[0])];
        void* lib = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            auto err = ::dlerror();
            auto msg = err ? std::string(err) : "dlopen failed";
            std::println(std::cerr, "c-load: {}", msg);
            return make_int(0);
        }
        auto idx = libs_.size();
        libs_.push_back(lib);
        return make_int(static_cast<std::int64_t>(idx));
    });

    add("c-func", [this, sh, coverage_counters](const auto& a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (coverage_counters)
            (*coverage_counters)[8]++;
        // (c-func lib-id "name" sig-string)
        // lib-id -1 uses RTLD_DEFAULT
        // Issue #979: diagnostics to stderr — bare println defaults to
        // stdout and pollutes --serve JSON protocol stream.
        if (a.size() < 3 || !types::is_int(a[0]) || !types::is_string(a[1])) {
            std::println(std::cerr, "c-func: expected (c-func lib-id \"name\" signature");
            std::println(std::cerr,
                         "  signature format: \"(ArgType) -> RetType\"  e.g. \"(String) -> Int\"");
            return make_int(0);
        }
        auto raw_lib_id = types::as_int(a[0]);
        void* lib = RTLD_DEFAULT;
        if (raw_lib_id >= 0) {
            auto lib_idx = static_cast<std::size_t>(raw_lib_id);
            if (lib_idx >= libs_.size()) {
                std::println(std::cerr,
                             "c-func: invalid library handle {} (use -1 for RTLD_DEFAULT)",
                             lib_idx);
                return make_int(0);
            }
            lib = libs_[lib_idx];
        }
        auto name = (*sh)[types::as_string_idx(a[1])];
        int ret_type = 1;
        std::vector<int> arg_types;
        if (types::is_string(a[2])) {
            auto sig = (*sh)[types::as_string_idx(a[2])];
            std::string sig_err;
            if (!parse_ffi_sig(sig, ret_type, arg_types, &sig_err)) {
                std::println(std::cerr, "c-func: invalid signature '{}'", sig);
                std::println(std::cerr, "  reason: {}", sig_err);
                std::println(std::cerr, "  expected: \"(ArgType) -> RetType\"");
                std::println(std::cerr, "  valid types: Int, Float, String, Opaque, Void");
                return make_int(0);
            }
        } else if (types::is_int(a[2])) {
            ret_type = static_cast<int>(types::as_int(a[2]));
            for (std::size_t i = 3; i < a.size(); ++i)
                if (types::is_int(a[i]))
                    arg_types.push_back(static_cast<int>(types::as_int(a[i])));
        } else {
            std::println(std::cerr,
                         "c-func: third arg must be signature string like \"(String) -> Int\"");
            return make_int(0);
        }
        auto* fn_ptr = ::dlsym(lib, name.c_str());
        if (!fn_ptr) {
            auto* err = ::dlerror();
            std::println(std::cerr, "c-func: symbol '{}' not found in library", name);
            if (err)
                std::println(std::cerr, "  dlerror: {}", err);
            std::println(std::cerr,
                         "  tip: use (c-func -1 \"{}\" \"(String) -> Int\") with RTLD_DEFAULT",
                         name);
            return make_int(0);
        }
        auto fidx = funcs_.size();
        funcs_.push_back({fn_ptr, name, ret_type, std::move(arg_types)});
        auto closure_id = static_cast<std::uint64_t>(fidx) | (1ULL << 63);
        return types::make_closure(closure_id);
    });

    add("c-opaque", [this, oh, coverage_counters](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (coverage_counters)
            (*coverage_counters)[8]++;
        if (a.empty() || !types::is_int(a[0]))
            return make_int(0);
        auto addr = types::as_int(a[0]);
        auto idx = oh->size();
        oh->push_back(reinterpret_cast<void*>(static_cast<std::uintptr_t>(addr)));
        return types::make_opaque(idx);
    });

    add("c-opaque?", [this](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        return types::make_bool(!a.empty() && types::is_opaque(a[0]));
    });

    add("c-opaque->int", [this, oh](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (a.empty() || !types::is_opaque(a[0]))
            return make_int(0);
        auto idx = types::as_opaque_idx(a[0]);
        if (idx >= oh->size())
            return make_int(0);
        return make_int(reinterpret_cast<std::int64_t>((*oh)[idx]));
    });

    add("c-alloc", [this, oh, coverage_counters](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (coverage_counters)
            (*coverage_counters)[8]++;
        if (a.empty() || !types::is_int(a[0]))
            return make_int(0);
        auto size = static_cast<std::size_t>(types::as_int(a[0]));
        if (size == 0)
            return make_int(0);
        auto* ptr = std::calloc(1, size);
        auto idx = oh->size();
        oh->push_back(ptr);
        // Issue #980: track allocation size for bounds checks.
        opaque_sizes_[ptr] = size;
        return types::make_opaque(idx);
    });

    add("c-free", [this, oh](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (a.empty() || !types::is_opaque(a[0]))
            return make_void();
        auto idx = types::as_opaque_idx(a[0]);
        if (idx >= oh->size())
            return make_void();
        void* ptr = (*oh)[idx];
        // Issue #1230: double-free / free-unknown detection.
        if (ptr == nullptr) {
            ++double_free_total_;
            return make_void();
        }
        auto it = opaque_sizes_.find(ptr);
        if (it == opaque_sizes_.end()) {
            // Raw c-opaque or already freed untracked pointer.
            ++free_unknown_total_;
        } else {
            opaque_sizes_.erase(it);
        }
        std::free(ptr);
        (*oh)[idx] = nullptr;
        return make_void();
    });

    // Issue #1230: (ffi:opaque-stats) → hash {count, total-bytes, free-unknown, double-free}
    // Facade-only via prim_registrar intercept (is_legacy_stats_name: *-stats).
    add("ffi:opaque-stats", [this](std::span<const EvalValue>) -> EvalValue {
        // Return a small pair-list style int vector via fixnums only:
        // use opaque_count encoded as int for Agent simplicity when hash
        // builder is not available here. Prefer multi-int via list of pairs
        // is heavy — return total_bytes as primary signal + count via side.
        // Actually return count as int for minimal path; full hash via
        // dashboard metrics. Count is the primary leak signal.
        (void)opaque_total_bytes();
        return make_int(static_cast<std::int64_t>(opaque_count()));
    });

    add("c-struct-size", [this](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        std::size_t total = 0;
        for (auto& arg : a) {
            if (types::is_int(arg))
                total += static_cast<std::size_t>(types::as_int(arg));
        }
        return make_int(static_cast<std::int64_t>(total));
    });

    add("c-struct-set!", [this, oh, coverage_counters](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (coverage_counters)
            (*coverage_counters)[8]++;
        if (a.size() < 3 || !types::is_opaque(a[0]) || !types::is_int(a[1]))
            return make_void();
        auto oi = types::as_opaque_idx(a[0]);
        if (oi >= oh->size() || !(*oh)[oi])
            return make_void();
        if (types::as_int(a[1]) < 0)
            return make_void();
        auto offset = static_cast<std::size_t>(types::as_int(a[1]));
        auto* base = static_cast<char*>((*oh)[oi]);
        auto& val = a[2];
        // Issue #980: bounds check against c-alloc tracked size.
        auto need = [&](std::size_t nbytes) -> bool {
            auto it = opaque_sizes_.find(base);
            if (it == opaque_sizes_.end())
                return true; // unknown size (raw c-opaque) — legacy allow
            return offset <= it->second && nbytes <= it->second - offset;
        };
        if (types::is_int(val)) {
            auto v = types::as_int(val);
            if (!need(sizeof(v)))
                return make_void();
            std::memcpy(base + offset, &v, sizeof(v));
        } else if (types::is_float(val)) {
            auto v = types::as_float(val);
            if (!need(sizeof(v)))
                return make_void();
            std::memcpy(base + offset, &v, sizeof(v));
        } else if (types::is_opaque(val)) {
            auto vi = types::as_opaque_idx(val);
            auto* ptr = vi < oh->size() ? (*oh)[vi] : nullptr;
            if (!need(sizeof(ptr)))
                return make_void();
            std::memcpy(base + offset, &ptr, sizeof(ptr));
        }
        return make_void();
    });

    add("c-struct-ref", [this, oh, coverage_counters](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (coverage_counters)
            (*coverage_counters)[8]++;
        if (a.size() < 3 || !types::is_opaque(a[0]) || !types::is_int(a[1]) || !types::is_int(a[2]))
            return make_int(0);
        auto oi = types::as_opaque_idx(a[0]);
        if (oi >= oh->size() || !(*oh)[oi])
            return make_int(0);
        if (types::as_int(a[1]) < 0)
            return make_int(0);
        auto offset = static_cast<std::size_t>(types::as_int(a[1]));
        auto type = static_cast<int>(types::as_int(a[2]));
        auto* base = static_cast<const char*>((*oh)[oi]);
        auto need = [&](std::size_t nbytes) -> bool {
            auto it = opaque_sizes_.find(const_cast<char*>(base));
            if (it == opaque_sizes_.end())
                return true;
            return offset <= it->second && nbytes <= it->second - offset;
        };
        if (type == 0) {
            std::int64_t v = 0;
            if (!need(sizeof(v)))
                return make_int(0);
            std::memcpy(&v, base + offset, sizeof(v));
            return make_int(v);
        } else if (type == 1) {
            double v = 0;
            if (!need(sizeof(v)))
                return make_int(0);
            std::memcpy(&v, base + offset, sizeof(v));
            return types::make_float(v);
        } else if (type == 2) {
            void* ptr = nullptr;
            if (!need(sizeof(ptr)))
                return make_int(0);
            std::memcpy(&ptr, base + offset, sizeof(ptr));
            auto ni = oh->size();
            oh->push_back(ptr);
            return types::make_opaque(ni);
        }
        return make_int(0);
    });

    // Issue #1354: (c-render-bind lib-id "binding-name" "c-symbol" "signature") → #t/#f
    // Resolves dlsym from lib-id (-1 = RTLD_DEFAULT) and registers in RenderFfiRegistry.
    add("c-render-bind", [this, sh](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (a.size() < 4 || !types::is_int(a[0]) || !types::is_string(a[1]) ||
            !types::is_string(a[2]) || !types::is_string(a[3]))
            return make_bool(false);
        if (!sh)
            return make_bool(false);
        auto raw_lib_id = types::as_int(a[0]);
        void* lib = RTLD_DEFAULT;
        if (raw_lib_id >= 0) {
            auto lib_idx = static_cast<std::size_t>(raw_lib_id);
            if (lib_idx >= libs_.size())
                return make_bool(false);
            lib = libs_[lib_idx];
        }
        const auto& name = (*sh)[types::as_string_idx(a[1])];
        const auto& c_name = (*sh)[types::as_string_idx(a[2])];
        const auto& sig = (*sh)[types::as_string_idx(a[3])];
        void* fn_ptr = ::dlsym(lib, c_name.c_str());
        // Allow register even if unresolved (fn_ptr null) for Agent discovery of planned binds.
        // Prefer non-null when available.
        auto& reg = render_ffi_registry();
        int rc = reg.register_binding(name, c_name, sig, fn_ptr);
        return make_bool(rc == 0);
    });

    // Issue #1354/#1560: (c-render-call "binding-name" [arg0 ...]) → #t/#f
    // Real batch hot-path dispatch (ffi_hot_path + registry). MetricsOnly ABI still
    // returns #t when binding exists (Agent backends that only need resolve/metrics).
    add("c-render-call", [sh](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]) || !sh)
            return make_bool(false);
        const auto& name = (*sh)[types::as_string_idx(a[0])];
        auto& reg = render_ffi_registry();
        {
            std::lock_guard<std::mutex> lock(reg.registry_mtx);
            if (reg.bindings.find(name) == reg.bindings.end())
                return make_bool(false);
        }
        // Collect optional int args for BatchArgs ABI.
        std::vector<std::int64_t> args;
        args.reserve(a.size() > 1 ? a.size() - 1 : 0);
        for (std::size_t i = 1; i < a.size(); ++i) {
            if (types::is_int(a[i]))
                args.push_back(types::as_int(a[i]));
        }
        // dispatch_batch_c_render enters hotpath + records dispatch.
        (void)aura::stdlib::render_ffi::dispatch_batch_c_render(name, args);
        return make_bool(true);
    });

    // Issue #1560: fixed-name render backends via hot path.
    // (c-render-draw [arg...]) → int (batch result or -1 if unbound)
    add("c-render-draw", [](std::span<const EvalValue> a) -> EvalValue {
        std::vector<std::int64_t> args;
        args.reserve(a.size());
        for (const auto& v : a) {
            if (types::is_int(v))
                args.push_back(types::as_int(v));
        }
        return make_int(aura::stdlib::render_ffi::dispatch_c_render_draw(args));
    });

    // (c-present-batch [arg...]) → int
    add("c-present-batch", [](std::span<const EvalValue> a) -> EvalValue {
        std::vector<std::int64_t> args;
        args.reserve(a.size());
        for (const auto& v : a) {
            if (types::is_int(v))
                args.push_back(types::as_int(v));
        }
        return make_int(aura::stdlib::render_ffi::dispatch_c_present_batch(args));
    });

    // (c-ansi-emit [arg...]) → int
    add("c-ansi-emit", [](std::span<const EvalValue> a) -> EvalValue {
        std::vector<std::int64_t> args;
        args.reserve(a.size());
        for (const auto& v : a) {
            if (types::is_int(v))
                args.push_back(types::as_int(v));
        }
        return make_int(aura::stdlib::render_ffi::dispatch_c_ansi_emit(args));
    });

    // Issue #2005: (ffi:pin-buffer [ptr:int] [gen:int] [arena_id:int]) → handle (int).
    // Allocates a LifetimePin, pins it to the FFI buffer, and arms
    // g_ffi_pin_defer_depth so compact_sweep / GCCollector defer destructive
    // reclaim while the pin is live. Used by the render hotpath + MutationBoundary
    // lightweight path (refines #2000 LifetimePin Phase 2). Returns a stable
    // handle (index into the FFI pin registry) for later (ffi:unpin-buffer).
    {
        static std::vector<std::unique_ptr<aura::core::lifetime::LifetimePin>> g_ffi_pin_registry;
        static std::mutex g_ffi_pin_registry_mtx;
        add("ffi:pin-buffer", [](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 2 || !types::is_int(a[0]) || !types::is_int(a[1]))
                return make_int(-1);
            void* ptr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(types::as_int(a[0])));
            std::uint64_t gen = static_cast<std::uint64_t>(types::as_int(a[1]));
            std::uint64_t arena_id = (a.size() >= 3 && types::is_int(a[2]))
                                         ? static_cast<std::uint64_t>(types::as_int(a[2]))
                                         : 0;
            auto pin = std::make_unique<aura::core::lifetime::LifetimePin>();
            pin->pin(ptr, gen, arena_id);
            aura::gc_hooks::arm_ffi_pin_defer();
            std::lock_guard<std::mutex> lock(g_ffi_pin_registry_mtx);
            const std::int64_t handle = static_cast<std::int64_t>(g_ffi_pin_registry.size());
            g_ffi_pin_registry.push_back(std::move(pin));
            return make_int(handle);
        });
        add("ffi:unpin-buffer", [](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !types::is_int(a[0]))
                return make_int(0);
            const std::size_t idx = static_cast<std::size_t>(types::as_int(a[0]));
            std::lock_guard<std::mutex> lock(g_ffi_pin_registry_mtx);
            if (idx >= g_ffi_pin_registry.size() || !g_ffi_pin_registry[idx])
                return make_int(0);
            g_ffi_pin_registry[idx]->unpin_on_compact();
            g_ffi_pin_registry[idx].reset();
            aura::gc_hooks::release_ffi_pin_defer();
            return make_int(1);
        });
    }

    // Issue #2005: (query:ffi-pin-count) → live FFI LifetimePin count (int).
    {
        static std::vector<std::unique_ptr<aura::core::lifetime::LifetimePin>> g_ffi_pin_registry;
        static std::mutex g_ffi_pin_registry_mtx;
        add("query:ffi-pin-count", [](std::span<const EvalValue>) -> EvalValue {
            std::lock_guard<std::mutex> lock(g_ffi_pin_registry_mtx);
            std::int64_t n = 0;
            for (const auto& p : g_ffi_pin_registry)
                if (p && p->pinned())
                    ++n;
            return make_int(n);
        });
    }

    // Issue #1354: (query:render-ffi-count) → registered binding count (int).
    // Full Agent hash lives at query:render-ffi-available (evaluator partition).
    add("query:render-ffi-count", [](std::span<const EvalValue>) -> EvalValue {
        auto& reg = render_ffi_registry();
        return make_int(static_cast<std::int64_t>(reg.registered.load(std::memory_order_relaxed)));
    });
}

} // namespace aura::compiler
