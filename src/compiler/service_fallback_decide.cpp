// service_fallback_decide.cpp - CompilerService tree-walker fallback
// decisions moved out of the module interface (#5242).
// Move-only: the bodies are unchanged.

module;

#include "core/ownership.hh"           // immortal<T>
#include "compiler/pipeline_policy.hh" // tree_walker_fallback_policy

module aura.compiler.service;

import std;

namespace aura::compiler {

bool CompilerService::needs_tree_walker_fallback(const aura::ast::FlatAST& flat,
                                                 const aura::ast::StringPool& pool,
                                                 aura::ast::NodeId root) const {
    // Issue #402 observability: bump the call counter
    // first so callers see every invocation (incl. early
    // returns for NULL_NODE / out-of-range root).
    metrics_.needs_tree_walker_fallback_calls.fetch_add(1, std::memory_order_relaxed);
    if (root == aura::ast::NULL_NODE || root >= flat.size())
        return false;
    // Whole-program Begin root (multi-form --load / pasted buffer):
    // forms are evaluated sequentially by the IR pipeline and each
    // top-level define registers as it evaluates — but nothing is
    // registered at gate time. Two hazards made such scripts
    // hard-error under the strict gate:
    //   (a) the slow-path scan walks the entire flat, including
    //       ORPHANED forms already pre-executed and stripped from the
    //       root by pre_exec_requires (e.g. (require "x" all:) leaves
    //       its trailing-colon keyword arg orphaned in the flat →
    //       bogus "unknown variable" all:);
    //   (b) free variables / calls referencing earlier sibling forms
    //       look unknown because those defines have not run yet.
    // Simulate the sequential order instead: direct-child define
    // names are known to every scanned sibling; each define's own
    // body keeps its existing define-path validation (checked with
    // real registered state during sequential evaluation).
    // Scope: the known-sim precision only reshapes the NON-Allow
    // decision. The Allow (legacy) face keeps the legacy whole-flat
    // routing below — "anything unrecognized → silent TakeWalker" is
    // a pinned contract there (#2577 heap tests, asan-verify 937d53d22
    // diagnostics face).
    if (flat.get(root).tag == aura::ast::NodeTag::Begin &&
        tree_walker_fallback_policy() != TreeWalkerFallbackPolicy::Allow) {
        const auto begin_v = flat.get(root);
        std::unordered_set<std::string> begin_defined;
        for (auto cid : begin_v.children) {
            if (cid >= flat.size())
                continue;
            const auto cv = flat.get(cid);
            if (cv.tag == aura::ast::NodeTag::Define && !cv.children.empty())
                begin_defined.insert(std::string(pool.resolve(cv.sym_id)));
        }
        for (auto cid : begin_v.children) {
            if (cid >= flat.size())
                continue;
            const auto cv = flat.get(cid);
            if (cv.tag == aura::ast::NodeTag::Define && !cv.children.empty()) {
                // Define bodies are lambda-aware checked here too: a body
                // that needs the walker (while, fiber:*, unknown refs) must
                // trip the gate so the disposition selects TakeWalker
                // (Allow) / HardError (Forbidden). Skipping defines entirely
                // silently IR-cached non-IR-safe bodies (937d53d22: while loop
                // never ran → growth=0 vacuous PASS).
                const auto dbody = cv.child(0);
                const auto dname = std::string(pool.resolve(cv.sym_id));
                if (define_body_needs_tree_walker_fallback(flat, pool, dbody, dname,
                                                           &begin_defined))
                    return true;
                continue;
            }
            if (subtree_needs_tree_walker_fallback(flat, pool, cid, &begin_defined))
                return true;
        }
        return false;
    }
    // Issue #657: quote-containing defines need bridge/fallback refresh.
    bool flat_has_quote = false;
    for (aura::ast::NodeId qid = 0; qid < flat.size(); ++qid) {
        if (flat.get(qid).tag == aura::ast::NodeTag::Quote) {
            flat_has_quote = true;
            break;
        }
    }

    // Issue #402: FAST PATH. When summary_flags_ == 0,
    // no MacroDef / DefineType / DefineModule / Set /
    // Lambda-dotted / TypeAnnotation-var is present ANY
    // in the flat. The fast path walks only the root
    // subtree (typically 4-10 nodes for arithmetic /
    // primitive calls) instead of the entire flat, and
    // applies the same Variable / Call resolution logic
    // as the slow path. For the common case (`(+ 1 2)`,
    // `(* x 3)` with x in ir_cache_, etc.) the fast path
    // returns false without paying the O(flat.size())
    // scan cost.
    if (flat.summary_flags() == 0) {
        metrics_.needs_tree_walker_fast_path_hits.fetch_add(1, std::memory_order_relaxed);
        // Issue #402: top-level defines need a body-aware
        // check (params + function name are NOT free vars,
        // even though walk_subtree would see them as such).
        // Delegate to define_body_needs_tree_walker_fallback
        // when root is a Define so the slow path's logic
        // is reused verbatim — same answer for the same AST.
        auto fast_root_v = flat.get(root);
        if (fast_root_v.tag == aura::ast::NodeTag::Define && !fast_root_v.children.empty()) {
            auto fast_body_id = fast_root_v.child(0);
            if (fast_body_id < flat.size()) {
                auto fast_name = std::string(pool.resolve(fast_root_v.sym_id));
                const bool needs =
                    define_body_needs_tree_walker_fallback(flat, pool, fast_body_id, fast_name);
                if (flat_has_quote && needs)
                    metrics_.compiler_core_quote_fallback_refresh_total.fetch_add(
                        1, std::memory_order_relaxed);
                return needs;
            }
        }
        const bool needs = subtree_needs_tree_walker_fallback(flat, pool, root);
        if (flat_has_quote && needs)
            metrics_.compiler_core_quote_fallback_refresh_total.fetch_add(
                1, std::memory_order_relaxed);
        return needs;
    }

    // Issue #402: SLOW PATH. summary_flags_ != 0 means
    // at least one fallback-relevant tag is present
    // somewhere in the flat. Fall back to the original
    // O(flat.size()) scan, which has the same logic as
    // before #402.
    metrics_.needs_tree_walker_slow_path_hits.fetch_add(1, std::memory_order_relaxed);

    // Issue #272: top-level defines are IR-native when the body only
    // references params, self, primitives, cached functions, or IR value cells.
    auto root_v = flat.get(root);
    if (root_v.tag == aura::ast::NodeTag::Define && !root_v.children.empty()) {
        auto body_id = root_v.child(0);
        if (body_id < flat.size()) {
            auto name_str = std::string(pool.resolve(root_v.sym_id));
            return define_body_needs_tree_walker_fallback(flat, pool, body_id, name_str);
        }
    }

    // ── Known names that must go through tree-walker ───────────────
    // Includes: EDSL primitives, special forms, module system operations.
    // Intentionally leaked (no destructor): the buckets of a function-local
    // static set were observed freed mid-run by an OrchSchedHolder
    // worker thread (ASAN heap-use-after-free in the visitor count()
    // — obs_facade rc=139 CI red).
    static const aura::core::immortal<std::unordered_set<std::string>> tree_walker_only{
        std::unordered_set<std::string>{
            // EDSL / AI agent primitives
            "define-type",
            "set-code",
            "eval-current",
            "apply",
            "typecheck-current",
            "typed-mutate",
            "typed-mutate-atomic", // Issue #1442 / #1408 EDSL atomic multi-mutate
            "rollback",
            "mutation-log",
            "query-mutation-log",
            "qr:ir-dirty?",
            "qr:ir-dependents",
            // Mutation primitives (issue #165/#166): tree-walker so the
            // returned bool/Int propagates correctly. The IR pipeline
            // either silently drops unknown primitives or wraps them as
            // ConstI64 0, breaking the success checks in
            // test_issue_165/166 (e.g. "mutate > 0").
            "mutate:rebind",
            "mutate:set-body",
            "mutate:remove-node",
            "mutate:move-node",
            "intend",
            "fiber:spawn",
            "fiber:join",
            "mw:define-strategy",
            "register-strategy!",
            "intend-history",
            "intend-analytics",
            "strategy-field",
            "strategy-set-field!",
            "strategy-inspect",
            "coverage-report",
            // C FFI — tree-walker needed to dispatch closure calls
            "c-func",
            "c-load",
            // Messaging primitives (tree-walker for argument passing)
            "send",
            "recv",
            "my-id",
            "json-encode",
            "json-get-string",
            "json-parse",
            // Scheduler metrics (Issue #32)
            "orch:metrics",
            "orch:reset-metrics",
            // Special forms not in IR
            "when",
            "set!",
            "load",
            "equal?",
            "unless",
            "export",
            "and",
            "or",
            "cond",
            "case",
            // Capability special forms
            "with-capability",
            "check-capability",
            "capability-stack", // DEPRECATED — uses primitive path instead
            // Module system (env side-effects)
            // "import", "use", "require" — now in lowering_known
        }};

    // Root-level bare variables (like `pi`, `sort`) may come from runtime imports.
    // The lowering doesn't know about them, so fallback to tree-walker.
    // Issue #1284: ir_cache_v2_ define cache also counts as known.
    if (flat.get(root).tag == aura::ast::NodeTag::Variable) {
        auto root_name = pool.resolve(flat.get(root).sym_id);
        if (evaluator_.primitives().slot_for_name(std::string(root_name)) >=
                evaluator_.primitives().slot_count() &&
            !name_in_ir_define_cache(std::string(root_name)))
            return true;
    }

    // Type annotations: if storing a variable name (3-arg form),
    // tree-walker handles variable binding; IR/JIT cannot.
    if (flat.get(root).tag == aura::ast::NodeTag::TypeAnnotation) {
        auto root_v = flat.get(root);
        if (!root_v.children.empty()) {
            if (root_v.int_value != 0 ||
                flat.get(root_v.child(0)).tag == aura::ast::NodeTag::Variable)
                return true;
        }
    }

    // Names that lowering explicitly handles (special forms lowered to IR)
    // These should NOT trigger tree-walker fallback even though they're
    // not primitives or cached defines.
    // Intentionally leaked (no destructor): the buckets of a function-local
    // static set were observed freed mid-run by an OrchSchedHolder
    // worker thread (ASAN heap-use-after-free in the visitor count()
    // — obs_facade rc=139 CI red).
    static const aura::core::immortal<std::unordered_set<std::string>> lowering_known{
        std::unordered_set<std::string>{
            "try",
            "catch",
            "raise",
            "require",
            "import",
            "use",
            "with-arena",
            "performance-region",
            "evolution-region",
        }};

    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        auto nv = flat.get(id);

        // MacroDef cannot be lowered to IR
        if (nv.tag == aura::ast::NodeTag::MacroDef)
            return true;
        if (nv.tag == aura::ast::NodeTag::DefineType)
            return true;
        if (nv.tag == aura::ast::NodeTag::DefineModule)
            return true;

        // Dotted rest lambda cannot be lowered to IR (rest param is
        // lowered as single Arg slot, not as pair list)
        if (nv.tag == aura::ast::NodeTag::Lambda && nv.int_value != 0)
            return true;

        // TypeAnnotation with var_annot (3-arg form (: name Type val)):
        // tree-walker handles variable binding; IR/JIT cannot.
        if (nv.tag == aura::ast::NodeTag::TypeAnnotation && nv.int_value != 0)
            return true;

        // General variable references to user-defined top-level values
        // (from (define name val) where val is not a lambda) need tree-walker
        // because IR lowering can't resolve them (they live in evaluator's env).
        // Lambda params and let-bound vars are NOT in user_bindings_, so they
        // won't trigger fallback.
        // Keyword variables (:foo) are self-evaluating — need tree-walker
        if (nv.tag == aura::ast::NodeTag::Variable) {
            auto var_name = pool.resolve(nv.sym_id);
            if (!var_name.empty() && var_name[0] == ':')
                return true;
            if (user_bindings_.count(std::string(var_name))) {
                // Issue #1284: also accept ir_cache_v2_ define hits.
                if (!name_in_ir_define_cache(std::string(var_name)))
                    return true;
            }
            auto vn = std::string(var_name);
            if (ir_value_cell_bindings_.count(vn))
                continue;
            if (!vn.empty() &&
                evaluator_.primitives().slot_for_name(vn) >= evaluator_.primitives().slot_count() &&
                !name_in_ir_define_cache(vn) && !lowering_known->count(vn)) {
                return true;
            }
        }

        // Set nodes (from set!) must go through tree-walker because they
        // mutate cells in the evaluator's cell heap, not the IR's local heap.
        if (nv.tag == aura::ast::NodeTag::Set)
            return true;

        if (nv.tag == aura::ast::NodeTag::Call) {
            auto callee = nv.child(0);
            if (callee != aura::ast::NULL_NODE && callee < flat.size()) {
                auto callee_v = flat.get(callee);
                if (callee_v.tag == aura::ast::NodeTag::Variable) {
                    auto name = std::string(pool.resolve(callee_v.sym_id));

                    // Names starting with query: / mutate:[ ] trigger AST server fallback
                    if (name.starts_with("query:") || name.starts_with("mutate:"))
                        return true;

                    // Known tree-walker-only names (EDSL, special forms, module)
                    if (tree_walker_only->count(name))
                        return true;

                    // Catch binding forms like (catch (e) handler) have the
                    // variable binding (e) as a Call node with callee="e".
                    // These should not trigger fallback — the try lowering
                    // handles them explicitly. Skip fallback check for nodes
                    // whose parent is a catch form.
                    if (name == "catch")
                        continue;

                    // Call callee that's not a known primitive or cached define
                    // may come from a runtime import — fallback to tree-walker.
                    // Issue #1284: ir_cache_v2_ counts as a define-cache hit.
                    if (evaluator_.primitives().slot_for_name(name) >=
                            evaluator_.primitives().slot_count() &&
                        !name_in_ir_define_cache(name) && !lowering_known->count(name)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool CompilerService::subtree_needs_tree_walker_fallback(
    const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, aura::ast::NodeId root,
    const std::unordered_set<std::string>* extra_known) const {
    if (root == aura::ast::NULL_NODE || root >= flat.size())
        return false;

    // Reuse the same known-names sets as the slow path.
    // They're already lazily-initialized static — no
    // allocation per call.
    // Intentionally leaked (no destructor): the buckets of a function-local
    // static set were observed freed mid-run by an OrchSchedHolder
    // worker thread (ASAN heap-use-after-free in the visitor count()
    // — obs_facade rc=139 CI red).
    static const aura::core::immortal<std::unordered_set<std::string>> tree_walker_only{
        std::unordered_set<std::string>{
            // EDSL / AI agent primitives
            "define-type",
            "set-code",
            "eval-current",
            "apply",
            "typecheck-current",
            "typed-mutate",
            "typed-mutate-atomic", // Issue #1442
            "rollback",
            "mutation-log",
            "query-mutation-log",
            "qr:ir-dirty?",
            "qr:ir-dependents",
            "mutate:rebind",
            "mutate:set-body",
            "mutate:remove-node",
            "mutate:move-node",
            "intend",
            "fiber:spawn",
            "fiber:join",
            "mw:define-strategy",
            "register-strategy!",
            "intend-history",
            "intend-analytics",
            "strategy-field",
            "strategy-set-field!",
            "strategy-inspect",
            "coverage-report",
            "c-func",
            "c-load",
            "send",
            "recv",
            "my-id",
            "json-encode",
            "json-get-string",
            "json-parse",
            "orch:metrics",
            "orch:reset-metrics",
            "when",
            "set!",
            "load",
            "equal?",
            "unless",
            "export",
            "and",
            "or",
            "cond",
            "case",
            "with-capability",
            "check-capability",
            "capability-stack",
        }};
    // Intentionally leaked (no destructor): the buckets of a function-local
    // static set were observed freed mid-run by an OrchSchedHolder
    // worker thread (ASAN heap-use-after-free in the visitor count()
    // — obs_facade rc=139 CI red).
    static const aura::core::immortal<std::unordered_set<std::string>> lowering_known{
        std::unordered_set<std::string>{
            "try",
            "catch",
            "raise",
            "require",
            "import",
            "use",
            "with-arena",
            "performance-region",
            "evolution-region",
        }};

    bool needs = false;
    const std::size_t visited = flat.walk_subtree(root, [&](aura::ast::NodeId id) {
        if (needs)
            return; // short-circuit
        const auto v = flat.get(id);
        // Tag-only early-returns (mirrors slow path).
        switch (v.tag) {
            case aura::ast::NodeTag::MacroDef:
            case aura::ast::NodeTag::DefineType:
            case aura::ast::NodeTag::DefineModule:
                needs = true;
                return;
            case aura::ast::NodeTag::Lambda:
                if (v.int_value != 0) {
                    needs = true;
                    return;
                }
                break;
            case aura::ast::NodeTag::TypeAnnotation:
                if (v.int_value != 0) {
                    needs = true;
                    return;
                }
                break;
            case aura::ast::NodeTag::Set:
                needs = true;
                return;
            default:
                break;
        }
        if (v.tag == aura::ast::NodeTag::Variable) {
            auto var_name = pool.resolve(v.sym_id);
            if (!var_name.empty() && var_name[0] == ':') {
                needs = true;
                return;
            }
            // Whole-program Begin eval: names defined by sibling forms
            // register sequentially — treat them as known (asan-verify 937d53d22).
            if (extra_known && extra_known->count(std::string(var_name)))
                return;
            if (user_bindings_.count(std::string(var_name))) {
                // Issue #1284: ir_cache_v2_ define cache avoids fallback.
                if (!name_in_ir_define_cache(std::string(var_name))) {
                    needs = true;
                    return;
                }
            }
            auto vn = std::string(var_name);
            if (ir_value_cell_bindings_.count(vn))
                return;
            if (!vn.empty() &&
                evaluator_.primitives().slot_for_name(vn) >= evaluator_.primitives().slot_count() &&
                !name_in_ir_define_cache(vn) && !lowering_known->count(vn)) {
                needs = true;
                return;
            }
        }
        if (v.tag == aura::ast::NodeTag::Call && v.children.empty() == false) {
            auto callee = v.child(0);
            if (callee != aura::ast::NULL_NODE && callee < flat.size()) {
                const auto callee_v = flat.get(callee);
                if (callee_v.tag == aura::ast::NodeTag::Variable) {
                    auto name = std::string(pool.resolve(callee_v.sym_id));
                    if (name.starts_with("query:") || name.starts_with("mutate:")) {
                        needs = true;
                        return;
                    }
                    // Whole-program Begin eval: sibling defines register
                    // sequentially — treat them as known (asan-verify 937d53d22).
                    if (extra_known && extra_known->count(name))
                        return;
                    if (tree_walker_only->count(name)) {
                        needs = true;
                        return;
                    }
                    if (name == "catch")
                        return;
                    // Issue #1284: ir_cache_v2_ define cache hits.
                    if (evaluator_.primitives().slot_for_name(name) >=
                            evaluator_.primitives().slot_count() &&
                        !name_in_ir_define_cache(name) && !lowering_known->count(name)) {
                        needs = true;
                        return;
                    }
                }
            }
        }
    });
    // Issue #402 observability: record visited-node count
    // for the fast path. If visited < flat.size() (i.e.
    // subtree was smaller than flat), the fast path saved
    // the iteration cost.
    (void)visited;
    return needs;
}

} // namespace aura::compiler
