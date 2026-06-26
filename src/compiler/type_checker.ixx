module;
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module aura.compiler.type_checker;

import std;
import aura.core;
import aura.core.type;
import aura.diag;
import aura.compiler.coercion_map;

namespace aura::compiler {

// ── Type Environment ─────────────────────────────────────
export class TypeEnv {
    aura::core::TypeRegistry& reg_;
    struct Binding {
        aura::core::TypeId type;
        bool is_poly = false;
        std::vector<aura::core::TypeId> type_args;
    };
    std::vector<std::unordered_map<std::string, Binding>> scopes_;

public:
    explicit TypeEnv(aura::core::TypeRegistry& reg);
    void push_scope();
    void pop_scope();
    void bind(std::string name, aura::core::TypeId type);
    aura::core::TypeId lookup(const std::string& name);
    bool is_bound(const std::string& name) const;
    void collect_names(std::vector<std::string>& out) const;
};

// ── Constraint System ────────────────────────────────────
export struct Constraint {
    enum Kind { EQUAL, CONSISTENT };
    Kind kind;
    aura::core::TypeId lhs, rhs;
};

// Issue #118: explicit solve result. The previous `bool solve()`
// returned `true` even when the worklist wasn't empty after the
// pass limit was reached (a soundness hole for AI-generated
// code that produces partial / under-constrained programs).
// The new return value distinguishes the three outcomes:
//
//   SOLVED   — worklist empty; all constraints resolved.
//   CONFLICT — a constraint failed unification; the inference
//              result is unsound and the caller should report
//              a TypeError.
//   TIMEOUT  — pass limit hit with non-empty worklist; some
//              constraints remain unresolved. The caller gets
//              the unresolved list via `unresolved_out` and can
//              report a Warning (permissive mode) or TypeError
//              (strict mode) with the constraint list attached.
export enum class SolveResult : std::uint8_t {
    SOLVED = 0,
    CONFLICT = 1,
    TIMEOUT = 2,
};

export class ConstraintSystem {
    aura::core::TypeRegistry& reg_;
    std::vector<Constraint> constraints_;
    std::vector<std::int64_t> parent_;        // Union-Find parent (self=root, -1=uninitialized)
    std::vector<std::uint32_t> rank_;         // Union-Find rank (for union-by-rank)
    std::vector<aura::core::TypeId> binding_; // binding[rep] = concrete type for var rep
    uint64_t fresh_counter_ = 0;
    uint64_t first_free_var_ = 0; // first var index that belongs to this CS

    // Issue #148 prep: dirty-constraint set for incremental
    // solving. add_delta / remove_delta / solve_delta (Phase 2
    // of the issue) will read+write this set. The set stores
    // constraint indices into `constraints_` — when a delta is
    // added, only the new constraints are touched; the
    // existing constraints are reused.
    //
    // Why std::vector<bool> instead of std::set: cache-locality
    // matters for solve loops that scan all constraints. The
    // vector<bool> is checked once per solve iteration; a set
    // would have pointer-chasing on every check.
    std::vector<bool> constraint_dirty_;
    std::size_t dirty_count_ = 0; // O(1) "is anything dirty?"
public:
    explicit ConstraintSystem(aura::core::TypeRegistry& reg);
    void add(Constraint c);
    // Issue #148: incremental constraint management. add_delta
    // appends a new constraint AND marks it dirty. solve_delta
    // re-solves only the dirty subset (constraints added since
    // the last solve). No dependency tracking yet — unification
    // can still touch other constraints' variables, but the
    // resulting unifications are persistent in the Union-Find
    // so downstream consumers see the correct bindings. For
    // larger delta sets (mutation touching N type ids), the
    // AC's ≥60% reduction comes from skipping the (M-N) clean
    // constraints in the worklist scan.
    //
    // Edge case: a delta that introduces a NEW unification
    // that conflicts with an existing clean constraint's
    // bindings will NOT be caught by solve_delta alone — the
    // clean constraint is not re-checked. Callers that need
    // full correctness across deltas should follow up with a
    // solve() pass. The benchmark in Phase 6 will measure
    // when this matters; until then, solve_delta is a best-
    // effort optimization.
    void add_delta(Constraint c);
    // Issue #118: returns SolveResult. If the result is TIMEOUT,
    // `unresolved_out` (if non-null) is filled with the constraints
    // that remained on the worklist when the pass limit was
    // reached. The caller can then attach the list to a
    // diagnostic so AI agents can see exactly which constraints
    // are still under-constrained.
    SolveResult solve(std::vector<Constraint>* unresolved_out = nullptr);
    // Issue #148: incremental solve — iterate only the dirty
    // subset. Returns SolveResult::SOLVED if all dirty
    // constraints unify cleanly, SolveResult::CONFLICT on
    // any unification failure, SolveResult::TIMEOUT if the
    // pass limit is hit before fixpoint.
    SolveResult solve_delta(std::vector<Constraint>* unresolved_out = nullptr);
    // Issue #258: split-out implementation of solve_delta().
    // The public solve_delta() wraps this with a timer that
    // accumulates into CompilerMetrics::delta_solve_time_us.
    // Splitting lets the wrapper use a uniform RAII-style
    // pattern without duplicating the early-return paths.
    SolveResult solve_delta_impl(std::vector<Constraint>* unresolved_out);
    // Issue #258: metrics pointer for solve_delta() timing.
    // Set by CompilerService::typecheck_full() and
    // incremental_infer() via set_metrics() (same pattern as
    // the JIT / IR interpreter). Null by default (unit tests
    // that construct ConstraintSystem directly don't need it).
    void* metrics_ = nullptr;

