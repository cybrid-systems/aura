// service_eval.cpp — CompilerService::eval moved out of the module
// interface (#5242). Move-only: the body is unchanged.

module;

#include "compiler/messaging_bridge.h" // CliMainEvalBodyLock
#include "compiler/pipeline_policy.hh" // TreeWalkerFallbackDisposition
#include "core/ownership.hh"           // NoCopy
#include "core/workspace_epoch.hh"     // current_mutation_epoch

module aura.compiler.service;

import std;

namespace aura::compiler {

[[nodiscard]] EvalResult CompilerService::eval(std::string_view input) {
    // Issue #4674: CLI oneshot holds this lock in main (#4356). Tests,
    // the REPL, and --serve enter eval directly. Thread-fallback
    // fiber:spawn then runs apply_closure on a std::thread with
    // g_current_fiber still null, so the worker writes the host name
    // cache and the sym-intern vectors while this eval is still
    // walking (spawn returns before fiber:join). That races
    // std::vector::operator[] in test_misc_issue_fold_batch
    // (NameResEntry and EvalValue). fiber:join, read-line, and
    // read-byte drop the lock across their wait. A re-entrant eval,
    // or a caller that already holds the lock, gets nullptr and does
    // not lock the non-recursive mutex again. Taken before
    // eval_mutex_ so the order matches main's oneshot path, and before
    // every blocking-wait resume (the wait guards re-take M0 before
    // M1, #4711).
    wire_eval_mutex_wait_hooks_(); // Issue #4711: M1 is droppable across a wait
    aura::messaging::CliMainEvalBodyLock cli_body_lock;
    // Issue #1431 follow-up Race #3 (and Race #4 #5 ...):
    // serialize the entire eval pipeline across threads.
    // Two concurrent eval callers share arena_ (which holds
    // the upstream memory_resource) and ASTArena::create
    // races on the polymorphic_allocator, surfacing as a
    // NULL memory_resource in pmr::memory_resource::allocate
    // from StringPool::rehash's _M_fill_assign. Each earlier
    // fix (TypeRegistry mutex, FlatAST add_node mutex)
    // exposed the next race in the parser/typecheck/eval
    // pipeline — eval_mutex_ serializes the whole pipeline
    // so all three races (and likely more) are resolved at
    // once. The push_back lock inside the Evaluator
    // (alloc_storage_lock_ tested by the stress test) is
    // unaffected — it protects cells_/pairs_/string_heap_
    // writes during eval, while eval_mutex_ protects the
    // parse + typecheck setup that runs before those writes.
    //
    // recursive_mutex: Path B (workspace fn call shortcut) may
    // re-enter eval() for non-literal args (e.g. string args
    // after eval_arg_fast fails). A plain mutex deadlocks the
    // calling thread and timed out jit_late1 on (f "hi") paths.
    // Issue #4711: unique_lock + the published scope so a blocking
    // wait inside this eval (fiber:join / read-line / http-post /
    // channel park) can drop M1 and re-take it after M0. The
    // recursive depth is unwound fully, so a re-entrant eval (Path B)
    // publishes a droppable state too.
    std::unique_lock<std::recursive_mutex> lock(eval_mutex_);
    EvalMutexWaitPublishScope eval_mutex_wait_scope(eval_mutex_);
    // Issue #411: snapshot the workspace mutation_log_ size at
    // entry so the post-eval auto-incremental typecheck can
    // detect whether this eval produced a new mutation
    // (mutate:* primitive) and run infer_flat_partial on the
    // most recent record. The check is "size grew" — when it
    // didn't, the eval was a pure read (define / typecheck /
    // query) and we skip the auto-invoke.
    cs_eval_mutation_log_size_at_entry_ = 0;
    if (auto* ws = evaluator_.workspace_flat())
        cs_eval_mutation_log_size_at_entry_ = ws->all_mutations().size();

    // Fix #165/#166 tests: workspace-aware eval shortcut. When a
    // entry so the post-eval auto-incremental typecheck can
    // detect whether this eval produced a new mutation
    // (mutate:* primitive) and run infer_flat_partial on the
    // most recent record. The check is "size grew" — when it
    // didn't, the eval was a pure read (define / typecheck /
    // query) and we skip the auto-invoke.
    cs_eval_mutation_log_size_at_entry_ = 0;
    if (auto* ws = evaluator_.workspace_flat())
        cs_eval_mutation_log_size_at_entry_ = ws->all_mutations().size();

    // Issue #411: RAII guard. Fires the auto-incremental
    // typecheck on every return path (tree-walker fallback,
    // early returns from the IR pipeline, the final return).
    PostEvalAutoInvokeGuard post_eval_guard(this, cs_eval_mutation_log_size_at_entry_);

    // Fix #165/#166 tests: workspace-aware eval shortcut. When a
    // workspace is set (via (set-code ...)), short-circuit bare
    // identifiers and top-level function calls so they find their
    // definitions in workspace_flat_ instead of being parsed into a
    // fresh FlatAST where workspace bindings are invisible.
    //
    // Path A (bare identifier): eval("x") — look up x's body in
    // workspace, eval body in top_env.
    //
    // Path B (function call): eval("(f arg1 arg2 ...)") — find f's
    // define in workspace, eval each arg in top_env, build a child
    // env with param bindings, eval f's body in that env. This
    // covers the common case (test_issue_166 tests 3 and 4 — calls
    // after set-code) without forcing set-code to install workspace
    // defines into top_env_ (which has been historically avoided
    // because of env-pollution concerns across reset/cycles).
    //
    // Macros (define-hygienic-macro / defmacro) still fall through
    // to the standard pipeline below, which runs macro_expand_all
    // against the freshly parsed AST. Macro-defined bindings will
    // still be picked up via Path A and Path B at the call-site
    // level; hygiene-preserving re-expansion after mutate:rebind
    // is a separate issue (#165 macro-re-expansion).
    if (evaluator_.workspace_flat() && evaluator_.workspace_pool()) {
        std::string trimmed(input);
        while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front()))
            trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))
            trimmed.pop_back();
        bool is_bare = !trimmed.empty() && (std::isalpha((unsigned char)trimmed[0]) ||
                                            trimmed[0] == '_' || trimmed[0] == '-');
        for (char c : trimmed) {
            if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') {
                is_bare = false;
                break;
            }
        }
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (is_bare) {
            // Issue #3918: env cell is SSOT after lockless rebind.
            if (auto bound = evaluator_.top_env().lookup(trimmed)) {
                types::EvalValue v = *bound;
                if (types::is_cell(v)) {
                    auto ci = types::as_cell_id(v);
                    if (ci < evaluator_.cells().size())
                        v = evaluator_.cells()[ci];
                }
                return EvalResult(v);
            }
            // Last live Define wins: mutate:rebind may intern a new
            // SymId and take the add-path, leaving the original
            // Define earlier in the table.
            aura::ast::NodeId found_body = aura::ast::NULL_NODE;
            for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id) {
                if (ws_flat->is_free_slot(id))
                    continue;
                auto v = ws_flat->get(id);
                if (v.tag != aura::ast::NodeTag::Define)
                    continue;
                if (v.sym_id == aura::ast::INVALID_SYM)
                    continue;
                if (v.children.empty())
                    continue;
                auto name = std::string(ws_pool->resolve(v.sym_id));
                if (name != trimmed)
                    continue;
                found_body = v.child(0);
            }
            if (found_body != aura::ast::NULL_NODE)
                return evaluator_.eval_flat(*ws_flat, *ws_pool, found_body, evaluator_.top_env());
            // Not in workspace — fall through to the normal
            // pipeline (which will return an "undefined variable"
            // diagnostic just as it would without the workspace).
        } else if (!trimmed.empty() && trimmed.front() == '(') {
            // Function-call shortcut: parse "(head arg1 arg2 ...)"
            // directly to find the callee name without spinning up
            // a full FlatAST. Only handle the simple case (head is
            // a bare identifier matching a workspace define).
            auto close = trimmed.find_last_of(')');
            if (close != std::string::npos && close + 1 == trimmed.size()) {
                // Scan inside the outer parens for the head symbol.
                std::size_t i = 1;
                while (i < trimmed.size() && std::isspace((unsigned char)trimmed[i]))
                    ++i;
                std::size_t head_start = i;
                while (i < trimmed.size() && !std::isspace((unsigned char)trimmed[i]) &&
                       trimmed[i] != ')')
                    ++i;
                std::string head_name = trimmed.substr(head_start, i - head_start);
                bool head_is_bare =
                    !head_name.empty() && (std::isalpha((unsigned char)head_name[0]) ||
                                           head_name[0] == '_' || head_name[0] == '-');
                for (char c : head_name) {
                    if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') {
                        head_is_bare = false;
                        break;
                    }
                }
                if (head_is_bare) {
                    // Issue #3918: apply the env closure (lockless rebind
                    // refresh) instead of the last all-nodes Define —
                    // parse_to_flat leftovers made (f -4) identity while
                    // HO (let ((g f)) (g -4)) was abs.
                    types::EvalValue env_fn{};
                    bool have_env_fn = false;
                    if (auto bound = evaluator_.top_env().lookup(head_name)) {
                        env_fn = *bound;
                        if (types::is_cell(env_fn)) {
                            auto ci = types::as_cell_id(env_fn);
                            if (ci < evaluator_.cells().size())
                                env_fn = evaluator_.cells()[ci];
                        }
                        have_env_fn = types::is_closure(env_fn);
                    }
                    aura::ast::NodeId def_body = aura::ast::NULL_NODE;
                    std::vector<aura::ast::SymId> fn_params;
                    bool dotted = false;
                    // Always record the workspace Lambda as fallback.
                    // #3918 env-SSOT apply_closure can miss a live
                    // eval-current TW closure; fail-closed InvalidClosure
                    // then breaks (h 41) / (id 7) / (mv 3) Path B.
                    {
                        // Last live Lambda Define wins (rebind add-path /
                        // intern SymId mismatch must not keep the old body).
                        for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id) {
                            if (ws_flat->is_free_slot(id))
                                continue;
                            auto v = ws_flat->get(id);
                            if (v.tag != aura::ast::NodeTag::Define)
                                continue;
                            if (v.sym_id == aura::ast::INVALID_SYM)
                                continue;
                            if (v.children.empty())
                                continue;
                            auto wname = std::string(ws_pool->resolve(v.sym_id));
                            if (wname != head_name)
                                continue;
                            auto body_id = v.child(0);
                            if (body_id >= ws_flat->size())
                                continue;
                            auto body_v = ws_flat->get(body_id);
                            if (body_v.tag != aura::ast::NodeTag::Lambda)
                                continue;
                            def_body =
                                body_v.children.empty() ? aura::ast::NULL_NODE : body_v.child(0);
                            fn_params.assign(body_v.params.begin(), body_v.params.end());
                            // Dotted flag is bit 0 of int_value (see ast.ixx
                            // add_lambda encoding).
                            dotted = (body_v.int_value & 1) != 0;
                        }
                    }
                    if (def_body != aura::ast::NULL_NODE) {
                        // Tokenize the args between head_end and close.
                        // Simple splitter: walks parens / strings.
                        std::size_t arg_start = i;
                        std::vector<std::string> arg_strs;
                        int depth = 0;
                        bool in_str = false;
                        std::size_t tok_start = arg_start;
                        for (std::size_t k = arg_start; k < close; ++k) {
                            char ch = trimmed[k];
                            if (in_str) {
                                if (ch == '\\' && k + 1 < close) {
                                    ++k;
                                    continue;
                                }
                                if (ch == '"')
                                    in_str = false;
                                continue;
                            }
                            if (ch == '"') {
                                in_str = true;
                                continue;
                            }
                            if (ch == '(') {
                                ++depth;
                                continue;
                            }
                            if (ch == ')') {
                                --depth;
                                continue;
                            }
                            if (depth > 0)
                                continue;
                            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                                if (tok_start < k) {
                                    arg_strs.push_back(trimmed.substr(tok_start, k - tok_start));
                                    tok_start = k + 1;
                                } else {
                                    tok_start = k + 1;
                                }
                            }
                        }
                        if (tok_start < close) {
                            arg_strs.push_back(trimmed.substr(tok_start, close - tok_start));
                        }
                        // Issue #4895: an argument that builds a closure
                        // (`(f (lambda () 1))`) is evaluated below by a
                        // nested eval() whose expression IRInterpreter
                        // owns the new closure and dies on return. The
                        // id then dangles when f's body calls it
                        // (`arity mismatch` on an id collision, else
                        // `eval_flat: invalid closure`). Leave such a
                        // call to the normal pipeline, which lowers the
                        // whole form in one interpreter.
                        for (const auto& as : arg_strs) {
                            if (arg_text_builds_closure_4895_(as)) {
                                def_body = aura::ast::NULL_NODE;
                                break;
                            }
                        }
                        // Evaluate args in top_env (recursively via eval()).
                        // We pass them through eval() for non-trivial
                        // expressions (so they see macros / IR cache),
                        // but for a pure numeric literal we parse
                        // directly to avoid the standard pipeline's
                        // global-state mutation (set_flat_pool,
                        // set_current_flat, last_ir_mod_) that was
                        // causing the second recursive eval to
                        // return the first's result.
                        std::vector<aura::compiler::types::EvalValue> arg_vals;
                        arg_vals.reserve(arg_strs.size());
                        for (auto& as : def_body == aura::ast::NULL_NODE
                                            ? std::span<std::string>{}
                                            : std::span<std::string>{arg_strs}) {
                            aura::compiler::types::EvalValue val = eval_arg_fast(as);
                            if (val.val == 0 && as != "0") {
                                // Fallback: try full eval for non-numeric args
                                auto ar = eval(as);
                                if (!ar)
                                    return ar;
                                val = *ar;
                            }
                            arg_vals.push_back(val);
                        }
                        // Issue #3918: env-SSOT apply only for workspace
                        // Defines (rebind leftovers). Stdin closures
                        // (hbump / bump) must use the normal pipeline —
                        // apply_closure after rebind can run a captured
                        // hash that hash-set! no longer mutates.
                        if (have_env_fn && def_body != aura::ast::NULL_NODE) {
                            auto cid = types::as_closure_id(env_fn);
                            if (auto r = evaluator_.apply_closure(cid, arg_vals))
                                return EvalResult(*r);
                        }
                        if (def_body != aura::ast::NULL_NODE) {
                            // Build a child env with param bindings.
                            Env call_env(&evaluator_.top_env());
                            call_env.set_primitives(&evaluator_.primitives());
                            if (ws_pool)
                                call_env.set_pool(ws_pool);
                            std::size_t named = dotted && !fn_params.empty() ? fn_params.size() - 1
                                                                             : fn_params.size();
                            for (std::size_t pi = 0; pi < named && pi < arg_vals.size(); ++pi) {
                                call_env.bind_symid(fn_params[pi], std::move(arg_vals[pi]));
                            }
                            // Dotted-rest: collect remaining args into a pair list.
                            if (dotted && !fn_params.empty() && arg_vals.size() > named) {
                                types::EvalValue rest = types::make_void();
                                for (std::size_t ri = arg_vals.size(); ri > named; --ri) {
                                    std::size_t pid = evaluator_.pairs().size();
                                    evaluator_.pairs().push_back(
                                        {std::move(arg_vals[ri - 1]), rest});
                                    rest = types::make_pair(pid);
                                }
                                call_env.bind_symid(fn_params.back(), rest);
                            }
                            return evaluator_.eval_flat(*ws_flat, *ws_pool, def_body, call_env);
                        }
                    }
                    // head is a Variable but no workspace define —
                    // fall through to the normal pipeline (which
                    // handles primitives, IR-cached calls, etc.).
                }
            }
        }
    }
    // Phase 4: parse directly into FlatAST, evaluator reads FlatAST directly.
    // A pure expression (`(+ 1 1)`) does not escape its AST. Allocating
    // each one in the service monotonic arena kept every dead tree until
    // the service died (chaos soak, ~50MB/s). Reuse one heap FlatAST /
    // StringPool and destroy it on the next pure eval. A tree that
    // defines, quotes a non-literal, or allocates a closure is retained
    // for the life of the service — those objects stay reachable.
    aura::ast::StringPool* pool_ptr = nullptr;
    aura::ast::FlatAST* flat_ptr = nullptr;
    // Issue #4070: stack pool_ptr / flat_ptr must not stay in
    // external_root_slots_for_densify_ after eval returns. EXEMPT
    // (no slot). set-code still covers its member addresses.
    release_dead_oneshot_eval_ast();
    const auto closures_before_oneshot = evaluator_.closures_size();
    recyclable_eval_pool_ = std::make_unique<aura::ast::StringPool>();
    recyclable_eval_flat_ = std::make_unique<aura::ast::FlatAST>();
    recyclable_eval_escaped_ = false;
    pool_ptr = recyclable_eval_pool_.get();
    flat_ptr = recyclable_eval_flat_.get();
    // Destructor suppresses implicit move, but copy would still be
    // generated. NoCopy deletes both without a user constructor, so
    // this stays an aggregate.
    struct NoteOneshotEscape : ::aura::core::NoCopy {
        CompilerService* self;
        const aura::ast::FlatAST* flat;
        std::size_t closures_before;
        ~NoteOneshotEscape() {
            if (self != nullptr && flat != nullptr)
                self->note_oneshot_eval_escaped(*flat, closures_before);
        }
    } note_oneshot_escape{{}, this, flat_ptr, closures_before_oneshot};
    static_assert(std::is_aggregate_v<NoteOneshotEscape>);
    static_assert(!std::is_copy_constructible_v<NoteOneshotEscape>);
    static_assert(!std::is_move_constructible_v<NoteOneshotEscape>);
    // Issue #4901: when this eval returns, pin the macro view of its
    // flat. A procedure it defined, called by a later eval after a
    // macro was redefined, keeps the definition in effect here.
    auto& ev_4901 = evaluator_;
    struct MacroViewPin4901 {
        MacroViewPin4901(aura::ast::FlatAST* flat_ast, decltype(ev_4901) evaluator_ref)
            : flat_ast_(flat_ast)
            , evaluator_(evaluator_ref) {}
        ~MacroViewPin4901() {
            if (flat_ast_ != nullptr)
                flat_ast_->macro_view_seq_4901 = evaluator_.macro_seq_4901();
        }
        MacroViewPin4901(const MacroViewPin4901&) = delete;
        MacroViewPin4901& operator=(const MacroViewPin4901&) = delete;
        MacroViewPin4901(MacroViewPin4901&&) = delete;
        MacroViewPin4901& operator=(MacroViewPin4901&&) = delete;

    private:
        aura::ast::FlatAST* flat_ast_;
        decltype(ev_4901) evaluator_;
    };
    const MacroViewPin4901 macro_view_pin_4901{flat_ptr, ev_4901};
    auto pr = aura::parser::parse_to_flat(input, *flat_ptr, *pool_ptr);
    if (pr.root == aura::ast::NULL_NODE) {
        return std::unexpected(parse_error_diag(pr));
    }
    // If there were parse errors but we recovered, log them and continue
    if (!pr.success && !pr.errors.empty()) {
        for (auto& e : pr.errors)
            std::println(std::cerr, "parse warning: {}", e.format());
    }
    flat_ptr->root = pr.root;
    // Store for mutation targeting (CompilerService's tracking, used by last_flat/last_pool)
    current_ast_ = flat_ptr;
    current_pool_ = pool_ptr;
    // Dual-workspace (Phase 1): make this flat/pool visible to
    // source-reading primitives (current-source default, etc.) and
    // also to mutate:* primitives. Set BEFORE any user code runs.
    // See dual-workspace design (archived: docs-archive-pre-2026-06)
    evaluator_.set_flat_pool(flat_ptr, pool_ptr);
    evaluator_.set_current_flat(flat_ptr);
    evaluator_.set_current_pool(pool_ptr);

    // Issue #4846: macros are tree-walker constructs (MacroDef cannot be
    // lowered to IR). eval_ir already short-circuits to the walker when
    // the flat contains one; eval() went through the #2213 gate first, so
    // the production default rejected every program that DEFINES a macro
    // with a generic "tree-walker fallback forbidden" internal error.
    // Take the same macro face here, before the gate runs.
    // Issue #4078 AC2: only the LEGACY defmacro (int_value bit 2
    // clear) needs the walker arm. define-hygienic-macro / *-macro* keep
    // the static macro_expand_all pass, which is what enforces the
    // process hygiene pass cap (a walker arm silently ignored the cap and
    // evaluated the refused half tree).
    //
    // Issue #4857: a legacy MacroDef must not drag the hygienic calls in
    // the same flat onto the runtime walker. Run the static pass first so
    // those calls are expanded once; the walker only sees (and lazily
    // expands) the procedural defmacro sites it needs.
    bool has_legacy_macro = false;
    bool has_static_macro = false;
    for (aura::ast::NodeId macro_id = 0; macro_id < flat_ptr->size(); ++macro_id) {
        const auto& node = flat_ptr->get(macro_id);
        if (node.tag != aura::ast::NodeTag::MacroDef)
            continue;
        if ((node.int_value & 2) == 0)
            has_legacy_macro = true;
        else
            has_static_macro = true;
    }
    if (has_legacy_macro) {
        aura::ast::NodeId macro_root = flat_ptr->root;
        if (has_static_macro)
            macro_root = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, macro_root);
        return evaluator_.eval_flat(*flat_ptr, *pool_ptr, macro_root, evaluator_.top_env());
    }

    // Pre-expand all macros in this expression
    auto expanded_root = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);
    // Issue #4078: pass/depth/gensym/steal/cap — do not typecheck or
    // eval the refused tree. The flat was rolled back in place; the
    // agent-visible reason is the existing hygiene string.
    if (aura_hygiene_expand_deny_blocks_eval())
        return std::unexpected(hygiene_expand_deny_diag_4078(flat_ptr));

    // Pre-execute top-level require/import/use calls to fill ir_cache_
    // so the remaining expression can go through the IR path without fallback.
    // Issue #123: capture the (possibly stripped) root and use it for
    // the rest of the eval pipeline.
    pre_exec_kept_failed_require_ = false;
    expanded_root = pre_exec_requires(*flat_ptr, *pool_ptr, expanded_root);
    // Issue #4575: a denied require stays in the tree. Replay it on
    // the walker so prefix forms run and IR cannot ConstVoid the deny.
    // Issue #4918: a require that failed to RESOLVE is kept too but not
    // stashed (#4817 re-runs it in program order); IR lowering would
    // ConstVoid it, so a missing module was silent with rc=0. File mode
    // replays it on the walker as well.
    if (evaluator_.has_pending_require_diag() ||
        (pre_exec_kept_failed_require_ && evaluator_.require_failure_is_diagnostic())) {
        auto result =
            evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
        if (auto leftover = evaluator_.take_pending_require_diag()) {
            if (result)
                return std::unexpected(std::move(*leftover));
        }
        return result;
    }
    // After pre_exec, root may be NULL_NODE (if the whole program
    // was a single require). Treat as a no-op.
    if (expanded_root == aura::ast::NULL_NODE) {
        return EvalResult(types::make_void());
    }

    // Compile-time AST validation (structural correctness)
    validate_ast(*flat_ptr, *pool_ptr, expanded_root);

    // Register ADT constructors from define-type forms (for match exhaustiveness)
    register_adt_from_define_types(*flat_ptr, *pool_ptr, expanded_root);

    // Collect match clause metadata on expanded flat (post-macro-expansion,
    // where node IDs are stable for the type checker and tree-walker)
    collect_match_info(*flat_ptr, *pool_ptr, expanded_root);

    // If root is a define form, skip fallback check — the IR pipeline
    // handles defines via try_extract_define + cache_define.
    // The Variable checks in needs_tree_walker_fallback would otherwise
    // always trigger fallback (variables in function body aren't in cache yet).
    auto expanded_v =
        expanded_root < flat_ptr->size() ? flat_ptr->get(expanded_root) : aura::ast::NodeView{};
    bool is_fn_define = false;
    if (expanded_v.tag == aura::ast::NodeTag::Define && !expanded_v.children.empty()) {
        auto body_id = expanded_v.child(0);
        if (body_id < flat_ptr->size()) {
            auto body_node = flat_ptr->get(body_id);
            if (body_node.tag == aura::ast::NodeTag::Lambda)
                is_fn_define = true;
        }
    }

    // Issue #2213: production forbids silent tree-walker fallback that
    // abandons SoA + Impact + partial-relower. Gate is zero-cost when
    // needs=false (AC4 happy path).
    if (!is_fn_define) {
        const auto tw_disp =
            consult_tree_walker_fallback_gate_(*flat_ptr, *pool_ptr, expanded_root);
        if (tw_disp == TreeWalkerFallbackDisposition::HardError) {
            // Issue #4478: a bound non-procedure callee is a type error
            // at the call site, not a pipeline-internal refusal.
            if (auto nc = noncallable_call_diag_4478(*flat_ptr, *pool_ptr, expanded_root))
                return std::unexpected(std::move(*nc));
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::InternalError,
                "tree-walker fallback forbidden in production pipeline "
                "(Issue #2213; set AURA_PIPELINE_STRICT=0 or force-soa to override)"});
        }
        if (tw_disp == TreeWalkerFallbackDisposition::TakeWalker) {
            auto result =
                evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
            // Track all bound names so subsequent eval calls don't fall
            // through to the IR pipeline (which silently returns 0 for
            // unknown vars). Issue #132: extracted to
            // aura::compiler.collect_user_bindings.
            for (auto& name :
                 aura::compiler::collect_user_bindings(*flat_ptr, *pool_ptr, expanded_root)) {
                user_bindings_.insert(std::move(name));
            }
            return result;
        }
        // ContinueIr (ForceSoa or needs=false): fall through to IR path.
    }

    // === Level 2: Type check via TypeCheckWrap pass ===
    // Issue #280: tc_pass is declared at this scope (not in
    // an inner block) so the IR pipeline below can read the
    // narrowing evidence captured by the type check.
    // Issue #283 follow-up #5: propagate bidirectional_mode_
    // so the user-controlled opt-out flag flows into the
    // typechecker.
    aura::compiler::TypeCheckWrap tc_pass;
    aura::diag::DiagnosticCollector diags;
    tc_pass.set_bidirectional_mode(bidirectional_mode_);
    tc_pass.set_gradual_permissiveness(gradual_permissiveness_);
    tc_pass.check_before_lowering(*flat_ptr, *pool_ptr, expanded_root, type_registry_, diags,
                                  aura::core::current_mutation_epoch(), &metrics_);
    {
        bool has_type_error = false;
        for (auto& d : diags.diagnostics()) {
            if (d.kind == aura::diag::ErrorKind::TypeError ||
                d.kind == aura::diag::ErrorKind::Note) {
                // Non-strict face: eval continues on the dynamic /
                // tree-walker path, so these diagnostics are
                // informational only — printing them leaked into
                // stderr-merging harnesses (tests/python/run-tests.sh
                // run_test compares 2>&1 output exactly) and broke the
                // pinned error:car / error:car-nonpair /
                // error:cdr-nonpair contracts, where a try-caught
                // dynamic type error must produce clean output.
                // Strict face keeps the detailed print: the abort
                // below makes it the primary error surface.
                if (strict_mode_)
                    std::println(std::cerr, "type: {}", d.format());
                if (d.kind == aura::diag::ErrorKind::TypeError)
                    has_type_error = true;
            }
        }
        if (strict_mode_ && has_type_error) {
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError,
                                                          "type error (strict mode)"});
        }
    }

    // Issue #1506: before define / call IR path, partially re-lower
    // any dirty ir_cache_v2_ entries (body-only bitmasks from
    // mark_define_dirty after set-body / rebind). Mirrors
    // eval-current's relower_dirty_defines_fn_ hook so eval("(f 5)")
    // after mutate consumes partial re-lower, not only full lower.
    (void)relower_dirty_defines_from_workspace();

    // Issue #4346-CI: multi-form Begin root (set-code multi-define
    // buffers): the single-define flow below only handles single-Define
    // roots; register each child Lambda define into the IR/dep
    // structures here so mark_define_dirty / dep-graph checks see them.
    // bind_in_env=true mirrors the single-define cache path; the
    // eval_flat fall-through below re-binds harmlessly.
    const auto root_v_reg = flat_ptr->get(expanded_root);
    if (root_v_reg.tag == aura::ast::NodeTag::Begin) {
        for (auto cid_reg : root_v_reg.children) {
            if (cid_reg >= flat_ptr->size())
                continue;
            if (auto def_reg = try_extract_define(*flat_ptr, *pool_ptr, cid_reg)) {
                auto& [n_reg, b_reg] = *def_reg;
                auto bnode_reg =
                    b_reg < flat_ptr->size() ? flat_ptr->get(b_reg) : aura::ast::NodeView{};
                if (bnode_reg.tag == aura::ast::NodeTag::Lambda) {
                    // Issue #4457: lower_to_ir starts at flat.root. The
                    // begin root would make bind-time execute() run every
                    // sibling form (display) before the real eval.
                    struct RestoreRoot : ::aura::core::NoCopy {
                        aura::ast::FlatAST* flat;
                        aura::ast::NodeId root;
                        ~RestoreRoot() { flat->root = root; }
                    } restore{{}, flat_ptr, flat_ptr->root};
                    flat_ptr->root = cid_reg;
                    (void)cache_define_prefer_partial(input, *flat_ptr, *pool_ptr, cid_reg,
                                                      std::string(n_reg), /*bind_in_env=*/true,
                                                      "__repl__", /*from_eval_ir=*/false);
                }
            }
        }
    }

    // Check for top-level (define ...) — cache IR + eval tree-walker for env persistence
    auto def = try_extract_define(*flat_ptr, *pool_ptr, expanded_root);
    if (def) {
        auto& [name, body_id] = *def;
        // Only cache function defines (Lambda body) are cached as IR
        // Value defines must go through tree-walker for env persistence
        auto body_node =
            body_id < flat_ptr->size() ? flat_ptr->get(body_id) : aura::ast::NodeView{};
        if (body_node.tag == aura::ast::NodeTag::Lambda) {
            // Issue #1506 / #1623: prefer partial re-lower when already
            // cached+dirty (relower_define_blocks before full cache_define).
            auto result = cache_define_prefer_partial(
                input, *flat_ptr, *pool_ptr, expanded_root, std::string(name),
                /*bind_in_env=*/true, "__repl__", /*from_eval_ir=*/false);
            if (!result)
                return result;
            user_bindings_.insert(std::string(name));
            // Issue #3681: a bare (define (f …)) is the mutate target.
            // set-body looks at workspace_flat_, which set-code fills and
            // a REPL define previously left null — so the pre-reemit IR
            // closure stayed applicable. Keep an existing workspace.
            if (!evaluator_.workspace_flat()) {
                evaluator_.set_workspace_flat(flat_ptr);
                evaluator_.set_workspace_pool(pool_ptr);
            }
            return EvalResult(types::make_void());
        }
        // Issue #2568 / #2213: Quote body must tree-walk BEFORE IR bind.
        // IR Quote-of-Variable used to fall through to variable lookup
        // (unbound → 0), and bind_value_define_via_ir "succeeded" with
        // that 0 — so `(define d 'commit)` bound fixnum 0 and never
        // reached the Quote special-case below. ast_to_data interns the
        // symbol on the string heap so eq?/equal? work with later quotes.
        if (body_node.tag == aura::ast::NodeTag::Quote) {
            auto result =
                evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
            user_bindings_.insert(std::string(name));
            return result;
        }
        // Issue #272 Cycle 3: value define via IR when the body is lowerable.
        if (!define_body_needs_tree_walker_fallback(*flat_ptr, *pool_ptr, body_id,
                                                    std::string(name))) {
            if (bind_value_define_via_ir(*flat_ptr, *pool_ptr, expanded_root, std::string(name))) {
                auto bound = evaluator_.top_env().lookup_binding(std::string(name));
                if (bound && types::is_cell(*bound))
                    return evaluator_.cells()[types::as_cell_id(*bound)];
                return EvalResult(types::make_void());
            }
        }
        // Value define: try compile-time constant evaluation
        auto const_val = try_const_eval(*flat_ptr, *pool_ptr, body_id);
        if (const_val) {
            auto ci = bind_define_value_in_env(std::string(name), *const_val);
            ir_value_cell_bindings_[std::string(name)] = ci;
            return *const_val;
        }
        // Issue #2213: value-define tree-walker fallback is also gated.
        // Body needed walker or IR bind failed — under Forbidden hard-fail;
        // under ForceSoa skip silent walker (no silent SoA abandonment).
        {
            metrics_.tree_walker_fallback_total.fetch_add(1, std::memory_order_relaxed);
            const auto disp = tree_walker_fallback_disposition(/*needs_fallback=*/true);
            if (disp == TreeWalkerFallbackDisposition::HardError) {
                metrics_.tree_walker_fallback_forbidden_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::InternalError,
                    "tree-walker fallback forbidden in production pipeline "
                    "(Issue #2213 value-define; AURA_PIPELINE_STRICT override)"});
            }
            if (disp == TreeWalkerFallbackDisposition::ContinueIr) {
                metrics_.tree_walker_fallback_forced_soa_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                // Soft ForceSoa: refuse silent walker; surface as error
                // rather than inventing partial IR for an unlowerable body.
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::InternalError,
                    "tree-walker fallback blocked (ForceSoa); value define not IR-lowerable "
                    "(Issue #2213)"});
            }
        }
        // Fallback: tree-walker for env persistence + subsequent fallback tracking.
        auto result =
            evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
        user_bindings_.insert(std::string(name));
        return result;
    }

    // Issue #3918: nested (display (f x)) / (begin (f x)) used the IR
    // cache of a leftover identity Define. Env cell is SSOT after
    // lockless rebind — tree-walk those Calls so they match HO lookup.
    {
        bool env_closure_call = false;
        for (aura::ast::NodeId id = 0; id < flat_ptr->size(); ++id) {
            if (flat_ptr->is_free_slot(id))
                continue;
            auto nv = flat_ptr->get(id);
            if (nv.tag != aura::ast::NodeTag::Call || nv.children.empty())
                continue;
            auto cal = flat_ptr->get(nv.child(0));
            if (cal.tag != aura::ast::NodeTag::Variable)
                continue;
            const std::string cn(pool_ptr->resolve(cal.sym_id));
            if (cn.empty())
                continue;
            auto bound = evaluator_.top_env().lookup(cn);
            if (!bound)
                continue;
            auto fn = *bound;
            if (types::is_cell(fn)) {
                auto ci = types::as_cell_id(fn);
                if (ci < evaluator_.cells().size())
                    fn = evaluator_.cells()[ci];
            }
            if (types::is_closure(fn)) {
                env_closure_call = true;
                break;
            }
        }
        if (env_closure_call)
            return evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
    }

    // ========== IR pipeline (default path for non-define expressions) ==========
    auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
    auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
    auto ir_mod = lower_to_ir_with_cache_tracked(
        *flat_ptr, *pool_ptr, arena_, cache_ptr, nullptr, &evaluator_.primitives(), nullptr,
        cache_strings_ptr, nullptr, &type_registry_, value_cells_for_lowering(),
        tc_pass.last_narrowing_evidence()); // Issue #280

    // Run passes (silent in default path — use eval_ir for debug)
    // Issue #1457: TypePropagationPass before DCE so CastOp
    // type_id / narrow_evidence reach zero-overhead elision.
    TypeSpecializationWrap ts(&type_registry_);
    TypePropagationPass tprop(&type_registry_);
    ComputeKindWrap ck;
    ArityWrap ar;
    ConstantFoldingWrap cf;
    DeadCoercionEliminationPass dce(&type_registry_);
    ts.run(ir_mod);
    tprop.run(ir_mod);
    ck.run(ir_mod);
    ar.run(ir_mod);
    cf.run(ir_mod);
    dce.run(ir_mod);
    accumulate_type_propagation_metrics(tprop);
    // Issue #253: accumulate linear-move elision count
    // into the shared metrics so snapshot() + the Aura
    // primitive (compile:linear-elide-count) read a single
    // source of truth.
    if (ts.linear_elide_count() > 0) {
        metrics_.linear_elide_count.fetch_add(ts.linear_elide_count(), std::memory_order_relaxed);
    }
    // Issue #433: dead coercion elimination
    // observability. Accumulate the dce pass's
    // eliminated_count() into the lifetime
    // CompilerMetrics counter. The pass existed
    // pre-#433 and was wired into the pipeline
    // (here, in the dce.run() call above), but
    // the eliminated_count was never surfaced
    // to the user — the metric lived only on
    // the per-call pass instance. Post-#433 the
    // lifetime total is readable via snapshot()
    // and the new (compile:dead-coercion-stats)
    // Aura primitive.
    // Issue #508 / #629 / #538: coercion elimination observability.
    accumulate_coercion_pass_metrics(ts, dce);

    if (ar.has_error()) {
        for (auto& d : ar.result().diagnostics) {
            std::println(std::cerr, "arity warning: {}", d.message);
        }
    }

    // ── Run escape analysis pass ───────────────────────
    // Issue #1531: EscapeAnalysisWrap (JIT maps) + EscapeAnalysisPass
    // (IRFunction::escape_map + linear ownership propagators) +
    // LinearOwnershipPass for post-escape ownership validation.
    EscapeAnalysisWrap escape_pass;
    escape_pass.run(ir_mod);
    EscapeAnalysisPass ir_escape;
    ir_escape.run(ir_mod);
    LinearOwnershipPass linear_own;
    linear_own.run(ir_mod);
    // Issue #1875: also run LinearOwnershipWrap for lifetime metrics
    // shared with the type-check post-mutate path.
    LinearOwnershipWrap linear_wrap;
    linear_wrap.run(ir_mod);
    metrics_.ir_escape_analysis_runs_total.fetch_add(1, std::memory_order_relaxed);
    if (ir_escape.escaped_slots_total() > 0) {
        metrics_.ir_escape_slots_marked_total.fetch_add(ir_escape.escaped_slots_total(),
                                                        std::memory_order_relaxed);
    }
    if (ir_escape.dirty_reruns() > 0) {
        metrics_.escape_analysis_dirty_reruns_total.fetch_add(ir_escape.dirty_reruns(),
                                                              std::memory_order_relaxed);
    }
    if (linear_wrap.use_after_move_count() > 0) {
        metrics_.linear_ir_use_after_move_total.fetch_add(linear_wrap.use_after_move_count(),
                                                          std::memory_order_relaxed);
    }
    if (linear_wrap.double_consume_count() > 0) {
        metrics_.linear_ir_double_consume_total.fetch_add(linear_wrap.double_consume_count(),
                                                          std::memory_order_relaxed);
    }
    metrics_.linear_ownership_escape_check_total.fetch_add(1, std::memory_order_relaxed);

    // Issue #462 / #1661: ShapeAwareFoldingPass — runs AFTER
    // EscapeAnalysis (consumes IRFunction::escape_map) + LinearOwnership
    // + ConstantFolding so shape/linear/narrow collaborative folds
    // see stable IR. Accumulates into (query:shape-folding-stats)
    // schema 1661 (shape_aware_fold_hits / linear_ownership_dce /
    // guardshape hits + specialized opportunities).
    ShapeAwareFoldingPass saf;
    // Optional name-keyed override from EscapeAnalysisWrap maps
    // (IRFunction::escape_map from EscapeAnalysisPass is primary).
    {
        const auto& wrap_maps = escape_pass.all_maps();
        for (std::size_t fi = 0; fi < ir_mod.functions.size() && fi < wrap_maps.size(); ++fi) {
            if (!wrap_maps[fi].empty())
                saf.set_escape_map(ir_mod.functions[fi].name, wrap_maps[fi]);
        }
    }
    saf.run(ir_mod);
    if (saf.fold_count() > 0) {
        metrics_.shape_fold_count.fetch_add(saf.fold_count(), std::memory_order_relaxed);
    }
    if (saf.linear_elide_count() > 0) {
        metrics_.shape_linear_elide_count.fetch_add(saf.linear_elide_count(),
                                                    std::memory_order_relaxed);
    }
    if (saf.narrow_check_count() > 0) {
        metrics_.shape_narrow_check_count.fetch_add(saf.narrow_check_count(),
                                                    std::memory_order_relaxed);
    }
    if (saf.guard_shape_hits() > 0) {
        metrics_.guard_shape_hits.fetch_add(saf.guard_shape_hits(), std::memory_order_relaxed);
    }
    if (saf.specialized_shape_fold_opportunities() > 0) {
        metrics_.shape_specialized_fold_opportunities.fetch_add(
            saf.specialized_shape_fold_opportunities(), std::memory_order_relaxed);
    }

    last_ir_mod_ = ir_mod;
    last_ir_stats_ = aura::ir::compute_ir_stats(*last_ir_mod_);
    last_escape_maps_ = escape_pass.take_maps();
    // Issue #2100: accumulate MacroIntroduced IR attrs (last_ir is
    // replaced per form; durable cache feeds deopt AST restore).
    note_macro_ir_attrs_from_module(*last_ir_mod_);

