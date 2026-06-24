// ──────────────────────────────────────────────────────────────────
//  query_matcher.ixx — shared pattern matcher for query:pattern
//  and mutate:replace-pattern (Issue #482).
//
//  Extracted from evaluator_primitives_query_workspace.cpp so both
//  primitives see the same node set for the same pattern. The
//  pre-#482 design had two independent matchers that disagreed on
//  `...` semantics (query was strict in #289, then Kleene in #481;
//  mutate was always strict). Sharing the matcher means both
//  primitives agree on which nodes match, regardless of
//  `:nested-arity` mode.
//
//  Module interface unit. Both call sites import this:
//    import aura.compiler.matcher;
// ──────────────────────────────────────────────────────────────────

module;

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

export module aura.compiler.matcher;

import aura.core.ast;
import aura.core.mutation;

using namespace aura::ast;

namespace aura::compiler {

// Per-match state. The captures vector is insertion-ordered; the
// matcher uses savepoint / resize-back for backtracking. Linear
// scan lookup is fine for typical patterns (≤10 captures).
export struct QueryMatchState {
    std::vector<std::pair<SymId, NodeId>> captures;
    bool nested_arity = false;  // true = Kleene (0..N), false = strict (exactly 1)
    int depth = 0;
};

// Concrete matcher. Construct with the four AST pointers + the
// interned wildcard symbol. Call `match_subtree(ws_root, pat_root)`
// to test if the workspace root matches the pattern root. The match
// state is owned by the matcher; reset between independent matches
// by reassigning `state` (preserving `nested_arity`).
export class QueryMatcher {
public:
    QueryMatcher(FlatAST* ws_flat,
                 StringPool* ws_pool,
                 FlatAST* pat_flat,
                 StringPool* pat_pool,
                 SymId wildcard_sym,
                 bool nested_arity);

    // ─── Pure helpers ─────────────────────────────────────────
    bool is_wildcard(NodeId pid) const;
    bool is_capture(NodeId pid) const;

    // ─── Savepoint helpers ─────────────────────────────────────
    std::size_t save() const { return state.captures.size(); }
    void rollback(std::size_t save) { state.captures.resize(save); }

    // ─── Core matchers ────────────────────────────────────────
    bool match_subtree(NodeId ws_id, NodeId pat_id);
    bool match_list(std::span<const NodeId> ws_ch,
                    std::span<const NodeId> pat_ch);
    bool pat_has_ellipsis_rec(NodeId pid);

    // ─── Issue #292: guard predicate support ────────────────
    // If a pattern is wrapped in `(:guard <sub-pat> "guard-expr")`,
    // the matcher detects the keyword-head Call, matches the
    // sub-pattern, and on success stashes a (capture_set,
    // guard_expr_string) pair into pending_guards_. The caller
    // (query:pattern) checks pending_guards_ after each match and
    // evaluates the guard via the Aura evaluator with captures
    // bound as locals. If the guard returns false, the match is
    // rejected.
    struct PendingGuard {
        std::vector<std::pair<SymId, NodeId>> captures;
        std::string guard_expr;
    };
    std::vector<PendingGuard> pending_guards_;
    SymId guard_sym_ = 0;  // interned ":guard" symbol in pat_pool

    // Setup: intern the ":guard" keyword symbol. Call from the
    // pattern-parsing site (e.g. query:pattern) before matching.
    void setup_guard_detection();

    // Returns true if the given pat_id is a (:guard <sub-pat> "expr")
    // wrapper Call node. Callers (e.g. query:pattern) can use this
    // to bypass the (tag, arity) index fast path — the wrapper's
    // tag is Call (not the sub-pattern's tag), so the index would
    // skip all matching positions. Slow path does a full walk.
    [[nodiscard]] bool is_guard_root(NodeId pat_id) const;

    // After match_subtree returns true, check pending_guards_.
    // Returns true if no guard or guard is satisfied; false if
    // guard failed (caller should reject the match).
    [[nodiscard]] bool has_pending_guard() const { return !pending_guards_.empty(); }
    [[nodiscard]] const PendingGuard& take_pending_guard() {
        return pending_guards_.back();
    }
    void clear_pending_guard() { pending_guards_.pop_back(); }

    // ─── Public state ─────────────────────────────────────────
    QueryMatchState state;

private:
    FlatAST* ws_flat_;
    StringPool* ws_pool_;
    FlatAST* pat_flat_;
    StringPool* pat_pool_;
    SymId wildcard_sym_;
};

}  // namespace aura::compiler