// macro_expansion.cpp — Issue #265: hygienic macro clone + expansion
// aura.compiler.macro_expansion module implementation.

module;

#include <atomic>
#include <cstdio>
#include <cstdint>
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

module aura.compiler.macro_expansion;

import std;
import aura.core.ast;
import aura.compiler.evaluator_pure;

extern "C" std::size_t aura_evaluator_mutation_boundary_depth();
extern "C" void aura_evaluator_bump_macro_expand_checkpoint_save();
extern "C" std::uint64_t aura_fiber_current_id();
extern "C" int aura_macro_provenance_repin_on_steal(void* ev_ptr, std::uint64_t cloned_marker);

namespace aura::compiler::macro_exp {

namespace detail {

    // Issue #965: keep special forms + high-frequency primitives that
    // macros must not gensym. Expanded beyond the original 56-name set
    // so new stdlib-facing builtins (length, vector, make-hash, …) are
    // preserved. Full registry-driven sync is Phase 2 (needs Evaluator
    // at expand time); this set is the offline source of truth.
    const std::unordered_set<std::string>& hygiene_builtins() {
        static const std::unordered_set<std::string> builtins = {
            // Special forms / control
            "if",
            "cond",
            "let",
            "let*",
            "letrec",
            "lambda",
            "define",
            "begin",
            "set!",
            "quote",
            "unquote",
            "quasiquote",
            "case",
            "when",
            "unless",
            "do",
            "delay",
            "force",
            "import",
            "export",
            "module",
            // Pair / list
            "car",
            "cdr",
            "cons",
            "list",
            "list-sort",
            "pair?",
            "null?",
            "list?",
            "length",
            "append",
            "reverse",
            "member",
            "member?",
            "assoc",
            "eq?",
            "equal?",
            "eqv?",
            // Arithmetic / compare
            "+",
            "-",
            "*",
            "/",
            "quotient",
            "remainder",
            "modulo",
            "=",
            "<",
            ">",
            "<=",
            ">=",
            "not",
            "and",
            "or",
            "void",
            "max",
            "min",
            "abs",
            // IO
            "display",
            "write",
            "newline",
            "read",
            "write-file",
            "read-file",
            // Type predicates
            "number?",
            "integer?",
            "float?",
            "boolean?",
            "string?",
            "symbol?",
            "char?",
            "vector?",
            "hash?",
            "procedure?",
            "error?",
            // String
            "string-append",
            "string-length",
            "string-ref",
            "substring",
            "number->string",
            "string->number",
            "string=?",
            "symbol->string",
            "string->symbol",
            // Higher-order / collections
            "apply",
            "map",
            "filter",
            "foldl",
            "foldr",
            "for-each",
            "vector",
            "make-vector",
            "vector-ref",
            "vector-set!",
            "vector-length",
            "make-hash",
            "hash-ref",
            "hash-set!",
            "hash-count",
            // Errors / assert / gensym
            "error",
            "assert",
            "gensym",
            "raise",
            // Eval / meta commonly used in macros
            "eval",
            "current-time",
        };
        return builtins;
    }

} // namespace detail

// Issue #365: MAX_HYGIENE_DEPTH — upper bound on recursive
// clone_macro_body nesting. Exported from macro_expansion.ixx.
// Prevents stack overflow on pathological inputs
// (self-referential macro bodies, deeply nested quasiquote
// chains). Tuned to be much larger than any realistic macro
// body so well-formed programs never hit it; only
// pathological or adversarial inputs trigger the
// graceful-degradation path.

// Thread-local depth counter for clone_macro_body recursion.
// Each top-level call starts fresh; the recursive calls bump
// the counter. When the counter reaches MAX_HYGIENE_DEPTH, the
// caller logs a warning + returns NULL_NODE (graceful
// degradation — the caller treats NULL as "use original name"
// and falls back to unhygienic behavior). thread_local is safe
// because clone_macro_body is re-entrant only via the caller's
// own recursion chain (no concurrent re-entry from other
// fibers / threads on the same Evaluator).
thread_local int s_hygiene_depth = 0;

// Issue #1245 Phase 1: concurrent hygiene / dirty observability.
std::atomic<std::uint64_t> g_macro_clone_concurrent_fiber_total{0};
std::atomic<std::uint64_t> g_macro_clone_hygiene_dirty_total{0};

// Issue #1247–#1248 Phase 1: macro-origin provenance + hygiene tracer.
std::atomic<std::uint64_t> g_macro_origin_provenance_errors{0};
std::atomic<std::uint64_t> g_hygiene_tracer_expansions{0};
std::atomic<std::uint64_t> g_hygiene_tracer_depth_max{0};
// Issue #1652: clone_macro_body expand observability counters (paired with
// #1611 MacroIntroduced hygiene gate). Bumped at the success path +
// early-return hygiene-violation paths inside clone_macro_body. Exposed via
// the C-linkage accessor + composed into existing (query:pattern-hygiene-stats)
// primitive surface (no new primitive per #1632 “原语最小化” directive).
std::atomic<std::uint64_t> g_macro_expansion_total{0};
std::atomic<std::uint64_t> g_macro_introduced_nodes_created_total{0};
std::atomic<std::uint64_t> g_hygiene_violation_in_macro_expand_total{0};
// Issue #2018: rest-param gensyms applied in clone_macro_body pre-scan /
// rename path (`__rest_<name>_<n>`). Folded into macro-hygiene-stats.
std::atomic<std::uint64_t> g_macro_rest_param_hygiene_total{0};
// Issue #2019: post-expand MacroIntroduced generation restamp calls.
std::atomic<std::uint64_t> g_macro_restamp_after_flat_total{0};

// Issue #1652: C-linkage accessors so the (query:pattern-hygiene-stats)
// primitive can read these file-level atomics from another TU without the
// Evaluator module import (paired pattern with #1648 reflect.hh +
// #1651 macro_expansion.cpp).
// C-linkage readers: atomics live in this namespace (not ::global),
// so use unqualified names — ::g_* fails under modules (#1652/#1757).
extern "C" {
inline std::uint64_t aura_macro_expansion_total_v_read() noexcept {
    return g_macro_expansion_total.load(std::memory_order_relaxed);
}
inline std::uint64_t aura_macro_introduced_nodes_created_total_v_read() noexcept {
    return g_macro_introduced_nodes_created_total.load(std::memory_order_relaxed);
}
inline std::uint64_t aura_hygiene_violation_in_macro_expand_total_v_read() noexcept {
    return g_hygiene_violation_in_macro_expand_total.load(std::memory_order_relaxed);
}
// Non-inline: other TUs (query:macro-hygiene-stats) read this counter.
std::uint64_t aura_macro_rest_param_hygiene_total_v_read() noexcept {
    return g_macro_rest_param_hygiene_total.load(std::memory_order_relaxed);
}
std::uint64_t aura_macro_restamp_after_flat_total_v_read() noexcept {
    return g_macro_restamp_after_flat_total.load(std::memory_order_relaxed);
}
} // extern "C"

// Issue #2019: restamp MacroIntroduced gens after a successful expand
// pass so FlatAST consumers (mutate / query / JIT) never see stale gen.
static void restamp_after_expand(aura::ast::FlatAST& flat) {
    const auto n = flat.restamp_macro_introduced_generations();
    if (n > 0)
        g_macro_restamp_after_flat_total.fetch_add(1, std::memory_order_relaxed);
}

aura::ast::NodeId clone_macro_body(
    aura::ast::FlatAST& target, aura::ast::StringPool& target_pool, aura::ast::FlatAST& source,
    aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId, aura::core::TransparentStringHash,
                             std::equal_to<>>* subst,
    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                       std::equal_to<>>* name_map,
    aura::ast::SyntaxMarker cloned_marker) {
    using namespace aura::ast;
    // Issue #1652: per-call success-path observability bump (fired once per
    // clone_macro_body invocation that survives the early-return hygiene
    // checks). The per-node count (clone_macro_introduced_nodes_created) is
    // deferred to #1688 along with the clone_macro_body recursive-walk
    // refactor that threads the cumulative count through the AST walk.
    g_macro_expansion_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #365: depth guard. The public API starts at
    // depth=0 (s_hygiene_depth is bumped on recursion inside
    // the function body below). When the depth exceeds
    // MAX_HYGIENE_DEPTH, we degrade gracefully by returning
    // NULL_NODE — the caller treats NULL as "no substitution"
    // and proceeds with the unhygienic fallback (original
    // name, no gensym). The warning is emitted ONCE per
    // top-level call (not per recursion level) to avoid
    // log spam.
    //
    // On top-level entry (s_hygiene_depth == 0), reset the
    // once-per-call warning flag so the next top-level call
    // gets a fresh warning budget. This is needed because
    // s_hygiene_depth is thread_local — a previous failed
    // call (or one that aborted via exception) might leave
    // the counter > 0, so we always reset on entry.
    static thread_local bool s_warned_this_call = false;
    if (s_hygiene_depth == 0) {
        s_warned_this_call = false;
    }
    if (s_hygiene_depth >= MAX_HYGIENE_DEPTH) {
        if (!s_warned_this_call) {
            s_warned_this_call = true;
            // Issue #1247: include macro-origin provenance in the diagnostic
            // so Agents can locate which MacroIntroduced path blew the depth.
            g_macro_origin_provenance_errors.fetch_add(1, std::memory_order_relaxed);
            // Issue #1652: paired bump — depth exceeded is a hygiene violation
            // against the macro expand contract. Bump the new g_* counter.
            g_hygiene_violation_in_macro_expand_total.fetch_add(1, std::memory_order_relaxed);
            const char* origin = (cloned_marker == aura::ast::SyntaxMarker::MacroIntroduced)
                                     ? "MacroIntroduced"
                                     : "User";
            std::fprintf(stderr,
                         "[#365/#1247 warning] clone_macro_body exceeded "
                         "MAX_HYGIENE_DEPTH=%d; marker=%s depth=%d "
                         "[MacroIntroduced provenance path]; falling back to "
                         "unhygienic substitution (original name).\n",
                         MAX_HYGIENE_DEPTH, origin, s_hygiene_depth);
        }
        return NULL_NODE;
    }
    // Issue #1248: hygiene provenance tracer — track max depth + expansions.
    {
        auto cur = static_cast<std::uint64_t>(s_hygiene_depth);
        auto prev = g_hygiene_tracer_depth_max.load(std::memory_order_relaxed);
        while (cur > prev && !g_hygiene_tracer_depth_max.compare_exchange_weak(
                                 prev, cur, std::memory_order_relaxed)) {
        }
    }
    if (body_id == NULL_NODE || body_id >= source.size()) {
        // Issue #1652: paired bump — invalid body_id is a hygiene violation
        // (caller passed an out-of-range NodeId for the macro body).
        g_hygiene_violation_in_macro_expand_total.fetch_add(1, std::memory_order_relaxed);
        return NULL_NODE;
    }
    auto v = source.get(body_id);

    // Variable substitution: if this variable is a macro param, return the arg clone.
    //
    // Issue #120: the arg's NodeId is in the *calling* FlatAST (= target),
    // not in `source` (the macro definition's FlatAST). The recursive
    // call to clone_macro_body with body_id=it->second would try to
    // read it->second from `source`, which is wrong (NodeId indices
    // are per-FlatAST). The fix: detect this case and return the
    // arg's NodeId as-is, then recursively clone its children from
    // `target` (not `source`).
    if (subst && v.tag == NodeTag::Variable && v.sym_id != INVALID_SYM) {
        auto name = std::string(source_pool.resolve(v.sym_id));
        auto it = subst->find(name);
        if (it != subst->end()) {
            // Issue #334 follow-up: REVERTED Quote-wrap from commit
            // 6b90641. The Quote-wrap made Variables in macro
            // bodies evaluate to the literal arg value (helped
            // define-struct), but it broke `set!` semantics in
            // normal macros: the set! target became a literal
            // symbol (the arg name) instead of the caller's
            // variable, causing test_issue_137/190 to fail. The
            // original AST subst (returning the arg NodeId
            // directly) is restored for now. The proper fix for
            // #230 #1 (define-struct) is the env-binding path
            // (tracked in issue 334), not Quote-wrap.
            return it->second;
        }
    }

    // Re-intern SymIds: resolve in source_pool, intern in target_pool
    auto transplant = [&](SymId sid) -> SymId {
        return (sid == INVALID_SYM) ? sid
                                    : target_pool.intern(std::string(source_pool.resolve(sid)));
    };

    // Resolve a name through name_map (hygiene: renamed binding)
    auto resolve_name = [&](SymId sid) -> std::string {
        if (sid == INVALID_SYM)
            return "";
        auto name = std::string(source_pool.resolve(sid));
        if (name_map) {
            auto it = name_map->find(name);
            if (it != name_map->end())
                return it->second;
        }
        return name;
    };

    // Rename a binding position for hygiene: gensym if macro-introduced
    std::uint64_t hyg_ctr = 0; // Issue #265: per-call counter
    auto rename_binding_pre = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        if ((subst && subst->count(name)) || detail::hygiene_builtins().count(name))
            return transplant(sid);
        auto it = name_map->find(name);
        if (it != name_map->end())
            return target_pool.intern(it->second);
        auto fresh = std::string("__") + name + "_" + std::to_string(hyg_ctr++);
        (*name_map)[name] = fresh;
        return target_pool.intern(fresh);
    };

