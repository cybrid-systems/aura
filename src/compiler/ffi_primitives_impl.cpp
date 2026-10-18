// ffi_primitives_impl.cpp — Implementation of the FFI
// primitives extracted from evaluator partition TUs
// (Issue #131).
//
// The C FFI state (loaded libraries, registered C
// functions) was previously file-scope statics in
// the monolithic evaluator TU. Now it's per-FFIRuntime, allowing
// multiple evaluators with independent FFI state.

module;
#include "compiler/locked_string_heap.hh" // Issue #4543

#include <dlfcn.h>
#include "compiler/ffi_hot_path.hh"
#include "compiler/value_tags.h" // Issue #4532: ffi closure id namespace
#include "compiler/typed_mutation_audit.h"
#include "core/densify_consistency_report.h"
#include "core/gc_hooks.h" // Issue #2005: ffi_pin_defer_active + arm/release
#include "core/moving_densify_health.hh"

module aura.compiler.ffi_primitives;

import std;
import aura.compiler.value;
import aura.core.arena; // Issue #3274: note_ffi_opaque_alias_densify_cover (slot+canary triad)
import aura.core.lifetime_pin; // Issue #2005: (ffi:pin-buffer)

// (Issue #131 / #1354 / #1560).
//
// The C FFI state is per-FFIRuntime.
// Issue #2626: render-loop FfiRenderHotpathGuard / render_ffi registry removed.

