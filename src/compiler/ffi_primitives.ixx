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
    // Issue #4597: parallel to arg_types. Empty unless the arg is kind 5
    // (Handle:<tag>). A 4-field brace init leaves this empty.
    std::vector<std::string> arg_handle_tags;
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
    // handle_tags, when non-null, is cleared and then filled in parallel with
    // the arguments this call appends (empty string unless the arg is
    // Handle:<tag>). arg_types is not cleared: callers pass an empty vector
    // and expect append. Handle is not a return type.
    static bool parse_ffi_sig(const std::string& sig, int& ret_type, std::vector<int>& arg_types,
                              std::string* err_type = nullptr,
                              std::vector<std::string>* handle_tags = nullptr) {
        if (handle_tags)
            handle_tags->clear();
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
        // Issue #4597: kind 5 is Handle:<tag>. The tag is the text after the
        // colon (no spaces — the arg list splits on spaces and parens).
        auto type_to_int = [](std::string_view tn, std::string* err, std::string* tag) -> int {
            auto t = tn;
            while (!t.empty() && t.front() == ' ')
                t.remove_prefix(1);
            while (!t.empty() && t.back() == ' ')
                t.remove_suffix(1);
            if (tag)
                tag->clear();
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
            constexpr std::string_view kHandle = "Handle:";
            if (t == "Handle" || t.starts_with(kHandle)) {
                if (!t.starts_with(kHandle) || t.size() == kHandle.size()) {
                    if (err)
                        *err = "Handle requires a tag (Handle:<tag>)";
                    return -1;
                }
                if (tag)
                    *tag = std::string(t.substr(kHandle.size()));
                return 5;
            }
            if (err)
                *err = t.empty() ? std::string{"empty type"} : "unknown type: " + std::string{t};
            return -1;
        };
        auto push_arg = [&](int at, std::string tag) {
            arg_types.push_back(at);
            if (handle_tags)
                handle_tags->push_back(std::move(tag));
        };
        std::string cur;
        for (auto c : arg_part) {
            if (c == ' ' || c == '(' || c == ')') {
                if (!cur.empty()) {
                    std::string tag;
                    int at = type_to_int(cur, err_type, &tag);
                    if (at < 0)
                        return false;
                    push_arg(at, std::move(tag));
                    cur.clear();
                }
                continue;
            }
            cur += c;
        }
        if (!cur.empty()) {
            std::string tag;
            int at = type_to_int(cur, err_type, &tag);
            if (at < 0)
                return false;
            push_arg(at, std::move(tag));
        }
        std::string ret_tag;
        ret_type = type_to_int(ret_part, err_type, &ret_tag);
        if (ret_type < 0)
            return false;
        if (ret_type == 5) {
            if (err_type)
                *err_type = "Handle is not a return type";
            return false;
        }
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

    // ── Issue #4556 / #4597: native memory views + native handles ─────
    // A view is an opaque_heap_ slot holding owner_base + offset, plus this
    // record: the element kind, the count, the owner's slot, and the owner's
    // generation at the time the view was made. Every element access
    // re-reads the owner's slot, so a c-free'd / closed owner (generation
    // bump) makes the view raise instead of reading freed memory. A handle
    // is an opaque slot with a finalizer that runs exactly once, on
    // c-close!, c-free, file-mode reclaim, or gc-heap / gc-temp. The
    // destructor does not run finalizers. Both tables are guarded by the
    // Evaluator's alloc_storage_lock_ (opaque_alloc_mu_).
    //
    // Issue #4597: the opaque index packs slot (low 32) and generation.
    // Closing or freeing a slot bumps the generation and freelists the
    // index. A stale value's generation does not match, so it cannot alias
    // the new tenant. Generation stops at kOpaqueGenMax (index bit 56 of
    // the tagged word is the sign bit); a slot that would wrap is nulled
    // and not freelisted.
    enum class ViewKind : std::uint8_t { F64, I64, U64, I32, U32, U8 };
    struct NativeView {
        std::size_t owner = 0;  // opaque slot of the owning block
        std::size_t offset = 0; // byte offset inside the owner
        std::size_t count = 0;  // elements
        ViewKind kind = ViewKind::F64;
        std::uint32_t owner_gen = 0; // owner's generation when the view was made
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
    // Live when the slot is in range and its generation matches the packed
    // index. A missing generation entry is generation 0 (slots minted before
    // the vector was synced). Caller holds the alloc lock.
    [[nodiscard]] bool generation_matches(std::size_t slot, std::uint32_t gen) const noexcept {
        if (slot >= opaque_generation_.size())
            return gen == 0;
        return opaque_generation_[slot] == gen;
    }
    [[nodiscard]] bool locate_opaque(const std::vector<void*>& oh, std::uint64_t packed,
                                     std::uint32_t& slot) const noexcept {
        slot = types::opaque_slot_index(packed);
        if (static_cast<std::size_t>(slot) >= oh.size())
            return false;
        return generation_matches(slot, types::opaque_generation_of(packed));
    }
    [[nodiscard]] std::uint32_t opaque_generation_at(std::size_t slot) const noexcept {
        return slot < opaque_generation_.size() ? opaque_generation_[slot] : 0;
    }
    [[nodiscard]] std::span<const std::uint32_t> opaque_generations() const noexcept {
        return opaque_generation_;
    }
    // Set from the Evaluator ctor after register_primitives. Empty in
    // FFIRuntime unit tests: c-opaque->int of a stale slot then returns 0.
    void set_opaque_raise(std::function<types::EvalValue(std::string)> fn) {
        opaque_raise_ = std::move(fn);
    }
    [[nodiscard]] types::EvalValue raise_stale_opaque(std::string msg) const {
        if (opaque_raise_)
            return opaque_raise_(std::move(msg));
        return types::make_int(0);
    }
    // Mint a slot for p. Reuses a freelist index (its generation was bumped
    // when it was recycled) or appends. Does not record an alias.
    [[nodiscard]] std::uint64_t mint_opaque_slot(std::vector<void*>& oh, void* p);
    // Cover failed. A fresh gen-0 tail is popped. Anything else is recycled
    // so a freelist mint cannot pop_back a different slot.
    void abandon_opaque_slot(std::vector<void*>& oh, std::uint64_t packed);
    // Null the slot, drop an alias that names it, bump the generation, and
    // freelist the index. A generation that would wrap is nulled and kept
    // off the freelist. A generation mismatch is a no-op.
    void recycle_opaque_slot(std::vector<void*>& oh, std::uint64_t packed);
    // push_opaque_heap_for_test appended a raw element. Generation 0, not a
    // recycled slot.
    void note_appended_opaque(const std::vector<void*>& oh) { sync_opaque_generation(oh.size()); }
    // Snapshot (finalizer, ptr) for every live handle, count them, and drop
    // view / handle / alias / generation / freelist records. Does not clear
    // oh and does not call the finalizers — the caller runs them after it
    // drops the alloc lock (and any heap / module / workspace lock).
    [[nodiscard]] std::vector<std::pair<void*, void*>>
    drain_native_finalizers(const std::vector<void*>& oh);
    // A reachable view keeps its owner alive. seen is indexed by raw slot.
    void mark_live_view_owners(std::vector<std::uint8_t>& seen) const;
    // Handles whose slot is unmarked and not past seen (newborn slots are
    // kept). Same close as take_native_handle. Finalizers are not called.
    [[nodiscard]] std::vector<std::pair<void*, void*>>
    take_unreachable_handles(std::vector<void*>& oh, const std::vector<std::uint8_t>& seen);
    // Live handle whose tag equals `tag` and whose pointer is non-null.
    [[nodiscard]] bool live_tagged_handle(const std::vector<void*>& oh, std::uint64_t packed,
                                          std::string_view tag, void*& ptr) const {
        std::uint32_t slot = 0;
        if (!locate_opaque(oh, packed, slot))
            return false;
        auto it = native_handles_.find(slot);
        if (it == native_handles_.end() || oh[slot] == nullptr || it->second.tag != tag)
            return false;
        ptr = oh[slot];
        return true;
    }
    // Resolve a view to its first element. `packed` is the opaque index.
    // False when it is not a live view or its owner was freed / closed.
    // Caller holds the alloc lock.
    [[nodiscard]] bool resolve_native_view(const std::vector<void*>& oh, std::uint64_t packed,
                                           char*& base, NativeView& out) const {
        std::uint32_t slot = 0;
        if (!locate_opaque(oh, packed, slot))
            return false;
        auto it = native_views_.find(slot);
        if (it == native_views_.end() || oh[slot] == nullptr)
            return false;
        const auto& v = it->second;
        if (v.owner >= oh.size() || oh[v.owner] == nullptr)
            return false;
        if (opaque_generation_at(v.owner) != v.owner_gen)
            return false;
        base = static_cast<char*>(oh[v.owner]) + v.offset;
        out = v;
        return true;
    }
    [[nodiscard]] bool is_native_view(std::uint64_t packed) const {
        const auto slot = types::opaque_slot_index(packed);
        if (!generation_matches(slot, types::opaque_generation_of(packed)))
            return false;
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
    // Issue #4556: packed index already minted for p by c-opaque /
    // c-struct-ref, if that slot is still live and still holds p. Null
    // never dedups. The returned index includes the generation, so two
    // polls compare equal.
    [[nodiscard]] std::optional<std::uint64_t> reuse_opaque_alias(const std::vector<void*>* oh,
                                                                  void* p) const {
        if (p == nullptr || oh == nullptr)
            return std::nullopt;
        auto it = opaque_alias_slot_.find(p);
        if (it == opaque_alias_slot_.end())
            return std::nullopt;
        std::uint32_t slot = 0;
        if (!locate_opaque(*oh, it->second, slot) || (*oh)[slot] != p)
            return std::nullopt;
        return it->second;
    }
    // c-alloc size of ptr (0 when unknown: raw c-opaque / foreign).
    [[nodiscard]] std::size_t opaque_size_of(void* ptr) const {
        auto it = opaque_sizes_.find(ptr);
        return it == opaque_sizes_.end() ? 0 : it->second;
    }
    void add_native_view(std::size_t slot, NativeView v) { native_views_[slot] = v; }
    void retire_native_view(std::vector<void*>& oh, std::uint64_t packed) {
        std::uint32_t slot = 0;
        if (!locate_opaque(oh, packed, slot))
            return;
        native_views_.erase(slot);
        recycle_opaque_slot(oh, packed);
    }
    [[nodiscard]] void* ffi_func_ptr(std::uint64_t i) const {
        return i < funcs_.size() ? funcs_[i].fn_ptr : nullptr;
    }
    void add_native_handle(std::size_t slot, void* finalizer, std::string tag) {
        native_handles_[slot] = NativeHandle{finalizer, std::move(tag)};
    }
    [[nodiscard]] bool is_native_handle(std::uint64_t packed) const {
        const auto slot = types::opaque_slot_index(packed);
        if (!generation_matches(slot, types::opaque_generation_of(packed)))
            return false;
        return native_handles_.count(slot) != 0;
    }
    [[nodiscard]] bool native_handle_tag(std::uint64_t packed, std::string& out) const {
        const auto slot = types::opaque_slot_index(packed);
        if (!generation_matches(slot, types::opaque_generation_of(packed)))
            return false;
        auto it = native_handles_.find(slot);
        if (it == native_handles_.end())
            return false;
        out = it->second.tag;
        return true;
    }
    // Close a handle slot: recycle it and hand back (finalizer, ptr) for
    // the caller to run outside the lock. False when the generation does
    // not match, the slot is null, or it is not a handle. The record is
    // dropped with the close, so create/close churn does not grow this table.
    bool take_native_handle(std::vector<void*>& oh, std::uint64_t packed, void*& fin, void*& ptr) {
        std::uint32_t slot = 0;
        if (!locate_opaque(oh, packed, slot))
            return false;
        auto it = native_handles_.find(slot);
        if (it == native_handles_.end() || oh[slot] == nullptr)
            return false;
        fin = it->second.finalizer;
        ptr = oh[slot];
        native_handles_.erase(it);
        if (fin != nullptr)
            ++native_finalizers_run_;
        // recycle reads oh[slot] for the alias drop before it nulls.
        recycle_opaque_slot(oh, packed);
        return true;
    }
    // Destructor only. The heap was already cleared; slot records would
    // misdescribe a later tenant. Finalizers do not run here (a user
    // finalizer during teardown can re-enter a half-destroyed evaluator).
    // gc-heap / gc-temp use drain_native_finalizers instead.
    void forget_native_slots() noexcept {
        native_views_.clear();
        native_handles_.clear();
        opaque_alias_slot_.clear();
        opaque_generation_.clear();
        opaque_freelist_.clear();
        opaque_on_freelist_.clear();
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
    // Issue #4556: views / handles by raw slot; ptr -> packed index for the
    // c-opaque / c-struct-ref (pointer field) mints, so polling the same
    // address reuses its slot instead of growing opaque_heap_.
    std::unordered_map<std::size_t, NativeView> native_views_;
    std::unordered_map<std::size_t, NativeHandle> native_handles_;
    std::unordered_map<void*, std::uint64_t> opaque_alias_slot_;
    std::uint64_t native_finalizers_run_ = 0;
    // Issue #4597: parallel to opaque_heap_. Freelist entries already hold
    // the next generation; no value names that generation until mint.
    std::vector<std::uint32_t> opaque_generation_;
    std::vector<std::uint32_t> opaque_freelist_;
    std::vector<std::uint8_t> opaque_on_freelist_;
    std::function<types::EvalValue(std::string)> opaque_raise_;
    // Leave index bit 56 clear (pool shift 6 would set the tagged sign bit).
    static constexpr std::uint32_t kOpaqueGenMax = 0x00FFFFFFu;

    void sync_opaque_generation(std::size_t n) {
        if (opaque_generation_.size() < n)
            opaque_generation_.resize(n, 0);
        if (opaque_on_freelist_.size() < n)
            opaque_on_freelist_.resize(n, 0);
    }
    void drop_alias_for_slot(void* p, std::uint32_t slot) {
        if (p == nullptr)
            return;
        auto it = opaque_alias_slot_.find(p);
        if (it != opaque_alias_slot_.end() && types::opaque_slot_index(it->second) == slot)
            opaque_alias_slot_.erase(it);
    }
};

} // namespace aura::compiler