    void set_metrics(void* m) { metrics_ = m; }
    // O(1) "is the constraint set dirty?". True iff
    // add_delta has been called since the last clear or solve.
    bool is_dirty() const { return dirty_count_ > 0; }
    // O(n) clear of dirty flags without removing constraints.
    // Useful when a full solve() has run and we want to reset
    // the delta tracking without losing constraint state.
    void mark_clean() {
        std::fill(constraint_dirty_.begin(), constraint_dirty_.end(), false);
        dirty_count_ = 0;
    }
    void clear();
    aura::core::TypeId fresh_var();
    // Issue #79: variant that takes a name hint, so the resulting type var
    // shows up in error messages as a meaningful name (e.g. 'a' or 'xs')
    // instead of the default '__t<N>'. The hint may be empty.
    aura::core::TypeId fresh_var_named(std::string_view hint);
    // Union-Find core
    aura::core::TypeId find_var(aura::core::TypeId id);
    bool unify(aura::core::TypeId t1, aura::core::TypeId t2);
    aura::core::TypeId find(aura::core::TypeId id); // normalize via Union-Find
    bool consistent_unify(aura::core::TypeId t1, aura::core::TypeId t2);
    bool consistent_subtype(aura::core::TypeId sub, aura::core::TypeId sup);
    bool occurs_check(aura::core::TypeId var, aura::core::TypeId ty);
    aura::core::TypeId normalize(aura::core::TypeId id);
};

// ── Ownership Environment (M4 Linear) ──────────────────────
export enum class OwnershipState : std::uint8_t {
    Owned,       // 拥有唯一所有权
    Moved,       // 所有权已转移
    Borrowed,    // 被不可变借用中
    MutBorrowed, // 被可变借用中
};

export struct OwnershipNote {
    aura::ast::NodeId node;
    std::string message;
    std::string kind; // "use-after-move" | "double-borrow" | "leaked-linear" | "invalid-state"
    // Issue #260: link post-mutation notes to MutationLog + BlameInfo.
    std::optional<std::uint64_t> source_mutation_id;
    std::optional<aura::diag::BlameInfo> blame;
};

export class OwnershipEnv {
    std::vector<std::unordered_map<std::string, OwnershipState>> scopes_;
    // Tracks which variable bindings have had structural mutations applied
    // and need ownership re-validation on the next validate pass.
    std::unordered_set<std::string> ownership_dirty_;

public:
    explicit OwnershipEnv() { scopes_.emplace_back(); }

    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() {
        if (scopes_.size() > 1)
            scopes_.pop_back();
    }

    void mark(const std::string& name, OwnershipState st) {
        // Always write to the current (innermost) scope.
        // This ensures that when the scope ends (pop_scope), the outer scope's
        // original state is restored — critical for lexical borrow scoping.
        scopes_.back()[name] = st;
    }

    OwnershipState get(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end())
                return f->second;
        }
        return OwnershipState::Owned; // unknown vars assumed Owned
    }

    // Can read the variable (owned or imm-borrowed)
    bool can_use(const std::string& name) const {
        auto st = get(name);
        return st == OwnershipState::Owned || st == OwnershipState::Borrowed;
    }

    // Can move the variable (only if fully owned, no outstanding borrows)
    bool can_move(const std::string& name) const { return get(name) == OwnershipState::Owned; }

    // Can drop the variable (only if fully owned)
    bool can_drop(const std::string& name) const { return get(name) == OwnershipState::Owned; }

    // Can imm-borrow (allowed if owned or already imm-borrowed)
    bool can_borrow(const std::string& name) const {
        auto st = get(name);
        return st == OwnershipState::Owned || st == OwnershipState::Borrowed;
    }

    // Can mut-borrow (only if owned, exclusive access)
    bool can_mut_borrow(const std::string& name) const {
        return get(name) == OwnershipState::Owned;
    }

    std::string state_name(OwnershipState st) const {
        switch (st) {
            case OwnershipState::Owned:
                return "owned";
            case OwnershipState::Moved:
                return "moved";
            case OwnershipState::Borrowed:
                return "borrowed";
            case OwnershipState::MutBorrowed:
                return "mut-borrowed";
        }
        return "unknown";
    }

    // ── Ownership Dirt Tracking ───────────────────────────────
    // After structural mutations, mark affected bindings as ownership-dirty.
    void mark_ownership_dirty(const std::string& name) { ownership_dirty_.insert(name); }
    void mark_ownership_dirty_subtree(const std::vector<std::string>& names) {
        for (auto& n : names)
            ownership_dirty_.insert(n);
    }
    bool is_ownership_dirty(const std::string& name) const {
        return ownership_dirty_.count(name) > 0;
    }
    void clear_ownership_dirty() { ownership_dirty_.clear(); }
    const std::unordered_set<std::string>& ownership_dirty_bindings() const {
        return ownership_dirty_;
    }

    // ── Post-Mutation Ownership Validation ────────────────────
    // Walks the AST within the dirty set, re-simulates ownership flow,
    // and reports any violations. Returns true if all checks pass.
    static bool validate_ownership(const aura::ast::FlatAST& flat,
                                   const aura::ast::StringPool& pool, aura::ast::NodeId root,
                                   const std::unordered_set<std::string>& dirty_bindings,
                                   std::vector<OwnershipNote>& notes_out);

    // Issue #117: full re-simulation mode. Walks the AST to
    // discover ALL linear-typed bindings (not just dirty ones)
    // and validates them as a single pass. Slower than the
    // dirty-only mode, but catches:
    //   - cross-function ownership flows (linear passed as
    //     argument to another function, where the callee
    //     moves it but the caller's dirty set doesn't include
    //     the callee's locals)
    //   - closure-captured linear resources (the closure
    //     body moves the captured value, but the closure
    //     itself isn't in the dirty set)
    //   - global-scope linear bindings (let-bound at top
    //     level, no enclosing scope to mark them as leaked
    //     in the dirty-only path)
    //
    // The function uses the type_id_ field on FlatAST (populated
    // by a prior infer_flat call) to discover linear-typed
    // bindings. The caller is expected to have run infer_flat
    // before calling this.
    static bool validate_ownership_full(const aura::ast::FlatAST& flat,
                                        const aura::ast::StringPool& pool, aura::ast::NodeId root,
                                        std::vector<OwnershipNote>& notes_out);

    // Internal: shared implementation of the post-hoc ownership
    // walk. Public callers should use validate_ownership or
    // validate_ownership_full. Kept separate so the two
    // discovery paths can share the same walk.
    static bool validate_ownership_impl(const aura::ast::FlatAST& flat,
                                        const aura::ast::StringPool& pool, aura::ast::NodeId root,
                                        const std::unordered_set<std::string>& dirty_bindings,
                                        std::vector<OwnershipNote>& notes_out);
};