namespace aura::compiler {
using EvalValue = types::EvalValue;
using namespace aura::compiler::types;

// Issue #3175: pin-count stays as a C++ body; SlimSurface scans add() only.
template <typename... Ts> void sink_query_prim(std::string_view name, Ts&&...) {
    (void)name;
}

// No-op stand-in after TUI/render hotpath removal (#2626).
struct FfiRenderHotpathGuard {
    FfiRenderHotpathGuard() = default;
};

// Issue #4396: production c-struct must not memcpy a densify-old opaque
// after Moving recycle and before the live container store. In-flight
// covers the recycle→publish gap (a previous green window still allows
// mutate). A red window refuses the same way. Soft/Off: first line.
// Not the apply LCP / remap / seq-skip chain.
[[nodiscard]] bool production_c_struct_densify_old_refuse(const void* eval_id) noexcept {
    if (!aura::compiler::typed_audit::production_defaults_active())
        return false;
    if (aura::core::densify_consistency::densify_in_flight_for(eval_id))
        return true;
    using namespace aura::core::moving_densify_health;
    return !window_would_allow_mutate(
        g_last_had_moving_densify.load(std::memory_order_relaxed) != 0,
        g_last_pin_contract_held.load(std::memory_order_relaxed) != 0,
        g_last_moving_incomplete_remap.load(std::memory_order_relaxed) != 0,
        g_last_untracked_kept.load(std::memory_order_relaxed),
        g_last_root_remap_fail_total.load(std::memory_order_relaxed));
}

// Issue #4416: the #4396 refuse is a check-then-act. After it returns, the
// opaque_heap_ element is only a local. Production copies that element
// under alloc_storage_lock_ (the mutex densify rewrite holds) and drops
// the lock before any #3210 canary acquire — live_compact holds the canary
// mutex and then takes alloc. bind_temporary_moving_live_ptr_any_arena
// either notes the address so Moving entry soft-gates, or blocks until
// recycle publishes the chased address and notes that. own_noted keeps the
// note until the caller finishes memcpy and interior registration.
// A refuse after the bind, or a Moving feature that could not note
// (sticky densify-off: bind is a no-op, #3123 can still recycle),
// fail-closes: do not memcpy. Creation-point observe / arm_observe is not
// this cover. Soft/Off: production_defaults_active is false — unlocked
// load, no canary, no alloc lock.
[[nodiscard]] bool production_c_struct_hold_base(std::vector<void*>* oh, std::uint64_t oi,
                                                 std::recursive_mutex* alloc_mu,
                                                 const void* eval_id, char*& base_out,
                                                 aura::ast::TemporaryMovingLivePtrCanary& canary) {
    if (!aura::compiler::typed_audit::production_defaults_active()) {
        if (oi >= oh->size() || !(*oh)[oi])
            return false;
        base_out = static_cast<char*>((*oh)[oi]);
        return true;
    }
    void* raw = nullptr;
    if (alloc_mu != nullptr) {
        std::lock_guard<std::recursive_mutex> alloc(*alloc_mu);
        if (oi >= oh->size() || !(*oh)[oi])
            return false;
        raw = (*oh)[oi];
    } else {
        if (oi >= oh->size() || !(*oh)[oi])
            return false;
        raw = (*oh)[oi];
    }
    if (production_c_struct_densify_old_refuse(eval_id))
        return false;
    bool noted = false;
    void* bound = aura::ast::bind_temporary_moving_live_ptr_any_arena(raw, &noted);
    if (noted)
        canary.own_noted(bound);
    else if (aura::ast::moving_compact_feature_enabled() != 0)
        return false;
    if (production_c_struct_densify_old_refuse(eval_id))
        return false;
    base_out = static_cast<char*>(bound);
    return true;
}

void FFIRuntime::register_primitives(RegisterFn add,
                                     ::aura::compiler::LockedStringHeap* string_heap,
                                     std::vector<void*>* opaque_heap,
                                     std::array<std::uint64_t, 16>* coverage_counters) {
    // Issue #3176: Evaluator ctor wraps `add` so c-* names go to
    // defer_std_host_prim (installed on require std/ffi + kEffectFfi).
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
        std::vector<std::string> handle_tags;
        if (types::is_string(a[2])) {
            auto sig = (*sh)[types::as_string_idx(a[2])];
            std::string sig_err;
            if (!parse_ffi_sig(sig, ret_type, arg_types, &sig_err, &handle_tags)) {
                std::println(std::cerr, "c-func: invalid signature '{}'", sig);
                std::println(std::cerr, "  reason: {}", sig_err);
                std::println(std::cerr, "  expected: \"(ArgType) -> RetType\"");
                std::println(std::cerr,
                             "  valid types: Int, Float, String, Opaque, Handle:<tag>, Void");
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
        // Issue #4553: reject a signature the call path cannot pass. The
        // old marshaler accepted any arity and silently dropped arguments
        // past the 6th; apply_closure now passes up to 16 integer-class
        // (Int / String / Opaque / Handle:tag) and 8 Float slots
        // (evaluator_pure.ixx kFfiMaxIntSlots / kFfiMaxSseSlots). Void is
        // only valid as the whole argument list ("(Void) -> Int" is a
        // nullary function). Kind 5 counts as integer-class.
        if (arg_types.size() == 1 && arg_types[0] == 0) {
            arg_types.clear();
            handle_tags.clear();
        }
        {
            std::size_t n_int = 0, n_sse = 0;
            bool void_arg = false;
            for (int at : arg_types) {
                if (at == 0)
                    void_arg = true;
                else if (at == 2)
                    ++n_sse;
                else
                    ++n_int;
            }
            if (void_arg || n_int > kFfiSigMaxIntSlots || n_sse > kFfiSigMaxSseSlots) {
                std::println(std::cerr, "c-func: unsupported signature for '{}'", name);
                if (void_arg)
                    std::println(std::cerr, "  reason: Void is not an argument type");
                else
                    std::println(std::cerr,
                                 "  reason: {} integer-class and {} Float arguments; at most {} "
                                 "integer-class (Int, String, Opaque, Handle:tag) and {} Float are "
                                 "supported",
                                 n_int, n_sse, kFfiSigMaxIntSlots, kFfiSigMaxSseSlots);
                return make_int(0);
            }
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
        FFIFunc rec;
        rec.fn_ptr = fn_ptr;
        rec.name = name;
        rec.ret_type = ret_type;
        rec.arg_types = std::move(arg_types);
        rec.arg_handle_tags = std::move(handle_tags);
        funcs_.push_back(std::move(rec));
        // Issue #4532: FFI ids carry their own namespace bit. The old
        // (1ULL << 63) tag died in the closure payload's pool shift — the
        // raw index then collided with TW closure ids in apply_closure.
        auto closure_id = types::make_ffi_closure_id(static_cast<std::uint64_t>(fidx));
        return types::make_closure(closure_id);
    });

    add("c-opaque", [this, oh, coverage_counters](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (coverage_counters)
            (*coverage_counters)[8]++;
        if (a.empty() || !types::is_int(a[0]))
            return make_int(0);
        auto addr = types::as_int(a[0]);
        void* p = reinterpret_cast<void*>(static_cast<std::uintptr_t>(addr));
        // Issue #4556: polling the same address reuses its slot (the slot
        // is valid while it still holds p — c-free / densify rewrite break
        // the match) instead of growing opaque_heap_ by one per call.
        if (auto reuse = reuse_opaque_alias(oh, p))
            return types::make_opaque(*reuse);
        auto packed = mint_opaque_slot(*oh, p);
        if (packed == ~std::uint64_t{0}) {
            const auto slot = static_cast<std::uint32_t>(oh->size());
            oh->push_back(p);
            note_appended_opaque(*oh);
            packed = types::pack_opaque_index(slot, 0);
        }
        if (p != nullptr)
            opaque_alias_slot_[p] = packed;
        // Issue #3022 / #3057: GENERAL_OBJECT_PIN_EXEMPT: external-native-addr
        // Not an arena create. Densify-tracked aliases in opaque_heap_
        // are slot-covered at known-root walk (#3057).
        aura::core::lifetime::note_ffi_opaque_create_exempt("external-native-addr");
        return types::make_opaque(packed);
    });

    add("c-opaque?", [this](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        return types::make_bool(!a.empty() && types::is_opaque(a[0]));
    });

    add("c-opaque->int", [this, oh](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (a.empty() || !types::is_opaque(a[0]))
            return make_int(0);
        const auto packed = types::as_opaque_idx(a[0]);
        // An index past the heap is not a recycled slot (unit tests and a
        // cleared heap). A generation mismatch must not return the new
        // tenant's pointer.
        if (types::opaque_slot_index(packed) >= oh->size())
            return make_int(0);
        std::uint32_t slot = 0;
        if (!locate_opaque(*oh, packed, slot))
            return raise_stale_opaque("c-opaque->int: stale opaque (the slot was recycled)");
        return make_int(reinterpret_cast<std::int64_t>((*oh)[slot]));
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
        // Issue #980: track allocation size for bounds checks. Record it
        // before the slot is published.
        opaque_sizes_[ptr] = size;
        auto packed = mint_opaque_slot(*oh, ptr);
        if (packed == ~std::uint64_t{0}) {
            const auto slot = static_cast<std::uint32_t>(oh->size());
            oh->push_back(ptr);
            note_appended_opaque(*oh);
            packed = types::pack_opaque_index(slot, 0);
        }
        // Issue #3022 / #3057: GENERAL_OBJECT_PIN_EXEMPT: libc-heap
        // calloc is not densify-tracked. Survive densify as-is.
        aura::core::lifetime::note_ffi_opaque_create_exempt("libc-heap");
        return types::make_opaque(packed);
    });

    add("c-free", [this, oh](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (a.empty() || !types::is_opaque(a[0]))
            return make_void();
        const auto packed = types::as_opaque_idx(a[0]);
        // Past the heap: nothing to free. A generation mismatch is a stale
        // value — do not free, take, or count a double-free of the new tenant.
        if (types::opaque_slot_index(packed) >= oh->size())
            return make_void();
        std::uint32_t slot = 0;
        if (!locate_opaque(*oh, packed, slot))
            return make_void();
        void* ptr = (*oh)[slot];
        // Issue #1230: double-free of a still-generation-matching null slot.
        // A recycled slot's second c-free misses above (generation bump).
        if (ptr == nullptr) {
            ++double_free_total_;
            return make_void();
        }
        // Issue #4556: c-free of a handle closes it (finalizer once, no
        // libc free of a foreign pointer); c-free of a view only retires
        // the view — the owner's memory is not the view's to free.
        if (is_native_handle(packed)) {
            void* fin = nullptr;
            void* hp = nullptr;
            bool closed = false;
            {
                std::unique_lock<std::recursive_mutex> lk;
                if (opaque_alloc_mu_ != nullptr)
                    lk = std::unique_lock<std::recursive_mutex>(*opaque_alloc_mu_);
                closed = take_native_handle(*oh, packed, fin, hp);
            }
            if (closed)
                run_native_finalizer(fin, hp);
            return make_void();
        }
        if (is_native_view(packed)) {
            std::unique_lock<std::recursive_mutex> lk;
            if (opaque_alloc_mu_ != nullptr)
                lk = std::unique_lock<std::recursive_mutex>(*opaque_alloc_mu_);
            retire_native_view(*oh, packed);
            return make_void();
        }
        auto it = opaque_sizes_.find(ptr);
        if (it == opaque_sizes_.end()) {
            // Raw c-opaque or already freed untracked pointer.
            ++free_unknown_total_;
        } else {
            // Issue #4068: drop interior slots inside this block before
            // the libc free. Ephemeral alias entries are not in this range.
            aura::ast::forget_struct_interior_slots_covering(ptr, it->second);
            opaque_sizes_.erase(it);
        }
        std::free(ptr);
        // recycle drops the alias from the pointer still stored in the slot,
        // then nulls it and bumps the generation.
        recycle_opaque_slot(*oh, packed);
        return make_void();
    });

    // Issue #1230 / #2628: (engine:metrics "ffi:opaque-stats") → count.
    // Facade-only via prim_registrar intercept (is_legacy_stats_name: *-stats).
    // Avoid add("…") string-literal form so inventory/docs scanners skip it.
    {
        const std::string opaque_stats_name = "ffi:opaque-stats";
        add(opaque_stats_name, [this](std::span<const EvalValue>) -> EvalValue {
            (void)opaque_total_bytes();
            return make_int(static_cast<std::int64_t>(opaque_count()));
        });
    }

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
        const auto base_packed = types::as_opaque_idx(a[0]);
        if (types::as_int(a[1]) < 0)
            return make_void();
        // A stale base must not memcpy through the new tenant.
        std::uint32_t oi = 0;
        if (!locate_opaque(*oh, base_packed, oi))
            return make_void();
        // Issue #4396: production_defaults_active and
        // densify_in_flight_for(densify_eval_id_) or !window_would_allow_mutate
        // refuse before memcpy. Recycle may already have reused this address.
        if (production_c_struct_densify_old_refuse(densify_eval_id_))
            return make_void();
        auto offset = static_cast<std::size_t>(types::as_int(a[1]));
        // Issue #4416: copy (*oh)[oi] under opaque_alloc_mu_, drop it, then
        // bind_temporary_moving_live_ptr_any_arena + own_noted across memcpy.
        // A later production_c_struct_densify_old_refuse fail-closes.
        // oi is the raw slot, not the packed index.
        aura::ast::TemporaryMovingLivePtrCanary base_canary;
        char* base = nullptr;
        if (!production_c_struct_hold_base(oh, oi, opaque_alloc_mu_, densify_eval_id_, base,
                                           base_canary))
            return make_void();
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
            // Issue #4244: the "pointer-sized int" FFI pattern can carry a
            // live arena address. Under Moving a resolving pattern (any live
            // arena's last_object_remap_ key or dtor_index_ live-tracked)
            // joins the durable interior-slot inventory (#4068 re-arm) so
            // the next densify rewrites this word; a non-arena int stays a
            // plain memcpy (Soft/Off zero extra remap — helper early-returns).
            if constexpr (sizeof(v) == sizeof(void*)) {
                (void)aura::ast::cover_interior_int_pattern_for_densify(
                    reinterpret_cast<void**>(base + offset), static_cast<std::uintptr_t>(v));
            }
        } else if (types::is_float(val)) {
            auto v = types::as_float(val);
            if (!need(sizeof(v)))
                return make_void();
            std::memcpy(base + offset, &v, sizeof(v));
            // Issue #4244: float bits can alias a live arena address the
            // same way — same resolve + durable-slot contract as the int
            // arm above.
            if constexpr (sizeof(v) == sizeof(void*)) {
                std::uintptr_t pattern = 0;
                std::memcpy(&pattern, &v, sizeof(pattern));
                (void)aura::ast::cover_interior_int_pattern_for_densify(
                    reinterpret_cast<void**>(base + offset), pattern);
            }
        } else if (types::is_opaque(val)) {
            const auto vpacked = types::as_opaque_idx(val);
            const auto vslot = types::opaque_slot_index(vpacked);
            void* ptr = nullptr;
            std::uint32_t vi = 0;
            if (vslot >= oh->size()) {
                ptr = nullptr; // out of range: write a null, as before
            } else if (!locate_opaque(*oh, vpacked, vi)) {
                // Stale: do not store the new tenant's pointer.
                return make_void();
            } else {
                ptr = (*oh)[vi];
            }
            if (!need(sizeof(ptr)))
                return make_void();
            std::memcpy(base + offset, &ptr, sizeof(ptr));
            // Issue #3947: interior opaque store is a live void** at
            // base+offset. Slot rewrite covers arena-tracked aliases
            // (same triad as opaque_heap_element_cover_or_required_fail).
            // libc-heap / external with no arena alias: helper EXEMPT
            // when !Moving (Soft/Off zero extra). Required + no slot
            // fail-closes — after memcpy the interior slot is live.
            void** interior = reinterpret_cast<void**>(base + offset);
            if (!aura::ast::opaque_heap_element_cover_or_required_fail(ptr, interior,
                                                                       "struct-interior")) {
                std::memset(base + offset, 0, sizeof(ptr));
                return make_void();
            }
            // Issue #4068: the cover helper's queue entry is consumed at
            // the end of this Moving window. The interior word lives at a
            // stable libc-heap base+offset, so re-arm it on the durable
            // side of the same inventory. Soft/Off: helper EXEMPT, this
            // register is a no-op.
            aura::ast::register_struct_interior_slot_for_densify(interior);
        }
        return make_void();
    });

    add("c-struct-ref", [this, oh, coverage_counters](std::span<const EvalValue> a) -> EvalValue {
        FfiRenderHotpathGuard hp;
        if (coverage_counters)
            (*coverage_counters)[8]++;
        if (a.size() < 3 || !types::is_opaque(a[0]) || !types::is_int(a[1]) || !types::is_int(a[2]))
            return make_int(0);
        const auto base_packed = types::as_opaque_idx(a[0]);
        if (types::as_int(a[1]) < 0)
            return make_int(0);
        // A stale base must not memcpy through the new tenant.
        std::uint32_t oi = 0;
        if (!locate_opaque(*oh, base_packed, oi))
            return make_int(0);
        // Issue #4396: production_defaults_active and
        // densify_in_flight_for(densify_eval_id_) or !window_would_allow_mutate
        // refuse before memcpy. Returns 0 so a ref can prove the set missed.
        if (production_c_struct_densify_old_refuse(densify_eval_id_))
            return make_int(0);
        auto offset = static_cast<std::size_t>(types::as_int(a[1]));
        auto type = static_cast<int>(types::as_int(a[2]));
        // Issue #4416: copy (*oh)[oi] under opaque_alloc_mu_, drop it, then
        // bind_temporary_moving_live_ptr_any_arena + own_noted across memcpy.
        // A later production_c_struct_densify_old_refuse fail-closes.
        // oi is the raw slot, not the packed index.
        aura::ast::TemporaryMovingLivePtrCanary base_canary;
        char* base = nullptr;
        if (!production_c_struct_hold_base(oh, oi, opaque_alloc_mu_, densify_eval_id_, base,
                                           base_canary))
            return make_int(0);
        auto need = [&](std::size_t nbytes) -> bool {
            auto it = opaque_sizes_.find(base);
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
            // Issue #4386: feature-on push takes the Evaluator's alloc mutex
            // then the rewrite mutex and stores the published new address.
            // The element address is not enqueued — realloc would free it
            // under the slot walk. Feature-off stays the unlocked push.
            const bool moving_gate = opaque_alloc_mu_ != nullptr && opaque_rewrite_mu_ != nullptr &&
                                     aura::ast::moving_compact_feature_enabled() != 0;
            std::unique_lock<std::recursive_mutex> moving_alloc;
            std::unique_lock<std::mutex> moving_rewrite;
            if (moving_gate) {
                moving_alloc = std::unique_lock<std::recursive_mutex>(*opaque_alloc_mu_);
                moving_rewrite = std::unique_lock<std::mutex>(*opaque_rewrite_mu_);
                if (opaque_rewrite_ && ptr) {
                    auto found = opaque_rewrite_->find(ptr);
                    if (found != opaque_rewrite_->end() && found->second)
                        ptr = found->second;
                }
            }
            // Issue #4556: a pointer field read repeatedly (polling a
            // native struct) reuses the slot minted for the same address.
            if (auto reuse = reuse_opaque_alias(oh, ptr))
                return types::make_opaque(*reuse);
            auto ni = mint_opaque_slot(*oh, ptr);
            if (ni == ~std::uint64_t{0}) {
                const auto slot = static_cast<std::uint32_t>(oh->size());
                oh->push_back(ptr);
                note_appended_opaque(*oh);
                ni = types::pack_opaque_index(slot, 0);
            }
            const auto ni_slot = types::opaque_slot_index(ni);
            // Issue #3022 / #3057 / #3274 / #3533: opaque-struct-copy may alias
            // an arena-tracked object (densify-tracked). Under production Moving
            // the alias joins the pin/slot/EXEMPT triad via slot-rewrite cover
            // (stable value slot, not this element) + #3210 canary
            // fail-closed backstop; Soft/Off falls back to EXEMPT (zero extra).
            // Required + no slot fail-closes at create (no live untracked ptr).
            // GENERAL_OBJECT_PIN_EXEMPT: opaque-struct-copy
            // note_ffi_opaque_alias_densify_cover (via required helper)
            void** slot = ni_slot < oh->size() ? &(*oh)[ni_slot] : nullptr;
            if (!aura::ast::opaque_heap_element_cover_or_required_fail(
                    ptr, slot, "opaque-struct-copy", /*enqueue_slot=*/!moving_gate)) {
                // A freelist mint must not pop_back the heap's tail.
                abandon_opaque_slot(*oh, ni);
                return make_int(0);
            }
            if (ptr != nullptr)
                opaque_alias_slot_[ptr] = ni;
            return types::make_opaque(ni);
        }
        return make_int(0);
    });

    // Issue #2626: c-render-* / c-present / c-ansi-emit removed.

    // Issue #2005 / #2048: shared FFI pin registry (single static — pin/unpin
    // and query:ffi-pin-count must share state). (ffi:pin-buffer) arms
    // g_ffi_pin_defer_depth so compact_sweep defers while any pin is live.
    {
        // File-scope-like statics at this block: one registry for all three prims.
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
            // Issue #3022: 4th arg != 0 → FfiOwned (blocks reclaim);
            // default FfiBorrowed (Moving allowed at PinOwner, live pin
            // still fail-closes Moving via live_pin_count).
            const bool owned = a.size() >= 4 && types::is_int(a[3]) && types::as_int(a[3]) != 0;
            if (owned)
                pin->mark_ffi_owned();
            else
                pin->mark_ffi_handoff(); // #2048: handoff signal on pin
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
        // Issue #2005 / #2048: (query:ffi-pin-count) → live FFI LifetimePin count.
        sink_query_prim("query:ffi-pin-count", [](std::span<const EvalValue>) -> EvalValue {
            std::lock_guard<std::mutex> lock(g_ffi_pin_registry_mtx);
            std::int64_t n = 0;
            for (const auto& p : g_ffi_pin_registry)
                if (p && p->pinned())
                    ++n;
            return make_int(n);
        });
    }
}