    // Issue #2018: rest-param binding hygiene. Dotted Lambda / MacroDef last
    // params get a dedicated `__rest_<name>_<n>` gensym so call-site free
    // identifiers cannot collide with the rest binding. name_map is filled
    // in pre-scan so body uses resolve via resolve_name / rename_binding.
    // Macro-param names that appear only as free uses still go through the
    // Variable subst path (source-name lookup) and are not forced through
    // this map when they are not template-introduced rest bindings.
    auto rename_rest_binding_pre = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        // Macro rest param itself is substituted (subst), not a template
        // binding — leave free uses substitutable under the original name.
        if (subst && subst->count(name))
            return transplant(sid);
        if (detail::hygiene_builtins().count(name))
            return transplant(sid);
        auto it = name_map->find(name);
        if (it != name_map->end())
            return target_pool.intern(it->second);
        auto fresh = std::string("__rest_") + name + "_" + std::to_string(hyg_ctr++);
        (*name_map)[name] = fresh;
        g_macro_rest_param_hygiene_total.fetch_add(1, std::memory_order_relaxed);
        return target_pool.intern(fresh);
    };

    // Issue #120: pre-scan the body to populate name_map BEFORE cloning.
    // The body may reference gensym'd bindings (e.g., `(let ((tmp a)) (set! b tmp))`
    // — the inner `tmp` Variable reference must see the gensym'd name
    // when it's cloned). Without the pre-scan, the recursive clone
    // would process the inner `tmp` (as a Variable reference) before the
    // let binding is processed (which is what gensym's `tmp`).
    //
    // Issue #2018: Lambda / MacroDef with dotted rest — last param is a
    // rest binding; gensym via rename_rest_binding_pre (`__rest_` prefix).
    if (name_map) {
        std::function<void(NodeId)> pre_scan = [&](NodeId nid) {
            if (nid == NULL_NODE || nid >= source.size())
                return;
            auto nv = source.get(nid);
            // If this node is a binding position, gensym its name
            // (into the name_map) but don't generate any target node.
            if (nv.tag == NodeTag::Let || nv.tag == NodeTag::LetRec || nv.tag == NodeTag::Define) {
                rename_binding_pre(nv.sym_id);
            } else if (nv.tag == NodeTag::Lambda) {
                const bool dotted = nv.int_value != 0;
                const auto nparams = nv.params.size();
                for (std::size_t i = 0; i < nparams; ++i) {
                    if (dotted && i + 1 == nparams)
                        rename_rest_binding_pre(nv.params[i]);
                    else
                        rename_binding_pre(nv.params[i]);
                }
            } else if (nv.tag == NodeTag::MacroDef) {
                // Nested macro defs inside a template: rest bit is bit 0 of
                // int_value (same encoding as add_macrodef).
                const bool dotted = (nv.int_value & 1) != 0;
                const auto nparams = nv.params.size();
                for (std::size_t i = 0; i < nparams; ++i) {
                    if (dotted && i + 1 == nparams)
                        rename_rest_binding_pre(nv.params[i]);
                    else
                        rename_binding_pre(nv.params[i]);
                }
            }
            std::vector<aura::ast::NodeId> scan_children(nv.children.begin(), nv.children.end());
            for (auto c : scan_children)
                pre_scan(c);
        };
        pre_scan(body_id);
    }

    auto rename_binding = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        // Prefer pre-scan / rest renames first so template-introduced
        // rest bindings (name_map) win over a same-named macro param.
        auto it = name_map->find(name);
        if (it != name_map->end())
            return target_pool.intern(it->second);
        // Macro params, builtins keep their name (free uses → Variable subst)
        if ((subst && subst->count(name)) || detail::hygiene_builtins().count(name))
            return transplant(sid);
        // Gensym! Create fresh name and track in name_map
        auto fresh = std::string("__") + name + "_" + std::to_string(hyg_ctr++);
        (*name_map)[name] = fresh;
        return target_pool.intern(fresh);
    };

    // Clone children recursively (pass cloned_marker through).
    // Issue #483: snapshot child NodeIds before recursion — recursive
    // clone / set_child on either flat can replace a parent's
    // PersistentChildVector and invalidate v.children spans.
    std::vector<aura::ast::NodeId> child_ids;
    std::vector<aura::ast::NodeId> source_children;
    {
        auto fresh = source.get(body_id);
        source_children.assign(fresh.children.begin(), fresh.children.end());
    }
    for (auto cid : source_children) {
        // Issue #365: bump the depth counter on recursive calls
        // so we can detect pathologically deep nesting. The
        // counter is decremented after the recursive call returns
        // (RAII pattern via ++/-- in the for-loop body) so each
        // sibling sees the same depth level.
        ++s_hygiene_depth;
        child_ids.push_back(clone_macro_body(target, target_pool, source, source_pool, cid, subst,
                                             name_map, cloned_marker));
        --s_hygiene_depth;
    }

    // Clone params (for Lambda nodes) — with hygienic renaming.
    // Issue #2018: rest (dotted last) already mapped via pre-scan
    // rename_rest_binding_pre; rename_binding prefers name_map.
    std::vector<aura::ast::SymId> param_syms;
    for (auto pid : v.params)
        param_syms.push_back(rename_binding(pid));

    aura::ast::NodeId new_id = NULL_NODE;
    switch (v.tag) {
        case NodeTag::LiteralInt:
            new_id = target.add_literal(v.int_value);
            break;
        case NodeTag::LiteralString:
            new_id = target.add_literalstring(transplant(v.sym_id));
            break;
        case NodeTag::Variable: {
            // Hygienic: check name_map for renamed bindings
            if (name_map) {
                auto name = resolve_name(v.sym_id);
                new_id = target.add_variable(target_pool.intern(name));
            } else {
                new_id = target.add_variable(transplant(v.sym_id));
            }
            break;
        }
        case NodeTag::Call: {
            std::vector<aura::ast::NodeId> args(child_ids.begin() + 1, child_ids.end());
            if (!child_ids.empty())
                new_id = target.add_call(child_ids[0], args);
            break;
        }
        case NodeTag::IfExpr:
            if (child_ids.size() >= 3)
                new_id = target.add_if(child_ids[0], child_ids[1], child_ids[2]);
            break;
        case NodeTag::Lambda:
            // Issue #2018: preserve dotted rest flag (int_value != 0).
            if (!child_ids.empty())
                new_id = target.add_lambda(param_syms, child_ids[0],
                                           /*dotted=*/v.int_value != 0);
            break;
        case NodeTag::Let:
        case NodeTag::LetRec:
            if (child_ids.size() >= 2)
                new_id =
                    (v.tag == NodeTag::Let)
                        ? target.add_let(rename_binding(v.sym_id), child_ids[0], child_ids[1])
                        : target.add_letrec(rename_binding(v.sym_id), child_ids[0], child_ids[1]);
            break;
        case NodeTag::Begin:
            if (!child_ids.empty())
                new_id = target.add_begin(child_ids);
            break;
        case NodeTag::Set:
            if (!child_ids.empty()) {
                // Issue #120: if the set! target is a macro param, look
                // up the arg and use ITS sym_id (resolved from target).
                // Otherwise the set! would target the macro param
                // (e.g., "a") which isn't bound in the calling env.
                SymId set_name_sid = transplant(v.sym_id);
                if (subst) {
                    auto set_name = std::string(source_pool.resolve(v.sym_id));
                    auto sit = subst->find(set_name);
                    if (sit != subst->end()) {
                        auto arg_v = target.get(sit->second);
                        if (arg_v.tag == NodeTag::Variable) {
                            set_name_sid = arg_v.sym_id;
                        }
                    }
                }
                new_id = target.add_set(set_name_sid, child_ids[0]);
            }
            break;
        case NodeTag::Quote:
            if (!child_ids.empty())
                new_id = target.add_quote(child_ids[0]);
            break;
        case NodeTag::Define:
            if (!child_ids.empty())
                new_id = target.add_define(rename_binding(v.sym_id), child_ids[0]);
            break;
        default:
            break;
    }

    if (new_id != NULL_NODE) {
        // Issue #190: use the caller's specified marker
        // (MacroIntroduced for macro expansion, User for closure
        // materialization). The recursive calls already set the
        // marker on each child node, so this is just the outer
        // wrapper node.

        // Issue #1908: force repin on MacroIntroduced clone (per #1908 AC).
        // The bridge hook routes through the active Evaluator (ev_ptr fallback
        // for module-aware call sites) and bumps both per-CompilerMetrics counters
        // + the file-level atomic fallback (covers module-unaware call sites
        // like clone_macro_body). This is the "强制 repin 在 MacroIntroduced 路径"
        // half of the #1908 improvement pseudocode.
        if (cloned_marker == aura::ast::SyntaxMarker::MacroIntroduced) {
            (void)aura_macro_provenance_repin_on_steal(nullptr,
                                                       static_cast<std::uint64_t>(cloned_marker));
        }
        target.set_marker(new_id, cloned_marker);
        target.set_loc(new_id, v.line, v.col);
        // Issue #390: populate the per-node schema
        // cache. Pre-#390 the type checker had to
        // re-infer the type of every macro-cloned
        // node from scratch (the cloned body had
        // no pre-computed type). Post-#390 we copy
        // the source node's schema_cache (or
        // type_id_ as a fallback) into the cloned
        // node's schema_cache column, so the type
        // checker can use it as a cache hit signal
        // and avoid the re-inference. The
        // (compile:schema-cache-stats) Aura
        // primitive reports the hit rate.
        if (source.schema_cache(body_id) != 0) {
            target.set_schema_cache(new_id, source.schema_cache(body_id));
        } else if (source.type_id(body_id) != 0) {
            target.set_schema_cache(new_id, source.type_id(body_id));
        }
        // Issue #290: also OR kMacroExpansion into the
        // macro_dirty_ bitmask on every node in the cloned
        // subtree (root + descendants). Single hook point for
        // ALL clone_macro_body callers (eval_flat top-level,
        // expand_inner_macros for nested, evaluator_eval_flat
        // closure materialization). We condition on
        // cloned_marker == MacroIntroduced so the
        // closure-materialization call site (which passes
        // User) doesn't accidentally trip the dirty bit.
        // Iterative walk via std::vector stack — no
        // recursion, safe for pathological depth.
        //
        // Issue #1891: also stamp expansion provenance at clone
        // time (not only fiber restamp #1612) so AST→IR lowering
        // sees non-zero provenance for MacroIntroduced nodes and
        // blame / JIT hygiene can correlate back to the expansion
        // origin without waiting for steal/GC repair.
        if (cloned_marker == aura::ast::SyntaxMarker::MacroIntroduced) {
            if (aura_evaluator_mutation_boundary_depth() > 0)
                aura_evaluator_bump_macro_expand_checkpoint_save();
            // Issue #1245 Phase 1: fiber-aware hygiene provenance counter for
            // concurrent clone_macro_body (steal/GC/hot-swap contexts).
            // Full dirty-to-fiber peel follows; metric makes the path visible.
            const auto fiber_id = aura_fiber_current_id();
            if (fiber_id != 0)
                g_macro_clone_concurrent_fiber_total.fetch_add(1, std::memory_order_relaxed);
            g_macro_clone_hygiene_dirty_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #1248: hygiene tracer expansion count (MacroIntroduced stamps).
            g_hygiene_tracer_expansions.fetch_add(1, std::memory_order_relaxed);
            (void)fiber_id;
            // Prefer source body provenance; else weak-link to source body id
            // (same weak pattern as fiber restamp #1612 / #1891).
            const std::uint32_t src_prov = source.provenance(body_id);
            const std::uint32_t origin =
                src_prov != 0 ? src_prov : static_cast<std::uint32_t>(body_id == 0 ? 1 : body_id);
            std::vector<aura::ast::NodeId> stack;
            stack.push_back(new_id);
            while (!stack.empty()) {
                auto cur = stack.back();
                stack.pop_back();
                if (cur == aura::ast::NULL_NODE)
                    continue;
                target.apply_macro_dirty_bits(
                    cur, static_cast<std::uint8_t>(
                             aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion));
                if (target.provenance(cur) == 0)
                    target.set_provenance(cur, origin);
                auto cv = target.get(cur);
                std::vector<aura::ast::NodeId> walk_children(cv.children.begin(),
                                                             cv.children.end());
                for (auto child : walk_children) {
                    if (child != aura::ast::NULL_NODE)
                        stack.push_back(child);
                }
            }
        }
    }
    return new_id;
}