// Issue #281: moved to the module interface so the
// InferenceEngine's predicate memo can use it as a value
// type. The struct is unchanged from the pre-#281 .cpp
// declaration (var_name, refined_type, is_negation).
export struct OccurrenceInfoFlat {
    std::string var_name;
    aura::core::TypeId refined_type;
    bool is_negation = false;
};
export class InferenceEngine {
    aura::core::TypeRegistry& reg_;
    aura::diag::DiagnosticCollector& diag_;
    ConstraintSystem cs_;
    TypeEnv env_;
    OwnershipEnv ownership_env_;
    aura::diag::SourceLocation cur_loc_; // location of expression being checked

    // Issue #79: strict type-check mode. In strict mode, cross-type
    // coercions (Int ↔ String, Int ↔ Bool, Float ↔ String) are reported
    // as TypeError instead of being silently coerced. Plumbed from
    // TypeChecker::infer_flat.
    bool strict_ = false;

    // Issue #103: permissive (LLM-friendly) mode. When true and
    // strict_ is false, inference errors degrade gracefully — the
    // constraint solver reports a Warning (not TypeError) and the
    // synthesized type falls back to Dynamic instead of aborting.
    // Goal: the LLM's first try at a function passes typecheck
    // (with caveats) rather than being blocked by a strict failure.
    // Strict mode wins: in strict mode, this flag has no effect
    // and errors are still TypeError.
    bool permissive_ = true;

    // ADT constructors are looked up via TypeRegistry::get_adt_constructors()

    // Issue #280: most recent IfExpr's narrowing evidence bitmask.
    // Bit assignments (match the bitmask tests in tests/test_issue_280.cpp):
    //   kNarrowNumber = 1 << 0   (number? / integer?)
    //   kNarrowString = 1 << 1   (string?)
    //   kNarrowBool   = 1 << 2   (boolean?)
    //   kNarrowVoid   = 1 << 3   (null? / void?)
    //   kNarrowPair   = 1 << 4   (pair?)
    //   kNarrowList   = 1 << 5   (list?)
    //   kNarrowFloat  = 1 << 6   (float?)
    //   kNarrowHash   = 1 << 7   (hash?)
    //   kNarrowSymbol = 1 << 8   (symbol?)
    //   kNarrowProc   = 1 << 9   (procedure?)
    //   kNarrowCustom = 1 << 10  (type? "Name")
    std::uint32_t last_if_narrowing_ = 0;
public:
    // Issue #79: set strict mode. Called by TypeChecker::infer_flat
    // before delegating to the engine.
    void set_strict(bool s) { strict_ = s; }

    // Issue #103: set permissive mode. Default is true. Set to
    // false to opt into the old behavior (error on constraint
    // solve failure, even in non-strict mode). Strict mode
    // (set_strict(true)) always wins — strict+permissive = strict.
    void set_permissive(bool p) { permissive_ = p; }

    // Issue #168 Phase 1: set the cache epoch. CompilerService
    // calls this before every infer_flat with the current
    // mutation_epoch_ (#166). The engine uses it to gate the
    // type cache: if the epoch has changed since the last
    // inference, the whole cache is invalidated (re-infer
    // everything). This is the safety net for mutations
    // that don't set is_dirty on the right nodes.
    void set_cache_epoch(std::uint64_t epoch) { cache_epoch_ = epoch; }
    // Issue #258: forward the metrics pointer to the
    // ConstraintSystem so solve_delta timing accumulates
    // into CompilerMetrics::delta_solve_time_us.
    void set_metrics(void* m) { cs_.set_metrics(m); }
    std::uint64_t epoch_invalidations() const { return epoch_invalidations_; }

    // Issue #283 follow-up #5: bidirectional-mode flag. When
    // true (default), check_flat's If branch applies Occurrence
    // Typing narrowing. When false, check_flat falls back to
    // the original uniform check (no narrowing). CompilerService
    // sets this from its own bidirectional_mode_ before each
    // infer_flat call.
    void set_bidirectional_mode(bool b) noexcept { bidirectional_mode_ = b; }
    bool bidirectional_mode() const noexcept { return bidirectional_mode_; }

    // Issue #281: predicate memoization stats accessors. Test/
    // observability can read these to verify the memo is doing
    // useful work (high hit rate = same-condition re-analysis
    // being skipped).
    std::uint64_t predicate_memo_hits() const noexcept { return predicate_memo_hits_; }
    std::uint64_t predicate_memo_misses() const noexcept { return predicate_memo_misses_; }
    std::uint64_t predicate_memo_evictions() const noexcept { return predicate_memo_evictions_; }

