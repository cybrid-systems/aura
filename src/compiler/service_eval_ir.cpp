// service_eval_ir.cpp — CompilerService::eval_ir moved out of the module
// interface (#5242). Move-only: the body is unchanged.

module;

#include "core/workspace_epoch.hh" // current_mutation_epoch

module aura.compiler.service;

import std;

namespace aura::compiler {

[[nodiscard]] EvalResult CompilerService::eval_ir(std::string_view input) {
    // Issue #411: snapshot mutation_log_ size at entry so the
    // post-eval auto-incremental typecheck can detect new
    // mutations. See eval() above for the full pattern.
    cs_eval_mutation_log_size_at_entry_ = 0;
    if (auto* ws = evaluator_.workspace_flat())
        cs_eval_mutation_log_size_at_entry_ = ws->all_mutations().size();

    // Issue #411: RAII guard. See eval() above.
    PostEvalAutoInvokeGuard post_eval_guard(this, cs_eval_mutation_log_size_at_entry_);

    // Phase 4: parse directly into FlatAST (bypasses Expr* entirely)
    // Arena-allocate FlatAST/Pool so closures can reference them across calls.
    auto alloc = arena_.allocator();
    aura::ast::StringPool* pool_ptr = nullptr;
    aura::ast::FlatAST* flat_ptr = nullptr;
    // Issue #4070: EXEMPT — stack void** dies before the next Moving rewrite.
    pool_ptr = arena_.create_with_cover<aura::ast::StringPool>(nullptr,
                                                               "stack-cover-dies-at-return", alloc);
    flat_ptr =
        arena_.create_with_cover<aura::ast::FlatAST>(nullptr, "stack-cover-dies-at-return", alloc);
    if (!pool_ptr || !flat_ptr) {
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "eval_ir: arena allocate failed"});
    }
    auto pr = aura::parser::parse_to_flat(input, *flat_ptr, *pool_ptr);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        return std::unexpected(parse_error_diag(pr));
    }
    flat_ptr->root = pr.root;
    // Store for mutation targeting
    current_ast_ = flat_ptr;
    current_pool_ = pool_ptr;
    // Dual-workspace (Phase 1): propagate to Evaluator.
    evaluator_.set_flat_pool(flat_ptr, pool_ptr);
    evaluator_.set_current_flat(flat_ptr);
    evaluator_.set_current_pool(pool_ptr);

    // IR pipeline doesn't support macros — fall back to tree-walker evaluator.
    // Issue #4390: preserved / defmacro / eval_data_as_code on that walk
    // consult MacroSelfEvo before expanding. This fallback stays.
    //
    // Issue #4857: do not hand the WHOLE flat to the walker just because it
    // contains a MacroDef. The walker expands a macro call every time the
    // call is evaluated, and every hygienic expansion appends nodes and
    // restamps the flat, so one textual macro call in a hot loop grows
    // super-linearly. Run the static pass first: it expands hygienic /
    // preserved macros once and leaves legacy defmacro call sites to the
    // walker (procedural expansion must stay at runtime).
    bool has_macro_def = false;
    bool has_legacy_macro = false;
    bool has_static_macro = false;
    for (aura::ast::NodeId id = 0; id < flat_ptr->size(); ++id) {
        const auto& node = flat_ptr->get(id);
        if (node.tag != aura::ast::NodeTag::MacroDef)
            continue;
        has_macro_def = true;
        if ((node.int_value & 2) == 0)
            has_legacy_macro = true;
        else
            has_static_macro = true;
    }
    if (has_macro_def) {
        aura::ast::NodeId macro_root = flat_ptr->root;
        if (!has_legacy_macro || has_static_macro) {
            macro_root = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, macro_root);
            // Issue #4078: a refused static expansion must not be evaluated.
            // The mixed legacy arm keeps its historical walker consult.
            if (!has_legacy_macro && aura_hygiene_expand_deny_blocks_eval())
                return std::unexpected(hygiene_expand_deny_diag_4078(flat_ptr));
        }
        return evaluator_.eval_flat(*flat_ptr, *pool_ptr, macro_root, evaluator_.top_env());
    }

    // Pre-expand all macros in this expression
    auto expanded_root = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);
    // Issue #4078: do not validate, typecheck, or lower a refused expand.
    if (aura_hygiene_expand_deny_blocks_eval())
        return std::unexpected(hygiene_expand_deny_diag_4078(flat_ptr));

    // Pre-execute top-level require/import/use calls to fill ir_cache_
    // so cached define functions are available during lowering.
    // Issue #123: capture the (possibly stripped) root.
    pre_exec_kept_failed_require_ = false;
    expanded_root = pre_exec_requires(*flat_ptr, *pool_ptr, expanded_root);
    // Issue #4575: same replay as eval(). A denied require is not void.
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
    if (expanded_root == aura::ast::NULL_NODE) {
        return EvalResult(types::make_void());
    }

    // Update root to expanded version
    flat_ptr->root = expanded_root;

    // Compile-time AST validation
    validate_ast(*flat_ptr, *pool_ptr, flat_ptr->root);

    // Pre-evaluate define-type forms to register constructor primitives
    // before IR lowering, so constructor calls in the code are resolvable.
    for (aura::ast::NodeId nid = 0; nid < flat_ptr->size(); ++nid) {
        if (flat_ptr->get(nid).tag == aura::ast::NodeTag::DefineType) {
            (void)evaluator_.eval_flat(*flat_ptr, *pool_ptr, nid, evaluator_.top_env());
        }
    }

    // Re-register ADT constructors from define-types for match exhaustiveness
    register_adt_from_define_types(*flat_ptr, *pool_ptr, flat_ptr->root);

    // Issue #73: run typecheck before lowering so the FlatAST's
    // type_id column is populated. Without this, the IR-direct
    // path (eval_ir) reads 0 from flat.type_id() and the IR's
    // type_id field is never set — which means type-driven
    // optimizations (TypeSpecializationWrap), the runtime type
    // check (IRInterpreter), and --inspect ir all see no types.
    // Compare to eval() (line ~661) which does this already.
    {
        aura::compiler::TypeCheckWrap tc_pass;
        aura::diag::DiagnosticCollector diags;
        tc_pass.set_bidirectional_mode(bidirectional_mode_);
        tc_pass.set_gradual_permissiveness(gradual_permissiveness_);
        tc_pass.check_before_lowering(*flat_ptr, *pool_ptr, flat_ptr->root, type_registry_, diags,
                                      aura::core::current_mutation_epoch(), &metrics_);
        bool has_type_error = false;
        for (auto& d : diags.diagnostics()) {
            if (d.kind == aura::diag::ErrorKind::TypeError) {
                std::println(std::cerr, "type warning (eval_ir): {}", d.format());
                has_type_error = true;
            }
        }
        if (strict_mode_ && has_type_error) {
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError,
                                                          "type error (strict mode, eval_ir)"});
        }
    }

    // Issue #1506: partial re-lower dirty defines before define/call
    // paths (parity with eval() + eval-current).
    (void)relower_dirty_defines_from_workspace();

    // === Phase 1: Define separation (IR caching) ===
    auto def = try_extract_define(*flat_ptr, *pool_ptr, flat_ptr->root);
    if (def) {
        auto& [name, _body_id] = *def;
        // Issue #1506 / #1623: prefer partial re-lower (eval_ir path).
        auto result = cache_define_prefer_partial(input, *flat_ptr, *pool_ptr, flat_ptr->root,
                                                  std::string(name), /*bind_in_env=*/true,
                                                  "__repl__", /*from_eval_ir=*/true);
        if (!result)
            return result;
        return EvalResult(types::make_void());
    }

    // Issue #2732: top-level Call to a named cached define — apply via
    // the env / ir_define closure (same path as let/apply) instead of
    // re-embedding the define IR into a fresh __top__ module. Nested
    // sibling calls re-embedded via MakeClosure remap can infinite-loop
    // on multi-define cross-call (h → g).
    if (auto applied = try_dispatch_toplevel_define_call_(*flat_ptr, *pool_ptr, expanded_root))
        return *applied;

    // === Normal IR path (with cache awareness) ===
    auto cache_ptr_local = ir_cache_.empty() ? nullptr : &ir_cache_;
    auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
    auto ir_mod = lower_to_ir_with_cache_tracked(
        *flat_ptr, *pool_ptr, arena_, cache_ptr_local, nullptr, &evaluator_.primitives(), nullptr,
        cache_strings_ptr, nullptr, &type_registry_, value_cells_for_lowering());

    TypeSpecializationWrap ts(&type_registry_);
    TypePropagationPass tprop(&type_registry_);
    ComputeKindWrap ck;
    ArityWrap ar;
    ConstantFoldingWrap cf;
    // Issue #1418: DeadCoercionEliminationPass was wired on the
    // default eval() path but missing from eval_ir's run_pipeline
    // pack — dead CastOps accumulated on --inspect / IR-direct.
    // Issue #1457: TypePropagation before DCE for CastOp elision.
    DeadCoercionEliminationPass dce(&type_registry_);
    const std::uint64_t pipeline_epoch = aura::core::current_mutation_epoch();
    ts.set_pipeline_epoch(pipeline_epoch);
    tprop.set_pipeline_epoch(pipeline_epoch);
    ar.set_pipeline_epoch(pipeline_epoch);
    cf.set_pipeline_epoch(pipeline_epoch);
    dce.set_pipeline_epoch(pipeline_epoch);

    // Issue #5235: diagnostic output is the user's; the pass-pipeline
    // trace stays behind an explicit debug switch.
    if (const char* trace = std::getenv("AURA_IR_PIPELINE_TRACE");
        trace != nullptr && *trace != '\0' && std::string_view(trace) != "0") {
        std::println(std::cerr, "PM: running {}->{}->{}->{}->{}->{}", ts.name(), tprop.name(),
                     ck.name(), ar.name(), cf.name(), dce.name());
    }

    // Issue #163: use run_pipeline (Pass concept fold) instead of
    // individual *.run() calls. Short-circuits on has_error().
    // Issue #1418: include DCE after ConstantFolding.
    // Issue #1457: type-propagation between TypeSpec and DCE.
    // Issue #3329: production purity / SoA / DirtyPropagator gate.
    // Issue #4007: first-eval interpreter still folds AoS IRModule
    // (SoA-only skip of dual-emit leaves no live IRModuleV2 here).
    // Named production factory is the V2 PureWrap pack; incremental
    // consume is run_production_soa_dirty_hot_pack. Do not call the
    // AoS named factory from this TU.
    aura::compiler::run_production_pipeline(ir_mod, ts, tprop, ck, ar, cf, dce);
    accumulate_type_propagation_metrics(tprop);
    accumulate_coercion_pass_metrics(ts, dce);

    if (ar.has_error()) {
        for (auto& d : ar.result().diagnostics) {
            std::println(std::cerr, "arith: {}", d.message);
        }
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch, "arity check failed"});
    }

    // Issue #253: accumulate linear-move elision count.
    if (ts.linear_elide_count() > 0) {
        metrics_.linear_elide_count.fetch_add(ts.linear_elide_count(), std::memory_order_relaxed);
    }

    if (cf.folded_count() > 0) {
        // Debug print removed (#63723): was polluting
        // test framework stream redirect for tests like
        // edsl-ir-cache:cascade-after-mutate. The folded
        // count is already in metrics_ via cf_pass metrics.
    }

    // ── Run escape analysis pass ───────────────────────
    EscapeAnalysisWrap escape_pass;
    escape_pass.run(ir_mod);

    last_ir_mod_ = ir_mod;
    const auto& live_ir_mod = *last_ir_mod_; // Issue #4936: engaged just above
    last_ir_stats_ = aura::ir::compute_ir_stats(live_ir_mod);
    last_escape_maps_ = escape_pass.take_maps();
    // Issue #2100: accumulate MacroIntroduced IR attrs for deopt restore.
    note_macro_ir_attrs_from_module(live_ir_mod);

    aura::compiler::IRContext ctx(evaluator_.primitives(), &type_registry_, &metrics_, &evaluator_);
    aura::compiler::IRInterpreter ir_interp(live_ir_mod, ctx);
    seed_eval_interp_closure_cursor_4895_(ir_interp);
    ir_interp.set_strategy(strategy_);
    if (!last_escape_maps_.empty())
        ir_interp.set_escape_maps(last_escape_maps_);
    if (strict_mode_)
        ir_interp.set_strict_mode(true);

    // Set IR closure bridge — enables tree-walker primitives (map/filter/foldl)
    // to call IR-produced closures.
    auto prev_bridge = evaluator_.closure_bridge();
    evaluator_.set_closure_bridge(
        [this, &ir_interp,
         &ir_mod](aura::compiler::ClosureId cid,
                  std::span<const types::EvalValue> args) -> std::optional<types::EvalValue> {
            auto snap = ir_interp.inspect_closure(cid);
            if (!snap)
                return std::nullopt;
            // Bridge Env must inherit `owner_` from top_env so primitive
            // dispatch (which calls eval_env.owner()->bump_primitive_call_count)
            // can find the CompilerMetrics via the SoA walk. Default-constructing
            // `Env ne` leaves owner_=nullptr → SIGSEGV (si_addr=0x528, vtable
            // slot offset) on the first primitive call inside an IR bridge,
            // e.g. `(map (lambda (x) (+ x 1)) (list 1 2 3))`. Mirrors
            // dispatch_ir_define_closure's pattern at the cached-define site.
            aura::compiler::Env ne(&evaluator_.top_env());
            ne.set_primitives(&evaluator_.primitives());
            for (std::size_t i = 0; i < snap->env.size() && i < snap->func_free_vars.size(); ++i)
                ne.bind(snap->func_free_vars[i], snap->env[i]);
            for (std::size_t i = 0; i < snap->func_params.size() && i < args.size(); ++i)
                ne.bind(snap->func_params[i], args[i]);
            // Try bridge data from IR module. Issue #224 Cycle 2:
            // shared_ptr is a non-owning view; dereference via .get()
            // and const_cast (eval_flat's signature takes non-const
            // refs but doesn't actually mutate the FlatAST).
            if (snap->func_id < ir_mod.closure_bridge.size()) {
                auto& bd = ir_mod.closure_bridge[snap->func_id];
                if (bd.flat && bd.pool) {
                    auto r = evaluator_.eval_flat(*const_cast<ast::FlatAST*>(bd.flat.get()),
                                                  *const_cast<ast::StringPool*>(bd.pool.get()),
                                                  bd.body_id, ne);
                    if (r)
                        return *r;
                }
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
            return std::nullopt;
        });

    auto result = ir_interp.execute();

    // Restore the caller's bridge when re-entered (eval-current inside a
    // running form: suite/concurrent_dual_rebind_2738's while closures
    // lost their bridge after the nested run), else the persistent one.
    if (prev_bridge)
        evaluator_.set_closure_bridge(std::move(prev_bridge));
    else
        install_persistent_define_closure_bridge();

    // Capture runtime state for --inspect
    last_closures_ = ir_interp.list_closures();
    last_cells_ = ir_interp.list_cells();

    // ── Shape profile recording (Phase 1, #53) ─────────────
    if (result) {
        record_eval_result_shape(session_id_, last_ir_mod_, &ir_interp, *result);
    }

    // Issue #411: post-eval auto-incremental typecheck.
    // The guard (constructed in eval_ir) fires the auto-
    // invoke on scope exit, covering ALL return paths.
    // See PostEvalAutoInvokeGuard for the full pattern.
    // Issue #4899: same statement-position conversion as eval().
    return file_mode_statement_error_4899(std::move(result), *flat_ptr, expanded_root);
}

} // namespace aura::compiler