// ── Try JIT execution when LLVM available ──────────────
#ifdef AURA_HAVE_LLVM
    {
        if (!jit_initialized_) {
            register_jit_primitives();
            jit_initialized_ = true;
        }

        // Only try JIT for expressions without complex structural nodes
        // (Lambda, strings, floats, Let/LetRec, macros, quotes, coercions).
        // Named function calls like (add 42) ARE allowed (unlike the old
        // jit_safe_primitives guard which blocked all non-arithmetic calls).
        bool skip_jit = false;
        for (aura::ast::NodeId nid = 0; nid < flat_ptr->size(); ++nid) {
            auto nv = flat_ptr->get(nid);
            auto tag = nv.tag;
            if (tag == aura::ast::NodeTag::Lambda || tag == aura::ast::NodeTag::LiteralString ||
                tag == aura::ast::NodeTag::LiteralFloat || tag == aura::ast::NodeTag::Let ||
                tag == aura::ast::NodeTag::LetRec || tag == aura::ast::NodeTag::Quote ||
                tag == aura::ast::NodeTag::Coercion || tag == aura::ast::NodeTag::MacroDef) {
                skip_jit = true;
                break;
            }
            // Check Call nodes for non-user-defined callees.
            // Built-in primitives (number?, cons, display, etc.) have
            // different result encoding than user-defined functions.
            // Allow calls to cached user-defined functions to pass
            // through for JIT optimization.
            if (tag == aura::ast::NodeTag::Call && !nv.children.empty()) {
                auto callee_id = nv.child(0);
                if (callee_id < flat_ptr->size()) {
                    auto callee_v = flat_ptr->get(callee_id);
                    if (callee_v.tag == aura::ast::NodeTag::Variable) {
                        auto callee_name = pool_ptr->resolve(callee_v.sym_id);
                        auto cname = std::string(callee_name);
                        // Only block if NOT a cached user-defined function
                        if (ir_cache_.count(cname) == 0) {
                            skip_jit = true;
                            break;
                        }
                    }
                }
            }
        }
        // Issue #4567: self-recursive MakeClosure stays on the JIT.
        // The old skip hid a workspace-depth wrap at 256 frames
        // (uint8_t g_depth) that deadlocked (g 1000 0). AURA_FORCE_IR
        // still selects the interpreter.
        if (!skip_jit && !std::getenv("AURA_FORCE_IR")) {
            auto jit_result = try_jit_execute(ir_mod, &last_escape_maps_);
            if (jit_result) {
                record_eval_result_shape(session_id_, last_ir_mod_, nullptr, *jit_result);
                return file_mode_statement_error_4899(std::move(*jit_result), *flat_ptr,
                                                      expanded_root);
            }
        }
    }