    // Issue #280: capture narrowing evidence from the most
    // recent IfExpr synthesize. Bitmask values are defined
    // below (kNarrowString = 1 << 1, kNarrowNumber = 1 << 2,
    // etc.). When no narrowing was applied the value is 0.
    //
    // Lowering queries this in flatten_expr_if's Branch emit
    // so the resulting Branch instruction can carry a hint
    // to DeadCoercionEliminationPass / JIT.
    void set_last_narrowing_evidence(std::uint32_t mask) noexcept {
        last_if_narrowing_ = mask;
    }
    std::uint32_t last_narrowing_evidence() const noexcept {
        return last_if_narrowing_;
    }

public:
    // declared_modules: name → module_path, 用于跨模块错误定位
    std::unordered_map<std::string, std::string> declared_modules_;

    // declared_sigs: name → TypeId, for binding declared type
    // signatures (from inject_type_sigs) to the env. We pass this
    // explicitly instead of relying on the registry's __decl_ name
    // scan, because after TypeId interning multiple names can map
    // to the same TypeId and the last writer wins the name field.
    // (See #77 regression follow-up: the 312-5 test in
    // test_regression.py / test_aura_type_multi_func failed because
    // add/mul dedup'd to one entry, mul's name overwrote add's.)
    std::unordered_map<std::string, aura::core::TypeId> declared_sigs_;

    // Issue #72: per-engine incremental typecheck stats.
    // Accumulated on TypeChecker (via stats()) for visibility.
    struct InnerStats {
        std::uint64_t cache_hits = 0;
        std::uint64_t cache_misses = 0;
        std::uint64_t stale_cache = 0;
        // Issue #412: cache hits rescued by the gen check.
        // Pre-#412 these were counted as stale_cache (the
        // free_vars check rejected them). Post-#412 the
        // generation counter confirms the binding structure
        // didn't change, so the polymorphic TYPE_VAR
        // children are still valid for this query. The
        // gen_saved / (stale_cache + gen_saved) ratio
        // measures the improvement.
        std::uint64_t gen_saved = 0;
        // Issue #411 follow-up #1: per-symbol re-inference
        // path tracking. per_symbol_used_total counts how
        // many mutations took the per-symbol path; the
        // companion visited_total is the total number of
        // nodes visited (use-sites only) across all those
        // invocations. ancestor_* mirror the same for the
        // fallback path. The derived
        // per_symbol_visited_avg =
        // per_symbol_visited_total / max(per_symbol_used,
        // 1) tells the user the average number of nodes
        // re-inferred per per-symbol mutation.
        std::uint64_t per_symbol_used_total = 0;
        std::uint64_t per_symbol_visited_total = 0;
        std::uint64_t ancestor_used_total = 0;
        std::uint64_t ancestor_visited_total = 0;
        // Issue #411 fu1 follow-up #3: per-DefUseIndex
        // re-inference path tracking. per_defuse_index_used
        // counts how many mutations took the O(uses)
        // per-DefUseIndex path (fastest). walk_fallback
        // counts how many times the path fell back to the
        // O(n) walk (tracker present but sym not in it —
        // signals index coverage gaps).
        std::uint64_t per_defuse_index_used_total = 0;
        std::uint64_t per_defuse_index_walk_fallback_total = 0;
    };
    InnerStats stats_;
    InnerStats stats() const { return stats_; }

    // Issue #168 Phase 1: cache epoch gate. CompilerService
    // sets this before every infer_flat call to the current
    // mutation_epoch_ (#166). The engine compares it to
    // last_inference_epoch_ at the start of infer_flat: if
    // they differ, the cache is globally invalidated
    // (coarse — re-infer everything). This catches stale
    // cache results from mutations that didn't set
    // is_dirty correctly (the issue's repro scenario).
    //
    // Tradeoff: invalidates MORE than strictly necessary
    // (whole cache vs per-node). But provably correct for
    // the bug class the issue describes.
    std::uint64_t cache_epoch_ = 0;
    std::uint64_t last_inference_epoch_ = 0;
    // Counter for how many times the epoch gate invalidated
    // the cache (visible via stats for debugging).
    std::uint64_t epoch_invalidations_ = 0;
    // Per-call flag: true at the start of infer_flat if the
    // epoch advanced (forces a re-infer of this call's nodes
    // even if is_dirty is false). Reset by the next call to
    // infer_flat that sees the same epoch.
    bool epoch_invalidated_ = false;
    // Issue #283 follow-up #5: when false, check_flat skips
    // the Occurrence Typing narrowing application in the If
    // branch. Used as a fast-path opt-out for workspaces
    // where bidirectional is too eager / slow.
    bool bidirectional_mode_ = true;

    // Issue #281: per-condition memoization for analyze_predicate_flat.
    // Keyed by cond NodeId + the epoch at which the predicate was
    // analyzed. On lookup, if the stored epoch matches cache_epoch_
    // we return the cached OccurrenceInfoFlat without re-walking
    // the predicate AST. This is the predicate-specific
    // counterpart to the coarse epoch gate above (which
    // invalidates the *whole* type cache on epoch change).
    //
    // When the epoch advances (#168 path), we clear the memo
    // wholesale. We don't try to be smarter than the global
    // dirty mechanism: that's #262's job. We just want to skip
    // redundant work for the common case where the same
    // (string? x) predicate is analyzed N times in a row.
    struct PredicateMemoEntry {
        aura::ast::NodeId cond_id{};
        std::uint64_t epoch = 0;
        std::optional<OccurrenceInfoFlat> result; // nullopt = no narrowing found
    };
    std::unordered_map<aura::ast::NodeId, PredicateMemoEntry> predicate_memo_;
    std::uint64_t predicate_memo_hits_ = 0;
    std::uint64_t predicate_memo_misses_ = 0;
    std::uint64_t predicate_memo_evictions_ = 0; // cleared on epoch change
    // Issue #281 follow-up #5: bound the predicate memo. The
    // memo is keyed by cond NodeId which is stable across
    // mutations within an epoch. Without a cap, a workspace
    // with many distinct (string? x) predicates across
    // functions can grow the map unbounded. We evict the
    // entire memo when it exceeds this threshold (cheap
    // because the next call repopulates on demand).
    // 4096 entries ≈ a few hundred functions, well below
    // typical workspace sizes.
    static constexpr std::size_t PREDICATE_MEMO_MAX_ENTRIES = 4096;

