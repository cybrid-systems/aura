module;
#include <cstdint>

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
    std::size_t dirty_count_ = 0;  // O(1) "is anything dirty?"
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
    bool can_move(const std::string& name) const {
        return get(name) == OwnershipState::Owned;
    }

    // Can drop the variable (only if fully owned)
    bool can_drop(const std::string& name) const {
        return get(name) == OwnershipState::Owned;
    }

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
    void mark_ownership_dirty(const std::string& name) {
        ownership_dirty_.insert(name);
    }
    void mark_ownership_dirty_subtree(const std::vector<std::string>& names) {
        for (auto& n : names)
            ownership_dirty_.insert(n);
    }
    bool is_ownership_dirty(const std::string& name) const {
        return ownership_dirty_.count(name) > 0;
    }
    void clear_ownership_dirty() {
        ownership_dirty_.clear();
    }
    const std::unordered_set<std::string>& ownership_dirty_bindings() const {
        return ownership_dirty_;
    }

    // ── Post-Mutation Ownership Validation ────────────────────
    // Walks the AST within the dirty set, re-simulates ownership flow,
    // and reports any violations. Returns true if all checks pass.
    static bool validate_ownership(
        const aura::ast::FlatAST& flat,
        const aura::ast::StringPool& pool,
        aura::ast::NodeId root,
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
    static bool validate_ownership_full(
        const aura::ast::FlatAST& flat,
        const aura::ast::StringPool& pool,
        aura::ast::NodeId root,
        std::vector<OwnershipNote>& notes_out);

    // Internal: shared implementation of the post-hoc ownership
    // walk. Public callers should use validate_ownership or
    // validate_ownership_full. Kept separate so the two
    // discovery paths can share the same walk.
    static bool validate_ownership_impl(
        const aura::ast::FlatAST& flat,
        const aura::ast::StringPool& pool,
        aura::ast::NodeId root,
        const std::unordered_set<std::string>& dirty_bindings,
        std::vector<OwnershipNote>& notes_out);
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
public:
    // Issue #79: set strict mode. Called by TypeChecker::infer_flat
    // before delegating to the engine.
    void set_strict(bool s) { strict_ = s; }

    // Issue #103: set permissive mode. Default is true. Set to
    // false to opt into the old behavior (error on constraint
    // solve failure, even in non-strict mode). Strict mode
    // (set_strict(true)) always wins — strict+permissive = strict.
    void set_permissive(bool p) { permissive_ = p; }
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
    };
    InnerStats stats_;
    InnerStats stats() const { return stats_; }

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
    aura::core::TypeId synthesize_flat_var(aura::ast::FlatAST& flat, aura::ast::StringPool& pool, aura::ast::NodeId id, aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_call(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                            aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_lambda(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                              aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_if(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                          aura::ast::NodeView v);
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
   void register_poly_primitive(std::string name,
                                 std::vector<aura::core::TypeId> param_types,
                                 aura::core::TypeId ret_type,
                                 std::vector<aura::core::TypeId> type_vars);
};

// ── TypeChecker — Public API ─────────────────────────────
export struct TypeChecker {
    aura::core::TypeRegistry& types;
    aura::core::TypeId infer_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                  aura::ast::NodeId node, aura::diag::DiagnosticCollector& diag);

    // 注入自定义类型签名（来自 declare-type / 模块类型声明）
    // 在 infer_flat 前调用。name_to_sig: (name → "param1 param2|rettype")。
    // 格式示例: "Int Int|Int" 表示 (Int, Int) -> Int
    // module_src: name → 来源模块文件（用于跨模块错误定位）
    void inject_type_sigs(
        const std::unordered_map<std::string, std::string>& sigs,
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
    };
    IncrementalStats stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

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
        if (total == 0) return 0.0;
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
    std::size_t infer_flat_partial(
        aura::ast::FlatAST& flat,
        const aura::ast::StringPool& pool,
        const aura::ast::MutationRecord& rec);

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
export aura::ast::InvariantStatus post_mutation_invariant_check(
    aura::ast::FlatAST& flat,
    const aura::ast::StringPool& pool,
    aura::core::TypeRegistry& reg,
    const aura::ast::MutationRecord& rec,
    std::vector<OwnershipNote>& notes_out);

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
affected_subtree_from_mutation(
    const aura::ast::FlatAST& flat,
    const aura::ast::MutationRecord& rec);

} // namespace aura::compiler
