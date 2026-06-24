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