// Issue #221: include the PersistentChildVector header in the
// module's global module fragment (same trick as ast.ixx for
// gap_buffer.hh / persistent_child_vector.hh). The global
// fragment is processed BEFORE the module's purview, so the
// std includes in persistent_child_vector.hh don't conflict
// with `import std;`.
module;

#include "../core/persistent_child_vector.hh"

export module aura.compiler.evaluator;
import std;
import aura.core;
import aura.compiler.ffi_primitives;
import aura.compiler.adt_runtime;  // Step 2.3 wiring (exact FFI pattern)
import aura.diag;
import aura.compiler.value;
import aura.compiler.evaluator_pure;

namespace aura::compiler {


using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;

export class Primitives {
public:
    Primitives();
    std::optional<PrimFn> lookup(const std::string& n) const
        pre (!n.empty());
    void add(const std::string& name, PrimFn fn) {
        auto slot = ordered_names_.size();
        table_[name] = std::move(fn);
        ordered_names_.push_back(name);
    }
    void set_string_heap(std::pmr::vector<std::string>* h) { string_heap_ = h; }
    std::span<const std::string> string_heap() const { return *string_heap_; }
    std::pmr::vector<std::string>& string_heap() { return *string_heap_; }
    // Slot-based lookup for primitive values
    const std::string& name_for_slot(std::size_t slot) const { return ordered_names_[slot]; }
    std::size_t slot_for_name(const std::string& name) const;
    std::size_t slot_count() const { return ordered_names_.size(); }

private:
    std::unordered_map<std::string, PrimFn> table_;
    // Issue #145 Phase 2.4: pmr-backed to match Evaluator's
    // string_heap_ arena allocation. Vector metadata lives in
    // the same monotonic arena as the underlying EvalValue /
    // Pair storage; std::string char buffers still heap-allocate
    // (Phase 2.4.1 will move string contents inline).
    std::pmr::vector<std::string>* string_heap_ = nullptr;
    std::vector<std::string> ordered_names_;
};

// Forward declaration: Evaluator is defined below.
// Required so Env can hold an Evaluator* back-pointer for
// Phase 2.2 SoA-walk migration (Issue #145).
export class Evaluator;

// EnvId — uint32_t index into Evaluator::env_frames_ arena.
// Full SoA documentation in §2.7.5 / §2.7.6 of
// docs/design/cpp26_guide.md. Declared here (above Env) so
// Env can hold an EnvId parent_id_ field. Issue #145 Phase 2.1.
export using EnvId = std::uint32_t;
export constexpr EnvId NULL_ENV_ID = std::numeric_limits<EnvId>::max();

export class Env final {
public:
    Env() = default;
    explicit Env(const Env* p)
        : parent_(p), owner_(p ? p->owner_ : nullptr),
          parent_id_(p ? p->parent_id_ : NULL_ENV_ID) {}
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
    // Issue #207 (Cycle 1): bind(name, value) writes to
    // bindings_ only. The migration to route through
    // bind_symid is a Cycle 2 item (requires pool_ to
    // become non-const, which is a bigger refactor that
    // touches many call sites).
    void bind(const std::string& n, types::EvalValue v) {
        bindings_.emplace_back(n, std::move(v));
    }
    // Issue #145: SymId fast path. The apply_closure loop hits
    // this once per parameter per call — replacing the old
    // string-compare lookup with integer-compare. Implemented
    // in evaluator_impl.cpp.
    void bind_symid(aura::ast::SymId s, types::EvalValue v);
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
    [[nodiscard]] std::optional<types::EvalValue> lookup(const std::string& n) const
        pre (!n.empty());
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
    std::optional<types::EvalValue>
    lookup_by_intern(const std::string& n,
                     const aura::ast::StringPool* pool) const
        pre (!n.empty());
    // Look up the raw binding without dereferencing cells (returns cell sentinel as-is)
    std::optional<types::EvalValue> lookup_binding(const std::string& n) const
        pre (!n.empty());
    std::optional<PrimFn> lookup_primitive(const std::string& n) const {
        return primitives_ ? primitives_->lookup(n) : std::nullopt;
    }
    types::EvalValue* lookup_cell_ptr(const std::string& n,
                                      std::vector<types::EvalValue>* cells) const;
    // Return cell index (stable across vector reallocation) or nullopt if not a cell
    std::optional<std::uint64_t> lookup_cell_index(const std::string& n) const;
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
    [[nodiscard]] std::vector<std::pair<std::string, types::EvalValue>>
    bindings_with_names() const;
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
    const aura::ast::StringPool* pool_ = nullptr;  // Issue #145
    // P0 step 2: cells_ pointer removed (was used for cell deref in
    // lookups). Bindings now always return the raw value (cell
    // sentinel if applicable); deref centralized via Evaluator
    // or passed cells to lookup_cell helpers. This + EnvFrame
    // change eliminates one class of pointer-to-reallocatable-heap.
    std::vector<std::pair<std::string, types::EvalValue>> bindings_;
    // Issue #145: parallel SymId-keyed store. Both arrays
    // share the same length and order. lookup_by_symid reads
    // bindings_symid_ (integer compare). bind_symid writes to
    // both (and resolves SymId→string via pool_ to mirror).
    std::vector<std::pair<aura::ast::SymId, types::EvalValue>> bindings_symid_;
    // Issue #145 Phase 2.2: SoA walk infrastructure.
    // `owner_` is the Evaluator that owns the env_frames_ arena
    // used for parent-chain walk. `parent_id_` is the index of
    // this Env's parent frame in that arena (NULL_ENV_ID =
    // no parent or legacy pointer walk). Set when the Env is
    // registered with an Evaluator (top_, modules_).
    Evaluator* owner_ = nullptr;
    EnvId parent_id_ = NULL_ENV_ID;
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
export struct EnvFrame {
    EnvId parent_id = NULL_ENV_ID;
    const Primitives* primitives_ = nullptr;
    const aura::ast::StringPool* pool_ = nullptr;
    // P0 (EnvFrame SoA migration): removed raw cells_ pointer.
    // EnvFrame is now pure data (bindings + parent_id index).
    // Cell deref (when bound value is cell sentinel) is centralized
    // in Evaluator::lookup_by_symid_chain (and legacy Env paths)
    // using the Evaluator-owned central pmr cells_ vector.
    // This eliminates one source of pointer-to-reallocatable-heap
    // and prepares for full removal of cells_/pairs_ pointers from
    // all env representations. See evaluator_impl.cpp lookup_local
    // variants and lookup_by_symid_chain.
    std::vector<std::pair<std::string, types::EvalValue>> bindings_;
    // Phase 1 parity: parallel SymId-keyed store. Same length
    // and order as bindings_. SymId fast path reads this;
    // bind_symid mirrors into both when pool_ is set.
    std::vector<std::pair<aura::ast::SymId, types::EvalValue>> bindings_symid_;

