// ffi_primitives.ixx — C FFI primitives extracted from
// the monolithic evaluator TU (Issue #131).
//
// This module owns the FFI state (loaded libraries and
// registered C functions) and provides a single
// registration function that wires the FFI primitives
// into a Primitives table. Callers (currently only
// Evaluator's constructor) call register_ffi_primitives
// once during init.
//
// The FFI state was previously file-scope statics in
// the evaluator monolith. Moving it here is the first step
// of the Issue #131 refactor: reduce the size of
// the evaluator partition by extracting focused modules.

module;
#include "compiler/locked_string_heap.hh" // Issue #4543
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

export module aura.compiler.ffi_primitives;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

// Mirror of aura::compiler::PrimFn (defined in
// evaluator.ixx). To avoid the cyclic import, we
// re-declare the same alias here. The compiler enforces
// that the types are compatible at the call site.
using PrimFn = std::function<types::EvalValue(std::span<const types::EvalValue>)>;

// Issue #4553: c-func signature limits; must equal kFfiMaxIntSlots /
// kFfiMaxSseSlots in evaluator_pure.ixx (the call path's slot counts).
inline constexpr std::size_t kFfiSigMaxIntSlots = 16;
inline constexpr std::size_t kFfiSigMaxSseSlots = 8;

// FFI function record. Each registered C function gets
// one of these. The closures_ lookup uses the index.
struct FFIFunc {
    void* fn_ptr = nullptr;
    std::string name;
    int ret_type = 0;           // 0=void, 1=Int, 2=Float, 3=String, 4=Opaque
    std::vector<int> arg_types; // per-arg type tags
};

// Issue #131: state is now per-FFIRuntime instance (not
// file-scope statics). The previous file-scope statics
// (g_ffi_libs, g_ffi_funcs) are gone. Multiple evaluators
// can now have independent FFI state.
//
// To break the cyclic import between this module and
// evaluator.ixx, we accept a callback (RegisterFn)
// rather than a `Primitives&` directly. The callback
// signature matches `Primitives::add(string, PrimFn)`.
export class FFIRuntime {
public:
    using RegisterFn = std::function<void(std::string, PrimFn)>;

    // Returns true if signature was parsed successfully.
    // sig format: "(ArgType) -> RetType"
    // e.g. "(String) -> Int", "(Float Float) -> Float"
    //
    // Defined inline in the .ixx so test_issue_131 can use
    // it without linking ffi_primitives_impl.cpp (which
    // transitively depends on libaura-reflect symbols).
    static bool parse_ffi_sig(const std::string& sig, int& ret_type, std::vector<int>& arg_types,
                              std::string* err_type = nullptr) {
        auto arrow = sig.find("->");
        if (arrow == std::string::npos) {
            if (err_type)
                *err_type = "missing '->' in signature";
            return false;
        }
        if (sig.empty() || sig[0] != '(') {
            if (err_type)
                *err_type = "signature must start with '('";
            return false;
        }
        // Issue #982: require ')' closing the arg list before '->'.
        // Malformed e.g. "(Int -> Int" (missing ')') is rejected.
        auto close_paren = sig.find(')');
        if (close_paren == std::string::npos || close_paren > arrow) {
            if (err_type)
                *err_type = "missing ')' before '->' in signature";
            return false;
        }
        auto arg_part = sig.substr(1, close_paren - 1);
        auto ret_part = sig.substr(arrow + 2);
        auto type_to_int = [](std::string_view tn, std::string* err = nullptr) -> int {
            // tn is string_view — trim with remove_prefix/suffix (no pop_back).
            auto t = tn;
            while (!t.empty() && t.front() == ' ')
                t.remove_prefix(1);
            while (!t.empty() && t.back() == ' ')
                t.remove_suffix(1);
            if (t == "Int")
                return 1;
            if (t == "Float")
                return 2;
            if (t == "String")
                return 3;
            if (t == "Opaque")
                return 4;
            if (t == "Void")
                return 0;
            if (err)
                *err = t.empty() ? std::string{"empty type"} : "unknown type: " + std::string{t};
            return -1;
        };
        std::string cur;
        for (auto c : arg_part) {
            if (c == ' ' || c == '(' || c == ')') {
                if (!cur.empty()) {
                    int at = type_to_int(cur, err_type);
                    if (at < 0)
                        return false;
                    arg_types.push_back(at);
                    cur.clear();
                }
                continue;
            }
            cur += c;
        }
        if (!cur.empty()) {
            int at = type_to_int(cur, err_type);
            if (at < 0)
                return false;
            arg_types.push_back(at);
        }
        ret_type = type_to_int(ret_part, err_type);
        if (ret_type < 0)
            return false;
        return true;
    }