namespace detail {

    aura::ast::NodeId unwrap_cons_chain_to_call(
        aura::ast::FlatAST* flat, aura::ast::StringPool* pool, aura::ast::NodeId root,
        const std::unordered_map<std::string, MacroExpansionDef, aura::core::TransparentStringHash,
                                 std::equal_to<>>& macros) {
        using namespace aura::ast;
        if (root == NULL_NODE)
            return NULL_NODE;
        auto v = flat->get(root);
        if (v.tag != NodeTag::Call || v.children.size() != 3)
            return NULL_NODE;
        auto callee_v = flat->get(v.child(0));
        if (callee_v.tag != NodeTag::Variable)
            return NULL_NODE;
        auto callee_name = std::string(pool->resolve(callee_v.sym_id));
        if (callee_name != "cons")
            return NULL_NODE;
        // First arg must be (quote <known-macro-sym>)
        auto arg0_v = flat->get(v.child(1));
        if (arg0_v.tag != NodeTag::Quote || arg0_v.children.empty())
            return NULL_NODE;
        auto quoted_v = flat->get(arg0_v.child(0));
        if (quoted_v.tag != NodeTag::Variable)
            return NULL_NODE;
        auto quoted_name = std::string(pool->resolve(quoted_v.sym_id));
        if (macros.find(quoted_name) == macros.end())
            return NULL_NODE;
        // Walk the cdr chain (v.child(2)) to collect arg NodeIds.
        // Each step: cdr is (cons <arg> <rest>) or (quote ()).
        std::vector<NodeId> args;
        NodeId cdr_id = v.child(2);
        while (cdr_id != NULL_NODE) {
            auto cdr_v = flat->get(cdr_id);
            if (cdr_v.tag == NodeTag::Quote) {
                // (quote ()) — end of list
                break;
            }
            if (cdr_v.tag != NodeTag::Call || cdr_v.children.size() != 3) {
                // Not a cons cell — bail
                return NULL_NODE;
            }
            auto c_callee = flat->get(cdr_v.child(0));
            if (c_callee.tag != NodeTag::Variable ||
                std::string(pool->resolve(c_callee.sym_id)) != "cons") {
                return NULL_NODE;
            }
            // Push the arg (cdr_v.child(1))
            args.push_back(cdr_v.child(1));
            cdr_id = cdr_v.child(2);
        }
        // Build Call(<quoted_name>, args...)
        auto macro_var = flat->add_variable(pool->intern(quoted_name));
        flat->set_marker(macro_var, SyntaxMarker::MacroIntroduced);
        return flat->add_call(macro_var, args);
    }

} // namespace detail

