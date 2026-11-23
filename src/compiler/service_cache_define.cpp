// service_cache_define.cpp - CompilerService::cache_define moved out of the
// module interface (#5242). Move-only: the body is unchanged.

module;

#include "core/workspace_epoch.hh"         // current_mutation_epoch
#include "compiler/typed_mutation_audit.h" // typed_audit::
#include "core/transparent_string_hash.hh" // TransparentStringHash

module aura.compiler.service;

import std;
import aura.compiler.type_checker; // TypeCheckWrap

namespace aura::compiler {

EvalResult CompilerService::cache_define(std::string_view source, aura::ast::FlatAST& flat,
                                         aura::ast::StringPool& pool,
                                         aura::ast::NodeId expanded_root,
                                         const std::string& name_str, bool bind_in_env,
                                         const std::string& module_name) {
    IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
    bool is_redefine = ir_cache_.count(name_str) > 0;

    // === Level 2: Type check via TypeCheckWrap pass ===
    {
        aura::compiler::TypeCheckWrap tc_pass;
        aura::diag::DiagnosticCollector diags;
        tc_pass.set_bidirectional_mode(bidirectional_mode_);
        tc_pass.set_gradual_permissiveness(gradual_permissiveness_);
        tc_pass.check_before_lowering(flat, pool, expanded_root, type_registry_, diags,
                                      aura::core::current_mutation_epoch(), &metrics_);
        bool has_type_error = false;
        // Issue #5056: check_before_lowering runs infer_flat and then the
        // pure check over the same tree, both reporting into this
        // collector — so one diagnostic is collected twice. Print each
        // distinct message once (the collector itself keeps both).
        std::unordered_set<std::string> printed_type_warnings;
        for (auto& d : diags.diagnostics()) {
            if (d.kind == aura::diag::ErrorKind::TypeError) {
                auto text = d.format();
                if (printed_type_warnings.insert(text).second)
                    std::println(std::cerr, "type warning ({}): {}", name_str, text);
                has_type_error = true;
            }
        }
        // Issue #5321: this per-define check is where the workspace's
        // pre-existing ground joins surface (as warnings) before any mutation
        // has run. Keep them as the delta baseline so a later mutation is not
        // rejected for joins it did not introduce.
        if (::std::getenv("AURA_GATE_TRACE") != nullptr)
            std::fprintf(stderr, "[gate] cache_define hook epoch=%llu tc=%d diags=%zu\n",
                         static_cast<unsigned long long>(aura::core::current_mutation_epoch()),
                         evaluator_.persistent_typechecker() != nullptr ? 1 : 0,
                         diags.diagnostics().size());
        if (aura::core::current_mutation_epoch() == 0) {
            if (auto* tc_ptr = evaluator_.persistent_typechecker()) {
                static_cast<aura::compiler::TypeChecker*>(tc_ptr)->record_baseline_diagnostics(
                    diags.diagnostics(), 0);
            }
        }
        if (strict_mode_ && has_type_error) {
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError,
                                                          "type error (strict mode)"});
        }
    }

    // Check for reasons to skip IR caching:
    // 1. `require` inside function body — IR lowering treats it as ConstI64 0
    // 2. Self-recursive calls — function not in ir_cache_ yet → ConstI64 0
    // (mitigated: self_name passed to lowering for MakeClosure pre-allocation)
    // 3. Calls to non-cached, non-primitive variables — not resolvable in IR
    bool skip_ir_cache = false;
    // Issue #4513: skip caused by an untrusted v1 callee hit (callee
    // v2 row dirty / abort-latched / unstored). See LambdaBodyWalker.
    bool refused_stale_callee_4513 = false;
    if (expanded_root < flat.size()) {
        auto def_v = flat.get(expanded_root);
        if (def_v.tag == aura::ast::NodeTag::Define) {
            auto body_id = def_v.children.empty() ? aura::ast::NULL_NODE : def_v.child(0);
            if (body_id < flat.size()) {
                auto body_v = flat.get(body_id);
                if (body_v.tag == aura::ast::NodeTag::Lambda) {
                    auto lambda_body =
                        body_v.children.empty() ? aura::ast::NULL_NODE : body_v.child(0);
                    // Collect parameter names to distinguish them from external references
                    auto params_list = body_v.params;
                    std::unordered_set<std::string> param_names;
                    for (auto pid : params_list)
                        param_names.insert(std::string(pool.resolve(pid)));

                    // Walk the lambda body for variables that can't be lowered
                    struct LambdaBodyWalker {
                        const aura::ast::FlatAST& f;
                        const aura::ast::StringPool& p;
                        const std::string& self_name;
                        const std::unordered_set<std::string>& param_names;
                        const Evaluator& eval;
                        const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>,
                                                 aura::core::TransparentStringHash,
                                                 std::equal_to<>>& ir_cache;
                        const std::unordered_map<std::string, IRCacheEntry,
                                                 aura::core::TransparentStringHash,
                                                 std::equal_to<>>& ir_cache_v2;
                        // Issue #4513: consult the callee's v2 row for v1
                        // hits (production / Full only — mirrors the #4436
                        // full-arm gate).
                        bool consult_v2_4513 = false;
                        std::unordered_set<std::string> let_bound;
                        bool skip = false;
                        // Issue #4513: a v1 hit was refused for an
                        // untrusted v2 row.
                        bool refused_callee_4513 = false;
                        void walk(aura::ast::NodeId id) {
                            if (skip || id == aura::ast::NULL_NODE || id >= f.size())
                                return;
                            auto nv = f.get(id);
                            if (nv.tag == aura::ast::NodeTag::Quote)
                                return;
                            if (nv.tag == aura::ast::NodeTag::Let ||
                                nv.tag == aura::ast::NodeTag::LetRec) {
                                auto nm = std::string(p.resolve(nv.sym_id));
                                if (!nv.children.empty())
                                    walk(nv.child(0));
                                let_bound.insert(nm);
                                if (nv.children.size() >= 2)
                                    walk(nv.child(1));
                                let_bound.erase(nm);
                                return;
                            }
                            if (nv.tag == aura::ast::NodeTag::Set) {
                                // Issue #3927: IR Set only stores to local
                                // scopes / captured cells. set! of a
                                // top-level value cell is a no-op if the
                                // name was never read (not a free var).
                                // Tree-walk so lookup_cell_index hits the
                                // evaluator cell; later < / = / - load it.
                                auto nm = std::string(p.resolve(nv.sym_id));
                                if (!param_names.count(nm) && !let_bound.count(nm) &&
                                    nm != self_name)
                                    skip = true;
                                for (auto c : nv.children)
                                    walk(c);
                                return;
                            }
                            if (nv.tag == aura::ast::NodeTag::Variable) {
                                auto var_name = std::string(p.resolve(nv.sym_id));
                                // Skip params, the function's own name (self-reference handled
                                // by lowering), primitives, cached functions
                                if (param_names.count(var_name) || let_bound.count(var_name))
                                    return;
                                if (var_name == self_name)
                                    return;
                                if (eval.primitives().slot_for_name(var_name) <
                                    eval.primitives().slot_count())
                                    return;
                                if (ir_cache.count(var_name)) {
                                    // Issue #4513: v1 presence alone is not
                                    // trust. When the callee's v2 row is
                                    // dirty / has dirty blocks /
                                    // abort-latched / unstored, v1 holds the
                                    // previous (post-abort: aborted) body
                                    // and the #4436 full arm would refuse
                                    // the embed. Refuse here too: skip
                                    // resolves via eval_flat on the restored
                                    // AST instead of copying that hit.
                                    if (consult_v2_4513) {
                                        auto v2it = ir_cache_v2.find(var_name);
                                        if (v2it != ir_cache_v2.end()) {
                                            const auto& ce = v2it->second;
                                            if (ce.dirty || ce.dirty_block_count() > 0 ||
                                                ce.abort_map_invalid ||
                                                !ce.content_stored_this_epoch) {
                                                skip = true;
                                                refused_callee_4513 = true;
                                            }
                                        } else {
                                            // Issue #4528: a v1 hit with NO v2
                                            // row is the same orphan family —
                                            // eviction dropped the v2 row while
                                            // v1 kept the pre-mutate body, or
                                            // cache_define filled v1 before any
                                            // store_define_v2 of the callee.
                                            // Refuse like a dirty v2 row: the
                                            // embed would freeze the orphan
                                            // bytes into the caller's next
                                            // clean generation.
                                            skip = true;
                                            refused_callee_4513 = true;
                                        }
                                    }
                                    return;
                                }
                                // Unknown variable — IR will emit ConstI64 0
                                skip = true;
                            }
                            for (auto c : nv.children)
                                walk(c);
                        }
                    };
                    const bool consult_v2_4513 =
                        aura::compiler::typed_audit::production_defaults_active() ||
                        aura::compiler::typed_audit::get_strategy() ==
                            aura::compiler::typed_audit::AuditStrategy::Full;
                    LambdaBodyWalker lbw{flat,       pool,      name_str,     param_names,
                                         evaluator_, ir_cache_, ir_cache_v2_, consult_v2_4513};
                    lbw.walk(lambda_body);
                    skip_ir_cache = lbw.skip;
                    // Issue #4513: the skip came from an untrusted v1
                    // callee hit.
                    refused_stale_callee_4513 = lbw.refused_callee_4513;
                }
            }
        }
    }

    // Issue #4618: the LambdaBodyWalker above is lambda-only, so a
    // value define whose body set!s a non-local (e.g. a workspace
    // define that bumps a global before returning a constant) still
    // reached lowering, which drops the store. Refuse it the same way.
    if (!skip_ir_cache && define_body_sets_nonlocal_4618(flat, pool, expanded_root, name_str))
        skip_ir_cache = true;
    // Issue #5421: a dotted-rest define is lowerable now that the IR
    // interpreter packs the trailing arguments into a pair list (the
    // JIT already did, #5222). Keep the old tree-walker escape only for
    // the non-local set! and unresolved-name gates below.

    if (skip_ir_cache) {
        // Issue #4618: a refused define owns no IR body. A rebind /
        // dirty re-lower (relower_define_blocks) or the workspace
        // specialise pass may have published a bundle for this name
        // earlier; populate_ir_cache_v2_from_workspace mirrors
        // entry.irs back as the next clean generation whenever
        // ir_cache_ has no entry, and a caller lower would then embed
        // the pre-rebind body. Drop the IR body; the tree walker owns
        // the name (its Set writes the cell lookup_cell_index hits).
        if (auto vit = ir_cache_v2_.find(name_str); vit != ir_cache_v2_.end()) {
            vit->second.irs.clear();
            vit->second.bridges.clear();
            vit->second.init_block_dirty_from_irs();
            vit->second.clear_all_block_dirty();
        }
        // Issue #4513: fail closed with no copy of the refused hit.
        // Drop this define's own v1 bundle — it embeds the stale
        // callee — so populate_ir_cache_v2_from_workspace cannot
        // store_define_v2 it back as the next clean generation.
        // abort_map_invalid / content_stored_this_epoch stay latched
        // until a lower that did not embed a stale v1 callee.
        if (refused_stale_callee_4513) {
            ir_cache_.erase(name_str);
            ir_cache_bridge_.erase(name_str);
            ir_cache_strings_.erase(name_str);
        }
        // Skip IR caching — use tree-walker only. The define has already been
        // evaluated via tree-walker below, so the env bindings are correct.
        function_sources_[name_str] = std::string(source);
        module_functions_["__repl__"].push_back(name_str);
        return evaluator_.eval_flat(flat, pool, expanded_root, evaluator_.top_env());
    }

    auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
    auto cache_bridge_ptr = ir_cache_bridge_.empty() ? nullptr : &ir_cache_bridge_;
    auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
    std::vector<std::string> cache_hits;
    // Pass self_name so lowering can emit correct MakeClosure for self-references
    auto ir_mod = lower_to_ir_with_cache_tracked(
        flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), cache_bridge_ptr,
        cache_strings_ptr, &name_str, &type_registry_, value_cells_for_lowering());
    // Issue #4630: the lowering inlines a cached callee bundle at every
    // call site, so a define whose body calls the next define twice
    // carries two copies of that bundle (2^depth down a chain). Hash-cons
    // the copies: the bundle then carries one copy per callee and the
    // per-function passes below run on O(depth) functions, not 2^depth.
    (void)hash_cons_module_functions_4630(ir_mod);
    // Issue #660 debug: dump IR after lowering
    if (name_str.empty()) {
        for (auto& func : ir_mod.functions) {
            std::println(std::cerr, "[IR-DUMP] func id={} name='{}' blocks={}", func.id, func.name,
                         func.blocks.size());
            for (auto& blk : func.blocks) {
                for (auto& inst : blk.instructions) {
                    std::println(std::cerr, "  inst op={} ops=[{},{},{},{}]",
                                 static_cast<int>(inst.opcode), inst.operands[0], inst.operands[1],
                                 inst.operands[2], inst.operands[3]);
                }
            }
        }
    }

    // Run passes per-function on the new function bundle
    {
        aura::compiler::ComputeKindWrap ck_pass;
        aura::compiler::ConstantFoldingWrap cf_pass;
        for (auto& func : ir_mod.functions) {
            if (func.id == ir_mod.entry_function_id)
                continue;
            ck_pass.compute_function(func);
            cf_pass.fold_function(func);
            // Issue #538: DCE after define cache lower.
            run_coercion_elim_on_function(func);
        }
    }

    // Issue #660 Option 1: assign each function a unique stable name
    // (user'''s define name + "#" + position). This name is used by
    // the runtime for cross-module closure identity when the closure'''s
    // func_id is out of bounds in the current module.
    std::vector<aura::ir::IRFunction> bundle;
    std::vector<aura::ir::ClosureBridgeData> bridge_bundle;
    std::size_t own_pos = 0;
    for (auto& func : ir_mod.functions) {
        if (func.id == ir_mod.entry_function_id)
            continue;
        // Override the default "__lambda__" name (or empty name) with
        // a unique stable name for cross-module closure identity.
        if (func.name.empty() || func.name == "__lambda__") {
            func.name = name_str + std::string("#") + std::to_string(own_pos++);
        }
        // Issue #660 follow-up: COPY instead of move (preserves ir_mod for binding)
        bundle.push_back(func);
        // Also save bridge data
        if (func.id < ir_mod.closure_bridge.size())
            bridge_bundle.push_back(ir_mod.closure_bridge[func.id]);
        else
            bridge_bundle.emplace_back();
    }
    // Issue #272 Cycle 4: snapshot before move + IR env bind for disk serialize.
    ir_disk_snapshots_[name_str] = bundle;
    ir_cache_[name_str] = std::move(bundle);
    ir_cache_bridge_[name_str] = std::move(bridge_bundle);
    ir_cache_strings_[name_str] = ir_mod.string_pool;
    function_sources_[name_str] = std::string(source);
    module_functions_[module_name].push_back(name_str);

    for (auto& called_name : cache_hits) {
        record_dependency(name_str, called_name);
    }
    note_slot_edges_4681_(name_str);

    if (is_redefine) {
        // Issue #660 follow-up: JIT cache keys are
        // `name + "#" + position` (e.g. "mul#0"), not the
        // bare user name. The previous fix erased
        // "__lambda__" + name_str, both of which miss the
        // actual entry. Walk the cache and erase any
        // entries whose key starts with name_str + "#"
        // (covers mul#0, mul#1, ... for the user's mul).
        // Also call jit_.invalidate_prefix(name_str) to
        // clear the AuraJIT::compile_fns_ + fn_trackers_
        // caches (which are keyed by ir_fn.name, also
        // name + "#" + pos).
        {
            std::unique_lock cache_write(jit_cache_mtx_);
            std::string prefix = name_str + "#";
            for (auto it = jit_cache_.begin(); it != jit_cache_.end();) {
                if (it->first == "__lambda__" || it->first.rfind(prefix, 0) == 0) {
                    it = jit_cache_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        jit_.invalidate_prefix(name_str.c_str());
        // Also drop the per-function compile cache inside AuraJIT
        // (compile_fns_) so the next compile() actually re-runs the
        // LLVM pipeline. Without this, compile() short-circuits
        // on a cache hit and returns the old fn_ptr even though we
        // erased jit_cache_ — and the runtime keeps calling the
        // stale implementation. The name is "__lambda__" because
        // the body of a top-level define is lowered as an
        // anonymous lambda, so every define shares that name in
        // the JIT's per-function cache.
        jit_.invalidate("__lambda__");
        invalidate_function(name_str);
        mark_module_dirty(name_str);
    }

    // Issue #272: bind env via IRInterpreter when possible (function defines).
    // Skip when bind_in_env=false (populate_ir_cache_v2_from_workspace
    // pre-populates the v2 cache without polluting the env).
    if (bind_in_env) {
        if (bind_function_define_via_ir(ir_mod, name_str)) {
            return EvalResult(types::make_void());
        }
        return evaluator_.eval_flat(flat, pool, expanded_root, evaluator_.top_env());
    }
    // bind_in_env=false: skip the env binding. Caller (e.g.
    // populate_ir_cache_v2_from_workspace) just wants the IR cached.
    return EvalResult(types::make_void());
}

} // namespace aura::compiler