    // Bind by name. Resolves to SymId via pool_ if set, mirrors
    // to bindings_symid_ so legacy lookup_by_symid still finds
    // it. If pool_ is null, only the string array is written.
    void bind(const std::string& n, types::EvalValue v);
    // Bind by SymId (fast path). Mirrors to bindings_ when
    // pool_ is set, so legacy lookup(string) in the lambda body
    // still finds the param.
    void bind_symid(aura::ast::SymId s, types::EvalValue v);
    // Local-only lookup (no parent walk). Phase 2.2 will add
    // walk-aware variants alongside `Evaluator::walk_env_frames`.
    std::optional<types::EvalValue> lookup_local(
        const std::string& n) const
        pre (!n.empty());
    std::optional<types::EvalValue> lookup_local_by_symid(
        aura::ast::SymId s) const;
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
    std::string name = "";  // function name (empty for lambdas)
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
    ast::ASTArena* owner_arena = nullptr;  // arena where flat/pool/env lives
    // Issue #223: epoch captured at closure construction. The
    // IRExecutor / apply_closure compares this against the
    // service's bridge_epoch(); a mismatch means the closure's
    // flat*/pool* are stale (arena was reset or a major mutation
    // invalidated the captured pointers). Default 0 = legacy
    // / unset (such closures are NOT auto-invalidated — they
    // pre-date the tracking and the caller is responsible for
    // setting this on construction). New code paths that bridge
    // from the IR executor or the parser should set this to the
    // current bridge_epoch() at construction time.
    std::uint64_t bridge_epoch = 0;
};

// Legacy alias — kept for backward compatibility during the
// P2 transition (Issue #127). New code should prefer
// `aura::diag::Result<types::EvalValue>`. Both names refer
// to the same type: `std::expected<types::EvalValue, Diagnostic>`.
export using EvalResult = aura::diag::Result<types::EvalValue>;

export class Evaluator {
public:
    Evaluator();
    // Issue #67: destructor walks modules_ and runs each env's
    // destructor to free their std::vector bindings_ heap allocations.
    // Without this, arena-allocated Envs leak at process exit (the
    // arena's bump-allocator doesn't run destructors).
    ~Evaluator();
    void set_arena(ast::ASTArena* a) { arena_ = a; }
    void set_temp_arena(ast::ASTArena* a) { temp_arena_ = a; }
    // Hot-swap callback (Issue #97 Action 1). Set by CompilerService
    // to enable the (hot-swap:fn "name" "new-source") primitive.
    // Returns true on success.
    using HotSwapFn = std::function<bool(const std::string& name,
                                        const std::string& new_source)>;
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

    // Module loaded callback: called after a module file is successfully loaded.
    using ModuleLoadedFn = std::function<void(const std::string& source, const std::string& path)>;