aura::ast::NodeId expand_inner_macros(
    aura::ast::FlatAST* flat, aura::ast::StringPool* pool, aura::ast::NodeId root, int depth,
    int max_depth,
    const std::unordered_map<std::string, MacroExpansionDef, aura::core::TransparentStringHash,
                             std::equal_to<>>& macros) {
    using namespace aura::ast;
    if (root == NULL_NODE || depth >= max_depth)
        return root;
    // Issue #158: unwrap qq-built cons chains whose head is a
    // known macro. Without this, `(bar ,x)` inside a macro body
    // stays as `(cons (quote bar) ...)` after expand_qq, and the
    // main macro check below (which expects a Call head matching
    // a known macro) misses it.
    if (auto unwrapped = detail::unwrap_cons_chain_to_call(flat, pool, root, macros);
        unwrapped != NULL_NODE) {
        // Substitute the unwrapped Call for the original cons chain
        // at the parent's child slot, then recurse.
        auto parent_id = flat->parent_of(root);
        if (parent_id != NULL_NODE) {
            auto parent_v = flat->get(parent_id);
            std::vector<aura::ast::NodeId> parent_children(parent_v.children.begin(),
                                                           parent_v.children.end());
            for (std::uint32_t ci = 0; ci < parent_children.size(); ++ci) {
                if (parent_children[ci] == root) {
                    flat->set_child(parent_id, ci, unwrapped);
                    flat->restamp_all_node_generations();
                    break;
                }
            }
        }
        // Recurse into the unwrapped Call (which is now a real
        // macro call site).
        return expand_inner_macros(flat, pool, unwrapped, depth, max_depth, macros);
    }
    {
        auto v = flat->get(root);
        if (v.tag == NodeTag::Call && !v.children.empty()) {
            std::vector<aura::ast::NodeId> call_args(v.children.begin(), v.children.end());
            auto callee_v = flat->get(call_args[0]);
            if (callee_v.tag == NodeTag::Variable) {
                auto cname = std::string(pool->resolve(callee_v.sym_id));
                auto it = macros.find(cname);
                if (it != macros.end()) {
                    // Build substitution: macro param → arg NodeId.
                    // Issue #146 follow-up: route through the pure helper
                    // so the substitution logic lives in evaluator_pure.ixx
                    // (single source of truth) and the legacy inline loop
                    // goes away. call_args is snapshotted above (Issue #483)
                    // so set_child during clone/expand cannot UAF v.children.
                    const auto& md = it->second;
                    auto subst = aura::compiler::pure::compute_macro_subst_pure(
                        md.params, call_args, md.dotted);
                    // Issue #2018: rest params on inner macros — build
                    // (list remaining...) into subst (same as eval_flat
                    // hygienic path).
                    if (md.dotted && !md.params.empty()) {
                        const std::size_t regular_count = md.params.size() - 1;
                        std::vector<aura::ast::NodeId> remaining;
                        for (std::size_t ai = regular_count + 1; ai < call_args.size(); ++ai)
                            remaining.push_back(call_args[ai]);
                        auto list_var = flat->add_variable(pool->intern("list"));
                        auto list_call = flat->add_call(list_var, remaining);
                        subst[md.params.back()] = list_call;
                    }
                    // Clone the macro body into the current flat and
                    // re-intern sym_ids. Use the runtime registry's
                    // `flat` / `pool` pointers as the source.
                    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                       std::equal_to<>>
                        rename_map;
                    auto* src_pool = md.pool ? md.pool : pool;
                    auto cloned = clone_macro_body(*flat, *pool, *md.flat, *src_pool, md.body_id,
                                                   &subst, &rename_map);
                    if (cloned == NULL_NODE)
                        return root;
                    // Recursively expand inner macros in the cloned body
                    cloned = expand_inner_macros(flat, pool, cloned, depth + 1, max_depth, macros);
                    // Rewrite the parent's child to use the cloned body
                    auto parent_id = flat->parent_of(root);
                    if (parent_id != NULL_NODE) {
                        auto parent_v = flat->get(parent_id);
                        std::vector<aura::ast::NodeId> parent_children(parent_v.children.begin(),
                                                                       parent_v.children.end());
                        for (std::uint32_t ci = 0; ci < parent_children.size(); ++ci) {
                            if (parent_children[ci] == root) {
                                flat->set_child(parent_id, ci, cloned);
                                // Issue #2019: set_child bumps generation_;
                                // restamp MacroIntroduced so cloned body
                                // matches surrounding AST gen.
                                restamp_after_expand(*flat);
                                break;
                            }
                        }
                    } else {
                        // Root-level expand: still restamp MacroIntroduced.
                        restamp_after_expand(*flat);
                    }
                    return cloned;
                }
            }
        }
    }
    // Not a macro call — recurse into children.
    // Issue #483: snapshot child ids; recursive set_child on this
    // node (or descendants) replaces PersistentChildVector storage.
    std::vector<aura::ast::NodeId> child_ids;
    {
        auto rv = flat->get(root);
        child_ids.assign(rv.children.begin(), rv.children.end());
    }
    for (auto child : child_ids)
        (void)expand_inner_macros(flat, pool, child, depth + 1, max_depth, macros);
    return root;
}
aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                   aura::ast::NodeId root, int max_passes) {
    using namespace aura::ast;
    // Issue #2019: track whether any pass expanded so we restamp
    // MacroIntroduced gens once before return (FlatAST consistency).
    bool any_expand = false;
    for (int pass = 0; pass < max_passes; ++pass) {
        // Phase 1: collect macro definitions
        struct MD {
            aura::ast::FlatAST* src_flat;
            aura::ast::StringPool* src_pool;
            std::vector<std::string> params;
            NodeId body_id;
            bool dotted;
            bool hygienic;  // Issue #120
            bool preserved; // Issue #230 #2
        };
        std::unordered_map<std::string, MD, aura::core::TransparentStringHash, std::equal_to<>>
            local_macros;
        bool has_macro_def = false;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::MacroDef) {
                has_macro_def = true;
                // Macro name is in sym_id; params follow
                auto macro_name = std::string(pool.resolve(v.sym_id));
                std::vector<std::string> params;
                for (auto pid : v.params)
                    params.push_back(std::string(pool.resolve(pid)));
                auto body_id = v.children.empty() ? NULL_NODE : v.child(0);
                // Issue #120: dotted is bit 0, hygienic is bit 1
                bool is_dotted = (v.int_value & 1) != 0;
                bool is_hygienic = (v.int_value & 2) != 0;
                bool is_preserved = (v.int_value & 4) != 0;
                local_macros[macro_name] = MD{&flat,     &pool,       std::move(params), body_id,
                                              is_dotted, is_hygienic, is_preserved};
            }
        }

        if (!has_macro_def) {
            // No more macro defs — final restamp if we expanded earlier.
            if (any_expand)
                restamp_after_expand(flat);
            return root;
        }

        // Phase 2: find and expand macro calls
        bool expanded_any = false;
        NodeId new_root = root;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                std::vector<aura::ast::NodeId> call_args(v.children.begin(), v.children.end());
                auto callee_v = flat.get(call_args[0]);
                if (callee_v.tag == NodeTag::Variable) {
                    auto cname = std::string(pool.resolve(callee_v.sym_id));
                    auto it = local_macros.find(cname);
                    if (it != local_macros.end()) {
                        // Build substitution: macro param → arg expression.
                        // Issue #146 follow-up: route through the pure
                        // helper. Rest-param handling stays here because
                        // it requires FlatAST mutation (allocating a
                        // pair-list) — that's stateful, not pure.
                        auto& md = it->second;
                        auto subst = aura::compiler::pure::compute_macro_subst_pure(
                            md.params, call_args, md.dotted);
                        // Issue #2018: rest param → (list remaining...) in subst
                        // so clone_macro_body substitutes free rest uses.
                        if (md.dotted && !md.params.empty()) {
                            const std::size_t regular_count = md.params.size() - 1;
                            std::vector<aura::ast::NodeId> remaining;
                            for (std::size_t ai = regular_count + 1; ai < call_args.size(); ++ai)
                                remaining.push_back(call_args[ai]);
                            auto list_var = flat.add_variable(pool.intern("list"));
                            auto list_call = flat.add_call(list_var, remaining);
                            subst[md.params.back()] = list_call;
                        }
                        // Clone macro body with substitution
                        std::unordered_map<std::string, std::string,
                                           aura::core::TransparentStringHash, std::equal_to<>>
                            rename_map;
                        auto expanded = clone_macro_body(
                            flat, pool, *md.src_flat, *md.src_pool, md.body_id, &subst, &rename_map,
                            /*cloned_marker=*/aura::ast::SyntaxMarker::MacroIntroduced);
                        if (expanded != NULL_NODE) {
                            if (id == root)
                                new_root = expanded;
                            expanded_any = true;
                        }
                    }
                }
            }
        }

        if (!expanded_any) {
            if (any_expand)
                restamp_after_expand(flat);
            return root;
        }
        any_expand = true;
        root = new_root;
    }
    // Issue #121: hit the pass limit with macros still in the
    // tree. Emit a warning so the user knows the result is
    // partial. This is the user-facing equivalent of the
    // solver TIMEOUT pattern from Issue #118.
    if (root != NULL_NODE) {
        std::println(std::cerr,
                     "warning: macro_expand_all hit pass limit ({}); "
                     "the result may have unexpanded macro calls",
                     max_passes);
    }
    // Issue #2019: restamp after multi-pass expand (pass-limit exit).
    if (any_expand)
        restamp_after_expand(flat);
    return root;
}
} // namespace aura::compiler::macro_exp
