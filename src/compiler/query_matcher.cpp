// ──────────────────────────────────────────────────────────────────
//  query_matcher.cpp — implementation of the QueryMatcher declared
//  in query_matcher.ixx (Issue #482).
//
//  Module implementation unit for aura.compiler.matcher. The
//  .ixx declares the class; this .cpp provides the bodies.
// ──────────────────────────────────────────────────────────────────

module;

module aura.compiler.matcher;
import std;

import aura.core.ast;
import aura.core.mutation;

namespace aura::compiler {

QueryMatcher::QueryMatcher(FlatAST* ws_flat, StringPool* ws_pool, FlatAST* pat_flat,
                           StringPool* pat_pool, SymId wildcard_sym, bool nested_arity,
                           bool skip_macro_introduced)
    : ws_flat_(ws_flat)
    , ws_pool_(ws_pool)
    , pat_flat_(pat_flat)
    , pat_pool_(pat_pool)
    , wildcard_sym_(wildcard_sym)
    , skip_macro_introduced_(skip_macro_introduced) {
    state.nested_arity = nested_arity;
    // Issue #292: intern ":guard" so the matcher can detect
    // (:guard <sub-pat> "expr") forms in the pattern.
    if (pat_pool_) {
        guard_sym_ = pat_pool_->intern(":guard");
    }
}

void QueryMatcher::setup_guard_detection() {
    // No-op for now; reserved for future guard variants.
}

bool QueryMatcher::is_guard_root(NodeId pat_id) const {
    if (pat_id == NULL_NODE || pat_id >= pat_flat_->size())
        return false;
    auto pn = pat_flat_->get(pat_id);
    if (pn.tag != NodeTag::Call || pn.children.size() < 3)
        return false;
    if (pn.children[0] == NULL_NODE || pn.children[0] >= pat_flat_->size())
        return false;
    auto head = pat_flat_->get(pn.children[0]);
    return head.tag == NodeTag::Variable && head.sym_id == guard_sym_;
}

bool QueryMatcher::is_wildcard(NodeId pid) const {
    if (pid == NULL_NODE || pid >= pat_flat_->size())
        return false;
    auto pn = pat_flat_->get(pid);
    return pn.tag == NodeTag::Variable && pn.sym_id == wildcard_sym_;
}

bool QueryMatcher::is_capture(NodeId pid) const {
    if (pid == NULL_NODE || pid >= pat_flat_->size())
        return false;
    auto pn = pat_flat_->get(pid);
    if (pn.tag != NodeTag::Variable)
        return false;
    auto name = pat_pool_->resolve(pn.sym_id);
    return name.size() >= 2 && name[0] == '?';
}

bool QueryMatcher::match_subtree(NodeId ws_id, NodeId pat_id) {
    if (++state.depth > 64)
        return false;
    if (pat_id >= pat_flat_->size())
        return ws_id >= ws_flat_->size();
    if (ws_id >= ws_flat_->size() || pat_id == NULL_NODE)
        return (pat_id == NULL_NODE) ? (ws_id == NULL_NODE) : false;

    // Issue #421 + #1255: recursive hygiene — hard skip MacroIntroduced
    // subtrees when the caller did not pass :include-macro-introduced /
    // :allow-macro? #t. Always count the filter (fast path included).
    if (skip_macro_introduced_ && ws_flat_->is_macro_introduced(ws_id)) {
        ++recursive_macro_skipped_;
        ++macro_intro_filtered_strict_;
        return false;
    }

    auto ws_node = ws_flat_->get(ws_id);
    auto pat_node = pat_flat_->get(pat_id);

    // (1) Wildcard "..." — match any subtree. Issue #482: also
    // bind a capture so mutate:replace-pattern can substitute
    // `...` placeholders in the replacement template. Query:
    // pattern doesn't read `state.captures` for `...`-only
    // matches (it only uses the bool return + the per-position
    // `?x` captures), so adding captures here is harmless for
    // the query site. The capture uses a sentinel sym_id (0)
    // so `?x` lookups never collide.
    if (is_wildcard(pat_id)) {
        // Issue #1695: StableNodeRef capture (gen-tagged).
        state.captures.emplace_back(static_cast<SymId>(0), ref_of(ws_id));
        return true;
    }

    // (2) Capture variable "?x" — bind on first, match on later.
    if (is_capture(pat_id)) {
        auto key = pat_node.sym_id;
        for (auto& kv : state.captures) {
            if (kv.first == key) {
                // Issue #1695: read NodeId from StableNodeRef.
                const auto bound_id = kv.second.id;
                if (bound_id == NULL_NODE || bound_id >= ws_flat_->size())
                    return false;
                auto bound = ws_flat_->get(bound_id);
                if (bound.tag != ws_node.tag)
                    return false;
                switch (bound.tag) {
                    case NodeTag::LiteralInt:
                        return bound.int_value == ws_node.int_value;
                    case NodeTag::LiteralFloat:
                        return bound.float_value == ws_node.float_value;
                    case NodeTag::Variable:
                    case NodeTag::LiteralString:
                        return ws_pool_->resolve(bound.sym_id) == ws_pool_->resolve(ws_node.sym_id);
                    default:
                        // Composite node: fall back to identity
                        return bound_id == ws_id;
                }
            }
        }
        state.captures.emplace_back(key, ref_of(ws_id));
        return true;
    }

    // (3) Issue #292: detect (:guard <sub-pat> "guard-expr")
    // form. This check runs BEFORE the tag comparison because
    // a (:guard ?x "expr") pattern wraps a sub-pattern that
    // may have a different tag than the wrapper Call itself.
    // The matcher extracts the sub-pattern, recurses on it,
    // and stashes (captures, guard-expr) for the caller to
    // evaluate. The guard sym is interned in the constructor.
    if (pat_node.tag == NodeTag::Call && pat_node.children.size() >= 2 &&
        pat_node.children[0] != NULL_NODE && pat_node.children[0] < pat_flat_->size()) {
        auto head_node = pat_flat_->get(pat_node.children[0]);
        if (head_node.tag == NodeTag::Variable && head_node.sym_id == guard_sym_) {
            // Form: (:guard <sub-pat> "guard-expr")
            // children[1] is the sub-pattern root
            // children[2] (if LiteralString) is the guard expression
            std::string guard_expr;
            NodeId sub_pat = NULL_NODE;
            if (pat_node.children.size() >= 3 && pat_node.children[1] != NULL_NODE &&
                pat_node.children[1] < pat_flat_->size()) {
                auto expr_node = pat_flat_->get(pat_node.children[1]);
                if (expr_node.tag == NodeTag::LiteralString) {
                    guard_expr = pat_pool_->resolve(expr_node.sym_id);
                }
            }
            if (pat_node.children.size() >= 3 && pat_node.children[2] != NULL_NODE) {
                sub_pat = pat_node.children[2];
            }
            // Recurse on sub-pattern.
            if (sub_pat != NULL_NODE) {
                auto save = state.captures.size();
                if (!match_subtree(ws_id, sub_pat)) {
                    state.captures.resize(save);
                    return false;
                }
            }
            // Stash (current_captures, guard_expr). The caller
            // checks pending_guards_ and evaluates the guard.
            PendingGuard pg;
            pg.captures = state.captures;
            pg.guard_expr = guard_expr;
            pending_guards_.push_back(std::move(pg));
            return true;
        }
    }

    // (4) Same tag required
    if (ws_node.tag != pat_node.tag)
        return false;

    switch (pat_node.tag) {
        case NodeTag::LiteralInt:
            return ws_node.int_value == pat_node.int_value;
        case NodeTag::LiteralFloat:
            return ws_node.float_value == pat_node.float_value;
        case NodeTag::Variable:
        case NodeTag::LiteralString:
            return ws_pool_->resolve(ws_node.sym_id) == pat_pool_->resolve(pat_node.sym_id);
        case NodeTag::MacroDef:
            return true;
        default: {
            // Composite node: recurse via match_list. Savepoint
            // ensures failed children-match rolls back any
            // captures bound during the attempt.
            // Issue #678: pin workspace children PCV so concurrent
            // structural mutations cannot invalidate ws_ch spans
            // held during Kleene backtracking.
            auto ws_safe = ws_flat_->children_safe_view(ws_id);
            auto save = state.captures.size();
            bool ok = match_list(ws_safe.span(), pat_node.children);
            if (!ok)
                state.captures.resize(save);
            return ok;
        }
    }
}

bool QueryMatcher::match_list(std::span<const NodeId> ws_ch, std::span<const NodeId> pat_ch) {
    if (++state.depth > 64)
        return false;
    if (pat_ch.empty() && ws_ch.empty())
        return true;
    if (pat_ch.empty())
        return false;
    if (ws_ch.empty()) {
        // Workspace exhausted, pattern still has elements.
        // All remaining pattern must be wildcards (each can
        // consume 0) for this to be a match.
        for (auto p : pat_ch)
            if (!is_wildcard(p))
                return false;
        return true;
    }

    auto pc = pat_ch[0];
    if (is_wildcard(pc)) {
        if (!state.nested_arity) {
            // Strict: "..." consumes exactly 1.
            if (!match_subtree(ws_ch[0], pc))
                return false;
            return match_list(ws_ch.subspan(1), pat_ch.subspan(1));
        }
        // Kleene: try consuming 0 first (most permissive),
        // then 1+ via recursive call. Savepoint ensures any
        // captures bound in path A are rolled back before
        // trying path B.
        //
        // Issue #482: in Kleene mode, the wildcard consumes 1+
        // workspace children. Each consumed child must be added
        // to captures so mutate:replace-pattern can substitute
        // `...` placeholders in the replacement template with
        // all the consumed nodes. We add 1 capture per consumed
        // child in the recursive call (path B); path A (consume 0)
        // adds no captures.
        auto save = state.captures.size();
        if (match_list(ws_ch, pat_ch.subspan(1)))
            return true;
        state.captures.resize(save);
        // Path B: consume 1 child + recurse on the rest.
        // Issue #1695: StableNodeRef capture.
        state.captures.emplace_back(static_cast<SymId>(0), ref_of(ws_ch[0]));
        return match_list(ws_ch.subspan(1), pat_ch);
    }
    // Fixed position: 1 ws child consumed, 1 pat element consumed.
    if (!match_subtree(ws_ch[0], pc))
        return false;
    return match_list(ws_ch.subspan(1), pat_ch.subspan(1));
}

bool QueryMatcher::pat_has_ellipsis_rec(NodeId pid) {
    if (pid == NULL_NODE || pid >= pat_flat_->size())
        return false;
    if (is_wildcard(pid))
        return true;
    auto pn = pat_flat_->get(pid);
    for (auto c : pn.children)
        if (pat_has_ellipsis_rec(c))
            return true;
    return false;
}

} // namespace aura::compiler