    void set_module_loaded_callback(ModuleLoadedFn cb) { module_loaded_cb_ = std::move(cb); }
    void set_type_registry(void* reg) { type_registry_ = reg; }
    void set_compiler_service(void* svc) { compiler_service_ = svc; }
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
    [[nodiscard]] std::uint64_t current_bridge_epoch() const noexcept {
        if (bridge_epoch_fn_ && compiler_service_) {
            return bridge_epoch_fn_(compiler_service_);
        }
        return 0;
    }
    void install_bridge_epoch_fn(BridgeEpochFn fn) noexcept {
        bridge_epoch_fn_ = fn;
    }
    // Issue #223: returns true if a closure's captured bridge_epoch
    // is stale relative to the current epoch. bridge_epoch == 0
    // means "legacy / not tracked" and is treated as trustworthy
    // (the closure pre-dates the tracking; caller manages its own
    // lifetime). Non-zero values are validated strictly.
    static bool is_bridge_stale(std::uint64_t bridge_epoch, std::uint64_t current_epoch) noexcept {
        if (bridge_epoch == 0) return false;  // legacy / unset: trust the closure
        return bridge_epoch != current_epoch;
    }
    void set_session_id(const std::string& id) { session_id_ = id; }
    // Phase 2: EDSL IR cache V2 hooks (set by CompilerService on init)
    void set_mark_define_dirty_fn(std::function<void(const std::string&)> fn) {
        mark_define_dirty_fn_ = std::move(fn);
    }
    void set_mark_all_defines_dirty_fn(std::function<void()> fn) {
        mark_all_defines_dirty_fn_ = std::move(fn);
    }
    // After (set-code ...) parses a new workspace, walk its top-level defines
    // and pre-populate the v2 cache. Source-hash based, so unchanged defines
    // hit the cache and skip the lowering work.
    std::function<void()> pre_cache_workspace_defines_fn_ = nullptr;
    void set_pre_cache_workspace_defines_fn(std::function<void()> fn) {
        pre_cache_workspace_defines_fn_ = std::move(fn);
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
    void set_try_jit_fn(std::function<TryJitFn> fn) {
        try_jit_fn_ = std::move(fn);
    }
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
    void set_get_jit_unhandled_count_fn(
        std::function<GetJitUnhandledCountFn> fn) {
        get_jit_unhandled_count_fn_ = std::move(fn);
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
    void set_get_dirty_block_count_fn(
        std::function<GetDirtyBlockCountFn> fn) {
        get_dirty_block_count_fn_ = std::move(fn);
    }
    using GetFuncDirtyBlockCountFn = std::uint64_t(const char*, std::size_t);
    std::function<GetFuncDirtyBlockCountFn> get_func_dirty_block_count_fn_ = nullptr;
    void set_get_func_dirty_block_count_fn(
        std::function<GetFuncDirtyBlockCountFn> fn) {
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

    // Clear the checkpoint (call after successful mutation commit).
    void commit_panic_checkpoint() { panic_safe_source_.clear(); }

    // Check if a safe checkpoint exists.
    bool has_panic_checkpoint() const { return !panic_safe_source_.empty(); }

    // Get the safe checkpoint source (for introspection).
    const std::string& panic_safe_source() const { return panic_safe_source_; }

    // Set/get a shared workspace tree (for cross-session workspace sharing in serve mode).
    void set_workspace_tree(void* wt) { workspace_tree_ = wt; }
    // Set the AST/pool that source-reading primitives (current-source,
    // workspace:conflicts-with, etc.) operate on. Called by CompilerService
    // at the start of each eval so primitives see the current script's AST.
    // Distinct from set_workspace_flat: workspace_flat_ is the persistent
    // EDSL workspace (set via (set-code ...)), this is the per-eval
    // source-being-evaluated.
    void set_workspace_flat(ast::FlatAST* f) {
        workspace_flat_ = f;
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
    }
    void set_workspace_pool(ast::StringPool* p) { workspace_pool_ = p; }
    ast::FlatAST* workspace_flat() const { return workspace_flat_; }
    ast::StringPool* workspace_pool() const { return workspace_pool_; }

    // Set the AST/pool that source-reading primitives (current-source) read
    // by default. The "per-eval current source" pointer. Set by
    // CompilerService::eval / eval_ir / exec_jit right after parsing the
    // script, before any user code runs. See docs/design/dual-workspace-incremental-ir.md
    // for the dual-workspace rationale.
    void set_current_flat(ast::FlatAST* f) { current_flat_ = f; }
    void set_current_pool(ast::StringPool* p) { current_pool_ = p; }
    ast::FlatAST* current_flat() const { return current_flat_; }
    ast::StringPool* current_pool() const { return current_pool_; }

    // Issue #145 follow-up / Phase 2.5.0 prep: canonical pool accessor.
    // For now, the canonical pool is the workspace pool — it is the
    // pool where almost all `intern()` calls already route (39 sites
    // in evaluator_impl.cpp, vs. ~5 in pat_pool / tmp_pool / local
    // scratch pools). The pool unification refactor (Phase 2.5.0 in
    // docs/design/cpp26_guide.md §2.7.7) will:
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
    static bool trigger_lazy_cow(void* wt);

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
    // the call site in evaluator_impl.cpp. This is the same
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
    // ── GC sweep / compaction (Issue #113 Phase 3) ──────────
    // After the GC collector has marked live objects, this method
    // reclaims the unmarked ones. Called from the GC coordinator's
    // `collect()` during the sweep phase (after the safepoint has
    // stopped all fibers, so no concurrent mutator can run).
    //
    // The opaque `void*` is `aura::serve::GCSweepBuffers*` from
    // gc_coordinator.h (cast at the call site in evaluator_impl.cpp
    // to keep the include surface minimal — same pattern as
    // `flush_gc_roots(void*)` above).
    //
    // The opaque `void*` return is `aura::messaging::GCSweepResultMsg*`
    // (defined in messaging_bridge.h) — a small POD with four
    // size_t fields. The cast at the call site keeps the
    // evaluator's public interface free of messaging_bridge
    // dependencies (messaging_bridge.h is a non-module .h,
    // which can't be imported into a module interface file).
    // The caller (serve_async.cpp) extracts the fields.
    void* compact_sweep(void* sweep_buffers);



    void set_messaging_callbacks(
        std::function<bool(const std::string&, const std::string&)>* send_fn,
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
    EnvId alloc_env_frame(EnvId parent_id = NULL_ENV_ID,
                          const Primitives* primitives = nullptr);
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
    // P0: frames carry pure bindings + parent_id only. Support
    // pointers (cells_/pool_/primitives_) are wired by the
    // caller (apply_closure etc.) after materialization.
    // In either case, primitives_/cells_/pool_ are wired from
    // the active Evaluator + Closure::pool — these are the
    // runtime support pointers the body needs to see, not part
    // of the captured scope itself.
    Env materialize_call_env(const Closure& cl);
    // Look up an EnvFrame by id. UB if id is invalid.
    const EnvFrame& env_frame(EnvId id) const
        pre (id != NULL_ENV_ID)
    {
        return env_frames_[id];
    }
    EnvFrame& env_frame_mut(EnvId id)
        pre (id != NULL_ENV_ID)
    {
        return env_frames_[id];
    }
    // Validity check (test-only helper; cheap).
    [[nodiscard]] bool is_valid_env_id(EnvId id) const {
        return id != NULL_ENV_ID && id < env_frames_.size();
    }
    // Acquire a shared_lock on the env_frames_ deque before
    // calling env_frame() / env_frame_mut() / iterating. This
    // prevents the main thread's alloc_env_frame from
    // reallocating the deque's map array (which would free the
    // pointer a reader is holding). See env_frames_mtx_ below
    // for the full rationale.
    [[nodiscard]] std::shared_mutex& env_frames_lock() const {
        return env_frames_mtx_;
    }
    // Number of live frames.
    [[nodiscard]] std::size_t env_frames_size() const {
        return env_frames_.size();
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
    void walk_env_frame_roots(
        std::vector<std::int64_t>& pair_roots_out,
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
    [[nodiscard]] std::int64_t resolve_pair(
        std::uint64_t old_id) const noexcept {
        if (pair_remap_.empty()) return static_cast<std::int64_t>(old_id);
        if (old_id >= pair_remap_.size()) return -1;
        return pair_remap_[old_id];
    }
    [[nodiscard]] std::size_t pair_remap_size() const noexcept {
        return pair_remap_.size();
    }
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
    [[nodiscard]] std::int64_t compact_pairs(
        const std::vector<bool>& live_mask);
    void clear_pair_remap() noexcept { pair_remap_.clear(); }
    // Walk the parent chain starting from `start`, calling
    // `f(EnvId, const EnvFrame&)` for each frame including
    // `start`. Stops when `f` returns false (early exit) or the
    // chain ends (parent_id == NULL_ENV_ID). Pure index walk —
    // no pointer chase, no cache-unfriendly hop.
    template<typename F>
    void walk_env_frames(EnvId start, F&& f) const
        pre (start != NULL_ENV_ID)
    {
        EnvId cur = start;
        while (cur != NULL_ENV_ID) {
            const EnvFrame& fr = env_frames_[cur];
            if (!std::forward<F>(f)(cur, fr)) return;
            cur = fr.parent_id;
        }
    }
    // Introspection: number of frames in the parent chain
    // starting at `start`. Useful for GC profiling and tests.
    [[nodiscard]] std::size_t env_depth(EnvId start) const
        pre (start != NULL_ENV_ID)
    {
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
    std::optional<types::EvalValue> lookup_by_symid_chain(
        EnvId start, aura::ast::SymId s) const
        pre (start != NULL_ENV_ID);
    // Bulk reset (testing + GC integration). Clears env_frames_
    // but does NOT free the modules_ Env* array (those live in
    // module arenas, lifetime managed separately).
    void reset_env_frames() { env_frames_.clear(); }

private:
    ClosureId next_id() { return next_id_++; }
    [[nodiscard]] std::size_t alloc_cell(const types::EvalValue& v) {
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
    // (apply_closure and expand_macro removed — use eval_flat directly)
    [[nodiscard]] EvalValue ast_to_data(const aura::ast::FlatAST& flat,
                                        const aura::ast::StringPool& pool, aura::ast::NodeId nid);
    [[nodiscard]] ast::NodeId data_to_flat(const types::EvalValue& data, aura::ast::FlatAST& flat,
                                           aura::ast::StringPool& pool, int depth = 0);
    [[nodiscard]] EvalResult eval_data_as_code(const types::EvalValue& data, const Env& env,
                                               aura::ast::FlatAST* flat = nullptr,
                                               aura::ast::StringPool* pool = nullptr);
    Env* copy_env(const Env& env, ast::ASTArena* target = nullptr)
        pre (target != nullptr);  // arena_ is private; impl also asserts via contract_assert
    void init_pair_primitives();
    void build_primitive_slots();
    // Load a module file, return module object (or void on failure)
    types::EvalValue load_module_file(const std::string& path);
    // Resolve a module path (supports AURA_PATH, .aura extension)
    std::string resolve_module_path(const std::string& path) const;

    // Centralized tagged error pair builder ("error" . ("kind" . "message")).
    // Replaces the ~14 duplicated local `auto merr = [this](...)` lambdas
    // (see docs/developer/evaluator.md §3.2). Body implemented in evaluator_impl.cpp.
    // Added in refactor Step 0.1 (pure addition, no call-site changes yet).
    [[nodiscard]] EvalValue make_merr(const std::string& k, const std::string& m);

    Env top_;
    Primitives primitives_;
    ast::ASTArena* arena_ = nullptr;
    ast::ASTArena* temp_arena_ = nullptr;
    // Owned multi-arena manager. Created in ctor so load_module_file can
    // hand each module its own arena without depending on a caller setting
    // it up. Lives for the Evaluator's whole lifetime.
    std::unique_ptr<ast::ArenaGroup> arena_group_;
    ast::FlatAST* mutate_target_flat_ = nullptr;     // for mutate:* primitives (set via set_flat_pool or eval_flat)
    ast::StringPool* mutate_target_pool_ = nullptr;
    ast::FlatAST* workspace_flat_ = nullptr;         // EDSL persistent workspace (set via (set-code ...))
    ast::StringPool* workspace_pool_ = nullptr;
    ast::FlatAST* current_flat_ = nullptr;           // per-eval source-being-evaluated (set by CompilerService eval paths)
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
    // reusing it across calls is the optimization. The
    // build cost is O(N) — a single pass over the
    // workspace, which is faster than the per-call
    // full-walk that the matcher would otherwise do.
    mutable std::unordered_map<
        std::uint64_t, std::vector<aura::ast::NodeId>>
        tag_arity_index_;
    // The workspace pointer the index was built for.
    // When this changes, the index must be rebuilt.
    mutable const ast::FlatAST* tag_arity_index_workspace_ = nullptr;
    // Build (or rebuild) the index for the current
    // workspace. Called by query:pattern (and other
    // future matchers) before walking.
    void build_tag_arity_index() const;
    // Drop the index (called on workspace changes).
    void invalidate_tag_arity_index() {
        tag_arity_index_.clear();
        tag_arity_index_workspace_ = nullptr;
    }
    void* type_registry_ = nullptr; // points to aura::core::TypeRegistry
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
    mutable std::shared_mutex closures_mtx_;
    ClosureBridgeFn closure_bridge_;
    ModuleLoadedFn module_loaded_cb_;
    std::unordered_map<std::string, MacroDef> macros_;
    std::vector<Env*> modules_; // module objects (arena-allocated, indexed by ModuleRef.index)
    std::unordered_map<std::string, std::uint64_t> module_cache_; // path → index
    std::unordered_set<std::string> loading_stack_;               // circular dep detection
    std::vector<std::string> module_names_;                       // display names for modules
    std::unordered_map<std::string, ast::ASTArena*> module_arena_ptrs_; // path → owning arena (for gc_module)
    // Issue #145 Phase 2.4 — runtime arena for high-churn
    // heap vectors. monotonic_buffer_resource bump-allocates
    // chunks; deallocate() is a no-op (monotonic semantics).
    // Sized to fit typical eval sessions (~1M cells/pairs +
    // ~10k strings) without falling back to upstream
    // (new_delete_resource). The buffer is freed wholesale at
    // Evaluator destruction (after the pmr vectors are
    // cleared, which runs ~string on each stored string).
    //
    // Declared BEFORE cells_/pairs_/string_heap_ so the
    // pmr::vector members can take its address in their
    // initializer (member init order matches declaration
    // order).
    std::pmr::monotonic_buffer_resource runtime_resource_;
    std::pmr::vector<types::EvalValue> cells_{&runtime_resource_};
    std::pmr::vector<Pair> pairs_{&runtime_resource_};
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
    mutable std::shared_mutex env_frames_mtx_;
    // Issue #206: pair remap table. Rebuilt by compact_pairs().
    // pair_remap_[old_idx] = new_idx (live, moved) or -1 (freed).
    // Empty means no compact has happened yet (identity mapping).
    std::vector<std::int64_t> pair_remap_;
    std::vector<types::EvalValue> error_values_; // error cause values (indexed by ErrorRef)
    std::vector<void*> opaque_heap_;             // opaque pointers (indexed by OpaqueRef)
    // Issue #131: FFI state moved to FFIRuntime instance
    // (formerly file-scope statics in evaluator_impl.cpp).
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
        std::string body; // strategy body as S-expression string
        int max_attempts = 3;        // tunable: 1..20
        double temperature = 0.3;    // tunable: 0.0..1.0
        std::string sys_prompt_template; // tunable: free-form
        int evolution = 0;           // generation counter
        std::string parent;         // parent strategy name
    };
    std::vector<StrategyDef> strategies_;
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
    std::uint64_t next_record_id_ = 1;
    static constexpr std::size_t MAX_HISTORY_SIZE = 1000;
    // ── Coverage counters (fuzz Phase 3) ──────────────────────
    // 0=parser, 1=typecheck, 2=eval, 3=jit, 4=macro, 5=edsl-set-code,
    // 6=edsl-query, 7=edsl-mutate, 8=ffi, 9-15=reserved
    std::array<std::uint64_t, 16> coverage_counters_ = {};
    // ── Workspace Tree (P13) ───────────────────────────────────
    void* workspace_tree_ = nullptr;  // WorkspaceTree*
    bool workspace_read_only_ = false;  // quick lock flag for P6 mutations
    // ── CompilerService pointer (for messaging) ─────────────────
    void* compiler_service_ = nullptr;  // CompilerService*
    // Issue #223: function pointer that returns the service's
    // current bridge epoch. Set by CompilerService on
    // set_compiler_service() so Evaluator can query the epoch
    // without a circular include of service.ixx.
    BridgeEpochFn bridge_epoch_fn_ = nullptr;
    // Function pointer callbacks (set by CompilerService to avoid circular deps)
    std::function<bool(const std::string&, const std::string&)>* msg_send_fn_ = nullptr;
    std::function<std::optional<std::string>(int)>* msg_recv_fn_ = nullptr;
    std::function<std::string()>* msg_id_fn_ = nullptr;
    std::string session_id_;  // from CompilerService (for my-id)


    // ── Snapshot storage (ast:snapshot / ast:restore) ───────────
    std::vector<std::string> snapshot_sources_;  // source code per snapshot (for ast:diff + source-fallback restore)
    std::vector<std::string> snapshot_names_;    // optional names

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
        bool has_flat = false;  // true if both flat + pool are valid
    };
    std::vector<FlatSnapshot> snapshot_flats_;

    // Hot-swap callback storage (Issue #97 Action 1)
    HotSwapFn hot_swap_fn_;

    // ── Auto-evolve state (Issue #97 Action 2) ──────────────
    // Background loop state for (auto-evolve-loop ...).
    // The two Aura callbacks (detect-fn, fix-fn) are stored as closure IDs
    // and invoked via apply_closure.
    bool auto_evolve_running_ = false;
    double auto_evolve_interval_ = 1.0;  // seconds between cycles
    std::uint64_t auto_evolve_detect_closure_ = 0;  // 0 = unset
    std::uint64_t auto_evolve_fix_closure_ = 0;
    std::uint64_t auto_evolve_cycle_count_ = 0;
    std::uint64_t auto_evolve_total_fixed_ = 0;

    // ── Panic auto-rollback (Issue #39) ─────────────────────────
    bool panic_auto_rollback_ = false;
    std::string panic_safe_source_;  // last known good source code

    // ── EDSL set-code error propagation ──────────────────────────
    // Stores (kind, message) for structured diagnostic return
    std::string last_set_code_error_kind_;
    std::string last_set_code_error_msg_;

    // Last mutate typecheck error (empty = no error). Set by auto-typecheck
    // after mutate:rebind etc. Cleared on next successful mutate.
    std::string last_mutate_error_;

    // ── Incremental eval cache ───────────────────────────────────
    // Caches the last eval-current result. Cleared when workspace dirty flags
    // are set (which happens on any mutation). This lets eval-current skip
    // full re-evaluation when nothing has changed.
    std::optional<types::EvalValue> last_eval_current_result_;

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
    // Issue #189: total mutations counter (for observability).
    // Bumped alongside defuse_version_ so dashboards can see
    // "how many mutations has this evaluator processed" without
    // having to interpret the version increment. Read via
    // `total_mutations()`. Relaxed-ordering for stats; not
    // used for control flow.
    std::atomic<std::uint64_t> total_mutations_{0};
    // Issue #192: atomic-batch observability counters. Bumped
    // by the (mutate:atomic-batch) primitive. Relaxed-ordering
    // (stats-only). Separate from total_mutations_ so dashboards
    // can distinguish "N mutations applied" from "M atomic
    // batches completed (covering K of those N)".
    std::atomic<std::uint64_t> atomic_batch_count_{0};
    std::atomic<std::uint64_t> atomic_batch_ops_total_{0};
    std::atomic<std::uint64_t> atomic_batch_rollbacks_{0};
    // Issue #164: per-join defuse_version_ snapshot. Set at the
    // start of fiber:join's wait, re-checked at wakeup to detect
    // mutations that happened DURING the join (the "transient
    // inconsistency" the issue calls out). 0 means "no active
    // wait" (fast-path or never-wait joins). Per-fiber; not atomic
    // (owning fiber reads/writes; the wakeup check happens on the
    // same fiber's worker thread). Issue #184 Cycle 3 may promote
    // this to atomic if cross-fiber resume becomes a thing.
    std::uint64_t defuse_version_at_wait_ = 0;
    // (#10) Track mutation-affected symbols for targeted index rebuild
    // Mutation primitives push affected sym names here; ensure_defuse
    // uses them to avoid full rebuild when only a few symbols changed.
    std::unordered_set<std::string> defuse_affected_syms_;
    // (#10) Number of times the def-use index has been rebuilt (for stats)
    std::uint64_t defuse_rebuild_count_ = 0;

    // ── 依赖图查询回调 ─────────────────────────────────────────
    // 在 mutation 原语中查询调用者节点，绕开 DefUseIndex 前向声明问题。
    // 在 init_pair_primitives 末尾（DefUseIndex 定义完成后）注册。
    // 签名: (defuse_index, sym_id) → [caller node IDs]
    // 用 std::function 而非函数指针，避免不完整类型问题。
    std::function<std::vector<aura::ast::NodeId>(void*, aura::ast::SymId)>
        dep_caller_fn_ = nullptr;

    // ── DefUseIndex per-sym version touch callback (#107 part 5) ───
    // 在 mutation 原语中调用，标记某个 sym 在 DefUseIndex 中为 stale。
    // 注册位置同 dep_caller_fn_，绕开 DefUseIndex 前向声明问题。
    // 签名: (defuse_index, sym_id) → void
    // 当 defuse_index_ 为 null 时回调内部应 no-op。
    std::function<void(void*, aura::ast::SymId)>
        defuse_touch_fn_ = nullptr;

    // ── EDSL IR cache V2 (Phase 2) hooks ─────────────────────────────
    // Function pointers set by CompilerService on init. Avoids
    // evaluator_impl.cpp needing to import CompilerService (circular).
    // mark_define_dirty_fn_(name)         → mark a single define's IR dirty
    // mark_all_defines_dirty_fn_()       → mark all cached defines dirty
    std::function<void(const std::string&)> mark_define_dirty_fn_ = nullptr;
    std::function<void()> mark_all_defines_dirty_fn_ = nullptr;

    // ── 模块类型签名（#8 跨模块类型检查） ──────────────────────
    // (declare-type "name" "param-types" "ret-type") 存储的签名，
    // 在 typecheck-current 时注入到类型环境中。
    // 格式: type_str = "param1 param2|rettype" | 分隔
    struct DeclaredType {
        std::string type_str;
        std::string module_file;  // 来源模块文件（用于跨模块错误定位）
        bool resolved = false;
    };
    std::unordered_map<std::string, DeclaredType> declared_type_sigs_;

    // ── Functor 泛型模块模板 ────────────────────────────────────
    struct ModuleTemplate {
        std::string body_source;                       // body source code (re-parsed at instantiation)
        std::vector<std::string> type_param_names;     // type parameter names (e.g., ["T", "K"])
        std::vector<std::string> cap_param_names;      // capability parameter names (e.g., ["cap"])
        std::vector<std::string> cap_require;           // required capabilities (e.g., ["FileRead", "FileWrite"])
    };
    std::unordered_map<std::string, ModuleTemplate> module_templates_;

    // ── Functor 实例化缓存 ──────────────────────────────────────
    // key = "template_name|arg1|arg2|..."
    // value = 实例化后的 env（指针通过 module 索引引用）
    std::unordered_map<std::string, std::uint64_t> functor_instance_cache_;

    // ── Timeline for intend (E2, backward compat) ───────────────
    std::vector<std::string> timeline_; //
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
    std::size_t eval_depth_ = 0; // recursion counter for friendly stack overflow
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
    std::shared_mutex workspace_mtx_;
public:
    // Issue #211: test accessors for the (tag, arity) index.
    [[nodiscard]] std::size_t tag_arity_index_size() const noexcept {
        return tag_arity_index_.size();
    }
    [[nodiscard]] const ast::FlatAST* tag_arity_index_workspace() const noexcept {
        return tag_arity_index_workspace_;
    }
    // Force a build of the index (for tests that need to
    // verify the index without going through query:pattern).
    // The index is normally lazy-built on first use, so
    // this is only needed for direct unit tests of the
    // index itself.
    void force_build_tag_arity_index() const { build_tag_arity_index(); }
    // Test accessors for setting the workspace directly
    // (bypassing the Aura primitive pipeline, which is
    // tested separately).
    void set_workspace_flat_for_test(ast::FlatAST* f) {
        set_workspace_flat(f);
    }
    void invalidate_tag_arity_index_for_test() {
        invalidate_tag_arity_index();
    }
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
        std::uint64_t version;              // defuse_version_ at boundary entry
        std::size_t mutation_log_size = 0;  // FlatAST::mutation_log_.size() at entry
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
        // std::pmr::vector matches the allocator of FlatAST::children_
        // (so the vector copy doesn't require allocator conversion).
        std::pmr::vector<aura::ast::PersistentChildVector<aura::ast::NodeId>>
            children_snapshot;
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
    static thread_local std::vector<MutationCheckpoint>
        g_main_thread_stack;

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
    void enter_mutation_boundary() {
        std::unique_lock<std::shared_mutex> lock(workspace_mtx_);
        // Issue #213 Cycle 1: capture the mutation log size at
        // entry so the exit(false) rollback knows how far to undo.
        // The size is the log size *at the moment of the boundary
        // entry* — any mutations recorded after this point (during
        // the boundary body) will be appended to positions >=
        // this size, and `rollback_to_size` will undo them.
        std::size_t log_size = workspace_flat_ ? workspace_flat_->all_mutations().size() : 0;
        // Issue #221: capture the per-node children_ vector. The
        // PCV's COW semantics make this a cheap copy (each PCV
        // is a shared_ptr to immutable storage; the snapshot
        // holds shared_ptrs that keep the pre-mutation PCs alive).
        // std::pmr::vector matches the allocator of FlatAST::children_
        // (so the vector copy doesn't require allocator conversion).
        std::pmr::vector<aura::ast::PersistentChildVector<aura::ast::NodeId>>
            children_snapshot;
        if (workspace_flat_) {
            children_snapshot = workspace_flat_->snapshot_children();
        }
        active_mutation_stack().push_back(
            {defuse_version_.load(std::memory_order_acquire), log_size,
             std::move(children_snapshot)});
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
        if (stack.empty()) return {0, 0};
        auto cp = stack.back();
        stack.pop_back();
        if (!success && workspace_flat_) {
            // Roll back the mutations that were appended between
            // enter and exit. The log size captured at entry
            // tells us how far to undo.
            std::size_t rolled = workspace_flat_->rollback_to_size(cp.mutation_log_size);
            (void)rolled;  // count is for debug; not exposed via the API
            // Issue #221: restore the per-node children_ from the
            // pre-mutation snapshot. The checkpoint's children_snapshot
            // holds shared_ptrs to the pre-mutation PCs (PCV COW),
            // so the restoration is O(1) per node.
            workspace_flat_->restore_children(std::move(cp.children_snapshot));
            // Invalidate the def-use index — the workspace state
            // is now different from what the index reflects.
            defuse_index_ = nullptr;
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
        return cp;
    }
    // Get the current checkpoint stack depth (for testing /
    // observability). Returns 0 if the stack is empty.
    static std::size_t mutation_boundary_depth() {
        return active_mutation_stack_static().size();
    }

    // Static version of active_mutation_stack() for observability
    // accessors that don't have an Evaluator instance handy.
    static std::vector<MutationCheckpoint>& active_mutation_stack_static();

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
    class MutationBoundaryGuard {
    public:
        MutationBoundaryGuard(Evaluator& ev, bool* success_flag) noexcept
            : ev_(&ev), flag_(success_flag) {
            if (flag_) *flag_ = true;  // optimistic default
            ev_->enter_mutation_boundary();
        }
        ~MutationBoundaryGuard() {
            if (!ev_) return;
            bool success = flag_ ? *flag_ : true;
            ev_->exit_mutation_boundary(success);
        }
        MutationBoundaryGuard(const MutationBoundaryGuard&) = delete;
        MutationBoundaryGuard& operator=(const MutationBoundaryGuard&) = delete;
        MutationBoundaryGuard(MutationBoundaryGuard&& o) noexcept
            : ev_(o.ev_), flag_(o.flag_) { o.ev_ = nullptr; o.flag_ = nullptr; }
        MutationBoundaryGuard& operator=(MutationBoundaryGuard&& o) noexcept {
            if (this != &o) {
                // Exit current if any, then steal.
                if (ev_) ev_->exit_mutation_boundary(flag_ ? *flag_ : true);
                ev_ = o.ev_; flag_ = o.flag_;
                o.ev_ = nullptr; o.flag_ = nullptr;
            }
            return *this;
        }
    private:
        Evaluator* ev_;
        bool* flag_;
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
    void lock_workspace_shared()   { workspace_mtx_.lock_shared(); }
    void unlock_workspace_shared() { workspace_mtx_.unlock_shared(); }
    void lock_workspace_unique()   { workspace_mtx_.lock(); }
    void unlock_workspace_unique() { workspace_mtx_.unlock(); }

    // Issue #157 Phase 1: defuse_version_ accessor for the JIT
    // version check (aura_get_defuse_version in aura_jit_runtime.cpp).
    // Phase 1b (deferred to follow-up) will use this in the L2
    // SHAPE_PAIR paths to do a version check at entry; on mismatch,
    // deopt to the slow path. For Phase 1 we just expose the accessor.
    //
    // Issue #184: the accessor uses acquire ordering so the JIT
    // runtime's version check synchronizes with any release-store
    // performed by enter_mutation_boundary() on another thread.
    std::uint64_t get_defuse_version() const {
        return defuse_version_.load(std::memory_order_acquire);
    }

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
    // unit" errors. The actual implementation is in evaluator_impl.cpp
    // which DOES include messaging_bridge.h.
    void yield_mutation_boundary();

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
    std::size_t post_mutation_macro_reexpand(
        aura::ast::FlatAST& flat,
        aura::ast::StringPool& pool,
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
};


// Pair-aware value formatting (recursively prints lists)
export inline std::string format_value(const types::EvalValue& v,
                                       std::span<const std::string> heap,
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
export aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                          aura::ast::NodeId root, int max_passes = 32);

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
    [[nodiscard]] std::optional<types::EvalValue> lookup(
        const std::string& name) const;

    // Lookup a SymId in the SymId-keyed bindings (Issue #145
    // fast path). Walks to parent_ if not found locally.
    [[nodiscard]] std::optional<types::EvalValue> lookup_by_symid(
        aura::ast::SymId s) const;

    // Issue #145 follow-up / Phase 2.5.0: SymId-first lookup
    // helper that takes a name string + pool. Same role as
    // Env::lookup_by_intern but for EnvView (which has no
    // pool_ field — the pool is always passed in). Used by
    // the parent-walk migration in Phase 2.5.0 commit 8.
    std::optional<types::EvalValue>
    lookup_by_intern(const std::string& n,
                     const aura::ast::StringPool* pool) const
        pre (!n.empty());

    // Number of local bindings (excludes parent).
    [[nodiscard]] std::size_t size() const {
        return string_bindings.size();
    }
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

} // namespace aura::compiler