    // Register the FFI primitives (c-load, c-func,
    // c-opaque, c-alloc, c-free, c-struct-*) via the
    // given RegisterFn callback. The string_heap and
    // opaque_heap are accessed via the pointers (the
    // Evaluator owns them; this module just borrows).
    // The coverage_counters pointer is the Evaluator's
    // coverage array; nullable for tests.
    void register_primitives(RegisterFn add_primitive,
                             ::aura::compiler::LockedStringHeap* string_heap,
                             std::vector<void*>* opaque_heap,
                             std::array<std::uint64_t, 16>* coverage_counters = nullptr);

    // Accessors for the FFI state (for tests / debug).
    std::size_t lib_count() const { return libs_.size(); }
    std::size_t func_count() const { return funcs_.size(); }
    const FFIFunc& func_at(std::size_t i) const { return funcs_[i]; }
    void* lib_at(std::size_t i) const { return libs_[i]; }

    // Issue #1230 Phase 1: opaque resource stats for leak detection.
    [[nodiscard]] std::size_t opaque_count() const noexcept { return opaque_sizes_.size(); }
    [[nodiscard]] std::uint64_t opaque_total_bytes() const noexcept {
        std::uint64_t sum = 0;
        for (auto& [_, sz] : opaque_sizes_)
            sum += static_cast<std::uint64_t>(sz);
        return sum;
    }
    [[nodiscard]] std::uint64_t opaque_free_unknown_total() const noexcept {
        return free_unknown_total_;
    }
    [[nodiscard]] std::uint64_t opaque_double_free_total() const noexcept {
        return double_free_total_;
    }

    // Issue #4386: Evaluator publishes the alloc mutex, the rewrite mutex,
    // and the this-window old→new map. Null (tests that build FFIRuntime
    // alone) keeps the unlocked push. Set after register_primitives; the
    // lambdas run later.
    void set_opaque_heap_moving_gate(std::recursive_mutex* alloc_mu, std::mutex* rewrite_mu,
                                     std::unordered_map<void*, void*>* rewrite) noexcept {
        opaque_alloc_mu_ = alloc_mu;
        opaque_rewrite_mu_ = rewrite_mu;
        opaque_rewrite_ = rewrite;
    }

    // Issue #4396: plain c-struct eval is outside the mutation-boundary
    // TLS identity. Phase-5 and the auto-arm store this Evaluator.
    void set_densify_eval_id(const void* id) noexcept { densify_eval_id_ = id; }