std::uint64_t FFIRuntime::mint_opaque_slot(std::vector<void*>& oh, void* p) {
    sync_opaque_generation(oh.size());
    while (!opaque_freelist_.empty()) {
        const auto slot = opaque_freelist_.back();
        opaque_freelist_.pop_back();
        if (static_cast<std::size_t>(slot) >= oh.size())
            continue;
        if (slot < opaque_on_freelist_.size())
            opaque_on_freelist_[slot] = 0;
        oh[slot] = p;
        const auto gen = slot < opaque_generation_.size() ? opaque_generation_[slot] : 0u;
        return types::pack_opaque_index(slot, gen);
    }
    // Caller appends. ~0 is not a packed index (see the declaration).
    return ~std::uint64_t{0};
}

void FFIRuntime::abandon_opaque_slot(std::vector<void*>& oh, std::uint64_t packed) {
    const auto slot = types::opaque_slot_index(packed);
    const auto gen = types::opaque_generation_of(packed);
    const bool on_list = slot < opaque_on_freelist_.size() && opaque_on_freelist_[slot] != 0;
    const bool fresh_tail = static_cast<std::size_t>(slot) + 1 == oh.size() && gen == 0 && !on_list;
    if (fresh_tail) {
        if (oh[slot] != nullptr)
            drop_alias_for_slot(oh[slot], slot);
        oh.pop_back();
        if (opaque_generation_.size() > oh.size())
            opaque_generation_.resize(oh.size());
        if (opaque_on_freelist_.size() > oh.size())
            opaque_on_freelist_.resize(oh.size());
        return;
    }
    recycle_opaque_slot(oh, packed);
}