#endif

    aura::compiler::IRContext ctx(evaluator_.primitives(), &type_registry_, &metrics_, &evaluator_);
    aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, ctx);
    seed_eval_interp_closure_cursor_4895_(ir_interp);
    ir_interp.set_strategy(strategy_);
    if (!last_escape_maps_.empty())
        ir_interp.set_escape_maps(last_escape_maps_);
    if (strict_mode_)
        ir_interp.set_strict_mode(true);

    try {

        // Set IR closure bridge: enables tree-walker primitives (map/filter/foldl)
        // to call IR-produced closures.
        auto prev_bridge = evaluator_.closure_bridge();
        evaluator_.set_closure_bridge([this, &ir_interp,
                                       &ir_mod](aura::compiler::ClosureId cid,
                                                std::span<const types::EvalValue> args)
                                          -> std::optional<types::EvalValue> {
            auto snap = ir_interp.inspect_closure(cid);
            if (!snap) {
                if (auto ir_define = dispatch_ir_define_closure(cid, args))
                    return ir_define;
                return std::nullopt;
            }

            // Issue #708 / thread-pool-enqueue-stdin: construct the bridge
            // Env with top_env as parent so owner_ is inherited (mirrors the
            // sibling bridge below). A default Env leaves owner_=nullptr →
            // eval_env.owner()->bump_primitive_call_count() null-derefs
            // (SIGSEGV si_addr=0x528) on the first primitive call in an IR
            // bridge, e.g. the body of an enqueued (lambda () (+ 1 2)).
            aura::compiler::Env ne(&evaluator_.top_env());
            ne.set_primitives(&evaluator_.primitives());
            for (std::size_t i = 0; i < snap->env.size() && i < snap->func_free_vars.size(); ++i)
                ne.bind(snap->func_free_vars[i], snap->env[i]);
            for (std::size_t i = 0; i < snap->func_params.size() && i < args.size(); ++i)
                ne.bind(snap->func_params[i], args[i]);

            // Try fast path: bridge data from current module.
            // Issue #224 Cycle 2: shared_ptr is a non-owning view;
            // we dereference to get the raw reference. The const
            // is discarded via const_cast because eval_flat takes
            // non-const refs (the FlatAST itself isn't mutated by
            // eval_flat's implementation, but the signature is
            // non-const for historical reasons).
            if (snap->func_id < ir_mod.closure_bridge.size()) {
                auto& bd = ir_mod.closure_bridge[snap->func_id];
                if (bd.flat && bd.pool) {
                    auto r = evaluator_.eval_flat(*const_cast<ast::FlatAST*>(bd.flat.get()),
                                                  *const_cast<ast::StringPool*>(bd.pool.get()),
                                                  bd.body_id, ne);
                    if (r) {
                        return *r;
                    }
                }
                // Fallback: re-parse lambda body from body_source
                // (needed when cached function's inner lambda has stale FlatAST ptr)
                if (!bd.body_source.empty()) {
                    auto fallback_alloc = arena_.allocator();
                    aura::ast::StringPool* f_pool = nullptr;
                    aura::ast::FlatAST* f_flat = nullptr;
                    // Issue #4070: EXEMPT — stack void** dies before the next Moving rewrite.
                    f_pool = arena_.create_with_cover<aura::ast::StringPool>(
                        nullptr, "stack-cover-dies-at-return", fallback_alloc);
                    f_flat = arena_.create_with_cover<aura::ast::FlatAST>(
                        nullptr, "stack-cover-dies-at-return", fallback_alloc);
                    if (!f_pool || !f_flat)
                        return std::nullopt;
                    auto f_pr = aura::parser::parse_to_flat(bd.body_source, *f_flat, *f_pool);
                    if (f_pr.success && f_pr.root != aura::ast::NULL_NODE) {
                        f_flat->root = f_pr.root;
                        auto r = evaluator_.eval_flat(*f_flat, *f_pool, f_pr.root, ne);
                        if (r)
                            return *r;
                    }
                }
            }

            // Fallback: re-parse from function_sources_ (survives arena resets)
            auto func_name = snap->func_name;
            auto src_it = function_sources_.find(func_name);
            if (src_it != function_sources_.end()) {
                auto fallback_alloc = arena_.allocator();
                aura::ast::StringPool* f_pool = nullptr;
                aura::ast::FlatAST* f_flat = nullptr;
                // Issue #4070: EXEMPT — stack void** dies before the next Moving rewrite.
                f_pool = arena_.create_with_cover<aura::ast::StringPool>(
                    nullptr, "stack-cover-dies-at-return", fallback_alloc);
                f_flat = arena_.create_with_cover<aura::ast::FlatAST>(
                    nullptr, "stack-cover-dies-at-return", fallback_alloc);
                if (!f_pool || !f_flat)
                    return std::nullopt;
                auto f_pr = aura::parser::parse_to_flat(src_it->second, *f_flat, *f_pool);
                if (f_pr.success && f_pr.root != aura::ast::NULL_NODE) {
                    f_flat->root = f_pr.root;
                    // The source is a (define name body) — body is child 0
                    auto define_v = f_flat->get(f_pr.root);
                    if (define_v.tag == aura::ast::NodeTag::Define && !define_v.children.empty()) {
                        auto r = evaluator_.eval_flat(*f_flat, *f_pool, define_v.child(0), ne);
                        if (r)
                            return *r;
                    }
                }
            }
            return std::nullopt;
        });

        auto result = ir_interp.execute();

        // Restore the caller's bridge when re-entered (eval-current inside
        // a running form), else the persistent define bridge.
        if (prev_bridge) {
            evaluator_.set_closure_bridge(std::move(prev_bridge));
        } else
            install_persistent_define_closure_bridge();

        last_closures_ = ir_interp.list_closures();
        last_cells_ = ir_interp.list_cells();
        // Issue #4899: a statement-position error value must not be the
        // silent program result.
        return file_mode_statement_error_4899(std::move(result), *flat_ptr, expanded_root);
    } catch (const std::exception& e) {
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::TypeError, std::string("runtime type error: ") + e.what()});
    }
}

} // namespace aura::compiler
