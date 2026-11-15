// service_exec_jit.cpp — CompilerService::exec_jit moved out of the module
// interface (#5242). Move-only: the body is unchanged.

module;

#include "aura_jit.h"                      // aura::jit
#include "aura_jit_bridge.h"               // deopt-pending / AOT table epoch
#include "runtime_shared.h"                // g_use_arena
#include "shape.h"                         // shape::
#include "shape_profiler.h"                // ShapeProfiler
#include "compiler/type_feedback.h"        // feedback::
#include "compiler/typed_mutation_audit.h" // typed_audit::
#include "compiler/value_tags.h"           // kSpecial*, AURA_V_*
#include "compiler/jit_assumptions.hh"     // jit_assume
#include "core/workspace_epoch.hh"         // current_mutation_epoch
#include "lock_order_audit.h"              // lock_order

module aura.compiler.service;

import std;

namespace aura::compiler {

namespace {

    // ── Issue #4600 §1/§2 (phase 2): frame-state deopt host ──
    // Armed by exec_jit around the entry native call. A JIT guard exit
    // calls the bridge stub aura_jit_deopt_frame, which dispatches here:
    // materialize an IRInterpreter frame at the guard's (block, instr)
    // resume point with the spilled locals and drain it — the resumed
    // run IS the IR interpreter, so the deopting native call returns the
    // same value the IR path would have produced (the #4567 corpus pins
    // that byte-identical). Mid-body resumes carry the spill array;
    // entry resumes (block == entry && instr == 0) carry the caller's
    // marshalled buffer and re-run the Arg ops from env++args.
    struct JitDeoptHostState {
        const aura::ir::IRModule* mod = nullptr;
        Primitives* prims = nullptr;
        const aura::core::TypeRegistry* types = nullptr;
        CompilerMetrics* metrics = nullptr;
        Evaluator* evaluator = nullptr;
        // Issue #4600 §3 (phase 3): registered-native table for the
        // named-let OSR escape (drain tail chains → ScalarFn).
        aura::jit::AuraJIT* jit = nullptr;
    };
    thread_local JitDeoptHostState t_jit_deopt_host;

    std::int64_t jit_deopt_frame_host_impl(const char* fn_name, std::uint32_t block,
                                           std::uint32_t instr, const std::int64_t* slots,
                                           std::uint32_t nslots, std::uint32_t argc) {
        const JitDeoptHostState st = t_jit_deopt_host;
        const auto degrade = [] {
            aura_jit_note_prologue_deopt_sentinel();
            return static_cast<std::int64_t>(0);
        };
        if (!st.mod || !fn_name || !fn_name[0] || !slots || !st.prims)
            return degrade();
        const aura::ir::IRFunction* fn = nullptr;
        for (const auto& f : st.mod->functions)
            if (f.name == fn_name) {
                fn = &f;
                break;
            }
        if (!fn)
            return degrade();
        std::vector<types::EvalValue> args;
        if (block == fn->entry_block && instr == 0) {
            const std::uint32_t env_count = aura_jit_deopt_frame_env_count(fn_name);
            const std::uint32_t arg_cap = aura_jit_deopt_frame_arg_count(fn_name);
            if (argc > arg_cap)
                argc = arg_cap;
            const std::size_t n =
                std::min<std::size_t>(std::size_t(env_count) + std::size_t(argc), nslots);
            args.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
                args.emplace_back(slots[i]);
        }
        std::vector<types::EvalValue> locals;
        locals.reserve(nslots);
        for (std::uint32_t i = 0; i < nslots; ++i)
            locals.emplace_back(slots[i]);
        aura::compiler::IRContext ctx(*st.prims, st.types, st.metrics, st.evaluator);
        // Issue #4600 §3: a frame reached through a native call made by an
        // interpreter on this thread carries that interpreter's cell /
        // closure ids in its spilled locals (e.g. the named-let self cell
        // read back by CellGet). CellGet / closure dispatch are per-instance
        // heaps, so the resume must run on the owning interpreter — a fresh
        // one degrades every such id to void and the body then calls a
        // non-procedure. Only reuse it when it executes the same module.
        std::unique_ptr<aura::compiler::IRInterpreter> owned;
        aura::compiler::IRInterpreter* interp = aura::compiler::IRInterpreter::active_instance();
        if (interp == nullptr || interp->module_ptr() != st.mod) {
            owned = std::make_unique<aura::compiler::IRInterpreter>(*st.mod, ctx);
            interp = owned.get();
        }
        auto res = interp->resume_deopt_frame(*fn, block, instr, std::move(locals), args);
        if (!res)
            return degrade();
        return static_cast<std::int64_t>(*res);
    }

    // Issue #4600 §3 (phase 3): named-let OSR host. Calls the fn's
    // REGISTERED native ScalarFn with the pended frame's env+args — no
    // compile (the code already exists; exec_jit registered every module
    // fn before its own entry call). The busy guard breaks the
    // OSR→deopt→resume nesting under forced-deopt mode: the resumed
    // interpreter's own hot tail chain stays interpreted for this chain.
    thread_local int t_jit_osr_busy = 0;
    int jit_osr_enter_host_impl(const char* fn_name, const std::int64_t* slots,
                                std::uint32_t nslots, std::uint32_t argc, std::int64_t* out_value) {
        const JitDeoptHostState st = t_jit_deopt_host;
        if (!st.jit || !fn_name || !fn_name[0] || !slots || nslots == 0 || !out_value)
            return 0;
        if (t_jit_osr_busy)
            return 0;
        auto* fnp = st.jit->get_function_ptr(fn_name);
        if (!fnp)
            return 0;
        std::vector<std::int64_t> osr_slots(slots, slots + nslots);
        struct OsrBusyGuard : ::aura::core::NoCopy {
            int& flag;
            ~OsrBusyGuard() { flag = 0; }
        } osr_busy_guard{{}, t_jit_osr_busy};
        t_jit_osr_busy = 1;
        const std::int64_t v = reinterpret_cast<aura::jit::ScalarFn>(fnp)(
            osr_slots.data(), static_cast<std::int32_t>(argc));
        // An old-path prologue deopt inside the OSR'd call (fns with
        // hidden pcell/spair state — e.g. the named-let self-reference
        // cell — are not frame-deopt armed) returns the #4084 fixnum-0
        // SENTINEL, not a value. The interpreted chain must keep that
        // iteration instead: decline the escape and swallow the sentinel
        // so the exec_jit top-level check does not fire (the frame-deopt
        // stub path sets no sentinel on a hosted resume — its value
        // passes through untouched).
        if (aura_jit_take_prologue_deopt_sentinel() != 0)
            return 0;
        *out_value = v;
        return 1;
    }