    // Issue #116: defer CoercionNode insertion to a separate
    // explicit pass. The type checker no longer mutates the
    // FlatAST's parent→child links; instead it accumulates
    // coercion intent here, and the caller applies it via
    // `apply_coercion_map(flat, take_coercions())` once type
    // checking is done. This preserves `ast:snapshot` /
    // `ast:rollback` semantics and makes the type checker safe
    // to call on versioned/shared FlatASTs.
    CoercionMap coercions_;
    const CoercionMap& coercions() const { return coercions_; }
    CoercionMap take_coercions() { return std::move(coercions_); }

    InferenceEngine(aura::core::TypeRegistry& reg, aura::diag::DiagnosticCollector& diag);

    // FlatAST inference entries
    aura::core::TypeId infer_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                  aura::ast::NodeId node);
    void check_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool, aura::ast::NodeId id,
                    aura::core::TypeId expected);

    // Initialize environment with primitive type signatures
    void init_primitive_env();

    // Bind declared type sigs (from inject_type_sigs) to the env.
    // Called by TypeChecker::infer_flat after construction so the
    // explicit name → TypeId map takes effect.
    void bind_declared_sigs();

    // Issue #100: expose is_coercible for unit tests. The compile-time
    // C++ tests in tests/test_ir.cpp construct an InferenceEngine
    // directly and call is_coercible on synthetic types (Record /
    // Variant / ADT width matching). Marking it public keeps the
    // production callers (TypeChecker::check_flat) using it as a
    // private member; only the test surface sees it. Adding "public"
    // here doesn't widen the type-checker module's external API — it
    // only relaxes the class's access modifier inside the module.
public:
    bool is_coercible(aura::core::TypeId from, aura::core::TypeId to);