void FFIRuntime::recycle_opaque_slot(std::vector<void*>& oh, std::uint64_t packed) {
    const auto slot = types::opaque_slot_index(packed);
    if (static_cast<std::size_t>(slot) >= oh.size())
        return;
    if (!generation_matches(slot, types::opaque_generation_of(packed)))
        return;
    sync_opaque_generation(oh.size());
    if (opaque_on_freelist_[slot] != 0)
        return;
    drop_alias_for_slot(oh[slot], slot);
    oh[slot] = nullptr;
    auto& g = opaque_generation_[slot];
    if (g >= kOpaqueGenMax)
        return;
    ++g;
    opaque_freelist_.push_back(slot);
    opaque_on_freelist_[slot] = 1;
}

std::vector<std::pair<void*, void*>>
FFIRuntime::drain_native_finalizers(const std::vector<void*>& oh) {
    std::vector<std::pair<void*, void*>> out;
    for (const auto& [slot, h] : native_handles_) {
        if (h.finalizer == nullptr || slot >= oh.size() || oh[slot] == nullptr)
            continue;
        out.emplace_back(h.finalizer, oh[slot]);
        ++native_finalizers_run_;
    }
    native_views_.clear();
    native_handles_.clear();
    opaque_alias_slot_.clear();
    opaque_generation_.clear();
    opaque_freelist_.clear();
    opaque_on_freelist_.clear();
    return out;
}