    // RAII arm: install the host for the duration of one exec_jit native
    // call (the IRModule is a local there). Restores the previous host on
    // unwind so nested/re-entrant exec_jit stays correct.
    struct JitDeoptHostArm {
        JitDeoptHostState prev;
        explicit JitDeoptHostArm(const JitDeoptHostState& next) {
            prev = t_jit_deopt_host;
            t_jit_deopt_host = next;
            aura_jit_set_deopt_frame_host(&jit_deopt_frame_host_impl);
            // Issue #4600 §3: the OSR escape shares the arm window — no
            // OSR without this frame-deopt host (unhosted resumes would
            // turn the escape into a sentinel protocol violation).
            aura_jit_set_osr_host(&jit_osr_enter_host_impl);
        }
        JitDeoptHostArm(const JitDeoptHostArm&) = delete;
        JitDeoptHostArm& operator=(const JitDeoptHostArm&) = delete;
        JitDeoptHostArm(JitDeoptHostArm&&) = delete;
        JitDeoptHostArm& operator=(JitDeoptHostArm&&) = delete;
        ~JitDeoptHostArm() {
            t_jit_deopt_host = prev;
            if (!prev.mod) {
                aura_jit_set_deopt_frame_host(nullptr);
                aura_jit_set_osr_host(nullptr);
            }
        }
    };

} // namespace
[[nodiscard]] EvalResult CompilerService::exec_jit(std::string_view input) {
    // Issue #411: snapshot mutation_log_ size at entry. See
    // eval() above for the full pattern.
    cs_eval_mutation_log_size_at_entry_ = 0;
    if (auto* ws = evaluator_.workspace_flat())
        cs_eval_mutation_log_size_at_entry_ = ws->all_mutations().size();

    // Issue #411: RAII guard. See eval() above.
    PostEvalAutoInvokeGuard post_eval_guard(this, cs_eval_mutation_log_size_at_entry_);

#ifdef AURA_HAVE_LLVM
    // Issue #59 Iter 3: shared-lock the Mutation Lock for the
    // duration of the JIT compile path. Compiles can run
    // concurrently with each other, but a mutate:* must wait
    // for in-flight compiles to drain.
    std::shared_lock mutate_read(mutate_mtx_);

    // Issue #4600 §8 (phase 4): session lowering memo. A re-run of the
    // identical program text whose own define names have not moved
    // reuses its post-pipeline IRModule: parse, typecheck, lowering,
    // the production pipeline and escape analysis are skipped. The
    // per-function jit_cache_ probe below still runs, so the #4569
    // assumption predicate is unchanged — this only removes the
    // re-lowering of an unchanged define. (The prior call's FlatAST
    // and StringPool stay alive in arena_ for the session, so the
    // skipped parse's evaluator_/current_* pointers remain live.)
    aura::ast::StringPool* pool_ptr = nullptr;
    aura::ast::FlatAST* flat_ptr = nullptr;
    bool memo_hit = false;
    if (jit_reuse_lowering_enabled()) {
        const auto* memo = jit_program_memo_.get();
        if (memo != nullptr && memo->input_size == input.size() && memo->strict == strict_mode_ &&
            memo->input_hash == jit_program_hash_(input) &&
            aura::compiler::jit_assume::first_broken(memo->deps) == nullptr) {
            memo_hit = true;
            aura::compiler::jit_assume::stats().lowering_skips.fetch_add(1,
                                                                         std::memory_order_relaxed);
        }
    }

    if (!memo_hit) {
        // Parse expression
        auto alloc = arena_.allocator();
        // Issue #4070: EXEMPT — stack void** dies before the next Moving rewrite.
        pool_ptr = arena_.create_with_cover<aura::ast::StringPool>(
            nullptr, "stack-cover-dies-at-return", alloc);
        flat_ptr = arena_.create_with_cover<aura::ast::FlatAST>(
            nullptr, "stack-cover-dies-at-return", alloc);
        if (!pool_ptr || !flat_ptr) {
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                          "exec_jit: arena allocate failed"});
        }
        auto pr = aura::parser::parse_to_flat(input, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        flat_ptr->root = pr.root;
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;
        // Dual-workspace (Phase 1): propagate to Evaluator.
        evaluator_.set_flat_pool(flat_ptr, pool_ptr);
        evaluator_.set_current_flat(flat_ptr);
        evaluator_.set_current_pool(pool_ptr);

        // Issue #73 Phase 5: run typecheck before lowering so the
        // FlatAST's type_id column is populated. Without this, the
        // JIT path compiles type-blind IR (every instruction's
        // type_id = 0) and any type-driven optimization (CastOp
        // insertion via TypeSpecializationWrap, type-aware
        // specialization) sees no types. Compare to eval_ir
        // (line ~957) and eval (line ~663) which both do this already.
        {
            aura::compiler::TypeCheckWrap tc_pass;
            aura::diag::DiagnosticCollector diags;
            tc_pass.set_bidirectional_mode(bidirectional_mode_);
            tc_pass.set_gradual_permissiveness(gradual_permissiveness_);
            tc_pass.check_before_lowering(*flat_ptr, *pool_ptr, flat_ptr->root, type_registry_,
                                          diags, aura::core::current_mutation_epoch(), &metrics_);
            bool has_type_error = false;
            for (auto& d : diags.diagnostics()) {
                if (d.kind == aura::diag::ErrorKind::TypeError) {
                    std::println(std::cerr, "type warning (exec_jit): {}", d.format());
                    has_type_error = true;
                }
            }
            if (strict_mode_ && has_type_error) {
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::TypeError, "type error (strict mode, exec_jit)"});
            }
        }
    }

    // Issue #4338: capture the mutation epoch the IR is sampled
    // under. A Workspace-rank EDSL mutate skips mutate_mtx_, so the
    // shared lock below does not exclude it: the epoch can advance
    // mid-compile. Stamping the cache with the CURRENT epoch at
    // insert time would publish a pre-mutate ScalarFn as fresh
    // (last_seen_epoch_ == current ⇒ revived on the next access).
    // Stamping with the sampled epoch leaves the entry stale under
    // any concurrent bump — fail-closed, next access recompiles.
    const std::uint64_t jit_sample_epoch = aura::core::current_mutation_epoch();
    // Issue #4569: define clock the IR is sampled under (same fail-closed
    // rule as jit_sample_epoch, for DefineVersion dependencies).
    const std::uint64_t jit_sample_clock =
        aura::compiler::jit_assume::define_clock().load(std::memory_order_acquire);

    // Lower to IR
    // Issue #4607: the lowering reports the name references it could
    // not resolve (an unbound global). The eval face never compiles
    // such a reference: needs_tree_walker_fallback (service.ixx) sends
    // every program with an unresolved name to the tree walker, which
    // raises the canonical "unbound variable: <name>" diagnostic
    // (evaluator_eval_flat.cpp, Variable case). The JIT path had no
    // such check: the reference was compiled as the constant 0, and
    // the native dispatch then treated 0 as closure id 0 -- a live
    // closure (wrong callee -> infinite recursion -> stack overflow ->
    // SIGSEGV) or an out-of-range id (the bounds-fail echo returned
    // the 0 silently).
    //
    // Refuse to compile such a program and take the tree walker
    // instead: the interpreter raises the canonical error with the
    // same message, the same source location and the same non-zero
    // exit as eval -- and, for a reference that is never reached, the
    // same result as eval (no error at all). The refuse happens
    // BEFORE anything is compiled or executed, so no program state is
    // produced twice.
    aura::ir::IRModule ir_mod;
    EscapeAnalysisWrap escape_pass;
    if (memo_hit) {
        // Issue #4600 §8 (phase 4): reuse the post-pipeline module. The
        // escape maps ride along so the #4569 fingerprint below is the
        // one the cached native code was published under (escape maps
        // are part of jit_assume_key_) — without them every fn would
        // fingerprint differently and recompile.
        ir_mod = jit_program_memo_->mod;
        escape_pass.maps = jit_program_memo_->escape_maps;
    } else {
        std::vector<std::pair<std::string, aura::ast::NodeId>> unbound_globals;
        ir_mod = aura::compiler::lower_to_ir(*flat_ptr, *pool_ptr, arena_, &evaluator_.primitives(),
                                             &type_registry_,
                                             /*narrowing_evidence=*/0, &unbound_globals);
        if (!unbound_globals.empty()) {
            // Issue #4607: the tree walker is the eval face -- the
            // reference face for "unbound variable: <name>". Expand
            // macros first (eval()/eval_ir() do the same; the JIT path
            // never expanded them, so a macro program keeps working).
            auto expanded_root_4607 =
                aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);
            // Drop the mutation read lock before the walker runs: a
            // mutation primitive in the program takes mutate_mtx_ in
            // write mode and this thread already holds it in read mode.
            mutate_read.unlock();
            return evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root_4607,
                                        evaluator_.top_env());
        }

        // Run passes
        {
            aura::compiler::TypeSpecializationWrap ts(&type_registry_);
            aura::compiler::TypePropagationPass tprop(&type_registry_);
            aura::compiler::ComputeKindWrap ck;
            aura::compiler::ArityWrap ar;
            aura::compiler::ConstantFoldingWrap cf;
            // Issue #1418: DCE was missing from the exec_jit
            // run_pipeline pack (only default eval + post-mutate
            // re-lower ran it). Wire after ConstantFolding so JIT
            // never compiles dead CastOps.
            // Issue #1457: TypePropagation before DCE.
            aura::compiler::DeadCoercionEliminationPass dce(&type_registry_);
            const std::uint64_t pipeline_epoch = aura::core::current_mutation_epoch();
            ts.set_pipeline_epoch(pipeline_epoch);
            tprop.set_pipeline_epoch(pipeline_epoch);
            ar.set_pipeline_epoch(pipeline_epoch);
            cf.set_pipeline_epoch(pipeline_epoch);
            dce.set_pipeline_epoch(pipeline_epoch);
            // Issue #163: run_pipeline (Pass concept fold) replaces
            // the individual *.run() calls. Issue #1418: include DCE.
            // Issue #3329: production purity / SoA / DirtyPropagator gate.
            // Issue #4007: exec_jit interpreter AoS — same first-eval
            // residual as eval_ir (no live V2 under SoA-only).
            aura::compiler::run_production_pipeline(ir_mod, ts, tprop, ck, ar, cf, dce);
            accumulate_type_propagation_metrics(tprop);
            accumulate_coercion_pass_metrics(ts, dce);
            // Issue #253: accumulate linear-move elision count.
            if (ts.linear_elide_count() > 0) {
                metrics_.linear_elide_count.fetch_add(ts.linear_elide_count(),
                                                      std::memory_order_relaxed);
            }
        }

        // ── Run escape analysis pass ───────────────────────
        escape_pass.run(ir_mod);

        if (ir_mod.functions.empty()) {
            return EvalResult(types::make_void());
        }
    } // !memo_hit


    // Register primitives with JIT runtime (first call only)
    if (!jit_initialized_) {
        register_jit_primitives();
        jit_initialized_ = true;
    }

    // Pass string pool to JIT compiler for OpConstString support
    jit_.set_string_pool(&ir_mod.string_pool);

    // Shape map storage for specialized compilation
    std::vector<std::uint8_t> current_shape_map;

    // Helper: convert IR function to FlatFunction
    // Optionally includes shape_map for L1 specialization.
    struct FlatFnBuilder {
        std::vector<std::vector<aura::jit::FlatInstruction>> flat_instrs;
        std::vector<aura::jit::FlatBlock> flat_blocks;
        aura::jit::FlatFunction flat_fn;
        std::string name_storage;
        // Shape map storage (must outlive FlatFunction usage)
        std::vector<std::uint8_t> shape_map_storage;
        std::vector<std::uint8_t> escape_map_storage;
        const aura::ir::IRFunction* ir_fn_ = nullptr;

        // Build from IR function, optionally accepting pre-computed escape map.
        FlatFnBuilder(const aura::ir::IRFunction& ir_fn,
                      const std::uint8_t* precomputed_escape = nullptr,
                      std::size_t precomputed_size = 0,
                      const std::uint8_t* precomputed_shape = nullptr,
                      const std::vector<std::uint8_t>* inst_dirty = nullptr) {
            ir_fn_ = &ir_fn;
            flat_instrs.resize(ir_fn.blocks.size());
            flat_blocks.resize(ir_fn.blocks.size());

            for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
                auto& block = ir_fn.blocks[bi];
                std::size_t inst_base = 0;
                for (std::size_t pbi = 0; pbi < bi; ++pbi)
                    inst_base += ir_fn.blocks[pbi].instructions.size();
                std::size_t inst_off = 0;
                for (auto& instr : block.instructions) {
                    // Issue #60 Iter 2: stamp shape_id on the FlatInstruction
                    // from the per-function shape_map. ops[0] is the result
                    // slot for ops with a result (most arith/load ops).
                    std::uint32_t shape = 0;
                    if (precomputed_shape && instr.operands[0] < ir_fn.local_count) {
                        shape = precomputed_shape[instr.operands[0]];
                    }
                    std::uint8_t dirty = 0;
                    if (inst_dirty) {
                        const auto gi = inst_base + inst_off;
                        if (gi < inst_dirty->size())
                            dirty = (*inst_dirty)[gi];
                    }
                    flat_instrs[bi].push_back({static_cast<std::uint32_t>(instr.opcode),
                                               {instr.operands[0], instr.operands[1],
                                                instr.operands[2], instr.operands[3]},
                                               shape,
                                               instr.narrow_evidence,
                                               instr.type_id,
                                               instr.linear_ownership_state,
                                               dirty,
                                               // Issue #1610 / #1616: IR→JIT marker + provenance
                                               instr.source_marker,
                                               instr.provenance});
                    ++inst_off;
                }
                flat_blocks[bi] = {block.id, flat_instrs[bi].data(),
                                   static_cast<std::uint32_t>(flat_instrs[bi].size())};
            }

            name_storage = ir_fn.name;
            flat_fn.name = name_storage.c_str();
            flat_fn.entry_block = ir_fn.entry_block;
            flat_fn.local_count = ir_fn.local_count;
            flat_fn.arg_count = ir_fn.arg_count;
            // Issue #5222: dotted rest parameter binding.
            flat_fn.variadic = ir_fn.variadic;
            // Issue #4601: env prefix width for direct self-calls.
            flat_fn.env_count = static_cast<std::uint32_t>(ir_fn.free_vars.size());
            flat_fn.blocks = flat_blocks.data();
            flat_fn.num_blocks = static_cast<std::uint32_t>(flat_blocks.size());
            flat_fn.func_id_map = nullptr;
            flat_fn.num_callees = 0;
            flat_fn.const_tags = nullptr;
            flat_fn.shape_map = nullptr;
            flat_fn.escape_map = nullptr;
            flat_fn.region = static_cast<std::uint8_t>(ir_fn.region);
            // Issue #2022: function-level MacroIntroduced tag for native
            // metadata (IRFunction::marker from lowering). Also OR any
            // instruction source_marker / first provenance for deopt recovery.
            flat_fn.source_marker = ir_fn.marker;
            flat_fn.provenance = 0;
            for (const auto& blk : ir_fn.blocks) {
                for (const auto& instr : blk.instructions) {
                    if (instr.source_marker == 1)
                        flat_fn.source_marker = 1;
                    if (flat_fn.provenance == 0 && instr.provenance != 0)
                        flat_fn.provenance = instr.provenance;
                    if (flat_fn.source_marker == 1 && flat_fn.provenance != 0)
                        break;
                }
                if (flat_fn.source_marker == 1 && flat_fn.provenance != 0)
                    break;
            }

            // Apply pre-computed escape map, or run analysis inline
            if (g_use_arena) {
                if (precomputed_escape && precomputed_size >= ir_fn.local_count) {
                    // Use pre-computed from EscapeAnalysisWrap pass
                    escape_map_storage.assign(precomputed_escape,
                                              precomputed_escape + ir_fn.local_count);
                    flat_fn.escape_map = escape_map_storage.data();
                } else {
                    // Run escape analysis inline
                    escape_map_storage.resize(flat_fn.local_count, 0);
                    aura::jit::run_escape_analysis(flat_instrs, flat_fn.local_count,
                                                   escape_map_storage);
                    flat_fn.escape_map = escape_map_storage.data();
                }
            }
        }

        // Issue #4567: do not stamp a function-level dominant shape onto
        // every local. Issue #4568: install JIT codes 1/2 only for
        // monomorphic user OpArg dests. Other, unseen, macro-origin,
        // and empty feedback stay on the generic body. check_shape_guard
        // is the entry guard for a map that is actually installed.
        bool set_shape_map(const shape::ShapeProfiler& profiler, const std::string& session,
                           const std::string& fn_name) {
            (void)profiler;
            (void)session;
            if (!ir_fn_)
                return false;
            std::vector<aura::compiler::feedback::ArgSlot> args;
            for (const auto& blk : ir_fn_->blocks) {
                for (const auto& instr : blk.instructions) {
                    if (instr.opcode != aura::ir::IROpcode::Arg)
                        continue;
                    aura::compiler::feedback::ArgSlot slot;
                    slot.dest = instr.operands[0];
                    slot.arg_index = instr.operands[1];
                    args.push_back(slot);
                }
            }
            if (!aura::compiler::feedback::shape_map_from_feedback(
                    fn_name, flat_fn.source_marker, flat_fn.local_count,
                    args.empty() ? nullptr : args.data(), static_cast<std::uint32_t>(args.size()),
                    shape_map_storage))
                return false;
            flat_fn.shape_map = shape_map_storage.data();
            return true;
        }
    };

    // Compile ALL functions (with JIT cache) and register with runtime
    int64_t entry_func_id = -1;
    bool entry_cache_hit = false;
    bool entry_cache_hit_refused = false;
    aura::jit::ScalarFn entry_hit_fn = nullptr;
    std::uint64_t entry_hit_epoch = 0;
    std::uint64_t entry_hit_drop_gen = 0;
    // Issue #4604: symbols already handed to the ORC JIT in this
    // module. A program that defines the same name twice (a
    // redefine) has two bodies under one name; they must not share
    // a symbol, a name-keyed compile-cache entry or an ORC module.
    std::unordered_set<std::string> jit_symbol_names;
    for (auto& ir_fn : ir_mod.functions) {
        if (ir_fn.id == ir_mod.entry_function_id) {
            entry_func_id = static_cast<int64_t>(ir_fn.id);
        }
        // Issue #4567: every define and named-let lambda is lowered as
        // "__lambda__". jit_cache_ and AuraJIT::compile_fns_ key on that
        // name, so the second function reused the first body (sumi → 0,
        // (g 3) ran f). Stamp the function id before any cache probe.
        if (ir_fn.name.empty() || ir_fn.name == "__lambda__" ||
            ir_fn.name == "__self_placeholder__") {
            const std::string stem = ir_fn.name.empty() ? "__lambda__" : ir_fn.name;
            ir_fn.name = stem + "#" + std::to_string(ir_fn.id);
        }
        // Issue #4604: a second define with the same name (a redefine
        // inside one program) gets its own symbol. Without it the two
        // bodies shared one name-keyed compile-cache entry AND one ORC
        // module: the second compile evicted the first module (its
        // memory came back for the new body), so the closure made from
        // the FIRST definition kept dispatching to the SECOND body —
        // "--jit" printed the last definition's value for every call
        // ((define (g x) (+ x 1)) (display (g 1)) (define (g x) (* x
        // 10)) (display (g 2)) printed 1020 instead of 220). With
        // distinct symbols both modules stay live: the first closure
        // keeps the first body, the second the second. The dep-name
        // logic (jit_assume_key_ / jit_program_dep_names_) strips the
        // "#<id>" suffix, so the renamed body still depends on the
        // define "g" and is invalidated with it.
        if (!jit_symbol_names.insert(ir_fn.name).second) {
            const std::string base = ir_fn.name;
            std::string candidate = base + "#" + std::to_string(ir_fn.id);
            while (!jit_symbol_names.insert(candidate).second)
                candidate += "#";
            ir_fn.name = candidate;
        }

        // env_count = number of captured free variables
        std::uint32_t env_count = static_cast<std::uint32_t>(ir_fn.free_vars.size());

        // Check JIT cache (shared lock for read, unique for write).
        aura::jit::ScalarFn fn_ptr = nullptr;
        bool is_top_level = (ir_fn.name == "__top__");
        bool need_compile = true;
        // Issue #4600 §8 (phase 4): true when a live jit_cache_ row
        // already carries this exact IR fingerprint, so a compile that
        // happens anyway (global-epoch mode, shape re-specialization)
        // may reuse AuraJIT's name-keyed native code. __top__ has no
        // row by construction and is recompiled per program.
        bool jit_name_same_ir = false;
        bool cache_hit = false;
        std::uint64_t hit_epoch = 0;
        std::uint64_t hit_drop_gen = 0;
        auto note_cache_hit = [&](const JitCachedFn& cached) {
            cache_hit = true;
            aura::compiler::jit_assume::stats().hits.fetch_add(1, std::memory_order_relaxed);
            // Issue #4569: the assumption predicate just re-validated
            // this native code at the current state, so re-stamp the
            // Apply prologue's dual-epoch fence (#1537) — otherwise any
            // unrelated edit's bridge-epoch bump would still deopt it at
            // entry (the global storm, moved into the prologue).
            if (!aura::compiler::jit_assume::global_epoch_mode())
                jit_.capture_fn_epoch(ir_fn.name.c_str(), aura_aot_func_table_epoch());
            hit_epoch = cached.last_seen_epoch_;
            hit_drop_gen = aura_jit_define_drop_gen();
        };
        // Issue #4569: assumption key for this IR (fingerprint + deps).
        const std::vector<std::uint8_t>* akey_dirty = nullptr;
        if (auto cit = ir_cache_v2_.find(ir_fn.name); cit != ir_cache_v2_.end()) {
            if (ir_fn.id < cit->second.instruction_dirty_per_func_.size())
                akey_dirty = &cit->second.instruction_dirty_per_func_[ir_fn.id];
        }
        const JitAssumeKey akey =
            jit_assume_key_(ir_fn, ir_mod.string_pool, akey_dirty, escape_pass.get_map(ir_fn.id),
                            escape_pass.get_map_size(ir_fn.id));
        if (!is_top_level) {
            {
                std::shared_lock cache_read(jit_cache_mtx_);
                auto cache_it = jit_cache_.find(ir_fn.name);
                if (cache_it != jit_cache_.end()) {
                    // Issue #166 Phase 1: epoch check. If the
                    // entry's last_seen_epoch_ doesn't match the
                    // current mutation_epoch_, the entry is stale
                    // (some mutation happened since this entry was
                    // cached, even if invalidate_function didn't
                    // explicitly target this function name — could
                    // be a transitive invalidation the dep_graph
                    // missed, or a mutation in a different function
                    // that the JIT code implicitly depends on).
                    // Treat as a cache miss and force re-compile.
                    // Issue #4569: assumption predicate (fingerprint +
                    // DefineVersion deps); the bare global epoch compare
                    // is AURA_JIT_GLOBAL_EPOCH_INVALIDATE=1 only.
                    if (jit_entry_stale_(cache_it->second, akey, ir_fn.name, false)) {
                        // Skip the cached entry — fall through to
                        // the compile path below. Don't erase here
                        // (we hold only the shared lock); the
                        // insert path will overwrite with a fresh
                        // entry that has the new epoch.
                        // Issue #4600 §8: an epoch-only staleness
                        // (global-epoch mode) still has the same IR,
                        // so the native code is reusable.
                        jit_name_same_ir = cache_it->second.fingerprint_ != 0 &&
                                           cache_it->second.fingerprint_ == akey.fp;
                    } else {
                        // Hot recompilation: if profiler now has stable shape for this
                        // function but cache entry was compiled without shape_map,
                        // evict and recompile with shape specialization.
                        auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                        // Issue #4600 §4: deopt-storm policy — a fn whose
                        // guards/prologue deopted N times (AURA_JIT_DEOPT_STORM_N,
                        // default 3) drops shape speculation for the session:
                        // megamorphic mark clears stability + bumps the profile
                        // version, so shape-specialized cache entries miss and
                        // the next compile is generic.
                        if (aura::compiler::jit_assume::DeoptStorms::instance().stormed(
                                ir_fn.name)) {
                            shape_profiler_.mark_megamorphic(fn_key);
                        }
                        if (!cache_it->second.has_shape_map && !cache_it->second.spec_declined &&
                            shape_profiler_.is_stable(fn_key)) {
                            // Drop shared lock; take unique for the
                            // #4567 specialization probe.
                        } else if (jit_cache_shape_version_stale(cache_it->second, fn_key)) {
                            // Issue #605: shape version bumped by mutate — re-compile.
                            shape::jit_shape_miss_count.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
                            need_compile = false;
                            note_cache_hit(cache_it->second);
                        }
                    }
                }
            }
            if (need_compile && fn_ptr == nullptr && !is_top_level) {
                // Re-probe under unique lock for the hot-recompile path.
                std::unique_lock cache_write(jit_cache_mtx_);
                auto cache_it = jit_cache_.find(ir_fn.name);
                auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                // Issue #4073: same epoch predicate as the shared probe.
                // A miss there must not be turned back into a hit here.
                // Issue #4569: assumption predicate (records the event).
                if (cache_it != jit_cache_.end() &&
                    jit_entry_stale_(cache_it->second, akey, ir_fn.name, true)) {
                    // Issue #4600 §8: same-IR epoch staleness (global
                    // mode) keeps the native body usable.
                    jit_name_same_ir = cache_it->second.fingerprint_ != 0 &&
                                       cache_it->second.fingerprint_ == akey.fp;
                    jit_cache_.erase(cache_it);
                } else if (cache_it != jit_cache_.end() && !cache_it->second.has_shape_map &&
                           !cache_it->second.spec_declined && shape_profiler_.is_stable(fn_key)) {
                    // Issue #4567: compile_specialized is the live
                    // entry. It returns null without a guarded per-arg
                    // map, so keep the generic body. Do not erase.
                    std::uint8_t no_map = 0;
                    auto* spec = spec_jit_.compile_specialized(
                        ir_fn.name, &no_map, 0,
                        cache_it->second.fn_ptr.load(std::memory_order_acquire), ir_fn.arg_count,
                        ir_fn.local_count);
                    (void)spec;
                    cache_it->second.spec_declined = true;
                    fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
                    need_compile = false;
                    note_cache_hit(cache_it->second);
                } else if (cache_it != jit_cache_.end() &&
                           jit_cache_shape_version_stale(cache_it->second, fn_key)) {
                    // Issue #605: drop stale shape-specialized entry.
                    jit_cache_.erase(cache_it);
                } else if (cache_it != jit_cache_.end()) {
                    fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
                    need_compile = false;
                    note_cache_hit(cache_it->second);
                }
            }
        }
        if (need_compile) {
            auto* precomp_escape = escape_pass.get_map(ir_fn.id);
            auto precomp_size = escape_pass.get_map_size(ir_fn.id);
            const std::vector<std::uint8_t>* inst_dirty = nullptr;
            if (auto cit = ir_cache_v2_.find(ir_fn.name); cit != ir_cache_v2_.end()) {
                if (ir_fn.id < cit->second.instruction_dirty_per_func_.size())
                    inst_dirty = &cit->second.instruction_dirty_per_func_[ir_fn.id];
            }
            FlatFnBuilder builder(ir_fn, precomp_escape, precomp_size, nullptr, inst_dirty);
            // Set shape_map from profiler for L1 specialization
            bool had_shape = builder.set_shape_map(shape_profiler_, session_id_, ir_fn.name);
            // Issue #60 Iter 2: rebuild flat_instrs with shape_id stamped
            // onto each instruction from the shape_map. We rebuild
            // instead of patching because the builder has already
            // populated flat_instrs in its ctor; we need to redo
            // the stamping after shape_map is known.
            if (had_shape) {
                auto& shape_vec = builder.shape_map_storage;
                for (std::size_t bi = 0; bi < builder.flat_instrs.size(); ++bi) {
                    for (auto& fi : builder.flat_instrs[bi]) {
                        if (fi.ops[0] < shape_vec.size()) {
                            fi.shape_id = shape_vec[fi.ops[0]];
                        }
                    }
                }
            }
            // Issue #4600 §8 (phase 4): AuraJIT::compile() serves its
            // name-keyed compile_fns_ cache (fn.name → ScalarFn) without
            // consulting the IR, so a service-level recompile — a
            // fingerprint/DefineVersion miss, or the always-recompiled
            // __top__ — would otherwise get the PREVIOUS body back. In
            // a session that made an edited constant (or a rebound
            // callee) invisible: the second program printed the first
            // program's value. Evict the name first when a prior body
            // exists; invalidate() is the JIT's own (re)define path
            // (resource tracker + compile_fns_ + unhandled counts).
            if (!jit_name_same_ir && jit_.get_function_ptr(ir_fn.name.c_str()) != nullptr)
                jit_.invalidate(ir_fn.name.c_str());
            fn_ptr = jit_.compile(builder.flat_fn);
            // Issue #1512: mirror AuraJIT coverage / unhandled masks into
            // CompilerMetrics for Agent query surfaces.
            metrics_.jit_opcode_covered_mask.store(
                jit_.metrics().opcode_covered_mask.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            metrics_.jit_opcode_unhandled_mask.store(
                jit_.metrics().opcode_unhandled_mask.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            // Issue #1516: mirror EH + per-function AOT counters.
            metrics_.jit_exception_opcode_lowered.store(
                jit_.metrics().exception_opcode_lowered.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            metrics_.jit_exception_opcode_mask.store(
                jit_.metrics().exception_opcode_mask.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            metrics_.jit_exception_opcodes_covered.store(jit_.exception_opcode_coverage_count(),
                                                         std::memory_order_relaxed);
            metrics_.aot_per_function_ir_total.store(
                jit_.metrics().aot_per_function_ir_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            metrics_.aot_per_function_object_total.store(
                jit_.metrics().aot_per_function_object_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            metrics_.aot_per_function_miss_total.store(
                jit_.metrics().aot_per_function_miss_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            metrics_.aot_last_module_object_total.store(
                jit_.metrics().aot_last_module_object_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            if (!fn_ptr) {
                // Issue #62 Iter 1: count compile misses
                metrics_.jit_compile_misses.fetch_add(1, std::memory_order_relaxed);
                metrics_.opcode_cov_unhandled_hot_total.fetch_add(1, std::memory_order_relaxed);
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::InternalError,
                    std::string("JIT compilation failed for function '") + ir_fn.name + "'"});
            }
            metrics_.opcode_cov_hits_total.fetch_add(1, std::memory_order_relaxed);
            // Success counter
            metrics_.jit_compilations.fetch_add(1, std::memory_order_relaxed);

            // Issue #4567: do not seed ShapeProfiler. A specialization
            // has to come from real per-arg feedback (#4568) or from
            // static types, and every speculated slot needs a guard.

            // Cache compiled function (skip __top__ — IR varies per eval)
            if (ir_fn.name != "__top__") {
                std::unique_lock cache_write(jit_cache_mtx_);
                auto [it, _ins] = jit_cache_.try_emplace(ir_fn.name);
                it->second.fn_ptr.store(fn_ptr, std::memory_order_release);
                // Issue #166: stamp the entry with the current
                // epoch. On the next access, if the epoch has
                // changed, the entry is treated as stale.
                // Issue #4338: "current" is the epoch the IR was
                // SAMPLED under, not the epoch at insert time — a
                // Workspace-rank mutate that lands mid-compile must
                // not see this pre-mutate ScalarFn revived as fresh.
                it->second.last_seen_epoch_ = jit_sample_epoch;
                // Issue #4569: assumptions this native code relies on.
                it->second.fingerprint_ = akey.fp;
                it->second.deps_ = jit_assume_snapshot_(akey, jit_sample_clock);
                it->second.local_count = ir_fn.local_count;
                it->second.arg_count = ir_fn.arg_count;
                it->second.env_count = env_count;
                it->second.has_shape_map = had_shape;
                if (had_shape) {
                    auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                    it->second.compiled_shape_version_ =
                        shape_profiler_.current_snapshot(fn_key).version;
                }
            }
        }

        // Issue #4388: same republish gate as try_jit_execute. No test hook.
        if (cache_hit && aura::compiler::typed_audit::production_defaults_active() &&
            !jit_cache_hit_still_current_(ir_fn.name, fn_ptr, hit_epoch, hit_drop_gen)) {
            if (ir_fn.id == ir_mod.entry_function_id)
                entry_cache_hit_refused = true;
            fn_ptr = nullptr;
            cache_hit = false;
        }

        // Register with runtime for closure calls.
        // Issue #660 Option 1: pass ir_fn.name so the runtime can
        // register the function by name (for cross-module closure
        // identity). The name is assigned by cache_define as
        // <user_define_name>#<bundle_position>.
        // Issue #2022: pass MacroIntroduced marker + first provenance so
        // the native side-table / FunctionMeta retain hygiene after JIT.
        {
            std::uint8_t reg_marker = ir_fn.marker;
            std::uint32_t reg_prov = 0;
            for (const auto& blk : ir_fn.blocks) {
                for (const auto& instr : blk.instructions) {
                    if (instr.source_marker == 1)
                        reg_marker = 1;
                    if (reg_prov == 0 && instr.provenance != 0)
                        reg_prov = instr.provenance;
                    if (reg_marker == 1 && reg_prov != 0)
                        break;
                }
                if (reg_marker == 1 && reg_prov != 0)
                    break;
            }
            const bool arm_republish = cache_hit && fn_ptr != nullptr &&
                                       aura::compiler::typed_audit::production_defaults_active();
            if (arm_republish)
                aura_jit_cache_hit_republish_arm(hit_epoch, hit_drop_gen);
            if (fn_ptr)
                jit_.register_function(static_cast<int64_t>(ir_fn.id), fn_ptr, ir_fn.local_count,
                                       ir_fn.arg_count, env_count,
                                       ir_fn.name.empty() ? nullptr : ir_fn.name.c_str(),
                                       reg_marker, reg_prov);
            if (arm_republish) {
                if (aura_jit_cache_hit_republish_refused() != 0 &&
                    ir_fn.id == ir_mod.entry_function_id)
                    entry_cache_hit_refused = true;
                aura_jit_cache_hit_republish_disarm();
            }
        }
        if (cache_hit && fn_ptr != nullptr && ir_fn.id == ir_mod.entry_function_id &&
            !entry_cache_hit_refused && aura::compiler::typed_audit::production_defaults_active()) {
            entry_cache_hit = true;
            entry_hit_fn = fn_ptr;
            entry_hit_epoch = hit_epoch;
            entry_hit_drop_gen = hit_drop_gen;
        }
    }

    // Issue #4600 §8 (phase 4): publish the lowering memo for the next
    // run of this exact text. Stored after the loop so the uniquified
    // function names (__lambda__#N) and the finalized entry id are the
    // ones the cache probe just used. The dep snapshot is the union of
    // the functions' own define names + free variables — the same name
    // set jit_assume_key_ records per function; any bump (rebind /
    // set-code cascade) turns the next run into a memo miss.
    if (jit_reuse_lowering_enabled()) {
        auto memo = std::make_unique<JitProgramMemo>();
        memo->input_hash = jit_program_hash_(input);
        memo->input_size = input.size();
        memo->strict = strict_mode_;
        memo->mod = ir_mod;
        memo->escape_maps = escape_pass.maps;
        memo->deps = jit_program_deps_(jit_program_dep_names_(ir_mod));
        jit_program_memo_ = std::move(memo);
    }

    // Find entry function and execute it
    auto entry_it = std::find_if(
        ir_mod.functions.begin(), ir_mod.functions.end(),
        [&](const aura::ir::IRFunction& f) { return f.id == ir_mod.entry_function_id; });
    if (entry_it == ir_mod.functions.end()) {
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "Entry function not found"});
    }

    auto& entry = *entry_it;
    std::vector<std::int64_t> locals(entry.local_count, 0);
    // Issue #3758: execute catalog before ScalarFn (exec_jit has no
    // interpreter fallback — TypeError + rollback counter).
    // Issue #4388: refuse a cache hit that went stale before the call.
    if (entry_cache_hit_refused ||
        (entry_cache_hit && !jit_cache_hit_still_current_(entry.name, entry_hit_fn, entry_hit_epoch,
                                                          entry_hit_drop_gen))) {
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError, "jit cache hit refused"});
    }
    if (aura::compiler::typed_audit::jit_execute_commit_readiness_blocked()) {
        metrics_.linear_post_mutate_force_rollback_total.fetch_add(1, std::memory_order_relaxed);
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError, "commit-readiness-refused"});
    }
    auto fn_ptr = jit_.get_function_ptr(entry.name.c_str());
    if (!fn_ptr) {
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "JIT entry function lookup failed"});
    }

    (void)aura_jit_take_prologue_deopt_sentinel();
    // Issue #5174: a stale depth flag from another host must not
    // refuse this call. A hit after the native call is real.
    (void)aura_jit_take_depth_message();
    // Issue #4600: exec_jit owns no linear env by construction — the
    // top-level entry has no captures for the prologue's linear probe.
    // Declare it so the #3654 fail-closed (unset context under the
    // production face) consults the callback as an audited no-op
    // (#1731) instead of deopting every entry.
    aura_jit_set_linear_env_none();
    // Issue #4600 §1/§2 (phase 2): arm the frame-state deopt host for
    // the duration of the native call — every guard exit (prologue,
    // entry shape guard, car/cdr, GuardShape) resumes the IR
    // interpreter here instead of surfacing an error/sentinel.
    const aura::compiler::JitDeoptHostState deopt_state_4600{
        &ir_mod, &evaluator_.primitives(), &type_registry_, &metrics_, &evaluator_, &jit_};
    aura::compiler::JitDeoptHostArm deopt_host_4600{deopt_state_4600};
    auto raw_result = reinterpret_cast<aura::jit::ScalarFn>(fn_ptr)(locals.data(), entry.arg_count);
    // Issue #5174: a native frame past the ceiling unwound with 0.
    // eval_ir would run it (the IR limit is far higher) and hide the
    // refuse. No "fiber=" — that prefix resumes onto the IR path.
    if (const char* depth_msg = aura_jit_take_depth_message()) {
        (void)aura_jit_take_prologue_deopt_sentinel();
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError, std::string(depth_msg)});
    }
    // Issue #4084: prologue deopt's fixnum 0 is not a successful value.
    // Issue #4606: a refused callee — a RefPrimitive-tagged prim word
    // (string?/getenv/eqv?/memv/...) loaded from a global or define
    // cell — is answered by the blessed dispatch entry with the
    // #4084 leave-native refuse, so the native frame unwinds with
    // the sentinel armed. The IR interpreter runs the restored IR
    // here — the same contract as try_jit_execute's nullopt ->
    // interpreter fallback (#4084): the result is eval-identical
    // instead of the raw prim word the old callee_val echo returned.
    if (aura_jit_take_prologue_deopt_sentinel() != 0) {
        return eval_ir(input);
    }

    // ── Convert JIT result to proper EvalValue type ──
    // Issue #5175: the native word is already the value. Classify it
    // from the ENTRY function only. A nested lambda's NewCell shares
    // slot numbers; the old all-functions scan wrapped a fixnum as
    // <cell[N]> (6<<1 == 12). Call/PrimCall write operands[3], Apply
    // writes operands[2]. Follow Local and the let bind-init CellSet
    // so a CellGet of a string or a closure is not left as a raw id.
    // A v2 string is reminted even when the producer is CellGet or
    // PrimCall — the JIT pool index is not an evaluator heap index.
    types::EvalValue ev_result;
    std::uint32_t ret_slot = std::numeric_limits<std::uint32_t>::max();
    for (auto& block : entry.blocks)
        for (auto& instr : block.instructions)
            if (instr.opcode == aura::ir::IROpcode::Return)
                ret_slot = instr.operands[0];

    enum class JitProd : std::uint8_t { Word, Bool, Void, Pair, Cell, Closure };
    auto result_operand = [](aura::ir::IROpcode op) -> int {
        using aura::ir::IROpcode;
        if (op == IROpcode::Call || op == IROpcode::PrimCall)
            return 3;
        if (op == IROpcode::Apply)
            return 2;
        switch (op) {
            case IROpcode::Nop:
            case IROpcode::Branch:
            case IROpcode::Jump:
            case IROpcode::Return:
            case IROpcode::Capture:
            case IROpcode::CaptureRef:
            case IROpcode::CellSet:
            case IROpcode::TryBegin:
            case IROpcode::DropOp:
            case IROpcode::ArenaPop:
            case IROpcode::TopCellStore:
                return -1;
            default:
                return 0;
        }
    };
    auto last_writer = [&](std::uint32_t slot) -> const aura::ir::IRInstruction* {
        const aura::ir::IRInstruction* found = nullptr;
        for (auto& block : entry.blocks) {
            for (auto& instr : block.instructions) {
                const int ri = result_operand(instr.opcode);
                if (ri >= 0 && instr.operands[static_cast<std::size_t>(ri)] == slot)
                    found = &instr;
            }
        }
        return found;
    };
    JitProd prod = JitProd::Word;
    if (ret_slot != std::numeric_limits<std::uint32_t>::max()) {
        std::uint32_t search_slot = ret_slot;
        for (int hop = 0; hop < 16; ++hop) {
            const aura::ir::IRInstruction* writer = last_writer(search_slot);
            if (!writer)
                break;
            if (writer->opcode == aura::ir::IROpcode::Local) {
                search_slot = writer->operands[1];
                continue;
            }
            if (writer->opcode == aura::ir::IROpcode::CellGet) {
                // The bind-init store is what the JIT kept (#5173).
                // A later set! has no bind flag and is not the value.
                const std::uint32_t cell = writer->operands[1];
                const aura::ir::IRInstruction* bind = nullptr;
                for (auto& block : entry.blocks) {
                    for (auto& instr : block.instructions) {
                        if (instr.opcode == aura::ir::IROpcode::CellSet &&
                            instr.operands[0] == cell && instr.operands[2] == 0xFFFFFFFFu)
                            bind = &instr;
                    }
                }
                if (!bind)
                    break;
                search_slot = bind->operands[1];
                continue;
            }
            using aura::ir::IROpcode;
            switch (writer->opcode) {
                case IROpcode::ConstBool:
                case IROpcode::Eq:
                case IROpcode::Lt:
                case IROpcode::Gt:
                case IROpcode::Le:
                case IROpcode::Ge:
                case IROpcode::And:
                case IROpcode::Or:
                case IROpcode::Not:
                    prod = JitProd::Bool;
                    break;
                case IROpcode::ConstVoid:
                case IROpcode::TryEnd:
                    prod = JitProd::Void;
                    break;
                case IROpcode::MakePair:
                    prod = JitProd::Pair;
                    break;
                case IROpcode::NewCell:
                    prod = JitProd::Cell;
                    break;
                case IROpcode::MakeClosure:
                    prod = JitProd::Closure;
                    break;
                default:
                    prod = JitProd::Word;
                    break;
            }
            break;
        }
    }

    // Copy a JIT pair chain into the evaluator pair vector so the
    // CLI printer walks it. Prelude pairs keep their own indices:
    // a tagged word whose index is already in pairs_ is left as-is
    // unless the producer is MakePair (that id is a g_pair_slots id).
    auto reify_jit_word =
        [&](auto& self, std::int64_t word, int depth,
            std::unordered_map<std::uint64_t, std::uint64_t>& memo) -> types::EvalValue {
        if (is_str_val(word))
            return types::EvalValue(convert_str_for_eval(word, &evaluator_.primitives()));
        if (depth > 64 || !types::is_pair(types::EvalValue(word)))
            return types::EvalValue(word);
        const auto id = types::as_pair_idx(types::EvalValue(word));
        if (auto it = memo.find(id); it != memo.end())
            return types::make_pair(it->second);
        if (id >= ::g_pair_slots.size() || ::g_pair_slots[id] == nullptr)
            return types::EvalValue(word);
        const std::int64_t car_w = ::g_pair_slots[id]->car;
        const std::int64_t cdr_w = ::g_pair_slots[id]->cdr;
        auto& pv = evaluator_.pairs();
        const auto idx = static_cast<std::uint64_t>(pv.size());
        pv.push_back(Pair{});
        memo.emplace(id, idx);
        pv[static_cast<std::size_t>(idx)].car = self(self, car_w, depth + 1, memo);
        pv[static_cast<std::size_t>(idx)].cdr = self(self, cdr_w, depth + 1, memo);
        return types::make_pair(idx);
    };

    const bool pair_word = types::is_pair(types::EvalValue(raw_result));
    const auto pair_id = pair_word ? types::as_pair_idx(types::EvalValue(raw_result)) : 0;
    const bool pair_in_eval = pair_word && pair_id < evaluator_.pairs().size();
    if (is_str_val(raw_result)) {
        ev_result = types::EvalValue(convert_str_for_eval(raw_result, &evaluator_.primitives()));
    } else if (types::is_float_raw_v2(raw_result)) {
        ev_result = types::EvalValue(raw_result);
    } else if (raw_result == 11) {
        ev_result = types::make_void();
    } else if (raw_result == AURA_V_UNSPEC) {
        ev_result = types::make_unspecified();
    } else if (raw_result == 3 || raw_result == 7) {
        ev_result = types::make_bool(raw_result == 7);
    } else if (pair_word && (prod == JitProd::Pair || !pair_in_eval)) {
        std::unordered_map<std::uint64_t, std::uint64_t> memo;
        ev_result = reify_jit_word(reify_jit_word, raw_result, 0, memo);
    } else if (pair_word) {
        ev_result = types::EvalValue(raw_result);
    } else if ((raw_result & 1) == 1 && types::is_ref(raw_result)) {
        ev_result = types::EvalValue(raw_result);
    } else if (prod == JitProd::Closure) {
        // aura_alloc_closure returns a raw slot id, not a tagged ref.
        ev_result = types::make_closure(static_cast<std::uint64_t>(raw_result));
    } else if (prod == JitProd::Cell) {
        ev_result = types::make_cell(static_cast<std::uint64_t>(raw_result));
    } else if (prod == JitProd::Bool) {
        ev_result = types::make_bool(raw_result == 7);
    } else if (prod == JitProd::Void) {
        ev_result = raw_result == AURA_V_UNSPEC ? types::make_unspecified() : types::make_void();
    } else if (raw_result < 0 && prod == JitProd::Pair) {
        ev_result = types::make_pair(static_cast<std::uint64_t>(-raw_result - 1));
    } else if ((raw_result & 1) == 0) {
        ev_result = types::EvalValue(raw_result); // tagged fixnum
    } else {
        ev_result = types::EvalValue(raw_result);
    }
    // PrimCall void-returning prims (Display/Write/Newline): void result.
    // Entry only — a nested display must not void the outer value.
    if (types::is_int(ev_result) && types::as_int(ev_result) == 0 &&
        ret_slot != std::numeric_limits<std::uint32_t>::max()) {
        for (auto& iblock : entry.blocks) {
            for (auto& instr : iblock.instructions) {
                if (instr.opcode == aura::ir::IROpcode::PrimCall &&
                    (instr.operands[0] == ret_slot || instr.operands[3] == ret_slot)) {
                    auto prim_id = static_cast<aura::ir::PrimId>(instr.operands[0]);
                    if (prim_id == aura::ir::PrimId::Display ||
                        prim_id == aura::ir::PrimId::Write ||
                        prim_id == aura::ir::PrimId::Newline) {
                        ev_result = types::make_void();
                        break;
                    }
                }
            }
            if (types::is_void(ev_result))
                break;
        }
    }
    // Record JIT result shape for profiling (triggers hot-recompilation)
    record_eval_result_shape(session_id_, last_ir_mod_, nullptr, ev_result);
    // Issue #411: post-eval auto-incremental typecheck.
    // The guard (constructed in exec_jit) fires the auto-
    // invoke on scope exit, covering ALL return paths.
    // See PostEvalAutoInvokeGuard for the full pattern.
    return EvalResult(ev_result);
#else
    (void)input;
    return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                  "JIT not available — rebuild with LLVM"});
#endif
}

} // namespace aura::compiler