    // ── Issue #4556: native memory views + native handles ──────────────
    // A view is an opaque_heap_ slot holding owner_base + offset, plus this
    // record: the element kind, the count, and the owner's slot. Every
    // element access re-reads the owner's slot, so a c-free'd / closed
    // owner makes the view raise instead of reading freed memory. A handle
    // is an opaque slot with a finalizer that runs exactly once, on
    // c-close! or c-free. Both tables are guarded by the Evaluator's
    // alloc_storage_lock_ (opaque_alloc_mu_).
    enum class ViewKind : std::uint8_t { F64, I64, U64, I32, U32, U8 };
    struct NativeView {
        std::size_t owner = 0;  // opaque slot of the owning block
        std::size_t offset = 0; // byte offset inside the owner
        std::size_t count = 0;  // elements
        ViewKind kind = ViewKind::F64;
    };
    struct NativeHandle {
        void* finalizer = nullptr; // void (*)(void*) or null
        std::string tag;
    };
    [[nodiscard]] static std::size_t view_width(ViewKind k) noexcept {
        switch (k) {
            case ViewKind::F64:
            case ViewKind::I64:
            case ViewKind::U64:
                return 8;
            case ViewKind::I32:
            case ViewKind::U32:
                return 4;
            case ViewKind::U8:
                return 1;
        }
        return 8;
    }
    // Resolve a view slot to its first element. False when slot is not a
    // view or its owner was freed / closed. Caller holds the alloc lock.
    [[nodiscard]] bool resolve_native_view(const std::vector<void*>& oh, std::size_t slot,
                                           char*& base, NativeView& out) const {
        auto it = native_views_.find(slot);
        if (it == native_views_.end())
            return false;
        const auto& v = it->second;
        if (slot >= oh.size() || oh[slot] == nullptr || v.owner >= oh.size() ||
            oh[v.owner] == nullptr)
            return false;
        base = static_cast<char*>(oh[v.owner]) + v.offset;
        out = v;
        return true;
    }
    [[nodiscard]] bool is_native_view(std::size_t slot) const {
        return native_views_.count(slot) != 0;
    }
    [[nodiscard]] std::size_t native_view_count() const noexcept { return native_views_.size(); }
    [[nodiscard]] std::size_t native_handle_count() const noexcept {
        return native_handles_.size();
    }
    [[nodiscard]] std::uint64_t native_finalizers_run() const noexcept {
        return native_finalizers_run_;
    }
    [[nodiscard]] std::size_t opaque_alias_count() const noexcept {
        return opaque_alias_slot_.size();
    }
    // Issue #4556: slot already minted for p by c-opaque / c-struct-ref,
    // if it still holds p. Null never dedups.
    [[nodiscard]] std::optional<std::size_t> reuse_opaque_alias(const std::vector<void*>* oh,
                                                                void* p) const {
        if (p == nullptr)
            return std::nullopt;
        auto it = opaque_alias_slot_.find(p);
        if (it == opaque_alias_slot_.end() || it->second >= oh->size() || (*oh)[it->second] != p)
            return std::nullopt;
        return it->second;
    }
    // c-alloc size of ptr (0 when unknown: raw c-opaque / foreign).
    [[nodiscard]] std::size_t opaque_size_of(void* ptr) const {
        auto it = opaque_sizes_.find(ptr);
        return it == opaque_sizes_.end() ? 0 : it->second;
    }
    void add_native_view(std::size_t slot, NativeView v) { native_views_[slot] = v; }
    void retire_native_view(std::vector<void*>& oh, std::size_t slot) {
        if (slot < oh.size())
            oh[slot] = nullptr;
        native_views_.erase(slot);
    }
    [[nodiscard]] void* ffi_func_ptr(std::uint64_t i) const {
        return i < funcs_.size() ? funcs_[i].fn_ptr : nullptr;
    }
    void add_native_handle(std::size_t slot, void* finalizer, std::string tag) {
        native_handles_[slot] = NativeHandle{finalizer, std::move(tag)};
    }
    [[nodiscard]] bool is_native_handle(std::size_t slot) const {
        return native_handles_.count(slot) != 0;
    }
    [[nodiscard]] bool native_handle_tag(std::size_t slot, std::string& out) const {
        auto it = native_handles_.find(slot);
        if (it == native_handles_.end())
            return false;
        out = it->second.tag;
        return true;
    }
    // Close a handle slot: null it and hand back (finalizer, ptr) for the
    // caller to run outside the lock. False when already closed. The
    // record is dropped with the close (a closed handle is a null slot),
    // so create/close churn does not grow this table.
    bool take_native_handle(std::vector<void*>& oh, std::size_t slot, void*& fin, void*& ptr) {
        auto it = native_handles_.find(slot);
        if (it == native_handles_.end() || slot >= oh.size() || oh[slot] == nullptr)
            return false;
        fin = it->second.finalizer;
        ptr = oh[slot];
        oh[slot] = nullptr;
        native_handles_.erase(it);
        if (fin != nullptr)
            ++native_finalizers_run_;
        return true;
    }
    // The opaque heap was cleared (gc-heap reset / teardown): slot ids are
    // recycled from 0, so view / handle / alias records keyed by slot would
    // misdescribe new tenants. Handle finalizers do not run here; the
    // handles' Aura references went with the heap (GC finalizers: follow-up).
    void forget_native_slots() noexcept {
        native_views_.clear();
        native_handles_.clear();
        opaque_alias_slot_.clear();
    }
    static void run_native_finalizer(void* fin, void* ptr) {
        if (fin != nullptr && ptr != nullptr)
            reinterpret_cast<void (*)(void*)>(fin)(ptr);
    }

private:
    std::vector<void*> libs_;
    std::vector<FFIFunc> funcs_;
    // Issue #980: allocation sizes for c-alloc'd opaques (keyed by pointer).
    // 0 / missing = unknown size (raw c-opaque) — bounds checks skipped.
    std::unordered_map<void*, std::size_t> opaque_sizes_;
    // Issue #1230: leak / free hygiene counters.
    std::uint64_t free_unknown_total_ = 0;
    std::uint64_t double_free_total_ = 0;
    // Issue #4386: non-owning. Evaluator outlives the lambdas.
    std::recursive_mutex* opaque_alloc_mu_ = nullptr;
    std::mutex* opaque_rewrite_mu_ = nullptr;
    std::unordered_map<void*, void*>* opaque_rewrite_ = nullptr;
    // Issue #4396: non-owning Evaluator*. Null until the ctor publishes it.
    const void* densify_eval_id_ = nullptr;
    // Issue #4556: views / handles by opaque slot; ptr -> slot for the
    // c-opaque / c-struct-ref (pointer field) mints, so polling the same
    // address reuses its slot instead of growing opaque_heap_.
    std::unordered_map<std::size_t, NativeView> native_views_;
    std::unordered_map<std::size_t, NativeHandle> native_handles_;
    std::unordered_map<void*, std::size_t> opaque_alias_slot_;
    std::uint64_t native_finalizers_run_ = 0;
};

} // namespace aura::compiler