void FFIRuntime::mark_live_view_owners(std::vector<std::uint8_t>& seen) const {
    for (const auto& [slot, v] : native_views_) {
        if (slot >= seen.size() || seen[slot] == 0)
            continue;
        if (v.owner >= seen.size())
            continue;
        if (opaque_generation_at(v.owner) != v.owner_gen)
            continue;
        seen[v.owner] = 1;
    }
}

std::vector<std::pair<void*, void*>>
FFIRuntime::take_unreachable_handles(std::vector<void*>& oh,
                                     const std::vector<std::uint8_t>& seen) {
    std::vector<std::size_t> doomed;
    doomed.reserve(native_handles_.size());
    for (const auto& [slot, h] : native_handles_) {
        (void)h;
        if (slot >= seen.size() || seen[slot] != 0)
            continue;
        doomed.push_back(slot);
    }
    std::vector<std::pair<void*, void*>> out;
    for (const auto slot : doomed) {
        if (slot >= oh.size()) {
            native_handles_.erase(slot);
            continue;
        }
        const auto gen = opaque_generation_at(slot);
        const auto packed = types::pack_opaque_index(static_cast<std::uint32_t>(slot), gen);
        void* fin = nullptr;
        void* ptr = nullptr;
        if (!take_native_handle(oh, packed, fin, ptr)) {
            native_handles_.erase(slot);
            continue;
        }
        if (fin != nullptr && ptr != nullptr)
            out.emplace_back(fin, ptr);
    }
    return out;
}

} // namespace aura::compiler