private:
    // FlatAST per-node-type inference
    aura::core::TypeId synthesize_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                       aura::ast::NodeId id, aura::ast::NodeView v);
    // Issue #118: takes FlatAST& + NodeId so diagnostic paths
    // can call set_node_error() at the offending node. The
    // previous signature only took NodeView, which made it
    // impossible to tag the AST node where the error was
    // detected (AuraQuery's `has-error?` was silently broken
    // for unbound-variable and module-member lookups).
    aura::core::TypeId synthesize_flat_var(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                           aura::ast::NodeId id, aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_call(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                            aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_lambda(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                              aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_if(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                          aura::ast::NodeId if_id, aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_let(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                           aura::ast::NodeId node_id, aura::ast::NodeView v,
                                           bool is_rec);
    aura::core::TypeId synthesize_flat_begin(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                             aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_annotation(aura::ast::FlatAST& flat,
                                                  aura::ast::StringPool& pool,
                                                  aura::ast::NodeView v);

    void check_flat_call(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                         aura::ast::NodeView v, aura::core::TypeId expected);
    void check_flat_lambda(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                           aura::ast::NodeView v, aura::core::TypeId expected);

    aura::core::TypeId lub(aura::core::TypeId a, aura::core::TypeId b);

    // Register all built-in primitives in the type environment
    void register_primitive(std::string name, std::vector<aura::core::TypeId> param_types,
                            aura::core::TypeId ret_type);
    void register_poly_primitive(std::string name, std::vector<aura::core::TypeId> param_types,
                                 aura::core::TypeId ret_type,
                                 std::vector<aura::core::TypeId> type_vars);
};

// ── TypeChecker — Public API ─────────────────────────────
// Issue #212 Phase 1d: pure-function extraction of the
// type-checker hot path. The result struct + free function
// below mirror the pattern in `aura.compiler.constant_folding`
// (Cycle 1 of #212) and `aura.compiler.compute_kind` /
// `aura.compiler.arity` (existing). The TypeChecker struct
// itself becomes a thin wrapper that holds per-instance
// state (declared-sig maps, strict mode, cache epoch,
// accumulated stats) and routes through the pure function.
//
// Why extract this:
//   - The hot path (`infer_flat`) is called from many sites
//     (service.ixx, main.cpp CLI, mutation primitives, typed-
//     mutate, partial re-inference, etc.). A pure function
//     makes the call graph explicit and testable in isolation
//     (no TypeChecker lifecycle).
//   - Strict-mode + cache-epoch + declared-sigs are caller
//     state. Today they're baked into TypeChecker member
//     fields. With the pure function, the caller passes them
//     in explicitly — same semantics, but the function
//     becomes a black box that any caller can drive.
//   - The result struct bundles the inferred type + the
//     deferred coercion map + the per-call cache stats, so
//     the caller doesn't need to call into the TypeChecker
//     to get the stats or take the coercions.
export struct TypeCheckResult {
    aura::core::TypeId inferred_type{}; // default-constructed = invalid
    CoercionMap coercions;              // deferred coercion intent
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t stale_cache = 0;
    // Issue #412: cache hits rescued by the gen check.
    // See InnerStats::gen_saved.
    std::uint64_t gen_saved = 0;

    // Issue #280: narrowing evidence bitmask captured from the
    // outermost IfExpr's predicate in the root expression. See
    // kNarrow* constants and narrowing_bit_for() below.
    // 0 = no narrowing applied (no predicate found, or the
    //     predicate wasn't recognized).
    std::uint32_t narrow_evidence = 0;

    // Issue #281: predicate memo stats. Mirrors the
    // InferenceEngine's internal counters, copied into the
    // result for the caller. hit_rate ≈ hits / (hits + misses).
    std::uint64_t predicate_memo_hits = 0;
    std::uint64_t predicate_memo_misses = 0;
    std::uint64_t predicate_memo_evictions = 0;
};

// Pure: type-check a FlatAST subtree and return the inferred
// type + deferred coercions + per-call stats. Constructs a
// short-lived InferenceEngine internally; the engine is the
// same as the legacy TypeChecker::infer_flat used.
//
// All caller state (declared-sigs, module-src, strict, cache
// epoch) is passed in as parameters. No `this`, no member
// access. The function is the canonical entry point; the
// TypeChecker struct is a thin convenience wrapper for
// callers that want to accumulate state across calls.
//
// The `sigs` map is the *resolved* form (name → TypeId), as
// produced by `TypeChecker::inject_type_sigs`. The string→string
// form stays in `inject_type_sigs` (the stateful setup step);
// this pure function assumes resolution has already happened.
export TypeCheckResult
type_check_flat_pure(aura::ast::FlatAST& flat, aura::ast::StringPool& pool, aura::ast::NodeId root,
                     aura::core::TypeRegistry& types, aura::diag::DiagnosticCollector& diag,
                     const std::unordered_map<std::string, aura::core::TypeId>& sigs = {},
                     const std::unordered_map<std::string, std::string>& module_src = {},
                     bool strict = false, std::uint64_t cache_epoch = 0,
                     void* metrics = nullptr, // Issue #258: optional metrics pointer
                     bool bidirectional_mode = true) // Issue #283 follow-up #5
    // Issue #213 follow-up: C++26 contract. The function
    // is total: it handles any `root` (including
    // NULL_NODE — returns an invalid TypeId), any sigs /
    // module_src maps (empty maps are the default). The
    // only precondition is that `diag` is a live
    // DiagnosticCollector (i.e. not moved-from). This is
    // implicit at the language level.
    pre(true);

export struct TypeChecker {
    aura::core::TypeRegistry& types;
    aura::core::TypeId infer_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                  aura::ast::NodeId node, aura::diag::DiagnosticCollector& diag);

    // 注入自定义类型签名（来自 declare-type / 模块类型声明）
    // 在 infer_flat 前调用。name_to_sig: (name → "param1 param2|rettype")。
    // 格式示例: "Int Int|Int" 表示 (Int, Int) -> Int
    // module_src: name → 来源模块文件（用于跨模块错误定位）
    void inject_type_sigs(const std::unordered_map<std::string, std::string>& sigs,
                          const std::unordered_map<std::string, std::string>& module_src = {});

    // 查询已注入类型的来源模块名
    std::string declared_type_module(const std::string& name) const;

    // Issue #72: access the incremental typecheck stats across
    // infer_flat calls. The InferenceEngine is short-lived (per
    // infer_flat), so we accumulate stats on the TypeChecker
    // itself for visibility.
    struct IncrementalStats {
        std::uint64_t cache_hits = 0;
        std::uint64_t cache_misses = 0;
        std::uint64_t stale_cache = 0;
        // Issue #412: cache hits rescued by the gen check.
        // See InnerStats::gen_saved for the full rationale.
        // gen_saved + (stale_cache - gen_saved) is the
        // breakdown of how many pre-#412 stale_cache
        // rejections were false positives (now hits) vs.
        // real staleness (still rejected).
        std::uint64_t gen_saved = 0;
        // Issue #411 follow-up #1: per-symbol re-inference
        // path tracking. See InnerStats for the full
        // field-by-field rationale.
        std::uint64_t per_symbol_used_total = 0;
        std::uint64_t per_symbol_visited_total = 0;
        std::uint64_t ancestor_used_total = 0;
        std::uint64_t ancestor_visited_total = 0;
        // Issue #411 fu1 follow-up #3: per-DefUseIndex
        // re-inference path tracking. See InnerStats
        // for the full rationale.
        std::uint64_t per_defuse_index_used_total = 0;
        std::uint64_t per_defuse_index_walk_fallback_total = 0;
    };
    IncrementalStats stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

    // Issue #168: gate the type cache by the global mutation
    // epoch. CompilerService calls this with the current
    // mutation_epoch_ (#166) before every infer_flat. The
    // epoch is stored on the TypeChecker and forwarded to
    // the per-call InferenceEngine, which uses it to
    // invalidate the cache on epoch advance.
    void set_cache_epoch(std::uint64_t epoch) { cache_epoch_ = epoch; }
    // Issue #258: plumb the CompilerMetrics pointer through
    // to ConstraintSystem::solve_delta() for timing. Today
    // solve_delta isn't called from infer_flat_partial, so
    // this is a future-use hook — but having the plumbing
    // in place means the actual optimization wiring in
    // a follow-up issue can just call set_metrics(&metrics)
    // and start timing automatically.
    void set_metrics(void* m) { metrics_ = m; }
    void* metrics_ = nullptr;

    // Issue #130: cache hit rate (0.0 .. 1.0). Computed
    // as hits / (hits + misses + stale). Returns 0.0 if
    // no incremental checks have been done. Useful for
    // profiling mutation-heavy workloads: a high hit rate
    // (e.g. >0.7) means the dirty-tracking is working
    // well; a low hit rate (e.g. <0.3) means most
    // mutations touch too much of the AST.
    double cache_hit_rate() const {
        const auto s = stats_;
        const std::uint64_t total = s.cache_hits + s.cache_misses + s.stale_cache;
        if (total == 0)
            return 0.0;
        return static_cast<double>(s.cache_hits) / static_cast<double>(total);
    }

    // Issue #148 Phase 4: partial re-inference entry point.
    // Given a MutationRecord, identify the affected node set
    // (via affected_subtree_from_mutation), re-infer each
    // affected node incrementally (add_delta + solve_delta
    // per node), and update IncrementalStats. The unaffected
    // nodes keep their cached type_id (cache hit).
    //
    // The MVP does per-node re-inference. A more efficient
    // batched approach (re-synthesize the subtree once, then
    // solve_delta) is a follow-up if the benchmark in Phase 6
    // shows the per-node overhead is significant.
    //
    // Returns the number of nodes that were re-inferred.
    // cache_hits is the number of nodes that were NOT in the
    // affected set (i.e. kept their cached type_id).
    std::size_t infer_flat_partial(aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                                   const aura::ast::MutationRecord& rec,
                                   aura::diag::DiagnosticCollector& diag);

    // Issue #411 fu1 follow-up #3: per-DefUseIndex-tracker-
    // aware overload. When `per_defuse_index_tracker` is
    // non-null AND the mutation record's sym_id is in the
    // tracker, the O(uses) per-DefUseIndex path is used
    // (replaces the O(n) per_symbol walk with an O(K)
    // lookup where K is the use-site count for that
    // specific binding). When the tracker is null or the
    // sym isn't tracked, falls back to the existing
    // O(n) `affected_subtree_for_symbol` walk. Falls back
    // further to the ancestor walk if neither path yields
    // a non-empty affected set.
    //
    // The metric bumps (per_symbol vs per_defuse_index vs
    // ancestor) tell the user which path was used. The
    // speedup ratio =
    // per_defuse_index_visited_total /
    // per_symbol_reinfer_visited_total (over time)
    // measures the optimization's effectiveness.
    std::size_t infer_flat_partial(aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                                   const aura::ast::MutationRecord& rec,
                                   aura::diag::DiagnosticCollector& diag,
                                   void* per_defuse_index_tracker);

    // Issue #116: deferred CoercionNode insertion. infer_flat
    // now collects coercion intent in this map rather than
    // mutating the FlatAST directly. The caller is expected
    // to apply it via `apply_coercion_map(flat, take_coercions())`
    // after type checking is done — typically right before
    // lowering to IR. The map is consumed (moved-out) on take
    // so subsequent infer_flat calls start from an empty map.
    const CoercionMap& last_coercions() const { return last_coercions_; }
    CoercionMap take_coercions() { return std::move(last_coercions_); }

    // Convenience: infer_flat + apply deferred coercions to
    // the FlatAST in one call. Use this when the caller is
    // about to lower the AST to IR and needs CoercionNodes
    // present. For pure type-checking paths (e.g. the
    // `typecheck` command that just reports types) the plain
    // `infer_flat` is sufficient and avoids unnecessary work.
    aura::core::TypeId infer_flat_apply(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                        aura::ast::NodeId node,
                                        aura::diag::DiagnosticCollector& diag) {
        auto tid = infer_flat(flat, pool, node, diag);
        auto cm = take_coercions();
        if (!cm.empty()) {
            apply_coercion_map(flat, cm);
        }
        return tid;
    }

    // Issue #79: strict type-check mode.
    //
    // When strict == true, `is_coercible` rejects cross-type coercions
    // (e.g. Int ↔ String) and the type-checker reports them as hard
    // `TypeError` diagnostics rather than silent `Note`s that pass
    // through `has_errors() == false`. Dynamic ↔ X (gradual core) is
    // still allowed in both modes.
    //
    // Default is `false` for backward compatibility with existing
    // callers (EDSL, tests). `CompilerService::typecheck()` opts in
    // because that's what users mean by "typecheck".
    void set_strict(bool s) { strict_ = s; }
    bool is_strict() const { return strict_; }

    explicit TypeChecker(aura::core::TypeRegistry& reg)
        : types(reg) {}

private:
    // name → 模块源文件路径（来自 inject_type_sigs 的 module_src）
    std::unordered_map<std::string, std::string> type_module_src_;

    // name → declared TypeId, set by inject_type_sigs and passed
    // to InferenceEngine so each declared name can be bound to
    // the env even if its TypeId is shared with another name
    // (post-#70 interning).
    std::unordered_map<std::string, aura::core::TypeId> type_sigs_;

    // Issue #72: incremental typecheck stats (accumulated across
    // infer_flat calls for test visibility).
    IncrementalStats stats_{};

    // Issue #79: strict type-check mode. When true, the InferenceEngine
    // rejects cross-type coercions (Int ↔ String, Int ↔ Bool, etc.) and
    // reports them as TypeError instead of silently coercing via Notes.
    bool strict_ = false;

    // Issue #168: cache epoch. Set by CompilerService to the
    // global mutation_epoch_ (#166) before each infer_flat
    // call. Forwarded to the per-call InferenceEngine, which
    // invalidates its cache on epoch advance.
    std::uint64_t cache_epoch_ = 0;

    // Issue #116: see last_coercions() / take_coercions() above.
    CoercionMap last_coercions_;
};

// Issue #147: post-mutation invariant check. Walks the dirty subtree
// implied by a single MutationRecord (descendants of target_node plus
// the dirty ancestors via mark_dirty_upward), re-validates linear
// ownership on dirty bindings, and emits a note for each occurrence
// narrowing live in the dirty scope (so the caller can warn or block
// under Strict mode).
//
// `reg` is the persistent TypeRegistry from CompilerService; the
// analyze_predicate_flat call inside needs it to look up refinement
// types. notes_out is appended to (not cleared) so callers can chain
// multiple checks. Returns:
//   - NotChecked if MutationRecord is malformed (NULL_NODE etc.) and
//     no useful work could be done.
//   - Ok if the check ran and produced zero notes.
//   - Warnings if the check produced any notes. The mode-based
//     decision (warn vs block) is the caller's responsibility — this
//     function does not see InvariantCheckMode by design (kept pure).
export aura::ast::InvariantStatus
post_mutation_invariant_check(aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                              aura::core::TypeRegistry& reg, const aura::ast::MutationRecord& rec,
                              std::vector<OwnershipNote>& notes_out);

// Issue #260: ADT match exhaustiveness for a single __match_tmp let node.
// Returns missing constructor names (empty if complete, wildcard, or N/A).
export std::vector<std::string>
analyze_match_exhaustiveness(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                             aura::core::TypeRegistry& reg, aura::ast::NodeId let_node);

// Issue #148 Phase 3: identify the affected node set for a mutation.
// Returns NodeIds in the dirty subtree (descendants of the mutated
// target) + the dirty-upward chain (ancestors marked by
// mark_dirty_upward). Used by InferenceEngine::infer_flat in Phase 4
// to scope partial re-inference to just the affected nodes — skipping
// untouched subtrees that would re-infer the same types.
//
// Walk strategy (matches post_mutation_invariant_check):
//   1. Pick walk_root = parent_id (subtree-level mutation) or
//      target_node (typed mutation on existing node).
//   2. collect_descendants(walk_root) — the entire dirty subtree.
//   3. Climb from rec.target_node via FlatAST::parent_of (public
//      accessor) to add the dirty-upward ancestor chain.
//   4. Safety-bounded ancestor walk to defend against parent_
//      cycles in malformed FlatASTs.
//
// Returns an empty vector if walk_root is NULL or out-of-range
// (the caller can then fall back to a full infer_flat).
export std::vector<aura::ast::NodeId>
affected_subtree_from_mutation(const aura::ast::FlatAST& flat,
                               const aura::ast::MutationRecord& rec);

// Issue #410: per-symbol affected subtree. The companion to
// affected_subtree_from_mutation above. Instead of walking
// the target_node's parent chain (ancestor-only), this walks
// every node in the flat looking for Variable nodes whose
// sym_id matches the input — that's the true "set of nodes
// affected by a change to symbol sym_id".
//
// Why this matters: today TypeChecker::affected_subtree_from_mutation
// returns descendants + dirty-upward ancestors. For a single
// `mutate:rebind` on `(define x 1)` inside a 50-binding body,
// the ancestor chain marks the whole body dirty (and infers
// every binding's type from scratch). With per-symbol
// granularity, only the ~3-5 nodes that USE `x` are in the
// affected set — the other 45 bindings keep their cached
// types.
//
// The set returned is exactly the nodes that re-infer_flat_partial
// would visit if #410 Phase 2/2 wires this into
// infer_flat_partial. The Phase 1/2 close ships this as a
// standalone helper so the observability primitive can
// measure the savings.
//
// Returns empty vector if sym_id is INVALID_SYM or the flat
// has no Variable nodes referencing it. O(n) walk — the
// production path (when DefUseIndex is built) can use
// DefUseIndex::query_def_use(sym).uses instead, which is O(uses).
export std::vector<aura::ast::NodeId>
affected_subtree_for_symbol(const aura::ast::FlatAST& flat,
                            aura::ast::SymId sym_id);

// Issue #274: post-mutation invariant visitor for run_mutation_pipeline.
// Skips records already checked; accumulates notes and worst status.
// apply_status_updates() writes invariant_status back to the log
// (visit_mutation takes const MutationRecord& per the concept).
export class PostMutationInvariantVisitor {
public:
    PostMutationInvariantVisitor(const aura::ast::StringPool& pool, aura::core::TypeRegistry& reg)
        : pool_(pool), reg_(reg) {}

    void visit_mutation(aura::ast::FlatAST& flat, const aura::ast::MutationRecord& rec) {
        if (rec.invariant_status != aura::ast::InvariantStatus::NotChecked)
            return;
        std::vector<OwnershipNote> notes;
        auto st = post_mutation_invariant_check(flat, pool_, reg_, rec, notes);
        status_updates_[rec.mutation_id] = st;
        if (st == aura::ast::InvariantStatus::Warnings) {
            worst_ = aura::ast::InvariantStatus::Warnings;
            for (auto& n : notes)
                notes_out_.push_back(std::move(n));
        }
    }

    bool has_error() const { return false; }

    aura::ast::InvariantStatus worst_status() const { return worst_; }
    const std::vector<OwnershipNote>& notes() const { return notes_out_; }

    void apply_status_updates(aura::ast::FlatAST& flat) const {
        if (status_updates_.empty())
            return;
        for (auto& rec : flat.all_mutations()) {
            if (auto it = status_updates_.find(rec.mutation_id); it != status_updates_.end())
                rec.invariant_status = it->second;
        }
    }

private:
    const aura::ast::StringPool& pool_;
    aura::core::TypeRegistry& reg_;
    aura::ast::InvariantStatus worst_ = aura::ast::InvariantStatus::Ok;
    std::vector<OwnershipNote> notes_out_;
    std::unordered_map<std::uint64_t, aura::ast::InvariantStatus> status_updates_;
};

static_assert(aura::ast::MutationVisitor<PostMutationInvariantVisitor>);

} // namespace aura::compiler
