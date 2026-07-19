// Issue #221: include the PersistentChildVector header in the
// module's global module fragment (same trick as ast.ixx for
// gap_buffer.hh / persistent_child_vector.hh). The global
// fragment is processed BEFORE the module's purview, so the
// std includes in persistent_child_vector.hh don't conflict
// with `import std;`.
module;

#include "../core/persistent_child_vector.hh"
// Issue #441 (rolled into #450): bump_primitive_call_count
// needs the CompilerMetrics struct (defined in
// observability_metrics.h). observability_metrics.h is a
// plain header (not a module), so we include it directly
// here in the module preamble (avoids the import-only
// restriction on .h).
#include "observability_metrics.h"
#include "lock_order_audit.h"
#include "typed_mutation_audit.h" // Issue #1589
#include "primitives_meta.h"
#include "render_telemetry.hh"
#include "core/arena_auto_policy_stats.h"
#include "core/gc_hooks.h"
#include "core/resource_quota.hh" // Issue #1579
// Issue #1416: capability names for invoke_prim_with_telemetry gate
#include "security_capabilities.h"
// Issue #1368: aura_set_aot_metrics for set_compiler_metrics auto-wire
#include "runtime_shared.h"
// Issue #1443/#1445: aura_invoke_long_mutation_scheduler_hook (C ABI)
#include "aura_jit_bridge.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module aura.compiler.evaluator;
import aura.compiler.macro_expansion;
import std;
import aura.core;
import aura.compiler.ffi_primitives;
import aura.compiler.adt_runtime; // Step 2.3 wiring (exact FFI pattern)
import aura.diag;
import aura.compiler.value;
import aura.compiler.evaluator_pure;

namespace aura::compiler {


using EvalValue = types::EvalValue;

// Issue #913: dual-path PrimFn — free-function pointers use a bare FnPtr
// (no type-erasure heap); capturing lambdas use std::function. Both
// copyable. table_["name"] = lambda works via converting assignment.
export class PrimFn {
    using FnPtr = EvalValue (*)(std::span<const EvalValue>);
    FnPtr ptr_ = nullptr;
    std::function<EvalValue(std::span<const EvalValue>)> fn_;

public:
    PrimFn() = default;

    PrimFn(std::nullptr_t) noexcept {}

    // Free function / function pointer — zero-allocation dispatch path.
    PrimFn(FnPtr fn) noexcept
        : ptr_(fn) {}

    template <class F>
        requires(!std::is_same_v<std::decay_t<F>, PrimFn> &&
                 !std::is_same_v<std::decay_t<F>, std::nullptr_t> &&
                 !std::is_same_v<std::decay_t<F>, FnPtr>)
    PrimFn(F&& fn)
        : fn_(std::forward<F>(fn)) {}

    template <class F>
        requires(!std::is_same_v<std::decay_t<F>, PrimFn>)
    PrimFn& operator=(F&& f) {
        if constexpr (std::is_same_v<std::decay_t<F>, std::nullptr_t>) {
            ptr_ = nullptr;
            fn_ = nullptr;
        } else if constexpr (std::is_convertible_v<std::decay_t<F>, FnPtr>) {
            ptr_ = static_cast<FnPtr>(f);
            fn_ = nullptr;
        } else {
            ptr_ = nullptr;
            fn_ = std::forward<F>(f);
        }
        return *this;
    }

    EvalValue operator()(std::span<const EvalValue> args) const {
        if (ptr_)
            return ptr_(args);
        return fn_(args);
    }

    EvalValue operator()(std::initializer_list<EvalValue> args) const {
        return (*this)(std::span<const EvalValue>(args.begin(), args.size()));
    }

    explicit operator bool() const noexcept { return ptr_ != nullptr || static_cast<bool>(fn_); }

    [[nodiscard]] bool is_function_pointer() const noexcept { return ptr_ != nullptr; }
};

// Issue #480: lightweight metadata for self-describing primitives.
// Issue #697: category + schema/contracts for AI Agent extension kit.
export struct PrimMeta {
    std::uint8_t arity = 255; // 255 = variadic
    bool pure = true;
    std::uint8_t safety_flags = 0; // 0x01=mutates, 0x02=io, 0x04=fiber
    // Issue #926: Agent-visible performance / security tiers.
    // perf_tier: 0=unknown, 1=hot-path/native, 2=normal, 3=cold/recursive
    // security_level: 0=unknown, 1=safe, 2=sandboxed, 3=privileged/network
    std::uint8_t perf_tier = 0;
    std::uint8_t security_level = 0;
    // Issue #1434 / P1b: prefer (engine:metrics …) over direct query:*-stats.
    bool deprecated = false;
    // Issue #1563: render-critical / stable_hot_path — stronger deopt throttle
    // under high-frequency AI mutation of draw/present closures.
    bool render_critical = false;
    bool stable_hot_path = false;
    std::string doc;
    std::string category; // eda | sva | verification | general | deprecated | rendering
    std::string schema;   // e.g. "(int string) -> bool"
};

// Issue #1563: treat rendering hot-tier + explicit flags as render-critical.
[[nodiscard]] inline bool is_render_critical_meta(const PrimMeta& m) noexcept {
    if (m.render_critical || m.stable_hot_path)
        return true;
    return m.category == "rendering" && m.perf_tier == kPrimPerfHot;
}

export class Primitives {
public:
    // Issue #1356: compact hot-tier entry.
    struct HotEntry {
        std::string name;
        PrimFn fn;
        std::size_t slot = 0;
    };

    Primitives();
    // Issue #914: string_view primary API (string& converts implicitly).
    [[nodiscard]] std::optional<PrimFn> lookup(std::string_view n) const pre(!n.empty());
    void add(const std::string& name, PrimFn fn) { add(name, std::move(fn), PrimMeta{}); }
    void add(const std::string& name, PrimFn fn, PrimMeta meta) {
        const std::size_t slot = ordered_names_.size();
        ordered_names_.push_back(name);
        const bool is_hot = (meta.perf_tier == kPrimPerfHot);
        meta_.push_back(std::move(meta));
        fn_slots_.push_back(fn);
        table_[name] = std::move(fn);
        // Issue #899: O(1) reverse index for slot_for_name.
        name_to_slot_[ordered_names_.back()] = slot;
        // Issue #1356: provisional hot-tier entry (finalized after all regs).
        if (is_hot)
            hot_map_[ordered_names_.back()] = fn_slots_.back();
    }
    // Issue #709: O(1) slot dispatch — dense table parallel to ordered_names_.
    [[nodiscard]] std::optional<PrimFn> slot_lookup_fast(std::size_t slot) const {
        if (slot >= fn_slots_.size())
            return std::nullopt;
        return fn_slots_[slot];
    }
    // Issue #891/#1356: name lookup — HotTierTable first, then main table_.
    [[nodiscard]] std::optional<PrimFn> lookup_cstr(std::string_view name) const {
        if (name.empty())
            return std::nullopt;
        // Issue #1356: prefer compact hot map (hot-tier primitives).
        if (auto hit = hot_map_.find(name); hit != hot_map_.end()) {
            hot_dispatch_hits_.fetch_add(1, std::memory_order_relaxed);
            if (aura::core::arena_policy::in_render_hotpath())
                hot_dispatch_hits_render_.fetch_add(1, std::memory_order_relaxed);
            return hit->second;
        }
        if (aura::core::arena_policy::in_render_hotpath())
            cold_dispatch_fallback_.fetch_add(1, std::memory_order_relaxed);
        auto it = table_.find(name);
        return it != table_.end() ? std::optional(it->second) : std::nullopt;
    }
    void set_string_heap(std::pmr::vector<std::string>* h) { string_heap_ = h; }
    std::span<const std::string> string_heap() const { return *string_heap_; }
    std::pmr::vector<std::string>& string_heap() { return *string_heap_; }
    // Slot-based lookup for primitive values
    const std::string& name_for_slot(std::size_t slot) const { return ordered_names_[slot]; }
    // Issue #914: string_view (transparent reverse index).
    std::size_t slot_for_name(std::string_view name) const;
    std::size_t slot_count() const { return ordered_names_.size(); }
    // Issue #480: metadata accessors for primitive:describe.
    [[nodiscard]] const PrimMeta& meta_for_slot(std::size_t slot) const { return meta_[slot]; }
    [[nodiscard]] std::size_t documented_meta_count() const noexcept {
        std::size_t n = 0;
        for (const auto& m : meta_)
            if (!m.doc.empty())
                ++n;
        return n;
    }
    // Issue #697: post-registration meta backfill for domain primitives.
    void set_meta_for_name(const std::string& name, PrimMeta meta) {
        const auto slot = slot_for_name(name);
        if (slot < meta_.size())
            meta_[slot] = std::move(meta);
        // Keep hot map coherent when tier is backfilled later.
        if (slot < meta_.size() && slot < ordered_names_.size() && slot < fn_slots_.size()) {
            if (meta_[slot].perf_tier == kPrimPerfHot)
                hot_map_[ordered_names_[slot]] = fn_slots_[slot];
            else
                hot_map_.erase(ordered_names_[slot]);
        }
    }
    // Issue #1356: rebuild HotTierTable from meta_ after full registration.
    // Issue #1357: wrap hot PrimFn so IR + tree-walker paths both record latency.
    void finalize_hot_table() {
        hot_map_.clear();
        hot_entries_.clear();
        for (std::size_t i = 0; i < ordered_names_.size() && i < meta_.size(); ++i) {
            if (meta_[i].perf_tier != kPrimPerfHot)
                continue;
            // Wrap once for render-hotpath latency (skip if already wrapped — name sentinel).
            const std::string name = ordered_names_[i];
            PrimFn original = fn_slots_[i];
            PrimFn timed = [original,
                            name](std::span<const types::EvalValue> a) -> types::EvalValue {
                if (!aura::core::arena_policy::in_render_hotpath())
                    return original(a);
                const auto t0 = std::chrono::steady_clock::now();
                auto r = original(a);
                const auto ns =
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now() - t0)
                                                   .count());
                aura::compiler::render_telemetry::record_tracked_prim(name, ns);
                return r;
            };
            fn_slots_[i] = timed;
            table_[name] = timed;
            HotEntry e;
            e.name = name;
            e.fn = timed;
            e.slot = i;
            hot_entries_.push_back(std::move(e));
            hot_map_[name] = timed;
        }
        // Sort by name for stable iteration / optional binary search.
        std::sort(hot_entries_.begin(), hot_entries_.end(),
                  [](const HotEntry& a, const HotEntry& b) { return a.name < b.name; });
        hot_table_size_.store(hot_map_.size(), std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t hot_table_size() const noexcept {
        return hot_table_size_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t hot_dispatch_hits() const noexcept {
        return hot_dispatch_hits_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t hot_dispatch_hits_render() const noexcept {
        return hot_dispatch_hits_render_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t cold_dispatch_fallback() const noexcept {
        return cold_dispatch_fallback_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const std::vector<HotEntry>& hot_entries() const noexcept { return hot_entries_; }
    [[nodiscard]] std::size_t category_meta_count(std::string_view category) const noexcept {
        std::size_t n = 0;
        for (const auto& m : meta_)
            if (m.category == category)
                ++n;
        return n;
    }
    [[nodiscard]] std::size_t schema_documented_meta_count() const noexcept {
        std::size_t n = 0;
        for (const auto& m : meta_)
            if (!m.doc.empty() && !m.schema.empty())
                ++n;
        return n;
    }
    // Count meta with perf_tier=hot (may exceed hot_table until finalize).
    [[nodiscard]] std::size_t hot_meta_count() const noexcept {
        std::size_t n = 0;
        for (const auto& m : meta_)
            if (m.perf_tier == kPrimPerfHot)
                ++n;
        return n;
    }
    // Issue #1563: count render-critical / stable_hot_path registrations.
    [[nodiscard]] std::size_t render_critical_meta_count() const noexcept {
        std::size_t n = 0;
        for (const auto& m : meta_)
            if (is_render_critical_meta(m))
                ++n;
        return n;
    }
    [[nodiscard]] bool is_render_critical_name(std::string_view name) const noexcept {
        if (name.empty())
            return false;
        const auto slot = slot_for_name(name);
        if (slot >= meta_.size())
            return false;
        return is_render_critical_meta(meta_[slot]);
    }

public:
    // Transparent hash so lookup_cstr / slot_for_name / Env avoid temporary std::string
    // (#891/#914).
    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
        std::size_t operator()(const std::string& s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    struct StringEq {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    };

private:
    std::unordered_map<std::string, PrimFn, StringHash, StringEq> table_;
    // Issue #1356: HotTierTable — name → fn for kPrimPerfHot only.
    std::unordered_map<std::string, PrimFn, StringHash, StringEq> hot_map_;
    std::vector<HotEntry> hot_entries_;
    mutable std::atomic<std::uint64_t> hot_dispatch_hits_{0};
    mutable std::atomic<std::uint64_t> hot_dispatch_hits_render_{0};
    mutable std::atomic<std::uint64_t> cold_dispatch_fallback_{0};
    std::atomic<std::size_t> hot_table_size_{0};
    // Issue #899: reverse index name → slot for O(1) slot_for_name.
    std::unordered_map<std::string, std::size_t, StringHash, StringEq> name_to_slot_;
    // Issue #145 Phase 2.4: pmr-backed to match Evaluator's
    // string_heap_ arena allocation. Vector metadata lives in
    // the same monotonic arena as the underlying EvalValue /
    // Pair storage; std::string char buffers still heap-allocate
    // (Phase 2.4.1 will move string contents inline).
    std::pmr::vector<std::string>* string_heap_ = nullptr;
    std::vector<std::string> ordered_names_;
    std::vector<PrimMeta> meta_;
    std::vector<PrimFn> fn_slots_;
};

// Forward declaration: Evaluator is defined below.
// Required so Env can hold an Evaluator* back-pointer for
// Phase 2.2 SoA-walk migration (Issue #145).
export class Evaluator;

// Issue #348 / #1682: mark IfExpr nodes under a subtree for occurrence dirty
// (cycle-safe iterative DFS). Defined in evaluator_primitives_mutate.cpp.
export void auto_wire_k_occurrence_dirty_for_subtree(
    aura::ast::FlatAST& flat,
    const std::function<bool(aura::ast::NodeId, bool)>& set_occurrence_dirty_fn,
    aura::ast::NodeId root);

// EnvId — uint32_t index into Evaluator::env_frames_ arena.
// Full SoA documentation in §2.7.5 / §2.7.6 of
// C++26 module conventions (.clang-format). Declared here (above Env) so
// Env can hold an EnvId parent_id_ field. Issue #145 Phase 2.1.
export using EnvId = std::uint32_t;
export constexpr EnvId NULL_ENV_ID = std::numeric_limits<EnvId>::max();

// Issue #356: INVALID_VERSION sentinel for frames that were
// allocated during a doomed transaction and survived a panic
// checkpoint restore. Materialize_call_env + the
// refresh_stale_frame_in_walk walker treat a frame with
// version_ == INVALID_VERSION as unusable: skip + emit a
// distinct [#356 warning] instead of materializing or walking
// into it. This is the scope-limited compromise for #356
// ("[Follow-up #242-2] Arena rollback for env_frames_ via
// stable-id indirection"): the stable-id indirection refactor
// is a follow-up — for now we mark post-rollback frames
// unusable so materialize_call_env never returns a poisoned
// Env to a closure body. The frames stay allocated (memory
// cost is bounded by the doomed transaction's size), but the
// invariant "any frame reachable from a live Closure is
// usable" is preserved.
//
// UINT64_MAX: monotonic counter never reaches this in practice
// (would require 2^64 - 1 MutationBoundaryGuard entries).
export constexpr std::uint64_t INVALID_VERSION = std::numeric_limits<std::uint64_t>::max();

export class Env final {
public:
    Env() = default;
    explicit Env(const Env* p)
        : parent_(p)
        , owner_(p ? p->owner_ : nullptr)
        , parent_id_(p ? p->parent_id_ : NULL_ENV_ID) {}
    Env(const Env&) = default;
    Env& operator=(const Env&) = default;
    void set_parent(const Env* p) { parent_ = p; }
    void set_primitives(const Primitives* p) { primitives_ = p; }
    // P0 step 2 (EnvFrame SoA migration, symmetric to EnvFrame): removed
    // set_cells and internal cells_ pointer. Legacy Env now also pure
    // for bindings (returns raw sentinels from lookups); cell deref
    // always uses central Evaluator::cells_ (or explicit param to
    // lookup_cell_*). Prepares for dropping pointer chasing entirely.
    // Issue #145 Phase 2.2 — SoA walk infrastructure.
    //
    // `owner_` is a back-pointer to the owning Evaluator, used
    // by walk-aware methods (lookup_cell_ptr, lookup_cell_index)
    // to traverse the parent chain via env_frames_ index lookup
    // instead of raw pointer chase. Set when an Env is registered
    // with an Evaluator (top_, modules_ arena envs).
    //
    // `parent_id_` mirrors `parent_`: when set, it indexes the
    // parent frame in `owner_->env_frames_()`. When NULL_ENV_ID
    // (the default), the legacy `parent_` pointer walk is used.
    // Both representations coexist during the Phase 2.2.x
    // transition; once all walk sites are migrated and the
    // legacy pointer is dead, `parent_` will be deleted (Phase
    // 2.6 — the rename).
    void set_owner(Evaluator* e) { owner_ = e; }
    [[nodiscard]] Evaluator* owner() const { return owner_; }
    void set_parent_id(EnvId id) { parent_id_ = id; }
    [[nodiscard]] EnvId parent_id() const { return parent_id_; }
    // Issue #207 / #1482 restore: route through bind_with_linear_state so
    // when pool_ is set the SymId PRIMARY array stays in lockstep with
    // string bindings_ (capture via alloc_env_frame_from_env needs
    // bindings_symid_; set!/lookup need string or SymId after materialize).
    void bind(std::string_view n, types::EvalValue v) {
        // 0 == linear_rt::Untracked (namespace defined later in this module)
        bind_with_linear_state(n, std::move(v), 0);
    }
    // Issue #1482 restore: wholesale string-binding replace used by
    // materialize_call_env when rehydrating an EnvFrame that still
    // carries legacy bindings_ (Define/begin paths that bound before
    // pool was set). Rebuilds binding_index_ for O(1) lookup.
    void replace_string_bindings(std::span<const std::pair<std::string, types::EvalValue>> bs) {
        bindings_.assign(bs.begin(), bs.end());
        binding_index_.clear();
        for (std::size_t i = 0; i < bindings_.size(); ++i)
            binding_index_[bindings_[i].first] = i;
    }
    // Issue #145: SymId fast path. The apply_closure loop hits
    // this once per parameter per call — replacing the old
    // string-compare lookup with integer-compare. Implemented
    // in evaluator_env.cpp.
    void bind_symid(aura::ast::SymId s, types::EvalValue v);
    // Issue #1539: bind with linear ownership state (defaults via bind_symid = Untracked).
    void bind_symid_with_linear_state(aura::ast::SymId s, types::EvalValue v, std::uint8_t state);
    void bind_with_linear_state(std::string_view n, types::EvalValue v, std::uint8_t state);
    bool set_linear_ownership_state(aura::ast::SymId s, std::uint8_t state);
    bool set_linear_ownership_state_by_name(std::string_view n, std::uint8_t state);
    // Issue #145: Optional pool reference. When set, bind_symid
    // mirrors the binding into the string-keyed bindings_ so
    // legacy lookup(string) in the lambda body still finds the
    // param. Without this, body code that does (lookup name)
    // would miss lambda params.
    void set_pool(const aura::ast::StringPool* p) { pool_ = p; }
    // Issue #145: SymId-based lookup. Fast path — integer compare
    // instead of string compare. Returns the most recent binding
    // (shadowing semantics preserved).
    std::optional<types::EvalValue> lookup_by_symid(aura::ast::SymId s) const;
    // Issue #914: string_view primary lookup (literals / views skip temp string).
    [[nodiscard]] std::optional<types::EvalValue> lookup(std::string_view n) const pre(!n.empty());
    // Issue #145 follow-up / Phase 2.5.0: SymId-first lookup that
    // takes a name string. Interns via the given pool (canonical
    // pool in the post-migration path), then routes through
    // lookup_by_symid. This is the migration scaffold for the
    // Phase 2.5 drop of bindings_ — callers that have a string
    // name but want the SymId fast path can use this helper
    // instead of interning the name themselves. The intern cost
    // is paid once per call; for hot lookups, callers should
    // intern the name once outside the loop.
    //
    // The `pool` parameter is the legacy pool (closure-captured,
    // env-captured). Pass canonical_pool() for new code; the
    // helper routes through pool_or_canonical semantics for
    // backward compat with pre-migration captures.
    std::optional<types::EvalValue> lookup_by_intern(std::string_view n,
                                                     const aura::ast::StringPool* pool) const
        pre(!n.empty());
    // Look up the raw binding without dereferencing cells (returns cell sentinel as-is)
    std::optional<types::EvalValue> lookup_binding(std::string_view n) const pre(!n.empty());
    std::optional<PrimFn> lookup_primitive(std::string_view n) const {
        return primitives_ ? primitives_->lookup(n) : std::nullopt;
    }
    types::EvalValue* lookup_cell_ptr(std::string_view n,
                                      std::vector<types::EvalValue>* cells) const;
    // Return cell index (stable across vector reallocation) or nullopt if not a cell
    std::optional<std::uint64_t> lookup_cell_index(std::string_view n) const;
    const Env* parent() const { return parent_; }
    // Issue #207 (Cycle 1): legacy bindings() accessors —
    // bumped for the bindings_legacy_uses metric.
    // Issue #210 (Cycle 4): these accessors are scheduled
    // for removal once all callers are migrated (per the
    // Cycle 2.5+ plan in #208). New code should use
    // bindings_symid_iter() or bindings_with_names().
    // The accessors still work and bump the
    // bindings_legacy_uses_ counter (for migration
    // observability). A full deprecation cycle (with
    // [[deprecated]] attribute or compile-time check)
    // is deferred until the migration is complete — see
    // the cleanup path in #208.
    std::vector<std::pair<std::string, types::EvalValue>>& bindings() {
        ++bindings_legacy_uses_;
        return bindings_;
    }
    std::span<const std::pair<std::string, types::EvalValue>> bindings() const {
        ++bindings_legacy_uses_;
        return bindings_;
    }
    // Issue #207: bindings_symid_iter() — preferred accessor
    // for new code. Returns a span over the SymId-keyed
    // bindings_symid_ array (the migration's primary storage
    // per the issue-174 plan). No string intern, no allocation;
    // just a view.
    [[nodiscard]] std::span<const std::pair<aura::ast::SymId, types::EvalValue>>
    bindings_symid_iter() const noexcept {
        return bindings_symid_;
    }
    // Issue #207: bindings_with_names() — materializes the
    // named version of the bindings. Uses pool_->resolve() to
    // get the name for each SymId. Returns a new vector;
    // the caller is expected to use it for display / debugging
    // (e.g., the env inspector primitive). Hot paths should
    // use bindings_symid_iter() instead — this helper pays
    // the resolve() cost per binding.
    [[nodiscard]] std::vector<std::pair<std::string, types::EvalValue>> bindings_with_names() const;
    // Issue #145: SymId-keyed view of the same bindings. Same
    // length and order as bindings(). Used by EnvView.
    std::span<const std::pair<aura::ast::SymId, types::EvalValue>> bindings_symid() const {
        return bindings_symid_;
    }
    // Mutable mirror of bindings_symid() for the SoA path:
    // Phase 2.3 materialize_call_env replaces the SymId-keyed
    // bindings wholesale from an EnvFrame. Kept as a thin
    // accessor so Env's invariants stay encapsulated.
    std::vector<std::pair<aura::ast::SymId, types::EvalValue>>& bindings_symid_mut() {
        return bindings_symid_;
    }
    // Issue #1539: mutable linear ownership SoA (materialize_call_env copy).
    std::vector<std::uint8_t>& bindings_linear_ownership_state_mut() {
        return bindings_linear_ownership_state_;
    }
    [[nodiscard]] std::span<const std::uint8_t> bindings_linear_ownership_state() const noexcept {
        return bindings_linear_ownership_state_;
    }

private:
    const Env* parent_ = nullptr;
    const Primitives* primitives_ = nullptr;
    // Issue #145: nullable legacy pool. Most envs built after
    // the canonical-pool migration leave this nullptr and rely
    // on Evaluator::canonical_pool() for symbol resolution.
    // The field is preserved (1) for backward compat with envs
    // built before the migration (their bindings were intern'd
    // in a different pool), and (2) for envs that intentionally
    // captured a non-canonical pool. Use
    // Evaluator::pool_or_canonical(pool_) at lookup sites to
    // get the right pool with the right fallback. Phase 2.5
    // drop (the original goal) removes this field after the
    // migration completes; see cpp26_guide.md §2.7.7.
    const aura::ast::StringPool* pool_ = nullptr; // Issue #145
    // P0 step 2: cells_ pointer removed (was used for cell deref in
    // lookups). Bindings now always return the raw value (cell
    // sentinel if applicable); deref centralized via Evaluator
    // or passed cells to lookup_cell helpers. This + EnvFrame
    // change eliminates one class of pointer-to-reallocatable-heap.
    std::vector<std::pair<std::string, types::EvalValue>> bindings_;
    // Issue #894 / #914: name → last local index; transparent hash for
    // string_view lookups without temporary std::string.
    std::unordered_map<std::string, std::size_t, Primitives::StringHash, Primitives::StringEq>
        binding_index_;
    // Issue #145: parallel SymId-keyed store. Both arrays
    // share the same length and order. lookup_by_symid reads
    // bindings_symid_ (integer compare). bind_symid writes to
    // both (and resolves SymId→string via pool_ to mirror).
    std::vector<std::pair<aura::ast::SymId, types::EvalValue>> bindings_symid_;
    // Issue #1539: parallel linear ownership SoA (same length as
    // bindings_symid_). Copied into EnvFrame by alloc_env_frame_from_env.
    std::vector<std::uint8_t> bindings_linear_ownership_state_;
    // Issue #145 Phase 2.2: SoA walk infrastructure.
    // `owner_` is the Evaluator that owns the env_frames_ arena
    // used for parent-chain walk. `parent_id_` is the index of
    // this Env's parent frame in that arena (NULL_ENV_ID =
    // no parent or legacy pointer walk). Set when the Env is
    // registered with an Evaluator (top_, modules_).
    Evaluator* owner_ = nullptr;
    EnvId parent_id_ = NULL_ENV_ID;
    // Issue #286: snapshot of `owner_->defuse_version_` at the
    // time this Env was materialized (for envs created via
    // materialize_call_env). The SoA walk in
    // `Env::lookup_cell_ptr` checks this against each frame's
    // version_ to detect a stale chain — same staleness
    // semantic as EnvFrame. Stale Envs (env_version_ <
    // current defuse_version_) still produce correct results
    // because each frame is independently stamped, but the
    // env-level stamp lets us surface a one-time warning in
    // materialize_call_env and skip re-walking already-validated
    // subchains.
    std::uint64_t env_version_ = 0;
    // Issue #207 (Cycle 1): bindings_legacy_uses_ counter.
    // Bumped on every access to the legacy bindings() accessor.
    // Provides observability for the migration from
    // string-keyed bindings_ to SymId-keyed bindings_symid_.
    // Cycle 2+ migrates callers incrementally, watching the
    // metric trend to 0.
    mutable std::size_t bindings_legacy_uses_ = 0;

public:
    // Issue #207: accessors for the metric.
    [[nodiscard]] std::size_t bindings_legacy_uses() const noexcept {
        return bindings_legacy_uses_;
    }
    void reset_bindings_legacy_uses() noexcept { bindings_legacy_uses_ = 0; }
    // Issue #286: accessors for the env_version_ snapshot stamp
    // (set by materialize_call_env; read by lookup_cell_ptr and
    // observability code).
    [[nodiscard]] std::uint64_t env_version() const noexcept { return env_version_; }
    void set_env_version(std::uint64_t v) noexcept { env_version_ = v; }
};

export using ClosureId = std::uint64_t;

// ═══════════════════════════════════════════════════════════════
// Issue #145 Phase 2.1 — EnvFrame SoA infrastructure
// ═══════════════════════════════════════════════════════════════
//
// Phase 1 (#145) shipped Closure::params as SymId[] and added
// EnvView / ClosureView zero-copy span views. Phase 2.1 adds
// the parallel SoA storage for Env: a `std::vector<EnvFrame>`
// arena owned by Evaluator, indexed by `EnvId` (uint32_t).
//
// Why uint32_t (not uint64_t like ClosureId)? 4G envs is plenty
// for any single evaluator lifetime. uint32_t halves the index
// width → better cache density when walking parent chains. If
// we ever need more, bumping to uint64_t is a one-line change.
//
// `NULL_ENV_ID` is UINT32_MAX (== uint32_t max). Off-by-one
// with `env_frames_.size()` is not possible because the
// invariant is: `id < env_frames_.size() || id == NULL_ENV_ID`.
//
// EnvFrame is structurally identical to Env (data layout
// parity is the Phase 2.2 migration contract — replacing Env
// with EnvFrame is a straight rename once we choose to flip
// the switch). Today Env and EnvFrame coexist: Env stays
// unchanged, EnvFrame is the new SoA arena. Future Phase 2.x
// sub-issues migrate call sites from Env to EnvFrame.

// EnvFrame — SoA-friendly data layout, parallel to Env.
// `parent_id_` replaces Env::parent_ (raw Env* pointer) with
// an index into Evaluator::env_frames_. Walking the parent
// chain becomes index lookups instead of pointer chases —
// cache-friendly and GC-safe (indices survive arena compaction
// if we ever adopt a moving collector).
// Issue #1539: runtime linear ownership state (mirrors IRInstruction
// linear_ownership_state in ir.ixx). Parallel SoA on EnvFrame (+ Env).
export namespace linear_rt {
    constexpr std::uint8_t Untracked = 0;
    constexpr std::uint8_t Owned = 1;
    constexpr std::uint8_t Borrowed = 2;
    constexpr std::uint8_t MutBorrowed = 3;
    constexpr std::uint8_t Moved = 4;
} // namespace linear_rt

// Forward declaration — EnvFrame body follows; resolve result types
// reference EnvFrame* (Issue #1756).
export struct EnvFrame;

// Issue #1756: detailed EnvFrame resolve status — callers must not
// treat all nullptrs from resolve_env_frame as the same failure.
// GENERATION_MISMATCH is reserved for free-list slot reuse (future
// #1360 follow-up); today terminal post-rollback uses INVALID_VERSION.
export enum class EnvFrameResolveStatus : std::uint8_t {
    OK = 0,
    NULL_ID = 1,             // id == NULL_ENV_ID
    OOB = 2,                 // id >= env_frames_.size()
    INVALID_VERSION = 3,     // live slot marked #356 INVALID_VERSION
    GENERATION_MISMATCH = 4, // reserved: freed slot reused under new gen
};

export struct EnvFrameResolveResult {
    const EnvFrame* frame = nullptr;
    EnvFrameResolveStatus status = EnvFrameResolveStatus::NULL_ID;
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == EnvFrameResolveStatus::OK && frame != nullptr;
    }
};

export struct EnvFrameResolveResultMut {
    EnvFrame* frame = nullptr;
    EnvFrameResolveStatus status = EnvFrameResolveStatus::NULL_ID;
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == EnvFrameResolveStatus::OK && frame != nullptr;
    }
};

export struct EnvFrame {
    EnvId parent_id = NULL_ENV_ID;
    const Primitives* primitives_ = nullptr;
    const aura::ast::StringPool* pool_ = nullptr;
    // Issue #242: snapshot of `defuse_version_` at allocation time.
    // Stale frames (frame.version_ < current defuse_version_) can
    // be detected by `Evaluator::is_env_frame_stale(id)`. The
    // post-mutation invariant check + materialize_call_env + parent
    // walks use this to log a warning + bump the frame's version
    // (so subsequent lookups see it as fresh) instead of reading
    // possibly-inconsistent bindings from a frame captured before a
    // mutation that invalidated the closure's scope.
    //
    // Default 0 means "never stamped" — frames allocated before
    // #242 ship have version_ == 0, and the current defuse_version_
    // is always >= 1 (incremented on every MutationBoundaryGuard
    // enter). So a version_ == 0 frame is always stale; this is
    // intentional — old frames should be re-validated.
    std::uint64_t version_ = 0;

    // Issue #1903: back-pointer to owning Evaluator. Used by
    // EnvFrame::ensure_dual_path_consistent() to bump the
    // dual-path consistency observability counters. Nullable
    // for stack-local frames in tests; the helper degrades
    // gracefully (no counter bump, no panic) when owner_ is null.
    Evaluator* owner_ = nullptr;

    // Issue #1384: explicit ctor that takes `version_` so the
    // "construct locally with version_ = current" pattern can't
    // be accidentally bypassed by future changes. Default ctor
    // still exists for legacy callers (e.g. emplace_back with
    // field-fill later — they must set version_ before push).
    EnvFrame(EnvId pid, const Primitives* prim, std::uint64_t version)
        : parent_id(pid)
        , primitives_(prim)
        , version_(version) {}
    EnvFrame() = default;

    // Issue #1903: owner back-pointer setter/getter. Mirrors Env's
    // pattern — env_frames_-allocated frames get owner_ set in
    // alloc_env_frame / alloc_env_frame_from_env right after the
    // push_back, so post-bind ensure_dual_path_consistent() can
    // route counter bumps through the Evaluator.
    void set_owner(Evaluator* e) noexcept { owner_ = e; }
    [[nodiscard]] Evaluator* owner() const noexcept { return owner_; }

    // P0 (EnvFrame SoA migration): removed raw cells_ pointer.
    // EnvFrame is now pure data (bindings + parent_id index).
    // Cell deref (when bound value is cell sentinel) is centralized
    // in Evaluator::lookup_by_symid_chain (and legacy Env paths)
    // using the Evaluator-owned central pmr cells_ vector.
    // This eliminates one source of pointer-to-reallocatable-heap
    // and prepares for full removal of cells_/pairs_ pointers from
    // all env representations. See evaluator_env.cpp lookup_local
    // variants and lookup_by_symid_chain.
    std::vector<std::pair<std::string, types::EvalValue>> bindings_;
    // Phase 1 parity: parallel SymId-keyed store. Same length
    // and order as bindings_. SymId fast path reads this;
    // bind_symid mirrors into both when pool_ is set.
    std::vector<std::pair<aura::ast::SymId, types::EvalValue>> bindings_symid_;
    // Issue #1539: parallel SoA for linear ownership (same length/order
    // as bindings_symid_). 0=untracked … 4=Moved. Walked by
    // linear_post_mutate_enforce.
    std::vector<std::uint8_t> bindings_linear_ownership_state_;

    // Bind by name. Resolves to SymId via pool_ if set, mirrors
    // to bindings_symid_ so legacy lookup_by_symid still finds
    // it. If pool_ is null, only the string array is written.
    // Ownership state defaults to Untracked (0).
    void bind(const std::string& n, types::EvalValue v);
    // Bind by SymId (fast path). Mirrors to bindings_ when
    // pool_ is set, so legacy lookup(string) in the lambda body
    // still finds the param. Ownership state defaults to Untracked.
    void bind_symid(aura::ast::SymId s, types::EvalValue v);
    // Issue #1539: bind with explicit linear ownership state.
    void bind_with_linear_state(const std::string& n, types::EvalValue v, std::uint8_t state);
    void bind_symid_with_linear_state(aura::ast::SymId s, types::EvalValue v, std::uint8_t state);
    // Mark most-recent binding of SymId / name as `state` (e.g. Moved).
    bool set_linear_ownership_state(aura::ast::SymId s, std::uint8_t state);
    bool set_linear_ownership_state_by_name(const std::string& n, std::uint8_t state);
    // Issue #1903: dual-path consistency enforcement. Called
    // from bind_with_linear_state / bind_symid_with_linear_state
    // (every mutate path under MutationBoundaryGuard),
    // complete_post_resume_steal_refresh (Fiber::resume / steal),
    // and post-materialize (after the bindings copy in
    // materialize_call_env). Detects drift between bindings_
    // (string-keyed) and bindings_symid_ (SymId-keyed), bumps the
    // appropriate observability counter via owner_ if set, and
    // returns true iff both paths are equivalent. In debug
    // builds, asserts on detected desync so the test suite
    // catches regressions early.
    //
    // Bumps envframe_dual_consistency_asserted_ on every call
    // (regardless of pass/fail). On desync: also bumps
    // envframe_desync_detected_. On pass: bumps
    // bindings_dual_sync_count_.
    [[nodiscard]] bool ensure_dual_path_consistent() const noexcept;
    // Local-only lookup (no parent walk). Phase 2.2 will add
    // walk-aware variants alongside `Evaluator::walk_env_frames`.
    std::optional<types::EvalValue> lookup_local(const std::string& n) const pre(!n.empty());
    std::optional<types::EvalValue> lookup_local_by_symid(aura::ast::SymId s) const;
};

export struct Pair {
    types::EvalValue car;
    types::EvalValue cdr;
};

export struct MacroDef {
    std::vector<std::string> params;
    bool dotted = false;
    // Issue #120: when true, macro expansion uses clone_macro_body
    // with a name_map (single-eval AST substitution + automatic
    // gensym for template-introduced bindings). When false, the
    // traditional double-eval path is used (body is evaluated
    // to data, data is re-evaluated as code). Hygenic macros
    // satisfy Ghuloum Step 16 — macro-introduced bindings
    // can't be captured by the call-site context, and
    // call-site bindings can't be captured by the macro.
    bool hygienic = false;
    // Issue #230 #2: when true, macro expansion binds the call's
    // args in a child env (env-binding path) instead of AST-
    // substituting them into the body. This is the right semantics
    // for symbol-generating macros like define-struct: the body
    // builds code that references the user's actual struct name
    // (a symbol) and field list (a list), and the env-binding path
    // makes these available as Variable lookups in the body,
    // matching what the non-hygienic `defmacro` path does. Set
    // by `define-hygienic-macro*` (the * variant).
    bool preserved = false;
    ast::FlatAST* flat = nullptr;
    ast::StringPool* pool = nullptr;
    ast::NodeId body_id = ast::NULL_NODE;
};

export struct Closure {
    std::string name = ""; // function name (empty for lambdas)
    // Issue #145: params are now stored as SymId (interned in the
    // closure's StringPool) instead of raw std::string. This is a
    // true SoA change: the hot path (apply_closure parameter
    // binding) was doing a string compare per arg; now it does an
    // integer compare. For typical 5-10 arg closures, this is
    // a measurable 3-5x speedup on the lookup loop.
    std::vector<aura::ast::SymId> params;
    ast::FlatAST* flat = nullptr;
    // Issue #145 follow-up / Phase 2.5.0: nullable legacy. Most
    // closures built after the canonical-pool migration set this
    // to nullptr and rely on Evaluator::canonical_pool() for
    // symbol resolution. The field is preserved (1) for
    // backward compat with closures built before the migration
    // (their params were intern'd in a different pool), and
    // (2) for closures that were intentionally captured in a
    // non-canonical pool (e.g. pattern-matching interning).
    // Use Evaluator::pool_or_canonical(pool) at lookup sites
    // to get the right pool with the right fallback.
    ast::StringPool* pool = nullptr;
    ast::NodeId body_id = ast::NULL_NODE;
    // P0 complete: legacy `env` pointer removed from Closure.
    // All captures now use only `env_id` (registered in env_frames_).
    // apply_closure and materialize always route through SoA
    // (env_frames_[env_id]). No more raw Env* for captured envs.
    // The temporary call-frame Env (ne) is still used for the
    // lambda scope bindings (with SoA parent_id for walks).
    // This eliminates the legacy captured-env pointer path.
    EnvId env_id = NULL_ENV_ID;
    bool dotted = false;
    ast::ASTArena* owner_arena = nullptr; // arena where flat/pool/env lives
    // Issue #223 / #1365: epoch captured at closure construction.
    // apply_closure compares against service bridge_epoch(); mismatch
    // means flat*/pool* may be stale. Default 0 = unstamped. All
    // construction sites must call Evaluator::stamp_closure_bridge_epoch
    // (Issue #1365). Unstamped + active tracking → is_bridge_stale true
    // unless AURA_BRIDGE_EPOCH_LEGACY_TRUST=1.
    std::uint64_t bridge_epoch = 0;
};

// Legacy alias — kept for backward compatibility during the
// P2 transition (Issue #127). New code should prefer
// `aura::diag::Result<types::EvalValue>`. Both names refer
// to the same type: `std::expected<types::EvalValue, Diagnostic>`.
export using EvalResult = aura::diag::Result<types::EvalValue>;

// Issue #478: unified primitive error construction for evaluator_primitives_*.
namespace primitives_detail {

    // Issue #751: defined in evaluator_eval_flat.cpp (after Evaluator is complete).
    void bump_prim_error_unified_total() noexcept;

    export inline types::EvalValue
    make_primitive_error(std::pmr::vector<std::string>& string_heap,
                         std::vector<types::EvalValue>& error_values, std::string_view msg,
                         std::atomic<std::uint64_t>* error_counter = nullptr) {
        auto sidx = string_heap.size();
        string_heap.emplace_back(msg);
        auto eidx = error_values.size();
        error_values.push_back(types::make_string(sidx));
        if (error_counter) {
            error_counter->fetch_add(1, std::memory_order_relaxed);
            bump_prim_error_unified_total();
        }
        return types::make_error(eidx);
    }

    // P2: single forward-decl hub for all primitives_detail::register_* TU entry points.
    void register_type_and_char_primitives(std::function<void(std::string, PrimFn)> add);
    void register_pair_and_string_primitives(std::function<void(std::string, PrimFn)> add,
                                             Evaluator& ev, std::pmr::vector<Pair>& pairs,
                                             std::pmr::vector<std::string>& string_heap,
                                             std::vector<EvalValue>& error_values,
                                             std::atomic<std::uint64_t>* primitive_error_counter);
    void register_json_primitives(std::function<void(std::string, PrimFn)> add,
                                  std::pmr::vector<Pair>& pairs,
                                  std::pmr::vector<std::string>& string_heap);
    void register_list_primitives(std::function<void(std::string, PrimFn)> add,
                                  std::pmr::vector<Pair>& pairs,
                                  std::pmr::vector<std::string>& string_heap,
                                  std::vector<EvalValue>& error_values, Evaluator& ev);
    void register_vector_and_hash_primitives(std::function<void(std::string, PrimFn)> add,
                                             std::pmr::vector<Pair>& pairs,
                                             std::pmr::vector<std::string>& string_heap,
                                             std::vector<EvalValue>& error_values,
                                             std::vector<std::vector<EvalValue>>& vector_heap,
                                             std::atomic<std::uint64_t>* primitive_error_counter);
    void register_math_regex_and_arithmetic_primitives(
        std::function<void(std::string, PrimFn)> add, std::pmr::vector<Pair>& pairs,
        std::pmr::vector<std::string>& string_heap, std::vector<EvalValue>& error_values,
        std::atomic<std::uint64_t>* primitive_error_counter, Evaluator& ev);
    void register_reflect_and_type_primitives(std::function<void(std::string, PrimFn)> add,
                                              std::pmr::vector<Pair>& pairs,
                                              std::pmr::vector<std::string>& string_heap,
                                              std::vector<std::string>& keyword_table,
                                              void*& type_registry);
    void register_query_primitives(
        std::function<void(std::string, PrimFn)> add, std::pmr::vector<Pair>& pairs,
        std::pmr::vector<std::string>& string_heap, void*& type_registry,
        std::function<std::string(const std::string&)> resolve_module_path, Evaluator& ev);
    void register_workspace_query_primitives(
        std::function<void(std::string, PrimFn)> add, std::shared_mutex& workspace_mtx,
        aura::ast::FlatAST*& workspace_flat, aura::ast::StringPool*& workspace_pool,
        void*& type_registry, std::vector<std::string>& keyword_table,
        std::pmr::vector<Pair>& pairs, std::pmr::vector<std::string>& string_heap,
        aura::ast::ASTArena*& temp_arena,
        std::unordered_map<std::uint64_t, std::vector<aura::ast::NodeId>>& tag_arity_index,
        // Issue #371: shared_mutex protecting `tag_arity_index`.
        // query:pattern fast path takes a shared_lock around the
        // `find` + bucket-iterate. Build/sync/invalidate helpers
        // take unique_lock internally; readers must drop their
        // shared_lock before triggering a build (see comment at
        // the query:pattern fast-path site for the trade-off).
        std::shared_mutex& tag_arity_index_mtx,
        std::function<aura::ast::StringPool*()> canonical_pool,
        std::function<void()> build_tag_arity_index,
        std::function<EvalValue(const std::string&, const std::string&)> mev, Evaluator& ev);
    void register_defuse_query_primitives(
        std::function<void(std::string, PrimFn)> add, std::shared_mutex& workspace_mtx,
        aura::ast::FlatAST*& workspace_flat, aura::ast::StringPool*& workspace_pool,
        std::pmr::vector<std::string>& string_heap, std::function<void*()> ensure_defuse,
        std::function<EvalValue(void* idx, aura::ast::SymId sym)> def_use_for_sym,
        std::function<EvalValue(void* idx, aura::ast::NodeId node)> reaches_for_node,
        std::function<EvalValue(void* idx, aura::ast::SymId sym)> effects_for_sym,
        std::function<EvalValue(void* idx)> build_index,
        std::function<EvalValue(void* idx)> index_stats,
        std::function<EvalValue(const std::string&, const std::string&)> make_merr);
    void
    register_mutate_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev,
                               std::function<EvalValue(const std::string&, const std::string&)> mev,
                               std::function<void()> destroy_defuse_index);
    void register_workspace_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev,
                                       std::function<void()> destroy_defuse_index);
    // Issue #1381: serialize-workspace / deserialize-workspace / workspace-persist-info
    void register_persist_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_ast_primitives(
        std::function<void(std::string, PrimFn)> add, Evaluator& ev,
        std::function<void()> destroy_defuse_index,
        std::function<std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>()>
            defuse_summary_stats);
    void register_compile_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_eval_observability_primitives(std::function<void(std::string, PrimFn)> add,
                                                Evaluator& ev);
    // Issues #923–#940: stdlib production-review dashboard primitives.
    void register_stdlib_review_primitives(std::function<void(std::string, PrimFn)> add,
                                           Evaluator& ev);
    void register_eda_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_verify_tool_primitives(
        std::function<void(std::string, std::function<aura::compiler::types::EvalValue(
                                            std::span<const aura::compiler::types::EvalValue>)>)>
            add,
        Evaluator& ev, std::function<aura::compiler::types::EvalValue(std::int32_t)> make_string,
        std::function<aura::compiler::types::EvalValue(std::int64_t)> make_int,
        std::function<aura::compiler::types::EvalValue(const std::string&, const std::string&)>
            mev);
    void register_jit_arena_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_messaging_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_git_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_network_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_tui_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_auto_evolve_primitives(std::function<void(std::string, PrimFn)> add,
                                         Evaluator& ev);
    void register_synthesize_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev,
                                        std::function<void()> destroy_defuse_index);
    void register_strategy_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_memory_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev,
                                    std::function<void()> destroy_defuse_index);
    void register_policy_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_security_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void
    register_eval_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev,
                             std::function<EvalValue(const std::string&, const std::string&)> mev,
                             std::function<void()> destroy_defuse_index);
    void register_type_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_hot_swap_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_diagnostic_primitives(std::function<void(std::string, PrimFn)> add,
                                        Evaluator& ev);
    void register_module_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_file_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_runtime_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_test_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_misc_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_control_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_char_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    void register_mutation_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);

    // Issue #909: friend structs holding peeled register parts (≤300 lines each).
#include "compiler/observability_prims_decl.inc"
#include "compiler/compile_prims_decl.inc"
} // namespace primitives_detail

void defuse_index_destroy(void** slot);

// Workspace layering (P13) — shared by workspace-tree TU + workspace primitives TU.
export struct WorkspaceNode {
    std::string name;
    ast::FlatAST* flat = nullptr;
    ast::StringPool* pool = nullptr;
    ast::FlatAST* parent_flat_ = nullptr;
    ast::StringPool* parent_pool_ = nullptr;
    // Issue #263: workspace-layer epoch. Synced to the owned flat's
    // generation_ after COW clone; 0 for shared (pre-COW) children.
    std::uint64_t generation = 0;
    // Issue #263: monotonic COW layer id assigned on lazy clone.
    std::uint64_t cow_epoch = 0;
    // Issue #276: direct parent workspace index + NodeId remap table.
    std::uint32_t parent_layer_idx = 0;
    ast::mutation::NodeIdRemapTable remap;
    bool read_only = false;
    bool has_own_flat = false;
    bool is_root = false;
    std::size_t memory_used = 0;
    std::size_t memory_budget = 0;
    std::uint64_t cow_refused_count = 0;
};

export struct WorkspaceTree {
    std::vector<WorkspaceNode> nodes_;
    std::uint32_t active_idx_ = 0;
    // Issue #263: global COW epoch counter (bumped on each lazy clone).
    std::uint64_t cow_epoch_ = 0;

    [[nodiscard]] std::size_t size() const { return nodes_.size(); }
    [[nodiscard]] std::uint32_t active_idx() const { return active_idx_; }
    WorkspaceNode* active() { return active_idx_ < nodes_.size() ? &nodes_[active_idx_] : nullptr; }

    bool ensure_local_flat(std::uint32_t idx);
    std::uint32_t create_child(const std::string& name, std::uint32_t parent_layer_idx,
                               ast::FlatAST* parent_flat, ast::StringPool* parent_pool);
    bool delete_child(std::uint32_t idx);
    bool set_active(std::uint32_t idx);
    void set_read_only(std::uint32_t idx, bool ro);
    [[nodiscard]] bool can_write(std::uint32_t idx);

    // Issue #276: remap a node id across workspace layers.
    [[nodiscard]] ast::NodeId remap_node_id(std::uint32_t from_layer, ast::NodeId id,
                                            std::uint32_t to_layer) const noexcept;
    [[nodiscard]] std::optional<ast::FlatAST::StableNodeRef>
    resolve_stable_ref(std::uint32_t from_layer, ast::FlatAST::StableNodeRef ref,
                       std::uint32_t to_layer) const noexcept;
};

export class Evaluator {
    friend void primitives_detail::register_mutate_primitives(
        std::function<void(std::string, PrimFn)> add, Evaluator& ev,
        std::function<EvalValue(const std::string&, const std::string&)> mev,
        std::function<void()> destroy_defuse_index);
    friend void
    primitives_detail::register_workspace_primitives(std::function<void(std::string, PrimFn)> add,
                                                     Evaluator& ev,
                                                     std::function<void()> destroy_defuse_index);
    friend void
    primitives_detail::register_persist_primitives(std::function<void(std::string, PrimFn)> add,
                                                   Evaluator& ev);
    friend void primitives_detail::register_ast_primitives(
        std::function<void(std::string, PrimFn)> add, Evaluator& ev,
        std::function<void()> destroy_defuse_index,
        std::function<std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>()>
            defuse_summary_stats);
    friend void
    primitives_detail::register_compile_primitives(std::function<void(std::string, PrimFn)> add,
                                                   Evaluator& ev);
    friend void primitives_detail::register_eval_observability_primitives(
        std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    friend void primitives_detail::register_stdlib_review_primitives(
        std::function<void(std::string, PrimFn)> add, Evaluator& ev);
    // Issue #909: peeled register structs access private Evaluator state.
    friend struct primitives_detail::ObservabilityPrims;
    friend struct primitives_detail::CompilePrims;
    friend void
    primitives_detail::register_eda_primitives(std::function<void(std::string, PrimFn)> add,
                                               Evaluator& ev);
    friend void primitives_detail::register_verify_tool_primitives(
        std::function<void(std::string, std::function<aura::compiler::types::EvalValue(
                                            std::span<const aura::compiler::types::EvalValue>)>)>
            add,
        Evaluator& ev, std::function<aura::compiler::types::EvalValue(std::int32_t)> make_string,
        std::function<aura::compiler::types::EvalValue(std::int64_t)> make_int,
        std::function<aura::compiler::types::EvalValue(const std::string&, const std::string&)>
            mev);
    friend void
    primitives_detail::register_jit_arena_primitives(std::function<void(std::string, PrimFn)> add,
                                                     Evaluator& ev);
    friend void
    primitives_detail::register_messaging_primitives(std::function<void(std::string, PrimFn)> add,
                                                     Evaluator& ev);
    friend void
    primitives_detail::register_git_primitives(std::function<void(std::string, PrimFn)> add,
                                               Evaluator& ev);
    friend void
    primitives_detail::register_network_primitives(std::function<void(std::string, PrimFn)> add,
                                                   Evaluator& ev);
    friend void
    primitives_detail::register_tui_primitives(std::function<void(std::string, PrimFn)> add,
                                               Evaluator& ev);
    friend void
    primitives_detail::register_auto_evolve_primitives(std::function<void(std::string, PrimFn)> add,
                                                       Evaluator& ev);
    friend void
    primitives_detail::register_synthesize_primitives(std::function<void(std::string, PrimFn)> add,
                                                      Evaluator& ev,
                                                      std::function<void()> destroy_defuse_index);
    friend void
    primitives_detail::register_strategy_primitives(std::function<void(std::string, PrimFn)> add,
                                                    Evaluator& ev);
    friend void
    primitives_detail::register_memory_primitives(std::function<void(std::string, PrimFn)> add,
                                                  Evaluator& ev,
                                                  std::function<void()> destroy_defuse_index);
    friend void
    primitives_detail::register_policy_primitives(std::function<void(std::string, PrimFn)> add,
                                                  Evaluator& ev);
    friend void
    primitives_detail::register_security_primitives(std::function<void(std::string, PrimFn)> add,
                                                    Evaluator& ev);
    friend void primitives_detail::register_eval_primitives(
        std::function<void(std::string, PrimFn)> add, Evaluator& ev,
        std::function<EvalValue(const std::string&, const std::string&)> mev,
        std::function<void()> destroy_defuse_index);
    friend void
    primitives_detail::register_type_primitives(std::function<void(std::string, PrimFn)> add,
                                                Evaluator& ev);
    friend void
    primitives_detail::register_hot_swap_primitives(std::function<void(std::string, PrimFn)> add,
                                                    Evaluator& ev);
    friend void
    primitives_detail::register_diagnostic_primitives(std::function<void(std::string, PrimFn)> add,
                                                      Evaluator& ev);
    friend void
    primitives_detail::register_module_primitives(std::function<void(std::string, PrimFn)> add,
                                                  Evaluator& ev);
    friend void
    primitives_detail::register_file_primitives(std::function<void(std::string, PrimFn)> add,
                                                Evaluator& ev);
    friend void
    primitives_detail::register_runtime_primitives(std::function<void(std::string, PrimFn)> add,
                                                   Evaluator& ev);
    friend void
    primitives_detail::register_test_primitives(std::function<void(std::string, PrimFn)> add,
                                                Evaluator& ev);
    friend void
    primitives_detail::register_misc_primitives(std::function<void(std::string, PrimFn)> add,
                                                Evaluator& ev);
    friend void
    primitives_detail::register_control_primitives(std::function<void(std::string, PrimFn)> add,
                                                   Evaluator& ev);
    friend void
    primitives_detail::register_char_primitives(std::function<void(std::string, PrimFn)> add,
                                                Evaluator& ev);
    friend void
    primitives_detail::register_mutation_primitives(std::function<void(std::string, PrimFn)> add,
                                                    Evaluator& ev);
    friend void primitives_detail::register_list_primitives(
        std::function<void(std::string, PrimFn)> add, std::pmr::vector<Pair>& pairs,
        std::pmr::vector<std::string>& string_heap, std::vector<EvalValue>& error_values,
        Evaluator& ev);

public:
    Evaluator();
    // Issue #67: destructor walks modules_ and runs each env's
    // destructor to free their std::vector bindings_ heap allocations.
    // Without this, arena-allocated Envs leak at process exit (the
    // arena's bump-allocator doesn't run destructors).
    ~Evaluator();
    // Issue #1746: stable identity for thread-local maps (depth slot).
    // Never recycled; survives free-list address reuse so Guard depth
    // cannot alias a destroyed Evaluator's TLS entry.
    [[nodiscard]] std::uint64_t instance_id() const noexcept { return instance_id_; }
    // Issue #1546 / #1554: C-style allow_fn for ASTArena / ArenaGroup owners.
    // Shared by set_arena, set_temp_arena, and arena_group default owner.
    static bool arena_quota_allow(void* owner, std::size_t size) noexcept {
        auto* ev = static_cast<Evaluator*>(owner);
        // check_arena_quota returns nullopt when allowed.
        return !ev->check_arena_quota(static_cast<std::uint64_t>(size)).has_value();
    }

    void set_arena(ast::ASTArena* a) {
        // Issue #1663: serialize set_arena transitions (rare path).
        // ASTArena::owner_mtx_ separately atomicizes owner+fn vs allocate_raw.
        std::lock_guard<std::mutex> lock(arena_set_mtx_);
        // Issue #1546: drop owner link from previous arena if any.
        // Issue #1662: also clear compact hook (captures `this`).
        const bool switching = (arena_ != a);
        if (arena_ && switching) {
            arena_->clear_arena_owner();
            arena_->set_on_compact_hook({});
        }
        arena_ = a;
        // Issue #1446 follow-up: register compact hook so GC-driven
        // compaction re-pins pinned StableNodeRef / COW children in
        // the active Guard stack via on_arena_compact_hook().
        //
        // Issue #1666: set_on_compact_hook **replaces** (does not append).
        // On first claim of an arena (switching or empty hook), take any
        // prior listener and chain: prior first, then re_pin — so external
        // hooks are not silently dropped. Idempotent set_arena(same):
        // leave an already-installed hook alone (avoids double re_pin wrap).
        // Multi-listener installs *after* set_arena must take+chain
        // (CompilerService ShapeProfiler does this).
        if (arena_) {
            if (switching || !arena_->has_on_compact_hook()) {
                auto prior = arena_->take_on_compact_hook();
                arena_->set_on_compact_hook([this, prior = std::move(prior)]() {
                    if (prior)
                        prior();
                    this->on_arena_compact_hook();
                });
            }
            // Issue #1546 / #1554: thread this Evaluator as arena_owner_ so
            // ASTArena::allocate_raw consults check_arena_quota before
            // every allocation (orphan arenas stay unlimited).
            // Issue #1662: ~Evaluator MUST clear_arena_owner + clear hook.
            // Issue #1663: set_arena_owner is atomic (owner_mtx_) vs allocate.
            arena_->set_arena_owner(this, &Evaluator::arena_quota_allow);
        }
        // Issue #1554: also bind ArenaGroup module arenas to the same quota.
        if (arena_group_)
            arena_group_->set_default_arena_owner(this, &Evaluator::arena_quota_allow);
    }
    void set_temp_arena(ast::ASTArena* a) {
        // Issue #1554: wire temp_arena the same way as primary arena so
        // task-context / gc-temp allocations honor ResourceQuota.
        // Issue #1662 / #1663: clear previous owner under arena_set_mtx_.
        std::lock_guard<std::mutex> lock(arena_set_mtx_);
        if (temp_arena_ && temp_arena_ != a)
            temp_arena_->clear_arena_owner();
        temp_arena_ = a;
        if (temp_arena_)
            temp_arena_->set_arena_owner(this, &Evaluator::arena_quota_allow);
    }
    // Hot-swap callback (Issue #97 Action 1). Set by CompilerService
    // to enable the (hot-swap:fn "name" "new-source") primitive.
    // Returns true on success.
    using HotSwapFn = std::function<bool(const std::string& name, const std::string& new_source)>;
    void set_hot_swap_fn(HotSwapFn fn) { hot_swap_fn_ = std::move(fn); }
    // Per-module arena group: load_module_file allocates each module's
    // StringPool/FlatAST/mod_env in a dedicated arena so the whole module
    // can be freed in one shot via reset_module(path).
    ast::ArenaGroup& arena_group() { return *arena_group_; }
    const ast::ArenaGroup& arena_group() const { return *arena_group_; }
    // Free a module's arena + all closures it owns. Returns false if
    // the module is not in the cache.
    bool gc_module(const std::string& path);
    // Set current FlatAST/Pool for mutation primitives
    void set_flat_pool(ast::FlatAST* f, ast::StringPool* p) {
        mutate_target_flat_ = f;
        mutate_target_pool_ = p;
    }
    [[nodiscard]] EvalResult eval_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                       aura::ast::NodeId id, const Env& env);
    const Primitives& primitives() const { return primitives_; }
    Primitives& primitives() { return primitives_; }
    const Env& top_env() const { return top_; }
    Env& top_env() { return top_; }
    // Issue #128: span-friendly read-only view of cells.
    // Replaces `const std::vector<types::EvalValue>&` with
    // `std::span<const types::EvalValue>` for zero-overhead
    // call sites that only iterate the cells. Callers that
    // need to mutate should use the non-const `cells()`.
    // Issue #145 Phase 2.4: cells_ is std::pmr::vector backed
    // by runtime_resource_ (monotonic arena). Pmr API is a
    // drop-in for std::vector at the call site; iteration /
    // size / push_back / clear all unchanged.
    std::span<const types::EvalValue> cells() const { return cells_; }
    std::pmr::vector<types::EvalValue>& cells() { return cells_; }
    std::span<const Pair> pairs() const { return pairs_; }
    std::pmr::vector<Pair>& pairs() { return pairs_; }
    std::span<const std::string> keyword_table() const { return keyword_table_; }
    std::vector<std::string>& keyword_table() { return keyword_table_; }

    // Issue #135: read-only access to the string heap for tests
    // that need to verify string-valued results (e.g. comparing
    // (current-source :workspace) output against expected text).
    // The heap is mutated only under the workspace_mtx_ shared
    // lock; this getter returns a const view that doesn't take
    // the lock — callers must ensure no concurrent mutation
    // (the test harness runs single-threaded).
    std::span<const std::string> string_heap() const { return string_heap_; }
    // Phase 2.4: mut accessor returns pmr::vector reference
    // (matches the underlying storage type).
    std::pmr::vector<std::string>& string_heap_mut() { return string_heap_; }

    // IR closure bridge: called when a closure id is not in closures_.
    using ClosureBridgeFn = std::function<std::optional<EvalValue>(
        ClosureId closure_id, std::span<const EvalValue> args)>;

    // Set the IR closure bridge for cross-evaluator closure calls
    void set_closure_bridge(ClosureBridgeFn bridge) { closure_bridge_ = std::move(bridge); }

    // Look up a closure and apply it with given args.
    // Tries closures_ first, then IR bridge.
    std::optional<EvalValue> apply_closure(ClosureId cid, std::span<const EvalValue> args);
    std::optional<EvalValue> apply_closure(ClosureId cid, std::initializer_list<EvalValue> args) {
        return apply_closure(cid, std::span<const EvalValue>(args.begin(), args.size()));
    }

    // Module loaded callback: called after a module file is successfully loaded.
    using ModuleLoadedFn = std::function<void(const std::string& source, const std::string& path)>;

    void set_module_loaded_callback(ModuleLoadedFn cb) { module_loaded_cb_ = std::move(cb); }
    void set_type_registry(void* reg); // Issue #911: defined in evaluator_ctor.cpp
    // Issue #911/#912: ensure owned TypeRegistry (returns opaque void*; cast at
    // use sites in TUs that import aura.core.type — TypeRegistry is incomplete
    // in this module interface by design to avoid cross-module BMI cycles).
    void* ensure_type_registry();
    [[nodiscard]] void* type_registry_ptr() noexcept { return type_registry_; }
    [[nodiscard]] const void* type_registry_ptr() const noexcept { return type_registry_; }
    // Issue #912: opaque handle remains void*; typed cast at call sites with core import.
    // Issue #252: closure dual-path observability. Pass a
    // CompilerMetrics* (or nullptr to disable). The Evaluator's
    // apply_closure increments the closure_* counters on the
    // metrics struct. The IR's IROpcode::Call/Apply also
    // writes to the same metrics struct (via IRContext).
    // Both paths use the same single source of truth.
    // Issue #1368: also auto-wire AOT bridge metrics so bare Evaluator
    // (no CompilerService) does not silently disable hot-update counters.
    void set_compiler_metrics(void* m) {
        compiler_metrics_ = m;
        if (m)
            aura_set_aot_metrics(static_cast<CompilerMetrics*>(m));
    }
    [[nodiscard]] void* compiler_metrics() const noexcept { return compiler_metrics_; }
    void set_compiler_service(void* svc) { compiler_service_ = svc; }
    // Issue #612: optional post-mutate ADT registry sync (wired by CompilerService).
    using WorkspaceAdtSyncFn = void (*)(void* compiler_service);
    void set_workspace_adt_sync_fn(WorkspaceAdtSyncFn fn) { workspace_adt_sync_fn_ = fn; }
    // Issue #612: post-mutate hook — re-sync TypeRegistry ADT ctors
    // from workspace DefineType nodes (delegates to CompilerService).
    void sync_workspace_adt_registry();
    // Issue #426: public getter for compiler_service_ (as
    // void*). The (query:compiler-cache-stats) primitive
    // uses it to call CompilerService::dirty_block_count().
    [[nodiscard]] void* compiler_service() const noexcept { return compiler_service_; }
    // Issue #223: returns the current bridge_epoch from the
    // service (or 0 if no service is bound). Closure-construction
    // sites capture this at construction time; apply_closure
    // compares against it to detect stale closures (arena was
    // reset or major mutation invalidated the captured flat*/pool*).
    //
    // The bridge_epoch() call goes through a function pointer
    // (bridge_epoch_fn_) to avoid a circular include with
    // service.ixx. CompilerService::install_bridge_epoch_fn()
    // sets the function pointer when binding to the Evaluator.
    using BridgeEpochFn = std::uint64_t (*)(void*);
    [[nodiscard]] std::uint64_t current_bridge_epoch() const noexcept;
    void install_bridge_epoch_fn(BridgeEpochFn fn) noexcept { bridge_epoch_fn_ = fn; }
    // Issue #1510: compact_env_frames must bump bridge_epoch so all
    // live closures observe is_bridge_stale after env_id rewrite.
    // Wired by CompilerService to bump_bridge_epoch() (same domain as
    // mutation_epoch_). No-op when unbound (tests without service).
    using BridgeEpochBumpFn = void (*)(void*);
    void install_bridge_epoch_bump_fn(BridgeEpochBumpFn fn) noexcept { bridge_epoch_bump_fn_ = fn; }
    // Issue #1510: optional IR / external remapper for compact remap
    // table (runtime_closures_ IRClosure::env_id when #1507 lands).
    // Called under compact_env_frames_lock_ after Closure rewrite
    // (before dual-epoch bump).
    using CompactEnvRemapFn = void (*)(void* ctx, const std::int64_t* remap, std::size_t n);
    void install_compact_env_remap_fn(CompactEnvRemapFn fn, void* ctx) noexcept {
        compact_env_remap_fn_ = fn;
        compact_env_remap_ctx_ = ctx;
    }
    // Issue #1526: after dual-epoch bump, restamp IRClosure::bridge_epoch
    // to post-compact epoch. Returns number of restamps.
    using CompactEnvRestampFn = std::size_t (*)(void* ctx, std::uint64_t new_bridge_epoch);
    void install_compact_env_restamp_fn(CompactEnvRestampFn fn) noexcept {
        compact_env_restamp_fn_ = fn;
    }
    // Issue #1526: public accessor for compact bridge restamp metric.
    [[nodiscard]] std::uint64_t get_envframe_compact_bridge_restamps() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->envframe_compact_bridge_restamps_total.load(std::memory_order_relaxed);
        return 0;
    }
    // Issue #1904: mutation guard coverage accessors. Read-only on
    // CompilerMetrics counters; the bump sites live inside
    // MutationBoundaryGuard ctor (wrapped) and the legacy sites (manual).
    [[nodiscard]] std::uint64_t get_mutation_boundary_primitives_wrapped() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->mutation_boundary_primitives_wrapped.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_mutation_legacy_manual_lock_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->mutation_legacy_manual_lock_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_mutation_guard_try_acquire_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_mutation_guard_try_acquire_reject_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->mutation_guard_try_acquire_reject_total.load(std::memory_order_relaxed);
        return 0;
    }
    // Issue #1907: reflect/EDSL bridge accessors backing the
    // (engine:metrics "query:reflect-schema") + (mutate:validate-reflected)
    // primitives + the post-mutation auto_validate + hygiene gate hook
    // (aura_validate_reflected_post_mutation, called from
    // flush_mutation_boundary outermost exit per Step 1 of the #1907 plan).
    // Read-only on CompilerMetrics counters; bump sites live in
    // aura_jit_bridge.cpp (post-mutation bridge hook) and in
    // evaluator_primitives_query.cpp (primitive impls).
    [[nodiscard]] std::uint64_t get_reflect_post_mutation_validate_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->reflect_post_mutation_validate_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_reflect_post_mutation_validate_fail_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->reflect_post_mutation_validate_fail_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_reflect_hygiene_macro_reject_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->reflect_hygiene_macro_reject_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_reflect_schema_query_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->reflect_schema_query_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_reflect_validate_reflected_query_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->reflect_validate_reflected_query_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_reflect_dirty_macro_nodes_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->reflect_dirty_macro_nodes_total.load(std::memory_order_relaxed);
        return 0;
    }
    void bump_reflect_post_mutation_validate_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflect_post_mutation_validate_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_reflect_post_mutation_validate_fail_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflect_post_mutation_validate_fail_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_reflect_hygiene_macro_reject_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflect_hygiene_macro_reject_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_reflect_schema_query_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflect_schema_query_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_reflect_validate_reflected_query_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflect_validate_reflected_query_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_reflect_dirty_macro_nodes_total(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflect_dirty_macro_nodes_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #1905: AOT incremental hot-update / invalidation loop
    // closure (#1046). 6 new counters backing the
    // (engine:metrics "query:aot-hot-update-stats") primitive.
    // Bump sites live in aura_jit_bridge.cpp (aura_refresh_live_closures_for_mutated_define
    // + flush_mutation_boundary outermost path) and in
    // complete_post_resume_steal_refresh (post-steal path).
    [[nodiscard]] std::uint64_t get_aot_live_closure_refresh_on_mutation_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->aot_live_closure_refresh_on_mutation_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_aot_live_closure_refresh_on_steal_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->aot_live_closure_refresh_on_steal_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_aot_bridge_epoch_bump_on_mutation_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->aot_bridge_epoch_bump_on_mutation_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_aot_bridge_epoch_bump_on_steal_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->aot_bridge_epoch_bump_on_steal_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_aot_region_mismatch_on_resume_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->aot_region_mismatch_on_resume_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_aot_stale_deopt_on_steal_total() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            return m->aot_stale_deopt_on_steal_total.load(std::memory_order_relaxed);
        return 0;
    }
    void bump_aot_live_closure_refresh_on_mutation_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_live_closure_refresh_on_mutation_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_aot_live_closure_refresh_on_steal_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_live_closure_refresh_on_steal_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_aot_bridge_epoch_bump_on_mutation_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_bridge_epoch_bump_on_mutation_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_aot_bridge_epoch_bump_on_steal_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_bridge_epoch_bump_on_steal_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_aot_region_mismatch_on_resume_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_region_mismatch_on_resume_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_aot_stale_deopt_on_steal_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_stale_deopt_on_steal_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1637: panic checkpoint lifecycle hardening — bump/getter
    // pair for the three path-specific restore counters (post-steal /
    // post-compact / post-hot-swap) plus the two outcome counters
    // (cross_fiber_panic_heal_success + mutation_boundary_steal_safe_total).
    // Bumped from restore_panic_checkpoint_on_<event>_if_needed() in
    // evaluator_workspace_tree.cpp; observable via
    // (query:mutation-boundary-coverage-stats) and the per-path
    // C accessors in aura_jit_bridge.cpp (file-scope atomic fallback).
    void bump_post_steal_checkpoint_restore_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->post_steal_checkpoint_restore_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_post_compact_checkpoint_restore_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->post_compact_checkpoint_restore_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_post_hot_swap_checkpoint_restore_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->post_hot_swap_checkpoint_restore_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_cross_fiber_panic_heal_success() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->cross_fiber_panic_heal_success.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_mutation_boundary_steal_safe_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutation_boundary_steal_safe_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1638: SoA EnvFrame dual-path consistency + mutation_log
    // compact. bump_/getter pair for the three new metrics covering
    // the three concrete gaps (lookup/walk GC/JIT dual-path stale
    // fallback, mutation_log compact bytes saved, env_frame version
    // drift prevented). Observable via (query:mutation-boundary-
    // coverage-stats) primitive (no new primitive added — surface
    // held within 521 budget per #1734 raise) and the file-scope
    // atomic fallback in aura_jit_bridge.cpp (mirror #1908 dual-write
    // pattern).
    void bump_dual_path_stale_fallback_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dual_path_stale_fallback_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_mutation_log_compact_bytes_saved(std::uint64_t bytes) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutation_log_compact_bytes_saved.fetch_add(bytes, std::memory_order_relaxed);
        }
    }
    void bump_env_frame_version_drift_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->env_frame_version_drift_prevented.fetch_add(1, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_dual_path_stale_fallback_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->dual_path_stale_fallback_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_mutation_log_compact_bytes_saved() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->mutation_log_compact_bytes_saved.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_env_frame_version_drift_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->env_frame_version_drift_prevented.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #1639: per-block dirty bitmask → partial re-lower wiring
    // (refine #1474 / #1495 / #1505 / #1514 / #1555 / #1601 / #1605).
    // bump_/getter pair for the 5 new metrics completing the spec's
    // observability surface (full_relower_count +
    // dirty_block_ratio_{numerator,denominator}_total +
    // relower_block_hit_rate_{numerator,denominator}_total).
    // Bumped from service.ixx::relower_define_blocks at the relevant
    // decision points (per-call dirty_block_ratio + hit_rate sums
    // so dashboards can compute the rolling averages). Observable
    // via (query:incremental-relower-stats) primitive (no new
    // primitive — surface held within 521 budget per #1734 raise).
    void bump_full_relower_count() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->full_relower_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_dirty_block_ratio(std::uint64_t numerator, std::uint64_t denominator) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dirty_block_ratio_numerator_total.fetch_add(numerator, std::memory_order_relaxed);
            m->dirty_block_ratio_denominator_total.fetch_add(denominator,
                                                             std::memory_order_relaxed);
        }
    }
    void bump_relower_block_hit_rate(std::uint64_t numerator,
                                     std::uint64_t denominator) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->relower_block_hit_rate_numerator_total.fetch_add(numerator,
                                                                std::memory_order_relaxed);
            m->relower_block_hit_rate_denominator_total.fetch_add(denominator,
                                                                  std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_full_relower_count() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->full_relower_count.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_dirty_block_ratio_numerator_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->dirty_block_ratio_numerator_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_dirty_block_ratio_denominator_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->dirty_block_ratio_denominator_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_relower_block_hit_rate_numerator_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->relower_block_hit_rate_numerator_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_relower_block_hit_rate_denominator_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->relower_block_hit_rate_denominator_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #1640: AOT bridge mangle versioning + region filtering +
    // aot_emit_version stale 检测 + incremental re-emit +
    // hot-update observability. bump_/getter pair for the 2 new
    // metrics (aot_env_frame_version_drift_prevented +
    // aot_incremental_reemit_triggered). Bumped from
    // aura_jit_bridge.cpp::aura_reload_aot_module_for_eval at the
    // new env_frame_version drift check site + at the new
    // incremental re-emit hook call. Observable via existing
    // aot_metrics accessor (file-scope atomic in aura_jit_bridge.cpp
    // for module-unaware consumers — same dual-write pattern as
    // #1908 / #1637).
    void bump_aot_env_frame_version_drift_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_env_frame_version_drift_prevented.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_aot_incremental_reemit_triggered() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_incremental_reemit_triggered.fetch_add(1, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_aot_env_frame_version_drift_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->aot_env_frame_version_drift_prevented.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_aot_incremental_reemit_triggered() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->aot_incremental_reemit_triggered.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #1641: Scheduler/Worker work-stealing awareness for
    // YieldReason::MutationBoundary + per-fiber mutation stack /
    // checkpoint transfer. bump_/getter pair for the 3 new metrics
    // covering the 3 spec gaps (defer + mitigation + safe-steal).
    // Bumped from serve/worker.cpp + serve/scheduler.cpp at the
    // steal decision points (the bump_ helpers are exposed as
    // public Evaluator methods because Worker/Scheduler operate
    // on the Evaluator's per-CompilerMetrics observability surface;
    // the legacy per-Fiber counters like static_cross_fiber_
    // mutation_safe_steal_total continue to bump independently at
    // the Fiber level).
    void bump_steal_mutation_boundary_deferred_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->steal_mutation_boundary_deferred_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_starvation_mitigated_for_boundary_count() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->starvation_mitigated_for_boundary_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_boundary_held_steal_safe_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->boundary_held_steal_safe_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_steal_mutation_boundary_deferred_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->steal_mutation_boundary_deferred_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_starvation_mitigated_for_boundary_count() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->starvation_mitigated_for_boundary_count.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_boundary_held_steal_safe_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->boundary_held_steal_safe_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_post_steal_checkpoint_restore_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->post_steal_checkpoint_restore_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_post_compact_checkpoint_restore_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->post_compact_checkpoint_restore_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_post_hot_swap_checkpoint_restore_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->post_hot_swap_checkpoint_restore_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_cross_fiber_panic_heal_success() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->cross_fiber_panic_heal_success.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_mutation_boundary_steal_safe_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->mutation_boundary_steal_safe_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #682: compiler GC root flush hook (service.ixx
    // implements flush_compiler_gc_roots). Called from
    // flush_gc_roots at safepoint after runtime heap roots.
    using CompilerGcRootsFlushFn = void (*)(void* service, void* root_set);
    void install_compiler_gc_roots_fn(CompilerGcRootsFlushFn fn) noexcept {
        compiler_gc_roots_fn_ = fn;
    }
    // Issue #223 / #296 / #1365: returns true if a closure's captured
    // bridge_epoch is stale relative to the current epoch.
    //
    // INVARIANT (Bridge Lifetime Contract, strict as of #1365):
    //   1. current_epoch == 0 → bridge tracking inactive (pre-service
    //      or never bumped): never treat as stale.
    //   2. bridge_epoch == 0 with current_epoch != 0 → unstamped
    //      while tracking is active → STALE (was legacy trust;
    //      AURA_BRIDGE_EPOCH_LEGACY_TRUST=1 restores trust).
    //   3. Non-zero mismatch → stale (safe-fallback / re-parse).
    //   4. Construction sites must call stamp_closure_bridge_epoch.
    //
    // State machine:
    //   fresh closure ─stamp─> bridge_epoch = current
    //   bump_bridge_epoch() ─bumps─> current
    //   is_bridge_stale(captured, current) ─checks─> bool
    //   stale closure ─falls back─> body_source re-parse
    static bool is_bridge_stale(std::uint64_t bridge_epoch, std::uint64_t current_epoch) noexcept {
        if (current_epoch == 0)
            return false; // tracking inactive
        if (bridge_epoch == 0) {
            // Issue #1365: strict by default; legacy opt-in for fixtures
            static const bool legacy_trust = []() noexcept {
                if (const char* e = std::getenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST"))
                    return e[0] != '0' && e[0] != '\0';
                return false;
            }();
            return !legacy_trust;
        }
        return bridge_epoch != current_epoch;
    }

    // Issue #1475: dual-check complement to is_bridge_stale. Even
    // when bridge_epoch matches, an EnvFrame captured pre-mutation
    // (frame.version_ < current defuse_version_) may have stale
    // bindings — the mutation that bumped defuse_version_ may have
    // overwritten a cell that the closure captured. Without this
    // second check, a long-lived closure that survives a
    // mutate:set-body could read post-mutation cell values against
    // pre-mutation captured bindings, producing semantically wrong
    // results (or, in the linear-ownership case, UAF on freed
    // binding memory).
    //
    // INVARIANT (EnvFrame Version Contract, parallel to #1365):
    //   1. current_defuse_version == 0 → tracking inactive
    //      (pre-#242 era): never treat as stale.
    //   2. env_id == NULL_ENV_ID → closure doesn't carry an EnvFrame
    //      reference (legacy / pre-SoA closures): never stale here
    //      (the bridge check still applies).
    //   3. frame not found by resolve_env_frame → env_id is post-
    //      truncate stale (Issue #1360): conservative stale.
    //   4. frame.version_ == 0 with current != 0 → unversioned
    //      while tracking is active → STALE (strict by default;
    //      AURA_BRIDGE_EPOCH_LEGACY_TRUST=1 re-enables legacy trust).
    //   5. Non-zero mismatch → stale (frame captured pre-mutation).
    //
    // Per-frame version_ is set on frame allocation (Issue #612):
    // `frame.version_ = defuse_version_.load(acquire)` at
    // evaluation-time capture, and `bump_defuse_version_for_test`
    // (or real mutation paths) bump it post-commit. After
    // mutation, frame.version_ < current means the captured
    // bindings are stale.
    static bool is_env_frame_stale(EnvId env_id, std::uint64_t frame_version,
                                   std::uint64_t current_defuse_version) noexcept {
        if (current_defuse_version == 0)
            return false; // tracking inactive
        if (env_id == NULL_ENV_ID) {
            // Issue #1475: closure without env_id is not subject to
            // this check (legacy / pre-SoA paths). The bridge check
            // still applies for those.
            return false;
        }
        if (frame_version == 0) {
            // Unversioned frame captured pre-#242 — strict by default
            // (legacy opt-in via AURA_BRIDGE_EPOCH_LEGACY_TRUST, same
            // env var as is_bridge_stale so a single env override
            // covers both domains).
            static const bool legacy_trust = []() noexcept {
                if (const char* e = std::getenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST"))
                    return e[0] != '0' && e[0] != '\0';
                return false;
            }();
            return !legacy_trust;
        }
        return frame_version < current_defuse_version;
    }

    // Issue #1365: stamp construction-site epoch (call on every new Closure).
    void stamp_closure_bridge_epoch(Closure& cl) const noexcept {
        cl.bridge_epoch = current_bridge_epoch();
    }

    // Issue #1660: single source-of-truth for apply_closure dual paths
    // (map + bridge), materialize_call_env, and JIT deopt gates.
    // True when bridge_epoch is stale OR EnvFrame is invalid/stale
    // (version_ / parent SoA). Does **not** include linear-only
    // ownership (use linear_post_mutate_enforce for that third arm).
    // Metrics distinguish epoch-stale vs env-stale at the call site.
    [[nodiscard]] bool closure_is_epoch_or_env_stale(const Closure& cl) const noexcept {
        if (is_bridge_stale(cl.bridge_epoch, current_bridge_epoch()))
            return true;
        if (cl.env_id != NULL_ENV_ID) {
            if (is_env_frame_invalid(cl.env_id) || is_env_frame_stale(cl.env_id))
                return true;
        }
        return false;
    }
    void set_session_id(const std::string& id) { session_id_ = id; }
    // Phase 2: EDSL IR cache V2 hooks (set by CompilerService on init)
    void set_mark_define_dirty_fn(std::function<void(const std::string&)> fn) {
        mark_define_dirty_fn_ = std::move(fn);
    }
    void set_mark_all_defines_dirty_fn(std::function<void()> fn) {
        mark_all_defines_dirty_fn_ = std::move(fn);
    }
    // Issue #680: full invalidate_function BFS for closure-heavy Define
    // mutations (mutate:rebind / mutate:query-and-replace success path).
    void set_invalidate_function_fn(std::function<void(const std::string&)> fn) {
        invalidate_function_fn_ = std::move(fn);
    }
    // Issue #680: ir_cache_pure impact_scope analysis on Define dirty.
    void set_define_impact_scope_fn(std::function<void(aura::ast::NodeId)> fn) {
        define_impact_scope_fn_ = std::move(fn);
    }
    [[nodiscard]] std::uint64_t precise_define_inval_hits() const noexcept {
        return precise_define_inval_hits_.load(std::memory_order_relaxed);
    }
    void bump_precise_define_inval_hit() noexcept {
        precise_define_inval_hits_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #680: post-mutate Define IR/JIT/bridge invalidation hook
    // (called from mutate:rebind / mutate:query-and-replace success).
    void finalize_define_mutate_invalidation(const aura::ast::FlatAST& flat,
                                             const std::string& name, aura::ast::NodeId define_id,
                                             bool run_full_invalidate = true);
    // Issue #262: precise def-use dirty propagation. Marks entry
    // nodes + ancestors with kDefUseDirty, records the sym for
    // incremental DefUseIndex refresh, and touches per-sym staleness.
    void propagate_defuse_dirty(aura::ast::SymId sym, const std::string& sym_name,
                                std::span<const aura::ast::NodeId> entry_nodes,
                                std::uint8_t reasons = aura::ast::FlatAST::kGeneralDirty) {
        defuse_affected_syms_.insert(sym_name);
        if (defuse_touch_fn_)
            defuse_touch_fn_(defuse_index_, sym);
        if (workspace_flat_ && !entry_nodes.empty())
            workspace_flat_->mark_dirty_defuse_entries(entry_nodes, reasons);
    }
    // After (set-code ...) parses a new workspace, walk its top-level defines
    // and pre-populate the v2 cache. Source-hash based, so unchanged defines
    // hit the cache and skip the lowering work.
    std::function<void()> pre_cache_workspace_defines_fn_ = nullptr;
    void set_pre_cache_workspace_defines_fn(std::function<void()> fn) {
        pre_cache_workspace_defines_fn_ = std::move(fn);
    }
    // Issue #63723: lightweight dep_graph-only repopulate hook
    // (called from mutate:rebind after the rebind success). See
    // set_repopulate_workspace_dep_graph_fn in service.ixx for
    // the rationale.
    std::function<void()> repopulate_workspace_dep_graph_fn_ = nullptr;
    void set_repopulate_workspace_dep_graph_fn(std::function<void()> fn) {
        repopulate_workspace_dep_graph_fn_ = std::move(fn);
    }
    // Issue #1495: prefer partial re-lower of dirty ir_cache_v2_
    // defines before tree-walker eval-current (AI set-body hot path).
    std::function<void()> relower_dirty_defines_fn_ = nullptr;
    void set_relower_dirty_defines_fn(std::function<void()> fn) {
        relower_dirty_defines_fn_ = std::move(fn);
    }
    // Phase 3: read cache entry from outside the module.
    using IsDefineDirtyFn = bool(const std::string&);
    std::function<IsDefineDirtyFn> is_define_dirty_fn_ = nullptr;
    void set_is_define_dirty_fn(std::function<IsDefineDirtyFn> fn) {
        is_define_dirty_fn_ = std::move(fn);
    }
    using GetDependentsFn = std::vector<std::string>(const std::string&);
    std::function<GetDependentsFn> get_dependents_fn_ = nullptr;
    void set_get_dependents_fn(std::function<GetDependentsFn> fn) {
        get_dependents_fn_ = std::move(fn);
    }
    // Phase 4: get the workspace source by unparsing workspace_flat_.
    // Used by (eval-current :jit) to pass a proper source string to
    // the JIT pipeline. Returns empty if no workspace is set.
    using GetWorkspaceSourceFn = std::string();
    std::function<GetWorkspaceSourceFn> get_workspace_source_fn_ = nullptr;
    void set_get_workspace_source_fn(std::function<GetWorkspaceSourceFn> fn) {
        get_workspace_source_fn_ = std::move(fn);
    }
    // Phase 4: JIT-execute an Aura source string and return the result.
    // Used by (eval-current :jit) to compile-and-run the workspace.
    // Returns nullopt if the service didn't install a hook (e.g. unit-test
    // Evaluator without a CompilerService).
    using TryJitFn = std::optional<aura::compiler::types::EvalValue>(const std::string&);
    std::function<TryJitFn> try_jit_fn_ = nullptr;
    void set_try_jit_fn(std::function<TryJitFn> fn) { try_jit_fn_ = std::move(fn); }
    // Issue #194: hook to query the runtime→intrinsic migration
    // counter from the AuraJIT. The Evaluator doesn't have a
    // direct pointer to the JIT (that's owned by
    // CompilerService), so the service installs a hook that
    // returns the current value. Returns 0 if no hook is
    // installed (e.g. unit-test Evaluator without a JIT).
    using GetIntrinsicCountFn = std::uint64_t();
    std::function<GetIntrinsicCountFn> get_intrinsic_count_fn_ = nullptr;
    void set_get_intrinsic_count_fn(std::function<GetIntrinsicCountFn> fn) {
        get_intrinsic_count_fn_ = std::move(fn);
    }
    // Issue #193: hook to query the per-function unhandled-opcode
    // count from the AuraJIT. fn-name is the Aura symbol name
    // (e.g. "my-func"). Returns 0 if the function has never been
    // compiled, or if no JIT hook is installed.
    using GetJitUnhandledCountFn = std::uint64_t(const char*);
    std::function<GetJitUnhandledCountFn> get_jit_unhandled_count_fn_ = nullptr;
    void set_get_jit_unhandled_count_fn(std::function<GetJitUnhandledCountFn> fn) {
        get_jit_unhandled_count_fn_ = std::move(fn);
    }
    // Issue #427: hook to query a single-line JIT metrics
    // summary (the same string AuraJIT::Metrics::format
    // produces). Returns "" if no hook is installed (e.g.
    // unit-test Evaluator without a JIT). The string is
    // produced into a thread-local buffer in the hook
    // closure (avoids lifetime issues across the
    // std::function boundary). Used by the
    // (query:jit-stats) Aura primitive.
    using GetJitStatsFn = const char*();
    std::function<GetJitStatsFn> get_jit_stats_fn_ = nullptr;
    void set_get_jit_stats_fn(std::function<GetJitStatsFn> fn) {
        get_jit_stats_fn_ = std::move(fn);
    }
    // Issue #196: hook to query the incremental-compilation
    // observability struct from the CompilerService. Returns
    // a packed uint64 with (cache_size << 48) | (dirty_count << 32)
    // | (epoch << 16) | (edges & 0xFFFF). Returns 0 if no hook
    // is installed (e.g. unit-test Evaluator without a
    // CompilerService). The 16-bit per-field packing is fine
    // for the realistic scale of AI multi-round mutations
    // (typically < 64K defines, < 64K dirty entries, < 64K
    // mutation epoch, < 64K dep edges per define).
    using GetIncrementalStatsFn = std::uint64_t();
    std::function<GetIncrementalStatsFn> get_incremental_stats_fn_ = nullptr;
    void set_get_incremental_stats_fn(std::function<GetIncrementalStatsFn> fn) {
        get_incremental_stats_fn_ = std::move(fn);
    }

    // Issue #196: per-block dirty hooks. Mirror the
    // get_incremental_stats_fn_ pattern: the hook is a
    // stateless function that reads the current state of
    // the CompilerService's ir_cache_v2_. The 3 hooks are:
    //   - dirty_block_count(name)           — total dirty blocks
    //   - func_dirty_block_count(name, i)   — dirty blocks in func i
    //   - is_block_dirty(name, i, b)        — is (i, b) dirty?
    //   - mark_block_dirty(name, i, b)      — mark (i, b) dirty
    //   - clear_block_dirty(name, i, b)     — clear (i, b) dirty
    // All return 0 / false if no hook is installed, so
    // unit-test Evaluator instances (without a CompilerService)
    // stay default-safe.
    using GetDirtyBlockCountFn = std::uint64_t(const char*);
    std::function<GetDirtyBlockCountFn> get_dirty_block_count_fn_ = nullptr;
    void set_get_dirty_block_count_fn(std::function<GetDirtyBlockCountFn> fn) {
        get_dirty_block_count_fn_ = std::move(fn);
    }
    using GetFuncDirtyBlockCountFn = std::uint64_t(const char*, std::size_t);
    std::function<GetFuncDirtyBlockCountFn> get_func_dirty_block_count_fn_ = nullptr;
    void set_get_func_dirty_block_count_fn(std::function<GetFuncDirtyBlockCountFn> fn) {
        get_func_dirty_block_count_fn_ = std::move(fn);
    }
    using IsBlockDirtyFn = bool(const char*, std::size_t, std::uint32_t);
    std::function<IsBlockDirtyFn> is_block_dirty_fn_ = nullptr;
    void set_is_block_dirty_fn(std::function<IsBlockDirtyFn> fn) {
        is_block_dirty_fn_ = std::move(fn);
    }
    using MarkBlockDirtyFn = bool(const char*, std::size_t, std::uint32_t);
    std::function<MarkBlockDirtyFn> mark_block_dirty_fn_ = nullptr;
    void set_mark_block_dirty_fn(std::function<MarkBlockDirtyFn> fn) {
        mark_block_dirty_fn_ = std::move(fn);
    }
    using ClearBlockDirtyFn = bool(const char*, std::size_t, std::uint32_t);
    std::function<ClearBlockDirtyFn> clear_block_dirty_fn_ = nullptr;
    void set_clear_block_dirty_fn(std::function<ClearBlockDirtyFn> fn) {
        clear_block_dirty_fn_ = std::move(fn);
    }
    // Issue #429: hook for (query:soa-dirty-stats). The
    // hook reads the live per-block / per-instruction dirty
    // state of the CompilerService's ir_cache_v2_ in one
    // pass and returns an 8-field SoaDirtyStats struct
    // (see CompilerService::get_soa_dirty_stats). Returns
    // a zero struct if no hook is installed. The hook
    // closure is stateless (a one-shot aggregate read);
    // no buffer lifetime issues.
    struct SoaDirtyStats {
        std::uint64_t cached_fns = 0;
        std::uint64_t dirty_fns = 0;
        std::uint64_t total_blocks = 0;
        std::uint64_t dirty_blocks = 0;
        std::uint64_t total_instructions = 0;
        std::uint64_t dirty_instructions = 0;
        std::uint64_t dirty_block_pct = 0;
        std::uint64_t dirty_instruction_pct = 0;
    };
    using GetSoaDirtyStatsFn = SoaDirtyStats();
    std::function<GetSoaDirtyStatsFn> get_soa_dirty_stats_fn_ = nullptr;
    void set_get_soa_dirty_stats_fn(std::function<GetSoaDirtyStatsFn> fn) {
        get_soa_dirty_stats_fn_ = std::move(fn);
    }
    // Issue #460: per-instruction dirty hooks. Mirror the
    // per-block pattern above. The 3 hooks are:
    //   - is_instruction_dirty(name, i, b, k)        — is (i, b, k) dirty?
    //   - mark_instruction_dirty(name, i, b, k)     — mark (i, b, k) dirty
    //   - clear_instruction_dirty(name, i, b, k)    — clear (i, b, k) dirty
    // Where (i, b, k) = (function-index, block-index, instruction-index).
    // All return 0 / false if no hook is installed, so
    // unit-test Evaluator instances stay default-safe.
    using IsInstructionDirtyFn = bool(const char*, std::size_t, std::uint32_t, std::uint32_t);
    std::function<IsInstructionDirtyFn> is_instruction_dirty_fn_ = nullptr;
    void set_is_instruction_dirty_fn(std::function<IsInstructionDirtyFn> fn) {
        is_instruction_dirty_fn_ = std::move(fn);
    }
    using MarkInstructionDirtyFn = bool(const char*, std::size_t, std::uint32_t, std::uint32_t);
    std::function<MarkInstructionDirtyFn> mark_instruction_dirty_fn_ = nullptr;
    void set_mark_instruction_dirty_fn(std::function<MarkInstructionDirtyFn> fn) {
        mark_instruction_dirty_fn_ = std::move(fn);
    }
    using ClearInstructionDirtyFn = bool(const char*, std::size_t, std::uint32_t, std::uint32_t);
    std::function<ClearInstructionDirtyFn> clear_instruction_dirty_fn_ = nullptr;
    void set_clear_instruction_dirty_fn(std::function<ClearInstructionDirtyFn> fn) {
        clear_instruction_dirty_fn_ = std::move(fn);
    }
    // Issue #460: partial-relower + impact-scope stats.
    // Counters bumped by the partial-relower path and the
    // impact-scope analysis. Stats-only (relaxed-ordering).
    std::atomic<std::uint64_t> partial_relower_count_{0};
    std::atomic<std::uint64_t> impact_scope_calls_{0};
    std::atomic<std::uint64_t> total_affected_blocks_{0};
    [[nodiscard]] std::uint64_t get_partial_relower_count() const noexcept {
        return partial_relower_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_impact_scope_calls() const noexcept {
        return impact_scope_calls_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_total_affected_blocks() const noexcept {
        return total_affected_blocks_.load(std::memory_order_relaxed);
    }
    void bump_partial_relower_count() noexcept {
        partial_relower_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_impact_scope_calls(std::uint64_t affected_blocks = 1) noexcept {
        impact_scope_calls_.fetch_add(1, std::memory_order_relaxed);
        total_affected_blocks_.fetch_add(affected_blocks, std::memory_order_relaxed);
        bump_incremental_closure_blocks_relowered(affected_blocks);
    }

    // Issue #240: per-node occurrence-dirty bit hooks.
    // Mutation primitives that affect occurrence narrowing
    // (e.g., mutating the predicate of an if-context whose
    // predicate narrows a variable) call these to tag the
    // affected node with DirtyReason::kOccurrenceDirty so
    // find_occurrence_contexts in type_checker_impl.cpp
    // can scope the diagnostic precisely (vs the conservative
    // pre-#240 path that flagged every if-context in the
    // dirty scope).
    //
    // Returns true if the hook is installed and the operation
    // succeeded; false otherwise. The hook signature takes a
    // node id (from the workspace FlatAST) and a "set" flag.
    // When `set` is true, marks the node with the occurrence
    // dirty reason; when false, clears it.
    using SetOccurrenceDirtyFn = bool(std::uint32_t /*node_id*/, bool /*set*/);
    std::function<SetOccurrenceDirtyFn> set_occurrence_dirty_fn_ = nullptr;
    void set_set_occurrence_dirty_fn(std::function<SetOccurrenceDirtyFn> fn) {
        set_occurrence_dirty_fn_ = std::move(fn);
    }
    // Issue #197: hook to query the inliner's lifetime
    // total counters. The InlinePass tracks
    // total_inlined (pre-#197 constant-substitution path)
    // and total_inlined_branch_aware (post-#197
    // branch-aware path) as static process-wide counters.
    // The hook returns them packed as (branch_aware << 32)
    // | inlined, so a single uint64 can carry both 32-bit
    // counts. Returns 0 if no hook is installed.
    using GetInlineStatsFn = std::uint64_t();
    std::function<GetInlineStatsFn> get_inline_stats_fn_ = nullptr;
    void set_get_inline_stats_fn(std::function<GetInlineStatsFn> fn) {
        get_inline_stats_fn_ = std::move(fn);
    }
    // Issue #388: separate getter for macro-hygiene skipped
    // count (the packed uint64 from get_inline_stats_fn_ is
    // already used for inlined + branch-aware counts).
    using GetMacroHygieneSkippedFn = std::uint64_t();
    std::function<GetMacroHygieneSkippedFn> get_macro_hygiene_skipped_fn_ = nullptr;
    void set_get_macro_hygiene_skipped_fn(std::function<GetMacroHygieneSkippedFn> fn) {
        get_macro_hygiene_skipped_fn_ = std::move(fn);
    }
    // Issue #1420 AC3: packed uint64 layout for
    // (compile:bidirectional-stats) EDSL primitive. The
    // hook reads 4 CompilerMetrics counters (per Issue
    // #1420's annotation contract enforcement in
    // check_flat_call) and packs them as:
    //   bits  0-23: check_call_total (24-bit, ~16M max)
    //   bits 24-39: annotation_pass_total (16-bit, 65535)
    //   bits 40-55: annotation_fail_total (16-bit, 65535)
    //   bits 56-63: coercion_deferred_total (8-bit, 255)
    // Mode (full / disabled) is read separately via
    // CompilerService::bidirectional_mode() because bool
    // fits awkwardly into 64-bit packed counters and the
    // accessor already exists for the persistent
    // TypeChecker instance.
    using GetBidirectionalStatsFn = std::uint64_t();
    std::function<GetBidirectionalStatsFn> get_bidirectional_stats_fn_ = nullptr;
    void set_get_bidirectional_stats_fn(std::function<GetBidirectionalStatsFn> fn) {
        get_bidirectional_stats_fn_ = std::move(fn);
    }

    // Mutation typecheck error state (P2 #34)
    const std::string& last_mutate_error() const { return last_mutate_error_; }
    void clear_last_mutate_error() { last_mutate_error_.clear(); }
    bool has_type_error() const { return !last_mutate_error_.empty(); }

    // ── Panic auto-rollback (Issue #39) ─────────────────────
    // When enabled, runtime errors after mutations automatically
    // restore the workspace to the last known good state.
    void set_auto_rollback_on_panic(bool v) { panic_auto_rollback_ = v; }
    bool auto_rollback_on_panic() const { return panic_auto_rollback_; }

    // Save current source as a safe checkpoint. Returns true if saved.
    // Call before a mutation to ensure we can rollback on failure.
    bool save_panic_checkpoint();

    // Restore to the last safe checkpoint (ast:restore-like).
    // Returns true if restore was performed.
    bool restore_panic_checkpoint();

    // Issue #1363: type-erased host for aura.core.panic_cp::PanicCheckpointGuard.
    // Core module cannot depend on Evaluator; bind save/restore via void*.
    [[nodiscard]] static aura::core::panic_cp::PanicCheckpointHost
    panic_checkpoint_host(Evaluator& ev) noexcept {
        // Issue #1393: bind expected_evaluator_id = &ev so the Guard
        // dtor can detect cross-evaluator restore attempts (aot:reload,
        // persist:load, fiber with cross-evaluator body). When the host
        // is rebound to a different Evaluator between ctor and dtor,
        // expected_evaluator_id (set here) != ctx (current binding)
        // → Guard skips restore + bumps
        // restores_discriminator_failed counter (no UB).
        // Issue #1727: also bind clear so mismatch path drops stale
        // panic_safe_* / GC-defer without restoring wrong state.
        return aura::core::panic_cp::PanicCheckpointHost{
            &ev,
            &ev, // expected_evaluator_id discriminator (Issue #1393)
            [](void* p) noexcept -> bool {
                return static_cast<Evaluator*>(p)->save_panic_checkpoint();
            },
            [](void* p) noexcept -> bool {
                return static_cast<Evaluator*>(p)->restore_panic_checkpoint();
            },
            [](void* p) noexcept -> bool {
                static_cast<Evaluator*>(p)->clear_panic_checkpoint();
                return true;
            },
        };
    }

    // Issue #1363: RAII guard that actually saves/restores panic checkpoint.
    [[nodiscard]] aura::core::panic_cp::PanicCheckpointGuard
    make_panic_checkpoint_guard() noexcept {
        return aura::core::panic_cp::PanicCheckpointGuard(panic_checkpoint_host(*this));
    }

    // Issue #1727: drop panic checkpoint fields without restore.
    // Used when cross-evaluator discriminator skips restore, and as
    // the shared body of commit_panic_checkpoint's field wipe.
    void clear_panic_checkpoint() noexcept {
        // Issue #1489 / #1667: release process-wide GC defer FIRST so a
        // throw from later bookkeeping cannot leave depth permanently
        // elevated (exception-safe dual of ~Evaluator release).
        release_gc_defer_for_pending_panic();
        panic_safe_source_.clear();
        // Issue #242: clear the arena-size snapshots too so a
        // subsequent save_panic_checkpoint() starts fresh.
        panic_safe_cells_size_ = 0;
        panic_safe_pairs_size_ = 0;
        panic_safe_string_heap_size_ = 0;
        panic_safe_env_frames_size_ = 0;
    }

    // Clear the checkpoint (call after successful mutation commit).
    void commit_panic_checkpoint() {
        clear_panic_checkpoint();
        // Issue #1728: bump bridge_epoch so cross-COW / cross-evaluator
        // closure freshness checks (is_bridge_stale / aura_closure_call)
        // observe the committed workspace transition. Same hook used by
        // compact_env_frames (#1510). No-op when service not bound.
        if (bridge_epoch_bump_fn_ && compiler_service_)
            bridge_epoch_bump_fn_(compiler_service_);
        // Issue #548: bump panic_checkpoint_commit_count_
        // so (query:panic-checkpoint-lifecycle-stats) can
        // report the lifetime commit count.
        bump_panic_checkpoint_commit_count();
        bump_longrunning_checkpoint_success();
    }

    // Check if a safe checkpoint exists.
    bool has_panic_checkpoint() const { return !panic_safe_source_.empty(); }

    // Issue #596: on fiber resume, restore a pending panic checkpoint when
    // the outermost Guard mutation failed (cross-fiber rollback path).
    void restore_panic_checkpoint_on_fiber_resume_if_needed() noexcept;
    // Issue #1637: GC compact variant — paired caller is the arena
    // compact hook (evaluator.ixx set_on_compact_hook → on_arena_compact_hook
    // in evaluator_fiber_mutation.cpp). Same closed-loop logic as the
    // resume path (truncate_env_frames_to_checkpoint + env_generation bump
    // + invalidate_post_rollback_env_frames + walk_active_closures bridge
    // refresh + clear_panic_checkpoint), but bumps
    // post_compact_checkpoint_restore_total instead of
    // post_steal_checkpoint_restore_total so dashboards can distinguish
    // GC-induced restores from steal-induced ones.
    void restore_panic_checkpoint_on_arena_compact_if_needed() noexcept;
    // Issue #1637: hot-swap deopt variant — wired via
    // aura_evaluator_hot_swap_panic_restore() C bridge trampoline from
    // evaluator_primitives_types.cpp after the (hot-swap:fn ...) callback
    // returns. Closed-loop restore with post_hot_swap_checkpoint_restore_total
    // counter bumped (so re-deopt + recover sequences are observable).
    void restore_panic_checkpoint_on_hot_swap_if_needed() noexcept;
    // Issue #1637: shared body of the three restore_<event>_if_needed()
    // variants. Runs truncate_env_frames_to_checkpoint → env_generation
    // bump → invalidate_post_rollback_env_frames → walk_active_closures
    // (refresh stale bridge_epoch) → clear_panic_checkpoint (release GC
    // defer per #1489) and bumps the cross_fiber_panic_heal_success +
    // (optionally) mutation_boundary_steal_safe_total outcome counters.
    void run_post_restore_lifecycle_close(bool safe_total_event) noexcept;
    // Issue #1638: dual-path consistency gate for EnvFrame dual-path
    // (bindings_ vs bindings_symid_ + bindings_linear_ownership_state_)
    // access sites. Called from materialize_call_env / lookup_by_symid_chain
    // / walk_env_frames / GCEnvWalkFn / JIT Apply prologue to verify
    // the frame is not stale (version drift vs defuse_version). On
    // stale detection bumps env_frame_version_drift_prevented (always)
    // and dual_path_stale_fallback_total (when the caller successfully
    // falls back to the symid path / rebuild). Returns false on stale
    // (caller decides whether to fall back or refuse the access).
    // `site` is a free-form string for log/metric attribution
    // (e.g., "materialize_call_env", "GCEnvWalkFn").
    bool ensure_env_frame_dual_path_consistent(EnvId id, const char* site) noexcept;
    // Issue #1638: mutation_log compact at boundary exit success path.
    // Delegates to FlatAST::compact_mutation_log() (shrink_to_fit on
    // mutation_log_) and bumps mutation_log_compact_bytes_saved by
    // the bytes saved. No-op when no workspace_flat_ is wired or
    // mutation_log_ is empty.
    void compact_mutation_log() noexcept;

    // Get the safe checkpoint source (for introspection).
    const std::string& panic_safe_source() const { return panic_safe_source_; }

    // Issue #242: arena-size snapshots for diagnostics + tests.
    // These reflect the sizes of the 4 pmr/append-only arenas at
    // the most recent save_panic_checkpoint() call. They're used by
    // restore_panic_checkpoint to truncate back; exposed here so
    // tests can verify the snapshot behavior without poking at
    // private members.
    std::size_t panic_safe_cells_size() const { return panic_safe_cells_size_; }
    std::size_t panic_safe_pairs_size() const { return panic_safe_pairs_size_; }
    std::size_t panic_safe_string_heap_size() const { return panic_safe_string_heap_size_; }
    std::size_t panic_safe_env_frames_size() const { return panic_safe_env_frames_size_; }
    // Test-only setters for the arena-size snapshots. Production
    // code sets them via save_panic_checkpoint(); tests use these
    // to set up pre-conditions without going through the full
    // save + restore cycle.
    void set_panic_safe_cells_size_for_test(std::size_t v) { panic_safe_cells_size_ = v; }
    void set_panic_safe_pairs_size_for_test(std::size_t v) { panic_safe_pairs_size_ = v; }
    void set_panic_safe_string_heap_size_for_test(std::size_t v) {
        panic_safe_string_heap_size_ = v;
    }
    void set_panic_safe_env_frames_size_for_test(std::size_t v) { panic_safe_env_frames_size_ = v; }

    // Set/get a shared workspace tree (for cross-session workspace sharing in serve mode).
    void set_workspace_tree(void* wt) { workspace_tree_ = wt; }
    // Set the AST/pool that source-reading primitives (current-source,
    // workspace:conflicts-with, etc.) operate on. Called by CompilerService
    // at the start of each eval so primitives see the current script's AST.
    // Distinct from set_workspace_flat: workspace_flat_ is the persistent
    // EDSL workspace (set via (set-code ...)), this is the per-eval
    // source-being-evaluated.
    void set_workspace_flat(ast::FlatAST* f) {
        // Issue #1729: exclusive workspace_mtx_ during the swap so
        // concurrent set/get races cannot tear the pointer mid-rebuild;
        // roll back workspace_flat_ if eager index rebuild throws so we
        // never leave new flat + empty index half-committed.
        std::unique_lock<std::shared_mutex> lk(workspace_mtx_);
        ast::FlatAST* const saved = workspace_flat_;
        workspace_flat_ = f;
        // Issue #1419: re-stamp agent fingerprint onto the new
        // workspace so subsequent mutations inherit the author.
        if (f) {
            const auto fp =
                current_agent_fingerprint_.load(std::memory_order_acquire); // Issue #1730
            if (fp != 0)
                f->set_mutation_author_fingerprint(fp);
        }
        // Issue #211: invalidate the (tag, arity) index
        // when the workspace changes. The index is keyed
        // by node positions in the OLD workspace, which
        // are invalid for the new workspace.
        // (The set-code primitive ALSO invalidates inline,
        // because it assigns to workspace_flat_ directly
        // instead of going through this setter. The double
        // invalidation is safe — invalidate is a no-op if
        // the index is already empty.)
        invalidate_tag_arity_index();
        try {
            if (f && pattern_index_policy_ == PatternIndexPolicy::EagerAfterCow)
                build_tag_arity_index(
                    static_cast<std::uint8_t>(PatternIndexRebuildTrigger::EagerCow));
        } catch (...) {
            // [SILENCE-PRIM-#615] Issue #1729: rethrow after rollback — not a
            // silent swallow. catch(...) restores workspace_flat_ + index so a
            // throwing rebuild never leaves half-committed state, then rethrows.
            workspace_flat_ = saved;
            invalidate_tag_arity_index();
            throw;
        }
    }
    void set_workspace_pool(ast::StringPool* p) { workspace_pool_ = p; }
    ast::FlatAST* workspace_flat() const { return workspace_flat_; }
    ast::StringPool* workspace_pool() const { return workspace_pool_; }

    // Issue #1419: AI agent identity for MutationRecord.author_fingerprint.
    // 0 = system (default). Non-zero = hash(agent_id). Propagated to the
    // workspace FlatAST provenance context so every add_mutation stamps
    // the author. Used by TypedTransactionGuard + (mutate:set-agent-fingerprint).
    // Issue #1730: atomic release/acquire — concurrent fibers race on
    // plain uint64 read/write of author identity.
    void set_current_agent_fingerprint(std::uint64_t fp) noexcept {
        current_agent_fingerprint_.store(fp, std::memory_order_release);
        if (workspace_flat_)
            workspace_flat_->set_mutation_author_fingerprint(fp);
    }
    [[nodiscard]] std::uint64_t current_agent_fingerprint() const noexcept {
        return current_agent_fingerprint_.load(std::memory_order_acquire);
    }

    // Set the AST/pool that source-reading primitives (current-source) read
    // by default. The "per-eval current source" pointer. Set by
    // CompilerService::eval / eval_ir / exec_jit right after parsing the
    // script, before any user code runs. See dual-workspace design (archived:
    // docs-archive-pre-2026-06) for the dual-workspace rationale.
    void set_current_flat(ast::FlatAST* f) { current_flat_ = f; }
    void set_current_pool(ast::StringPool* p) { current_pool_ = p; }
    ast::FlatAST* current_flat() const { return current_flat_; }
    ast::StringPool* current_pool() const { return current_pool_; }

    // Issue #145 follow-up / Phase 2.5.0 prep: canonical pool accessor.
    // For now, the canonical pool is the workspace pool — it is the
    // pool where almost all `intern()` calls already route (39 sites
    // across evaluator partition TUs, vs. ~5 in pat_pool / tmp_pool / local
    // scratch pools). The pool unification refactor (Phase 2.5.0 in
    // C++26 module layout) will:
    //
    //   1. Route every new `intern()` call through this accessor
    //      instead of the named pool_ members.
    //   2. Migrate the few pat_pool / tmp_pool scratch pools to
    //      use the canonical pool (or extract the names they
    //      intern into a separate "interning-once" set).
    //   3. Eventually demote the other pool_ members to nullable
    //      legacy fields (or remove them) once the migration
    //      completes.
    //
    // Returns nullptr when no workspace is loaded (callers must
    // check — that's the same contract as `workspace_pool()`).
    ast::StringPool* canonical_pool() const { return workspace_pool_; }

    // Issue #145 follow-up / Phase 2.5.0: helper for the demote-
    // to-legacy pool migration. Closure::pool and Env::pool_ are
    // nullable legacy fields — for closures/envis built after
    // the canonical-pool migration, the field is nullptr and the
    // helper falls through to canonical_pool. Legacy call sites
    // (closures built before the migration) still pass through
    // their captured pool for backward compat.
    //
    // Returns nullptr only when BOTH the legacy pool and the
    // canonical pool are unavailable (no workspace loaded).
    ast::StringPool* pool_or_canonical(ast::StringPool* legacy) const {
        return legacy ? legacy : canonical_pool();
    }

    void* workspace_tree() const { return workspace_tree_; }
    // Update the shared tree's root node to point to this evaluator's workspace.
    void update_shared_tree_root();

    // Create a new workspace tree with a root node (for serve mode sharing).
    // The caller owns the returned pointer.
    static void* create_workspace_tree();

    // Free a workspace tree created by create_workspace_tree().
    static void destroy_workspace_tree(void* wt);

    // Issue #141 AC: lazy COW. Trigger clone for the active child
    // workspace if it still shares parent's flat. No-op if active is
    // root, already cloned, or read-only. Returns true on success,
    // false if read-only or COW refused (budget exceeded).
    bool trigger_lazy_cow(void* wt);

    // After trigger_lazy_cow, the active workspace's flat/pool may
    // have been reallocated. Call this to refresh the pointers.
    // Sets *out_flat and *out_pool. Returns true if the tree has
    // a valid active workspace.
    static bool refresh_active_flat_pool(void* wt, void** out_flat, void** out_pool);

    // ── Issue #152 P0 — Control Plane Isolation (Phase 0 prep) ─
    // The above primitives (create_workspace_tree, set /
    // get workspace_tree, trigger_lazy_cow, refresh_active_flat_pool)
    // are the foundation. The 3-issue #152 work builds on top:
    //
    // Phase 1 (Finish WorkspaceTree, ~3-4 commits):
    //   - workspace:create Aura form (parse + add to
    //     primitives table; the WorkspaceTree* is returned
    //     to the caller as an opaque handle).
    //   - workspace:switch (atomic — COW parent's flat if
    //     the target is read-only, then set as active).
    //   - workspace:lock (read-only flag on a workspace
    //     node; writes fail with a diagnostic).
    //   - workspace:merge (child -> parent, diff-based;
    //     only the changed subtrees are copied).
    //   - workspace:discard (free a transient workspace,
    //     optionally promote child to root if it was the
    //     last reference).
    //   - transient/experimental workspace support (COW
    //     on write; the lazy COW from #141 is the
    //     building block).
    //
    // Phase 2 (Mutation Impact Analysis, ~2-3 commits):
    //   - Cross-link to #150 Phase 3's defines_referencing_sym
    //     helper (commit 02a1c75). The query is already
    //     there; the wiring into typed_mutate's invalidation
    //     path is the consumer.
    //   - Per-mutation affected-set: when a mutate:rebind
    //     is requested, call defines_referencing_sym to
    //     find the affected Defines, then invalidate only
    //     those AOT cache entries (cross-link to #151
    //     Phase 3's region-aware cache invalidation).
    //   - "What would this mutation affect?" query
    //     primitive for the agent orchestration layer.
    //
    // Phase 3 (Orchestration Integration, ~2 commits):
    //   - orch:* primitives + agent spawning respect
    //     workspace boundaries (an agent spawned in
    //     workspace W sees only W's mutations).
    //   - Examples: safe experimentation in a transient
    //     workspace, merge-or-discard workflow for
    //     code evolution.
    //
    // Today's behavior: unchanged. The prep commit is a
    // design-doc anchor so the actual phases have a
    // shared roadmap; no code changes outside this
    // comment. The 3 phases will follow the same
    // conservative-MVP + read-side-first pattern as #148,
    // #149, #150, #151.

    const std::string& session_id() const { return session_id_; }

    // ── GC root registration (Issue #113) ──────────────
    // Walk the evaluator's vector heaps and populate the GC root set
    // pointed to by `root_set_out` (an opaque `aura::serve::GCRootSet*`).
    //
    // We use `void*` (not the full GCRootSet& reference) to avoid
    // pulling serve/gc_coordinator.h into every TU that includes
    // this module interface. The full definition is only needed at
    // the call site in evaluator_gc.cpp. This is the same
    // pattern as `defuse_index_destroy` (Issue #107) and the
    // GCRootFlushFn typedef in messaging_bridge.h.
    //
    // Called by the GC collector during the root collection phase
    // (after the safepoint has stopped all fibers, so no concurrent
    // mutator can run). Holds `heap_mutex()` so a non-fiber thread
    // in serve-async mode can't race a concurrent
    // `string_heap_.push_back` (or similar) with the walk.
    void flush_gc_roots(void* root_set_out);
    // Total number of vector-heap entries that would be marked as roots.
    // Cheap (no allocation, just sizes + map iteration). Useful for
    // pre-GC metrics and for tests that want to verify root set
    // population without allocating the GCRootSet.
    [[nodiscard]] std::size_t gc_root_count() const;
    // Issue #682: export tree-walker Closure / EnvId roots for
    // compiler GC coordination (bridge epoch gate).
    // `current_bridge_epoch` is the caller's safepoint snapshot;
    // Issue #1734 compares it to live current_bridge_epoch() and
    // bumps gc_roots_bridge_epoch_drift_total on mismatch, filtering
    // with the live epoch.
    void collect_compiler_managed_gc_roots(std::vector<std::int64_t>& closure_roots_out,
                                           std::vector<std::int64_t>& env_roots_out,
                                           std::uint64_t current_bridge_epoch) const;
    // Issue #683 / #1515 / #1755: linear ownership runtime probe.
    // Pure dual-epoch check: frame_version vs current_version, and
    // bridge_epoch vs current_bridge_epoch (bridge_epoch==0 skips the
    // bridge half — unbridged / untracked capture).
    // Issue #1755: when bridge epochs mismatch (and bridge_epoch != 0),
    // returns false (unsafe) and optionally bumps
    // *bridge_epoch_drift_counter (linear_validate_bridge_epoch_drift_total).
    // Static-safe: counter is an optional out-parameter so call sites
    // with CompilerMetrics can observe drift without making this method
    // non-static.
    [[nodiscard]] static bool validate_linear_ownership_state(
        std::uint8_t linear_state, std::uint64_t frame_version, std::uint64_t current_version,
        std::uint64_t bridge_epoch, std::uint64_t current_bridge_epoch,
        std::atomic<std::uint64_t>* bridge_epoch_drift_counter = nullptr) noexcept;
    void probe_linear_ownership_at_gc_safepoint() noexcept;
    void probe_linear_ownership_on_fiber_steal() noexcept;
    // Issue #1490 / #1580: post-steal / post-resume EnvFrame + bridge_epoch
    // consistency. Walks live closures (and optional hint env_id),
    // refreshes stale frame.version_ vs defuse_version_, detects
    // bridge_epoch drift, repairs dual-path when needed, bumps
    // post-steal metrics. hint_env_id / expected_epoch may be 0
    // (full scan / current epoch). Returns # frames refreshed.
    std::size_t refresh_stale_frames_after_steal(std::uint64_t hint_env_id = 0,
                                                 std::uint64_t expected_epoch = 0) noexcept;
    // Issue #1612: MacroIntroduced marker / provenance refresh after
    // fiber resume, steal, or GC compact. Restamps Atomic/COW pins that
    // target MacroIntroduced nodes; repairs marker drift when expansion
    // provenance was lost. Returns # of macro-related repairs.
    std::size_t refresh_stale_macro_frames(std::uint64_t hint_env_id = 0,
                                           std::uint64_t expected_epoch = 0) noexcept;
    // Issue #1612: probe + repin MacroIntroduced provenance on pinned
    // StableNodeRefs (pairs with probe_and_repin_linear_on_steal).
    void probe_and_repin_macro_provenance() noexcept;
    // Issue #1490: linear ownership probe + COW/StableNodeRef re-pin
    // after fiber steal/resume (wraps probe_linear + re_pin).
    void probe_and_repin_linear_on_steal() noexcept;
    // Issue #1580: transfer pending PanicCheckpoint across steal/resume
    // and revalidate defuse/bridge versions (unified entry for Fiber::resume).
    // fiber_void may be null (uses g_current_fiber_void). Returns true if a
    // pending checkpoint was transferred/revalidated.
    bool transfer_and_revalidate_panic_checkpoint(void* fiber_void = nullptr) noexcept;
    // Issue #1580: full post-resume closed loop — refresh frames, repin
    // linear/StableNodeRef, transfer panic checkpoint. Uses fiber resume
    // hints when fiber_void is a Fiber*.
    void complete_post_resume_steal_refresh(void* fiber_void = nullptr) noexcept;
    // Issue #1595: after Fiber::join / parallel child completion — linear
    // ownership probe + StableNodeRef restamp (joiner-side enforcement).
    // joined_fiber_void may be null (uses current fiber / full scan).
    void complete_post_join_linear_enforcement(void* joined_fiber_void = nullptr) noexcept;
    // Issue #1614: TypedMutationAudit real invariant suite (type reval +
    // linear_post_mutate_enforce_all + provenance/reflect). Gated by
    // typed_audit::should_audit. Records trail + counters; returns true
    // if all three checks passed (or checks were not applicable).
    [[nodiscard]] bool run_typed_mutation_invariant_audit(std::uint64_t mutation_id,
                                                          std::string_view op_name,
                                                          std::uint32_t target_node,
                                                          std::uint64_t before_epoch,
                                                          std::uint64_t after_epoch) noexcept;
    // Issue #1595: MultiFiberMailbox attach/recv/broadcast path.
    // Returns false when a linear claim in payload fails ownership checks
    // (caller must not deliver). Always runs light StableNodeRef probe.
    [[nodiscard]] bool probe_mailbox_linear_and_stable_refs(std::uint64_t from_fiber,
                                                            std::uint64_t to_fiber,
                                                            std::string_view payload) noexcept;
    [[nodiscard]] std::uint64_t get_post_steal_refresh_count() const noexcept {
        return post_steal_refresh_count_.load(std::memory_order_relaxed);
    }
    // Issue #1612: macro-specific post-steal / resume refresh counters.
    [[nodiscard]] std::uint64_t get_macro_stale_ref_prevented() const noexcept {
        if (auto* m = static_cast<const CompilerMetrics*>(compiler_metrics()))
            return m->macro_stale_ref_prevented_total.load(std::memory_order_relaxed);
        return 0;
    }
    // Issue #1908: MutationBoundaryGuard + macro clone provenance
    // hardening (refine #1014 / #1047). Read-only on the 2 new
    // counters backing (query:macro-provenance-stats) primitive +
    // bridge-hook observability for self-evolution Agents.
    [[nodiscard]] std::uint64_t get_macro_provenance_repin_on_steal_total() const noexcept {
        if (auto* m = static_cast<const CompilerMetrics*>(compiler_metrics()))
            return m->macro_provenance_repin_on_steal_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_hygiene_violation_prevented_on_boundary_total() const noexcept {
        if (auto* m = static_cast<const CompilerMetrics*>(compiler_metrics()))
            return m->hygiene_violation_prevented_on_boundary_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_macro_provenance_repin_total() const noexcept {
        if (auto* m = static_cast<const CompilerMetrics*>(compiler_metrics()))
            return m->macro_provenance_repin_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_macro_refresh_invoke_count() const noexcept {
        return macro_refresh_invoke_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_linear_join_enforcement_total() const noexcept {
        if (auto* m = static_cast<const CompilerMetrics*>(compiler_metrics()))
            return m->linear_join_enforcement_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_mailbox_linear_violation_count() const noexcept {
        if (auto* m = static_cast<const CompilerMetrics*>(compiler_metrics()))
            return m->mailbox_linear_violation_count.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t get_stable_ref_post_join_repin_total() const noexcept {
        if (auto* m = static_cast<const CompilerMetrics*>(compiler_metrics()))
            return m->stable_ref_post_join_repin_total.load(std::memory_order_relaxed);
        return 0;
    }
    // Alias of get_panic_checkpoint_transfer_count for #1580 AC naming.
    [[nodiscard]] std::uint64_t get_panic_transfer_on_steal_count() const noexcept {
        return get_panic_checkpoint_transfer_count();
    }
    // Issue #740: re-snapshot compiler-managed GC roots after
    // invalidate when linear metadata may have changed in JIT L2.
    void resync_linear_jit_gc_roots_after_invalidate() noexcept;
    // Issue #1515: GC safepoint / invalidate unified entry —
    // resync linear JIT GC roots under live bridge_epoch then
    // probe EnvFrame.version_ × linear_ownership_state.
    void sync_linear_roots_and_bridge_epoch() noexcept;
    // Issue #1545: walk tree-walker live closures_ (unique lock while
    // mutating via callback). Parallel to AuraJIT::walk_active_closures
    // and IRExecutor::walk_runtime_closures.
    using ActiveClosureWalkFn = std::function<void(ClosureId, Closure&)>;
    void walk_active_closures(const ActiveClosureWalkFn& fn);
    // Scan live closures for linear captures (EnvFrame SoA state !=
    // Untracked). When mark_invalid: stamp Closure::bridge_epoch = 0 so
    // apply_closure takes safe_fallback (is_bridge_stale). Bumps
    // linear_live_closure_scans_total (+ marked_invalid_total).
    // only_if_moved (#1486 mutation-boundary): mark only when any
    // binding is Moved (use-after-move); invalidate/compact keep
    // only_if_moved=false (mark all linear captures).
    struct LinearLiveClosureScanResult {
        std::size_t examined = 0;
        std::size_t with_linear_capture = 0;
        std::size_t with_moved_capture = 0;
        std::size_t marked_invalid = 0;
    };
    LinearLiveClosureScanResult
    // Issue #1545 / #1486 / #1494 / #1659: scan live TW closures for linear
    // captures. mark_invalid → bridge_epoch=0 (tombstone / safe_fallback).
    // only_if_moved → only mark when a binding is Moved. filter_env_id !=
    // NULL_ENV_ID → only closures capturing that EnvFrame (#1494 env-scoped).
    // #1659: end-to-end with IRInstruction::linear_ownership_state,
    // linear_heap_ live flag, GC safepoint probe, and JIT Apply dual-check.
    scan_live_closures_for_linear_captures(bool mark_invalid = true, bool only_if_moved = false,
                                           EnvId filter_env_id = NULL_ENV_ID) noexcept;
    // Test/helper: register a Closure in closures_ (stamps bridge_epoch).
    ClosureId register_active_closure(Closure cl);
    // Issue #1665: erase tree-walker Closure from closures_ (TW free).
    // Distinct from aura_free_closure (JIT g_closure_freed table).
    // Returns true if an entry was removed.
    bool erase_active_closure(ClosureId id) noexcept;
    // Test/helper: snapshot a live Closure by id (nullopt if missing).
    [[nodiscard]] std::optional<Closure> find_active_closure(ClosureId id) const;
    // Issue #1543: linear GC root registration consistency audit.
    // Path ids match docs/design/linear-gc-roots.md §mutation paths.
    static constexpr std::uint8_t kLinearGcRootAuditTypedMutate = 0;
    static constexpr std::uint8_t kLinearGcRootAuditInvalidate = 1;
    static constexpr std::uint8_t kLinearGcRootAuditCompact = 2;
    static constexpr std::uint8_t kLinearGcRootAuditJitHotSwap = 3;
    static constexpr std::uint8_t kLinearGcRootAuditFiberSteal = 4;
    static constexpr std::uint8_t kLinearGcRootAuditGcSafepoint = 5;
    static constexpr std::uint8_t kLinearGcRootAuditManual = 6;
    static constexpr std::size_t kLinearGcRootAuditRingSize = 32;
    struct LinearGcRootAuditEntry {
        std::uint64_t seq = 0;
        std::uint8_t path = 0;
        std::uint8_t ok = 1; // 1 = invariants held
        std::uint64_t registrations = 0;
        std::uint64_t stale_hits = 0;
        std::uint64_t violations_prevented = 0;
        std::uint64_t env_version_resync = 0;
        std::uint64_t live_roots = 0; // current collect_compiler_managed size
    };
    // Snapshot counters, check monotonicity + balance invariants, append
    // audit-log entry, bump linear_gc_root_audit_checks_total. Returns ok.
    bool run_linear_gc_root_audit(std::uint8_t path) noexcept;
    [[nodiscard]] std::uint64_t linear_gc_root_audit_total() const noexcept {
        return linear_gc_root_audit_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t linear_gc_root_audit_seq() const noexcept {
        return linear_gc_root_audit_seq_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const LinearGcRootAuditEntry&
    linear_gc_root_audit_entry_at(std::uint64_t seq) const noexcept {
        return linear_gc_root_audit_ring_[seq % kLinearGcRootAuditRingSize];
    }
    [[nodiscard]] static std::string_view
    linear_gc_root_audit_path_name(std::uint8_t path) noexcept;
    // Issue #1568: unified linear boundary consistency closed-loop.
    // Runs scan_live_closures_for_linear_captures + linear_post_mutate_enforce_all
    // + epoch-fence force-drop + run_linear_gc_root_audit. mark_all_linear:
    //   true  → mark every linear-capturing closure invalid (invalidate/compact/steal)
    //   false → only Moved captures (Guard exit / typed_mutate)
    struct LinearBoundaryEnforceResult {
        std::size_t frames_checked = 0;
        std::size_t closures_scanned = 0;
        std::size_t marked_invalid = 0;
        std::size_t epoch_fence_hits = 0;
        std::size_t moved_violations = 0;
        bool all_safe = true;
    };
    LinearBoundaryEnforceResult
    enforce_linear_boundary_consistency(std::uint8_t path, bool mark_all_linear = true) noexcept;
    // Force Drop / logical invalid: Closure.bridge_epoch = 0 → safe_fallback.
    void force_drop_or_mark_invalid(ClosureId id) noexcept;
    // Linear violation provenance audit ring (#1568).
    static constexpr std::uint8_t kLinearViolReasonMoved = 1;
    static constexpr std::uint8_t kLinearViolReasonEpochStale = 2;
    static constexpr std::uint8_t kLinearViolReasonForceDrop = 3;
    static constexpr std::size_t kLinearViolationAuditRingSize = 64;
    struct LinearViolationAuditEntry {
        std::uint64_t seq = 0;
        std::uint8_t path = 0;
        std::uint8_t reason = 0;
        std::uint64_t epoch = 0;
        std::uint64_t defuse_version = 0;
        std::uint32_t env_id = 0;
        std::uint64_t closure_id = 0;
    };
    [[nodiscard]] std::uint64_t linear_violation_audit_total() const noexcept {
        return linear_violation_audit_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t linear_violation_audit_seq() const noexcept {
        return linear_violation_audit_seq_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const LinearViolationAuditEntry&
    linear_violation_audit_entry_at(std::uint64_t seq) const noexcept {
        return linear_violation_audit_ring_[seq % kLinearViolationAuditRingSize];
    }
    // ── GC sweep / compaction (Issue #113 Phase 3) ──────────
    // After the GC collector has marked live objects, this method
    // reclaims the unmarked ones. Called from the GC coordinator's
    // `collect()` during the sweep phase (after the safepoint has
    // stopped all fibers, so no concurrent mutator can run).
    //
    // The opaque `void*` is `aura::serve::GCSweepBuffers*` /
    // messaging GCSweepPassThru (cast at the call site in
    // evaluator_gc.cpp — same pattern as `flush_gc_roots(void*)`).
    //
    // Issue #1732: return a typed POD (not void*) so callers need
    // no cast. Layout matches messaging_bridge.h GCSweepResultMsg
    // (4×size_t) for ABI-compatible conversion at the bridge edge.
    struct CompactSweepResult {
        std::size_t strings_freed = 0;
        std::size_t pairs_freed = 0;
        std::size_t closures_freed = 0;
        std::size_t fiber_results_freed = 0;
        [[nodiscard]] bool empty() const noexcept {
            return strings_freed == 0 && pairs_freed == 0 && closures_freed == 0 &&
                   fiber_results_freed == 0;
        }
    };
    // Null sweep_buffers → zeroed result (no work). Non-null → fill
    // counts; never heap-allocates the result (Issue #1732).
    CompactSweepResult compact_sweep(void* sweep_buffers);


    void
    set_messaging_callbacks(std::function<bool(const std::string&, const std::string&)>* send_fn,
                            std::function<std::optional<std::string>(int)>* recv_fn,
                            std::function<std::string()>* id_fn) {
        msg_send_fn_ = send_fn;
        msg_recv_fn_ = recv_fn;
        msg_id_fn_ = id_fn;
    }

    // ═══════════════════════════════════════════════════════════
    // Issue #145 Phase 2.1 — EnvFrame SoA infrastructure
    // ═══════════════════════════════════════════════════════════
    //
    // `env_frames_` is a `std::vector<EnvFrame>` arena owned by
    // the Evaluator. New envs are appended (push_back); old envs
    // can be reclaimed in bulk via `reset_env_frames()`.
    //
    // Why not in ASTArena? Env data lives across many modules
    // and survives (gc-temp) reclamation. A separate
    // Evaluator-owned vector gives the SoA pattern its own
    // lifetime without tying it to AST arena grouping.
    //
    // Allocate a new EnvFrame and return its EnvId. The frame
    // is appended to env_frames_; the id is the new size()-1.
    // Returns NULL_ENV_ID on overflow (>4G envs).
    EnvId alloc_env_frame(EnvId parent_id = NULL_ENV_ID, const Primitives* primitives = nullptr);
    // Issue #145 Phase 2.3 — allocate a new EnvFrame from an
    // existing Env's bindings (string + SymId parallel arrays).
    // Mirrors `e.bindings()` and `e.bindings_symid()` into a
    // fresh frame in env_frames_; the new frame's parent_id is
    // `e.parent_id()` (or the explicit `parent_id` arg if
    // provided). Returns the new id, or NULL_ENV_ID on overflow.
    //
    // Used at Closure construction to register the captured env
    // in the SoA arena (P0: legacy Closure::env pointer path removed;
    // env_id is the only capture handle).
    EnvId alloc_env_frame_from_env(const Env& e, EnvId parent_id = NULL_ENV_ID);
    // Issue #145 Phase 2.3 — materialize a fresh Env suitable
    // for evaluating a closure body. Always uses env_frames_[cl.env_id]
    // (SoA walk, GC-safe; P0 legacy cl.env fallback completely removed).
    //
    // Issue #1542: on entry, runs linear_post_mutate_enforce(cl.env_id)
    // so every materialize site (apply_closure, TCO tail call,
    // eval_data_as_code) enforces the same linear post-mutate contract
    // as closure_needs_safe_fallback. On Moved → empty Env fallback
    // (materialize_fallback_total); bumps linear_post_mutate_
    // enforcements for counter parity with #1478.
    //
    // P0: frames carry pure bindings + parent_id only. Support
    // pointers (cells_/pool_/primitives_) are wired by the
    // caller (apply_closure etc.) after materialization.
    // In either case, primitives_/cells_/pool_ are wired from
    // the active Evaluator + Closure::pool — these are the
    // runtime support pointers the body needs to see, not part
    // of the captured scope itself.
    Env materialize_call_env(const Closure& cl);
    // Issue #242: detect a stale EnvFrame (one whose `version_`
    // snapshot is older than the current `defuse_version_`). A
    // closure captured against env_frames_[id] whose frame.version_
    // < defuse_version_ may have inconsistent bindings — the
    // captured scope was mutated after the closure was constructed.
    //
    // Issue #1754: returns true ONLY when the frame exists AND
    // version_ < defuse_version_. NULL / OOB ids return false —
    // use is_env_frame_invalid_id() for that case (no frame, not
    // "stale"). Terminal INVALID_VERSION is is_env_frame_invalid().
    // Call sites that need any of the three: check invalid_id /
    // invalid first, then stale.
    bool is_env_frame_stale(EnvId id) const;
    // Issue #1754: true if id is NULL_ENV_ID or OOB (frame does not
    // exist). Distinct from is_env_frame_invalid (INVALID_VERSION
    // sentinel on a live slot) and is_env_frame_stale (version drift).
    [[nodiscard]] bool is_env_frame_invalid_id(EnvId id) const noexcept {
        return id == NULL_ENV_ID || id >= env_frames_.size();
    }
    // Issue #1478 / #1539 / #1542: linear post-mutate enforcement
    // (real per-cell scan via bindings_linear_ownership_state_).
    // Parallel to is_env_frame_stale. Per-frame check that captured
    // linear values have not been Moved since the frame was stamped.
    // Bumps linear_post_mutate_enforcements (#1478) on every call
    // when env_id has a valid frame.
    //
    // Entry points (both must call — #1542 closes the gap):
    //   1. apply_closure → closure_needs_safe_fallback
    //      (evaluator_eval_flat.cpp) — refuse / bridge safe path
    //   2. materialize_call_env (evaluator_env.cpp) — empty-Env
    //      fallback for TCO / eval_data_as_code / any site that
    //      materializes without going through apply_closure
    //
    // Returns true if safe (no Moved captures), false if any
    // binding is Moved (bumps linear_ownership_violation_prevented;
    // callers take their safe path).
    //
    // Coordinated with #1475 epoch check and #1476
    // atomic_bump_epochs_and_stamp_bridge. JIT Apply path: #1540
    // aura_jit_linear_post_mutate_enforce host callback.
    bool linear_post_mutate_enforce(EnvId env_id) const noexcept;
    // Issue #1539: test/helper — mark most-recent binding as Moved on Env
    // and its parent_id EnvFrame (if registered), for use-after-move tests.
    bool mark_linear_binding_moved(Env& env, aura::ast::SymId s);
    bool mark_linear_binding_moved_by_name(Env& env, std::string_view name);
    // Issue #1538: sweep all live env frames with linear_post_mutate_enforce.
    // Used by the combined post-mutation linear pipeline (with
    // post_mutation_invariant_check) so typed_mutate surfaces both
    // type-checker OwnershipEnv notes and runtime env-frame enforcement.
    // Returns frames checked; all_safe is false if any frame fails.
    struct LinearPostMutateSweepResult {
        std::uint64_t frames_checked = 0;
        bool all_safe = true;
    };
    [[nodiscard]] LinearPostMutateSweepResult linear_post_mutate_enforce_all() const noexcept;
    // Issue #356: is_env_frame_invalid — true if the frame's
    // version_ has been marked INVALID_VERSION by a post-rollback
    // invalidation pass. Distinct from is_env_frame_stale:
    //   - stale = version_ < current defuse_version_ (will be
    //     refreshed on next walk by refresh_stale_frame_in_walk)
    //   - invalid = version_ == INVALID_VERSION (terminal:
    //     NEVER refreshable, must skip)
    // Frames become invalid when they were allocated during a
    // doomed transaction that was rolled back via panic
    // checkpoint restore — their bindings may reference AST
    // nodes / pool strings that no longer exist. Returning
    // false for invalid ids is a safety net (NULL/OOB treated as
    // unusable). Issue #1754: prefer is_env_frame_invalid_id for the
    // NULL/OOB case when distinguishing "no frame" from "terminal
    // INVALID_VERSION on a live slot".
    [[nodiscard]] bool is_env_frame_invalid(EnvId id) const;
    // Issue #356: invalidate_post_rollback_env_frames — mark
    // doomed frames INVALID_VERSION without shrinking the deque
    // (helper retained for tests + pre-#1360 behavior).
    // Prefer truncate_env_frames_to_checkpoint() on restore.
    void invalidate_post_rollback_env_frames();
    // Issue #1360: actually shrink env_frames_ to
    // panic_safe_env_frames_size_. Append-only EnvId means
    // indices [0, checkpoint) stay valid; post-checkpoint
    // EnvIds become OOB and resolve to nullptr. Bumps
    // env_generation_ + envframe_truncate counters.
    // Issue #1739: also bumps bridge_epoch (via service hook)
    // when frames are dropped so cross-COW freshness checks
    // observe the truncate (same class as #1728 commit).
    // Returns number of frames dropped.
    std::size_t truncate_env_frames_to_checkpoint();
    // Issue #1386 / #1510 / #1526: compact env_frames_ arena — reclaim
    // stale frames (version_ < current defuse_version_) that are not
    // referenced by any live Closure. Builds a remap table, rewrites
    // Closure::env_id + EnvFrame::parent_id, optionally remaps IR
    // runtime_closures_ via install_compact_env_remap_fn, then under
    // compact_env_frames_lock_ atomically:
    //   1. bump defuse_version_ + bridge_epoch (+ AOT table via service)
    //   2. restamp survivor Closure::bridge_epoch (and IR via restamp hook)
    // so remapped env_id stays dual-check consistent with the new epoch
    // (no "fresh JIT epoch + dangling env_id" race).
    //
    // Returns the number of frames reclaimed (0 if nothing was
    // stale). Locking: compact_env_frames_lock_ first, then
    // env_frames_mtx_ unique; briefly takes closures_mtx_
    // (shared then unique) to read references + rewrite env_id.
    //
    // Concurrency contract: caller should serialize at the
    // workspace level — concurrent apply_closure is NOT fully safe
    // during a compact because Closure::env_id may be
    // temporarily inconsistent (the unique_lock on closures_mtx_
    // blocks new apply_closure lookups but does NOT block in-flight
    // ones that already hold a Closure copy). Documented in
    // issue #1386 / #1510.
    //
    // Replaces the legacy truncate_env_frames_to_checkpoint
    // for non-rollback reclamation: compact_env_frames is for
    // operator-driven reclamation in long-running processes.
    std::size_t compact_env_frames();
    // Issue #1360 / #1756: stable resolve — nullptr if id is NULL,
    // OOB, INVALID_VERSION, or (future) generation-mismatched free
    // slot. Prefer resolve_env_frame_detailed when the failure mode
    // matters (materialize / GC / diagnostics).
    [[nodiscard]] const EnvFrame* resolve_env_frame(EnvId id) const noexcept;
    [[nodiscard]] EnvFrame* resolve_env_frame_mut(EnvId id) noexcept;
    // Issue #1756: distinguish NULL / OOB / INVALID_VERSION / OK.
    [[nodiscard]] EnvFrameResolveResult resolve_env_frame_detailed(EnvId id) const noexcept;
    [[nodiscard]] EnvFrameResolveResultMut resolve_env_frame_mut_detailed(EnvId id) noexcept;
    [[nodiscard]] std::uint64_t env_generation() const noexcept { return env_generation_; }
    [[nodiscard]] std::uint64_t get_envframe_truncate_count() const noexcept {
        return envframe_truncate_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_envframe_truncated_frames() const noexcept {
        return envframe_truncated_frames_.load(std::memory_order_relaxed);
    }
    // Issue #1386: compact_env_frames observability accessors.
    [[nodiscard]] std::uint64_t get_envframe_compact_count() const noexcept {
        return envframe_compact_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_envframe_reclaimed_frames() const noexcept {
        return envframe_reclaimed_frames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_envframe_compact_closures_rewritten() const noexcept {
        return envframe_compact_closures_rewritten_.load(std::memory_order_relaxed);
    }
    // Accessor for the post-rollback invalidation count
    // (observability for (query:envframe-dualpath-stats)).
    // Defined near the other envframe_*_ accessors below.
    // Issue #355: refresh_stale_frame_in_walk — single source of
    // truth for the "saw a stale frame during a walk" pattern.
    // When a SoA parent walk encounters a frame whose version_ is
    // older than the current defuse_version_ snapshot, callers
    // invoke this helper to:
    //   1. bump the frame's version_ to silence future warnings,
    //   2. bump envframe_stale_refresh_count_ (the stats counter
    //      surfaced by (query:envframe-dualpath-stats)),
    //   3. emit the same [#242 warning] stderr message that
    //      materialize_call_env uses, gated behind
    //      AURA_VERBOSE_ENVFRAME so production runs + the default
    //      CI flow stay silent.
    // The helper takes the caller's site label (e.g.
    // "lookup_by_symid_chain") so the warning can disambiguate
    // which walker observed the staleness.
    //
    // Why a helper: lookup_by_symid_chain / walk_env_frame_roots /
    // Env::lookup_cell_ptr / Env::lookup_cell_index all need the
    // same behavior. Concentrating it here keeps the warning text
    // and stats accounting consistent across sites.
    //
    // Thread-safety: caller is expected to hold the shared
    // env_frames_mtx_ read lock for the duration of the frame
    // reference (same precondition as materialize_call_env).
    // The version_ mutation uses const_cast on the const ref, so
    // a concurrent writer holding the exclusive lock would race
    // — by design the exclusive lock excludes walker threads.
    void refresh_stale_frame_in_walk(EnvId id, const char* site) const;
    // Look up an EnvFrame by id. UB if id is invalid — prefer
    // resolve_env_frame() when the id may be post-truncate stale (#1360).
    const EnvFrame& env_frame(EnvId id) const pre(id != NULL_ENV_ID) { return env_frames_[id]; }
    EnvFrame& env_frame_mut(EnvId id) pre(id != NULL_ENV_ID) { return env_frames_[id]; }
    // Validity check (cheap). After #1360 truncate, post-checkpoint
    // EnvIds fail this check (OOB) instead of lingering as INVALID_VERSION.
    [[nodiscard]] bool is_valid_env_id(EnvId id) const {
        return id != NULL_ENV_ID && id < env_frames_.size();
    }
    // Acquire a shared_lock on the env_frames_ deque before
    // calling env_frame() / env_frame_mut() / iterating. This
    // prevents the main thread's alloc_env_frame from
    // reallocating the deque's map array (which would free the
    // pointer a reader is holding). See env_frames_mtx_ below
    // for the full rationale.
    [[nodiscard]] std::shared_mutex& env_frames_lock() const { return env_frames_mtx_; }
    // Number of live frames.
    [[nodiscard]] std::size_t env_frames_size() const { return env_frames_.size(); }
    // Issue #1384: test-only accessor for a frame by id. Returns
    // a const ref into env_frames_ under the shared lock so the
    // frame can't be reallocated out from under the caller.
    // Throws std::out_of_range on invalid id.
    [[nodiscard]] const aura::compiler::EnvFrame&
    env_frame_for_test(aura::compiler::EnvId id) const {
        std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
        if (id == aura::compiler::NULL_ENV_ID || id >= env_frames_.size())
            throw std::out_of_range("env_frame_for_test: invalid id");
        return env_frames_[id];
    }

    // Issue #1385: snapshot env_frames_ metrics into a
    // CompilerMetrics struct. O(N) iteration under shared lock
    // for stale count; O(1) for size. Safe to call from any
    // thread. Does NOT touch arena metrics — that's the caller's
    // job (CompilerService owns the arena + its counting MR).
    void refresh_env_arena_metrics(CompilerMetrics& m) const {
        m.env_frames_size_total.store(env_frames_.size(), std::memory_order_relaxed);
        std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
        auto current = defuse_version_.load(std::memory_order_acquire);
        std::size_t stale = 0;
        for (const auto& fr : env_frames_) {
            if (fr.version_ < current)
                ++stale;
        }
        m.env_frames_stale_count.store(stale, std::memory_order_relaxed);
    }
    // Issue #205: walk env_frames_ and collect pair/closure
    // indices reachable through env bindings. The GC calls
    // this (via the env_walk callback) to discover roots
    // that are only reachable through env parent chains.
    //
    // Algorithm: linear pass over env_frames_ (O(frames)).
    // For each frame, walk bindings_ + bindings_symid_ and
    // extract pair/closure ref indices. The parent_id chain
    // is implicit: every frame in env_frames_ is reachable
    // from some root (the workspace), so we just scan all
    // bindings of all frames.
    //
    // The walk is purely SoA: env_frames_ is a std::vector,
    // bindings_/bindings_symid_ are std::vector, refs are
    // 64-bit tagged pointers. No pointer chase, no
    // recursive descent, no risk of stack overflow.
    void walk_env_frame_roots(std::vector<std::int64_t>& pair_roots_out,
                              std::vector<std::int64_t>& closure_roots_out) const;

    // Issue #206: remap table + resolve_X helpers.
    //
    // When the GC sweeps a heap (e.g., after marking), it
    // compacts the arena: live objects are moved to the
    // front, freed objects leave a hole. The OLD id of a
    // live object now points to either a different object
    // (if the compact moved it) or a hole (if it was freed).
    //
    // The remap table is the mechanism for resolving old
    // ids to new ids:
    //   - remap_[old_idx] = new_idx (if old was live, moved)
    //   - remap_[old_idx] = -1 (if old was freed)
    //
    // resolve_X(old_id) consults the remap table and returns
    // the new id, or -1 if the old id was freed.
    //
    // The remap is REBUILT on each compact (the table
    // reflects the most recent compact). Before any compact,
    // the table is empty and resolve_X returns the input
    // (identity mapping, since no compact has happened yet).
    //
    // Currently: only pairs have a remap table (the test
    // focuses on pairs). Other heaps (cells, closures,
    // strings) follow the same pattern; their remap is a
    // follow-up.
    [[nodiscard]] std::int64_t resolve_pair(std::uint64_t old_id) const noexcept {
        if (pair_remap_.empty())
            return static_cast<std::int64_t>(old_id);
        if (old_id >= pair_remap_.size())
            return -1;
        return pair_remap_[old_id];
    }
    [[nodiscard]] std::size_t pair_remap_size() const noexcept { return pair_remap_.size(); }
    // Issue #206: compact the pairs_ arena. `live_mask[i]`
    // is true if pairs_[i] is live (should be kept). Pairs
    // not in live_mask are removed, and the remap table is
    // built:
    //   pair_remap_[old_idx] = new_idx (if live_mask[old_idx])
    //   pair_remap_[old_idx] = -1       (otherwise)
    //
    // The arena is shrunk to fit the live pairs.
    // Returns the number of pairs after compact.
    //
    // If pair_remap_ is empty after compact (size 0
    // because live_mask was all-false), resolve_X returns
    // the input identity (since remap is empty).
    //
    // If live_mask is empty (no entries), the compact is
    // a no-op and pair_remap_ is rebuilt as identity
    // (all pairs treated as live).
    [[nodiscard]] std::int64_t compact_pairs(const std::vector<bool>& live_mask);
    void clear_pair_remap() noexcept { pair_remap_.clear(); }
    // Walk the parent chain starting from `start`, calling
    // `f(EnvId, const EnvFrame&)` for each frame including
    // `start`. Stops when `f` returns false (early exit) or the
    // chain ends (parent_id == NULL_ENV_ID). Pure index walk —
    // no pointer chase, no cache-unfriendly hop.
    //
    // Phase C4 / Issue #1753: visitor must return something
    // convertible to bool (true = continue walk, false = stop).
    // Prefer static_assert over a `requires` clause so a wrong F
    // signature yields a clear diagnostic (and the constraint is
    // not solely a C++20 concept gate). Project build is C++26
    // (CMAKE_CXX_STANDARD 26); zero runtime cost.
    template <typename F> void walk_env_frames(EnvId start, F&& f) const pre(start != NULL_ENV_ID) {
        // Issue #1753: explicit invocable+bool-return check.
        static_assert(std::is_invocable_v<F, EnvId, const EnvFrame&>,
                      "walk_env_frames: F must be invocable with (EnvId, const EnvFrame&)");
        static_assert(std::is_convertible_v<std::invoke_result_t<F, EnvId, const EnvFrame&>, bool>,
                      "walk_env_frames: F must return a type convertible to bool "
                      "(true=continue, false=stop)");
        EnvId cur = start;
        while (cur != NULL_ENV_ID) {
            // Issue #1360 / #1756: skip on NULL/OOB/INVALID_VERSION
            const auto r = resolve_env_frame_detailed(cur);
            if (!r)
                return;
            if (!std::forward<F>(f)(cur, *r.frame))
                return;
            cur = r.frame->parent_id;
        }
    }
    // Introspection: number of frames in the parent chain
    // starting at `start`. Useful for GC profiling and tests.
    [[nodiscard]] std::size_t env_depth(EnvId start) const pre(start != NULL_ENV_ID) {
        std::size_t depth = 0;
        walk_env_frames(start, [&](EnvId, const EnvFrame&) {
            ++depth;
            return true;
        });
        return depth;
    }
    // Look up a SymId across the full parent chain starting
    // at `start`. Returns the first match (closest frame
    // wins; shadowing semantics match Env::lookup_by_symid).
    // Demonstrates the SoA infra: walks via env_frames_ index
    // lookup, not pointer chase.
    //
    // P0 (EnvFrame SoA migration): cell sentinels returned by
    // EnvFrame local lookups are resolved here against the
    // Evaluator's central cells_ pmr vector. EnvFrame is now
    // free of raw heap pointers. Legacy Env still uses its
    // cells_ pointer during transition.
    std::optional<types::EvalValue> lookup_by_symid_chain(EnvId start, aura::ast::SymId s) const
        pre(start != NULL_ENV_ID);
    // Bulk reset (testing + GC integration). Clears env_frames_
    // but does NOT free the modules_ Env* array (those live in
    // module arenas, lifetime managed separately).
    void reset_env_frames() { env_frames_.clear(); }

private:
    ClosureId next_id() { return next_id_++; }
    [[nodiscard]] std::size_t alloc_cell(const types::EvalValue& v) {
        // Issue #1397: push_back + size read MUST be atomic so the
        // returned index is stable across concurrent `fiber:spawn`
        // workers (which share the Evaluator). std::lock_guard RAII
        // releases the mutex at scope exit; the duration here is
        // bounded to the vector reallocation + the size_t return.
        std::lock_guard lock(alloc_storage_lock_);
        cells_.push_back(v);
        return cells_.size() - 1;
    }
    // Forward decl: MemoryPolicy is defined further down in the class
    // (after the long state-decl block). The build_policy_hash helper
    // (which needs it as a parameter) is also defined further down.
    struct MemoryPolicy;
    // Build a 6-key policy hash (for set-memory-policy and get-memory-policy).
    // Member function (not a local lambda) so it has proper lifetime when
    // captured by std::function in the primitive table. A captured local
    // lambda would dangle as soon as the enclosing function returns.
    [[nodiscard]] types::EvalValue build_policy_hash(const MemoryPolicy& p);
    // Issue #263: build lifecycle/validation stats hash. Member function
    // (not a local lambda) so std::function-captured primitives do not
    // hold a dangling reference to a stack-local helper.
    [[nodiscard]] types::EvalValue
    build_ast_lifecycle_hash(std::span<const std::pair<std::string, types::EvalValue>> kv);
    // (apply_closure and expand_macro removed — use eval_flat directly)
    [[nodiscard]] EvalValue ast_to_data(const aura::ast::FlatAST& flat,
                                        const aura::ast::StringPool& pool, aura::ast::NodeId nid);
    [[nodiscard]] ast::NodeId data_to_flat(const types::EvalValue& data, aura::ast::FlatAST& flat,
                                           aura::ast::StringPool& pool, int depth = 0);
    [[nodiscard]] EvalResult eval_data_as_code(const types::EvalValue& data, const Env& env,
                                               aura::ast::FlatAST* flat = nullptr,
                                               aura::ast::StringPool* pool = nullptr);
    Env* copy_env(const Env& env, ast::ASTArena* target = nullptr)
        pre(target != nullptr); // arena_ is private; impl also asserts via contract_assert
    void init_pair_primitives();
    void register_all_primitives();
    void install_defuse_subsystem();
    void build_primitive_slots();
    // Issue #697: post-registration SV/EDA PrimMeta backfill.
    void backfill_eda_sv_primitive_meta();
    // Issue #1416: post-registration tier-assignment for the 7 EDSL
    // escape-hatch primitives (Part 4 #1396) so the dispatch-site
    // capability gate in invoke_prim_with_telemetry can deny
    // unauthorized calls. All 7 are set to kPrimSecPrivileged
    // (require kCapWildcard to invoke).
    void backfill_capability_tiers();
    // Dynamic ADT ctor registration (define-type eval path).
    void register_adt_ctor(const std::string& ctor_name, types::EvalValue tag_str, int field_count);
    [[nodiscard]] types::EvalValue make_adt_zero_arg_ctor(types::EvalValue tag_str);
    // Callback passed to primitives_detail::register_* helpers.
    // Issue #1439: *-stats names go to ObservabilityPrims internal table only
    // (not public Primitives / api-reference). Call sites should use
    // ObservabilityPrims::register_stats_impl; this intercept is a safety net.
    [[nodiscard]] std::function<void(std::string, PrimFn)> prim_registrar() {
        return [this](std::string name, PrimFn fn) {
            if (primitives_detail::ObservabilityPrims::is_legacy_stats_name(name)) {
                primitives_detail::ObservabilityPrims::register_stats_impl(std::move(name),
                                                                           std::move(fn));
                return;
            }
            primitives_.add(std::move(name), std::move(fn));
        };
    }
    // Issue #709: meta-aware registrar for EDA/SV primitives with DEFINE_PRIMITIVE_META.
    [[nodiscard]] std::function<void(std::string, PrimFn, PrimMeta)> prim_registrar_with_meta() {
        return [this](std::string name, PrimFn fn, PrimMeta meta) {
            if (primitives_detail::ObservabilityPrims::is_legacy_stats_name(name)) {
                (void)meta; // stats are internal; PrimMeta not published
                primitives_detail::ObservabilityPrims::register_stats_impl(std::move(name),
                                                                           std::move(fn));
                return;
            }
            primitives_.add(std::move(name), std::move(fn), std::move(meta));
        };
    }

public:
    // Load a module file, return module object (or void on failure).
    // Public for direct Evaluator access (tests + future
    // evaluator-driven tooling). Aura primitive `(load-module ...)` is
    // the user-facing surface; this is the underlying implementation.
    types::EvalValue load_module_file(const std::string& path);
    // Resolve a module path (supports AURA_PATH, .aura extension).
    std::string resolve_module_path(const std::string& path) const;

private:
    // Centralized tagged error pair builder ("error" . ("kind" . "message")).
    // Replaces the ~14 duplicated local `auto merr = [this](...)` lambdas
    // (see docs/contributing.md §3). Body implemented in evaluator_adt.cpp.
    // Added in refactor Step 0.1 (pure addition, no call-site changes yet).
    [[nodiscard]] EvalValue make_merr(const std::string& k, const std::string& m);

    Env top_;
    Primitives primitives_;
    // Issue #1663: serializes set_arena / set_temp_arena (rare path).
    // Pair with ASTArena::owner_mtx_ for allocate_raw atomicity.
    mutable std::mutex arena_set_mtx_;
    ast::ASTArena* arena_ = nullptr;
    ast::ASTArena* temp_arena_ = nullptr;
    // Owned multi-arena manager. Created in ctor so load_module_file can
    // hand each module its own arena without depending on a caller setting
    // it up. Lives for the Evaluator's whole lifetime.
    std::unique_ptr<ast::ArenaGroup> arena_group_;
    ast::FlatAST* mutate_target_flat_ =
        nullptr; // for mutate:* primitives (set via set_flat_pool or eval_flat)
    ast::StringPool* mutate_target_pool_ = nullptr;
    ast::FlatAST* workspace_flat_ = nullptr; // EDSL persistent workspace (set via (set-code ...))
    // Issue #1419 / #1730: current AI agent fingerprint (0 = system).
    // atomic: concurrent set/get across fibers (mutate:set-agent-fingerprint).
    std::atomic<std::uint64_t> current_agent_fingerprint_{0};
    ast::StringPool* workspace_pool_ = nullptr;
    ast::FlatAST* current_flat_ =
        nullptr; // per-eval source-being-evaluated (set by CompilerService eval paths)
    ast::StringPool* current_pool_ = nullptr;
    // Issue #211: (tag, arity) index for the query:pattern
    // primitive. Built on demand, cached for the lifetime
    // of the workspace (invalidated when workspace_flat_
    // is changed via set_workspace_flat). The index lets
    // the matcher skip nodes whose (tag, arity) doesn't
    // match the pattern's root — a massive speedup for
    // patterns where the root's tag+arity is rare.
    //
    // Keyed by (tag, arity) pair. tag is a NodeTag enum
    // value (cast to uint8_t); arity is the children
    // count. The value is a vector of NodeIds whose node
    // has the matching (tag, arity).
    //
    // The index is per-evaluator (process-wide), not
    // per-primitive-call. Building it on first use and
    // reusing it across calls is the optimization.
    //
    // Issue #271: incremental maintenance. After the
    // initial O(N) build, append-only growth is O(ΔN)
    // and post-mutation sync keys off FlatAST generation
    // + dirty_ instead of clearing the whole map.
    mutable std::unordered_map<std::uint64_t, std::vector<aura::ast::NodeId>> tag_arity_index_;
    // Reverse map: node-id → indexed (tag, arity) key.
    mutable std::vector<std::uint64_t> tag_arity_indexed_key_;
    // Issue #1501: user-only (non-MacroIntroduced) parallel index so
    // query:pattern hygiene default serves O(1) buckets without
    // scanning MacroIntroduced roots out of the full (tag,arity) map.
    mutable std::unordered_map<std::uint64_t, std::vector<ast::NodeId>> tag_arity_index_user_;
    // flat.size() / generation captured at last sync.
    mutable std::size_t tag_arity_index_synced_size_ = 0;
    mutable std::uint16_t tag_arity_index_synced_gen_ = 0;
    // The workspace pointer the index was built for.
    // When this changes, the index must be rebuilt.
    mutable const ast::FlatAST* tag_arity_index_workspace_ = nullptr;
    // Issue #1501: # of times snapshot served the user-only hygiene index.
    mutable std::atomic<std::uint64_t> tag_arity_hygiene_index_served_total_{0};
    // Issue #371: shared_mutex around the (tag, arity)
    // index. Reader (query:pattern fast path read in
    // evaluator_primitives_query_workspace.cpp,
    // tag_arity_index_size/entry_count accessors) takes a
    // shared_lock; writer (build_tag_arity_index, all the
    // insert/remove/rebuild/append/prune/sync helpers, and
    // invalidate_tag_arity_index) takes a unique_lock.
    //
    // Without this lock, a fiber mutator's
    // invalidate_tag_arity_index() can .clear() the
    // std::unordered_map while another fiber iterating the
    // same map in query:pattern races on the hash table —
    // UB that surfaces as ASan SEGV / heap-use-after-free
    // on fuzz. Locking discipline mirrors closures_mtx_ /
    // workspace_mtx_: writers exclusive, readers concurrent,
    // never both inside the same task at once.
    mutable std::shared_mutex tag_arity_index_mtx_;
    // Issue #1372: bumped on every successful build/sync under
    // unique_lock. Snapshot path records epoch at copy time;
    // a mismatched epoch would mean a concurrent writer raced
    // the lock (should be impossible) — counted as race hit.
    mutable std::atomic<std::uint64_t> tag_arity_index_epoch_{0};
    mutable std::atomic<std::uint64_t> tag_arity_index_race_window_hits_{0};
    // Build (or rebuild) the index for the current
    // workspace. Called by query:pattern (and other
    // future matchers) before walking.
    void build_tag_arity_index(std::uint8_t trigger = static_cast<std::uint8_t>(
                                   PatternIndexRebuildTrigger::LazyQuery)) const;
    // Assumes tag_arity_index_mtx_ already held exclusively.
    void build_tag_arity_index_unlocked(std::uint8_t trigger) const;
    void maybe_eager_rebuild_pattern_index_after_cow() const noexcept;
    void bump_pattern_index_rebuild_trigger(std::uint8_t trigger) const noexcept;
    void tag_arity_index_insert_node(const ast::FlatAST& flat, ast::NodeId id) const;
    void tag_arity_index_remove_node(ast::NodeId id) const;
    void tag_arity_index_rebuild_full(const ast::FlatAST& flat) const;
    void tag_arity_index_append_nodes(const ast::FlatAST& flat, std::size_t from_id) const;
    void tag_arity_index_prune_stale_entries(const ast::FlatAST& flat) const;
    void tag_arity_index_sync_after_mutation(const ast::FlatAST& flat) const;
    // Drop the index (called on workspace changes).
    // Issue #371: take unique_lock on tag_arity_index_mtx_.
    // The helpers (build_tag_arity_index, insert/remove/
    // rebuild/append/prune/sync_after_mutation) likewise
    // take unique_lock at entry — once a writer is in,
    // nested helpers do not re-acquire (assume already held).
    void invalidate_tag_arity_index() const {
        std::unique_lock<std::shared_mutex> wlock(tag_arity_index_mtx_);
        tag_arity_index_.clear();
        tag_arity_index_user_.clear();
        tag_arity_indexed_key_.clear();
        tag_arity_index_workspace_ = nullptr;
        tag_arity_index_synced_size_ = 0;
        tag_arity_index_synced_gen_ = 0;
        tag_arity_index_epoch_.fetch_add(1, std::memory_order_release);
    }
    // Issue #912: typed pointer preferred; void* kept for set_type_registry
    // ABI with CompilerService (core::TypeRegistry lives in another module).
    void* type_registry_ = nullptr; // points to aura::core::TypeRegistry
    // Issue #911: true when type_registry_ was allocated by this Evaluator.
    bool owns_type_registry_ = false;
    std::unordered_map<ClosureId, Closure> closures_;
    // Issue #145 P0 follow-up: shared_mutex protects closures_
    // against concurrent access from fiber threads (e.g. a
    // std::thread spawned by fiber:spawn calling apply_closure)
    // and the main thread (mutate, define, eval). Without this
    // lock, concurrent insert/erase + lookup races corrupt the
    // hash table and the read returns NULL pointers (ASan
    // SEGV at address 0x8 in the test_issue_164 heap trace).
    // Reader (apply_closure, materialize_call_env) takes a
    // shared lock; writer (closures_[cid] = ...) takes unique.
    //
    // Issue #1664 — dual-lock order when BOTH are held:
    //   1. closures_mtx_  (shared or unique)
    //   2. env_frames_mtx_ (shared)
    // Matches scan_live_closures_for_linear_captures / apply_closure.
    // NEVER acquire env_frames_mtx_ then closures_mtx_ (deadlock footgun).
    // Exception: compact_env_frames holds compact_env_frames_lock_ first,
    // then env unique, then briefly closures (documented there).
    mutable std::shared_mutex closures_mtx_;
    ClosureBridgeFn closure_bridge_;
    // Issue #252: optional pointer to CompilerMetrics for
    // closure_* counter increments. nullptr = counters
    // disabled (Evaluator constructed without service
    // orchestration; e.g. legacy standalone usage).
    void* compiler_metrics_ = nullptr;
    // Issue #1746: process-unique Evaluator identity for TLS maps
    // (mutation_boundary_depth_slot). Assigned once in ctor; never 0
    // after construction (counter starts at 1).
    std::uint64_t instance_id_ = 0;
    // Issue #1563: when true, bump_jit_deopt_on_mutate uses render throttle
    // even outside an active render hotpath frame (render-critical mutate).
    mutable std::atomic<bool> prefer_render_critical_deopt_throttle_{false};
    // Issue #1564: default AutoRefreshOnBoundary for ensure_valid_or_refresh.
    mutable std::atomic<bool> stable_ref_auto_refresh_policy_{true};
    // Issue #1357: per-slot render prim latency + frame mark.
    // unique_ptr: PrimLatencyStats holds atomics (not movable for vector reallocation).
    std::vector<std::unique_ptr<aura::compiler::render_telemetry::PrimLatencyStats>> prim_latency_;
    std::chrono::steady_clock::time_point last_frame_mark_{};
    ModuleLoadedFn module_loaded_cb_;
    std::unordered_map<std::string, MacroDef> macros_;
    std::vector<Env*> modules_; // module objects (arena-allocated, indexed by ModuleRef.index)
    std::unordered_map<std::string, std::uint64_t> module_cache_; // path → index
    std::unordered_set<std::string> loading_stack_;               // circular dep detection
    std::vector<std::string> module_names_;                       // display names for modules
    std::unordered_map<std::string, ast::ASTArena*>
        module_arena_ptrs_; // path → owning arena (for gc_module)
    // Issue #145 Phase 2.4 — runtime arena for high-churn
    // heap vectors. monotonic_buffer_resource bump-allocates
    // chunks; deallocate() is a no-op (monotonic semantics).
    // Sized to fit typical eval sessions (~1M cells/pairs +
    // ~10k strings) without falling back to upstream
    // (new_delete_resource). The buffer is freed wholesale at
    // Evaluator destruction (after the pmr vectors are
    // cleared, which runs ~string on each stored string).
    //
private:
    // Declared BEFORE cells_/pairs_/string_heap_ so the
    // pmr::vector members can take its address in their
    // initializer (member init order matches declaration
    // order).
    std::pmr::monotonic_buffer_resource runtime_resource_;
    std::pmr::vector<types::EvalValue> cells_{&runtime_resource_};
    std::pmr::vector<Pair> pairs_{&runtime_resource_};

public:
    // Issue #1397: uniform mutex guarding `cells_` / `pairs_` /
    // `string_heap_` push_back + size reads + indexed mutations.
    // `fiber:spawn` shares the Evaluator between the spawning thread
    // and the worker thread; without this guard, `pairs_.push_back`
    // + `pairs_.size()-1` index return, `set-car!` /
    // `set-cdr!` element writes, and `string_heap_.push_back` race
    // (lost updates, iterator invalidation mid-iter, torn pair slots).
    // See https://github.com/cybrid-systems/aura/issues/1397 for the
    // full audit + AC. Opted for Option A (uniform mutex) over
    // Option B (per-thread arena) / Option C (doc-only) for the
    // simple, correctness-first closure. Hot path overhead is bounded
    // to the lock_guard acquire/release (sub-100ns); Phase 2 may
    // revisit with Option B's per-thread arena if profiling demands.
    // Public: the register_*_primitives free functions in
    // evaluator_primitives_*.cpp need to acquire this lock on
    // push_back / set-car! / set-cdr! paths to keep the Aura
    // primitive surface thread-safe under fiber:spawn.
    //
    // recursive_mutex: ast_to_data holds the lock while converting a
    // Call/Begin/... and recursively converts nested Call children
    // that re-enter the same lock (and the cd() helper also locks).
    // A plain mutex deadlocks on quoted nested calls such as
    // '(f (g x)), '(unquote (+ 1 2)), ``(,',(+ 1 2)) — which hung
    // suite/ast_viz and fuzz seed edge_qq_nested (Issue #1397 lock
    // added without same-thread re-entrancy).
    mutable std::recursive_mutex alloc_storage_lock_;
    // Issue #1401: serializes load_module_file ↔ compact_env_frames().
    // compact_env_frames (Issue #1386) re-packs env_frames_ and
    // rewrites Closure::env_id via a remap table. load_module_file
    // allocates fresh env_frames_ + adds new closures_ for the
    // loaded module. Without this interlock, a concurrent
    // compact_env_frames could miss freshly-added closures (Step 2
    // walk) and reclaim frames the loader is about to use (Step 3
    // pack), producing torn env_id remap + iterator invalidation.
    //
    // Lock order: compact_env_frames_lock_ is acquired FIRST in both
    // call sites, before env_frames_mtx_ / workspace_mtx_. No
    // caller holds any other lock before taking this one, so the
    // canonical order is `compact_env_frames_lock_ → env_frames_mtx_
    // | workspace_mtx_`, and these two functions cannot interleave
    // (mutex-exclusive). Regular std::mutex (not shared) — both
    // paths are exclusive and rare (module reload / env-frame GC
    // are not hot paths), so we serialize fully rather than allow
    // concurrent reloads.
    // recursive: nested (require ...) re-enters load_module_file (deps);
    // plain mutex hung suite/ast_viz (ast-viz → list/string). Circular
    // deps still blocked via loading_stack_.
    mutable std::recursive_mutex compact_env_frames_lock_;
    // Issue #145 Phase 2.1 — SoA arena for Env. Frames are
    // appended in alloc_env_frame and indexed by EnvId. Frame
    // data is parallel to Env; parent walks go via parent_id_
    // (index) instead of Env::parent_ (pointer).
    //
    // Issue #145 P0 follow-up: deque (not vector) so that
    // push_back doesn't invalidate element references. This
    // matters because materialize_call_env takes a
    // `const EnvFrame&` reference and the closure body executes
    // on a fiber thread (T27 in the ASan trace). A vector
    // push_back on the main thread (T0) can reallocate the
    // buffer, freeing the frame the fiber thread is still
    // reading. deque grows in chunks without invalidating
    // element references (BUT the map array itself can still
    // be reallocated when the deque grows past the current
    // map capacity — see env_frames_mtx_ below).
    std::deque<EnvFrame> env_frames_;
    // Issue #145 P0 follow-up: shared_mutex protects
    // env_frames_ against concurrent access from fiber threads
    // (e.g. a std::thread spawned by fiber:spawn calling
    // materialize_call_env -> env_frame) and the main thread
    // (alloc_env_frame). Without this lock, deque's map array
    // reallocation on push_back can free the map-pointer that
    // a fiber thread is reading via env_frame[id], causing a
    // heap-use-after-free. Reader (env_frame, env_frame_mut,
    // materialize_call_env, lookup_by_symid_chain) takes a
    // shared lock; writer (alloc_env_frame, alloc_env_frame_
    // from_env) takes unique.
    // Lock-order contract (Issue #1388): acquire ONLY after
    // mutate_mtx_ + workspace_mtx_. dep_graph_mtx_ acquires
    // AFTER this one. Canonical order is mutate → workspace →
    // env_frames → dep_graph. Reverse order is NOT allowed.
    //
    // Issue #1664 — when held together with closures_mtx_:
    // always take closures_mtx_ FIRST, then env_frames_mtx_
    // (see closures_mtx_ comment). Solo env_frames acquires OK.
    mutable std::shared_mutex env_frames_mtx_;
    // Issue #206: pair remap table. Rebuilt by compact_pairs().
    // pair_remap_[old_idx] = new_idx (live, moved) or -1 (freed).
    // Empty means no compact has happened yet (identity mapping).
    std::vector<std::int64_t> pair_remap_;
    std::vector<types::EvalValue> error_values_; // error cause values (indexed by ErrorRef)
    std::vector<void*> opaque_heap_;             // opaque pointers (indexed by OpaqueRef)
    // Issue #131: FFI state moved to FFIRuntime instance
    // (formerly file-scope statics in the monolithic evaluator TU).
    FFIRuntime ffi_runtime_;
    // Step 2.3: ADT state moved to AdtRuntime (exact same pattern).
    AdtRuntime adt_runtime_;

public:
    // Step 2.3 follow-up: expose AdtRuntime to Env (which holds
    // a back-pointer via owner_). Env::lookup needs to check
    // the ADT constructor table as a fallback after primitives
    // and parent lookups. The field stays private to preserve
    // encapsulation; the accessor returns a const reference
    // so Env can only read.
    const AdtRuntime& adt_runtime() const { return adt_runtime_; }

private:
    std::unique_ptr<std::unordered_set<std::string>> current_export_set_;
    // ── Strategy storage (E2) ──────────────────────────────────
    // Issue #63 Phase 3: extend with tunable fields.
    // `max_attempts_set`/`temperature_set` distinguish "not specified"
    // (-1 sentinel) from "explicitly 0" (also possible after evolve).
    struct StrategyDef {
        std::string name;
        std::string body;                // strategy body as S-expression string
        int max_attempts = 3;            // tunable: 1..20
        double temperature = 0.3;        // tunable: 0.0..1.0
        std::string sys_prompt_template; // tunable: free-form
        int evolution = 0;               // generation counter
        std::string parent;              // parent strategy name
    };
    std::vector<StrategyDef> strategies_;
    // Issue #1720 / #1722: concurrent fiber access to strategies_ /
    // active_strategy_ (define-strategy, strategy-field, evolve-strategy, …).
    mutable std::shared_mutex strategies_mtx_;
    // ── Intend history (E4 Phase 1) ────────────────────────────
    struct IntendRecord {
        std::uint64_t record_id;
        std::string strategy_name;
        std::string task_desc;
        bool success;
        int attempts;
        std::vector<std::string> errors;
        std::vector<std::string> error_types;
        std::vector<std::string> generated_codes;
        std::uint64_t llm_call_count;
        std::uint64_t llm_tokens;
        std::uint64_t duration_ms;
        std::uint64_t timestamp;
        std::uint64_t parent_record_id;
    };
    std::vector<IntendRecord> intend_history_;
    mutable std::shared_mutex intend_history_mtx_; // Issue #1720
    std::uint64_t next_record_id_ = 1;
    static constexpr std::size_t MAX_HISTORY_SIZE = 1000;
    // Issue #1720: concurrent-safe timeline / intend-history API.
    void timeline_clear();
    void timeline_push(std::string line);
    [[nodiscard]] std::string timeline_snapshot() const;
    [[nodiscard]] std::string timeline_tail(std::size_t max_n) const;
    void intend_history_push(IntendRecord rec);
    // ── Coverage counters (fuzz Phase 3) ──────────────────────
    // 0=parser, 1=typecheck, 2=eval, 3=jit, 4=macro, 5=edsl-set-code,
    // 6=edsl-query, 7=edsl-mutate, 8=ffi, 9-15=reserved
    std::array<std::uint64_t, 16> coverage_counters_ = {};
    // ── Workspace Tree (P13) ───────────────────────────────────
    void* workspace_tree_ = nullptr;   // WorkspaceTree*
    bool workspace_read_only_ = false; // quick lock flag for P6 mutations
    // ── Issue #373: hygiene guard for mutate:* on MacroIntroduced
    // nodes. When false (default), every mutate:* entry point
    // pre-checks the target node's SyntaxMarker and returns a
    // ("hygiene-protected" "...") error pair if the target was
    // produced by clone_macro_body. Setter is exposed via the
    // (hygiene:set-allow-macro-mutate!) Aura primitive so
    // self-mod / AI-agent code can opt out globally. Per-mutate
    // opt-out via :allow-macro? #t kwarg bypasses the flag
    // without changing global state. Both opt-outs preserve the
    // existing mutate:* behavior (no behavior change for tests
    // that don't pass :allow-macro? or set the flag).
    bool allow_macro_mutate_ = false;
    // ── CompilerService pointer (for messaging) ─────────────────
    void* compiler_service_ = nullptr; // CompilerService*
    WorkspaceAdtSyncFn workspace_adt_sync_fn_ = nullptr;
    // Issue #223: function pointer that returns the service's
    // current bridge epoch. Set by CompilerService on
    // set_compiler_service() so Evaluator can query the epoch
    // without a circular include of service.ixx.
    BridgeEpochFn bridge_epoch_fn_ = nullptr;
    // Issue #1510: service-side bridge_epoch bump after compact.
    BridgeEpochBumpFn bridge_epoch_bump_fn_ = nullptr;
    // Issue #1510: optional external (IR) env_id remap under compact lock.
    CompactEnvRemapFn compact_env_remap_fn_ = nullptr;
    CompactEnvRestampFn compact_env_restamp_fn_ = nullptr;
    void* compact_env_remap_ctx_ = nullptr;
    CompilerGcRootsFlushFn compiler_gc_roots_fn_ = nullptr;
    // Function pointer callbacks (set by CompilerService to avoid circular deps)
    std::function<bool(const std::string&, const std::string&)>* msg_send_fn_ = nullptr;
    std::function<std::optional<std::string>(int)>* msg_recv_fn_ = nullptr;
    std::function<std::string()>* msg_id_fn_ = nullptr;
    std::string session_id_; // from CompilerService (for my-id)


    // ── Snapshot storage (ast:snapshot / ast:restore) ───────────
    std::vector<std::string>
        snapshot_sources_; // source code per snapshot (for ast:diff + source-fallback restore)
    std::vector<std::string> snapshot_names_; // optional names

    // Direct FlatAST snapshots (Issue #107 part 6). Each entry owns
    // a deep copy of the workspace's FlatAST and StringPool at the
    // time of snapshot. ast:restore prefers this over re-parsing
    // from source — it preserves all metadata (mutation_log_,
    // type_id_, value_cache_) that a re-parse would lose, and
    // avoids reparsing the source string. The source is still
    // stored (snapshot_sources_) for ast:diff and as a fallback
    // when a direct snapshot is missing (e.g. older snapshots
    // taken before part 6).
    struct FlatSnapshot {
        std::unique_ptr<aura::ast::FlatAST> flat;
        std::unique_ptr<aura::ast::StringPool> pool;
        bool has_flat = false; // true if both flat + pool are valid
        // Issue #263: metadata captured at snapshot time for restore checks.
        std::uint16_t flat_generation = 0;
        std::size_t flat_size = 0;
        std::uint64_t cow_epoch = 0;
    };
    std::vector<FlatSnapshot> snapshot_flats_;
    // Issue #263: last post-restore validation result (0 = consistent).
    std::size_t last_post_restore_violations_ = 0;
    aura::ast::PostRestoreReport last_post_restore_report_{};

    // Hot-swap callback storage (Issue #97 Action 1)
    HotSwapFn hot_swap_fn_;

    // ── Auto-evolve state (Issue #97 Action 2) ──────────────
    // Background loop state for (auto-evolve-loop ...).
    // The two Aura callbacks (detect-fn, fix-fn) are stored as closure IDs
    // and invoked via apply_closure.
    bool auto_evolve_running_ = false;
    double auto_evolve_interval_ = 1.0;            // seconds between cycles
    std::uint64_t auto_evolve_detect_closure_ = 0; // 0 = unset
    std::uint64_t auto_evolve_fix_closure_ = 0;
    std::uint64_t auto_evolve_cycle_count_ = 0;
    std::uint64_t auto_evolve_total_fixed_ = 0;

    // Issue #444: active mutation strategy name.
    // Empty = no strategy selected yet (the controller
    // starts in coverage-greedy by default). Updated by
    // (strategy:set-strategy name) and read by
    // (query:strategy-evolution-stats).
    std::string active_strategy_;

    // ── Panic auto-rollback (Issue #39) ─────────────────────────
    bool panic_auto_rollback_ = false;
    std::string panic_safe_source_; // last known good source code
    // Issue #1489: true while process-wide gc_hooks defer depth was
    // armed for this evaluator's live PanicCheckpoint (save→commit/restore).
    bool gc_defer_armed_for_panic_cp_ = false;

    // Issue #753: long-running resource quota limits (0 = unlimited).
    // resource_quota_memory_ = per-request size cap (#1481).
    // resource_quota_memory_total_ = cumulative arena.used + request (#1498).
    std::uint64_t resource_quota_memory_ = 0;
    std::uint64_t resource_quota_memory_total_ = 0;
    std::uint64_t resource_quota_fibers_ = 0;
    std::uint64_t resource_quota_time_us_ = 0;
    // Issue #1547: mutation budget (0 = unlimited). Separate from
    // metrics.resource_quota_max_mutations (Agent-facing default display).
    std::uint64_t resource_quota_mutations_ = 0;
    std::atomic<std::uint64_t> mutation_quota_used_{0};

    // Issue #242 / #1360: panic checkpoint for append-only arenas.
    // save_panic_checkpoint() snapshots each size; on restore we
    // truncate cells_/pairs_/string_heap_/env_frames_ back.
    //
    // env_frames_ (#1360): EnvId is an append-only deque index.
    // End-truncating to panic_safe_env_frames_size_ keeps all
    // pre-checkpoint EnvIds valid (indices 0..N-1 unchanged).
    // Post-checkpoint EnvIds become OOB; resolve_env_frame →
    // nullptr (no UAF). Full free-list stable-id (slot+gen pack)
    // remains a follow-up if mid-arena reclaim is needed.
    std::size_t panic_safe_cells_size_ = 0;
    std::size_t panic_safe_pairs_size_ = 0;
    std::size_t panic_safe_string_heap_size_ = 0;
    std::size_t panic_safe_env_frames_size_ = 0;
    // Bumped each time env_frames_ is truncated on panic restore.
    std::uint64_t env_generation_ = 0;

    // ── EDSL set-code error propagation ──────────────────────────
    // Stores (kind, message) for structured diagnostic return
    std::string last_set_code_error_kind_;
    std::string last_set_code_error_msg_;
    // Issue #1381: last successful set-code source (for serialize-workspace).
    // Prefer get_workspace_source_fn_ when CompilerService is wired; this
    // fallback keeps bare Evaluator persistable.
    std::string workspace_source_text_;

    // Last mutate typecheck error (empty = no error). Set by auto-typecheck
    // after mutate:rebind etc. Cleared on next successful mutate.
    std::string last_mutate_error_;

    // ── Incremental eval cache ───────────────────────────────────
    // Caches the last eval-current result. Cleared when workspace dirty flags
    // are set (which happens on any mutation). This lets eval-current skip
    // full re-evaluation when nothing has changed.
    std::optional<types::EvalValue> last_eval_current_result_;
    // Issue #1441: invalidate eval-current cache when FlatAST generation
    // advances (e.g. try_rollback_rebind_op / structural rollback bumps
    // generation even if a subsequent clear_all_dirty left last_form clean).
    std::uint16_t last_eval_current_generation_ = 0;

    // ── Incremental typecheck cache (Issue #159 Phase 5) ──────
    // Caches the last typecheck-current result string. Used to skip the
    // full traversal when the workspace is clean (no dirty nodes).
    // Cleared on any mutation (via the dirty flag check).
    std::optional<std::string> last_typecheck_result_;

    // ── Def-Use Analysis (P1) ───────────────────────────────────
    void* defuse_index_ = nullptr;
    // Issue #184: defuse_version_ is std::atomic so other fibers /
    // threads can read it without a torn read, and so the version
    // bump performed by enter_mutation_boundary() is a release-store
    // that publishes prior mutation writes to any thread that
    // subsequently acquire-loads. The orderings are:
    //   - write (mutation): memory_order_release — the increment
    //     publishes all mutation writes to acquirers.
    //   - read (JIT deopt check, fiber resume check, etc.):
    //     memory_order_acquire — synchronizes with prior releases.
    //   - read for stats / debug: memory_order_relaxed is fine.
    std::atomic<std::uint64_t> defuse_version_{0};

public:
    // Issue #957: production-facing defuse_version accessor (AOT emit,
    // serve metrics, Agent probes). Acquire load — synchronizes with
    // MutationBoundaryGuard release stores.
    [[nodiscard]] std::uint64_t defuse_version() const noexcept {
        return defuse_version_.load(std::memory_order_acquire);
    }
    // Test-only aliases (stable name for existing tests).
    std::uint64_t defuse_version_for_test() const { return defuse_version(); }
    void bump_defuse_version_for_test() { defuse_version_.fetch_add(1, std::memory_order_acq_rel); }

    // Issue #266: stats from the most recent boundary exit(false).
    struct BoundaryRollbackStats {
        std::size_t field_records_rolled = 0;
        bool children_column_restored = false;
        bool sym_id_column_restored = false;
        bool param_columns_restored = false;
    };

    // Issue #490: pattern-index rebuild policy + trigger kinds.
    enum class PatternIndexPolicy : std::uint8_t {
        Lazy = 0,
        EagerAfterMutate = 1,
        EagerAfterCow = 2,
    };
    enum class PatternIndexRebuildTrigger : std::uint8_t {
        LazyQuery = 0,
        EagerMutate = 1,
        EagerCow = 2,
    };

private:
    // Issue #189: total mutations counter (for observability).
    // Bumped alongside defuse_version_ so dashboards can see
    // "how many mutations has this evaluator processed" without
    // having to interpret the version increment. Read via
    // `total_mutations()`. Relaxed-ordering for stats; not
    // used for control flow.
    std::atomic<std::uint64_t> total_mutations_{0};
    // Issue #895: pack atomic-batch counters on one cache line to
    // reduce false sharing vs adjacent panic/self-evolution domains.
    struct alignas(64) AtomicBatchDomain {
        std::atomic<std::uint64_t> count{0};
        std::atomic<std::uint64_t> ops_total{0};
        std::atomic<std::uint64_t> rollbacks{0};
        std::atomic<std::uint64_t> bumps_saved_total{0};
        std::atomic<std::uint64_t> cross_fiber_steals_total{0};
        std::atomic<std::uint64_t> hygiene_violations_total{0};
        std::atomic<std::uint64_t> in_fiber_total{0};
        std::atomic<std::uint64_t> pinned_refs_total{0};
        std::atomic<std::uint64_t> snapshot_rollbacks{0};
        std::atomic<std::uint64_t> snapshot_captures{0};
    };
    AtomicBatchDomain atomic_batch_domain_{};
    // Back-compat field names → domain members (reference aliases not
    // allowed for atomics; use macros-free accessors via domain only).
    // Call sites updated to atomic_batch_domain_.* (#895).
    // Issue #737: pinned StableNodeRef list (non-atomic).
    std::vector<aura::ast::FlatAST::StableNodeRef> atomic_batch_pinned_refs_{};
    std::int64_t last_atomic_batch_snapshot_id_ = -1;
    // Issue #453: panic checkpoint lifecycle metrics. Bumped by
    // the bridge hooks (g_transfer_panic_checkpoint, etc.) when
    // a checkpoint transfer or GC defer happens across a fiber
    // migration. Stats-only (relaxed-ordering). Exposed via
    // observability snapshot + (compile:panic-recovery-stats).
    // Public getters + bump accessors live in the public section
    // below (around line 1955, alongside #211 test accessors).
    // Issue #895 Phase 2: pack panic-checkpoint counters on one
    // cache line (separate domain from AtomicBatchDomain).
    struct alignas(64) PanicCheckpointDomain {
        std::atomic<std::uint64_t> transfer_count{0};
        std::atomic<std::uint64_t> lost_on_steal{0};
        std::atomic<std::uint64_t> gc_blocked_by_pending{0};
        std::atomic<std::uint64_t> save_count{0};
        std::atomic<std::uint64_t> restore_count{0};
        std::atomic<std::uint64_t> commit_count{0};
        std::atomic<std::uint64_t> size_mismatch{0};
        std::atomic<std::uint64_t> rollback_success_on_panic{0};
    };
    PanicCheckpointDomain panic_checkpoint_domain_{};
    // Issue #425 / #548: size-mismatch + rollback-on-panic live in
    // panic_checkpoint_domain_ (Issue #895 Phase 2 pack).
    // Issue #549: self-evolution-stability counters. Bumped
    // by validate_stable_ref + exit_mutation_boundary(false)
    // + fiber yield hooks. Stats-only (relaxed-ordering).
    // Exposed via the new
    // (query:self-evolution-stability-stats) primitive.
    //   - cross_cow_invalidations_  (# of StableNodeRef
    //     rejections caused by crossing a COW snapshot
    //     boundary — i.e. captured gen != current gen)
    //   - fiber_stale_ref_count_  (# of stale-ref detections
    //     where the captured gen is from a different fiber's
    //     workspace — surfaced via validate_stable_ref)
    //   - mutation_log_rollback_count_  (# of times
    //     exit_mutation_boundary(false) actually rolled back
    //     the log — i.e. there were mutations to undo)
    //   - provenance_mismatch_  (# of stable-ref checks
    //     where the captured provenance (origin layer)
    //     didn't match the current workspace layer)
    mutable std::atomic<std::uint64_t> cross_cow_invalidations_{0};
    mutable std::atomic<std::uint64_t> fiber_stale_ref_count_{0};
    mutable std::atomic<std::uint64_t> mutation_log_rollback_count_{0};
    mutable std::atomic<std::uint64_t> provenance_mismatch_{0};
    // Issue #550: incremental typed self-mod dirty + narrowing
    // observability counters. Exposed via
    // (query:typed-mutation-stats) + (query:dirty-impact)
    // primitives. Stats-only (relaxed-ordering). Currently
    // exposed as reachable + initialized to 0; the actual
    // wiring into TypeChecker / OccurrenceInfoFlat is the
    // follow-up.
    //   - narrowing_refresh_count_  (# of OccurrenceInfoFlat
    //     entries refreshed after a dirty propagation — the
    //     selective-recheck the review called out)
    //   - cross_delta_conflicts_caught_  (# of times
    //     touched_roots_ detected a CONFLICT between two
    //     delta batches that would have been missed by
    //     per-delta solving alone)
    //   - passes_skipped_type_dirty_  (# of clean Pass
    //     blocks skipped by the DirtyAwarePass short-circuit
    //     when type dirty is non-empty — the measurable
    //     latency win)
    //   - touched_roots_size_  (current size of the
    //     touched_roots_ set — a snapshot, not a counter)
    mutable std::atomic<std::uint64_t> narrowing_refresh_count_{0};
    mutable std::atomic<std::uint64_t> cross_delta_conflicts_caught_{0};
    mutable std::atomic<std::uint64_t> passes_skipped_type_dirty_{0};
    mutable std::atomic<std::uint64_t> touched_roots_size_{0};
    // Issue #551: reflect post-mutate observability counters.
    // Exposed via (query:reflect-postmutate-stats) primitive.
    // Stats-only (relaxed-ordering).
    //   - impact_snapshot_count_  (# of post-mutate impact
    //     snapshots produced by the Guard dtor success path
    //     — the data the AI loop reads for adaptive strategy)
    //   - schema_validation_pass_count_  (# of auto_validate
    //     calls that passed — the post-mutate structural
    //     integrity check)
    //   - schema_validation_fail_count_  (# of auto_validate
    //     calls that caught an inconsistency — production
    //     critical to detect silent corruption)
    //   - dirty_nodes_in_snapshot_  (# of dirty nodes captured
    //     in the latest impact snapshot — a per-snapshot stat
    //     that the AI loop reads to scope re-queries)
    mutable std::atomic<std::uint64_t> impact_snapshot_count_{0};
    mutable std::atomic<std::uint64_t> schema_validation_pass_count_{0};
    mutable std::atomic<std::uint64_t> schema_validation_fail_count_{0};
    mutable std::atomic<std::uint64_t> dirty_nodes_in_snapshot_{0};
    mutable std::atomic<std::uint64_t> macro_markers_in_snapshot_{0};
    // Issue #502: last post_mutation_reflect_validate() outcome (1=pass, 0=fail).
    mutable std::atomic<std::uint8_t> last_schema_validation_ok_{1};
    // Issue #555: Task1 typed self-mod observability counters.
    // Exposed via (query:typed-mutation-stats-task1) primitive.
    // Stats-only (relaxed-ordering).
    //   - dirty_propagation_count_  (# of times mark_dirty_upward
    //     walked an upward dirty chain — measures the dirty
    //     propagation throughput)
    //   - selective_recheck_count_  (# of selective re-narrows
    //     triggered by the Guard dtor (vs full re-solve))
    //   - touched_roots_conflict_count_  (# of times
    //     touched_roots_ detected a CONFLICT between two
    //     delta batches that would have been missed by
    //     per-delta solving)
    //   - guard_dirty_epoch_count_  (# of Guard dtor success
    //     paths that propagated dirty to the type cache
    //     generation — measures the Guard + type integration)
    mutable std::atomic<std::uint64_t> dirty_propagation_count_{0};
    mutable std::atomic<std::uint64_t> selective_recheck_count_{0};
    mutable std::atomic<std::uint64_t> touched_roots_conflict_count_{0};
    mutable std::atomic<std::uint64_t> guard_dirty_epoch_count_{0};
    // Issue #391: automatic staleness check observability.
    //   stale_ref_blocked_count_  (Strict policy blocked
    //     a mutate because a captured stable-ref was stale)
    //   stale_ref_warned_count_  (Warn policy observed a
    //     stale stable-ref but did not block)
    // Both stats-only (relaxed-ordering). Exposed via
    // (query:stale-ref-stats) primitive.
    std::atomic<std::uint64_t> stale_ref_blocked_count_{0};
    std::atomic<std::uint64_t> stale_ref_warned_count_{0};
    // Issue #489: raw NodeId vs StableNodeRef usage in mutate/query
    // primitive entry points (stats-only, relaxed-ordering).
    std::atomic<std::uint64_t> raw_nodeid_usage_in_primitives_count_{0};
    std::atomic<std::uint64_t> stable_ref_validated_in_primitives_count_{0};
    // Issue #438: fiber migration + work-stealing
    // observability. Bumped in
    // transfer_mutation_stack_to_current_fiber
    // (called by Fiber::resume() before migration) +
    // the scheduler work-steal path (follow-up).
    //   mutation_steal_attempts_  (lifetime # of steal
    //     attempts the scheduler logged)
    //   boundary_violation_count_  (lifetime # of
    //     attempts at an unsafe boundary that were
    //     either deferred or skipped)
    // Both stats-only (relaxed-ordering). Exposed via
    // the (query:fiber-migration-stats) primitive.
    mutable std::atomic<std::uint64_t> mutation_steal_attempts_{0};
    mutable std::atomic<std::uint64_t> boundary_violation_count_{0};
    // Issue #556: EDSL concurrency safety observability counters.
    // Exposed via (query:edsl-concurrency-stats) primitive.
    // Stats-only (relaxed-ordering).
    //   - unsafe_boundary_attempts_  (# of attempts at an
    //     unsafe boundary (steal during active outermost
    //     Guard, fiber yield during mutate, etc.) — should
    //     be 0 in production with proper locking)
    //   - lock_contention_us_         (lifetime microseconds
    //     spent waiting on workspace_mtx_ + Guard locks —
    //     the AI Agent reads this to compute contention_ratio
    //     and decide when to enable lockless fast paths)
    mutable std::atomic<std::uint64_t> unsafe_boundary_attempts_{0};
    mutable std::atomic<std::uint64_t> lock_contention_us_{0};
    // Issue #1504: first-class safe yield-at-boundary observability.
    //   safe_yield_ok_          — cooperative yield at depth==0 succeeded
    //   safe_yield_skipped_held_— refused because MutationBoundaryGuard held
    //   safe_yield_no_fiber_    — safe point observed but no fiber to yield
    mutable std::atomic<std::uint64_t> safe_yield_ok_total_{0};
    mutable std::atomic<std::uint64_t> safe_yield_skipped_held_total_{0};
    mutable std::atomic<std::uint64_t> safe_yield_no_fiber_total_{0};
    // Issue #439: GC safepoint + MutationBoundary
    // coordination observability. Bumped in
    // Fiber::check_gc_safepoint / request_gc_safepoint /
    // wait_for_safepoint.
    //   gc_safepoint_requests_total_  (lifetime # of
    //     safepoint requests)
    //   gc_safepoint_waits_total_  (lifetime # of wait
    //     completions)
    //   gc_safepoint_deferred_total_  (lifetime # of
    //     deferrals because a fiber held an outermost
    //     MutationBoundary guard)
    // All stats-only (relaxed-ordering). Exposed via
    // the (query:gc-safepoint-stats) primitive.
    std::atomic<std::uint64_t> gc_safepoint_requests_total_{0};
    std::atomic<std::uint64_t> gc_safepoint_waits_total_{0};
    std::atomic<std::uint64_t> gc_safepoint_deferred_total_{0};
    // Issue #1490: lifetime # of post-steal EnvFrame refresh passes
    // (Fiber::resume migration + post-yield validate path).
    std::atomic<std::uint64_t> post_steal_refresh_count_{0};
    // Issue #1612: refresh_stale_macro_frames invocation count.
    std::atomic<std::uint64_t> macro_refresh_invoke_count_{0};
    // Issue #439: safepoint wait time (sum of all
    // wait_for_safepoint call durations, in ns). P0
    // returns 0; the follow-up adds the actual
    // wait_for_safepoint implementation that
    // records the duration.
    std::atomic<std::uint64_t> gc_safepoint_wait_total_ns_{0};
    // Issue #443: external simulator tool-calling
    // observability. Bumped in
    // (verify:run-external-sim) /
    // (verify:parse-coverage) /
    // (verify:parse-failures) primitives.
    //   verify_tool_calls_total_  (lifetime # of
    //     run-external-sim calls)
    //   verify_tool_cache_hits_total_  (lifetime # of
    //     cache hits on (cmd, generation_) lookup)
    //   verify_tool_parse_errors_total_  (lifetime # of
    //     parse errors in cov-data / fail-data)
    // All stats-only (relaxed-ordering). Exposed via
    // the (query:verify-tool-stats) primitive.
    std::atomic<std::uint64_t> verify_tool_calls_total_{0};
    std::atomic<std::uint64_t> verify_tool_cache_hits_total_{0};
    std::atomic<std::uint64_t> verify_tool_parse_errors_total_{0};
    // Issue #710: Guard/StableRef/dirty propagation in verify_tool + diagnostic.
    std::atomic<std::uint64_t> verify_tool_guard_captures_total_{0};
    std::atomic<std::uint64_t> verify_tool_dirty_propagations_total_{0};
    std::atomic<std::uint64_t> verify_tool_stable_ref_hits_total_{0};
    std::atomic<std::uint64_t> verify_tool_feedback_mutate_success_total_{0};
    // Issue #443: result cache keyed by (cmd, gen).
    // Bounded LRU (P0: 64 entries, FIFO eviction; the
    // follow-up uses a proper LRU). Used by
    // (verify:run-external-sim) to cache results so
    // repeated calls in the same generation don't
    // re-execute the tool.
    struct VerifyToolCacheEntry {
        std::string cmd;
        std::uint16_t gen;
        std::string result;
    };
    // Issue #443: result cache (P0 = private field, only
    // accessible via friend register_verify_tool_primitives).
    std::deque<VerifyToolCacheEntry> verify_tool_cache_;
    static constexpr std::size_t kVerifyToolCacheMax = 64;
    // Issue #391: stale-ref policy. Default 'warn' for
    // production AI agent loops (logs the violation but
    // does not block). Use 'strict' for safety-critical
    // contexts; 'disabled' for raw-NodeId paths.
    enum class StaleRefPolicy : std::uint8_t {
        Disabled = 0,
        Warn = 1,
        Strict = 2,
    };
    StaleRefPolicy stale_ref_policy_ = StaleRefPolicy::Warn;
    PatternIndexPolicy pattern_index_policy_ = PatternIndexPolicy::Lazy;
    mutable std::atomic<std::uint64_t> pattern_index_lazy_rebuilds_{0};
    mutable std::atomic<std::uint64_t> pattern_index_eager_mutate_rebuilds_{0};
    mutable std::atomic<std::uint64_t> pattern_index_eager_cow_rebuilds_{0};
    // Issue #1503: Lazy policy auto-syncs when index is already warm
    // (post first query:pattern) so mutate→query stays incremental.
    mutable std::atomic<std::uint64_t> pattern_index_auto_warm_syncs_{0};
    // Issue #448: mutation coordination observability. Bumped
    // by the scheduler / fiber hooks when:
    //   - a work-steal attempt is deferred or skipped
    //     because the victim fiber is in an unsafe
    //     MutationBoundary state
    //   - a GC safepoint request is deferred because an
    //     outermost MutationBoundary guard is held
    //   - a scheduler waits for a fiber to reach a safe
    //     mutation boundary (sum of wait times, in ns)
    //
    // P0: only the counter fields + bump accessors
    // ship. The actual scheduler/GC wiring (the
    // production coordination logic) is a follow-up.
    std::atomic<std::uint64_t> mutation_steal_violation_count_{0};
    std::atomic<std::uint64_t> gc_blocked_by_mutation_boundary_{0};
    std::atomic<std::uint64_t> safepoint_mutation_wait_total_ns_{0};
    // Issue #543: dual-path / version / stale observability.
    // See the public-section comment above for the bump
    // semantics. All relaxed-ordering (stats-only).
    mutable std::atomic<std::uint64_t> envframe_desync_detected_{0};
    // Issue #485: dual-path bindings_ vs bindings_symid_ length
    // matched (consistency probe succeeded).
    mutable std::atomic<std::uint64_t> bindings_dual_sync_count_{0};
    mutable std::atomic<std::uint64_t> envframe_stale_refresh_count_{0};
    mutable std::atomic<std::uint64_t> envframe_version_mismatch_in_walk_{0};
    mutable std::atomic<std::uint64_t> envframe_gc_walk_safe_skips_{0};
    // Issue #1903: dual-path consistency enforcement counters.
    //   - envframe_dual_consistency_asserted_: # of frames where
    //     EnvFrame::ensure_dual_path_consistent() ran (every
    //     bind/bind_symid + post-steal refresh + post-materialize).
    //   - envframe_post_steal_dual_synced_: # of frames where dual-
    //     path consistency was restored after a Fiber::resume / steal
    //     cycle (complete_post_resume_steal_refresh call site).
    //   - envframe_materialize_consistency_checks_: # of materialize_
    //     call_env invocations that explicitly asserted consistency
    //     after the bindings copy (post-copy under Guard).
    //   - envframe_gc_walk_legacy_fallback_uses_: # of GC walk frames
    //     where bindings_symid_ was empty and walk fell back to the
    //     legacy bindings_ vector (legacy pool-less frames only).
    mutable std::atomic<std::uint64_t> envframe_dual_consistency_asserted_{0};
    mutable std::atomic<std::uint64_t> envframe_post_steal_dual_synced_{0};
    mutable std::atomic<std::uint64_t> envframe_materialize_consistency_checks_{0};
    mutable std::atomic<std::uint64_t> envframe_gc_walk_legacy_fallback_uses_{0};
    // Issue #356: # of env_frames_ entries marked INVALID_VERSION
    // by invalidate_post_rollback_env_frames after a panic
    // checkpoint restore. Stats-only (relaxed-ordering).
    // Surfaced by (query:envframe-dualpath-stats).
    mutable std::atomic<std::uint64_t> envframe_post_rollback_invalidations_{0};
    // Issue #1360: truncate_env_frames_to_checkpoint observability.
    mutable std::atomic<std::uint64_t> envframe_truncate_count_{0};
    mutable std::atomic<std::uint64_t> envframe_truncated_frames_{0};
    // Issue #1386: compact_env_frames observability. Distinct
    // from truncate (operator-driven vs rollback-driven).
    // envframe_compact_count_: number of compact_env_frames()
    // calls that reclaimed at least 1 frame. envframe_reclaimed_
    // frames_: total reclaimed frames across all compacts.
    // envframe_compact_closures_rewritten_: total Closure::env_id
    // rewrites (verify env_id walk touched the expected count).
    mutable std::atomic<std::uint64_t> envframe_compact_count_{0};
    mutable std::atomic<std::uint64_t> envframe_reclaimed_frames_{0};
    mutable std::atomic<std::uint64_t> envframe_compact_closures_rewritten_{0};
    // Issue #458: query hygiene metrics. Bumped by query:pattern
    // (and friends) when they skip a MacroIntroduced node during
    // traversal. Stats-only (relaxed-ordering). Exposed via
    // the new `query:hygiene-stats` primitive.
    std::atomic<std::uint64_t> hygiene_violation_count_{0};
    // Issue #422: blocked mutate attempts on MacroIntroduced
    // nodes (hygiene_protected_error + replace-subtree gate).
    std::atomic<std::uint64_t> hygiene_violation_attempts_{0};
    std::atomic<std::uint64_t> macro_introduced_skipped_in_query_{0};
    std::atomic<std::uint64_t> total_query_calls_{0};
    // Issue #478: primitive-layer error counter. Bumped by
    // primitives_detail::make_primitive_error when a hotspot
    // primitive returns an error_values_ entry.
    std::atomic<std::uint64_t> primitive_error_count_{0};
    // Issue #480: primitive metadata query counters.
    std::atomic<std::uint64_t> primitive_describe_count_{0};
    std::atomic<std::uint64_t> primitive_list_meta_count_{0};
    // Issue #456: mutation-impact observability. Bumped in
    // exit_mutation_boundary (success path) when the outermost
    // guard flushes. The deltas track: how many nodes were
    // touched, what reasons were seen, and the epoch delta
    // (defuse_version_ increment since boundary entry).
    // Exposed via (query:mutation-impact) + (query:epoch-stats).
    std::atomic<std::uint64_t> mutation_impact_count_{0};
    std::atomic<std::uint64_t> mutation_impact_nodes_changed_total_{0};
    std::atomic<std::uint64_t> mutation_impact_reasons_seen_mask_{0};
    // Issue #456: small ring buffer (8 entries) of the most
    // recent successful mutation-impact summaries. Written
    // under workspace_mtx_ (outermost guard) + relaxed-atomic
    // flag for new-data notification. Used by
    // (query:mutation-impact) to return the head entry as an
    // int. P0: each entry is a 4-tuple of (count, delta,
    // reasons_mask, seq); we encode as 1 int = total
    // impact_events_count for the simplest P0 contract.
    // Follow-up: return 4-tuple / list.
    struct MutationImpactEntry {
        std::uint64_t epoch_after;
        std::uint64_t epoch_delta;
        std::uint64_t nodes_changed;
        std::uint8_t reasons_mask;
    };
    static constexpr std::size_t kMutationImpactRingSize = 8;
    std::array<MutationImpactEntry, kMutationImpactRingSize> mutation_impact_ring_{};
    std::atomic<std::uint64_t> mutation_impact_ring_seq_{0};
    std::atomic<std::uint64_t> last_queried_epoch_{0};
    // Issue #266: fine-grained SoA rollback request + stats.
    bool fine_rollback_for_next_boundary_ = false;
    BoundaryRollbackStats last_boundary_rollback_stats_{};
    // Issue #164: per-join defuse_version_ snapshot. Set at the
    // start of fiber:join's wait, re-checked at wakeup to detect
    // mutations that happened DURING the join (the "transient
    // inconsistency" the issue calls out). 0 means "no active
    // wait" (fast-path or never-wait joins). Per-fiber; not atomic
    // (owning fiber reads/writes; the wakeup check happens on the
    // same fiber's worker thread). Issue #184 Cycle 3 may promote
    // this to atomic if cross-fiber resume becomes a thing.
    std::uint64_t defuse_version_at_wait_ = 0;
    // Issue #264: yield-boundary + compaction observability.
    std::atomic<std::uint64_t> mutation_yield_count_{0};
    std::atomic<std::uint64_t> compaction_paused_by_boundary_{0};
    std::atomic<std::uint64_t> cross_fiber_rollback_count_{0};
    // Issue #417: cross-TU MutationBoundaryGuard + per-fiber
    // stack / depth-slot invariant drift (stack.empty() vs
    // mutation_boundary_depth_slot == 0). Bumped by
    // ensure_mutation_invariants() on Guard dtor and
    // materialize_call_env hot paths.
    mutable std::atomic<std::uint64_t> total_invariant_violations_{0};
    // Issue #679: nested Guard + atomic-batch rollback alignment.
    mutable std::atomic<std::uint64_t> nested_guard_depth_max_{0};
    mutable std::atomic<std::uint64_t> suppressed_misalign_caught_{0};
    mutable std::atomic<std::uint64_t> macro_rollback_hits_{0};
    // Issue #420: MacroIntroduced marker vs macro_dirty_
    // (kMacroExpansion) end-to-end contract drift. Bumped by
    // ensure_macro_hygiene_contract() on eval-current and
    // query-side probes.
    mutable std::atomic<std::uint64_t> macro_hygiene_contract_violations_{0};
    // Issue #421: query:pattern recursive MacroIntroduced filter
    // observability (post query-split hygiene contract).
    std::atomic<std::uint64_t> pattern_recursive_macro_skipped_{0};
    mutable std::atomic<std::uint64_t> pattern_macro_filter_violations_{0};
    // Issue #423: query:pattern structural (tag, arity) pre-index
    // fast-path observability on the Evaluator-side index.
    std::atomic<std::uint64_t> pattern_structural_index_hits_{0};
    std::atomic<std::uint64_t> pattern_structural_index_misses_{0};
    mutable std::atomic<std::uint64_t> pattern_index_consistency_violations_{0};
    // Issue #424: StableNodeRef / WorkspaceTree cross-layer
    // COW consistency observability.
    std::atomic<std::uint64_t> stable_ref_workspace_resolves_{0};
    std::atomic<std::uint64_t> stable_ref_workspace_resolve_misses_{0};
    mutable std::atomic<std::uint64_t> stable_ref_workspace_tree_violations_{0};
    // ── Cross-fiber yield × MutationBoundaryGuard (Issue #1373) ──
    //
    // mutation_boundary_held_: set by the OUTERMOST
    // MutationBoundaryGuard ctor; cleared by that Guard's dtor.
    // Nested guards do not clear/reset the flag.
    //
    // Fiber::yield (src/serve/fiber.cpp) reads this via
    // messaging::g_mutation_boundary_held:
    //   - debug: assert(false) if true (yield-under-lock is a bug)
    //   - release: WARNING log, then continue
    //
    // Yield / restore paths (documented behavior):
    //
    // 1) Same-thread yield (Fiber::yield / fiber_resume):
    //    - Guard remains alive on the caller's stack frame.
    //    - workspace_mtx_ still held by this thread (unique_lock
    //      is thread-affine and not released by yield).
    //    - checkpoint_yield_boundary records defuse_version +
    //      depths; restore_post_yield_or_rollback typically
    //      returns true (no rollback).
    //    - Counter: mutation_boundary_yield_same_thread_total
    //
    // 2) Cross-thread migration (worker steal while boundary
    //    was active at yield):
    //    - DANGEROUS: shared_mutex unique_lock has thread affinity;
    //      the Guard must not be held across a true thread move
    //      of the lock owner.
    //    - restore_post_yield_or_rollback() detects thread_id
    //      mismatch on the yield checkpoint, restamps metadata,
    //      and may force *outermost_mutation_success_flag_ = false
    //      so the Guard dtor rolls back batched mutations when
    //      version/depth still drift after restamp (or panic path).
    //    - Counters: mutation_boundary_cross_thread_migration_total,
    //      mutation_boundary_yield_rollback_total (on rollback)
    //
    // 3) Fiber exit while Guard is live:
    //    - CRITICAL: RAII requires ~MutationBoundaryGuard to run
    //      before fiber storage is freed (stack-frame Guard).
    //    - Verified by tests/test_mutate_cross_thread_migration.cpp
    //
    // Production guidance:
    //   - Prefer not yielding inside mutate:* / Guard scopes.
    //   - Fiber pool may pin work to avoid path (2); agents should
    //     keep mutation sections short (see hold-time counters).
    //
    // Hold-time: outermost Guard records enter_ts_; dtor updates
    // mutation_boundary_hold_* / mutation_hold_* CompilerMetrics.
    std::atomic<bool> mutation_boundary_held_{false};
    // Set by outermost MutationBoundaryGuard; used by
    // restore_post_yield_or_rollback to signal rollback on
    // cross-thread migration / yield desync during an active boundary.
    bool* outermost_mutation_success_flag_ = nullptr;
    // (#10) Track mutation-affected symbols for targeted index rebuild
    // Mutation primitives push affected sym names here; ensure_defuse
    // uses them to avoid full rebuild when only a few symbols changed.
    std::unordered_set<std::string> defuse_affected_syms_;
    // (#10) Number of times the def-use index has been rebuilt (for stats)
    std::uint64_t defuse_rebuild_count_ = 0;
    // Issue #1255: precise incremental DefUse vs full-rebuild fallback.
    std::uint64_t defuse_incremental_updates_ = 0;
    std::uint64_t defuse_full_rebuild_fallbacks_ = 0;

    // ── 依赖图查询回调 ─────────────────────────────────────────
    // 在 mutation 原语中查询调用者节点，绕开 DefUseIndex 前向声明问题。
    // 在 init_pair_primitives 末尾（DefUseIndex 定义完成后）注册。
    // 签名: (defuse_index, sym_id) → [caller node IDs]
    // 用 std::function 而非函数指针，避免不完整类型问题。
    std::function<std::vector<aura::ast::NodeId>(void*, aura::ast::SymId)> dep_caller_fn_ = nullptr;

    // ── DefUseIndex per-sym version touch callback (#107 part 5) ───
    // 在 mutation 原语中调用，标记某个 sym 在 DefUseIndex 中为 stale。
    // 注册位置同 dep_caller_fn_，绕开 DefUseIndex 前向声明问题。
    // 签名: (defuse_index, sym_id) → void
    // 当 defuse_index_ 为 null 时回调内部应 no-op。
    std::function<void(void*, aura::ast::SymId)> defuse_touch_fn_ = nullptr;

    // ── EDSL IR cache V2 (Phase 2) hooks ─────────────────────────────
    // Function pointers set by CompilerService on init. Avoids
    // evaluator partition TUs needing to import CompilerService (circular).
    // mark_define_dirty_fn_(name)         → mark a single define's IR dirty
    // mark_all_defines_dirty_fn_()       → mark all cached defines dirty
    // invalidate_function_fn_(name)        → BFS re-lower + JIT eviction (#680)
    // define_impact_scope_fn_(node)      → ir_cache_pure impact_scope (#680)
    std::function<void(const std::string&)> mark_define_dirty_fn_ = nullptr;
    std::function<void()> mark_all_defines_dirty_fn_ = nullptr;
    std::function<void(const std::string&)> invalidate_function_fn_ = nullptr;
    std::function<void(aura::ast::NodeId)> define_impact_scope_fn_ = nullptr;
    std::atomic<std::uint64_t> precise_define_inval_hits_{0};

    // ── 模块类型签名（#8 跨模块类型检查） ──────────────────────
    // (declare-type "name" "param-types" "ret-type") 存储的签名，
    // 在 typecheck-current 时注入到类型环境中。
    // 格式: type_str = "param1 param2|rettype" | 分隔
    struct DeclaredType {
        std::string type_str;
        std::string module_file; // 来源模块文件（用于跨模块错误定位）
        bool resolved = false;
    };
    std::unordered_map<std::string, DeclaredType> declared_type_sigs_;

    // ── Functor 泛型模块模板 ────────────────────────────────────
    struct ModuleTemplate {
        std::string body_source;                   // body source code (re-parsed at instantiation)
        std::vector<std::string> type_param_names; // type parameter names (e.g., ["T", "K"])
        std::vector<std::string> cap_param_names;  // capability parameter names (e.g., ["cap"])
        std::vector<std::string>
            cap_require; // required capabilities (e.g., ["FileRead", "FileWrite"])
    };
    std::unordered_map<std::string, ModuleTemplate> module_templates_;

    // ── Functor 实例化缓存 ──────────────────────────────────────
    // key = "template_name|arg1|arg2|..."
    // value = 实例化后的 env（指针通过 module 索引引用）
    std::unordered_map<std::string, std::uint64_t> functor_instance_cache_;

    // ── Timeline for intend (E2, backward compat) ───────────────
    std::vector<std::string> timeline_;      //
    mutable std::shared_mutex timeline_mtx_; // Issue #1720
    // Issue #145 Phase 2.4 — pmr-backed (matches cells_/pairs_
    // backing). Vector metadata lives in runtime_resource_;
    // std::string char buffers still heap-allocate per string
    // (Phase 2.4.1 / Phase 2.5 will inline string contents
    // into the arena).
    std::pmr::vector<std::string> string_heap_{&runtime_resource_};
    // Short string cache: ≤6 byte strings are deduplicated via this hash
    // (avoids redundant string_heap_ pushes and enables faster equal?)
    std::unordered_map<std::string, types::EvalValue> short_str_cache_;
    std::vector<std::string> keyword_table_; // keyword name strings (indexed by KeywordRef)
    std::size_t eval_depth_ = 0;             // recursion counter for friendly stack overflow
    static constexpr std::size_t MAX_EVAL_DEPTH = 50000;

    // ── Memory pressure observability (Issue #69) ───────────────
    // eval_depth_ snapshot at the last (gc-temp) call. Used by
    // memory-pressure to decide whether to suggest "gc-temp" in the
    // suggestions vector (only if no recent gc-temp call, i.e.
    // eval_depth_ - last_gc_temp_eval_depth_ > 100).
    std::size_t last_gc_temp_eval_depth_ = 0;

    // ── Memory pressure governance (P1) ─────────────────────
    // Policy is configured by (set-memory-policy hash). Default: no
    // auto-gc, warn at 80%, critical at 95%, sample every 1000 evals,
    // cooldown 5000 evals between auto-gc firings, gc-temp "recent"
    // window 100 evals. The auto-gc fires (gc-module top-arena) when
    // overall used-pct >= critical-pct AND the cooldown has elapsed
    // since the last auto-gc.
    struct MemoryPolicy {
        bool auto_gc = false;
        int warn_pct = 80;
        int critical_pct = 95;
        std::size_t sample_every = 1000;
        std::size_t cooldown_evals = 5000;
        std::size_t recent_gc_temp_window = 100;
    } memory_policy_;
    // Issue #991: hot path uses thread_local copies (eval_flat);
    // these fields remain for policy-reset observers / single-thread
    // mirrors only — do not read them as the ground truth under
    // multi-worker serve.
    // Last eval_depth_ at which auto-gc fired (for cooldown).
    std::size_t last_auto_gc_eval_depth_ = 0;
    // Counter to implement "sample every N evals".
    std::size_t sample_counter_ = 0;
    // Last logged warning level (so we don't spam the same warning).
    std::string last_warn_level_;

    std::vector<std::vector<types::EvalValue>> vector_heap_;
    std::uint64_t next_id_ = 1;
    ClosureId gc_safe_closure_id_ = 0;
    // Issue #68: depth counter (was bool) so nested `intend` calls
    // correctly keep outer closures in the persistent arena. With
    // bool, an outer intend's closures could be allocated in the
    // temp arena when an inner intend flipped the flag, then
    // collected by gc-temp. With a counter, only depth > 0 (i.e.
    // inside at least one intend) routes to temp arena, and the
    // outer intend's depth-1 boundary still goes to persistent.
    int in_task_context_ = 0;

    // ── Capability 上下文栈 ─────────────────────────────────────
    // 每层包含当前作用域允许的 effect 名称列表
    std::vector<std::vector<std::string>> capability_stack_;
    // Issue #676: persistent grants (security:grant-capability!)
    std::vector<std::string> granted_capabilities_;
    // Issue #676: sandbox mode — when true, sensitive primitives
    // require matching capabilities (io/mutate/exec).
    bool sandbox_mode_ = false;
    // Issue #1565: multi-tenant id for capability effect checks.
    std::uint64_t capability_tenant_id_ = 0;
    static constexpr std::size_t kMutationAuditRingSize = 64;
    struct MutationAuditEntry {
        std::uint64_t seq = 0;
        std::uint64_t timestamp_ms = 0;
        std::int64_t fiber_id = 0;
        std::uint32_t nodes_changed = 0;
        std::uint32_t epoch_delta = 0;
        std::uint32_t target_node = 0;
        char op[48]{};
        // Issue #1565: capability effect + provenance audit fields
        std::uint16_t effect_bits = 0;
        std::uint64_t tenant_id = 0;
        std::uint64_t provenance_mutation_id = 0;
        bool effect_denied = false;
        // Issue #1567: bridge/provenance epoch at emit (additive, ring layout
        // compatible — new field at end).
        std::uint64_t epoch = 0;
    };
    std::array<MutationAuditEntry, kMutationAuditRingSize> mutation_audit_ring_{};
    std::atomic<std::uint64_t> mutation_audit_seq_{0};
    std::atomic<std::uint64_t> mutation_audit_total_{0};
    // Issue #1543: linear GC root audit ring (see run_linear_gc_root_audit).
    std::array<LinearGcRootAuditEntry, kLinearGcRootAuditRingSize> linear_gc_root_audit_ring_{};
    std::atomic<std::uint64_t> linear_gc_root_audit_seq_{0};
    std::atomic<std::uint64_t> linear_gc_root_audit_total_{0};
    // Last-seen counter snapshots for monotonicity checks across audits.
    std::uint64_t linear_gc_root_audit_prev_reg_{0};
    std::uint64_t linear_gc_root_audit_prev_stale_{0};
    std::uint64_t linear_gc_root_audit_prev_viol_{0};
    std::uint64_t linear_gc_root_audit_prev_resync_{0};
    // Issue #1568: linear violation provenance audit ring.
    std::array<LinearViolationAuditEntry, kLinearViolationAuditRingSize>
        linear_violation_audit_ring_{};
    std::atomic<std::uint64_t> linear_violation_audit_seq_{0};
    std::atomic<std::uint64_t> linear_violation_audit_total_{0};
    void record_linear_violation_audit(std::uint8_t path, std::uint8_t reason, std::uint32_t env_id,
                                       std::uint64_t closure_id) noexcept;
    std::atomic<std::uint64_t> capability_denial_count_{0};
    // Issue #1448: PrimMeta.deprecated dispatch-site hits (compat still runs).
    std::atomic<std::uint64_t> deprecated_prim_dispatch_total_{0};

    // ── Concurrent Channels (fiber-safe message passing) ─────
    struct Channel {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<std::string> queue;
        std::size_t buffer_size = 0; // 0 = rendezvous (unbuffered)
        bool closed = false;
    };
    std::vector<std::shared_ptr<Channel>> channels_;
    std::mutex channels_mtx_;

    // ── Heap mutex (P2 thread-safe GC) ────────────────────────
    // Protects string_heap_, pairs_, closures_, cells_,
    // vector_heap_, opaque_heap_, error_values_.
    // Locked during gc-heap and gc-temp operations.
    std::mutex heap_mtx_;
    std::mutex& heap_mutex() { return heap_mtx_; }

    // ── Issue #107: Workspace AST mutex (shared) ────────────────
    // Protects the FlatAST (workspace_flat_) and dependent caches
    // (defuse_index_, defuse_version_, dirty flags) against
    // concurrent mutate / query / typecheck in multi-agent
    // orchestration. The mutex is std::shared_mutex so multiple
    // readers (query:*, typecheck-current, eval_current) can
    // hold it simultaneously while a single writer (mutate:*)
    // holds it exclusively.
    //
    // Locking discipline:
    //   - mutate:* primitives   — std::unique_lock (exclusive)
    //   - query:* / typecheck    — std::shared_lock (shared)
    //   - eval_current / set-code — std::shared_lock (shared)
    //
    // The read-only-flag (`workspace_read_only_`) is an
    // additional layer: when set, mutate primitives return
    // "read-only" without acquiring the lock. This is checked
    // BEFORE lock acquisition to keep the no-op fast path.
    // Lock-order contract (Issue #1388): acquire ONLY after
    // mutate_mtx_. env_frames_mtx_ + dep_graph_mtx_ acquire
    // AFTER this one. Canonical order is mutate → workspace →
    // env_frames → dep_graph. Reverse order is NOT allowed.
    std::shared_mutex workspace_mtx_;

public:
    // Issue #373: hygiene guard accessors. The flag gates
    // mutate:* operations on MacroIntroduced nodes (those
    // produced by clone_macro_body from a hygienic macro
    // expansion). Default = false (safe); AI-agent / self-mod
    // code can opt in via (hygiene:set-allow-macro-mutate!).
    [[nodiscard]] bool get_allow_macro_mutate() const noexcept { return allow_macro_mutate_; }
    void set_allow_macro_mutate(bool v) noexcept { allow_macro_mutate_ = v; }
    // Issue #676: sandbox + capability model.
    [[nodiscard]] bool sandbox_mode() const noexcept { return sandbox_mode_; }
    void set_sandbox_mode(bool v) noexcept { sandbox_mode_ = v; }
    [[nodiscard]] bool has_capability(std::string_view cap) const noexcept;
    void grant_capability(std::string cap);
    void bump_capability_denial() noexcept {
        capability_denial_count_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t capability_denial_count() const noexcept {
        return capability_denial_count_.load(std::memory_order_relaxed);
    }
    // Issue #1448: how many times a PrimMeta.deprecated primitive was invoked.
    [[nodiscard]] std::uint64_t deprecated_prim_dispatch_total() const noexcept {
        return deprecated_prim_dispatch_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t mutation_audit_total() const noexcept {
        return mutation_audit_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t mutation_audit_seq() const noexcept {
        return mutation_audit_seq_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t granted_capability_count() const noexcept {
        return granted_capabilities_.size();
    }
    [[nodiscard]] const MutationAuditEntry&
    mutation_audit_entry_at(std::uint64_t seq) const noexcept {
        return mutation_audit_ring_[seq % kMutationAuditRingSize];
    }
    void emit_mutation_audit(std::uint32_t nodes_changed, std::uint32_t epoch_delta,
                             std::string_view op, ast::NodeId target_node) noexcept;
    // Issue #1567: optional mutation audit WAL (append + crash recovery).
    // Returns true if persist enabled (and replay applied when dir has data).
    bool enable_mutation_audit_wal(std::string_view persist_dir) noexcept;
    void disable_mutation_audit_wal() noexcept;
    [[nodiscard]] bool mutation_audit_wal_enabled() const noexcept;
    // Issue #1565: capability effect check + audit (returns true if allowed).
    // Integrates sandbox Strict/Restricted + grant matrix + provenance.
    [[nodiscard]] bool check_and_record_effect(std::uint16_t required_effect_bits,
                                               std::uint16_t actual_effect_bits,
                                               std::string_view op, ast::NodeId target_node = 0,
                                               std::uint64_t tenant_id = 0,
                                               std::uint64_t provenance_mutation_id = 0) noexcept;
    void grant_effect_capability(std::uint64_t tenant_id, std::string_view name,
                                 std::uint16_t effect_bits,
                                 std::uint64_t provenance_mutation_id = 0) noexcept;
    void set_effect_sandbox_mode(std::uint8_t mode) noexcept; // 0 Off, 1 Restricted, 2 Strict
    [[nodiscard]] std::uint8_t effect_sandbox_mode() const noexcept;
    void set_capability_tenant_id(std::uint64_t tenant) noexcept { capability_tenant_id_ = tenant; }
    [[nodiscard]] std::uint64_t capability_tenant_id() const noexcept {
        return capability_tenant_id_;
    }
    // Issue #1566: WorkspaceIsolationPolicy enforcement.
    void set_tenant_principal(std::uint64_t tenant_id, std::string_view name = {},
                              bool allow_cross = false) noexcept;
    void grant_cross_tenant_access(std::uint64_t from_tenant, std::uint64_t to_tenant,
                                   std::uint16_t effect_bits) noexcept;
    // Returns true if allowed. target_tenant=0 → use capability_tenant_id_.
    // ref_tenant=0 → no provenance stamp on ref.
    [[nodiscard]] bool check_workspace_isolation(std::uint64_t target_tenant = 0,
                                                 std::uint64_t ref_tenant = 0,
                                                 std::uint16_t required_effects = 0,
                                                 std::string_view op = "workspace") noexcept;
    // Stamp FlatAST::StableNodeRef.tenant_id from current principal.
    void stamp_ref_tenant(ast::FlatAST::StableNodeRef& ref) const noexcept;
    // Issue #211: test accessors for the (tag, arity) index.
    [[nodiscard]] std::size_t tag_arity_index_size() const noexcept {
        // Issue #371: shared_lock for read parity with
        // query:pattern. The unordered_map may be torn
        // down by an invalidate_tag_arity_index() call
        // on another fiber; without the lock .size()
        // races on the bucket pointer.
        std::shared_lock<std::shared_mutex> rlock(tag_arity_index_mtx_);
        return tag_arity_index_.size();
    }
    // Issue #453: panic checkpoint metric accessors. Public so
    // the bridge trampolines (in evaluator_fiber_mutation.cpp)
    // can read + bump them. Read is for observability; bump is
    // for the transfer / GC-defer paths.
    [[nodiscard]] std::uint64_t get_panic_checkpoint_transfer_count() const noexcept {
        return panic_checkpoint_domain_.transfer_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_panic_checkpoint_lost_on_steal() const noexcept {
        return panic_checkpoint_domain_.lost_on_steal.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_gc_blocked_by_pending_panic() const noexcept {
        return panic_checkpoint_domain_.gc_blocked_by_pending.load(std::memory_order_relaxed);
    }
    void bump_panic_checkpoint_transfer_count() noexcept {
        panic_checkpoint_domain_.transfer_count.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_panic_checkpoint_lost_on_steal() noexcept {
        panic_checkpoint_domain_.lost_on_steal.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_gc_blocked_by_pending_panic() noexcept {
        panic_checkpoint_domain_.gc_blocked_by_pending.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1489: arm/release process-wide GC defer while a PanicCheckpoint
    // is live (save → commit/restore). Idempotent per-evaluator (one arm per
    // armed window). Scheduler + compact_sweep consult gc_hooks depth.
    void arm_gc_defer_for_pending_panic() noexcept {
        if (gc_defer_armed_for_panic_cp_)
            return;
        gc_defer_armed_for_panic_cp_ = true;
        aura::gc_hooks::arm_gc_defer_pending_panic();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->gc_panic_pending_deferral_total.fetch_add(1, std::memory_order_relaxed);
    }
    void release_gc_defer_for_pending_panic() noexcept {
        if (!gc_defer_armed_for_panic_cp_)
            return;
        gc_defer_armed_for_panic_cp_ = false;
        aura::gc_hooks::release_gc_defer_pending_panic();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->gc_panic_conflict_resolved_total.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] bool gc_defer_armed_for_pending_panic() const noexcept {
        return gc_defer_armed_for_panic_cp_;
    }
    // Issue #548: panic-checkpoint lifecycle counters
    // + bump helpers. Public so the
    // (query:panic-checkpoint-lifecycle-stats) primitive
    // can read them, and save_panic_checkpoint /
    // restore_panic_checkpoint / commit_panic_checkpoint
    // (the follow-up wires these to the actual save/restore
    // call sites).
    [[nodiscard]] std::uint64_t get_panic_checkpoint_save_count() const noexcept {
        return panic_checkpoint_domain_.save_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_panic_checkpoint_restore_count() const noexcept {
        return panic_checkpoint_domain_.restore_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_panic_checkpoint_commit_count() const noexcept {
        return panic_checkpoint_domain_.commit_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_rollback_success_on_panic() const noexcept {
        return panic_checkpoint_domain_.rollback_success_on_panic.load(std::memory_order_relaxed);
    }
    // Issue #592: arena size mismatch counter observable from tests.
    [[nodiscard]] std::uint64_t get_panic_checkpoint_size_mismatch() const noexcept {
        return panic_checkpoint_domain_.size_mismatch.load(std::memory_order_relaxed);
    }
    void bump_panic_checkpoint_save_count() noexcept {
        panic_checkpoint_domain_.save_count.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_panic_checkpoint_restore_count() noexcept {
        panic_checkpoint_domain_.restore_count.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_panic_checkpoint_commit_count() noexcept {
        panic_checkpoint_domain_.commit_count.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #425: bumped by restore_panic_checkpoint when the
    // recorded checkpoint size exceeds the current size (the
    // snapshot was taken in a different transactional state
    // than the restore — likely fiber yield + nested Guard
    // stack drift). The fix is a future issue; for now this
    // counter is the observability hook. The post-truncate
    // skip is the safe behavior (don't corrupt the arena).
    void bump_panic_checkpoint_size_mismatch() noexcept {
        panic_checkpoint_domain_.size_mismatch.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_rollback_success_on_panic() noexcept {
        panic_checkpoint_domain_.rollback_success_on_panic.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #441 (rolled into #450): hot-path primitive
    // dispatch counter. Called from the primitive-dispatch
    // path in evaluator_eval_flat.cpp. No-op when
    // compiler_metrics_ is unset (zero overhead in that
    // case). The counter is used by
    // (query:primitive-perf-stats).
    inline void bump_primitive_call_count() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->primitive_call_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Issue #1357: invoke PrimFn; latency is recorded by hot PrimFn wrappers
    // installed in finalize_hot_table() (covers IR + tree-walker).
    // Issue #1416: capability gate based on PrimMeta.security_level.
    // On dispatch we look up the primitive's security tier and gate
    // the call against the evaluator's capability set. kPrimSecPrivileged
    // requires kCapWildcard, kPrimSecSandboxed requires kCapSandbox,
    // kPrimSecSafe (default) and 0/unknown pass through. The gate
    // lives here (vs inside each primitive body) so individual primitives
    // don't each re-implement the same check (DRY) and so that
    // tier-assignment can be done post-registration via
    // set_meta_for_name (used by backfill_capability_tiers in
    // evaluator_ctor.cpp to tier-assign the 7 EDSL escape hatches).
    // The O(1) name → slot → meta lookup is only paid for the
    // gate path (skipped when name is empty or security_level is 0).
    template <typename Call>
    inline types::EvalValue invoke_prim_with_telemetry(std::string_view name, Call&& call) {
        bump_primitive_call_count();

        if (!name.empty()) {
            const auto slot = primitives_.slot_for_name(name);
            if (slot < primitives_.slot_count()) {
                const auto& meta = primitives_.meta_for_slot(slot);
                // Issue #1676: render-critical / rendering+hot fast path —
                // trusted tier skips capability gate + deprecation tax.
                // Security is enforced at sandbox boundary; present/draw
                // must not re-pay slot security checks every frame.
                if (is_render_critical_meta(meta)) {
                    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                        m->render_hotpath_dispatch_fast_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                    }
                    return call();
                }
                // Issue #1448: PrimMeta.deprecated → dispatch-site warning.
                // Still executes (compat), but bumps a counter so agents
                // and (engine:metrics) can see remaining debt. Prefer
                // op-dispatch / engine:metrics / stdlib over deprecated names.
                if (meta.deprecated) {
                    deprecated_prim_dispatch_total_.fetch_add(1, std::memory_order_relaxed);
                    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                        m->prim_write_side_deprecation_hits.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (meta.security_level == kPrimSecPrivileged &&
                    !has_capability(security::kCapWildcard)) {
                    bump_capability_denial();
                    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                        m->capability_compile_denials.fetch_add(1, std::memory_order_relaxed);
                        m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
                    }
                    return primitives_detail::make_primitive_error(
                        string_heap_, error_values_,
                        "capability denied: privileged primitive requires kCapWildcard",
                        primitive_error_counter_ptr());
                }
                if (meta.security_level == kPrimSecSandboxed &&
                    !has_capability(security::kCapSandbox)) {
                    bump_capability_denial();
                    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                        m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
                    }
                    return primitives_detail::make_primitive_error(
                        string_heap_, error_values_,
                        "capability denied: sandboxed primitive requires kCapSandbox",
                        primitive_error_counter_ptr());
                }
                // Cold / non-render-critical while a frame is in flight.
                if (aura::core::arena_policy::in_render_hotpath()) {
                    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                        m->render_hotpath_dispatch_full_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                    }
                }
            }
        }

        return call();
    }

    // Issue #1676: dual-epoch + linear fence at TUI/render primitive entry
    // (parity with Apply prologue, without O(frames) full sweep).
    // Enforces linear_post_mutate on the newest live EnvFrame and refreshes
    // stale frames. Returns false only when linear Moved is observed
    // (present still proceeds; callers use return for audit).
    [[nodiscard]] bool fence_render_hot_entry() const noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
            m->render_hotpath_linear_fence_total.fetch_add(1, std::memory_order_relaxed);
            m->render_hotpath_epoch_fence_total.fetch_add(1, std::memory_order_relaxed);
        }
        // Epoch half: sample bridge epoch so agents can correlate fence vs bump.
        (void)current_bridge_epoch();

        EnvId hint = NULL_ENV_ID;
        {
            std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
            if (!env_frames_.empty()) {
                for (std::size_t i = env_frames_.size(); i > 0; --i) {
                    const EnvId id = static_cast<EnvId>(i - 1);
                    if (env_frames_[id].version_ != INVALID_VERSION) {
                        hint = id;
                        break;
                    }
                }
            }
        }
        if (hint == NULL_ENV_ID)
            return true;

        bool ok = true;
        if (is_env_frame_invalid(hint) || is_env_frame_stale(hint)) {
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                m->render_hotpath_epoch_stale_total.fetch_add(1, std::memory_order_relaxed);
            }
            // Recover: re-stamp so subsequent materialize does not walk pre-mutate frame.
            if (is_valid_env_id(hint) && !is_env_frame_invalid(hint))
                refresh_stale_frame_in_walk(hint, "render_hot_entry");
        }
        if (!linear_post_mutate_enforce(hint)) {
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                m->render_hotpath_linear_block_total.fetch_add(1, std::memory_order_relaxed);
            }
            ok = false;
        }
        return ok;
    }

    void bump_render_hotpath_dispatch_fast(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_hotpath_dispatch_fast_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_hotpath_dispatch_full(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_hotpath_dispatch_full_total.fetch_add(n, std::memory_order_relaxed);
        }
    }

    // Issue #1357: mark frame boundary for histogram (called from arena-render-frame-reset).
    void mark_render_frame_boundary() noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (last_frame_mark_.time_since_epoch().count() != 0) {
            const auto ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_frame_mark_)
                    .count());
            aura::compiler::render_telemetry::global_frame_time_stats().record(ns);
            if (compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
                m->render_frame_time_samples.fetch_add(1, std::memory_order_relaxed);
                m->render_frame_time_total_ns.fetch_add(ns, std::memory_order_relaxed);
            }
        }
        last_frame_mark_ = now;
    }

    [[nodiscard]] const std::vector<
        std::unique_ptr<aura::compiler::render_telemetry::PrimLatencyStats>>&
    prim_latency_table() const noexcept {
        return prim_latency_;
    }
    // Issue #491 / #680 rebind observability: bump
    // hotswap-invalidate + invalidate_function-calls on
    // (mutate:rebind) without running the BFS cascade (the
    // rebind path is lazy re-lower, not hard invalidate).
    // Counters are observability-only relaxed atomics; the
    // direct bump is safe alongside mark_define_dirty and
    // doesn't interfere with dep_graph_ or invalidate_function.
    inline void bump_rebind_invalidate() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
            m->invalidate_function_calls.fetch_add(1, std::memory_order_relaxed);
        }
        // Also bump the per-Evaluator precise-inval counter so
        // query:hotswap-stats / query:jit-stats surfaces rebinds
        // as a precise invalidation event. Mirrors the bump that
        // finalize_define_mutate_invalidation(run_full_invalidate=true)
        // performs inside the BFS cascade. Relaxed atomic, safe
        // alongside the lazy mark_define_dirty path.
        precise_define_inval_hits_.fetch_add(1, std::memory_order_relaxed);
        // Issue #683: linear ownership revalidate probe after
        // rebind. The full revalidate path
        // (run_linear_ownership_revalidate_after_invalidate)
        // walks the AST + bumps safepoint probes; that's heavy.
        // For the rebind path, the underlying invalidation has
        // already been marked via mark_define_dirty, so we just
        // bump the counter so observability surfaces the
        // revalidate event. (The actual re-lower is still lazy.)
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_relower_revalidate_hits.fetch_add(1, std::memory_order_relaxed);
            m->linear_post_mutate_revalidations_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #672: linear ownership runtime enforcement
    // observability bump helpers. These wrap the existing
    // atomics on CompilerMetrics so callers (Guard dtor,
    // probe paths, AI Agent verify-tool hooks) have a
    // canonical surface that survives the
    // atomic-vs-Evaluator-metrics-access pattern split.
    //
    // Bumped on every linear-ownership violation event —
    // use-after-move / double-borrow / frame-version
    // mismatch / bridge_epoch mismatch. Per the issue's
    // recommended-action matrix, callers may follow this
    // bump with either panic_or_rollback (strict policy)
    // or log_warn + auto_revalidate (lenient policy).
    inline void bump_linear_ownership_violation() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
            m->linear_deopt_on_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #672: bump on a successful post-mutate linear
    // ownership revalidate (i.e. the Guard exit saw no
    // violations). Companion to bump_linear_ownership_violation
    // — the AI Agent can derive the violation_rate =
    // violations / (violations + passes) per session.
    inline void bump_linear_ownership_pass() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
            m->linear_check_pass_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #672: bump on every Guard exit (success path)
    // — the Guard exit IS an enforcement event, and the
    // AI Agent can compute the propagation_ratio =
    // exits / violations to gauge how often the post-mutate
    // revalidate is "clean". Pairs with
    // linear_post_mutate_enforcements_total.
    inline void bump_linear_post_mutate_enforcement() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #672: bump when the linear-ownership probe
    // detects a leaked linear binding (a `(let ((x (Linear e))) ...)`
    // where the binding escapes without consuming). Pairs with
    // linear_violations_caught_total for the AI Agent to
    // distinguish "use-after-move" from "leaked-linear".
    inline void bump_linear_leak_prevented() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_leak_prevented_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #672: public linear ownership enforcement entry
    // point. Wraps the private validate_linear_ownership_state
    // + bumps the violation counter on failure. Takes the
    // frame_version + linear_state as raw args (the public
    // StableNodeRef API exposes `.gen` for the caller to
    // pass here) so this method doesn't need to import the
    // ast module's StableNodeRef type. Returns true if the
    // ref is still valid (frame_version >= current and
    // bridge_epoch matches). The caller is expected to
    // follow up with either rollback (strict policy) or
    // auto_revalidate + log (lenient policy).
    [[nodiscard]] bool check_linear_ownership_for_frame(std::uint64_t frame_version,
                                                        std::uint8_t linear_state = 1) noexcept {
        const auto current_ver = defuse_version_snapshot();
        const auto current_bridge = current_bridge_epoch();
        const bool ok = validate_linear_ownership_state(linear_state, frame_version, current_ver, 0,
                                                        current_bridge);
        if (!ok) {
            bump_linear_ownership_violation();
        } else {
            bump_linear_ownership_pass();
        }
        return ok;
    }
    // Issue #614: primitives hot-path memory-stability counters.
    // Bumped by evaluator_primitives_list.cpp (pair pushes +
    // cdr walks) so the AI agent can correlate the
    // (query:primitive-perf-stats) call_total with the pair/
    // traverse cost it imposed.
    inline void bump_pair_alloc_count() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pair_alloc_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    inline void bump_linear_traverse_count(std::uint64_t steps,
                                           std::uint64_t max_depth_observed) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_traverse_total.fetch_add(steps, std::memory_order_relaxed);
            // max_high_water — observe the largest ever. relaxed load +
            // store is the standard low-contention pattern; a brief
            // race on which exact max wins is acceptable (the next
            // hot list operation will re-establish the high-water).
            auto prev = m->cdr_depth_max.load(std::memory_order_relaxed);
            while (max_depth_observed > prev &&
                   !m->cdr_depth_max.compare_exchange_weak(prev, max_depth_observed,
                                                           std::memory_order_relaxed)) {
            }
        }
    }
    // Issue #549: self-evolution-stability accessors + bump
    // helpers. Public so the
    // (query:self-evolution-stability-stats) primitive can
    // read them, and validate_stable_ref + exit_mutation_boundary
    // + fiber yield hooks can bump them.
    [[nodiscard]] std::uint64_t get_cross_cow_invalidations() const noexcept {
        return cross_cow_invalidations_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_fiber_stale_ref_count() const noexcept {
        return fiber_stale_ref_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_mutation_log_rollback_count() const noexcept {
        return mutation_log_rollback_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_provenance_mismatch() const noexcept {
        return provenance_mismatch_.load(std::memory_order_relaxed);
    }
    void bump_cross_cow_invalidations() const noexcept {
        cross_cow_invalidations_.fetch_add(1, std::memory_order_relaxed);
        bump_edsl_cow_stable_ref_remap();
    }
    void bump_fiber_stale_ref_count() const noexcept {
        fiber_stale_ref_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_mutation_log_rollback_count() noexcept {
        mutation_log_rollback_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_provenance_mismatch() const noexcept {
        provenance_mismatch_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #550: incremental typed self-mod accessors +
    // bump helpers. Public so the (query:typed-mutation-
    // stats) + (query:dirty-impact) primitives can read
    // them, and the follow-up TypeChecker / OccurrenceInfo
    // wiring can bump them.
    [[nodiscard]] std::uint64_t get_narrowing_refresh_count() const noexcept {
        return narrowing_refresh_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_cross_delta_conflicts_caught() const noexcept {
        return cross_delta_conflicts_caught_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_passes_skipped_type_dirty() const noexcept {
        return passes_skipped_type_dirty_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_touched_roots_size() const noexcept {
        return touched_roots_size_.load(std::memory_order_relaxed);
    }
    void bump_narrowing_refresh_count() const noexcept {
        narrowing_refresh_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_cross_delta_conflicts_caught() const noexcept {
        cross_delta_conflicts_caught_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_passes_skipped_type_dirty(std::size_t n = 1) const noexcept {
        if (n > 0) {
            passes_skipped_type_dirty_.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void set_touched_roots_size(std::uint64_t v) const noexcept {
        touched_roots_size_.store(v, std::memory_order_relaxed);
    }
    // Issue #551: reflect post-mutate accessors + bump helpers.
    // Public so (query:reflect-postmutate-stats) can read
    // them, and the Guard dtor / auto_validate hook can
    // bump them.
    [[nodiscard]] std::uint64_t get_impact_snapshot_count() const noexcept {
        return impact_snapshot_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_schema_validation_pass_count() const noexcept {
        return schema_validation_pass_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_schema_validation_fail_count() const noexcept {
        return schema_validation_fail_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_dirty_nodes_in_snapshot() const noexcept {
        return dirty_nodes_in_snapshot_.load(std::memory_order_relaxed);
    }
    void bump_impact_snapshot_count() const noexcept {
        impact_snapshot_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_schema_validation_pass_count() const noexcept {
        schema_validation_pass_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_schema_validation_fail_count() const noexcept {
        schema_validation_fail_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void set_dirty_nodes_in_snapshot(std::uint64_t v) const noexcept {
        dirty_nodes_in_snapshot_.store(v, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_macro_markers_in_snapshot() const noexcept {
        return macro_markers_in_snapshot_.load(std::memory_order_relaxed);
    }
    void set_macro_markers_in_snapshot(std::uint64_t v) const noexcept {
        macro_markers_in_snapshot_.store(v, std::memory_order_relaxed);
    }
    void set_last_schema_validation_ok(bool v) const noexcept {
        last_schema_validation_ok_.store(v ? 1 : 0, std::memory_order_relaxed);
    }
    [[nodiscard]] bool get_last_schema_validation_ok() const noexcept {
        return last_schema_validation_ok_.load(std::memory_order_relaxed) != 0;
    }
    // Issue #488: post-mutate reflect validation + latest impact entry.
    [[nodiscard]] bool post_mutation_reflect_validate() const noexcept;
    [[nodiscard]] MutationImpactEntry get_latest_mutation_impact_entry() const noexcept;
    // Issue #555: Task1 typed self-mod accessors + bump helpers.
    [[nodiscard]] std::uint64_t get_dirty_propagation_count() const noexcept {
        return dirty_propagation_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_selective_recheck_count() const noexcept {
        return selective_recheck_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_touched_roots_conflict_count() const noexcept {
        return touched_roots_conflict_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_guard_dirty_epoch_count() const noexcept {
        return guard_dirty_epoch_count_.load(std::memory_order_relaxed);
    }
    void bump_dirty_propagation_count() const noexcept {
        dirty_propagation_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_selective_recheck_count() const noexcept {
        selective_recheck_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_touched_roots_conflict_count() const noexcept {
        touched_roots_conflict_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_guard_dirty_epoch_count() const noexcept {
        guard_dirty_epoch_count_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #391: stale-ref policy + observability
    // accessors + bump helpers. Public so the
    // (mutate:set-stale-ref-policy) / (query:stale-ref-policy)
    // / (query:stale-ref-stats) primitives can read+write
    // them, and the core mutate primitives can check the
    // policy + bump the counters.
    [[nodiscard]] StaleRefPolicy get_stale_ref_policy() const noexcept { return stale_ref_policy_; }
    void set_stale_ref_policy(StaleRefPolicy p) noexcept { stale_ref_policy_ = p; }
    [[nodiscard]] PatternIndexPolicy get_pattern_index_policy() const noexcept {
        return pattern_index_policy_;
    }
    void set_pattern_index_policy(PatternIndexPolicy p) noexcept { pattern_index_policy_ = p; }
    [[nodiscard]] std::uint64_t get_pattern_index_lazy_rebuilds() const noexcept {
        return pattern_index_lazy_rebuilds_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_pattern_index_eager_mutate_rebuilds() const noexcept {
        return pattern_index_eager_mutate_rebuilds_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_pattern_index_eager_cow_rebuilds() const noexcept {
        return pattern_index_eager_cow_rebuilds_.load(std::memory_order_relaxed);
    }
    // Issue #1503: true when Evaluator tag_arity_index_ is populated for
    // the current workspace_flat_ (safe shared-lock snapshot).
    [[nodiscard]] bool tag_arity_index_is_warm() const noexcept {
        std::shared_lock<std::shared_mutex> rlock(tag_arity_index_mtx_);
        return workspace_flat_ != nullptr && tag_arity_index_workspace_ == workspace_flat_ &&
               !tag_arity_index_.empty();
    }
    void bump_pattern_index_auto_warm_syncs() const noexcept {
        pattern_index_auto_warm_syncs_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_pattern_index_auto_warm_syncs() const noexcept {
        return pattern_index_auto_warm_syncs_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_stale_ref_blocked_count() const noexcept {
        return stale_ref_blocked_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_stale_ref_warned_count() const noexcept {
        return stale_ref_warned_count_.load(std::memory_order_relaxed);
    }
    void bump_stale_ref_blocked_count() noexcept {
        stale_ref_blocked_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_stale_ref_warned_count() noexcept {
        stale_ref_warned_count_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_raw_nodeid_usage_in_primitives_count() const noexcept {
        return raw_nodeid_usage_in_primitives_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_stable_ref_validated_in_primitives_count() const noexcept {
        return stable_ref_validated_in_primitives_count_.load(std::memory_order_relaxed);
    }
    void bump_raw_nodeid_usage_in_primitives_count() noexcept {
        raw_nodeid_usage_in_primitives_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_stable_ref_validated_in_primitives_count() noexcept {
        stable_ref_validated_in_primitives_count_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #438: fiber-migration observability
    // accessors + bump helpers. Public so the
    // (query:fiber-migration-stats) primitive can read
    // them, and the scheduler / fiber hooks (the
    // follow-up) can bump them.
    [[nodiscard]] std::uint64_t get_mutation_steal_attempts() const noexcept {
        return mutation_steal_attempts_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_boundary_violation_count() const noexcept {
        return boundary_violation_count_.load(std::memory_order_relaxed);
    }
    void bump_mutation_steal_attempt() noexcept {
        mutation_steal_attempts_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_boundary_violation_count() noexcept {
        boundary_violation_count_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #556: EDSL concurrency safety accessors + bump helpers.
    [[nodiscard]] std::uint64_t get_unsafe_boundary_attempts() const noexcept {
        return unsafe_boundary_attempts_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_lock_contention_us() const noexcept {
        return lock_contention_us_.load(std::memory_order_relaxed);
    }
    void bump_unsafe_boundary_attempts() const noexcept {
        unsafe_boundary_attempts_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_lock_contention_us(std::uint64_t delta_us) const noexcept {
        lock_contention_us_.fetch_add(delta_us, std::memory_order_relaxed);
    }
    // Issue #439: GC safepoint observability accessors +
    // bump helpers. Public so the
    // (query:gc-safepoint-stats) primitive can read them,
    // and the Fiber::check_gc_safepoint /
    // request_gc_safepoint helpers (the follow-up) can
    // bump them via the C-linkage shim.
    [[nodiscard]] std::uint64_t get_gc_safepoint_requests_total() const noexcept {
        return gc_safepoint_requests_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_gc_safepoint_waits_total() const noexcept {
        return gc_safepoint_waits_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_gc_safepoint_deferred_total() const noexcept {
        return gc_safepoint_deferred_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_gc_safepoint_wait_total_ns() const noexcept {
        return gc_safepoint_wait_total_ns_.load(std::memory_order_relaxed);
    }
    void bump_gc_safepoint_request() noexcept {
        gc_safepoint_requests_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_gc_safepoint_wait() noexcept {
        gc_safepoint_waits_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_gc_safepoint_deferred() noexcept {
        gc_safepoint_deferred_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_gc_safepoint_wait_ns(std::uint64_t delta_ns) noexcept {
        gc_safepoint_wait_total_ns_.fetch_add(delta_ns, std::memory_order_relaxed);
    }
    // Issue #443: external simulator tool-calling
    // observability accessors + bump helpers. Public
    // so the (query:verify-tool-stats) primitive can
    // read them, and the verify:run-external-sim /
    // verify:parse-coverage / verify:parse-failures
    // primitives can bump them.
    [[nodiscard]] std::uint64_t get_verify_tool_calls_total() const noexcept {
        return verify_tool_calls_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_verify_tool_cache_hits_total() const noexcept {
        return verify_tool_cache_hits_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_verify_tool_parse_errors_total() const noexcept {
        return verify_tool_parse_errors_total_.load(std::memory_order_relaxed);
    }
    void bump_verify_tool_call() noexcept {
        verify_tool_calls_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_verify_tool_cache_hit() noexcept {
        verify_tool_cache_hits_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_verify_tool_parse_error() noexcept {
        verify_tool_parse_errors_total_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_verify_tool_guard_captures_total() const noexcept {
        return verify_tool_guard_captures_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_verify_tool_dirty_propagations_total() const noexcept {
        return verify_tool_dirty_propagations_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_verify_tool_stable_ref_hits_total() const noexcept {
        return verify_tool_stable_ref_hits_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_verify_tool_feedback_mutate_success_total() const noexcept {
        return verify_tool_feedback_mutate_success_total_.load(std::memory_order_relaxed);
    }
    void bump_verify_tool_guard_capture() noexcept {
        verify_tool_guard_captures_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_verify_tool_dirty_propagation() noexcept {
        verify_tool_dirty_propagations_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_verify_tool_stable_ref_hit() noexcept {
        verify_tool_stable_ref_hits_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_verify_tool_feedback_mutate_success() noexcept {
        verify_tool_feedback_mutate_success_total_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #443: public cache accessors (called from
    // verify_tool.cpp's lambdas, which don't get
    // friend access because they're not class members).
    [[nodiscard]] std::optional<std::string>
    lookup_verify_tool_cache(const std::string& cmd) const noexcept {
        auto* ws = workspace_flat();
        if (!ws)
            return std::nullopt;
        const auto gen = ws->generation();
        for (const auto& entry : verify_tool_cache_) {
            if (entry.cmd == cmd && entry.gen == gen) {
                return entry.result;
            }
        }
        return std::nullopt;
    }
    void insert_verify_tool_cache(const std::string& cmd, const std::string& result) {
        auto* ws = workspace_flat();
        if (!ws)
            return;
        const auto gen = ws->generation();
        verify_tool_cache_.push_back({cmd, gen, result});
        while (verify_tool_cache_.size() > kVerifyToolCacheMax) {
            verify_tool_cache_.pop_front();
        }
    }
    // Issue #443: public string-heap append helper (for
    // verify_tool.cpp to push a result string + return
    // its index). The lambdas in verify_tool.cpp can't
    // access the private string_heap_ directly, so
    // they use this wrapper.
    [[nodiscard]] std::size_t string_heap_size() const noexcept { return string_heap_.size(); }
    const std::string& string_heap_at(std::size_t idx) const { return string_heap_[idx]; }
    std::int32_t push_string_heap(const std::string& s) {
        const auto idx = static_cast<std::int32_t>(string_heap_.size());
        string_heap_.push_back(s);
        return idx;
    }
    // Issue #346: push_pair helper. Returns the new
    // pair's index. Used by the query:mutation-log +
    // query:mutations-since primitives to build the
    // pair-list result without needing direct access
    // to the private pairs_ vector. The pair is
    // stored by value (copy), so the input values
    // remain valid after the call.
    std::int32_t push_pair(EvalValue car, EvalValue cdr) {
        const auto idx = static_cast<std::int32_t>(pairs_.size());
        pairs_.push_back({car, cdr});
        return idx;
    }
    // Issue #439: request_gc_safepoint() — the
    // pre-requisite helper for safe GC coordination.
    // P0 scope-limited ship: this method bumps the
    // request counter; if an outermost MutationBoundary
    // guard is held (per-thread depth > 0), it returns
    // 1 (deferred) and bumps the deferred counter. If
    // no guard is held, it returns 0 (immediate) and
    // lets the caller proceed with the safepoint.
    //
    // The follow-up wires the call from
    // Fiber::check_gc_safepoint (via the C-linkage
    // shim) + the Scheduler::request_gc_safepoint
    // entry point.
    int request_gc_safepoint() noexcept {
        bump_gc_safepoint_request();
        // Issue #1675: never STW/compact mid render present (hotpath).
        // Adaptive GC + arena compact soft-gate already consult
        // in_render_hotpath(); safepoint entry must defer too so
        // long-running terminal apps keep frame-time predictability.
        if (aura::core::arena_policy::in_render_hotpath()) {
            bump_gc_safepoint_deferred();
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
                m->render_hotpath_skip_total.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
        // Issue #1489 / #651 / #1581 AC2: defer while PanicCheckpoint
        // recovery window is open (process-wide arm or live checkpoint).
        if (aura::gc_hooks::should_defer_compact_for_pending_checkpoint() ||
            has_panic_checkpoint()) {
            bump_gc_safepoint_deferred();
            bump_gc_blocked_by_pending_panic();
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
                m->gc_blocked_by_panic_total.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
        if (mutation_boundary_depth() > 0) {
            bump_gc_safepoint_deferred();
            bump_orchestration_llm_gc_safepoint_adapted();
            // Issue #1483 C4: natural defer path also bumps the
            // adaptive threshold (the same pressure that triggered
            // mutation_boundary_depth > 0 is a pressure signal for
            // the adaptive heuristic). Doubles the threshold.
            bump_safepoint_adaptive_threshold();
            return 1;
        }
        // Issue #1483 C4: adaptive-threshold consult at the
        // immediate path. When the threshold is > 0 AND the
        // current per-fiber mutation_stack_depth exceeds the
        // threshold (pressure signal), force deferral instead
        // of immediate. Bumps the adaptive-defer counter
        // (distinct from the natural-defer path) and doubles
        // the threshold.
        if (should_adapt_safepoint_threshold()) {
            bump_gc_safepoint_deferred();
            bump_safepoint_adaptive_defer_count();
            bump_safepoint_adaptive_threshold();
            return 1;
        }
        // No adaptive pressure: reset the threshold so future
        // immediate paths aren't deferred by stale state.
        reset_safepoint_adaptive_threshold();
        // Issue #1515: at immediate safepoint, sync linear roots +
        // bridge_epoch then probe EnvFrame ownership (supersedes
        // probe-only #683 path — still probes inside sync).
        sync_linear_roots_and_bridge_epoch();
        // Issue #1543: GC safepoint path audit after root re-register.
        (void)run_linear_gc_root_audit(kLinearGcRootAuditGcSafepoint);
        return 0;
    }
    // Issue #439: wait_for_safepoint(timeout_ms) — the
    // pre-requisite helper for safe GC wait. P0
    // scope-limited ship: this method bumps the wait
    // counter + records the timeout (in ns). The
    // follow-up wires the call from
    // Scheduler::wait_for_safepoint + the fiber yield
    // path.
    void wait_for_safepoint(std::uint64_t timeout_ms) noexcept {
        bump_gc_safepoint_wait();
        bump_gc_safepoint_wait_ns(timeout_ms * 1'000'000);
    }
    // Issue #438: transfer_mutation_stack_to_current_fiber
    // — the pre-requisite helper for safe fiber migration
    // across workers. P0 scope-limited ship: this method
    // is a no-op that bumps the migration-attempt counter
    // + logs the request, because each Evaluator already
    // owns its own per-thread mutation stack (the
    // transfer is trivially a no-op at the data level).
    // The follow-up wires the call from Fiber::resume()
    // before g_fiber_setter_ runs.
    // Issue #1500: also batch-restamp atomic-batch + COW-boundary
    // pinned StableNodeRefs so long-held refs survive steal.
    void transfer_mutation_stack_to_current_fiber() noexcept {
        sync_per_fiber_mutation_stack(nullptr);
        bump_mutation_steal_attempt();
        // Issue #1490 / #1580: full closed-loop refresh using fiber hints
        // (EnvFrame + linear/StableNodeRef re-pin + panic transfer).
        complete_post_resume_steal_refresh(g_current_fiber_void);
    }

    // Issue #1500: batch refresh_if_stale over atomic_batch_pinned_refs_
    // and cow_boundary_pinned_refs_. Called from fiber steal / Guard
    // dtor / re_pin_cow_children_from_snapshot. Returns # refreshed.
    std::size_t restamp_pinned_stable_refs() noexcept;
    // Issue #1497: site tags for unified auto-restamp hooks on
    // GC compact / post-steal / safepoint / yield-resume paths.
    enum class StableRefRefreshSite : std::uint8_t {
        Steal = 0,
        GcSafepoint = 1,
        CompactOrRepin = 2,
        YieldResume = 3,
        Join = 4, // Issue #1595: Fiber::join / parallel_intend post-join
    };
    // Unified sweep: restamp_pinned_stable_refs + site counters.
    // Force-calls validate_or_refresh semantics via refresh_if_stale.
    std::size_t auto_restamp_pinned_stable_refs_at(StableRefRefreshSite site) noexcept;
    // Issue #1564: policy-gated ensure — full provenance validate/refresh.
    // Default AutoRefreshOnBoundary. Bumps process-wide provenance counters.
    // Returns NodeView on success; nullopt if hard-invalid (fail path).
    [[nodiscard]] std::optional<aura::ast::NodeView>
    ensure_valid_or_refresh(aura::ast::FlatAST::StableNodeRef& ref,
                            bool auto_refresh = true) noexcept;
    // Policy toggle (default true): when false, only validate (no restamp).
    void set_stable_ref_auto_refresh_policy(bool on) noexcept {
        stable_ref_auto_refresh_policy_.store(on, std::memory_order_relaxed);
    }
    [[nodiscard]] bool stable_ref_auto_refresh_policy() const noexcept {
        return stable_ref_auto_refresh_policy_.load(std::memory_order_relaxed);
    }
    // Issue #391: validate a (id . gen) stable-ref pair
    // against the current workspace's generation. Returns
    // true if the ref is still valid (in-range + gen
    // match), false otherwise. Side-effect: bumps
    // stale_ref_warned_count_ on invalid (regardless of
    // policy — observability is unconditional). Returns
    // (valid, is_stale). is_stale=true means the ref was
    // captured but the AST has changed since.
    [[nodiscard]] std::pair<bool, bool>
    validate_stable_ref(aura::ast::NodeId id, std::uint16_t captured_gen) const noexcept {
        if (!workspace_flat_)
            return {false, false};
        const auto& flat = *workspace_flat_;
        if (id >= flat.size()) {
            // Out-of-range → invalid + stale.
            return {false, true};
        }
        if (flat.generation() != captured_gen) {
            // Issue #549: classify the gen-mismatch as either
            // a cross-COW-snapshot invalidation (most common
            // case under mutate load — a structural mutate
            // bumped generation_), or a fiber-stale-ref (when
            // the captured gen is from a different fiber's
            // workspace, surfaced via the captured-vs-current
            // delta). We use the delta between captured_gen and
            // the current generation_ as a heuristic: small
            // delta = cross-COW (likely same fiber, just
            // mutated); large delta = fiber-stale (captured
            // from a different workspace's history).
            const auto delta = (captured_gen > flat.generation())
                                   ? static_cast<std::uint32_t>(captured_gen - flat.generation())
                                   : static_cast<std::uint32_t>(flat.generation() - captured_gen);
            if (delta <= 8) {
                bump_cross_cow_invalidations();
            } else {
                bump_fiber_stale_ref_count();
            }
            return {false, true};
        }
        return {true, false};
    }
    // Issue #458: query hygiene accessors + bump helpers.
    [[nodiscard]] std::uint64_t get_hygiene_violation_count() const noexcept {
        return hygiene_violation_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_macro_introduced_skipped_in_query() const noexcept {
        return macro_introduced_skipped_in_query_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_total_query_calls() const noexcept {
        return total_query_calls_.load(std::memory_order_relaxed);
    }
    void bump_hygiene_violation_count() noexcept {
        hygiene_violation_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_hygiene_violation_attempt() noexcept {
        hygiene_violation_attempts_.fetch_add(1, std::memory_order_relaxed);
        bump_hygiene_violation_count();
    }
    [[nodiscard]] std::uint64_t get_hygiene_violation_attempts() const noexcept {
        return hygiene_violation_attempts_.load(std::memory_order_relaxed);
    }
    // Issue #422: lightweight probe that mutate hygiene guards
    // are wired (attempts recorded at block sites).
    void ensure_hygiene_violation_detection() const noexcept;
    void bump_macro_introduced_skipped_in_query() noexcept {
        macro_introduced_skipped_in_query_.fetch_add(1, std::memory_order_relaxed);
        // Issue #593: correlate query:pattern hygiene skips with
        // the AST→IR closed-loop observability surface.
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_ir_capture_prevented_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #593: tag_arity delta hits during hygiene-filtered query.
    void bump_tag_arity_hygiene_query_delta() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->tag_arity_hygiene_query_delta_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #596: Guard + panic checkpoint + reflect closed-loop observability.
    void bump_guard_panic_reflect_restores_on_resume() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->guard_panic_reflect_restores_on_resume_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_guard_panic_reflect_validate_hook() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->guard_panic_reflect_validate_hook_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_guard_panic_reflect_boundary_violation_prevented() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->guard_panic_reflect_boundary_violation_prevented_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    // Issue #599: compiler root epoch/version protocol observability.
    void bump_compiler_root_stale_closure_detected() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_root_stale_closure_detected_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_compiler_root_env_version_mismatch() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_root_env_version_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_compiler_root_dangling_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_root_dangling_prevented_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1485 C1: bump_stale_closure_prevented — lifetime count of
    // stale closures detected at apply_closure entry (complements
    // closure_stale_apply_count_total which is bumped inside
    // closure_needs_safe_fallback). This top-level "we caught a stale
    // closure before dispatch" signal is distinct from the per-check
    // breakdowns the helper bumps. See C1 commit message + the
    // observability_metrics.h:218 area for the semantic split.
    void bump_stale_closure_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stale_closure_prevented.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1485 C1: bump_closure_epoch_mismatch_fallback —
    // lifetime count of safe-fallback paths taken after a stale
    // closure was detected (complements
    // closure_safe_fallback_apply_count_total which counts ANY safe
    // fallback; this one is specific to the bridge_epoch /
    // defuse_version_ mismatch surface — distinct from
    // linear_post_mutate_enforce fallbacks which are counted
    // separately via linear_ownership_violation_prevented).
    void bump_closure_epoch_mismatch_fallback() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->closure_epoch_mismatch_fallback.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1485 C1: accessors for the 2 new atomics.
    [[nodiscard]] std::uint64_t get_stale_closure_prevented() const noexcept {
        if (!compiler_metrics_)
            return 0;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        return m->stale_closure_prevented.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_closure_epoch_mismatch_fallback() const noexcept {
        if (!compiler_metrics_)
            return 0;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        return m->closure_epoch_mismatch_fallback.load(std::memory_order_relaxed);
    }
    // Issue #600: incremental per-block re-lower + closure bridge synergy.
    void bump_incremental_closure_blocks_relowered(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_closure_blocks_relowered_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_incremental_closure_min_scope_win(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_closure_min_scope_win_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_incremental_closure_jit_sync() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_closure_jit_sync_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #741: impact_scope → closure_bridge + EnvFrame version re-stamp.
    void bump_incremental_closure_bridge_impact_blocks(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_closure_bridge_impact_blocks_total.fetch_add(n,
                                                                        std::memory_order_relaxed);
        }
    }
    void bump_incremental_closure_quote_lambda_stale_prevented() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_closure_quote_lambda_stale_prevented_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    void bump_incremental_closure_env_version_resync() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_closure_env_version_resync_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #743 / #1621: MutationBoundary / fiber transition arena
    // smart auto-policy probe (frag + dirty + Shape churn + defrag_req).
    void probe_arena_auto_policy_on_boundary_exit(bool success) noexcept {
        if (!arena_group_)
            return;
        const double frag = arena_ ? arena_->stats().fragmentation_ratio() : 0.0;
        const double small_util = arena_ ? arena_->small_pool_utilization() : 0.0;
        const bool want_defrag = arena_ && arena_->defrag_requested();
        const bool dirty =
            aura::core::arena_policy::dirty_cascade_pending.load(std::memory_order_acquire);
        const bool shape = aura::core::arena_policy::peek_shape_churn();
        const auto decision = aura::core::arena_policy::evaluate_auto_compact_policy(
            frag, want_defrag, dirty, shape, /*fiber_active=*/false,
            aura::core::arena_policy::in_render_hotpath(), small_util);
        if (success) {
            aura::core::arena_policy::record_fragmentation_post_mutate(frag);
            if (decision.should_compact && compiler_metrics_) {
                static_cast<CompilerMetrics*>(compiler_metrics_)
                    ->arena_guard_request_defrag_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (decision.should_compact) {
            (void)aura::core::arena_policy::consume_dirty_cascade();
            (void)aura::core::arena_policy::consume_shape_churn();
            const std::size_t saved = arena_group_->auto_compact_with_safety();
            aura::core::arena_policy::record_boundary_exit_compact();
            if (success && saved > 0) {
                aura::core::arena_policy::record_env_reval_success();
                if (resync_live_closure_env_versions_on_invalidate() > 0)
                    bump_incremental_closure_env_version_resync();
            }
        } else {
            arena_group_->bump_auto_compact_guard_call();
        }
    }
    void probe_arena_auto_policy_on_fiber_transition() noexcept {
        if (!arena_group_)
            return;
        const double frag = arena_ ? arena_->stats().fragmentation_ratio() : 0.0;
        const double small_util = arena_ ? arena_->small_pool_utilization() : 0.0;
        const bool want_defrag = arena_ && arena_->defrag_requested();
        const bool dirty =
            aura::core::arena_policy::dirty_cascade_pending.load(std::memory_order_acquire);
        const bool shape = aura::core::arena_policy::peek_shape_churn();
        const auto decision = aura::core::arena_policy::evaluate_auto_compact_policy(
            frag, want_defrag, dirty, shape, /*fiber_active=*/true,
            aura::core::arena_policy::in_render_hotpath(), small_util);
        if (!decision.should_compact)
            return;
        (void)aura::core::arena_policy::consume_dirty_cascade();
        (void)aura::core::arena_policy::consume_shape_churn();
        arena_group_->auto_compact_with_safety();
        aura::core::arena_policy::record_fiber_transition_compact();
        aura::core::arena_policy::record_defrag_fiber_safe_hit();
    }
    // Proactive EnvFrame version re-stamp for live tree-walker closures
    // held across invalidate_function / partial re-lower. Returns the
    // number of frames resynced.
    std::uint64_t resync_live_closure_env_versions_on_invalidate();
    // Issue #654: macro hygiene vs fiber/panic/AOT/SoA cross-cutting gaps.
    void bump_macro_hygiene_panic_restamp() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_hygiene_panic_restamp_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_hygiene_provenance_violation() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_hygiene_provenance_violations_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_expand_checkpoint_save() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_expand_checkpoint_saves_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1908: MutationBoundaryGuard + macro clone provenance hardening.
    // Bump sites (per #1908 plan):
    //   - bump_macro_provenance_repin_on_steal_total: clone_macro_body
    //     MacroIntroduced path (via bridge hook
    //     aura_macro_provenance_repin_on_steal) +
    //     complete_post_resume_steal_refresh post probe +
    //     transfer_and_revalidate_panic_checkpoint post panic restamp.
    //   - bump_hygiene_violation_prevented_on_boundary_total: outermost
    //     flush_mutation_boundary exit post dirty/epoch bump +
    //     complete_post_resume_steal_refresh post probe +
    //     transfer_and_revalidate_panic_checkpoint post panic restamp.
    // Both counters track boundary-interaction signals: the boundary
    // did its job (repin / prevent violation) under concurrent fiber
    // steal + GC compact + macro clone (the #1908 AC contract).
    void bump_macro_provenance_repin_on_steal_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_provenance_repin_on_steal_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_hygiene_violation_prevented_on_boundary_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->hygiene_violation_prevented_on_boundary_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
        }
    }
    void bump_macro_reflect_hygiene_validation() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_reflect_hygiene_validation_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #712: subtree-level reflect validation counters. These
    // are bumped by post_mutation_reflect_validate() when it walks
    // MacroIntroduced nodes and finds hygiene drift. They back the
    // standalone (query:macro-reflect-validation-stats) primitive.
    void bump_macro_reflect_validation_calls() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_reflect_validation_calls_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_reflect_schema_mismatches_caught() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_reflect_schema_mismatches_caught_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_reflect_post_mutate_hygiene_drift() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_reflect_post_mutate_hygiene_drift_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
        }
    }
    // Issue #713: JIT/AOT/Interpreter macro-hygiene violation
    // counters backing the standalone (query:macro-jit-hygiene-stats)
    // primitive. The bumps are wired from aura_jit_bridge.cpp
    // (aot reload path) and evaluator.ixx fast paths; deopt hook
    // (LLVM IR emit) is follow-up work for a focused JIT session.
    void bump_macro_jit_hygiene_deopt() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_jit_hygiene_deopt_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_aot_reload_marker_mismatches() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_aot_reload_marker_mismatches_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_interpreter_fallback_hygiene_hits() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_interpreter_fallback_hygiene_hits_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
        }
    }
    // Issue #714: self-evolution closed-loop strategy recommendation
    // counters backing the (query:self-evolution-closedloop-stats)
    // primitive. Each bump records an Agent-facing decision point:
    //   - safe:        hygiene risk detected, recommend conservative
    //                  mutate (e.g. avoid touching MacroIntroduced
    //                  subtrees until dirty impact settles)
    //   - aggressive:  hygiene risk low + dirty impact == 0, recommend
    //                  deep mutate (e.g. macro expansion / EDSL rewrites)
    //   - balanced:    default — recommend the median-path mutate
    //
    // Phase 1 ships the bump helpers so future Guard dtor /
    // mark_dirty_upward / reflect auto_validate hooks can call them
    // at each decision point. The primitive derives the current
    // "recommended mutation strategy" string from the highest of
    // these three counters.
    void bump_self_evo_strategy_recommend_safe() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->self_evo_strategy_recommend_safe_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_self_evo_strategy_recommend_aggressive() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->self_evo_strategy_recommend_aggressive_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_self_evo_strategy_recommend_balanced() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->self_evo_strategy_recommend_balanced_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #715: StableNodeRef cross-layer validation counters
    // backing the (query:stable-ref-layer-stats) primitive. The
    // is_valid_in_layer() helper on StableNodeRef is pure read
    // and does NOT bump these counters itself (allocation-free,
    // safe in tight loops); callers that want per-call
    // observability can route through these helpers at each
    // decision point (e.g. inside MutationBoundaryGuard on
    // rebind, or at workspace merge time).
    void bump_stable_ref_cross_layer_validation() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_cross_layer_validations_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_cross_layer_mismatch() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_cross_layer_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_cow_boundary_pin() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_cow_boundary_pins_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #716: pattern matcher observability counters backing
    // the (query:pattern-stats) primitive. These are public so
    // future query_matcher.cpp + evaluator_primitives_query.cpp
    // hot-path wiring can call them at each decision point
    // (matcher invocation / is_macro_introduced skip / fast-path
    // promotion). The primitive currently exposes them as raw
    // counters so the Agent can compute its own derived
    // statistics (filter ratio = filtered / calls; fast-path
    // promotion rate = fast-path / calls).
    void bump_pattern_matcher_call() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_matcher_calls_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_pattern_macro_intro_filtered(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_macro_intro_filtered_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_pattern_fast_path_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_fast_path_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #717: fiber-safe MutationBoundaryGuard recovery
    // counters backing the (query:fiber-boundary-violation-stats)
    // primitive. These are public so future MutationBoundaryGuard
    // dtor / panic_checkpoint wiring + fiber-mutation paths can
    // call them at each decision point (rollback / yield-resume /
    // recovery-failure). The primitive currently exposes them as
    // raw counters so the Agent can compute its own derived
    // statistics (recovery_success_rate = yield_resumes /
    // (yield_resumes + recovery_failures); rollback_rate =
    // rollbacks / guard_entries).
    void bump_mutation_boundary_rollback() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutation_boundary_rollbacks_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_mutation_boundary_yield_resume() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutation_boundary_yield_resumes_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_mutation_boundary_recovery_failure() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutation_boundary_recovery_failures_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1904: public bump helper for the legacy mutate:* lock + bump
    // counter. Called from legacy migration sites (#1904 Commit 2) BEFORE
    // they are migrated to MutationBoundaryGuard RAII; the linter
    // (scripts/check_legacy_mutate_lock.py) will fail the build when any
    // legacy pattern is reintroduced, so this counter monotonically
    // decreases to 0 as the migration completes. Public so tests +
    // Commit 2 migration use the same API and never touch the
    // CompilerMetrics struct directly (no module dependency).
    void bump_mutation_legacy_manual_lock_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutation_legacy_manual_lock_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #718: fine-grained per-block re-lower observability
    // counters backing the (query:incremental-relower-stats)
    // primitive. These are public so future service.ixx::
    // invalidate_function + lowering_impl.cpp::lower_to_ir_with_
    // cache + pass_manager.ixx::run_incremental_pipeline wiring
    // can call them at each decision point (impact_scope hit /
    // partial re-lower / full fallback / time saved estimate).
    //
    // The time-saved bump helper takes a microsecond value so the
    // caller can record the estimated savings (full_relower_cost -
    // partial_relower_cost) without having to track it locally.
    void bump_incremental_impact_blocks_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_impact_blocks_hit_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_incremental_partial_relower() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_partial_relower_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_incremental_full_fallback() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_full_fallback_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_incremental_time_saved_us(std::uint64_t us) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_time_saved_us_total.fetch_add(us, std::memory_order_relaxed);
        }
    }
    // Issue #719: Prompt 6 closure/EnvFrame epoch + linear ownership
    // + GC root sync safety counters backing the
    // (query:closure-env-epoch-safety-stats) primitive. These are
    // public so future apply_closure hot path + invalidate_function
    // hook + GuardShape/Linear op handlers + JIT PrimCall/Capture +
    // ScopedCompilerRoot register/unregister wiring can call them
    // at each decision point (mismatch detected / linear violation
    // caught / GC root sync / dangling prevented).
    void bump_closure_epoch_mismatch() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->closure_epoch_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_linear_violation_post_mutate() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_violation_post_mutate_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_gc_root_sync() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->gc_root_sync_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_dangling_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dangling_prevented_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #720: JIT/Interpreter parity counters backing the
    // (query:jit-interpreter-parity-stats) primitive. These are
    // public so future aura_jit.cpp lower() + FlatInstruction
    // conversion + unhandled hook + GuardShape/linear full consume
    // + JIT->CompilerService deopt/invalidate wiring can call them
    // at each decision point (unhandled spike / metadata mismatch
    // / deopt-on-mutate / interpreter fallback).
    void bump_jit_unhandled_opcode_spike() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_unhandled_opcode_spikes_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_jit_metadata_mismatch() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_metadata_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_jit_deopt_on_mutate() const noexcept {
        // Issue #1316/#1563: under render hot path OR when prefer_render_critical
        // throttle is set, cap deopt storms (default ≤1 / 500ms).
        if (aura::core::arena_policy::in_render_hotpath() ||
            prefer_render_critical_deopt_throttle_.load(std::memory_order_relaxed)) {
            bump_render_jit_deopt_throttled();
            return;
        }
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_deopt_on_mutate_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1563: force next deopt-on-mutate through render-critical throttle
    // (e.g. set-body of a draw/present closure outside an active hotpath frame).
    void set_prefer_render_critical_deopt_throttle(bool on) const noexcept {
        prefer_render_critical_deopt_throttle_.store(on, std::memory_order_relaxed);
    }
    // Issue #1316/#1563: render-stable deopt throttle (never more than once per window_ms).
    // Returns true if deopt was applied, false if throttled (keep previous JIT).
    [[nodiscard]] bool bump_render_jit_deopt_throttled() const noexcept {
        const auto window_ms =
            compiler_metrics_
                ? static_cast<CompilerMetrics*>(compiler_metrics_)
                      ->render_deopt_throttle_window_ms.load(std::memory_order_relaxed)
                : 500ull;
        const bool apply = aura::core::arena_policy::try_render_deopt_throttle(window_ms);
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            if (apply) {
                m->render_jit_deopt_applied.fetch_add(1, std::memory_order_relaxed);
                m->jit_deopt_on_mutate_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                m->render_jit_deopt_throttled.fetch_add(1, std::memory_order_relaxed);
            }
            // Keep stable_hot_path flag visible.
            m->render_stable_hot_path_active.store(1, std::memory_order_relaxed);
        }
        return apply;
    }
    // Issue #1563: deopt path for a named prim — render-critical always throttled.
    [[nodiscard]] bool bump_deopt_for_prim_name(std::string_view name) const noexcept {
        if (primitives().is_render_critical_name(name) ||
            aura::core::arena_policy::in_render_hotpath()) {
            return bump_render_jit_deopt_throttled();
        }
        bump_jit_deopt_on_mutate();
        return true;
    }
    void enter_render_hotpath() const noexcept {
        aura::core::arena_policy::enter_render_hotpath();
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_hotpath_enter_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void exit_render_hotpath() const noexcept { aura::core::arena_policy::exit_render_hotpath(); }
    [[nodiscard]] bool in_render_hotpath() const noexcept {
        return aura::core::arena_policy::in_render_hotpath();
    }
    // Issue #1676: RAII enter + fence for TUI/render prim bodies.
    struct RenderHotEntryGuard {
        const Evaluator* ev;
        explicit RenderHotEntryGuard(const Evaluator& e) noexcept
            : ev(&e) {
            e.enter_render_hotpath();
            (void)e.fence_render_hot_entry();
        }
        ~RenderHotEntryGuard() noexcept {
            if (ev)
                ev->exit_render_hotpath();
        }
        RenderHotEntryGuard(const RenderHotEntryGuard&) = delete;
        RenderHotEntryGuard& operator=(const RenderHotEntryGuard&) = delete;
    };
    void bump_jit_fallback_to_interpreter() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_fallback_to_interpreter_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #721: IRFunctionSoA column migration + dirty cascade
    // counters backing the (query:ir-soa-completeness-stats)
    // primitive. These are public so future ir_soa.ixx + lowering_
    // impl.cpp + evaluator + aura_jit.cpp + ShapeProfiler hot-path
    // wiring can call them at each decision point (SoA view hit /
    // dirty cascade to shape / PCV byte savings).
    //
    // The byte-savings bump helper takes a byte count so the
    // caller can record the actual PCV-saved delta without
    // tracking it locally. The bump_ir_soa_pcv_wiring_savings
    // helper uses fetch_add with the caller-supplied value.
    void bump_ir_soa_column_migration_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_soa_column_migration_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_ir_soa_dirty_cascade_to_shape() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_soa_dirty_cascade_to_shape_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_ir_soa_pcv_wiring_savings_bytes(std::uint64_t bytes) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_soa_pcv_wiring_savings_bytes_total.fetch_add(bytes, std::memory_order_relaxed);
        }
    }
    // Issue #722: Arena tier/dtor/compact integration counters
    // backing the (query:arena-integration-stats) primitive. These
    // are public so future arena.ixx create/try_allocate overflow +
    // reset + dtor thunk + auto-compact policy + ShapeProfiler +
    // ir_cache_pure wiring can call them at each decision point
    // (tier fallback / dtor-triggered dirty hook / auto-compact
    // trigger / fragmentation update).
    //
    // The fragmentation setter takes a value (not delta) because
    // the ratio is a snapshot, not a cumulative count. The caller
    // is responsible for scaling the float ratio to 0..1e6 before
    // passing.
    void bump_arena_tier_fallback() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_tier_fallbacks_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_arena_dtor_dirty_hook() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_dtor_dirty_hooks_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_arena_auto_compact_trigger() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_auto_compact_triggers_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void set_arena_fragmentation_post_mutate(std::uint64_t ratio_scaled) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_fragmentation_post_mutate.store(ratio_scaled, std::memory_order_relaxed);
        }
    }
    // Issue #767: Arena Auto-Compact Policy + Live Defrag + Fiber/GC
    // Safepoint Yield observability counters backing the (query:
    // arena-auto-compact-defrag-fiber-stats) primitive. These are
    // public so future arena.ixx allocate_raw auto-compact policy +
    // compact/defrag paths + gc_hooks.h + fiber integration +
    // on_compact_hook_ Shape/Dirty integration can call them at
    // each decision point (fiber yield during compact / defrag
    // blocked fibers).
    //
    // fiber_yield_during_compact_total bumps on every actual fiber
    // yield event inside compact()/defrag(); pairs with the existing
    // #685 yield_checks_hit (observability-only). The defrag_blocked_
    // fibers_total bump increments when a fiber hits a defrag
    // safepoint and waits (a new metric #767 introduces to surface
    // the hidden defrag-fiber interaction cost; no equivalent in
    // #685 or #642).
    void bump_arena_auto_compact_fiber_yield_during_compact(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_auto_compact_fiber_yield_during_compact_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_arena_auto_compact_defrag_blocked_fibers(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_auto_compact_defrag_blocked_fibers_total.fetch_add(n,
                                                                        std::memory_order_relaxed);
        }
    }
    // Issue #768: Shape + Pass + Contracts hot-path observability
    // counters backing the (query:shape-pass-hotpath-stats)
    // primitive. These are public so future shape_profiler.cpp
    // inline_shape_of + history push + dominant compute +
    // record_shape stability transition + pass_manager.ixx
    // JITFriendlyPass + DirtyAwarePass + SoAView /
    // ShapeStablePass Concept + arena.ixx shape_inval_on_compact
    // hook + ir_soa.ixx shape_ids_ column can call them at each
    // decision point (hot-path contract checks / shape stability
    // transitions / JIT epoch sync / targeted deopt skips /
    // Concept violations caught).
    void bump_shape_pass_contract_checks_hotpath(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->shape_pass_contract_checks_hotpath_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_shape_stability_transitions(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->shape_stability_transitions_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_jit_epoch_sync_hits(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_epoch_sync_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_deopt_targeted_skips(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->deopt_targeted_skips_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_concept_violations_caught(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->concept_violations_caught_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #795: deep hot-path Contracts + stronger
    // SoAView/ShapeStablePass Concepts + ShapeProfiler
    // JIT Epoch Sync + Dirty Propagation observability
    // bump helpers (Non-duplicative refinement of
    // #768/#507/#766/#767/#741). Called from the
    // planned Phase 2+ wire-up sites:
    // - bump_soa_view_violations_caught() in
    //   pass_manager.ixx + lowering/JIT
    //   run_incremental_dirty_pipeline when the
    //   SoAView concept static_assert catches a
    //   violation
    // - bump_shape_stable_pass_violations() in
    //   pass_manager.ixx + dominant_shape /
    //   ShapePropagationPass when the
    //   ShapeStablePass concept static_assert
    //   catches a violation
    // - bump_targeted_deopt_via_impact_scope() in
    //   shape_profiler.cpp deopt hook when #741
    //   impact_scope is consulted for targeted
    //   invalidation
    // - bump_on_compact_hook_invocation() in
    //   arena.ixx + ir_soa.ixx on_compact_hook_
    //   when Arena compact triggers shape_inval +
    //   dirty cascade
    void bump_soa_view_violations_caught(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->soa_view_violations_caught_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_shape_stable_pass_violations(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->shape_stable_pass_violations_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_targeted_deopt_via_impact_scope(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->targeted_deopt_via_impact_scope_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_on_compact_hook_invocation(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->on_compact_hook_invocations_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #806: registry-extension validation-pass counter
    // (P0 stdlib AI-native extension surface foundation; refines/
    // consolidates #775 Extension Kit + #711 + #480; non-duplicative
    // with #775 query:extension-kit-stats and #633 query:stdlib-
    // compiler-demands-stats-hash). Called from the planned
    // Phase 2+ wire-up sites:
    // - bump_registry_extension_validation_pass() in
    //   evaluator_primitives_registry.cpp when
    //   `(primitive:extend-registry-safe ...)` auto-validation
    //   pipeline runs the capture-contract probe + PrimMeta
    //   backfill + schema check + safety-flag check and the
    //   probe returns ok (then bumps +1)
    void bump_registry_extension_validation_pass(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->registry_extension_validation_passes_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #803: SEVA Long-Running Concurrent Verification
    // Evolution SLO bump helpers (P0 EDA-SV-verification-
    // production long-running concurrent multi-agent harness
    // foundation; consolidates/non-duplicates #794 + #755 + #773
    // + #774 + #802). Called from the planned Phase 2+ wire-up
    // sites:
    // - bump_seva_concurrent_ref_drift_prevented() in
    //   evaluator_fiber_mutation.cpp + ast.ixx stable-ref +
    //   guard-framework integration when a ref-drift is caught
    //   during a long-running concurrent SEVA round and
    //   prevented (i.e. StableNodeRef.refresh_if_stale + auto
    //   re-resolve succeeded within the round)
    // - bump_seva_concurrent_steal_during_verification_mutate()
    //   in evaluator_fiber_mutation.cpp when fiber steal
    //   fires during a verification mutate (mutation_stack_ +
    //   outermost MutationBoundaryGuard active during a SEVA
    //   round)
    // - bump_seva_concurrent_dirty_propagation_hits() in
    //   ast.ixx mark_dirty_upward + verify_dirty_ pass-mark
    //   during a SEVA round (no-fail signal — the inverse
    //   would be a dirty inconsistency violation)
    void bump_seva_concurrent_ref_drift_prevented(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->seva_concurrent_ref_drift_prevented_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_seva_concurrent_steal_during_verification_mutate(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->seva_concurrent_steal_during_verification_mutate_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_seva_concurrent_dirty_propagation_hits(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->seva_concurrent_dirty_propagation_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #772: SV Verification closed-loop SLO observability
    // counters backing the (query:sv-closedloop-slo) primitive.
    // These are public so future hardware_backend.ixx emit_sv_
    // verification_structured + sv_ir_impl.cpp dirty-triggered
    // incremental re-emit queue + eda:validate-sv-emit roundtrip
    // stub can call them at each decision point (emit parse
    // success/failure / re-emit latency max / SLO breach).
    //
    // The bump_sv_slo_reemit_latency_max_us helper uses compare-
    // exchange to maintain a high-water mark (only updates if the
    // new value exceeds the current max). The other helpers use
    // fetch_add for cumulative counters.
    void bump_sv_slo_emit_parse_success(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_slo_emit_parse_success_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_slo_emit_parse_failure(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_slo_emit_parse_failure_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_slo_reemit_latency_max_us(std::uint64_t us) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            auto cur = m->sv_slo_reemit_latency_max_us.load(std::memory_order_relaxed);
            while (us > cur) {
                if (m->sv_slo_reemit_latency_max_us.compare_exchange_weak(
                        cur, us, std::memory_order_relaxed))
                    break;
            }
        }
    }
    void bump_sv_slo_reemit_hits(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_slo_reemit_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_slo_breach(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_slo_breach_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #773: Workspace closed-loop fiber/multi-agent EDA
    // verification orchestration observability counters backing
    // the (query:workspace-closedloop-fiber-eda-stats) primitive.
    // These are public so future ast.ixx pin_for_cow() +
    // Workspace COW/clone/split + EDSL primitives yield
    // instrumentation + fiber/Guard steal/resume auto-refresh
    // can call them at each decision point (shared_mutex contention
    // time / multi-Agent edit fidelity / stale ref prevention).
    //
    // shared_mutex_contention_ns_total takes a nanosecond delta so
    // the caller can record the actual elapsed time without
    // tracking it locally. multi_agent_edit_fidelity_pct uses
    // 0-10000 fixed-point percent (× 100) — 9900 = 99.00%. The
    // stale_ref_prevented_eda_loops_total bump increments when a
    // cross-COW StableRef access is caught stale and refreshed.
    void bump_workspace_closedloop_shared_mutex_contention_ns(std::uint64_t ns) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->workspace_closedloop_shared_mutex_contention_ns_total.fetch_add(
                ns, std::memory_order_relaxed);
        }
    }
    void
    set_workspace_closedloop_multi_agent_edit_fidelity_pct(std::uint64_t pct_x100) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->workspace_closedloop_multi_agent_edit_fidelity_pct.store(pct_x100,
                                                                        std::memory_order_relaxed);
        }
    }
    void
    bump_workspace_closedloop_stale_ref_prevented_eda_loops(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->workspace_closedloop_stale_ref_prevented_eda_loops_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    // Issue #791: exhaustive fiber yield-point instrumentation
    // + automatic StableRef/dirty cross-boundary
    // propagation observability bump helpers (Refine
    // #773/#762 non-duplicative). Called from the
    // planned Phase 2+ wire-up sites:
    // - bump_workspace_closedloop_autoprop_ref() in
    //   workspace tree + is_valid_in / WeakRef registry
    //   paths when StableRefs are auto-propagated
    //   across COW/clone/split boundary
    // - bump_workspace_closedloop_autoprop_dirty() in
    //   mark_dirty_upward cross-boundary notification
    //   path when dirty bits are auto-propagated on
    //   COW/clone/split
    // - bump_workspace_closedloop_missed_yield() when
    //   a long walk missed a yield point (negative
    //   signal — high value = yield starvation under
    //   concurrent fiber load)
    void bump_workspace_closedloop_autoprop_ref(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->workspace_closedloop_autoprop_refs_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_workspace_closedloop_autoprop_dirty(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->workspace_closedloop_autoprop_dirty_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_workspace_closedloop_missed_yield(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->workspace_closedloop_missed_yield_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #792: compiler invalidate_function +
    // mutation_epoch_ synchronization observability
    // bump helpers (Refine #783/#755/#784/#787
    // non-duplicative). Called from the planned
    // Phase 2+ wire-up sites:
    // - bump_compiler_invalidate_deferred() in
    //   service.ixx invalidate_function when
    //   active MutationBoundaryGuard depth > 0
    //   (defer epoch bump / re-lower to post-yield
    //   boundary)
    // - bump_compiler_version_refresh_hit() in
    //   evaluator_fiber_mutation.cpp +
    //   apply_closure / materialize_call_env on
    //   steal resume / restore_post_yield_or_
    //   rollback (force bridge_epoch / EnvFrame
    //   version_ re-stamp)
    // - bump_compiler_guardshape_deopt_on_steal() in
    //   aura_jit_bridge.cpp + JIT hot-swap when
    //   bridge_epoch mismatch detected (trigger
    //   GuardShape deopt)
    // - bump_compiler_live_closure_stale_prevented()
    //   in apply_closure dual-path + bridge_epoch
    //   check (closure_bridge_ refresh prevents
    //   stale IRClosure reference)
    void bump_compiler_invalidate_deferred(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_invalidate_deferred_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_compiler_version_refresh_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_version_refresh_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_compiler_guardshape_deopt_on_steal(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_guardshape_deopt_on_steal_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_compiler_live_closure_stale_prevented(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_live_closure_stale_prevented_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #793: JIT/AOT hot-swap fidelity observability
    // bump helpers (Consolidate #785/#787/#755
    // non-duplicative). Called from the planned Phase
    // 2+ wire-up sites:
    // - bump_jit_deopt_forced_on_reload() in
    //   aura_jit.cpp + aura_jit_bridge.cpp hot-swap
    //   path on successful refcount swap / region
    //   reload when active fiber holds GuardShape/
    //   Apply on affected func
    // - bump_jit_linear_violation_prevented() in
    //   aura_jit.cpp JIT codegen for Linear* ops
    //   when runtime version_ probe or bridge_epoch
    //   compare catches a stale check
    // - bump_jit_env_version_sync_hit() in
    //   evaluator_fiber_mutation.cpp + apply_closure
    //   on steal resume / post-rollback when
    //   JIT-executed closure triggers EnvFrame
    //   version_ sync
    // - bump_jit_guardshape_stale_reject() in
    //   ir_executor.ixx + evaluator.ixx apply_
    //   closure bridge_epoch check when
    //   GuardShape expected_shape / shape_id
    //   mismatch is caught
    void bump_jit_deopt_forced_on_reload(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_deopt_forced_on_reload_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_jit_linear_violation_prevented(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_linear_violation_prevented_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_jit_env_version_sync_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_env_version_sync_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_jit_guardshape_stale_reject(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_guardshape_stale_reject_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #794: full closed-loop compiler + EDSL
    // fidelity observability bump helpers (Non-
    // duplicative to #786/#787/#755/#792/#793).
    // Called from the planned Phase 2+
    // tests/test_full_compiler_edsl_closedloop_
    // fidelity.cpp harness wire-up:
    // - bump_cross_layer_guardshape_deopt_hit() when
    //   the harness detects GuardShape expected vs
    //   runtime shape mismatch
    // - bump_cross_layer_linear_enforce_success()
    //   when linear_ownership_state is respected
    //   across compiler + EDSL boundary
    // - bump_cross_layer_epoch_sync() when
    //   EnvFrame version_ + bridge_epoch are
    //   synchronized across layers
    // - bump_cross_layer_drift_detection() when the
    //   harness detects any cross-layer drift
    //   (negative signal — high value = SLO breach)
    void bump_cross_layer_guardshape_deopt_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->cross_layer_guardshape_deopt_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_cross_layer_linear_enforce_success(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->cross_layer_linear_enforce_success_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_cross_layer_epoch_sync(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->cross_layer_epoch_sync_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_cross_layer_drift_detection(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->cross_layer_drift_detections_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #796: end-to-end IR SoA full migration +
    // DirtyAware short-circuit + DepGraph integration
    // observability bump helpers (Non-duplicative
    // extension of #766/#741). Called from the
    // planned Phase 2+ wire-up sites:
    // - bump_ir_soa_instructions_emitted() (already
    //   exists at line 5527 — duplicates removed)
    // - bump_ir_soa_dirty_block_skips() (already
    //   exists at line 5533 — duplicates removed)
    // - bump_ir_soa_jit_codegen_time_ns() (already
    //   exists at line 5551 — duplicates removed; the
    //   #796 primitive uses
    //   ir_soa_jit_codegen_time_ns_total atomic which
    //   is already exposed by the existing helper)
    // - bump_ir_soa_impact_dirty_hybrid_skip() in
    //   service.ixx invalidate_function when both
    //   DepGraph impact_scope + SoA block dirty
    //   are consulted together (only NEW helper,
    //   atomic + bump helper both added by #796)
    void bump_ir_soa_impact_dirty_hybrid_skip(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_soa_impact_dirty_hybrid_skips_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #723: Pass pipeline DirtyAware + Value v2 + Shape
    // history observability counters backing the (query:value-dispatch-
    // stats) primitive. These are public so future pass_manager.ixx
    // + value.ixx + value_tags.h + shape_profiler.cpp hot-path wiring
    // can call them at each decision point (dispatch call /
    // unknown tag / v2 string collision / shape history shift).
    void bump_value_dispatch_call() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->value_dispatch_calls_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_value_unknown_tag() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->value_unknown_tag_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_value_v2_string_collision() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->value_v2_string_collisions_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_shape_history_shift() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->shape_history_shift_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #726: closed-loop self-evolution reliability counters
    // backing the (query:closed-loop-reliability-stats) primitive.
    // These are public so future verify:parse-coverage-feedback /
    // parse-assert-failure / parse-formal-cex / mutate:from-
    // verification-feedback primitives + closed-loop controller
    // (seva:run-closed-loop) + enhanced subtree StableNodeRef
    // validation in MutationBoundaryGuard can call them at each
    // decision point (ref drift prevented / rollback success /
    // feedback mutate round completed).
    void bump_closed_loop_ref_drift_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->closed_loop_ref_drift_prevented_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_closed_loop_rollback_success() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->closed_loop_rollback_success_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_closed_loop_feedback_mutate_round() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->closed_loop_feedback_mutate_rounds_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #728: unified structured error + provenance + recovery
    // observability counters backing the (query:unified-error-stats)
    // primitive. These are public so future evaluator_primitives_*.cpp
    // refactors + new (primitive:error) / (with-error) / (primitive:try)
    // primitives + Guard auto-capture can call them at each decision
    // point (structured error constructed / provenance captured /
    // recovery succeeded). Pairs with #585 (query:primitives-error-stats)
    // but tracks the *unified* model specifically — structured
    // ErrorValue hits + provenance StableNodeRef capture + recovery
    // success — not the coarse error-rate / panic-recovery / rollback
    // surface.
    void bump_unified_error_structured_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->unified_error_structured_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_unified_error_provenance_captured() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->unified_error_provenance_captured_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_unified_error_recovery_success() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->unified_error_recovery_success_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #731: Arena + SoA + EnvFrame concurrent compaction safety
    // counters backing the (query:arena-concurrent-compact-stats)
    // primitive. These are public so future arena.ixx + gc_coordinator +
    // evaluator_gc.cpp concurrent compact / defrag success path + fiber.cpp
    // resume() / transfer hooks + panic checkpoint integration can call
    // them at each decision point (concurrent compact acquired / EnvFrame
    // revalidation completed / panic rollback fired on compact / race
    // prevented). Pairs with #722 (query:arena-integration-stats tier
    // integration) and #743 (Arena auto-compact policy + fiber safepoint)
    // but tracks the *concurrent* safety specifically — scheduler-safepoint
    // coordination + EnvFrame GCEnvWalkFn revalidation + panic-rollback-
    // compact integration + race prevention — not the coarse tier/
    // policy surface.
    void bump_arena_concurrent_compact() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_concurrent_compacts_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_arena_envframe_revalidation() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_envframe_revalidations_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_arena_panic_rollback_compact_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_panic_rollback_compact_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_arena_race_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_races_prevented_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1396: 4 unwired AOT hot-reload bump helpers
    // (bump_aot_safe_boundary_hit + 3 from #785) gated behind
    // AOT_RELOAD_PHASE_2_PLUS. Phase 1 ships the metric fields
    // in observability_metrics.h:5232-5234, 5298 + the query
    // primitives in evaluator_primitives_obs_jit_01.cpp:850 +
    // obs_jit_03.cpp:692-700; per-decision-point bump sites
    // are Phase 2+ wire-up work in aura_jit_bridge.cpp +
    // MutationBoundaryGuard + fiber.cpp + EnvFrame sync.
    // Defining the macro re-enables the helpers + the call-
    // site tests below skip cleanly via the same #ifdef.
    // Metric fields stay unconditionally (query primitives
    // return hashes with zero fields even when helpers are
    // absent). See tests/test_issue_1396.cpp for design doc.
#ifdef AOT_RELOAD_PHASE_2_PLUS
    // Issue #732: AOT hot-reload safe-swap at MutationBoundary
    // observability counter backing the
    // (query:aot-safe-swap-boundary-stats) primitive. Public so
    // future aura_jit_bridge.cpp aura_reload_aot_module +
    // MutationBoundaryGuard outermost exit hook + fiber.cpp
    // resume() / transfer hooks can call it at the safe-swap
    // decision point (reload successfully fired at outermost
    // MutationBoundary safe-swap point). Pairs with #708
    // (query:aot-reload-stats 5-7 field high-level summary)
    // + #644 (query:aot-reload-func-table-stats enforcement
    // with ref-bump / ref-decrement / region-reapply) + #590
    // (query:aot-hotupdate-stats 3 atomics) but tracks the
    // *safe-swap at MutationBoundary* specifically — reloads
    // that fired at the outermost safe-swap point (NOT mid-
    // mutation) — not the coarse reload summary / refcount
    // protocol / hot-update counters.
    void bump_aot_safe_boundary_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_safe_boundary_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #785: AOT concurrent hot-update observability
    // bump helpers. Called from the planned Phase 2+
    // wire-up sites:
    // - bump_aot_concurrent_steal_during_reload() in
    //   WorkerThread::steal() when steal is deferred
    //   due to active AOT reload on the victim
    // - bump_aot_grace_period_hit() in
    //   aura_reload_aot_module() before/after the
    //   refcount swap
    // - bump_aot_env_version_sync_on_reload() in
    //   aura_reload_aot_module() after the swap when
    //   EnvFrame version is bumped
    void bump_aot_concurrent_steal_during_reload() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_concurrent_steal_during_reload_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_aot_grace_period_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_grace_period_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_aot_env_version_sync_on_reload() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_env_version_sync_on_reload_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
#endif // AOT_RELOAD_PHASE_2_PLUS
    // Issue #733: macro marker propagation + IR/JIT hygiene
    // enforcement counters backing the (query:ir-marker-hygiene-stats)
    // primitive. These are public so future lowering_impl.cpp +
    // emit paths + ir_soa.ixx + aura_jit.cpp + aura_jit_runtime.cpp
    // + ir_executor.ixx can call them at each decision point
    // (IRInstruction creation from AST node propagates marker /
    // IRFunction marker set from root AST marker / marker-loss
    // detected at hot path / JIT conservative policy applied on
    // MacroIntroduced / marker-propagation successful across all
    // emit sites). Pairs with the existing #714 (query:self-evolution-
    // closedloop-stats — ref drift + rollback + feedback mutate
    // rounds) but tracks the *marker propagation + IR/JIT enforcement*
    // specifically — user-instrs vs macro-introduced-instrs split,
    // marker-loss events, JIT hygiene violations prevented, marker
    // propagation hits — not the coarse closed-loop reliability
    // surface.
    void bump_ir_marker_user_instr() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_marker_user_instrs_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_ir_marker_macro_introduced_instr() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_marker_macro_introduced_instrs_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_ir_marker_loss_event() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_marker_loss_events_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_ir_hygiene_jit_violation_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_hygiene_jit_violations_prevented_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_ir_hygiene_marker_propagation_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_hygiene_marker_propagation_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #735: MacroIntroduced provenance in StableNodeRef +
    // targeted dirty/rollback for macro subtrees observability
    // counters backing the (query:macro-provenance-stats) primitive.
    // These are public so future ast.ixx StableNodeRef + make_ref +
    // MutationBoundaryGuard + mark_dirty_upward + evaluator_primitives_
    // mutate.cpp can call them at each decision point (StableNodeRef
    // captured with macro_introduced_at_capture + original_macro_
    // expansion_id populated / dirty propagation targeted to macro
    // subtree / rollback success on macro subtree hygiene drift
    // detected / is_macro_introduced hot-path consult). Pairs with
    // #733 (query:ir-marker-hygiene-stats — IR-level marker
    // propagation) + #750 (query:reflection-schema-stats — runtime
    // reflection validate) but tracks the *MacroIntroduced
    // provenance + targeted macro-subtree handling* specifically —
    // capture-time provenance, hot-path consult, targeted dirty
    // propagation, rollback success — not the IR-marker propagation
    // or runtime reflection surface.
    void bump_macro_provenance_captured() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_provenance_captured_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_provenance_is_macro_introduced() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_provenance_is_macro_introduced_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_provenance_dirty_impact() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_provenance_dirty_impact_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_provenance_rollback_success() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_provenance_rollback_success_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #756: EnvFrame dual-path consistency enforcement +
    // desync panic policy + GCEnvWalkFn stale handling under
    // concurrent mutation/steal counters backing the
    // (query:envframe-dualpath-policy-stats) primitive. These
    // are public so future evaluator.ixx + evaluator_env.cpp +
    // gc_coordinator can call them at each decision point
    // (mandatory ensure_envframe_dual_path_consistency call in
    // walk_env_frames / GCEnvWalkFn / materialize_call_env /
    // post-rollback paths / desync panic / desync log-and-sync /
    // GCEnvWalkFn stale handling / concurrent steal/resume
    // re-ensure). Pairs with the existing #647 (query:envframe-
    // dualpath-stale-stats-hash — 3 fields: cross-fiber-stale /
    // version-mismatch / dualpath-repair) but tracks the *desync
    // panic policy + GCEnvWalkFn stale handling* specifically —
    // strict-panic vs log-and-sync mode + GC walk detected stale
    // under concurrency — not the coarse cross-fiber-stale +
    // version-mismatch + dualpath-repair surface.
    void bump_envframe_desync_panic() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->envframe_desync_panic_count_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_envframe_gc_stale_desync_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->envframe_gc_stale_desync_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #784: mandatory dual-path consistency
    // enforcement + concurrent steal/GC resync bump
    // helpers (refines #756). Called from the planned
    // (Phase 2+) mandatory ensure_ call sites in
    // walk_env_frames / GCEnvWalkFn /
    // materialize_call_env / post-rollback / fiber
    // steal resume. The P0 ships the bump helpers +
    // atomics + primitive; the actual call sites are
    // deferred (each is a separate session touching
    // evaluator.ixx + evaluator_env.cpp + gc_coordinator).
    void bump_envframe_mandatory_enforce() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->envframe_mandatory_enforce_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_envframe_mandatory_enforce_desync() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->envframe_mandatory_enforce_desync_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_envframe_concurrent_steal_resync() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->envframe_concurrent_steal_resync_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #757: fine-grained MacroIntroduced provenance
    // tracking + dynamic inliner policy + AI-queryable
    // hygiene violation correlation counters backing the
    // (query:macro-hygiene-provenance-stats) primitive. These
    // are public so future ast.ixx FlatAST + marker column +
    // query primitives + InlinePass in lowering + aura_jit +
    // MutationBoundaryGuard + macro_expansion.cpp can call
    // them at each decision point (provenance captured at
    // clone_macro_body / QueryExpr :marker MacroIntroduced
    // :provenance filter hits / hygiene:set-inliner-respect-
    // macro! primitive call / InlinePass respect_macro_hygiene_
    // dynamic check / hygiene_violation_by_macro correlation).
    // Pairs with the existing #654 (query:macro-hygiene-fiber-
    // panic-stats 5 fields) but tracks the *fine-grained
    // provenance + dynamic inliner policy + per-macro
    // correlation* specifically — provenance captured,
    // inliner policy violation firings, per-macro hygiene
    // violation correlation, query-filter hits — not the
    // coarse panic-restamp / provenance-violations /
    // macro-expand-checkpoints / reflect-hygiene-validation /
    // hygiene-dirty-impact surface.
    void bump_macro_hygiene_provenance_captured() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_hygiene_provenance_captured_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_hygiene_inliner_policy_violation() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_hygiene_inliner_policy_violations_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
        }
    }
    // Issue #1644: IR hygiene full-pipeline (refine #1047).
    // Bumped at the InlinePass respect_macro_hygiene_ cross-marker skip
    // sites (paired with the legacy per-Fiber macro_hygiene_skipped_ at
    // InlinePass namespace scope) + at the lowering_impl source_marker
    // propagation sites (where current_flat->marker(current_source_id) is
    // non-zero and is forwarded to blk.instructions.back().source_marker).
    void bump_ir_macro_introduced_inlined_skipped_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_macro_introduced_inlined_skipped_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_lowering_marker_propagated_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->lowering_marker_propagated_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Getters — for use by the (query:ir-marker-stats) primitive
    // composition layer (paired with the Issue #757 / #1637 / #1641
    // observability hook pattern).
    std::uint64_t ir_macro_introduced_inlined_skipped_total() const noexcept {
        return compiler_metrics_
                   ? static_cast<CompilerMetrics*>(compiler_metrics_)
                         ->ir_macro_introduced_inlined_skipped_total.load(std::memory_order_relaxed)
                   : 0;
    }
    std::uint64_t lowering_marker_propagated_total() const noexcept {
        return compiler_metrics_
                   ? static_cast<CompilerMetrics*>(compiler_metrics_)
                         ->lowering_marker_propagated_total.load(std::memory_order_relaxed)
                   : 0;
    }
    // Issue #1646: MutationBoundaryGuard long-running observability wiring
    // (refine #1637 / #1014). Bumped at the Guard dtor (success path) /
    // flush_mutation_boundary (deep propagation path) / epoch-bump-on-macro /
    // hygiene-violation-detection sites. Per Issue #1644 module-boundary
    // pattern with Evaluator::yield_hook_evaluator() null fallback.
    void bump_mutation_boundary_success_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutation_boundary_success_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_mutation_boundary_macro_dirty_propagated_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutation_boundary_macro_dirty_propagated_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        }
    }
    void bump_mutation_boundary_epoch_bump_for_macro_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutation_boundary_epoch_bump_for_macro_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_mutation_boundary_hygiene_violation_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutation_boundary_hygiene_violation_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Getters paired with the Issue #1637 / #1641 / #1644 observability
    // hook pattern — used by the (query:mutation-boundary-observability-
    // stats) primitive composition layer.
    std::uint64_t mutation_boundary_success_total() const noexcept {
        return compiler_metrics_
                   ? static_cast<CompilerMetrics*>(compiler_metrics_)
                         ->mutation_boundary_success_total.load(std::memory_order_relaxed)
                   : 0;
    }
    std::uint64_t mutation_boundary_macro_dirty_propagated_total() const noexcept {
        return compiler_metrics_ ? static_cast<CompilerMetrics*>(compiler_metrics_)
                                       ->mutation_boundary_macro_dirty_propagated_total.load(
                                           std::memory_order_relaxed)
                                 : 0;
    }
    std::uint64_t mutation_boundary_epoch_bump_for_macro_total() const noexcept {
        return compiler_metrics_ ? static_cast<CompilerMetrics*>(compiler_metrics_)
                                       ->mutation_boundary_epoch_bump_for_macro_total.load(
                                           std::memory_order_relaxed)
                                 : 0;
    }
    std::uint64_t mutation_boundary_hygiene_violation_total() const noexcept {
        return compiler_metrics_
                   ? static_cast<CompilerMetrics*>(compiler_metrics_)
                         ->mutation_boundary_hygiene_violation_total.load(std::memory_order_relaxed)
                   : 0;
    }
    // Issue #1647: StableNodeRef cross-boundary auto-refresh observability
    // pairing — per-CompilerMetrics bump at successful validate_or_refresh /
    // refresh_if_stale sites across COW / sub-workspace / fiber boundaries
    // (paired with the legacy namespace-scope bump_stable_ref_cross_cow_refresh
    // counter at the #1250 / #715 / #738 sites). Distinct from
    // stable_ref_auto_pin_total (pin-time) — this is refresh-success.
    void bump_cross_boundary_auto_refresh_success_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->cross_boundary_auto_refresh_success_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1649: composite mutate atomic batch + SyntaxMarker propagation
    // observability wiring (refine #1900 / #1502 / #1908). Bumped at the
    // atomic_batch_pinning commit site (Guard exit + template-respect site)
    // when a MacroIntroduced hygiene violation is prevented (distinct from
    // the legacy per-Fiber bump_atomic_batch_hygiene_violation pin-time counter).
    void bump_atomic_batch_hygiene_violation_prevented_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->atomic_batch_hygiene_violation_prevented_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        }
    }
    void bump_mutate_template_marker_propagated_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_template_marker_propagated_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::uint64_t atomic_batch_hygiene_violation_prevented_total() const noexcept {
        return compiler_metrics_ ? static_cast<CompilerMetrics*>(compiler_metrics_)
                                       ->atomic_batch_hygiene_violation_prevented_total.load(
                                           std::memory_order_relaxed)
                                 : 0;
    }
    std::uint64_t mutate_template_marker_propagated_total() const noexcept {
        return compiler_metrics_
                   ? static_cast<CompilerMetrics*>(compiler_metrics_)
                         ->mutate_template_marker_propagated_total.load(std::memory_order_relaxed)
                   : 0;
    }
    // Getter paired with the Issue #1637 / #1641 / #1644 / #1646
    // observability hook pattern — used by the (query:stable-ref-stats)
    // composition layer (no new primitive per #1632 "Aura 原语最小化").
    std::uint64_t cross_boundary_auto_refresh_success_total() const noexcept {
        return compiler_metrics_
                   ? static_cast<CompilerMetrics*>(compiler_metrics_)
                         ->cross_boundary_auto_refresh_success_total.load(std::memory_order_relaxed)
                   : 0;
    }
    // Issue #758: runtime auto_validate bridge for user-defined
    // EDSL structs under MutationBoundaryGuard with macro hygiene
    // invariant correlation counters backing the
    // (query:edsl-reflection-stats) primitive. These are public
    // so future reflect.hh + new runtime_reflect_edsl_bridge.cpp +
    // evaluator_primitives_mutate.cpp can call them at each
    // decision point (runtime_validate_edsl_struct call on
    // EDSL-tagged nodes pre-commit / auto_validate pass / fail /
    // hygiene invariants held / MacroIntroduced descendants verified
    // for valid provenance / hygiene invariant correlation /
    // dirty/epoch cascade on violation / mutation-impact-snapshot
    // correlation). Pairs with the existing #750 (query:reflection-
    // schema-stats — 4 fields: validated / hygiene-invariants-held /
    // schema-violations / stale-validation-prevented) but tracks
    // the *user-defined EDSL struct + macro hygiene invariant
    // correlation* specifically — per-type EDSL struct pass,
    // MacroIntroduced descendants verified for valid provenance,
    // per-type EDSL struct fail, macro_def_id-correlated violations
    // — not the general macro body schema validation surface.
    void bump_edsl_validated() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_validated_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_edsl_hygiene_invariants_held() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_hygiene_invariants_held_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_edsl_schema_fail_by_type() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_schema_fail_by_type_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_edsl_macro_correlated_violation() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_macro_correlated_violations_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #759: unified 'code-as-data' closed-loop maturity
    // metrics backing the (query:code-as-data-maturity-stats)
    // primitive. These are public so future
    // tests/test_task6_code_as_data_closedloop_harness.cpp +
    // SEVA demo + SLO deployment + clone_macro_body marker
    // propagation sampling wire-up + MutationBoundaryGuard
    // rollback hygiene-safe observation wire-up +
    // runtime_validate_edsl_struct macro/EDSL schema coverage
    // wire-up + Prometheus text/OTLP exporter can call them at
    // each decision point. Pairs with the existing #757
    // (macro-hygiene-provenance 2 new atomics: provenance-
    // captured + inliner-policy-violations) + #758 (edsl-
    // reflection 4 atomics: validated-edsl + hygiene-invariants-
    // held + schema-fail-by-type + macro-correlated-violations)
    // but tracks the *code-as-data closed-loop maturity
    // composite* — marker propagation fidelity (drift / samples),
    // Guard rollback hygiene safety (safe / attempts), reflection
    // schema coverage on macro/EDSL subtrees (covered / total),
    // concurrent fiber stress success — not the per-component
    // surfaces.
    void bump_code_as_data_fidelity_sample() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->code_as_data_fidelity_samples_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_code_as_data_fidelity_drift() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->code_as_data_fidelity_drift_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_code_as_data_rollback_hygiene_safe() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->code_as_data_rollback_hygiene_safe_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_code_as_data_reflect_schema_macro_edsl() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->code_as_data_reflect_schema_macro_edsl_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #760: query:pattern performance + hygiene fidelity
    // observability counters backing the
    // (query:pattern-performance-stats) primitive. These are
    // public so future query_matcher.cpp + evaluator_primitives_
    // query.cpp tag_arity_index_ hot-path + ... wildcard trie/
    // DFA + deep hygiene predicate (marker MacroIntroduced :
    // provenance macroX) + children_safe_view / StableNodeRef
    // pinning + MutationBoundaryGuard reader snapshot +
    // tests/test_query_pattern_indexing_hygiene_concurrent.cpp
    // harness + (query:pattern-explain node pattern) primitive
    // can call them at each decision point. Pairs with the
    // existing #757 (macro-hygiene-provenance 2 new atomics:
    // provenance-captured + inliner-policy-violations) + #758
    // (edsl-reflection 4 atomics: validated-edsl + hygiene-
    // invariants-held + schema-fail-by-type + macro-correlated-
    // violations) + #759 (code-as-data-maturity 4 atomics:
    // fidelity-samples + fidelity-drift + rollback-hygiene-safe
    // + reflect-schema-macro-edsl) but tracks the *query:pattern
    // performance + hygiene fidelity* specifically — linear scans
    // vs index hits (perf cliff detection), wildcard cost (early
    // exit / DFA benefit), hygiene filtered (deep hygiene predicate
    // activity), avg AST size sampled — not the per-component
    // surfaces.
    void bump_pattern_match_linear_scan() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_match_linear_scans_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_pattern_match_index_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_match_index_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_pattern_match_wildcard() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_match_wildcard_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_pattern_match_hygiene_filtered() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_match_hygiene_filtered_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #789: SafePCVSpan mandate + generation pin
    // check observability bump helpers (refine
    // #760). Called from the planned Phase 2+
    // query_matcher.cpp + evaluator_primitives_query.cpp
    // + ast.ixx children_safe_view wire-up sites:
    // - bump_pattern_safe_span_use() when children_
    //   safe_view / SafePCVSpan pin succeeds in
    //   matcher paths
    // - bump_pattern_dangling_prevented() when the
    //   generation pin check fires and prevents a
    //   potential UAF (mandate enforcement signal)
    void bump_pattern_safe_span_use() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_safe_span_uses_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_pattern_dangling_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_dangling_prevented_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #761: end-to-end atomic batch mutate primitives +
    // suppressed generation bump observability + cross-fiber
    // safety metrics for reliable multi-step AI iterative edits.
    // These are public so future evaluator_primitives_mutate.cpp
    // + ast.ixx + (mutate:batch [body]) or begin/commit primitives
    // + per-boundary atomic_batch_bumps_saved_ + cross-fiber
    // steal during suppressed batch re-stamp + tests/test_mutate_
    // batch_atomic_cross_fiber_safety.cpp harness can call them
    // at each decision point. Pairs with the existing #757
    // (macro-hygiene-provenance 2 new atomics) + #758 (edsl-
    // reflection 4 atomics) + #759 (code-as-data-maturity 4
    // atomics) + #760 (pattern-performance 4 atomics) but tracks
    // the *end-to-end atomic batch mutate + suppressed generation
    // bump + cross-fiber safety composite* — batch lifecycle,
    // suppressed bump count (churn saved), cross-fiber steals
    // during suppressed batch, hygiene violations caught within
    // batch boundary — not the per-component surfaces.
    void bump_mutate_batch_started() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_batches_started_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_mutate_suppressed_bump() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_suppressed_bumps_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_mutate_cross_fiber_steal_during_batch() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_cross_fiber_steals_during_batch_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_mutate_hygiene_violation_in_batch() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_hygiene_violations_in_batch_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #762: Workspace '锁定-导航-修改-执行' closed-loop
    // reliability observability under concurrent fiber orchestration
    // + multi-Agent parallel edits. These are public so future
    // evaluator_primitives_query.cpp + mutate.cpp + workspace paths
    // + restore_post_yield + steal paths + tests/test_workspace_
    // closedloop_fiber_multiagent_orchestration.cpp harness can
    // call them at each decision point. Pairs with the existing
    // #757 (macro-hygiene-provenance 2 new atomics) + #758
    // (edsl-reflection 4 atomics) + #759 (code-as-data-maturity
    // 4 atomics) + #760 (pattern-performance 4 atomics) + #761
    // (mutate-batch 4 atomics) but tracks the *Workspace closed-
    // loop orchestration* specifically — concurrent query/mutate
    // success under fiber steal, cross-COW StableRef validity
    // (auto-propagation win rate), yield point hit count
    // (exhaustive yield coverage), shared_mutex contention events
    // (orchestration bottleneck detection) — not the per-
    // component surfaces.
    void bump_workspace_closedloop_concurrent_query_mutate() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->workspace_closedloop_concurrent_query_mutate_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    void bump_workspace_closedloop_cross_cow_ref_valid() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->workspace_closedloop_cross_cow_ref_valid_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        }
    }
    void bump_workspace_closedloop_yield_point_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->workspace_closedloop_yield_points_hit_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_workspace_closedloop_shared_mutex_contention() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->workspace_closedloop_shared_mutex_contention_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    // Issue #763: runtime linear_ownership_state enforcement +
    // GC root registration for IRClosure/EnvFrame in invalidate_
    // function and live-closure paths. These are public so future
    // service.ixx invalidate_function + LoweringState walk of
    // live IRClosure + evaluator_gc.cpp + gc_coordinator root
    // registration hook + ir_executor.ixx + aura_jit.cpp Apply/
    // MakeClosure runtime check + tests/test_prompt6_linear_
    // ownership_gc_root_invalidate_closure.cpp harness can call
    // them at each decision point. Pairs with the existing #757
    // (macro-hygiene-provenance 2 new atomics) + #758 (edsl-
    // reflection 4 atomics) + #759 (code-as-data-maturity 4
    // atomics) + #760 (pattern-performance 4 atomics) + #761
    // (mutate-batch 4 atomics) + #762 (workspace-closedloop 4
    // atomics) but tracks the *compiler IRClosure + EnvFrame +
    // invalidate runtime linear enforcement composite* — root
    // registrations, stale root hits, runtime linear violations
    // caught, Env version re-syncs — not the per-component
    // surfaces.
    void bump_linear_ownership_gc_root_registration() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_ownership_gc_root_registrations_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_linear_ownership_gc_root_stale_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_ownership_gc_root_stale_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_linear_ownership_gc_violation_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_ownership_gc_violations_prevented_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        }
    }
    void bump_linear_ownership_gc_env_version_resync() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_ownership_gc_env_version_resync_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #764: Arena AST / shared_ptr<FlatAST> lifetime safety
    // vs GC-managed Env/Closure in closure_bridge_ under
    // incremental re-lower + mutation. These are public so future
    // service.ixx invalidate_function + LoweringState + evaluator_
    // gc.cpp + gc_coordinator + lowering_impl.cpp set_closure_
    // bridge_ptr + apply_closure + tests/test_prompt6_arena_ast_
    // sharedptr_closure_bridge_gc_lifetime.cpp harness can call
    // them at each decision point. Pairs with the existing #757
    // (macro-hygiene-provenance 2 new atomics) + #758 (edsl-
    // reflection 4 atomics) + #759 (code-as-data-maturity 4
    // atomics) + #760 (pattern-performance 4 atomics) + #761
    // (mutate-batch 4 atomics) + #762 (workspace-closedloop 4
    // atomics) + #763 (linear-ownership-gc-compiler 4 atomics) but
    // tracks the *compiler Arena AST / shared_ptr<FlatAST> lifetime
    // vs GC-managed Env/Closure in closure_bridge_* composite
    // specifically — arena AST root hits, bridge shared_ptr pinned,
    // cross-lifetime violations prevented, invalidate AST refresh
    // count — not the per-component surfaces.
    void bump_compiler_arena_closure_lifetime_root_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_arena_closure_lifetime_root_hits_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
        }
    }
    void bump_compiler_arena_closure_lifetime_bridge_sharedptr_pinned() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_arena_closure_lifetime_bridge_sharedptr_pinned_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    void bump_compiler_arena_closure_lifetime_cross_violation_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_arena_closure_lifetime_cross_violations_prevented_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    void bump_compiler_arena_closure_lifetime_invalidate_ast_refresh() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_arena_closure_lifetime_invalidate_ast_refresh_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    // Issue #799: DeadCoercionElimination + narrow_evidence elision stats.
    void bump_dead_coercion_elision_elided_casts(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dead_coercion_elision_elided_casts_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dead_coercion_elision_evidence_hits(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dead_coercion_elision_evidence_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dead_coercion_elision_narrowing_stable_paths(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dead_coercion_elision_narrowing_stable_paths_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_dead_coercion_elision_runtime_check_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dead_coercion_elision_runtime_check_savings_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_dead_coercion_elision_elided_casts() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->dead_coercion_elision_elided_casts_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_dead_coercion_elision_evidence_hits() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->dead_coercion_elision_evidence_hits_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_dead_coercion_elision_narrowing_stable_paths() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->dead_coercion_elision_narrowing_stable_paths_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_dead_coercion_elision_runtime_check_savings() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->dead_coercion_elision_runtime_check_savings_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #765: Full DepEntry quote/lambda tracking + impact_
    // scope propagation to bridge_epoch bump, EnvFrame version
    // re-stamp and linear state refresh in LoweringState/
    // invalidate. These are public so future ir_cache_pure.ixx
    // compute_dependencies + compute_impact_scope + service dep_
    // graph_ DepEntry quote/lambda flag + impact_scope priority +
    // service.ixx invalidate_function + LoweringState bridge_epoch
    // bump + EnvFrame version_ re-stamp + linear_ownership_state
    // re-emit + lowering_impl.cpp Variable + set_closure_bridge_
    // ptr + emit paths linear_state propagation + tests/test_
    // prompt2_6_dep_quote_lambda_impact_linear_bridge_env.cpp
    // harness can call them at each decision point. Pairs with the
    // existing #757 (macro-hygiene-provenance 2 new atomics) +
    // #758 (edsl-reflection 4 atomics) + #759 (code-as-data-
    // maturity 4 atomics) + #760 (pattern-performance 4 atomics) +
    // #761 (mutate-batch 4 atomics) + #762 (workspace-closedloop
    // 4 atomics) + #763 (linear-ownership-gc-compiler 4 atomics)
    // + #764 (compiler-arena-closure-lifetime 4 atomics) but
    // tracks the *incremental compilation safety for quote/
    // lambda/closure-heavy defines composite* — DepEntry quote/
    // lambda hit, bridge_epoch bump on impact, EnvFrame version
    // refresh, linear state refreshed — not the per-component
    // surfaces.
    void bump_incremental_quote_lambda_dep_hit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_quote_lambda_dep_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_incremental_quote_lambda_bridge_epoch_bump() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_quote_lambda_bridge_epoch_bump_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    void bump_incremental_quote_lambda_env_version_refresh() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_quote_lambda_env_version_refresh_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    void bump_incremental_quote_lambda_linear_state_refreshed() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->incremental_quote_lambda_linear_state_refreshed_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    // Issue #800: linear ownership post-mutate fidelity stats.
    void bump_linear_postmutate_post_rollback_revalidate(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_postmutate_post_rollback_revalidate_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_linear_postmutate_escape_violations_prevented(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_postmutate_escape_violations_prevented_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_linear_postmutate_guard_boundary_linear_safe(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_postmutate_guard_boundary_linear_safe_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_linear_postmutate_env_version_sync(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_postmutate_env_version_sync_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_linear_postmutate_post_rollback_revalidate() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->linear_postmutate_post_rollback_revalidate_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_linear_postmutate_escape_violations_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->linear_postmutate_escape_violations_prevented_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_linear_postmutate_guard_boundary_linear_safe() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->linear_postmutate_guard_boundary_linear_safe_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_linear_postmutate_env_version_sync() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->linear_postmutate_env_version_sync_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #798: ConstraintSystem incremental fidelity stats.
    void bump_type_incremental_cross_delta_blame_complete(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->type_incremental_cross_delta_blame_complete_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_type_incremental_reverify_truncated_under_guard(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->type_incremental_reverify_truncated_under_guard_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_type_incremental_epoch_sync_hits(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->type_incremental_epoch_sync_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_type_incremental_blame_chain_length(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->type_incremental_blame_chain_length_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_type_incremental_cross_delta_blame_complete() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->type_incremental_cross_delta_blame_complete_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t
    get_type_incremental_reverify_truncated_under_guard() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->type_incremental_reverify_truncated_under_guard_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_type_incremental_epoch_sync_hits() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->type_incremental_epoch_sync_hits_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_type_incremental_blame_chain_length() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->type_incremental_blame_chain_length_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #841: EDA production infrastructure stats.
    void bump_eda_infra_parse_success(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->eda_infra_parse_success_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_eda_infra_structured_mutate(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->eda_infra_structured_mutate_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_eda_infra_feedback_ingest(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->eda_infra_feedback_ingest_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_eda_infra_cosim_invoke(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->eda_infra_cosim_invoke_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_eda_infra_parse_success() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->eda_infra_parse_success_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_eda_infra_structured_mutate() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->eda_infra_structured_mutate_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_eda_infra_feedback_ingest() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->eda_infra_feedback_ingest_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_eda_infra_cosim_invoke() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->eda_infra_cosim_invoke_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #801: SV commercial emit fidelity stats.
    void record_sv_commercial_emit_fidelity(bool validation_ok, bool dirty_reemit,
                                            bool commercial_stub = true) const noexcept {
        if (!compiler_metrics_)
            return;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        if (dirty_reemit)
            m->sv_commercial_emit_dirty_reemit_total.fetch_add(1, std::memory_order_relaxed);
        if (validation_ok) {
            m->sv_commercial_emit_parse_success_total.fetch_add(1, std::memory_order_relaxed);
            if (commercial_stub)
                m->sv_commercial_emit_tool_compatible_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            m->sv_commercial_emit_roundtrip_mismatch_prevented_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    void bump_sv_commercial_emit_parse_success(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_commercial_emit_parse_success_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_commercial_emit_roundtrip_mismatch_prevented(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_commercial_emit_roundtrip_mismatch_prevented_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_sv_commercial_emit_dirty_reemit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_commercial_emit_dirty_reemit_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_commercial_emit_tool_compatible(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_commercial_emit_tool_compatible_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_sv_commercial_emit_parse_success() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_commercial_emit_parse_success_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t
    get_sv_commercial_emit_roundtrip_mismatch_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_commercial_emit_roundtrip_mismatch_prevented_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_commercial_emit_dirty_reemit() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_commercial_emit_dirty_reemit_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_commercial_emit_tool_compatible() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_commercial_emit_tool_compatible_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #802: SV verification self-evolution closed-loop stats.
    void bump_sv_self_evo_feedback_parse(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_self_evo_feedback_parse_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_self_evo_structured_mutate(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_self_evo_structured_mutate_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_self_evo_closed_loop_rounds(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_self_evo_closed_loop_rounds_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_self_evo_convergence_hits(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_self_evo_convergence_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #766: IR-SoA migration observability + DirtyAware
    // incremental pipeline counters backing the (query:ir-soa-
    // migration-stats) primitive. These are public so future
    // ir_soa.ixx IRFunctionSoA + IRModuleV2 add_instruction /
    // mark_block_dirty / mark_all_blocks_dirty + pass_manager.ixx
    // DirtyAwarePass + run_incremental_dirty_pipeline +
    // lowering_impl.cpp set_soa_emit_path + apply_soa_view +
    // evaluator_impl.cpp soa_interp_dispatch + aura_jit.cpp
    // emit_soa_function can call them at each decision point
    // (IR SoA column counts / DirtyAware short-circuit hits /
    // pmr column utilization / JIT SoA codegen time).
    //
    // The clean_block_hit_rate_pct and pmr_column_utilization_pct
    // bumps take a 0-10000 fixed-point percent (× 100) so the
    // primitive can report 100.00% as 10000 without float drift
    // under parallel update. The jit_codegen_time_ns bump takes
    // a nanosecond delta so the caller can record the actual
    // elapsed ns without tracking it locally.
    void bump_ir_soa_instructions_emitted(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_soa_instructions_emitted_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_ir_soa_dirty_block_skips(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_soa_dirty_block_skips_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void set_ir_soa_clean_block_hit_rate_pct(std::uint64_t pct_x100) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_soa_clean_block_hit_rate_pct.store(pct_x100, std::memory_order_relaxed);
        }
    }
    void set_ir_soa_pmr_column_utilization_pct(std::uint64_t pct_x100) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_soa_pmr_column_utilization_pct.store(pct_x100, std::memory_order_relaxed);
        }
    }
    void bump_ir_soa_jit_codegen_time_ns(std::uint64_t ns) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ir_soa_jit_codegen_time_ns_total.fetch_add(ns, std::memory_order_relaxed);
        }
    }
    // Issue #818: StableNodeRef full provenance + cross-COW enforcement.
    void bump_stable_ref_provenance_enforced(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_provenance_enforced_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_cross_cow_refresh(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_cross_cow_refresh_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_fiber_workspace_mismatch_prevented(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_fiber_workspace_mismatch_prevented_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_steal_auto_refresh(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_steal_auto_refresh_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #1497: boundary_pinned-specific refresh count (subset of
    // restamp_pinned_stable_refs that held boundary_pinned=true).
    void bump_boundary_pinned_refresh(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->boundary_pinned_refresh_count.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_boundary_pinned_refresh_count() const noexcept {
        if (!compiler_metrics_)
            return 0;
        return static_cast<CompilerMetrics*>(compiler_metrics_)
            ->boundary_pinned_refresh_count.load(std::memory_order_relaxed);
    }
    // Issue #805: registry + list-apply hot-path load samples.
    void bump_hotpath_registry_apply_sample(std::uint64_t ns,
                                            std::uint64_t linear_cost = 0) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->hotpath_registry_apply_samples_total.fetch_add(1, std::memory_order_relaxed);
            m->hotpath_registry_ns_accum_total.fetch_add(ns, std::memory_order_relaxed);
            if (linear_cost)
                m->hotpath_registry_linear_cost_total.fetch_add(linear_cost,
                                                                std::memory_order_relaxed);
        }
    }
    void bump_hotpath_registry_bench_run(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->hotpath_registry_bench_runs_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_hotpath_registry_extension_reg_ns(std::uint64_t ns) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->hotpath_registry_extension_reg_ns_total.fetch_add(ns, std::memory_order_relaxed);
        }
    }
    // Issues #809–#817 Phase 1 bump helpers
    void bump_error_policy_interop_conversion(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->error_policy_interop_conversions_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_error_policy_contract_as_aura_error(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->error_policy_contract_as_aura_error_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_fiber_init_aura_result_ok(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->fiber_init_aura_result_ok_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_fiber_init_aura_result_err(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->fiber_init_aura_result_err_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_scheduler_init_aura_result_ok(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->scheduler_init_aura_result_ok_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_scheduler_init_aura_result_err(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->scheduler_init_aura_result_err_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_jit_guest_exception_bridge(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_guest_exception_bridge_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_jit_internal_aura_result_path(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_internal_aura_result_path_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_steal_arena_yield_during_compact(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->steal_arena_yield_during_compact_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_steal_outermost_only_enforced(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->steal_outermost_only_enforced_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_steal_linear_probe_on_success(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->steal_linear_probe_on_success_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_guard_aura_result_path(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->guard_aura_result_path_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_guard_panic_checkpoint_aura_result(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->guard_panic_checkpoint_aura_result_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_runtime_self_heal(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->runtime_self_heal_invocations_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_runtime_health_drift_detected(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->runtime_health_drift_detected_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_macro_ir_source_marker_stamp(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_ir_source_marker_stamps_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_macro_provenance_query(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_provenance_query_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_edsl_define_struct(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_define_struct_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_edsl_define_struct_validate_pass(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_define_struct_validate_pass_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_edsl_define_struct_validate_fail(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_define_struct_validate_fail_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dirty_epoch_macro_introduced_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dirty_epoch_macro_introduced_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dirty_epoch_targeted_relower(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dirty_epoch_targeted_relower_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dirty_epoch_hygiene_drift_prevented(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dirty_epoch_hygiene_drift_prevented_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issues #819–#829 Phase 1 bumps
    void bump_pattern_hygiene_provenance_predicate_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_hygiene_provenance_predicate_hits_total.fetch_add(n,
                                                                         std::memory_order_relaxed);
        }
    }
    void bump_pattern_hygiene_index_enforced_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_hygiene_index_enforced_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_pattern_hygiene_yield_enforced(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_hygiene_yield_enforced_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_pattern_hygiene_safe_span_enforced(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_hygiene_safe_span_enforced_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_mutate_batch_e2e_started(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_batch_e2e_started_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_mutate_batch_e2e_suppressed_bumps(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_batch_e2e_suppressed_bumps_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_mutate_batch_e2e_hygiene_in_batch(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_batch_e2e_hygiene_in_batch_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_mutate_batch_e2e_cross_fiber_steals(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_batch_e2e_cross_fiber_steals_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_mutate_batch_e2e_pinned_snapshot(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_batch_e2e_pinned_snapshot_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_mutate_batch_e2e_panic_recoveries(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->mutate_batch_e2e_panic_recoveries_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_jit_fiber_ex_stack_local(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_fiber_ex_stack_local_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_jit_fiber_ex_cross_prevented(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_fiber_ex_cross_prevented_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_jit_fiber_ex_deopt_interpreter(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->jit_fiber_ex_deopt_interpreter_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_l2_spec_pair_fastpath(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->l2_spec_pair_fastpath_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_l2_spec_deopt_version(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->l2_spec_deopt_version_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_l2_spec_guardshape_narrow(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->l2_spec_guardshape_narrow_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_l2_spec_linear_probe(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->l2_spec_linear_probe_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_opcode_cov_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->opcode_cov_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_opcode_cov_unhandled_hot(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->opcode_cov_unhandled_hot_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_opcode_cov_per_fn_deopt(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->opcode_cov_per_fn_deopt_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_term_render_clear(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->term_render_clear_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_term_render_draw_batch(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->term_render_draw_batch_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_term_render_present(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->term_render_present_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_term_render_dirty_region(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->term_render_dirty_region_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_term_render_present_ns(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->term_render_present_ns_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_ffi_batch_call(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_ffi_batch_calls_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_ffi_zerocopy_view(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_ffi_zerocopy_views_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_ffi_crossing_ns(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_ffi_crossing_ns_accum_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_ffi_allocs_frame(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_ffi_allocs_frame_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_hp_dirty_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_hp_dirty_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_hp_present_delta(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_hp_present_delta_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_hp_jit_coverage(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_hp_jit_coverage_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_hp_mutation_impact(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_hp_mutation_impact_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_contract_hotpath_check(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_contract_hotpath_checks_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_consteval_dispatch_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_consteval_dispatch_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_stability_transition(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_stability_transitions_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_irsoa_enforce_dirty_skip(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->irsoa_enforce_dirty_skips_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_irsoa_enforce_impact_hybrid(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->irsoa_enforce_impact_hybrid_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_irsoa_enforce_pmr_util_pct(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->irsoa_enforce_pmr_util_pct.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_irsoa_enforce_relower_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->irsoa_enforce_relower_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_arena_ldefrag_auto_trigger(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_ldefrag_auto_triggers_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_arena_ldefrag_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_ldefrag_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_arena_ldefrag_fiber_yield(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_ldefrag_fiber_yield_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_arena_ldefrag_shape_inval(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_ldefrag_shape_inval_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_arena_ldefrag_pointer_fixup(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_ldefrag_pointer_fixup_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Open-issues Phase 1 bumps (bulk)
    void bump_pass_shape_epoch(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pass_shape_epoch_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_pass_shape_epoch_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pass_shape_epoch_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_pass_shape_epoch_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pass_shape_epoch_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_edsl_hotpath_real(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_hotpath_real_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_edsl_hotpath_real_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_hotpath_real_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_edsl_hotpath_real_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_hotpath_real_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dead_coercion_elim(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dead_coercion_elim_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dead_coercion_elim_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dead_coercion_elim_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dead_coercion_elim_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dead_coercion_elim_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_occurrence_renarrow(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->occurrence_renarrow_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_occurrence_renarrow_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->occurrence_renarrow_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_occurrence_renarrow_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->occurrence_renarrow_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_linear_escape_mutate(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_escape_mutate_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_linear_escape_mutate_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_escape_mutate_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_linear_escape_mutate_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->linear_escape_mutate_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_typed_mutate_coercion(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typed_mutate_coercion_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_typed_mutate_coercion_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typed_mutate_coercion_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_typed_mutate_coercion_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typed_mutate_coercion_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_fiber_epoch_type(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->fiber_epoch_type_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_fiber_epoch_type_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->fiber_epoch_type_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_fiber_epoch_type_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->fiber_epoch_type_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_feedback_mutate(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_feedback_mutate_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_feedback_mutate_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_feedback_mutate_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_feedback_mutate_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_feedback_mutate_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_seva_harness_v2(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->seva_harness_v2_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_seva_harness_v2_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->seva_harness_v2_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_seva_harness_v2_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->seva_harness_v2_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_typed_mut_audit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typed_mut_audit_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_typed_mut_audit_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typed_mut_audit_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_typed_mut_audit_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typed_mut_audit_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_full_v2(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_full_v2_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_full_v2_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_full_v2_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_full_v2_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_full_v2_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_longrun_ai_infra(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrun_ai_infra_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_longrun_ai_infra_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrun_ai_infra_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_longrun_ai_infra_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrun_ai_infra_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_ai_native_meta(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ai_native_meta_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_ai_native_meta_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ai_native_meta_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_ai_native_meta_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->ai_native_meta_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_orch_telemetry(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->orch_telemetry_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_orch_telemetry_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->orch_telemetry_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_orch_telemetry_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->orch_telemetry_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_per_fiber_ex_state(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->per_fiber_ex_state_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_per_fiber_ex_state_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->per_fiber_ex_state_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_per_fiber_ex_state_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->per_fiber_ex_state_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Issue #1483 C2: bump_per_fiber_mutation_stack_depth_max —
    // CAS-loop update of the lifetime max across all observed
    // fibers. Wired into cp.mutation_stack_depth assignment
    // sites (L186 / L316 / L450 of evaluator_fiber_mutation.cpp).
    // The current_max is NOT updated here (that one tracks
    // only live fibers — see bump_per_fiber_mutation_stack_depth_current_max).
    void bump_per_fiber_mutation_stack_depth_max(std::size_t depth) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            std::uint64_t cur =
                m->per_fiber_mutation_stack_depth_max.load(std::memory_order_relaxed);
            while (cur < static_cast<std::uint64_t>(depth)) {
                if (m->per_fiber_mutation_stack_depth_max.compare_exchange_weak(
                        cur, static_cast<std::uint64_t>(depth), std::memory_order_relaxed))
                    break;
            }
            // Issue #1493: sample depth into histogram (every observation).
            note_mutation_stack_depth_histogram(depth);
        }
    }
    // Issue #1493: depth histogram buckets 0,1,2,3,4,5-7,8-15,16+.
    void note_mutation_stack_depth_histogram(std::size_t depth) const noexcept {
        if (!compiler_metrics_)
            return;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        std::size_t bucket = 7; // 16+
        if (depth == 0)
            bucket = 0;
        else if (depth == 1)
            bucket = 1;
        else if (depth == 2)
            bucket = 2;
        else if (depth == 3)
            bucket = 3;
        else if (depth == 4)
            bucket = 4;
        else if (depth <= 7)
            bucket = 5;
        else if (depth <= 15)
            bucket = 6;
        m->mutation_stack_depth_histogram[bucket].fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1493: avg mutation hold → gc_frequency_tune_ratio adaptive.
    // Longer holds → raise ratio (more frequent safepoint checks).
    // Short holds → decay ratio toward default 50.
    void adapt_gc_frequency_from_hold_us(std::uint64_t hold_us) const noexcept {
        if (!compiler_metrics_)
            return;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        const auto thr = m->long_mutation_threshold_us.load(std::memory_order_relaxed);
        auto cur = aura_gc_frequency_tune_ratio_load();
        if (thr > 0 && hold_us > thr) {
            // AC2: shorter safepoint interval under long mutation.
            const auto next = cur >= 100 ? 100u : std::min(100u, cur + 10u);
            if (next > cur) {
                aura_gc_frequency_tune_ratio_store(next);
                m->safepoint_frequency_adapt_up_total.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (thr > 0 && hold_us < thr / 10 && cur > 50) {
            const auto next = cur <= 50 ? 50u : std::max(50u, cur - 5u);
            if (next < cur) {
                aura_gc_frequency_tune_ratio_store(next);
                m->safepoint_frequency_adapt_down_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    // Issue #1483 C2: bump_per_fiber_mutation_stack_depth_current_max —
    // monotonic-CAS update of the current (resettable) max across
    // live fibers. Wire at fiber enter sites; reset at fiber exit
    // sites via reset_per_fiber_mutation_stack_depth_current_max
    // (the reset helper is intentionally omitted in C2 — the
    // current_max tracks "max observed this compile session"
    // and resets on Evaluator construction).
    void bump_per_fiber_mutation_stack_depth_current_max(std::size_t depth) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            std::uint64_t cur =
                m->per_fiber_mutation_stack_depth_current_max.load(std::memory_order_relaxed);
            while (cur < static_cast<std::uint64_t>(depth)) {
                if (m->per_fiber_mutation_stack_depth_current_max.compare_exchange_weak(
                        cur, static_cast<std::uint64_t>(depth), std::memory_order_relaxed))
                    break;
            }
        }
    }
    [[nodiscard]] std::uint64_t get_per_fiber_mutation_stack_depth_max() const noexcept {
        if (!compiler_metrics_)
            return 0;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        return m->per_fiber_mutation_stack_depth_max.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_per_fiber_mutation_stack_depth_current_max() const noexcept {
        if (!compiler_metrics_)
            return 0;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        return m->per_fiber_mutation_stack_depth_current_max.load(std::memory_order_relaxed);
    }
    // Issue #1483 C4: bump_safepoint_adaptive_threshold — CAS-loop
    // exponential backoff. Doubles the current threshold on each
    // call, capped at kSafepointAdaptiveThresholdMax (1024). The
    // CAS retry pattern matches bump_per_fiber_mutation_stack_depth_max
    // from C2 (CAS under contention is benign — the doubling only
    // depends on "is current < target").
    void bump_safepoint_adaptive_threshold() const noexcept {
        if (!compiler_metrics_)
            return;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        constexpr std::uint64_t kMax = 1024;
        std::uint64_t cur = m->safepoint_adaptive_threshold.load(std::memory_order_relaxed);
        while (cur < kMax) {
            const std::uint64_t next = (cur == 0) ? 1 : (cur * 2);
            const std::uint64_t capped = next > kMax ? kMax : next;
            if (m->safepoint_adaptive_threshold.compare_exchange_weak(cur, capped,
                                                                      std::memory_order_relaxed))
                break;
        }
    }
    // Issue #1483 C4: reset_safepoint_adaptive_threshold — zeroes
    // the threshold (called when an immediate safepoint succeeds
    // and the pressure signal drops).
    void reset_safepoint_adaptive_threshold() const noexcept {
        if (!compiler_metrics_)
            return;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->safepoint_adaptive_threshold.store(0, std::memory_order_relaxed);
    }
    // Issue #1483 C4: bump_safepoint_adaptive_defer_count — increments
    // the adaptive-defer counter (distinct from the natural
    // mutation_boundary_depth > 0 defer path).
    void bump_safepoint_adaptive_defer_count() const noexcept {
        if (!compiler_metrics_)
            return;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->safepoint_adaptive_defer_count.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_safepoint_adaptive_threshold() const noexcept {
        if (!compiler_metrics_)
            return 0;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        return m->safepoint_adaptive_threshold.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_safepoint_adaptive_defer_count() const noexcept {
        if (!compiler_metrics_)
            return 0;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        return m->safepoint_adaptive_defer_count.load(std::memory_order_relaxed);
    }
    // Issue #1483 C4: should_adapt_safepoint_threshold — returns true
    // when the adaptive threshold should force deferral at the
    // immediate path. The heuristic (per #1483 plan (a) exponential
    // backoff): defer when threshold > 0 AND current per-fiber
    // mutation_stack_depth > threshold. This is the "pressure
    // signal" — if fibers are already deep, an immediate safepoint
    // would cause excessive drift, so we back off.
    [[nodiscard]] bool should_adapt_safepoint_threshold() const noexcept {
        const std::uint64_t threshold = get_safepoint_adaptive_threshold();
        if (threshold == 0)
            return false;
        const std::uint64_t pressure = get_per_fiber_mutation_stack_depth_current_max();
        return pressure > threshold;
    }
    void bump_aot_hotswap_pipe(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_hotswap_pipe_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_aot_hotswap_pipe_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_hotswap_pipe_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_aot_hotswap_pipe_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->aot_hotswap_pipe_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_macro_hyg_query_v2(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_hyg_query_v2_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_macro_hyg_query_v2_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_hyg_query_v2_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_macro_hyg_query_v2_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_hyg_query_v2_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_reflect_edsl_v2(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflect_edsl_v2_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_reflect_edsl_v2_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflect_edsl_v2_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_reflect_edsl_v2_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflect_edsl_v2_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_selfevo_hyg_dirty(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->selfevo_hyg_dirty_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_selfevo_hyg_dirty_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->selfevo_hyg_dirty_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_selfevo_hyg_dirty_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->selfevo_hyg_dirty_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_fb_closedloop(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_fb_closedloop_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_fb_closedloop_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_fb_closedloop_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_fb_closedloop_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_fb_closedloop_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_pattern_defuse_hyg(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_defuse_hyg_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_pattern_defuse_hyg_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_defuse_hyg_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_pattern_defuse_hyg_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->pattern_defuse_hyg_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_mutlog(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_mutlog_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_mutlog_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_mutlog_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stable_ref_mutlog_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stable_ref_mutlog_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dirty_impact_v2(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dirty_impact_v2_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dirty_impact_v2_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dirty_impact_v2_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dirty_impact_v2_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dirty_impact_v2_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_live_irclosure_gc(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->live_irclosure_gc_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_live_irclosure_gc_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->live_irclosure_gc_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_live_irclosure_gc_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->live_irclosure_gc_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_src_marker_linear(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->src_marker_linear_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_src_marker_linear_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->src_marker_linear_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_src_marker_linear_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->src_marker_linear_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_term_buf_diff(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->term_buf_diff_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_term_buf_diff_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->term_buf_diff_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_term_buf_diff_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->term_buf_diff_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_obs_v2(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_obs_v2_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_obs_v2_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_obs_v2_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_obs_v2_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_obs_v2_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_jit_soa(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_jit_soa_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_jit_soa_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_jit_soa_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_render_jit_soa_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->render_jit_soa_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_arena_ldefrag_v2(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_ldefrag_v2_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_arena_ldefrag_v2_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_ldefrag_v2_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_arena_ldefrag_v2_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->arena_ldefrag_v2_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_irsoa_dirty_v2(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->irsoa_dirty_v2_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_irsoa_dirty_v2_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->irsoa_dirty_v2_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_irsoa_dirty_v2_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->irsoa_dirty_v2_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_val_shape_ceval_v2(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->val_shape_ceval_v2_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_val_shape_ceval_v2_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->val_shape_ceval_v2_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_val_shape_ceval_v2_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->val_shape_ceval_v2_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_defuse_infer_part(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->defuse_infer_part_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_defuse_infer_part_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->defuse_infer_part_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_defuse_infer_part_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->defuse_infer_part_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_own_escape_post(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->own_escape_post_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_own_escape_post_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->own_escape_post_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_own_escape_post_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->own_escape_post_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_typed_audit_pass(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typed_audit_pass_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_typed_audit_pass_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typed_audit_pass_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_typed_audit_pass_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typed_audit_pass_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_backend_bi(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_backend_bi_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_backend_bi_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_backend_bi_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_backend_bi_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_backend_bi_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_large_sv_pattern(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->large_sv_pattern_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_large_sv_pattern_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->large_sv_pattern_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_large_sv_pattern_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->large_sv_pattern_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_longrun_sref_dirty(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrun_sref_dirty_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_longrun_sref_dirty_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrun_sref_dirty_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_longrun_sref_dirty_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrun_sref_dirty_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_eda_prims(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_eda_prims_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_eda_prims_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_eda_prims_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_eda_prims_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_eda_prims_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_prim_quota_fiber(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->prim_quota_fiber_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_prim_quota_fiber_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->prim_quota_fiber_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_prim_quota_fiber_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->prim_quota_fiber_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_decl_prim_reg(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->decl_prim_reg_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_decl_prim_reg_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->decl_prim_reg_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_decl_prim_reg_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->decl_prim_reg_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_prim_ns_alias(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->prim_ns_alias_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_prim_ns_alias_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->prim_ns_alias_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_prim_ns_alias_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->prim_ns_alias_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_guard_steal_gc_v2(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->guard_steal_gc_v2_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_guard_steal_gc_v2_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->guard_steal_gc_v2_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_guard_steal_gc_v2_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->guard_steal_gc_v2_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dirty_ircache_cons(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dirty_ircache_cons_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dirty_ircache_cons_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dirty_ircache_cons_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_dirty_ircache_cons_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->dirty_ircache_cons_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stats_builder_ref(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stats_builder_ref_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stats_builder_ref_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stats_builder_ref_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_stats_builder_ref_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->stats_builder_ref_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_load_or_zero_help(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->load_or_zero_help_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_load_or_zero_help_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->load_or_zero_help_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_load_or_zero_help_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->load_or_zero_help_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_cpp26_mod_sweep(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->cpp26_mod_sweep_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_cpp26_mod_sweep_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->cpp26_mod_sweep_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_cpp26_mod_sweep_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->cpp26_mod_sweep_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_metrics_meta_refl(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->metrics_meta_refl_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_metrics_meta_refl_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->metrics_meta_refl_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_metrics_meta_refl_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->metrics_meta_refl_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_harness_boot(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_harness_boot_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_harness_boot_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_harness_boot_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_harness_boot_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_harness_boot_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_bundle_codegen_dec(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->bundle_codegen_dec_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_bundle_codegen_dec_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->bundle_codegen_dec_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_bundle_codegen_dec_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->bundle_codegen_dec_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_bundle_mig(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_bundle_mig_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_bundle_mig_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_bundle_mig_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_bundle_mig_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_bundle_mig_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_profile_flag(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_profile_flag_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_profile_flag_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_profile_flag_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_profile_flag_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_profile_flag_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_harness_mod(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_harness_mod_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_harness_mod_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_harness_mod_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_harness_mod_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_harness_mod_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_json_report(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_json_report_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_json_report_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_json_report_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_test_json_report_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->test_json_report_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_gcc16_modules_env(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->gcc16_modules_env_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_gcc16_modules_env_hit(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->gcc16_modules_env_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_gcc16_modules_env_savings(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->gcc16_modules_env_savings_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_stable_ref_provenance_enforced() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->stable_ref_provenance_enforced_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_stable_ref_cross_cow_refresh() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->stable_ref_cross_cow_refresh_hits_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_stable_ref_fiber_workspace_mismatch_prevented() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->stable_ref_fiber_workspace_mismatch_prevented_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_stable_ref_steal_auto_refresh() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->stable_ref_steal_auto_refresh_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_self_evo_feedback_parse() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_self_evo_feedback_parse_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_self_evo_structured_mutate() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_self_evo_structured_mutate_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_self_evo_closed_loop_rounds() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_self_evo_closed_loop_rounds_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_self_evo_convergence_hits() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_self_evo_convergence_hits_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    void bump_macro_hygiene_dirty_impact() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->macro_hygiene_dirty_impact_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_macro_hygiene_panic_restamp_from_workspace() const noexcept {
        auto* ws = workspace_flat_;
        if (!ws)
            return;
        for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
            if (ws->is_macro_introduced(id)) {
                bump_macro_hygiene_panic_restamp();
                return;
            }
        }
    }
    // Issue #655: EDSL core stability observability.
    void bump_edsl_cow_stable_ref_remap() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_cow_stable_ref_remap_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_edsl_tag_arity_delta_patch() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_tag_arity_delta_patch_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_edsl_nested_atomic_rollback() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_nested_atomic_rollback_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_edsl_mutate_invalidate_precision() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->edsl_mutate_invalidate_precision_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #657: compiler core incremental self-mod observability.
    void bump_compiler_core_bridge_epoch_sync() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_core_bridge_epoch_sync_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_compiler_core_jit_unhandled_invalidate() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_core_jit_unhandled_invalidate_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_compiler_core_linear_metadata_flow() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_core_linear_metadata_flow_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void bump_compiler_core_quote_fallback_refresh() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->compiler_core_quote_fallback_refresh_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #673: Unified Runtime Observability Layer (P1) — cross-module
    // correlation atomics. Bumped from the existing
    // aura_evaluator_bump_mutation_steal_attempt /
    // aura_evaluator_bump_steal_deferred_violation / and from
    // probe_linear_ownership_on_fiber_steal (when violation == true).
    // All three are no-ops when compiler_metrics_ is null (early startup
    // / test fixtures without metrics backing).
    void bump_runtime_observability_steal_attempt_correlated() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->runtime_observability_steal_attempt_correlated_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    void bump_runtime_observability_steal_deferred_correlated() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->runtime_observability_steal_deferred_correlated_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    void bump_runtime_observability_steal_ownership_violation_correlated() noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->runtime_observability_steal_ownership_violation_correlated_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t
    get_runtime_observability_steal_attempt_correlated() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->runtime_observability_steal_attempt_correlated_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t
    get_runtime_observability_steal_deferred_correlated() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->runtime_observability_steal_deferred_correlated_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t
    get_runtime_observability_steal_ownership_violation_correlated() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->runtime_observability_steal_ownership_violation_correlated_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #674: Closed-loop self-evolution chaos stress observability.
    // The 3 bump helpers below back the (query:self-evolution-chaos-
    // stats) primitive. They are wired ONLY from the chaos test harness
    // (test_self_evolution_chaos_stable_674) — they are NOT bumped on
    // the production mutation path, so the production counter set
    // (cross_fiber_rollback_count_, panic_checkpoint_size_mismatch_,
    // envframe_desync_detected_, etc.) is preserved unchanged.
    //
    // All three are no-ops when compiler_metrics_ is null.
    void bump_self_evolution_chaos_cycles(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->self_evolution_chaos_cycles_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_self_evolution_chaos_failures(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->self_evolution_chaos_failures_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_self_evolution_chaos_corruptions(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->self_evolution_chaos_corruptions_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_self_evolution_chaos_cycles() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->self_evolution_chaos_cycles_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_self_evolution_chaos_failures() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->self_evolution_chaos_failures_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_self_evolution_chaos_corruptions() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->self_evolution_chaos_corruptions_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #661: SV InterfaceIR/ModportIR structure observability
    // (P1 EDA-SV foundation). The 3 bump helpers back the
    // (query:sv-interface-structure-stats) primitive. Wired into
    // `eda:parse-netlist`'s interface/modport parse paths (and
    // available for any future `eda:set-port-direction` primitive
    // from Action #3 in the issue body).
    //
    // All three are no-ops when compiler_metrics_ is null.
    void bump_sv_interface_ports(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_interface_ports_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_interface_modport_views(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_interface_modport_views_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_interface_direction_changes(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_interface_direction_changes_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_sv_interface_ports_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_interface_ports_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_interface_modport_views_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_interface_modport_views_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_interface_direction_changes_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_interface_direction_changes_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #664: SV DefUse incremental observability (P1). The 3
    // bump helpers back the (query:sv-defuse-stats) primitive.
    // (1) nested_modports: bumped when DefUse build discovers a
    //     Modport child of an Interface (nested modport at depth
    //     >= 1).
    // (2) cross_refs: bumped when a use-record resolves to an
    //     Interface/Modport symbol defined in another scope
    //     (cross-interface / cross-modport reference).
    // (3) incremental_updates: bumped per DefUse incremental
    //     rebuild triggered by an SV structural mutate
    //     (vs. a full rebuild).
    //
    // Initially the bump sites are the test-fixture callers + the
    // planned production wiring (issue body Actions #1 + #2).
    // The follow-up wiring for Actions #1 + #2 is separate work.
    void bump_sv_defuse_nested_modports(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_defuse_nested_modports_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_defuse_cross_refs(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_defuse_cross_refs_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_defuse_incremental_updates(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_defuse_incremental_updates_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_sv_defuse_nested_modports() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_defuse_nested_modports_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_defuse_cross_refs() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_defuse_cross_refs_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_defuse_incremental_updates() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_defuse_incremental_updates_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #665: SV stability observability (P1 EDA-SV scalability).
    // The 3 bump helpers + 3 accessors back the (query:sv-stability-
    // stats) primitive. Wired into mark_dirty_upward (SV-tagged path),
    // generation_wrap detection (SV-tagged path), and StableRef
    // invalidation (SV-tagged path). Initially bumped from test
    // fixtures + planned production wiring for issue body Action #1
    // (early-exit / special-case SV tags in mark_dirty_upward) +
    // Action #2 (compact_nodes / restore subtree-gen scope for SV).
    void bump_sv_dirty_traversal_depth(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_dirty_traversal_depth_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_generation_wrap(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_generation_wrap_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_sv_stable_ref_invalidation(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->sv_stable_ref_invalidation_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_sv_dirty_traversal_depth() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_dirty_traversal_depth_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_generation_wrap() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_generation_wrap_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_sv_stable_ref_invalidation() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->sv_stable_ref_invalidation_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #667: list/map/filter apply hot-path observability
    // (P1 stdlib-impl performance). The 3 bump helpers + 3
    // accessors back the (query:primitives-apply-stats)
    // primitive. Wired into the list/map/filter apply_unary /
    // apply_pred / apply_binary helpers (the per-element
    // hot path). Initially bumped from test fixtures +
    // production wiring in evaluator_primitives_list.cpp.
    void bump_primitives_apply_lookup_hits(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->primitives_apply_lookup_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_primitives_apply_closure_calls(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->primitives_apply_closure_calls_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_primitives_apply_fastpath_wins(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->primitives_apply_fastpath_wins_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_primitives_apply_lookup_hits() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->primitives_apply_lookup_hits_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_primitives_apply_closure_calls() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->primitives_apply_closure_calls_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_primitives_apply_fastpath_wins() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->primitives_apply_fastpath_wins_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #752: list/vector map/filter SoA hot-path observability.
    // Bump helpers + accessors back (query:list-soa-hotpath-stats).
    void bump_list_chain_traversals(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->list_chain_traversals_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_list_soa_hits(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->list_soa_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_list_intrinsic_dispatches(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->list_intrinsic_dispatches_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_list_estimated_cache_misses(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->list_estimated_cache_misses_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_list_chain_traversals() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->list_chain_traversals_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_list_soa_hits() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->list_soa_hits_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_list_intrinsic_dispatches() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->list_intrinsic_dispatches_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_list_estimated_cache_misses() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->list_estimated_cache_misses_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #753: long-running deployment infra observability.
    void set_resource_quota_memory(std::uint64_t limit) noexcept { resource_quota_memory_ = limit; }
    // Issue #1498: cumulative arena heap budget (used + request > limit → reject).
    // Independent of per-request resource_quota_memory_ (0 = unlimited).
    void set_resource_quota_memory_total(std::uint64_t limit) noexcept {
        resource_quota_memory_total_ = limit;
    }
    [[nodiscard]] std::uint64_t resource_quota_memory_total() const noexcept {
        return resource_quota_memory_total_;
    }
    // Live arena.stats().used when arena_ is set (0 otherwise).
    [[nodiscard]] std::uint64_t resource_quota_current_usage() const noexcept {
        if (!arena_)
            return 0;
        return static_cast<std::uint64_t>(arena_->stats().used);
    }
    void set_resource_quota_fibers(std::uint64_t limit) noexcept {
        resource_quota_fibers_ = limit;
        // Issue #1579: mirror into process ResourceQuota so scheduler spawn enforces.
        aura::core::resource_quota::process_resource_quota().set_limit(
            aura::core::resource_quota::Dimension::Fibers, limit);
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->resource_quota_max_fibers.store(limit, std::memory_order_relaxed);
    }
    // Issue #1547: host-set mutation budget for try_acquire / check_mutation_quota.
    void set_resource_quota_mutations(std::uint64_t limit) noexcept {
        resource_quota_mutations_ = limit;
        // Issue #1618: mirror into process ResourceQuota Mutations dim
        // so ResourceQuotaManager sees a unified mutation budget.
        aura::core::resource_quota::process_resource_quota().set_limit(
            aura::core::resource_quota::Dimension::Mutations, limit);
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->resource_quota_max_mutations.store(limit, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t resource_quota_mutations() const noexcept {
        return resource_quota_mutations_;
    }
    [[nodiscard]] std::uint64_t mutation_quota_used() const noexcept {
        return mutation_quota_used_.load(std::memory_order_relaxed);
    }
    void reset_mutation_quota_used() noexcept {
        mutation_quota_used_.store(0, std::memory_order_relaxed);
    }
    void set_resource_quota_time_us(std::uint64_t limit) noexcept {
        resource_quota_time_us_ = limit;
    }
    [[nodiscard]] std::uint64_t resource_quota_memory() const noexcept {
        return resource_quota_memory_;
    }
    [[nodiscard]] std::uint64_t resource_quota_fibers() const noexcept {
        return resource_quota_fibers_;
    }
    [[nodiscard]] std::uint64_t resource_quota_time_us() const noexcept {
        return resource_quota_time_us_;
    }
    // Issue #1481: typed-error ResourceQuota enforcement helpers.
    //
    // Each helper returns std::nullopt if the request fits within
    // the quota (or if no quota is set — limit == 0 means unlimited),
    // or std::optional<AuraError> with AuraErrorKind::ResourceQuotaExceeded
    // if the request would exceed. Bumps resource_quota_checks_total on
    // every call + resource_quota_rejects_total on rejection. Callers
    // that want strict quota enforcement call these BEFORE the actual
    // allocation / spawn / mutation commit and surface the returned
    // AuraError via make_unexpected().
    //
    // Pattern: existing callers that don't care about typed errors
    // (e.g. arena.allocate_raw) keep working unchanged. Strict new
    // callers (e.g. allocate_checked / MutationBoundaryGuard::try_acquire)
    // route through these helpers and propagate the error.
    // Issue #1481: typed-error ResourceQuota helpers.
    // nullopt = within quota (or unlimited); AuraError on reject.
    // Issue #1618: typed quota reject bookkeeping — ResourceQuotaExceeded
    // path (not PanicCheckpoint). Distinguishes quota from generic panic
    // for long-running agent recovery.
    void note_quota_reject_typed(bool mutation_budget = false) noexcept {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        if (!m)
            return;
        m->resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
        m->quota_violation_total.fetch_add(1, std::memory_order_relaxed);
        m->quota_reject_typed_total.fetch_add(1, std::memory_order_relaxed);
        m->panic_quota_distinguished_total.fetch_add(1, std::memory_order_relaxed);
        m->longrunning_quota_violations_total.fetch_add(1, std::memory_order_relaxed);
        if (mutation_budget)
            m->mutation_budget_rejected_total.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::optional<aura::core::AuraError>
    check_arena_quota(std::uint64_t requested_bytes) noexcept {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        if (m)
            m->resource_quota_checks_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1481: per-request size cap (0 = unlimited).
        if (resource_quota_memory_ != 0 && requested_bytes > resource_quota_memory_) {
            note_quota_reject_typed(false);
            return aura::core::AuraError{aura::core::AuraErrorKind::ResourceQuotaExceeded,
                                         std::string("arena quota exceeded: requested ") +
                                             std::to_string(requested_bytes) + " bytes > limit " +
                                             std::to_string(resource_quota_memory_) +
                                             " [reason=mutation_budget_exceeded dim=memory]"};
        }
        // Issue #1498: cumulative heap budget — production AI loops need
        // total used + request, not only single-allocation size.
        if (resource_quota_memory_total_ != 0 && arena_) {
            const auto used = static_cast<std::uint64_t>(arena_->stats().used);
            if (used + requested_bytes > resource_quota_memory_total_) {
                note_quota_reject_typed(false);
                return aura::core::AuraError{
                    aura::core::AuraErrorKind::ResourceQuotaExceeded,
                    std::string("arena cumulative quota exceeded: used ") + std::to_string(used) +
                        " + requested " + std::to_string(requested_bytes) + " > total limit " +
                        std::to_string(resource_quota_memory_total_) +
                        " [reason=mutation_budget_exceeded dim=memory]"};
            }
        }
        return std::nullopt;
    }

    // Issue #1481 / #1547 / #1618: typed-error mutation budget check.
    // pending_count = mutations about to enter (default 1 per try_acquire).
    // resource_quota_mutations_ == 0 → unlimited (default; matches #1481 AC5).
    // On reject: ResourceQuotaExceeded (typed) — never PanicCheckpoint.
    [[nodiscard]] std::optional<aura::core::AuraError>
    check_mutation_quota(std::uint64_t pending_count = 1) noexcept {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        if (m)
            m->resource_quota_checks_total.fetch_add(1, std::memory_order_relaxed);
        if (resource_quota_mutations_ == 0)
            return std::nullopt;
        const auto used = mutation_quota_used_.load(std::memory_order_relaxed);
        if (used + pending_count <= resource_quota_mutations_)
            return std::nullopt;
        note_quota_reject_typed(true);
        // Issue #1618: provenance from process manager (host may set)
        // + defuse_version of outermost active boundary when present.
        std::uint64_t mut_id =
            aura::core::resource_quota::process_resource_quota_manager().provenance_mutation_id;
        if (mut_id == 0) {
            auto& stack = active_mutation_stack();
            if (!stack.empty())
                mut_id = stack.back().version; // defuse_version at boundary entry
        }
        std::string msg = std::string("mutation quota exceeded: used ") + std::to_string(used) +
                          " + pending " + std::to_string(pending_count) + " > limit " +
                          std::to_string(resource_quota_mutations_) +
                          " [reason=mutation_budget_exceeded dim=mutations]";
        if (mut_id != 0) {
            msg += " provenance_mutation_id=";
            msg += std::to_string(mut_id);
        }
        return aura::core::AuraError{aura::core::AuraErrorKind::ResourceQuotaExceeded,
                                     std::move(msg)};
    }

    // Issue #1579: real fiber quota — uses process ResourceQuota fibers_used
    // against resource_quota_fibers_ (Evaluator host limit). Also mirrors
    // limit into process_resource_quota for scheduler spawn enforcement.
    [[nodiscard]] std::optional<aura::core::AuraError> check_fiber_quota() noexcept {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        if (m)
            m->resource_quota_checks_total.fetch_add(1, std::memory_order_relaxed);
        // Mirror host limit → process quota (scheduler reads process quota).
        if (resource_quota_fibers_ != 0) {
            aura::core::resource_quota::process_resource_quota().set_limit(
                aura::core::resource_quota::Dimension::Fibers, resource_quota_fibers_);
            if (m)
                m->resource_quota_max_fibers.store(resource_quota_fibers_,
                                                   std::memory_order_relaxed);
        }
        if (resource_quota_fibers_ == 0)
            return std::nullopt;
        const auto used = aura::core::resource_quota::process_resource_quota().used(
            aura::core::resource_quota::Dimension::Fibers);
        if (used < resource_quota_fibers_)
            return std::nullopt;
        note_quota_reject_typed(false);
        return aura::core::AuraError{aura::core::AuraErrorKind::ResourceQuotaExceeded,
                                     std::string("fiber quota exceeded: used ") +
                                         std::to_string(used) + " >= limit " +
                                         std::to_string(resource_quota_fibers_) +
                                         " [reason=mutation_budget_exceeded dim=fibers]"};
    }

    [[nodiscard]] std::optional<aura::core::AuraError>
    check_time_quota(std::uint64_t elapsed_us) noexcept {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        if (m)
            m->resource_quota_checks_total.fetch_add(1, std::memory_order_relaxed);
        if (resource_quota_time_us_ == 0)
            return std::nullopt;
        if (elapsed_us <= resource_quota_time_us_)
            return std::nullopt;
        note_quota_reject_typed(false);
        return aura::core::AuraError{aura::core::AuraErrorKind::ResourceQuotaExceeded,
                                     std::string("time quota exceeded: elapsed ") +
                                         std::to_string(elapsed_us) + "us > limit " +
                                         std::to_string(resource_quota_time_us_) + "us" +
                                         " [reason=mutation_budget_exceeded dim=time_us]"};
    }

    // Issue #1481 / #1546 / #1554: typed-error arena allocation.
    // Delegates to ASTArena::allocate_checked so quota is enforced once
    // (no double-count of resource_quota_checks_total on the hot path).
    [[nodiscard]] aura::core::AuraResult<void*>
    allocate_checked(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) noexcept {
        if (!arena_) {
            return std::unexpected(
                aura::core::AuraError{aura::core::AuraErrorKind::InternalInvariantViolation,
                                      std::string("allocate_checked called with null arena")});
        }
        // Prefer arena typed factory (owner callback → check_arena_quota once).
        if (arena_->has_arena_owner())
            return arena_->allocate_checked(size, alignment);
        // Orphan / pre-bind path: explicit Evaluator check then try_allocate
        // (try_allocate has no owner so it will not re-check).
        if (auto err = check_arena_quota(static_cast<std::uint64_t>(size)))
            return std::unexpected(std::move(*err));
        void* ptr = arena_->try_allocate(size);
        if (!ptr) {
            return std::unexpected(
                aura::core::AuraError{aura::core::AuraErrorKind::ArenaOutOfMemory,
                                      std::string("allocate_checked: arena try_allocate failed")});
        }
        (void)alignment;
        return ptr;
    }

    void bump_longrunning_quota_violations(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrunning_quota_violations_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_longrunning_checkpoint_success(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrunning_checkpoint_success_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_longrunning_heal_triggers(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrunning_heal_triggers_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_longrunning_resource_trend(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrunning_resource_trend_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_longrunning_deployment_slo_hits(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrunning_deployment_slo_hits_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_longrunning_quota_violations() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->longrunning_quota_violations_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_longrunning_checkpoint_success() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->longrunning_checkpoint_success_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_longrunning_heal_triggers() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->longrunning_heal_triggers_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_longrunning_resource_trend() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->longrunning_resource_trend_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_longrunning_deployment_slo_hits() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->longrunning_deployment_slo_hits_total.load(std::memory_order_relaxed);
        }
        return 0;
    }

    // Issue #1583 / #1207: recovery latency stall budget (µs). Default 5000.
    enum class RecoveryLatencyKind : std::uint8_t { PanicRestore = 0, QuotaReject = 1 };
    void set_recovery_stall_budget_us(std::uint64_t us) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->longrunning_recovery_stall_budget_us.store(us, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t recovery_stall_budget_us() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->longrunning_recovery_stall_budget_us.load(std::memory_order_relaxed);
        }
        return 5000;
    }
    // Record one recovery sample; bumps histogram, max, and stall violations.
    void record_recovery_latency_us(std::uint64_t us, RecoveryLatencyKind kind) noexcept {
        if (!compiler_metrics_)
            return;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->longrunning_recovery_latency_us_total.fetch_add(us, std::memory_order_relaxed);
        m->longrunning_recovery_samples.fetch_add(1, std::memory_order_relaxed);
        if (kind == RecoveryLatencyKind::PanicRestore)
            m->longrunning_recovery_panic_samples.fetch_add(1, std::memory_order_relaxed);
        else
            m->longrunning_recovery_quota_samples.fetch_add(1, std::memory_order_relaxed);
        // 9-bucket histogram (same edges as mutation hold #1375).
        std::size_t bucket = 8;
        if (us < 100)
            bucket = 0;
        else if (us < 500)
            bucket = 1;
        else if (us < 1000)
            bucket = 2;
        else if (us < 5000)
            bucket = 3;
        else if (us < 10000)
            bucket = 4;
        else if (us < 50000)
            bucket = 5;
        else if (us < 100000)
            bucket = 6;
        else if (us < 1000000)
            bucket = 7;
        m->longrunning_recovery_histogram[bucket].fetch_add(1, std::memory_order_relaxed);
        auto prev_max = m->longrunning_recovery_latency_us_max.load(std::memory_order_relaxed);
        while (us > prev_max && !m->longrunning_recovery_latency_us_max.compare_exchange_weak(
                                    prev_max, us, std::memory_order_relaxed)) {
        }
        const auto budget = m->longrunning_recovery_stall_budget_us.load(std::memory_order_relaxed);
        if (us > budget)
            m->longrunning_recovery_stall_violations_total.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_recovery_stall_violations() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->longrunning_recovery_stall_violations_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_recovery_latency_samples() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->longrunning_recovery_samples.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Approximate p50/p99 from the 9-bucket recovery histogram (upper edges).
    [[nodiscard]] std::uint64_t recovery_latency_p50_us() const noexcept {
        return recovery_latency_percentile_us(50);
    }
    [[nodiscard]] std::uint64_t recovery_latency_p99_us() const noexcept {
        return recovery_latency_percentile_us(99);
    }
    [[nodiscard]] std::uint64_t recovery_latency_percentile_us(int pct) const noexcept {
        if (!compiler_metrics_)
            return 0;
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        std::uint64_t counts[CompilerMetrics::kLongrunningRecoveryHistBuckets]{};
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < CompilerMetrics::kLongrunningRecoveryHistBuckets; ++i) {
            counts[i] = m->longrunning_recovery_histogram[i].load(std::memory_order_relaxed);
            total += counts[i];
        }
        if (total == 0)
            return 0;
        // Upper edges of buckets (µs).
        static constexpr std::uint64_t kEdges[CompilerMetrics::kLongrunningRecoveryHistBuckets] = {
            100, 500, 1000, 5000, 10000, 50000, 100000, 1000000, 2000000};
        const std::uint64_t target = (total * static_cast<std::uint64_t>(pct) + 99) / 100; // ceil
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < CompilerMetrics::kLongrunningRecoveryHistBuckets; ++i) {
            cum += counts[i];
            if (cum >= target)
                return kEdges[i];
        }
        return kEdges[CompilerMetrics::kLongrunningRecoveryHistBuckets - 1];
    }

    // Issue #754: LLM-bottleneck orchestration GC safepoint self-tuning.
    void bump_orchestration_llm_gc_safepoint_adapted(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->orchestration_llm_gc_safepoint_adapted_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_orchestration_llm_gc_safepoint_adapted() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->orchestration_llm_gc_safepoint_adapted_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #755: end-to-end concurrent safety full-cycle integration stats.
    void bump_concurrent_safety_steal_boundary_success(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->concurrent_safety_steal_boundary_success_total.fetch_add(n,
                                                                        std::memory_order_relaxed);
        }
    }
    void bump_concurrent_safety_aot_reload_at_guard(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->concurrent_safety_aot_reload_at_guard_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_concurrent_safety_gc_safepoint_during_steal(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->concurrent_safety_gc_safepoint_during_steal_total.fetch_add(
                n, std::memory_order_relaxed);
        }
    }
    void bump_concurrent_safety_recovery_success(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->concurrent_safety_recovery_success_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t get_concurrent_safety_steal_boundary_success() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->concurrent_safety_steal_boundary_success_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_concurrent_safety_aot_reload_at_guard() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->concurrent_safety_aot_reload_at_guard_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_concurrent_safety_gc_safepoint_during_steal() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->concurrent_safety_gc_safepoint_during_steal_total.load(
                std::memory_order_relaxed);
        }
        return 0;
    }
    [[nodiscard]] std::uint64_t get_concurrent_safety_recovery_success() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->concurrent_safety_recovery_success_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    // Issue #668: math regex primitive error observability.
    // Bumped alongside primitive_error_count_ (the general
    // counter) every time a regex-* primitive returns PRIM_ERROR.
    [[nodiscard]] std::uint64_t get_primitives_regex_error_total() const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            return m->primitives_regex_error_total.load(std::memory_order_relaxed);
        }
        return 0;
    }
    void bump_primitives_regex_error_total(std::uint64_t n = 1) noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->primitives_regex_error_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_total_query_calls() noexcept {
        total_query_calls_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #583: registry observability for query:primitives-stats.
    [[nodiscard]] std::size_t get_primitive_slot_count() const noexcept {
        return primitives_.slot_count();
    }
    // Issue #478: primitive error observability.
    [[nodiscard]] std::uint64_t get_primitive_error_count() const noexcept {
        return primitive_error_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_primitive_error_values_size() const noexcept {
        return error_values_.size();
    }
    void bump_primitive_error_count() noexcept {
        primitive_error_count_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #804: per-unified-error-path bump helpers (P0
    // stdlib reliability foundation; refines/consolidates
    // #585 + #751 + #775 + #478). Called from the planned
    // Phase 2+ wire-up sites:
    // - bump_primitive_error_with_provenance() in
    //   primitives_detail.h PRIM_ERROR / make_primitive_error
    //   when the (kind, msg, provenance) schema is filled
    //   in (the *good* path the body asks for — 100% of
    //   primitives should hit this path; 0 silent fallbacks)
    // - bump_primitive_error_silent_fallback() in the
    //   Phase 2+ audit grep-step when ad-hoc returns
    //   (make_int(0) / void / catch-all on bad args) are
    //   detected in evaluator_primitives_*.cpp
    // - bump_primitive_error_recovery_hook() in
    //   evaluator_fiber_mutation.cpp Guard + retry path
    //   when a recovery-hook firing happens in response to
    //   a structured error
    void bump_primitive_error_with_provenance(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->primitive_error_with_provenance_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_primitive_error_silent_fallback(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->primitive_error_silent_fallback_total.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void bump_primitive_error_recovery_hook(std::uint64_t n = 1) const noexcept {
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->primitive_error_recovery_hook_invocations_total.fetch_add(n,
                                                                         std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::atomic<std::uint64_t>* primitive_error_counter_ptr() noexcept {
        return &primitive_error_count_;
    }
    // Issue #480: primitive metadata observability.
    [[nodiscard]] std::uint64_t get_primitive_describe_count() const noexcept {
        return primitive_describe_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_primitive_list_meta_count() const noexcept {
        return primitive_list_meta_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t get_primitive_documented_meta_count() const noexcept {
        return primitives_.documented_meta_count();
    }
    // Issue #778: FFI call count accessor — exposes the
    // coverage_counters_[8] slot that all FFI primitives
    // (c-load, c-func, c-opaque, c-alloc, c-struct-set!,
    // c-struct-ref) increment. Used by the
    // (query:ffi-call-overhead-stats, schema 778) primitive
    // to surface FFI call volume to the observability
    // surface. The FFI call closure is NOT counted here
    // (it's applied through the regular apply_closure
    // path, not through coverage_counters_).
    [[nodiscard]] std::uint64_t get_ffi_call_count() const noexcept {
        return coverage_counters_[8];
    }
    void bump_primitive_describe_count() noexcept {
        primitive_describe_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_primitive_list_meta_count() noexcept {
        primitive_list_meta_count_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] const ast::FlatAST* tag_arity_index_workspace() const noexcept {
        return tag_arity_index_workspace_;
    }
    [[nodiscard]] std::size_t tag_arity_index_synced_size() const noexcept {
        return tag_arity_index_synced_size_;
    }
    [[nodiscard]] std::uint16_t tag_arity_index_synced_gen() const noexcept {
        return tag_arity_index_synced_gen_;
    }
    [[nodiscard]] std::size_t tag_arity_index_entry_count() const noexcept {
        // Issue #371: shared_lock for read parity with
        // query:pattern. Iterating the map while invalidate
        // .clear()s it on another fiber is UB (the bucket
        // pointer is freed mid-iteration). rlock pairs with
        // the unique_lock in invalidate_tag_arity_index.
        std::shared_lock<std::shared_mutex> rlock(tag_arity_index_mtx_);
        std::size_t total = 0;
        for (const auto& [_, bucket] : tag_arity_index_)
            total += bucket.size();
        return total;
    }
    // Force a build of the index (for tests that need to
    // verify the index without going through query:pattern).
    // The index is normally lazy-built on first use, so
    // this is only needed for direct unit tests of the
    // index itself.
    void force_build_tag_arity_index() const {
        build_tag_arity_index(static_cast<std::uint8_t>(PatternIndexRebuildTrigger::LazyQuery));
    }
    // Issue #1372 / #1501: build (if needed) + copy bucket under a
    // single unique_lock. When skip_macro_introduced is true, serve
    // the user-only hygiene index (no MacroIntroduced roots).
    [[nodiscard]] std::vector<ast::NodeId> snapshot_tag_arity_bucket(
        std::uint64_t key,
        std::uint8_t trigger = static_cast<std::uint8_t>(PatternIndexRebuildTrigger::LazyQuery),
        bool skip_macro_introduced = false) const;
    // Issue #1372: race-window hits (0 with snapshot path). Exposed
    // via query:pattern-index-stats-hash "race-window-hits".
    [[nodiscard]] std::uint64_t get_tag_arity_index_race_window_hits() const noexcept {
        return tag_arity_index_race_window_hits_.load(std::memory_order_relaxed);
    }
    // Issue #1501: hygiene index serve counter.
    [[nodiscard]] std::uint64_t get_tag_arity_hygiene_index_served() const noexcept {
        return tag_arity_hygiene_index_served_total_.load(std::memory_order_relaxed);
    }
    // Test accessors for setting the workspace directly
    // (bypassing the Aura primitive pipeline, which is
    // tested separately).
    void set_workspace_flat_for_test(ast::FlatAST* f) { set_workspace_flat(f); }
    void invalidate_tag_arity_index_for_test() { invalidate_tag_arity_index(); }
    ast::FlatAST* workspace_flat_for_test() const { return workspace_flat_; }
    // Expose the arena allocator so tests can build
    // workspace FlatASTs.
    ast::ASTArena& test_arena() { return *arena_; }
    // ── Issue #177: explicit Mutation Boundary + per-fiber checkpoint ───
    //
    // The MutationCheckpoint + enter/exit APIs establish a
    // structured scope around mutate:* operations. A checkpoint
    // captures the defuse_version_ at the boundary entry, so
    // the evaluator can roll back to that point on failure
    // (panic, GC safepoint abort, or explicit rollback request).
    //
    // The boundary is per-fiber (thread_local stack) so concurrent
    // fibers each have their own checkpoint stack. This composes
    // with the existing workspace_mtx_ shared_mutex: a fiber
    // inside a boundary holds the exclusive write lock; the
    // checkpoint provides a safe rollback point if the fiber
    // yields (yield is a fiber-level operation; the workspace
    // lock is released on yield).
    //
    // Usage (callers will migrate to this API over time):
    //   enter_mutation_boundary();        // captures + version++
    //   if (some_panic_condition) {
    //       exit_mutation_boundary(false);  // rollback
    //   } else {
    //       exit_mutation_boundary(true);   // commit
    //   }
    //
    // Performance: minimal (the boundary only fires on the
    // mutate path, which is already mutex-protected; the
    // checkpoint capture is a single uint64_t read + push).
    //
    // Current state: API surface + the actual rollback path
    // (Issue #213 Cycle 1). The checkpoint captures both the
    // defuse_version_ and the mutation log size at entry; on
    // exit_mutation_boundary(false), the log is rolled back to
    // the checkpoint size, the defuse_index_ is invalidated,
    // and the defuse_version_ is bumped again so any pending
    // readers see the rolled-back state.
    struct MutationCheckpoint {
        std::uint64_t version;             // defuse_version_ at boundary entry
        std::size_t mutation_log_size = 0; // FlatAST::mutation_log_.size() at entry
        // Issue #679: atomic-batch + macro marker snapshot at outermost
        // boundary entry for rollback realignment.
        bool bump_suppressed_at_entry = false;
        std::uint64_t macro_introduced_count_at_entry = 0;
        std::uint16_t flat_generation_at_entry = 0;
        // Issue #221: snapshot of FlatAST::children_ (the per-node
        // PersistentChildVector list). On rollback (exit(false)),
        // the captured vector is reinstalled in workspace_flat_->
        // children_. The PCV's COW semantics make the snapshot
        // cheap (each PCV is a shared_ptr copy — the underlying
        // storage is kept alive by the shared_ptr in the
        // checkpoint). Back-references to the pre-mutation PCs
        // (e.g. closures that captured children[id] pre-mutation)
        // stay valid because the checkpoint holds a shared_ptr to
        // each pre-mutation PCV.
        std::vector<aura::ast::PersistentChildVector<aura::ast::NodeId>> children_snapshot;
        // Issue #266: optional column snapshots for bulk sym/param
        // mutations (mutate:rename-symbol). Captured only when
        // fine_rollback is enabled on the boundary.
        bool fine_rollback = false;
        std::pmr::vector<aura::ast::SymId> sym_id_snapshot;
        aura::ast::FlatAST::ParamColumnsSnapshot param_snapshot;
        // Issue #1355: render hot-path lightweight checkpoint (no full
        // children snapshot; field mutations use FlatAST side log).
        bool lightweight = false;
    };
    // Issue #264: snapshot taken at fiber yield while a mutation
    // boundary may be active (per-fiber stack on Fiber).
    struct YieldBoundaryCheckpoint {
        std::uint64_t defuse_version = 0;
        std::size_t boundary_depth = 0;
        std::size_t mutation_stack_depth = 0;
        std::thread::id thread_id{};
        bool had_active_boundary = false;
    };
    // Per-fiber checkpoint stack. Each Fiber carries its own
    // `mutation_stack_` (added in Issue #213 Cycle 3), so a
    // fiber that migrates between threads brings its stack
    // with it. The static accessor `current_fiber()` returns
    // the fiber currently running on this thread (or nullptr
    // if no fiber is active, e.g. main-thread execution).
    //
    // The `static thread_local std::vector<...> g_mutation_stack`
    // pattern from #184 was a conservative choice that worked
    // when fibers were always pinned to their spawning thread.
    // With the GC safepoint + scheduler work in P2, fibers can
    // be stolen between workers; the per-fiber state avoids
    // the resulting "where did my stack go" bugs.
    //
    // When no fiber is active, we fall back to a thread-local
    // stack so the main-thread eval path still works.
    static thread_local std::vector<MutationCheckpoint> g_main_thread_stack;
    static thread_local std::vector<YieldBoundaryCheckpoint> g_main_thread_yield_checkpoints;

    // Current fiber on this thread. Set by the scheduler
    // before resume(); cleared after the fiber yields. nullptr
    // when the main thread is running (no fiber).
    // Stored as void* to avoid a circular include between
    // evaluator.ixx and fiber.h. The .cpp file casts to Fiber*.
    static thread_local void* g_current_fiber_void;
    static void* get_current_fiber() { return g_current_fiber_void; }
    static void set_current_fiber(void* f) { g_current_fiber_void = f; }

    // Internal: get the active stack (fiber's if a fiber is
    // running on this thread, else the main-thread fallback).
    // The fiber's stack is stored as an opaque void*; we
    // cast to the proper type here. The pointer is owned
    // by the fiber (allocated lazily on first enter, freed
    // on fiber destruction — see fiber.cpp's destructor).
    std::vector<MutationCheckpoint>& active_mutation_stack();
    // Enter a mutation boundary. Acquires the exclusive write
    // lock and captures a checkpoint with the current
    // defuse_version_. The version is bumped so any pending
    // readers (holding shared locks from before this call) see
    // a version mismatch and deopt.
    //
    // Issue #184: the version load is acquire (synchronizes with
    // prior release-stores from earlier mutations on other
    // threads); the version increment is release (publishes any
    // writes the caller will make under the boundary to acquirers
    // on other threads).
    // Issue #266: request fine-grained column snapshots for the
    // next boundary entry. Consumed by enter_mutation_boundary().
    void request_fine_rollback_for_next_boundary() noexcept {
        fine_rollback_for_next_boundary_ = true;
    }
    [[nodiscard]] BoundaryRollbackStats last_boundary_rollback_stats() const noexcept {
        return last_boundary_rollback_stats_;
    }

    void enter_mutation_boundary() {
        // Issue #233: the workspace_mtx_ lock was previously
        // acquired HERE as a local unique_lock that destructed
        // at function return, releasing the lock immediately.
        // That meant mutate:* primitives ran UNLOCKED — the
        // MutationBoundaryGuard's whole purpose was defeated.
        //
        // The lock is now held by MutationBoundaryGuard as a
        // member (so it survives across enter + body + exit).
        // enter_mutation_boundary() no longer acquires the
        // lock; it just does the version bump + log-size
        // capture. The guard's destructor releases the lock
        // after exit_mutation_boundary() runs.
        //
        // The bump performed by enter_mutation_boundary() is
        // a release-store (publishes any writes the caller
        // will make under the boundary to acquirers on other
        // threads); the version increment is release (publishes any
        // writes the caller will make under the boundary to acquirers
        // on other threads).
        std::size_t log_size = workspace_flat_ ? workspace_flat_->all_mutations().size() : 0;
        // Issue #1355: inside render hot path, use lightweight checkpoint —
        // no full children_ snapshot, field mutations go to side log.
        const bool lightweight =
            aura::core::arena_policy::in_render_hotpath() && workspace_flat_ != nullptr;
        // Issue #221: capture the per-node children_ vector. The
        // PCV's COW semantics make this a cheap copy (each PCV
        // is a shared_ptr to immutable storage; the snapshot
        // holds shared_ptrs that keep the pre-mutation PCs alive).
        std::vector<aura::ast::PersistentChildVector<aura::ast::NodeId>> children_snapshot;
        bool fine_rollback = fine_rollback_for_next_boundary_ && !lightweight;
        fine_rollback_for_next_boundary_ = false;
        std::pmr::vector<aura::ast::SymId> sym_id_snapshot;
        aura::ast::FlatAST::ParamColumnsSnapshot param_snapshot;
        bool bump_suppressed_at_entry = false;
        std::uint64_t macro_introduced_count_at_entry = 0;
        std::uint16_t flat_generation_at_entry = 0;
        if (workspace_flat_) {
            if (lightweight) {
                workspace_flat_->begin_render_lightweight_checkpoint();
                if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                    m->mutation_lightweight_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                children_snapshot = workspace_flat_->snapshot_children();
                if (fine_rollback) {
                    sym_id_snapshot = workspace_flat_->snapshot_sym_id();
                    param_snapshot = workspace_flat_->snapshot_param_columns();
                }
            }
            bump_suppressed_at_entry = workspace_flat_->atomic_batch_active();
            flat_generation_at_entry = workspace_flat_->generation();
        }
        MutationCheckpoint cp{defuse_version_.load(std::memory_order_acquire),
                              log_size,
                              bump_suppressed_at_entry,
                              macro_introduced_count_at_entry,
                              flat_generation_at_entry,
                              std::move(children_snapshot),
                              fine_rollback,
                              std::move(sym_id_snapshot),
                              std::move(param_snapshot),
                              lightweight};
        active_mutation_stack().push_back(std::move(cp));
        const std::size_t depth = active_mutation_stack().size();
        std::uint64_t prev_max = nested_guard_depth_max_.load(std::memory_order_relaxed);
        while (depth > prev_max &&
               !nested_guard_depth_max_.compare_exchange_weak(
                   prev_max, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
        if (depth == 1 && workspace_flat_ && !lightweight) {
            for (aura::ast::NodeId id = 0; id < workspace_flat_->size(); ++id) {
                if (workspace_flat_->is_macro_introduced(id))
                    ++macro_introduced_count_at_entry;
            }
            active_mutation_stack().back().macro_introduced_count_at_entry =
                macro_introduced_count_at_entry;
        }
        defuse_version_.fetch_add(1, std::memory_order_release);
        // Issue #189: bump the total-mutations counter for
        // observability. Relaxed because it's stats-only.
        total_mutations_.fetch_add(1, std::memory_order_relaxed);
    }
    // Exit a mutation boundary. Pops the checkpoint. If success
    // is true, the version advance is kept; if false, the
    // mutations recorded between enter and exit are rolled back
    // via the MutationRecord inverse (Issue #213 Cycle 1).
    // The lock is released by the unique_lock going out of scope.
    //
    // Issue #213 Cycle 2 — version-bump invariant:
    //   Both success and failure bump the version a second
    //   time (legacy behavior: enter + exit = 2 bumps per
    //   boundary). The bump is release-store so any pending
    //   readers holding a snapshot from before the boundary
    //   see a version mismatch and deopt. This invariant
    //   matters for primitives that hold a snapshot across
    //   the boundary (e.g. JIT-specialized L2 SHAPE_PAIR
    //   paths) — they expect 2 bumps per boundary to know
    //   the workspace was definitely mutated.
    //
    // Issue #213 Cycle 1 — rollback path:
    //   1. Call workspace_flat_->rollback_to_size(cp.mutation_log_size)
    //      to walk the log in reverse and apply the inverse
    //      mutation for each record beyond the checkpoint. The
    //      inverse is computed by FlatAST::rollback(mutation_id):
    //      - For field-level (int_val_/type_id_): restore the
    //        old_value at the field_offset.
    //      - For subtree-level: mark RolledBack and bump
    //        generation. (The actual re-parse + re-attach is
    //        done at a higher level by the rollback primitive
    //        in the Aura surface layer; see ast.ixx:1488.)
    //   2. Invalidate defuse_index_ so the next query rebuilds
    //      it from the rolled-back state.
    //   3. Bump defuse_version_ again (release-store) so any
    //      pending readers holding a snapshot from before the
    //      rollback see a version mismatch and deopt.
    //   4. Bump total_mutations_ for observability.
    //
    // Returns the popped checkpoint (or {0} if the stack is
    // empty — a defensive fallback for unbalanced calls).
    MutationCheckpoint exit_mutation_boundary(bool success) {
        auto& stack = active_mutation_stack();
        if (stack.empty())
            return {0, 0};
        const bool nested_boundary = stack.size() > 1;
        auto cp = stack.back();
        stack.pop_back();
        if (cp.lightweight && workspace_flat_) {
            // Issue #1355: lightweight path — commit or rollback side log.
            if (success) {
                workspace_flat_->commit_render_lightweight_checkpoint();
                if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                    m->mutation_lightweight_commit_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                const auto n = workspace_flat_->rollback_render_lightweight_checkpoint();
                // Also undo any durable log entries (structural ops fall through).
                BoundaryRollbackStats stats;
                stats.field_records_rolled =
                    n + workspace_flat_->rollback_to_size(cp.mutation_log_size);
                if (stats.field_records_rolled > 0)
                    bump_mutation_log_rollback_count();
                last_boundary_rollback_stats_ = stats;
                if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                    m->mutation_lightweight_rollback_total.fetch_add(1, std::memory_order_relaxed);
                defuse_index_ = nullptr;
            }
        } else if (!success && workspace_flat_) {
            // Roll back the mutations that were appended between
            // enter and exit. The log size captured at entry
            // tells us how far to undo.
            BoundaryRollbackStats stats;
            stats.field_records_rolled = workspace_flat_->rollback_to_size(cp.mutation_log_size);
            // Issue #549: bump mutation_log_rollback_count_ so
            // (query:self-evolution-stability-stats) can report
            // the lifetime # of times the log was actually
            // rolled back (a stricter subset of the lifetime #
            // of failed boundaries; bumps only when there were
            // mutations to undo).
            if (stats.field_records_rolled > 0) {
                bump_mutation_log_rollback_count();
                if (nested_boundary)
                    bump_edsl_nested_atomic_rollback();
            }
            // Issue #221: restore the per-node children_ from the
            // pre-mutation snapshot. The checkpoint's children_snapshot
            // holds shared_ptrs to the pre-mutation PCs (PCV COW),
            // so the restoration is O(1) per node.
            // Issue #1281: PCV topology fidelity is mandatory on
            // every failed boundary — restore_children always runs.
            // Issue #1502: restore_children also rebuilds parent_
            // from the restored child lists (full children_/parent_
            // topology), so partial MutationRecord inverse failures
            // cannot leave parent_of() inconsistent with children().
            workspace_flat_->restore_children(std::move(cp.children_snapshot));
            stats.children_column_restored = true;
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                m->children_topology_rollback_count.fetch_add(1, std::memory_order_relaxed);
                // Issue #1502: parent topology restored with children.
                m->parent_topology_rollback_count.fetch_add(1, std::memory_order_relaxed);
            }
            // Issue #266: restore sym_id_ / param columns for bulk
            // rename operations when fine rollback was requested.
            if (cp.fine_rollback) {
                workspace_flat_->restore_sym_id(std::move(cp.sym_id_snapshot));
                workspace_flat_->restore_param_columns(std::move(cp.param_snapshot));
                stats.sym_id_column_restored = true;
                stats.param_columns_restored = true;
            }
            // Issue #679: realign atomic-batch suppressed flag if a
            // nested path left it inconsistent with the snapshot.
            if (workspace_flat_->atomic_batch_active() != cp.bump_suppressed_at_entry) {
                if (cp.bump_suppressed_at_entry)
                    workspace_flat_->begin_atomic_batch();
                else
                    workspace_flat_->rollback_atomic_batch();
                suppressed_misalign_caught_.fetch_add(1, std::memory_order_relaxed);
            }
            if (stats.children_column_restored && cp.macro_introduced_count_at_entry > 0) {
                macro_rollback_hits_.fetch_add(1, std::memory_order_relaxed);
            }
            last_boundary_rollback_stats_ = stats;
            // Invalidate the def-use index — the workspace state
            // is now different from what the index reflects.
            defuse_index_ = nullptr;
        }
        // Issue #273: structural mutates bump generation_; refresh all
        // live node_gen_ entries so subsequent eval_flat paths see
        // valid NodeIds (including unrelated workspace defines).
        // Issue #1282: restamp also consumes auto_restamp_pending_
        // after a generation wrap so live node_gen_ recovers.
        if (workspace_flat_) {
            const bool wrap_pending = workspace_flat_->auto_restamp_pending();
            workspace_flat_->restamp_all_node_generations();
            if (wrap_pending) {
                if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                    m->generation_auto_restamp_on_wrap.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Issue #1283: unified provenance capture at Guard boundary exit.
        // Stamps defuse_version / mutation impact into Agent-visible metrics
        // so closed-loop self-evo can blame dirty nodes on this boundary.
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->provenance_boundary_capture_count.fetch_add(1, std::memory_order_relaxed);
        // Issue #1638: mutation_log compact at boundary exit (success
        // path only — failure path already rolls back via
        // rollback_to_size, so the log is already shrunk). Threshold
        // gate avoids the shrink_to_fit cost on small log states
        // (heavy-mutation safety net — 200MB+/day reclaim in long-
        // running Agent scenarios per the open mutation-log-growth
        // issue). Cheap when under threshold (single size() read).
        if (success && workspace_flat_) {
            static constexpr std::size_t kCompactThreshold = 64 * 1024; // 64KB
            if (workspace_flat_->mutation_log_size() > kCompactThreshold)
                compact_mutation_log();
        }
        // Bump version on both success and failure (legacy
        // invariant: 2 bumps per boundary). The lock is
        // released by the unique_lock going out of scope.
        defuse_version_.fetch_add(1, std::memory_order_release);
        // Issue #189: bump the total-mutations counter for
        // observability. Relaxed because it's stats-only.
        // We bump it even on rollback so dashboards can see
        // "the boundary attempted to mutate, then rolled back".
        total_mutations_.fetch_add(1, std::memory_order_relaxed);
        // Issue #550 / #518: narrowing_refresh_count_ is
        // bumped from TypeChecker::infer_flat_partial's
        // reanalyze_occurrence_contexts path (actual
        // OccurrenceInfoFlat refresh), not here.
        // Issue #551: bump impact_snapshot_count_ on every
        // successful Guard exit — mirrors the post-mutate
        // impact snapshot the AI loop reads for adaptive
        // strategy. Stats-only (relaxed-ordering); the
        // follow-up wires the actual snapshot collection
        // (dirty_nodes_in_snapshot_, marker delta, epoch
        // change, affected roots via StableNodeRef).
        bump_impact_snapshot_count();
        // Issue #555: bump guard_dirty_epoch_count_ on
        // every successful Guard exit — measures the
        // Guard + type cache integration. Pairs with
        // dirty_propagation_count_ (bumped in
        // mark_dirty_upward) so the AI Agent can compute
        // propagation_ratio = dirty_propagation / guard_dirty_epoch
        // (close to 1.0 = every Guard exit propagates).
        bump_guard_dirty_epoch_count();
        // Issue #672: every successful Guard exit is itself a
        // linear ownership enforcement event — bump the
        // post-mutate enforcement counter so the AI Agent can
        // gauge how often Guard exits propagate through
        // (query:linear-ownership-enforcement-stats). Pairs
        // with bump_guard_dirty_epoch_count() above so the
        // Agent can compute enforcement_ratio =
        // linear_post_mutate_enforcements / guard_dirty_epoch.
        bump_linear_post_mutate_enforcement();
        // Issue #555 / #518: selective_recheck_count_ is
        // bumped from infer_flat_partial's
        // reanalyze_occurrence_contexts path, not here.
        // Issue #456: record mutation-impact summary on
        // success only. Walk the workspace mutation log
        // from `mutation_log_size` (pre-mutation) to
        // current size (post-mutation) and count entries.
        // Skip on rollback (the rolled-back mutations
        // don't actually affect state).
        //
        // P0: the per-record DirtyReason bitmask is NOT
        // stored on MutationRecord (issue #188 stores it
        // on the AST node's dirty_ column, not on the
        // log entry). So we count log entries (= nodes
        // touched) and use the defuse_version_ delta as
        // the "reasons seen" surrogate: any delta >= 2
        // implies a structural change (kStructuralDirty
        // equivalent). Follow-up: extend MutationRecord
        // to carry a dirty_reasons byte so we can OR the
        // actual reasons in here.
        if (success && workspace_flat_) {
            const auto post_size = workspace_flat_->all_mutations().size();
            std::uint64_t nodes_changed = 0;
            if (post_size > cp.mutation_log_size) {
                nodes_changed = post_size - cp.mutation_log_size;
            }
            const std::uint64_t epoch_after = defuse_version_.load(std::memory_order_acquire);
            const std::uint64_t epoch_delta = epoch_after - cp.version;
            // Surrogate reasons mask: bit 0 = any node was
            // touched (kGeneralDirty equivalent).
            // Higher bits reserved for follow-up
            // MutationRecord reason bytes.
            const std::uint8_t reasons_mask = nodes_changed > 0 ? 0x01 : 0x00;
            mutation_impact_count_.fetch_add(1, std::memory_order_relaxed);
            if (nodes_changed > 0) {
                mutation_impact_nodes_changed_total_.fetch_add(nodes_changed,
                                                               std::memory_order_relaxed);
            }
            // OR the new reasons into the running mask
            // (relaxed atomic CAS loop; the mask is for
            // observability only).
            std::uint64_t cur = mutation_impact_reasons_seen_mask_.load(std::memory_order_relaxed);
            while (!mutation_impact_reasons_seen_mask_.compare_exchange_weak(
                cur, cur | reasons_mask, std::memory_order_relaxed)) {
            }
            // Append to the ring buffer (lockless; the
            // 8-slot ring tolerates torn writes from
            // concurrent boundaries — worst case is one
            // stale entry visible to (query:mutation-impact)
            // for one read, which is acceptable for
            // observability). We index by ring_seq_
            // modulo the ring size.
            const auto seq = mutation_impact_ring_seq_.fetch_add(1, std::memory_order_relaxed);
            auto& slot = mutation_impact_ring_[seq % kMutationImpactRingSize];
            slot.epoch_after = epoch_after;
            slot.epoch_delta = epoch_delta;
            slot.nodes_changed = nodes_changed;
            slot.reasons_mask = reasons_mask;
            // Issue #676: security audit event for successful mutations.
            std::string_view audit_op = "structural";
            ast::NodeId audit_target = ast::NULL_NODE;
            const auto& log = workspace_flat_->all_mutations();
            if (post_size > cp.mutation_log_size && post_size <= log.size()) {
                const auto& rec = log[post_size - 1];
                audit_op = rec.operator_name;
                audit_target = rec.target_node;
            }
            emit_mutation_audit(static_cast<std::uint32_t>(nodes_changed),
                                static_cast<std::uint32_t>(epoch_delta), audit_op, audit_target);
            // Issue #1589 / #1614: TypedMutationAudit trail + real invariant suite.
            {
                const std::uint64_t mid = total_mutations_.load(std::memory_order_relaxed);
                const auto fid = static_cast<std::int64_t>(aura_fiber_current_id());
                // #1614: when should_audit, run type + linear + provenance checks
                // and record a single trail event with pass/fail outcome.
                if (nodes_changed > 0 && typed_audit::should_audit(mid)) {
                    (void)run_typed_mutation_invariant_audit(
                        mid, audit_op, static_cast<std::uint32_t>(audit_target), cp.version,
                        epoch_after);
                } else {
                    typed_audit::record_boundary_outcome(
                        mid, audit_op, cp.version, epoch_after, /*success=*/true,
                        static_cast<std::uint32_t>(audit_target),
                        static_cast<std::uint32_t>(nodes_changed), fid);
                }
            }
            // Issue #488: post-mutate reflect validation + snapshot fields.
            // (Also covered as provenance leg of #1614 invariant audit when sampled.)
            (void)post_mutation_reflect_validate();
        } else if (!success) {
            // Issue #1589: TypedMutationAudit rollback trail.
            const std::uint64_t epoch_after = defuse_version_.load(std::memory_order_acquire);
            const std::uint64_t mid = total_mutations_.load(std::memory_order_relaxed);
            const auto fid = static_cast<std::int64_t>(aura_fiber_current_id());
            typed_audit::record_boundary_outcome(mid, "rollback", cp.version, epoch_after,
                                                 /*success=*/false, 0, 0, fid);
        }
        return cp;
    }
    // Get the current checkpoint stack depth (for testing /
    // observability). Returns 0 if the stack is empty.
    static std::size_t mutation_boundary_depth() { return active_mutation_stack_static().size(); }
    // Issue #354: per-Evaluator atomic flag set by
    // outermost MutationBoundaryGuard. The Fiber::yield
    // path reads this (cheap atomic load) to detect
    // "yield while holding a mutation boundary". Returns
    // true when an outermost guard is alive.
    [[nodiscard]] bool mutation_boundary_held() const noexcept {
        return mutation_boundary_held_.load(std::memory_order_acquire);
    }

    // Static version of active_mutation_stack() for observability
    // accessors that don't have an Evaluator instance handy.
    static std::vector<MutationCheckpoint>& active_mutation_stack_static();
    // Issue #300 follow-up #1: clear the main-thread fallback
    // mutation stack at CompilerService teardown so PCV snapshots
    // do not outlive workspace_flat_ destruction.
    static void clear_main_thread_mutation_stack() noexcept {
        g_main_thread_stack.clear();
        g_main_thread_yield_checkpoints.clear();
    }

    // Issue #588: on fiber resume, clear the worker thread's
    // main-thread fallback stacks so steal probes and guard
    // entry route exclusively through the per-fiber storage.
    static void sync_per_fiber_mutation_stack(void* per_fiber_stack) noexcept;

    // Issue #236 follow-up / #1746: per-fiber (thread_local) depth
    // counter for MutationBoundaryGuard nesting. Implementation in
    // evaluator_fiber_mutation.cpp uses
    // `thread_local std::unordered_map<std::uint64_t, int>` keyed by
    // Evaluator::instance_id_ (not raw address — free-list reuse must
    // not revive a dead Evaluator's depth). thread_local gives LIFO
    // ordering on each fiber's call stack; multiple fibers on the
    // same Evaluator are independent. Cross-fiber serialization
    // happens at the unique_lock layer.
    static int* mutation_boundary_depth_slot(Evaluator* ev);

    // Issue #264: yield-boundary checkpoint handshake (per-fiber
    // stack, stored on Fiber like mutation_stack_storage_).
    void checkpoint_yield_boundary(bool at_mutation_boundary_yield);
    bool restore_post_yield_or_rollback();
    // Issue #1446 AC1: re-pin all pinned StableNodeRef / COW children
    // in the current Guard stack. Called from restore_post_yield_or_rollback
    // and from the arena compact hook.
    bool re_pin_cow_children_from_snapshot();
    // Issue #1446 AC2: arena compact hook entry — registered during
    // Evaluator ctor via set_on_compact_hook().
    void on_arena_compact_hook();
    // Issue #1473: public test accessors for the 3 hook points wired by
    // #1473 (validate_or_refresh sweeps for pinned StableNodeRefs). The
    // production code paths (restore_post_yield_or_rollback,
    // on_arena_compact_hook, fiber-steal safepoint, GC safepoint) all
    // hit these functions through internal call sites; the test-only
    // accessors below let tests/test_issue_1473.cpp drive a 1000+ iter
    // stress loop without scheduling fibers.
    [[nodiscard]] bool test_re_pin_cow_children_from_snapshot() {
        return re_pin_cow_children_from_snapshot();
    }
    void test_probe_linear_at_gc_safepoint() noexcept { probe_linear_ownership_at_gc_safepoint(); }
    void test_probe_linear_on_fiber_steal() noexcept { probe_linear_ownership_on_fiber_steal(); }
    // Issue #1478: public test accessors for the 2 new linear
    // post-mutate enforcement counters. Tests drive the helper
    // indirectly through closure_needs_safe_fallback (which calls
    // linear_post_mutate_enforce from #1478 step 4) and read back
    // via these accessors to verify the counter increments per
    // closure invocation.
    [[nodiscard]] std::uint64_t test_linear_post_mutate_enforce_count() const noexcept {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        return m ? m->linear_post_mutate_enforcements.load(std::memory_order_relaxed) : 0;
    }
    [[nodiscard]] std::uint64_t test_linear_ownership_violation_prevented_count() const noexcept {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        return m ? m->linear_ownership_violation_prevented.load(std::memory_order_relaxed) : 0;
    }

    [[nodiscard]] bool any_active_mutation_boundary() const noexcept;
    [[nodiscard]] std::uint64_t mutation_yield_count() const noexcept {
        return mutation_yield_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t compaction_paused_by_boundary() const noexcept {
        return compaction_paused_by_boundary_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t cross_fiber_rollback_count() const noexcept {
        return cross_fiber_rollback_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_total_invariant_violations() const noexcept {
        return total_invariant_violations_.load(std::memory_order_relaxed);
    }
    // Issue #679: nested Guard / atomic-batch / macro rollback stats.
    [[nodiscard]] std::uint64_t nested_guard_depth_max() const noexcept {
        return nested_guard_depth_max_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t suppressed_misalign_caught() const noexcept {
        return suppressed_misalign_caught_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t macro_rollback_hits() const noexcept {
        return macro_rollback_hits_.load(std::memory_order_relaxed);
    }
    // Issue #417: lightweight cross-TU invariant check for
    // active_mutation_stack() vs mutation_boundary_depth_slot.
    void ensure_mutation_invariants() noexcept;
    // Issue #420: clone/expand → query → mutate → IR
    // MacroIntroduced hygiene contract probe. Verifies
    // SyntaxMarker::MacroIntroduced nodes carry the
    // kMacroExpansion macro_dirty_ bit set by
    // clone_macro_body / expand_inner_macros.
    void ensure_macro_hygiene_contract() const noexcept;
    [[nodiscard]] std::uint64_t get_macro_hygiene_contract_violations() const noexcept {
        return macro_hygiene_contract_violations_.load(std::memory_order_relaxed);
    }
    // Issue #421: recursive query:pattern MacroIntroduced filter.
    void bump_pattern_recursive_macro_skipped(std::uint64_t n = 1) noexcept {
        pattern_recursive_macro_skipped_.fetch_add(n, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_pattern_recursive_macro_skipped() const noexcept {
        return pattern_recursive_macro_skipped_.load(std::memory_order_relaxed);
    }
    void verify_pattern_result_hygiene(const aura::ast::FlatAST& flat, types::EvalValue result,
                                       bool with_markers) noexcept;
    void ensure_pattern_macro_filter_consistency(const aura::ast::FlatAST& flat) const noexcept;
    [[nodiscard]] std::uint64_t get_pattern_macro_filter_violations() const noexcept {
        return pattern_macro_filter_violations_.load(std::memory_order_relaxed);
    }
    // Issue #423: structural pre-index fast-path + consistency probe.
    void bump_pattern_structural_index_hit(std::uint64_t n = 1) noexcept {
        pattern_structural_index_hits_.fetch_add(n, std::memory_order_relaxed);
    }
    void bump_pattern_structural_index_miss(std::uint64_t n = 1) noexcept {
        pattern_structural_index_misses_.fetch_add(n, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_pattern_structural_index_hits() const noexcept {
        return pattern_structural_index_hits_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_pattern_structural_index_misses() const noexcept {
        return pattern_structural_index_misses_.load(std::memory_order_relaxed);
    }
    void ensure_pattern_index_consistency(const aura::ast::FlatAST& flat) const noexcept;
    [[nodiscard]] std::uint64_t get_pattern_index_consistency_violations() const noexcept {
        return pattern_index_consistency_violations_.load(std::memory_order_relaxed);
    }
    // Issue #424: WorkspaceTree / cross-layer StableRef probe.
    void bump_stable_ref_workspace_resolve(std::uint64_t n = 1) noexcept {
        stable_ref_workspace_resolves_.fetch_add(n, std::memory_order_relaxed);
    }
    void bump_stable_ref_workspace_resolve_miss(std::uint64_t n = 1) noexcept {
        stable_ref_workspace_resolve_misses_.fetch_add(n, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_stable_ref_workspace_resolves() const noexcept {
        return stable_ref_workspace_resolves_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_stable_ref_workspace_resolve_misses() const noexcept {
        return stable_ref_workspace_resolve_misses_.load(std::memory_order_relaxed);
    }
    void ensure_stable_ref_workspace_consistency() const noexcept;
    [[nodiscard]] std::uint64_t get_stable_ref_workspace_tree_violations() const noexcept {
        return stable_ref_workspace_tree_violations_.load(std::memory_order_relaxed);
    }
    std::vector<YieldBoundaryCheckpoint>& active_yield_checkpoint_stack();
    static std::vector<YieldBoundaryCheckpoint>& active_yield_checkpoint_stack_static();
    void bind_yield_hook_evaluator();
    void unbind_yield_hook_evaluator();

    // Issue #456: per-thread Evaluator pointer for
    // observability primitives. Set once at
    // CompilerService ctor; read by query:* primitives
    // that need the Evaluator outside any
    // MutationBoundaryGuard.
    static void set_query_evaluator(Evaluator* ev) noexcept;
    static Evaluator* get_query_evaluator() noexcept;

    // Issue #63723: clear per-thread/process-wide
    // Evaluator* slots that point at this dying
    // instance. Called by ~Evaluator to prevent
    // use-after-return on worker threads that read
    // g_query_evaluator / g_scheduler_stats_evaluator
    // (e.g. aura_evaluator_bump_mutation_steal_attempt
    // path). See evaluator_fiber_mutation.cpp for the
    // implementation + CAS rationale.
    void unbind_query_evaluator() noexcept;

    // Issue #285: getter for the thread-local yield-hook
    // evaluator pointer (set by bind_yield_hook_evaluator).
    // Used by Fiber::yield to find the active evaluator for
    // flush_mutation_boundary(). Returns nullptr when no
    // outermost guard is active.
    static Evaluator* yield_hook_evaluator() noexcept;

    // Issue #453: returns true if an outermost MutationBoundaryGuard
    // is currently active AND has captured a panic checkpoint
    // (i.e. `had_panic_checkpoint_ == true` on the guard). Used
    // by the bridge trampoline `g_pending_panic_checkpoint` to
    // decide whether a fiber migration should transfer or defer
    // a checkpoint. Returns false when no guard is active.
    [[nodiscard]] bool pending_panic_checkpoint() const noexcept;

    // Issue #189: reader-side version snapshot API. Fibers /
    // JIT-compiled code / closure dispatch that need to detect
    // concurrent mutations capture a snapshot via
    // `defuse_version_snapshot()` and later check
    // `is_version_current(snap)`. The snapshot is acquire-loaded
    // so it synchronizes with prior release-stores from mutation
    // boundaries.
    //
    // Usage:
    //   auto snap = ev.defuse_version_snapshot();
    //   // ... do work that may read AST/cells/pairs ...
    //   if (!ev.is_version_current(snap)) {
    //     // mutation happened during work; deopt / re-fetch /
    //     // restart as needed
    //   }
    //
    // This is the per-fiber equivalent of the issue's suggested
    // `Fiber::snapshot_version_` — implemented on the Evaluator
    // so the JIT/closure-dispatch paths can call it without
    // needing a Fiber handle.
    [[nodiscard]] std::uint64_t defuse_version_snapshot() const noexcept {
        return defuse_version_.load(std::memory_order_acquire);
    }
    // Issue #189: returns true if `snap` still represents the
    // current defuse version (no mutation happened between the
    // snapshot and now). Returns false if a mutation has
    // occurred (i.e., the workspace may have changed and cached
    // AST/IR/cell data is potentially stale).
    [[nodiscard]] bool is_version_current(std::uint64_t snap) const noexcept {
        auto now = defuse_version_.load(std::memory_order_acquire);
        return now == snap;
    }
    // Issue #189: observability accessor for the raw counter.
    // Equivalent to defuse_version_snapshot() but with relaxed
    // ordering (for stats / display only, not for control flow).
    [[nodiscard]] std::uint64_t defuse_version_relaxed() const noexcept {
        return defuse_version_.load(std::memory_order_relaxed);
    }
    // Issue #189: the total number of mutations ever applied to
    // this evaluator (sum of all fetch_add increments since the
    // evaluator was constructed). Tracked via a counter that's
    // bumped alongside defuse_version_ in the MutationBoundaryGuard
    // and the legacy fetch_add callsites.
    [[nodiscard]] std::uint64_t total_mutations() const noexcept {
        return total_mutations_.load(std::memory_order_relaxed);
    }

    // ── Issue #250: atomic-batch accessors ───────────
    [[nodiscard]] std::uint64_t atomic_batch_count() const noexcept {
        return atomic_batch_domain_.count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t atomic_batch_ops_total() const noexcept {
        return atomic_batch_domain_.ops_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t atomic_batch_rollbacks() const noexcept {
        return atomic_batch_domain_.rollbacks.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t atomic_batch_bumps_saved_total() const noexcept {
        return atomic_batch_domain_.bumps_saved_total.load(std::memory_order_relaxed);
    }
    // Issue #396 Phase 3: lifetime total of atomic-batch
    // commits that ran while the bridge fiber setter was
    // active (i.e. in serve mode with a fiber context).
    // Heuristic for "ran under concurrent fiber pressure".
    [[nodiscard]] std::uint64_t atomic_batch_in_fiber_total() const noexcept {
        return atomic_batch_domain_.in_fiber_total.load(std::memory_order_relaxed);
    }
    // Issue #737: atomic-batch snapshot + pinning accessors.
    void begin_atomic_batch_pinning() noexcept {
        atomic_batch_pinned_refs_.clear();
        last_atomic_batch_snapshot_id_ = -1;
    }
    void pin_node_for_atomic_batch(aura::ast::NodeId id) noexcept {
        if (!workspace_flat_ || id >= workspace_flat_->size())
            return;
        const auto ref = workspace_flat_->make_safe_ref(id);
        for (const auto& existing : atomic_batch_pinned_refs_) {
            if (existing.id == ref.id)
                return;
        }
        atomic_batch_pinned_refs_.push_back(ref);
    }
    void pin_dirty_nodes_for_atomic_batch() noexcept {
        if (!workspace_flat_)
            return;
        for (aura::ast::NodeId id = 0; id < workspace_flat_->size(); ++id) {
            if (workspace_flat_->is_dirty(id))
                pin_node_for_atomic_batch(id);
        }
    }
    void commit_atomic_batch_pinning() noexcept {
        if (!workspace_flat_)
            return;
        atomic_batch_domain_.pinned_refs_total.fetch_add(atomic_batch_pinned_refs_.size(),
                                                         std::memory_order_relaxed);
        // Issue #1500: full-provenance restamp (gen/wrap/cow/mutation_id),
        // not gen-only — matches refresh_if_stale semantics.
        for (auto& ref : atomic_batch_pinned_refs_) {
            if (ref.id < workspace_flat_->size())
                (void)ref.refresh_if_stale(*workspace_flat_);
        }
    }
    void rollback_atomic_batch_pinning() noexcept {
        atomic_batch_pinned_refs_.clear();
        last_atomic_batch_snapshot_id_ = -1;
    }
    void record_atomic_batch_snapshot_capture(std::int64_t snap_id) noexcept {
        last_atomic_batch_snapshot_id_ = snap_id;
        if (snap_id >= 0)
            atomic_batch_domain_.snapshot_captures.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_atomic_batch_snapshot_rollback() noexcept {
        atomic_batch_domain_.snapshot_rollbacks.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t atomic_batch_pinned_ref_count() const noexcept {
        return atomic_batch_pinned_refs_.size();
    }
    [[nodiscard]] std::uint64_t atomic_batch_pinned_refs_total() const noexcept {
        return atomic_batch_domain_.pinned_refs_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t atomic_batch_snapshot_rollbacks() const noexcept {
        return atomic_batch_domain_.snapshot_rollbacks.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t atomic_batch_snapshot_captures() const noexcept {
        return atomic_batch_domain_.snapshot_captures.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::int64_t last_atomic_batch_snapshot_id() const noexcept {
        return last_atomic_batch_snapshot_id_;
    }
    // Issue #738: COW/sub-workspace StableNodeRef boundary pinning.
    // Issue #1311 (P0): mutex + snapshot iteration — concurrent pin +
    // propagate was a data race on this vector (and re-entrant push
    // during outer iteration).
    //
    // Issue #1406 Option 2 (#3, bounded retention): cap the pin
    // vector to kMaxPinnedBoundaryRefs and drop the oldest pin on
    // overflow. Unbounded growth was the RSS leak vector that the
    // issue body flagged as the primary symptom (same family as
    // #1398 cells_/pairs_/string_heap_ race + #1384 env_frames_
    // version init). Drops are best-effort — the dropped ref may
    // still be live but loses its cross-boundary pin guarantee;
    // validate_stable_ref_cross_cow_boundary will fall back to
    // is_valid() for the next check on that ref.
    std::vector<aura::ast::FlatAST::StableNodeRef> cow_boundary_pinned_refs_{};
    mutable std::mutex cow_boundary_pins_mtx_{};
    std::atomic<std::uint64_t> cow_boundary_pins_total_{0};
    static constexpr std::size_t kMaxPinnedBoundaryRefs = 4096;
    void pin_stable_ref_for_cow_boundary(aura::ast::FlatAST::StableNodeRef ref) noexcept {
        ref.pin_for_cow();
        {
            std::lock_guard<std::mutex> lock(cow_boundary_pins_mtx_);
            for (const auto& existing : cow_boundary_pinned_refs_) {
                if (existing.id == ref.id && existing.workspace_id == ref.workspace_id)
                    return;
            }
            // Issue #1406 Option 2: bounded retention. If we are at
            // the cap, drop the OLDEST pin (front of the vector) and
            // log via the metric. Newest pins are the most likely to
            // be in active use, so drop-oldest is the right policy.
            if (cow_boundary_pinned_refs_.size() >= kMaxPinnedBoundaryRefs) {
                cow_boundary_pinned_refs_.erase(cow_boundary_pinned_refs_.begin());
                if (workspace_flat_)
                    workspace_flat_->bump_pinned_across_boundaries_dropped();
            }
            cow_boundary_pinned_refs_.push_back(ref);
        }
        cow_boundary_pins_total_.fetch_add(1, std::memory_order_relaxed);
        if (workspace_flat_)
            workspace_flat_->bump_pinned_across_boundaries();
    }
    void propagate_cow_pins_after_clone(std::uint32_t child_layer,
                                        std::uint64_t child_cow_epoch) noexcept {
        if (!workspace_tree_ || !workspace_flat_)
            return;
        auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
        if (child_layer >= wt->size())
            return;
        workspace_flat_->set_workspace_cow_epoch(child_cow_epoch);
        const auto parent_layer = wt->nodes_[child_layer].parent_layer_idx;
        // Issue #1406 Option 2 (#1): concurrent snapshot+iter with
        // version check retry. Snapshot a version counter, iterate,
        // then re-check — if a concurrent pin happened during
        // iteration, re-snapshot and retry. Bounded to kMaxRetries
        // to avoid livelock on a hot pinning source. The counter is
        // cow_boundary_pins_total_ (atomic, monotonic) — any
        // difference between version_before and version_after
        // indicates new pins added during the snapshot window.
        constexpr int kMaxRetries = 3;
        for (int retry = 0; retry < kMaxRetries; ++retry) {
            std::vector<aura::ast::FlatAST::StableNodeRef> snapshot;
            const auto version_before = cow_boundary_pins_total_.load(std::memory_order_acquire);
            {
                std::lock_guard<std::mutex> lock(cow_boundary_pins_mtx_);
                snapshot = cow_boundary_pinned_refs_;
            }
            for (const auto& pinned : snapshot) {
                if (pinned.workspace_id != parent_layer)
                    continue;
                auto resolved = wt->resolve_stable_ref(parent_layer, pinned, child_layer);
                if (!resolved)
                    continue;
                auto child_ref = *resolved;
                child_ref.cow_epoch_at_capture = child_cow_epoch;
                child_ref.pin_for_cow();
                pin_stable_ref_for_cow_boundary(child_ref);
            }
            const auto version_after = cow_boundary_pins_total_.load(std::memory_order_acquire);
            if (version_after == version_before)
                break; // success — no concurrent pin during this snapshot
        }
    }
    [[nodiscard]] bool
    validate_stable_ref_cross_cow_boundary(const aura::ast::FlatAST::StableNodeRef& ref,
                                           std::uint64_t current_cow_epoch) const noexcept {
        if (!workspace_flat_)
            return false;
        workspace_flat_->bump_cross_boundary_validations();
        if (ref.cow_epoch_at_capture == 0 || ref.cow_epoch_at_capture == current_cow_epoch)
            return workspace_flat_->is_valid(ref);
        if (ref.boundary_pinned && workspace_flat_->is_live_node(ref.id))
            return true;
        return workspace_flat_->is_valid(ref);
    }
    [[nodiscard]] std::uint64_t cow_boundary_pins_total() const noexcept {
        return cow_boundary_pins_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t cow_boundary_pinned_ref_count() const noexcept {
        std::lock_guard<std::mutex> lock(cow_boundary_pins_mtx_);
        return cow_boundary_pinned_refs_.size();
    }
    // Issue #737: snapshot capture/restore without re-acquiring
    // workspace_mtx_ (caller must hold the exclusive lock).
    [[nodiscard]] std::int64_t
    capture_workspace_snapshot_under_lock(std::string_view name) noexcept;
    [[nodiscard]] bool restore_workspace_snapshot_under_lock(std::size_t id) noexcept;
    // Issue #459: nested atomic-batch + suppressed-bump
    // observability. Bumped when:
    //   - atomic_batch_steal_violation_ — a fiber steal
    //     happens while a MutationBoundaryGuard with
    //     atomic_batch_active=true is held (defensive)
    //   - suppressed_bump_lost_on_gc_ — a GC safepoint
    //     wait while a suppressed bump is pending (the
    //     bump should be flushed before the safepoint;
    //     this counter catches the violation case)
    // Stats-only (relaxed-ordering). The P0 ship exposes
    // them via the new query:atomic-batch-stats primitive.
    std::atomic<std::uint64_t> atomic_batch_steal_violation_{0};
    std::atomic<std::uint64_t> suppressed_bump_lost_on_gc_{0};
    [[nodiscard]] std::uint64_t get_atomic_batch_steal_violation() const noexcept {
        return atomic_batch_steal_violation_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_suppressed_bump_lost_on_gc() const noexcept {
        return suppressed_bump_lost_on_gc_.load(std::memory_order_relaxed);
    }
    void bump_atomic_batch_steal_violation() noexcept {
        atomic_batch_steal_violation_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #790: atomic batch cross-fiber steal +
    // hygiene violation observability bump helpers
    // (Refine/Consolidate #737/#761 non-duplicative).
    // Called from the planned Phase 2+ wire-up sites:
    // - bump_atomic_batch_cross_fiber_steal() in
    //   restore_post_yield_or_rollback + Mutation
    //   BoundaryGuard when inside suppressed batch
    //   (counts steals that fired while batch was
    //   active, NOT violations — separate from the
    //   existing atomic_batch_steal_violation_)
    // - bump_atomic_batch_hygiene_violation() in
    //   hygiene_protected_error path inside batch
    //   body (counts hygiene violations caught
    //   during the batch, separate from the
    //   #757 macro-hygiene-provenance-stats
    //   violations counter which is general)
    void bump_atomic_batch_cross_fiber_steal() noexcept {
        atomic_batch_domain_.cross_fiber_steals_total.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_atomic_batch_hygiene_violation() noexcept {
        atomic_batch_domain_.hygiene_violations_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1900: AC3 — dispatch-coverage + interleaving-prevention telemetry.
    // `unsupported_op_total` only ever increments when a future-version primitive
    // name lands before its lockless helper ships (i.e. the 14-op dispatch is now
    // complete and any new sub-op name must trigger this counter).
    // `interleaved_mutation_prevented` increments whenever the batch's outer
    // MutationBoundaryGuard successfully serialized a concurrent mutate attempt
    // (observed via the workspace_mtx_ unique_lock acquisition in eval_flat_apply_mutate_*).
    void bump_atomic_batch_unsupported_op() noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->atomic_batch_unsupported_op_total.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_atomic_batch_interleaved_prevented() noexcept {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->atomic_batch_interleaved_mutation_prevented.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t atomic_batch_unsupported_op_total() const noexcept {
        if (auto* m = static_cast<const CompilerMetrics*>(compiler_metrics_))
            return m->atomic_batch_unsupported_op_total.load(std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::uint64_t atomic_batch_interleaved_prevented_total() const noexcept {
        if (auto* m = static_cast<const CompilerMetrics*>(compiler_metrics_))
            return m->atomic_batch_interleaved_mutation_prevented.load(std::memory_order_relaxed);
        return 0;
    }
    // Issue #790: public accessors for the 2 NEW
    // atomic_batch_* fields (mirror the existing
    // atomic_batch_count() / atomic_batch_domain_.rollbacks
    // / atomic_batch_bumps_saved_total() pattern).
    [[nodiscard]] std::uint64_t atomic_batch_cross_fiber_steals_total() const noexcept {
        return atomic_batch_domain_.cross_fiber_steals_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t atomic_batch_hygiene_violations_total() const noexcept {
        return atomic_batch_domain_.hygiene_violations_total.load(std::memory_order_relaxed);
    }
    void bump_suppressed_bump_lost_on_gc() noexcept {
        suppressed_bump_lost_on_gc_.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #456: mutation-impact observability accessors.
    [[nodiscard]] std::uint64_t get_mutation_impact_ring_seq() const noexcept {
        return mutation_impact_ring_seq_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t get_mutation_impact_count() const noexcept {
        return mutation_impact_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_mutation_impact_nodes_changed_total() const noexcept {
        return mutation_impact_nodes_changed_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_mutation_impact_reasons_seen_mask() const noexcept {
        return mutation_impact_reasons_seen_mask_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_defuse_version() const noexcept {
        return defuse_version_.load(std::memory_order_acquire);
    }
    // Issue #419: modular public alias for AOT bridge, runtime
    // closure dispatch, and scheduler version reads. Equivalent
    // to get_defuse_version() / defuse_version_snapshot().
    [[nodiscard]] std::uint64_t current_defuse_version() const noexcept {
        return get_defuse_version();
    }
    [[nodiscard]] std::uint64_t get_last_queried_epoch() const noexcept {
        return last_queried_epoch_.load(std::memory_order_acquire);
    }
    void record_epoch_query() noexcept {
        last_queried_epoch_.store(defuse_version_.load(std::memory_order_acquire),
                                  std::memory_order_release);
    }
    // Issue #448: mutation-coordination observability
    // accessors + bump helpers. Public so the
    // (query:mutation-coordination-stats) primitive can
    // read them, and the scheduler / fiber hooks (the
    // follow-up) can bump them.
    [[nodiscard]] std::uint64_t get_mutation_steal_violation_count() const noexcept {
        return mutation_steal_violation_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_gc_blocked_by_mutation_boundary() const noexcept {
        return gc_blocked_by_mutation_boundary_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_safepoint_mutation_wait_total_ns() const noexcept {
        return safepoint_mutation_wait_total_ns_.load(std::memory_order_relaxed);
    }
    void bump_mutation_steal_violation_count() noexcept {
        mutation_steal_violation_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_gc_blocked_by_mutation_boundary() noexcept {
        gc_blocked_by_mutation_boundary_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_safepoint_mutation_wait_ns(std::uint64_t delta_ns) noexcept {
        safepoint_mutation_wait_total_ns_.fetch_add(delta_ns, std::memory_order_relaxed);
    }
    // Issue #543: SoA EnvFrame/EnvId dual-path + version
    // stamping + stale detection observability. Bumped by
    //   - materialize_call_env (stale_refresh_count_)
    //   - lookup_by_symid_chain / walk_env_frames
    //     (version_mismatch_in_walk_)
    //   - walk_env_frame_roots (gc_walk_safe_skips_)
    //   - dual-path length/order desync detection
    //     (desync_detected_)
    // All stats-only (relaxed-ordering). Exposed via the
    // (query:envframe-dualpath-stats) primitive.
    [[nodiscard]] std::uint64_t get_envframe_desync_detected() const noexcept {
        return envframe_desync_detected_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_bindings_dual_sync_count() const noexcept {
        return bindings_dual_sync_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_envframe_stale_refresh_count() const noexcept {
        return envframe_stale_refresh_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_envframe_version_mismatch_in_walk() const noexcept {
        return envframe_version_mismatch_in_walk_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_envframe_gc_walk_safe_skips() const noexcept {
        return envframe_gc_walk_safe_skips_.load(std::memory_order_relaxed);
    }
    void bump_envframe_desync_detected() const noexcept {
        envframe_desync_detected_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_bindings_dual_sync_count() const noexcept {
        bindings_dual_sync_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_envframe_stale_refresh_count() const noexcept {
        envframe_stale_refresh_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_envframe_version_mismatch_in_walk() const noexcept {
        envframe_version_mismatch_in_walk_.fetch_add(1, std::memory_order_relaxed);
        bump_compiler_root_env_version_mismatch();
    }
    void bump_envframe_gc_walk_safe_skips() const noexcept {
        envframe_gc_walk_safe_skips_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1903: dual-path consistency enforcement bump/getter.
    [[nodiscard]] std::uint64_t get_envframe_dual_consistency_asserted() const noexcept {
        return envframe_dual_consistency_asserted_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_envframe_post_steal_dual_synced() const noexcept {
        return envframe_post_steal_dual_synced_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_envframe_materialize_consistency_checks() const noexcept {
        return envframe_materialize_consistency_checks_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_envframe_gc_walk_legacy_fallback_uses() const noexcept {
        return envframe_gc_walk_legacy_fallback_uses_.load(std::memory_order_relaxed);
    }
    void bump_envframe_dual_consistency_asserted() const noexcept {
        envframe_dual_consistency_asserted_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_envframe_post_steal_dual_synced() const noexcept {
        envframe_post_steal_dual_synced_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_envframe_materialize_consistency_checks() const noexcept {
        envframe_materialize_consistency_checks_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_envframe_gc_walk_legacy_fallback_uses() const noexcept {
        envframe_gc_walk_legacy_fallback_uses_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_envframe_post_rollback_invalidations() const noexcept {
        return envframe_post_rollback_invalidations_.load(std::memory_order_relaxed);
    }
    void bump_envframe_post_rollback_invalidations(std::uint64_t n = 1) const noexcept {
        envframe_post_rollback_invalidations_.fetch_add(n, std::memory_order_relaxed);
    }
    void bump_envframe_truncate(std::uint64_t frames_dropped) const noexcept {
        envframe_truncate_count_.fetch_add(1, std::memory_order_relaxed);
        envframe_truncated_frames_.fetch_add(frames_dropped, std::memory_order_relaxed);
    }
    // Issue #1386: bump compact_env_frames counters. Only bumps
    // the per-call counter when at least 1 frame was reclaimed
    // (so operators can distinguish "compact ran" from "compact
    // reclaimed"). The closure-rewrite counter always bumps
    // (it's a per-closure touch count, not a per-call flag).
    void bump_envframe_compact(std::uint64_t frames_reclaimed,
                               std::uint64_t closures_rewritten) const noexcept {
        if (frames_reclaimed > 0) {
            envframe_compact_count_.fetch_add(1, std::memory_order_relaxed);
            envframe_reclaimed_frames_.fetch_add(frames_reclaimed, std::memory_order_relaxed);
        }
        envframe_compact_closures_rewritten_.fetch_add(closures_rewritten,
                                                       std::memory_order_relaxed);
    }
    // Issue #418: bindings_ vs bindings_symid_ length consistency
    // probe for EnvFrame SoA dual-path + stale policy paths.
    [[nodiscard]] bool ensure_envframe_dual_path_consistency(const EnvFrame& fr) const noexcept;


// ── Issue #184: MutationBoundaryGuard (RAII) ─────────────
// Acquires the exclusive workspace write lock + bumps
// defuse_version_ on construction, pops the checkpoint on
// destruction. Replaces manual enter/exit pairs so callers
// can't forget to pair them on the panic / early-return path.
//
// Usage:
//   bool ok = true;
//   {
//       MutationBoundaryGuard guard(evaluator, &ok);
//       // ... mutation work ...
//       // Set ok = false before destruction to request rollback.
//   }  // guard exits here; lock released; checkpoint popped.
//
// The success_flag is a pointer to a bool that the caller
// controls. On construction it's set to true (default);
// caller can set it to false to signal rollback intent.
// The exit path reads the flag (today both branches commit;
// the rollback path is a follow-up that requires hooking
// into MutationRecord (#142) and the defuse_index_
// restoration).
//
// Move-only (the lock + state can't be safely copied); do
// not copy. Construction blocks until the exclusive lock
// is acquired.
//
// Issue #241 (panic-checkpoint integration): the Guard now
// owns the panic-checkpoint lifecycle for mutate:* primitives.
// Previously each primitive had to manually call
//   save_panic_checkpoint()  // at entry
//   ... body ...
//   commit_panic_checkpoint()   // on success
//   restore_panic_checkpoint()  // on failure (with auto_rollback)
// The Guard now does all three automatically:
//   - ctor: save_panic_checkpoint() — captures the current
//     `current-source` so a panic / typecheck-failure can be
//     recovered via restore.
//   - dtor (outermost only): based on `flag_`:
//       ok == true                       → commit (clear checkpoint)
//       ok == false + panic_auto_rollback_ → restore (source reverted)
//       ok == false + !panic_auto_rollback_ → leave alive (caller
//                                                may retry)
//   - nested guards (depth > 1) skip the panic-checkpoint
//     step — only the outermost owns the checkpoint, matching
//     the lock-ownership rule.
// Issue #1444: convenience macro for primitive authors to wrap
// their body in a MutationBoundaryGuard without typing the
// bool ok + ctor boilerplate. Usage:
//   AURA_MUTATION_BOUNDARY_PROTECT(ev, [&]{
//       // body — `ev` already Guard-wrapped, `ok` is local
//   });
// If the body (or any nested Guard) flips `ok` to false, the
// Guard dtor rolls back (linear ownership + panic-checkpoint
// restore + defuse_version_ bump suppression).
// Issue #1547: try_acquire-based protect macro. On quota reject the
// body is skipped (ok stays true for no-op; callers that need the
// error use try_acquire directly).
// Issue #1745: no bare `break` — use if/else so expansion inside a
// switch case cannot be misread / mis-audited as escaping the switch
// (classic macro footgun). do/while(0) remains for statement-like use.
#define AURA_MUTATION_BOUNDARY_PROTECT(EV, BODY)                                                   \
    do {                                                                                           \
        bool _aura_mbp_ok = true;                                                                  \
        auto _aura_mbp_gr = ::aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(       \
            (EV), /*pending_count=*/1, &_aura_mbp_ok);                                             \
        if (_aura_mbp_gr) {                                                                        \
            auto& _aura_mbp_guard = **_aura_mbp_gr;                                                \
            (void)_aura_mbp_guard;                                                                 \
            BODY;                                                                                  \
        } else {                                                                                   \
            _aura_mbp_ok = false;                                                                  \
        }                                                                                          \
        (void)_aura_mbp_ok;                                                                        \
    } while (0)

    class MutationBoundaryGuard {
        // Issue #241: did we capture a panic checkpoint at ctor?
        // (save_panic_checkpoint returns false if no source is loaded
        //  or if (current-source) isn't registered.)
        bool had_panic_checkpoint_ = false;
        bool fine_rollback_ = false;
        // Issue #459: was this guard entered under a
        // (mutate:atomic-batch) — i.e. should the guard
        // suppress its own generation bump? The flag is
        // set by `suppress_generation_bump(true)` before
        // construction. Nested atomic batches stack
        // (any non-zero nesting → atomic_batch_active).
        bool atomic_batch_active_ = false;
        bool suppress_bump_ = false;
        // Issue #1253: outermost mutation hold-time tracking.
        bool is_outermost_ = false;
        // Issue #1590: inert guard after legacy-ctor mutation-quota reject
        // (no lock, no depth, dtor is a no-op). try_acquire returns AuraError
        // instead of constructing inert.
        bool inert_ = false;
        std::chrono::steady_clock::time_point enter_ts_{};

    public:
        // Issue #1254: true only for the lock-owning outermost guard.
        [[nodiscard]] bool is_outermost() const noexcept { return is_outermost_; }
        // Issue #1590: true when legacy ctor soft-failed on mutation quota.
        [[nodiscard]] bool is_inert() const noexcept { return inert_; }
        // Issue #1684: mark success_flag false so dtor rolls back (if
        // panic_auto_rollback_) instead of committing a partial mutate.
        void mark_failed() noexcept {
            if (flag_)
                *flag_ = false;
        }
        // Issue #1684: run callable under Guard; on any throw, mark_failed
        // so ~MutationBoundaryGuard does not commit. Prefer this for
        // validate / typecheck / ownership and other external call sites
        // inside add_mutate bodies (throws previously left ok=true).
        // Returns true if fn completed without throwing.
        template <typename F>
        [[nodiscard]] bool run_or_rollback(F&& fn, std::string* err_out = nullptr) {
            if (inert_) {
                if (err_out)
                    *err_out = "inert guard";
                return false;
            }
            try {
                std::forward<F>(fn)();
                return true;
            } catch (const std::exception& e) {
                mark_failed();
                if (err_out)
                    *err_out = e.what();
                if (ev_) {
                    if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                        m->mutation_boundary_exception_rollback_total.fetch_add(
                            1, std::memory_order_relaxed);
                }
                return false;
            } catch (...) {
                // [SILENCE-PRIM-#1684] intentional: convert unknown throw into
                // Guard mark_failed + false so mutate does not commit.
                mark_failed();
                if (err_out)
                    *err_out = "unknown exception";
                if (ev_) {
                    if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                        m->mutation_boundary_exception_rollback_total.fetch_add(
                            1, std::memory_order_relaxed);
                }
                return false;
            }
        }
        // Issue #266: enable fine-grained column snapshots for the
        // next guard on this evaluator (call before construction).
        static void enable_fine_rollback(Evaluator& ev) noexcept {
            ev.request_fine_rollback_for_next_boundary();
        }

        // Issue #1547 / #1556 / #1590: typed-error factory — check_mutation_quota
        // then construct. Replaces panic/throw paths with AuraResult. On pass,
        // bumps mutation_quota_used_ by pending_count. Prefer this over legacy ctor.
        [[nodiscard]] static aura::core::AuraResult<std::unique_ptr<MutationBoundaryGuard>>
        try_acquire(Evaluator& ev, std::uint64_t pending_count = 1, bool* success_flag = nullptr,
                    bool fine_rollback = false) noexcept {
            // Issue #1547 / #1618 / #1628: typed ResourceQuotaExceeded —
            // never PanicCheckpoint / runtime_error on quota reject.
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_))
                m->mutation_guard_try_acquire_total.fetch_add(1, std::memory_order_relaxed);
            if (auto err = ev.check_mutation_quota(pending_count)) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_)) {
                    m->manager_enforce_total.fetch_add(1, std::memory_order_relaxed);
                    m->mutation_guard_try_acquire_reject_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                }
                return std::unexpected(std::move(*err));
            }
            ev.mutation_quota_used_.fetch_add(pending_count, std::memory_order_relaxed);
            // Mirror consume into process Mutations dim for manager dashboards.
            if (ev.resource_quota_mutations_ != 0) {
                (void)aura::core::resource_quota::process_resource_quota().check_and_consume(
                    aura::core::resource_quota::Dimension::Mutations, pending_count);
            }
            // Construct via private AcquireTag path (quota already checked).
            return std::unique_ptr<MutationBoundaryGuard>(
                new MutationBoundaryGuard(ev, success_flag, fine_rollback, AcquireTag{},
                                          /*quota_prechecked=*/true));
        }

        // Issue #1547 / #1556 / #1590: legacy RAII ctor — now enforces mutation
        // quota via soft-fail (success_flag=false, inert guard) because ctors
        // cannot return AuraResult. Prefer try_acquire() for typed errors.
        // Marked [[deprecated]] per #1556 AC2; project uses
        // -Wno-deprecated-declarations so remaining call-sites still compile.
        [[deprecated("use MutationBoundaryGuard::try_acquire for typed ResourceQuotaExceeded "
                     "(#1547/#1556/#1590)")]]
        MutationBoundaryGuard(Evaluator& ev, bool* success_flag,
                              bool fine_rollback = false) noexcept
            : MutationBoundaryGuard(ev, success_flag, fine_rollback, AcquireTag{},
                                    /*quota_prechecked=*/false) {}

    private:
        struct AcquireTag {};
        // Shared implementation for try_acquire + legacy ctor.
        // quota_prechecked=true: caller already ran check_mutation_quota + bump.
        // quota_prechecked=false (#1590 legacy): check here; on reject → inert_.
        MutationBoundaryGuard(Evaluator& ev, bool* success_flag, bool fine_rollback, AcquireTag,
                              bool quota_prechecked = true) noexcept
            : fine_rollback_(fine_rollback)
            , ev_(&ev)
            , flag_(success_flag)
            ,
            // Issue #233 + #236 follow-up: the unique_lock is
            // now a MEMBER of the guard (was previously a local
            // in enter_mutation_boundary() that destructed at
            // function return, releasing the lock immediately).
            //
            // enter_mutation_boundary() now does only the
            // version bump + log-size capture (no lock
            // acquire); this constructor acquires the
            // exclusive write lock and holds it for the
            // entire guard lifetime.
            //
            // NESTED GUARD HANDLING (test_issue_184 Test 5):
            // shared_mutex is NOT recursive, so a nested guard
            // would deadlock on the inner acquire. The fix:
            // only the OUTERMOST guard acquires the lock.
            // Track nesting depth via a member counter; only
            // acquire when depth 0→1, release when 1→0.
            // The depth is shared (static thread_local) so
            // nested guards in the same thread cooperate.
            lock_(ev.workspace_mtx_, std::defer_lock) {
            if (!quota_prechecked) {
                // Issue #1590: soft-fail mutation quota on legacy ctor path.
                if (auto err = ev.check_mutation_quota(1)) {
                    (void)err;
                    inert_ = true;
                    if (flag_)
                        *flag_ = false;
                    return;
                }
                ev.mutation_quota_used_.fetch_add(1, std::memory_order_relaxed);
            }
            if (flag_)
                *flag_ = true; // optimistic default
            // Issue #236 / #1746: thread_local depth counter keyed by
            // Evaluator::instance_id_ (not address). Each fiber has its
            // own LIFO call stack, so nested guards on a single fiber
            // are always outermost-then-inner (destructed innermost-
            // first). Cross-fiber synchronization happens at unique_lock.
            int* slot = Evaluator::mutation_boundary_depth_slot(ev_);
            int prev = ++(*slot);
            bool outermost = (prev == 1);
            is_outermost_ = outermost;
            if (outermost) {
                // Issue #1253: start hold-time clock for long-mutation policy.
                enter_ts_ = std::chrono::steady_clock::now();
                // Issue #1523: Workspace level in #1388 order (after Mutate).
                aura::compiler::lock_order::on_acquire(
                    aura::compiler::lock_order::Level::Workspace);
                lock_.lock();
                ev_->outermost_mutation_success_flag_ = flag_;
                ev_->bind_yield_hook_evaluator();
                // Issue #354: set the atomic flag so
                // Fiber::yield can detect "yield while
                // holding a mutation boundary". The
                // check is O(1) (atomic load) and the
                // flag is cleared by the Guard dtor
                // (the outermost one only).
                ev_->mutation_boundary_held_.store(true, std::memory_order_release);
                // Issue #1252: coverage counter — every outermost Guard wrap.
                // Issue #1364: mutation × safepoint telemetry (benign race).
                if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
                    m->mutation_boundary_primitives_wrapped.fetch_add(1, std::memory_order_relaxed);
                    if (aura::gc_hooks::in_gc_safepoint()) {
                        m->mutation_in_safepoint_total.fetch_add(1, std::memory_order_relaxed);
                        // Collision = mutation entry observed during active STW flag
                        m->safepoint_collision_total.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            if (fine_rollback_)
                ev_->request_fine_rollback_for_next_boundary();
            ev_->enter_mutation_boundary();
            // Issue #241: capture panic checkpoint at the OUTERMOST
            // guard only (nested guards share the outer checkpoint).
            // save_panic_checkpoint() snapshots `current-source` so
            // the source can be restored if the mutation rolls back.
            // It returns false if there's no workspace / no source /
            // no (current-source) primitive — in those cases the
            // Guard just skips the checkpoint step.
            if (outermost) {
                had_panic_checkpoint_ = ev_->save_panic_checkpoint();
                // Issue #813: Guard hot path uses explicit Result-style
                // control (success flag / checkpoint bool) — never throws.
                ev_->bump_guard_aura_result_path();
                if (had_panic_checkpoint_)
                    ev_->bump_guard_panic_checkpoint_aura_result();
            }
        }

    public:
        ~MutationBoundaryGuard() {
            if (!ev_ || inert_)
                return; // Issue #1590: quota soft-reject never entered a boundary
            bool success = flag_ ? *flag_ : true;
            // exit_mutation_boundary runs under the lock for
            // the outermost guard; lockless for nested guards
            // (lock is held by the outer guard).
            // exit_mutation_boundary runs under the lock for
            // the outermost guard; lockless for nested guards
            // (lock is held by the outer guard).
            int* slot = Evaluator::mutation_boundary_depth_slot(ev_);
            int prev = (*slot)--;
            bool outermost = (prev == 1);
            // Issue #1253 / #1373 / #1375 / #1747: outermost hold-duration
            // telemetry. Issue #1747: compute BatchMutationMetrics locally,
            // then publish with ≤6 atomic writes on the common path (was
            // 15+ scattered fetch_add/CAS on every dtor — cache-line bounce
            // under high-frequency mutate). Nested guards skip (no enter_ts_).
            if (outermost && enter_ts_.time_since_epoch().count() != 0) {
                const auto dur = std::chrono::steady_clock::now() - enter_ts_;
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(dur).count();
                const auto uus = static_cast<std::uint64_t>(us > 0 ? us : 0);
                if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
                    // ── local batch (no atomics yet) ──
                    struct BatchMutationMetrics {
                        std::uint64_t hold_us = 0;
                        std::uint64_t holds = 0;
                        std::uint64_t holds_over_1ms = 0;
                        std::uint64_t too_long = 0;
                        std::uint64_t starvation_prevented = 0;
                        std::uint64_t extreme = 0;
                        std::uint64_t contention_us = 0;
                        std::size_t hist_bucket = 0;
                        std::uint64_t long_fiber_id = 0;
                        bool update_max = false;
                        bool force_fail = false;
                    } b{};
                    b.hold_us = uus;
                    b.holds = 1;
                    if (uus > 1000)
                        b.holds_over_1ms = 1;
                    // Issue #1375: 9-bucket hold-time histogram.
                    b.hist_bucket = 8; // >1s
                    if (uus < 100)
                        b.hist_bucket = 0;
                    else if (uus < 500)
                        b.hist_bucket = 1;
                    else if (uus < 1000)
                        b.hist_bucket = 2;
                    else if (uus < 5000)
                        b.hist_bucket = 3;
                    else if (uus < 10000)
                        b.hist_bucket = 4;
                    else if (uus < 50000)
                        b.hist_bucket = 5;
                    else if (uus < 100000)
                        b.hist_bucket = 6;
                    else if (uus < 1000000)
                        b.hist_bucket = 7;
                    // Issue #1493: adaptive safepoint (may touch GC hooks; not a metric atomic).
                    ev_->adapt_gc_frequency_from_hold_us(uus);
                    // Issue #1443: threshold load (1 relaxed load; not a write).
                    const auto max_us = static_cast<std::int64_t>(
                        m->long_mutation_threshold_us.load(std::memory_order_relaxed));
                    if (us > max_us) {
                        b.too_long = 1;
                        b.starvation_prevented = 1;
                        b.contention_us = uus;
                        b.long_fiber_id = aura_fiber_current_id();
                        b.update_max = true;
                        ::aura_invoke_long_mutation_scheduler_hook(b.long_fiber_id, uus);
                        const auto extreme_us = static_cast<std::int64_t>(
                            m->max_extreme_mutation_us.load(std::memory_order_relaxed));
                        if (m->long_mutation_strict_mode.load(std::memory_order_relaxed) != 0 &&
                            us > extreme_us) {
                            b.extreme = 1;
                            b.force_fail = true;
                        }
                    } else {
                        // Only publish max if uus might raise it (1 load; CAS later if needed).
                        const auto prev_max =
                            m->mutation_hold_duration_us_max.load(std::memory_order_relaxed);
                        b.update_max = (uus > prev_max);
                    }

                    // ── publish common path: ≤6 atomic writes (#1747) ──
                    // 1–4: dual hold counters (legacy #1253 + agent #1373)
                    // 5: histogram bucket
                    // 6: max (store) when raised; over_1ms is rare path extra
                    m->mutation_hold_duration_us_total.fetch_add(b.hold_us,
                                                                 std::memory_order_relaxed);
                    m->mutation_hold_samples.fetch_add(b.holds, std::memory_order_relaxed);
                    m->mutation_boundary_hold_time_total_us.fetch_add(b.hold_us,
                                                                      std::memory_order_relaxed);
                    m->mutation_boundary_holds_total.fetch_add(b.holds, std::memory_order_relaxed);
                    m->mutation_boundary_hold_histogram[b.hist_bucket].fetch_add(
                        1, std::memory_order_relaxed);
                    if (b.update_max) {
                        // Single store is enough when uus is a new max under relaxed
                        // telemetry (races only lose intermediate max samples).
                        m->mutation_hold_duration_us_max.store(b.hold_us,
                                                               std::memory_order_relaxed);
                    }
                    // Optional / rare path atomics (not on every dtor).
                    if (b.holds_over_1ms)
                        m->mutation_boundary_holds_over_1ms_total.fetch_add(
                            b.holds_over_1ms, std::memory_order_relaxed);
                    if (b.too_long) {
                        m->mutation_too_long_total.fetch_add(b.too_long, std::memory_order_relaxed);
                        m->starvation_prevented_count.fetch_add(b.starvation_prevented,
                                                                std::memory_order_relaxed);
                        m->last_long_mutation_fiber_id.store(b.long_fiber_id,
                                                             std::memory_order_relaxed);
                        m->last_long_mutation_duration_us.store(b.hold_us,
                                                                std::memory_order_relaxed);
                        m->mutation_boundary_contention_us_hist.fetch_add(
                            b.contention_us, std::memory_order_relaxed);
                        if (b.extreme)
                            m->long_mutation_extreme_total.fetch_add(1, std::memory_order_relaxed);
                        if (b.force_fail && flag_)
                            *flag_ = false;
                    }
                    // Export-ready: one load + conditional store (avoid write every dtor).
                    if (m->runtime_obs_export_ready.load(std::memory_order_relaxed) == 0)
                        m->runtime_obs_export_ready.store(1, std::memory_order_relaxed);
                }
            }
            // Issue #1461: Agent Decision Metrics liveness — outermost
            // failed Guard must bump the fiber-boundary rollback counter
            // so (agent:decision-metrics) / stats facade see a real signal
            // (not a dead zero). Nested guards do not bump (outer owns the
            // transaction outcome).
            if (outermost && !success)
                ev_->bump_mutation_boundary_rollback();
            ev_->exit_mutation_boundary(success);
            // Issue #1486 / #1545 / #1568 / #1634: post-boundary linear closed-loop.
            // Unified consistency: scan Moved captures + enforce_all +
            // epoch fence + GC root audit (only_if_moved for Guard exit).
            // #1634: on failure, force full linear_post_mutate_enforce_all +
            // probe active closures so rollback cannot leave dangling
            // linear/JIT state after dual-epoch restore.
            if (outermost) {
                if (!success) {
                    (void)ev_->linear_post_mutate_enforce_all();
                    (void)ev_->enforce_linear_boundary_consistency(
                        Evaluator::kLinearGcRootAuditTypedMutate, /*mark_all_linear=*/true);
                    // Walk live closures so JIT/IR see post-rollback epoch.
                    ev_->walk_active_closures([](ClosureId, Closure&) {
                        // Metrics-only probe; deopt is driven by is_bridge_stale
                        // on next apply. Walk ensures registry is hot.
                    });
                    if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics()))
                        m->guard_failure_linear_enforce_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                } else {
                    (void)ev_->enforce_linear_boundary_consistency(
                        Evaluator::kLinearGcRootAuditTypedMutate, /*mark_all_linear=*/false);
                }
            }
            // Issue #1500: after restamp_all_node_generations inside
            // exit_mutation_boundary, pinned StableNodeRefs still hold
            // the pre-boundary gen — batch refresh them under the
            // still-held outermost write lock so Agent long-held pins
            // remain usable across the Guard boundary.
            if (outermost) {
                (void)ev_->restamp_pinned_stable_refs();
            }
            // Issue #285: explicit flush at the boundary exit so any
            // pending mutation stack state is visible to other fibers
            // BEFORE we drop the write lock. The flush runs
            // lockless (no shared_mutex acquire) and is cheap.
            // Only the outermost guard runs the flush; nested guards
            // don't need it (the outer guard handles visibility).
            if (outermost) {
                ev_->flush_mutation_boundary();
            }
            if (outermost) {
                // Issue #354: clear the flag that
                // Fiber::yield reads to detect "yield
                // while holding a mutation boundary".
                // We clear BEFORE releasing the write
                // lock so any concurrent Fiber::yield
                // observes the cleared flag (acquire
                // ordering on the flag load is
                // synchronized with the release
                // ordering on this store).
                ev_->mutation_boundary_held_.store(false, std::memory_order_release);
                lock_.unlock();
                // Issue #1523: pair Workspace acquire in ctor.
                aura::compiler::lock_order::on_release(
                    aura::compiler::lock_order::Level::Workspace);
                ev_->outermost_mutation_success_flag_ = nullptr;
                ev_->unbind_yield_hook_evaluator();
            }
            // Issue #241: panic-checkpoint commit / restore.
            // Only the outermost guard owns the checkpoint;
            // nested guards (which can't fail independently
            // of their outer) don't touch it.
            if (outermost && had_panic_checkpoint_) {
                if (success) {
                    // Mutation succeeded — checkpoint is no
                    // longer needed; clear so the next
                    // mutation starts fresh.
                    ev_->commit_panic_checkpoint();
                } else if (ev_->panic_auto_rollback_) {
                    // Mutation failed + auto-rollback enabled —
                    // restore the saved source via (set-code).
                    ev_->restore_panic_checkpoint();
                }
                // else: failed + !auto-rollback — leave the
                // checkpoint alive so a subsequent retry can
                // roll back to it. (Pre-#241 behavior on
                // failure was to leave the checkpoint.)
            }
            // Issue #417: verify stack/depth-slot consistency
            // after boundary exit (cross-TU drift detection).
            ev_->ensure_mutation_invariants();
            // Issue #422: hygiene violation detection hook on
            // Guard exit (mutate paths record attempts at block).
            ev_->ensure_hygiene_violation_detection();
            // Issue #464: bump the ArenaGroup
            // auto_compact_guard_call_count_ counter on
            // every guard dtor (the closed-loop signal for
            // long AI sessions). The actual
            // auto_compact_with_safety() call is wired in
            // #464 follow-up commits (Cycle 2) when the
            // fiber-safety check + safe-point integration
            // are in place. For now: counter only.
            // Issue #464: bump the ArenaGroup
            // auto_compact_guard_call_count_ counter on
            // every outermost guard exit (the closed-loop
            // signal for long AI sessions). The actual
            // auto_compact_with_safety() call is wired in
            // #464 follow-up commits (Cycle 2) when the
            // fiber-safety check + safe-point integration
            // are in place. For now: counter only.
            //
            // We bump on every outermost exit (regardless
            // of success) so the agent can monitor
            // mutation attempts (success + failure). The
            // success/failure distinction is a #464
            // follow-up. The counter is the precondition
            // that the AI Agent can monitor.
            if (outermost && ev_->arena_group_) {
                ev_->probe_arena_auto_policy_on_boundary_exit(success);
            }
            // Issue #490 / #1503: proactive Evaluator tag_arity_index
            // maintenance on successful outermost Guard exit:
            //   - EagerAfterMutate: always rebuild/sync
            //   - Lazy + warm index: auto incremental sync so the next
            //     query:pattern after self-mutate stays O(dirty), not a
            //     surprise O(N) full rebuild on large ASTs
            if (outermost && success && ev_->workspace_flat_) {
                const bool eager =
                    ev_->pattern_index_policy_ == PatternIndexPolicy::EagerAfterMutate;
                const bool warm_lazy = ev_->pattern_index_policy_ == PatternIndexPolicy::Lazy &&
                                       ev_->tag_arity_index_is_warm();
                if (eager || warm_lazy) {
                    if (warm_lazy)
                        ev_->bump_pattern_index_auto_warm_syncs();
                    ev_->build_tag_arity_index(
                        static_cast<std::uint8_t>(eager ? PatternIndexRebuildTrigger::EagerMutate
                                                        : PatternIndexRebuildTrigger::LazyQuery));
                }
            }
            // Issue #1252: post-mutate linear ownership revalidate on
            // successful outermost Guard exit (#672 path made mandatory).
            if (outermost && success) {
                ev_->bump_linear_post_mutate_enforcement();
                if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
                    m->mutation_boundary_linear_revalidations.fetch_add(1,
                                                                        std::memory_order_relaxed);
                }
            } else if (outermost && !success) {
                if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
                    m->mutation_boundary_steal_recoveries.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Issue #1255: on Guard exit, if hygiene drift was seen,
            // force DefUseIndex sync before releasing the boundary.
            if (outermost && ev_->workspace_flat_) {
                const auto dirty = ev_->workspace_flat_->mark_dirty_upward_call_count();
                if (dirty > 0) {
                    if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
                        m->pattern_hygiene_defuse_sync_on_guard.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
            }
            // unique_lock destructor runs automatically here.
        }
        MutationBoundaryGuard(const MutationBoundaryGuard&) = delete;
        MutationBoundaryGuard& operator=(const MutationBoundaryGuard&) = delete;
        // Issue #266: capture sym_id_/param column snapshots for the
        // active boundary. Call before mutations when fine_rollback was
        // not requested at construction time.
        void enable_fine_rollback() noexcept {
            if (!ev_ || !ev_->workspace_flat_)
                return;
            auto& stack = ev_->active_mutation_stack();
            if (stack.empty())
                return;
            auto& cp = stack.back();
            if (cp.fine_rollback)
                return;
            cp.fine_rollback = true;
            cp.sym_id_snapshot = ev_->workspace_flat_->snapshot_sym_id();
            cp.param_snapshot = ev_->workspace_flat_->snapshot_param_columns();
            fine_rollback_ = true;
        }

        [[nodiscard]] BoundaryRollbackStats get_rollback_stats() const noexcept {
            return ev_ ? ev_->last_boundary_rollback_stats() : BoundaryRollbackStats{};
        }
        // Issue #453: returns true if this guard captured a panic
        // checkpoint (i.e. save_panic_checkpoint() succeeded at ctor).
        // Used by `Evaluator::pending_panic_checkpoint()` to decide
        // whether a fiber migration should transfer the checkpoint.
        // Only the outermost guard has a meaningful value here
        // (nested guards never call save_panic_checkpoint).
        [[nodiscard]] bool has_pending_checkpoint() const noexcept { return had_panic_checkpoint_; }
        // Issue #459: per-guard atomic-batch accessors.
        // `is_atomic_batch_active()` returns true when this
        // guard was entered under a (mutate:atomic-batch)
        // — the caller can use this to detect a violation
        // (e.g. a fiber steal happening while an atomic
        // guard is held).
        [[nodiscard]] bool is_atomic_batch_active() const noexcept { return atomic_batch_active_; }
        // `suppress_generation_bump(true)` marks this guard
        // as a suppressed-bump guard. The ctor reads the
        // flag to decide whether to skip the defuse_version_
        // bump on enter (the (mutate:atomic-batch) primitive
        // does a single bump on commit instead, saving N-1
        // bumps for N ops in a batch).
        void suppress_generation_bump(bool v) noexcept { suppress_bump_ = v; }
        [[nodiscard]] bool is_suppress_bump_set() const noexcept { return suppress_bump_; }

        MutationBoundaryGuard(MutationBoundaryGuard&& o) noexcept
            : had_panic_checkpoint_(o.had_panic_checkpoint_)
            , fine_rollback_(o.fine_rollback_)
            , ev_(o.ev_)
            , flag_(o.flag_)
            , lock_(std::move(o.lock_)) {
            // Move transfers the lock state (the unique_lock
            // member moves its ownership to the new guard).
            // The depth counter was already incremented by
            // the source's ctor — it stays. The source's
            // dtor will see ev_=nullptr and no-op, so the
            // depth stays correctly at the same value.
            // The new guard will decrement it once when it
            // destructs.
            o.ev_ = nullptr;
            o.flag_ = nullptr;
        }
        MutationBoundaryGuard& operator=(MutationBoundaryGuard&& o) noexcept {
            if (this != &o) {
                if (ev_)
                    ev_->exit_mutation_boundary(flag_ ? *flag_ : true);
                ev_ = o.ev_;
                flag_ = o.flag_;
                lock_ = std::move(o.lock_);
                o.ev_ = nullptr;
                o.flag_ = nullptr;
            }
            return *this;
        }

    private:
        Evaluator* ev_;
        bool* flag_;
        // Issue #233: hold the exclusive workspace write lock
        // for the guard's lifetime. The lock is initialized in
        // deferred mode and only locked for the outermost guard
        // (depth 0→1). Nested guards increment the depth counter
        // but do NOT touch the mutex — the outer guard already
        // holds it.
        std::unique_lock<std::shared_mutex> lock_;
    };

public:
    // Issue #157 Phase 1: lock hooks for the JIT runtime bridges
    // (aura_lock_workspace_read/write + aura_unlock_workspace_read/write
    // in aura_jit_runtime.cpp). These are public thin wrappers that
    // service.ixx's register_jit_primitives() binds to a global
    // LockHooks struct so the JIT code can participate in the
    // workspace_mtx_ + defuse_version_ protocol. Without these, the
    // JIT-specialized L2 paths (OpCar/OpCdr SHAPE_PAIR) and runtime
    // bridges (aura_alloc_pair, aura_pair_car, aura_prim_call) bypass
    // the protocol and race with mutate:foo.
    //
    // The hook pattern is the same as g_prim_dispatcher: a function
    // pointer table that service.ixx sets on CompilerService init.
    // This keeps workspace_mtx_ private to Evaluator (no global mutex)
    // while still letting global C functions participate in locking.
    // Issue #917: prefer WorkspaceSharedLock / WorkspaceUniqueLock RAII below.
    // Raw hooks remain for JIT C ABI (aura_lock_workspace_*).
    // Issue #1523: report Workspace level into lock_order TLS when
    // JIT / C bridges take workspace locks (canonical #1388 order).
    void lock_workspace_shared() {
        aura::compiler::lock_order::on_acquire(aura::compiler::lock_order::Level::Workspace);
        workspace_mtx_.lock_shared();
    }
    void unlock_workspace_shared() {
        workspace_mtx_.unlock_shared();
        aura::compiler::lock_order::on_release(aura::compiler::lock_order::Level::Workspace);
    }
    void lock_workspace_unique() {
        aura::compiler::lock_order::on_acquire(aura::compiler::lock_order::Level::Workspace);
        workspace_mtx_.lock();
    }
    void unlock_workspace_unique() {
        workspace_mtx_.unlock();
        aura::compiler::lock_order::on_release(aura::compiler::lock_order::Level::Workspace);
    }

    // Issue #917 Phase 1: RAII workspace locks for C++ call sites.
    class WorkspaceSharedLock {
        std::shared_lock<std::shared_mutex> lock_;

    public:
        explicit WorkspaceSharedLock(Evaluator& ev)
            : lock_(ev.workspace_mtx_) {}
    };
    class WorkspaceUniqueLock {
        std::unique_lock<std::shared_mutex> lock_;

    public:
        explicit WorkspaceUniqueLock(Evaluator& ev)
            : lock_(ev.workspace_mtx_) {}
    };
    // Non-blocking shared lock for read paths that may be
    // called from within the IR interpreter (e.g. snapshot()
    // bumping marker counts from inside a query primitive). If
    // the same thread already holds unique_lock (e.g. via
    // set-code / restore-ast / mutate:*), the blocking
    // lock_shared() would throw EDEADLK ("Resource deadlock
    // avoided") under pthread robust-mutex semantics. Returns
    // true on acquire, false on contention — callers may
    // safely skip the read.
    bool try_lock_workspace_shared() { return workspace_mtx_.try_lock_shared(); }

    // Issue #157 Phase 1: defuse_version_ accessor for the JIT
    // version check (aura_get_defuse_version in aura_jit_runtime.cpp).
    // Phase 1b (deferred to follow-up) will use this in the L2
    // SHAPE_PAIR paths to do a version check at entry; on mismatch,
    // deopt to the slow path. For Phase 1 we just expose the accessor.
    //
    // Issue #184: the accessor uses acquire ordering so the JIT
    // runtime's version check synchronizes with any release-store
    // performed by enter_mutation_boundary() on another thread.
    // (Note: a public `get_defuse_version()` already exists
    //  at line ~2555; this internal-scope copy has been removed
    //  to avoid overload collision.)

    // Issue #157 Phase 1: yield-mutation-boundary hook for the JIT.
    // High-level mutate primitives call g_fiber_yield_mutation_boundary
    // (extern function pointer defined in messaging_bridge_impl.cpp)
    // after taking the write lock + defuse_version_++; the JIT runtime
    // bridges can do the same via this hook to keep the multi-agent
    // scheduler fair.
    //
    // Forward-declared here (rather than including messaging_bridge.h)
    // because evaluator.ixx is a module interface and the bridge
    // header is a non-module .h — including a non-module header in
    // a module interface causes "declaration in module implementation
    // unit" errors. The actual implementation is in evaluator_fiber_mutation.cpp
    // which DOES include messaging_bridge.h.
    void yield_mutation_boundary();

    // Issue #1504: Agent-facing cooperative yield at a *safe* point.
    // Contract (non-duplicative to #362 / #1014):
    //   - If MutationBoundaryGuard is held (depth > 0 or held flag):
    //     do NOT yield (would deadlock on workspace_mtx_); count skip.
    //   - Else: yield with MutationBoundary reason when a fiber is
    //     active; if no fiber (test/stdin path), no-op count as safe.
    // Returns: 0 = yielded (or safe no-op no-fiber), 1 = skipped-held.
    // Optional timeout_ms is reserved for future preemption (MVP ignored).
    int try_safe_yield_at_boundary(std::int64_t timeout_ms = 0) noexcept;
    [[nodiscard]] std::uint64_t get_safe_yield_ok_total() const noexcept {
        return safe_yield_ok_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_safe_yield_skipped_held_total() const noexcept {
        return safe_yield_skipped_held_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_safe_yield_no_fiber_total() const noexcept {
        return safe_yield_no_fiber_total_.load(std::memory_order_relaxed);
    }
    // Issue #1504: per-Evaluator thread-local Guard nesting (depth slot).
    [[nodiscard]] int mutation_boundary_depth_slot_value() const noexcept;

    // Issue #285: explicit mutation-boundary flush. Called from
    // MutationBoundaryGuard destructor and Fiber::yield
    // (MutationBoundary reason) to make sure:
    //   1. The per-fiber MutationCheckpoint stack is consistent
    //      (top-of-stack reflects the current boundary's state).
    //   2. defuse_version_ is visible to other threads (acquire
    //      barrier so subsequent stale-frame checks see the bump).
    //   3. If a fiber is active, the active mutation stack is the
    //      per-fiber one (we re-route via active_mutation_stack()).
    //
    // No-op when not in a mutation boundary (safe to call from
    // Fiber::yield unconditionally).
    void flush_mutation_boundary();

    // Issue #236: helpers used by mutate:atomic-batch to apply
    // sub-ops WITHOUT acquiring the workspace write lock (the
    // batch's outer MutationBoundaryGuard already holds it —
    // calling the existing primitives from inside the batch
    // would cause each sub-op to try to acquire the same lock
    // a second time, triggering a deadlock via std::shared_mutex
    // which is not recursive).
    //
    // The MVP supports :rebind (the most common refactor op);
    // :replace-value and :tweak-literal are stubs that return
    // a "not yet supported in batch" error. Agents needing those
    // ops can fall back to the standalone primitive (and accept
    // that those ops aren't transactional with the batch).
    EvalResult eval_flat_apply_mutate_rebind(std::span<const types::EvalValue> a);
    EvalResult eval_flat_apply_mutate_replace_value(std::span<const types::EvalValue> a);
    EvalResult eval_flat_apply_mutate_tweak_literal(std::span<const types::EvalValue> a);
    // Issue #396 Phase 2: lockless variants for the two
    // high-frequency mutate operations that previously fell
    // back to error inside atomic-batch. Used by
    // (mutate:atomic-batch) sub-op routing; the wrapper
    // primitives (mutate:remove-node / mutate:insert-child)
    // still use these internally too.
    EvalResult eval_flat_apply_mutate_remove_node(std::span<const types::EvalValue> a);
    EvalResult eval_flat_apply_mutate_insert_child(std::span<const types::EvalValue> a);
    // Issue #1900: 10 lockless helpers extracted to expand (mutate:atomic-batch)
    // dispatch from 5 → 14 ops. Each helper does the inner mutation work WITHOUT
    // acquiring a nested MutationBoundaryGuard (the outer atomic-batch Guard
    // already holds workspace_mtx_ for the whole batch). Helpers also skip the
    // workspace_read_only_ check, lazy COW trigger, post-mutate typecheck, and
    // linear-ownership validation — the outer batch performs these once.
    EvalResult eval_flat_apply_mutate_set_body(std::span<const types::EvalValue> a);
    EvalResult eval_flat_apply_mutate_replace_pattern(std::span<const types::EvalValue> a);
    EvalResult eval_flat_apply_mutate_replace_subtree(std::span<const types::EvalValue> a);
    EvalResult eval_flat_apply_mutate_splice(std::span<const types::EvalValue> a);
    EvalResult eval_flat_apply_mutate_wrap(std::span<const types::EvalValue> a);
    EvalResult eval_flat_apply_mutate_rename_symbol(std::span<const types::EvalValue> a);
    EvalResult eval_flat_apply_mutate_move_node(std::span<const types::EvalValue> a);
    EvalResult eval_flat_apply_mutate_inline_call(std::span<const types::EvalValue> a);

    // Issue #165 Phase 1B: post-mutation macro re-expansion.
    // Walks the affected subtree of a mutation record looking
    // for MacroDef nodes (the macro's body was mutated) or
    // Call nodes whose callee resolves to a registered macro
    // (the call site's context was mutated). Re-expands them
    // via clone_macro_body + expand_inner_macros with fresh
    // gensym for hygiene, and sets SyntaxMarker::MacroIntroduced
    // on the new expansion so downstream consumers know the
    // code is macro-introduced.
    //
    // Returns the number of call sites re-expanded. Zero is
    // a valid result (the mutation didn't touch any macro-
    // related state). The function is safe to call on any
    // mutation record — it does its own precondition checks
    // and bails on malformed input.
    //
    // The pattern mirrors post_mutation_invariant_check
    // (Issue #147) — pure function, no service state, the
    // caller (typed_mutate) invokes it after tx.commit.
    std::size_t post_mutation_macro_reexpand(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                             const aura::ast::MutationRecord& rec);

    // Type-aliasing accessor for the mutex type. Lets the
    // C++ test surface verify the lock type (Issue #107) without
    // exposing the actual mutex (which is internal state).
    using WorkspaceMutex = std::shared_mutex;

    // Issue #107 part 4 (deferred): inline typecheck helper
    // declarations were prototyped here but the implementation
    // + 4 fuzzer call-site conversions require careful handling
    // of the surrounding if/else scopes (an over-eager sed left
    // dangling tc_r references in the first attempt). The 4 fuzzer
    // paths (~11575, ~11846, ~11963, ~11992) still call
    // typecheck-current via the primitive, which would deadlock
    // under the new shared/exclusive lock if they were entered
    // from a mutate context. The 4 fuzzer call sites
    // (lines ~11575, 11846, 11963, 11992) now use the inline
    // helpers below instead. They are NOT in the default test
    // path (only reached via the explicit fuzzer Aura
    // primitives), so the changes are correctness-only and
    // covered by the runtime test path for the helpers
    // themselves.
    std::string run_typecheck_no_lock();
    bool run_typecheck_no_lock_bool();
    // Issue #526: selective infer_flat_partial on the latest
    // MutationRecord when the log is non-empty; full infer_flat
    // fallback otherwise. Applies CoercionMap before return.
    bool run_post_mutate_typecheck_no_lock();
};


// Pair-aware value formatting (recursively prints lists)
export inline std::string format_value(const types::EvalValue& v, std::span<const std::string> heap,
                                       std::span<const Pair> pairs, int depth = 0,
                                       const Primitives* primitives = nullptr,
                                       std::span<const std::string> keywords = {}) {
    const int max_depth = 64;
    if (depth > max_depth)
        return "...";
    if (types::is_void(v))
        return "()";
    if (types::is_bool(v))
        return types::as_bool(v) ? "#t" : "#f";
    if (types::is_int(v))
        return std::to_string(types::as_int(v));
    if (types::is_float(v))
        return std::to_string(types::as_float(v));
    // IMPORTANT: Check is_string BEFORE is_keyword (Issue #96 bug fix;
    // pre-#181 encoding rationale). The current v2 encoding
    // (Issue #181) has (v & 3) == 2 as the dedicated string tag,
    // disjoint from ref / special. The ordering is still
    // semantically important (a string at a particular idx is
    // never a keyword), but the encoding no longer relies on
    // ordering for correctness.
    if (types::is_string(v)) {
        if (!heap.empty()) {
            auto idx = types::as_string_idx(v);
            if (idx < heap.size())
                return std::format("\"{}\"", heap[idx]);
        }
        return std::format("<string[{}]>", types::as_string_idx(v));
    }
    if (types::is_keyword(v)) {
        auto kidx = types::as_keyword_idx(v);
        if (!keywords.empty() && kidx < keywords.size())
            return keywords[kidx];
        return ":" + std::to_string(kidx);
    }
    if (types::is_pair(v) && !pairs.empty()) {
        auto idx = types::as_pair_idx(v);
        if (idx >= pairs.size())
            return std::format("<pair[{}]>", idx);

        // Walk the cdr chain to collect all elements
        std::vector<std::string> elements;
        auto current = v;

        while (types::is_pair(current)) {
            auto cidx = types::as_pair_idx(current);
            if (cidx >= pairs.size()) {
                break;
            }
            elements.push_back(
                format_value(pairs[cidx].car, heap, pairs, depth + 1, primitives, keywords));
            current = pairs[cidx].cdr;
            if (elements.size() > 256) {
                elements.push_back("...");
                break;
            }
        }

        std::string result = "(";
        for (std::size_t i = 0; i < elements.size(); ++i) {
            if (i > 0)
                result += " ";
            result += elements[i];
        }
        // Check end-of-list: void sentinel (11) or fixnum 0 are list terminators
        bool is_proper = (current.val == 11) || (current.val == 0) ||
                         (types::is_int(current) && types::as_int(current) == 0);
        if (is_proper) {
            // proper list
        } else {
            if (!elements.empty())
                result += " . ";
            result += format_value(current, heap, pairs, depth + 1, primitives, keywords);
        }
        result += ")";
        return result;
    }
    if (types::is_vector(v))
        return std::format("<vector[{}]>", types::as_vector_idx(v));
    if (types::is_hash(v))
        return std::format("<hash[{}]>", types::as_hash_idx(v));
    if (types::is_closure(v)) {
        std::println("⚠ program returned an uncalled function");
        return "#<procedure>";
    }
    if (types::is_cell(v))
        return std::format("<cell[{}]>", types::as_cell_id(v));
    if (types::is_primitive(v)) {
        if (primitives) {
            auto slot = types::as_primitive_slot(v);
            if (slot < primitives->slot_count())
                return std::format("<primitive:{}>", primitives->name_for_slot(slot));
        }
        return "<primitive>";
    }
    if (types::is_module(v))
        return "<module>";
    if (types::is_error(v))
        return "<error>";
    return "<unknown>";
}

// Pre-expand all macros in a FlatAST. Returns (possibly new) root.
//
// Issue #230 (Bug 4): the previous default of 10 passes was too low
// for deeply nested macro expressions — stdlib modules like
// std/struct produce 12-15 levels of nested expansion. The new
// default of 32 covers all current call sites (max measured depth
// is 18 in test_issue_137) and still terminates fast for non-macro
// inputs (the function early-exits when no MacroDef is found).
export using aura::compiler::macro_exp::macro_expand_all;

// ══════════════════════════════════════════════════════════════════
// Issue #145: EnvView + ClosureView — zero-copy span views
// ══════════════════════════════════════════════════════════════════
//
// These mirror the existing NodeView pattern (ast.ixx): a
// non-owning read view that exposes the underlying storage as
// std::spans. No allocation, no copy. LLM tooling and the JIT
// bridge can consume them without touching the raw pointers or
// the cell-dereference dance.
//
// Why a separate type vs adding more accessors to Env/Closure?
//   1. The views make the "no-mutation" intent explicit (no
//      bind() or set_parent() on the view).
//   2. JIT/AOT codegen can pattern-match on the view type at
//      compile time and skip the cell-deref branch if cells_
//      is known to be empty.
//   3. Tests can construct a view from a small fixed buffer
//      without spinning up a full Env/Closure.

// ── EnvView — read-only view over an Env's bindings ────────
// Exposes both the string-keyed bindings (legacy) and the
// SymId-keyed bindings (Issue #145 fast path). Spans stay
// valid as long as the underlying Env does.
export struct EnvView {
    std::span<const std::pair<std::string, types::EvalValue>> string_bindings;
    std::span<const std::pair<aura::ast::SymId, types::EvalValue>> symid_bindings;
    const Env* parent = nullptr;

    // Lookup a name in the string-keyed bindings. Walks to
    // parent_ if not found locally.
    [[nodiscard]] std::optional<types::EvalValue> lookup(const std::string& name) const;

    // Lookup a SymId in the SymId-keyed bindings (Issue #145
    // fast path). Walks to parent_ if not found locally.
    [[nodiscard]] std::optional<types::EvalValue> lookup_by_symid(aura::ast::SymId s) const;

    // Issue #145 follow-up / Phase 2.5.0: SymId-first lookup
    // helper that takes a name string + pool. Same role as
    // Env::lookup_by_intern but for EnvView (which has no
    // pool_ field — the pool is always passed in). Used by
    // the parent-walk migration in Phase 2.5.0 commit 8.
    std::optional<types::EvalValue> lookup_by_intern(const std::string& n,
                                                     const aura::ast::StringPool* pool) const
        pre(!n.empty());

    // Number of local bindings (excludes parent).
    [[nodiscard]] std::size_t size() const { return string_bindings.size(); }
};

// ── ClosureView — read-only view over a Closure's fields ────
// Exposes params as a SymId span (Issue #145 SoA). Other
// fields are direct; no copy.
export struct ClosureView {
    std::span<const aura::ast::SymId> params;
    aura::ast::NodeId body_id = aura::ast::NULL_NODE;
    bool dotted = false;
    const aura::ast::FlatAST* flat = nullptr;
    const aura::ast::StringPool* pool = nullptr;
    // P0: legacy env removed from ClosureView too. Use env_id
    // for inspection of captured env (via Evaluator env_frame if needed).
    EnvId env_id = NULL_ENV_ID;
    const aura::ast::ASTArena* owner_arena = nullptr;
    std::string_view name;

    // Returns the i-th param's SymId, or NULL_SYM if out of range.
    [[nodiscard]] aura::ast::SymId param_at(std::size_t i) const {
        return i < params.size() ? params[i] : aura::ast::SymId{};
    }
    [[nodiscard]] std::size_t arity() const { return params.size(); }
};

// Factory functions (not members, to keep Env/Closure
// header-light).
export EnvView make_env_view(const Env& env);
export ClosureView make_closure_view(const Closure& cl);

// WorkspaceTree method bodies (declared above Evaluator).
inline bool WorkspaceTree::ensure_local_flat(std::uint32_t idx) {
    auto& n = nodes_[idx];
    if (n.is_root)
        return true;
    if (n.read_only)
        return false;
    if (n.has_own_flat)
        return true;
    if (n.parent_flat_) {
        std::size_t parent_bytes = 0;
        if (n.parent_pool_)
            parent_bytes += n.parent_pool_->data_size();
        if (n.parent_flat_)
            parent_bytes += n.parent_flat_->size() * 64;
        if (n.memory_budget > 0 && (n.memory_used + parent_bytes) > n.memory_budget) {
            ++n.cow_refused_count;
            return false;
        }
        // Issue #975: unique_ptr RAII — if copy throws bad_alloc, no leak.
        auto new_flat = std::make_unique<ast::FlatAST>();
        auto new_pool = std::make_unique<ast::StringPool>();
        *new_flat = *n.parent_flat_;
        *new_pool = *n.parent_pool_;
        n.flat = new_flat.release();
        n.pool = new_pool.release();
        n.has_own_flat = true;
        n.cow_epoch = ++cow_epoch_;
        n.generation = n.flat->generation();
        n.memory_used = parent_bytes;
        n.remap.reset_identity(n.parent_layer_idx, n.cow_epoch, n.flat->size());
        return true;
    }
    return false;
}

inline ast::NodeId WorkspaceTree::remap_node_id(std::uint32_t from_layer, ast::NodeId id,
                                                std::uint32_t to_layer) const noexcept {
    if (from_layer >= nodes_.size() || to_layer >= nodes_.size() || id == ast::NULL_NODE)
        return ast::NULL_NODE;
    if (from_layer == to_layer)
        return id;
    ast::NodeId cur = id;
    if (from_layer < to_layer) {
        for (std::uint32_t layer = from_layer + 1; layer <= to_layer; ++layer)
            cur = nodes_[layer].remap.resolve_from_parent(cur);
    } else {
        for (std::uint32_t layer = from_layer; layer > to_layer; --layer)
            cur = nodes_[layer].remap.resolve_to_parent(cur);
    }
    return cur;
}

inline std::optional<ast::FlatAST::StableNodeRef>
WorkspaceTree::resolve_stable_ref(std::uint32_t from_layer, ast::FlatAST::StableNodeRef ref,
                                  std::uint32_t to_layer) const noexcept {
    if (from_layer >= nodes_.size() || to_layer >= nodes_.size())
        return std::nullopt;
    const auto* target_flat = nodes_[to_layer].flat;
    if (!target_flat)
        return std::nullopt;
    if (from_layer == to_layer) {
        if (!target_flat->is_valid(ref))
            return std::nullopt;
        auto out = ref;
        out.workspace_id = to_layer;
        return out;
    }
    const auto mapped = remap_node_id(from_layer, ref.id, to_layer);
    if (!target_flat->is_live_node(mapped))
        return std::nullopt;
    ast::FlatAST::StableNodeRef out{mapped,
                                    target_flat->generation(),
                                    ref.mutation_id_at_capture,
                                    to_layer,
                                    ref.fiber_id,
                                    ref.last_validated_generation,
                                    ref.wrap_epoch,
                                    ref.subtree_gen_at_capture,
                                    target_flat->workspace_cow_epoch(),
                                    ref.boundary_pinned};
    return out;
}

inline std::uint32_t WorkspaceTree::create_child(const std::string& name,
                                                 std::uint32_t parent_layer_idx,
                                                 ast::FlatAST* parent_flat,
                                                 ast::StringPool* parent_pool) {
    auto idx = static_cast<std::uint32_t>(nodes_.size());
    WorkspaceNode node;
    node.name = name;
    node.parent_layer_idx = parent_layer_idx;
    node.parent_flat_ = parent_flat;
    node.parent_pool_ = parent_pool;
    node.flat = parent_flat;
    node.pool = parent_pool;
    nodes_.push_back(std::move(node));
    return idx;
}

inline bool WorkspaceTree::delete_child(std::uint32_t idx) {
    if (idx == 0 || idx >= nodes_.size())
        return false;
    auto& n = nodes_[idx];
    if (n.has_own_flat) {
        delete n.flat;
        delete n.pool;
    }
    n.flat = nullptr;
    n.pool = nullptr;
    n.parent_flat_ = nullptr;
    n.parent_pool_ = nullptr;
    n.remap = ast::mutation::NodeIdRemapTable{};
    n.cow_epoch = 0;
    n.generation = 0;
    return true;
}

inline bool WorkspaceTree::set_active(std::uint32_t idx) {
    if (idx >= nodes_.size())
        return false;
    const std::uint32_t from = active_idx_;
    active_idx_ = idx;
    // Issue #1257 Phase 1: on layer switch, auto-pin any COW-boundary
    // refs that would cross the new active layer (provenance refresh
    // is completed by Evaluator::on_workspace_layer_switch when wired).
    (void)from;
    return true;
}

inline void WorkspaceTree::set_read_only(std::uint32_t idx, bool ro) {
    if (idx < nodes_.size())
        nodes_[idx].read_only = ro;
}

inline bool WorkspaceTree::can_write(std::uint32_t idx) {
    if (idx >= nodes_.size())
        return false;
    return !nodes_[idx].read_only;
}

} // namespace aura::compiler
