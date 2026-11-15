// service_jit.cpp — CompilerService::try_jit_execute moved out of the
// module interface (#5242). Move-only: the body is unchanged.

module;

#include "aura_jit.h"                        // aura::jit
#include "aura_jit_bridge.h"                 // deopt-pending / AOT table epoch
#include "runtime_shared.h"                  // g_use_arena
#include "shape.h"                           // shape::
#include "shape_profiler.h"                  // shape dirty-scope constants
#include "shape_jit_pass_closedloop_stats.h" // shape_jit_pass::
#include "compiler/type_feedback.h"          // feedback::
#include "compiler/typed_mutation_audit.h"   // typed_audit::
#include "compiler/value_tags.h"             // kSpecial*, AURA_V_*
#include "compiler/jit_assumptions.hh"       // jit_assume
#include "core/workspace_epoch.hh"           // current_mutation_epoch
#include "lock_order_audit.h"                // lock_order

module aura.compiler.service;

import std;

namespace aura::compiler {

std::optional<types::EvalValue>
CompilerService::try_jit_execute(aura::ir::IRModule& ir_mod,
                                 const std::vector<std::vector<std::uint8_t>>* escape_maps) {
    // Issue #59 Iter 3: shared-lock the Mutation Lock so a
    // concurrent mutate:* waits for this compile to drain before
    // invalidating. Compiles can run concurrently with each other.
    // Issue #1523: mutate is FIRST in canonical order (shared is fine).
    lock_order::OrderedSharedLock<std::shared_mutex> mutate_read(mutate_mtx_,
                                                                 lock_order::Level::Mutate);
    sync_lock_order_metrics_();
    // Issue #4338: epoch the incoming IR was sampled under (closest
    // capture to the compile loop; a Workspace-rank mutate skips
    // mutate_mtx_ so the shared lock cannot fence it).
    const std::uint64_t jit_sample_epoch = aura::core::current_mutation_epoch();
    // Issue #4569: define clock, same capture point as jit_sample_epoch.
    const std::uint64_t jit_sample_clock =
        aura::compiler::jit_assume::define_clock().load(std::memory_order_acquire);
    if (ir_mod.functions.empty())
        return std::nullopt;

    // Set string pool before compiling (for OpConstString)
    jit_.set_string_pool(&ir_mod.string_pool);

    // Issue #4388: entry cache-hit state across the register loop.
    bool entry_cache_hit = false;
    bool entry_cache_hit_refused = false;
    aura::jit::ScalarFn entry_hit_fn = nullptr;
    std::uint64_t entry_hit_epoch = 0;
    std::uint64_t entry_hit_drop_gen = 0;
    try_jit_entry_hit_refused_for_test_ = 0;

    // Compile ALL functions (with JIT cache) and register with runtime
    for (auto& ir_fn : ir_mod.functions) {
        // Issue #4567: same unique name as exec_jit. A shared
        // "__lambda__" cache entry aliases every lambda in the module.
        if (ir_fn.name.empty() || ir_fn.name == "__lambda__" ||
            ir_fn.name == "__self_placeholder__") {
            const std::string stem = ir_fn.name.empty() ? "__lambda__" : ir_fn.name;
            ir_fn.name = stem + "#" + std::to_string(ir_fn.id);
        }
        std::uint32_t env_count = static_cast<std::uint32_t>(ir_fn.free_vars.size());

        // Check JIT cache (shared lock for read, unique for write).
        aura::jit::ScalarFn fn_ptr = nullptr;
        bool need_compile = true;
        bool cache_hit = false;
        std::uint64_t hit_epoch = 0;
        std::uint64_t hit_drop_gen = 0;
        auto note_cache_hit = [&](const JitCachedFn& entry) {
            cache_hit = true;
            aura::compiler::jit_assume::stats().hits.fetch_add(1, std::memory_order_relaxed);
            // Issue #4569: the assumption predicate just re-validated
            // this native code at the current state, so re-stamp the
            // Apply prologue's dual-epoch fence (#1537) — otherwise any
            // unrelated edit's bridge-epoch bump would still deopt it at
            // entry (the global storm, moved into the prologue).
            if (!aura::compiler::jit_assume::global_epoch_mode())
                jit_.capture_fn_epoch(ir_fn.name.c_str(), aura_aot_func_table_epoch());
            hit_epoch = entry.last_seen_epoch_;
            hit_drop_gen = aura_jit_define_drop_gen();
        };
        // Issue #4569: assumption key for this IR. The escape map is the
        // one the compile below uses (pre-computed when available).
        const std::vector<std::uint8_t>* akey_esc = (escape_maps && ir_fn.id < escape_maps->size())
                                                        ? &(*escape_maps)[ir_fn.id]
                                                        : &ir_fn.escape_map;
        const JitAssumeKey akey =
            jit_assume_key_(ir_fn, ir_mod.string_pool, nullptr, akey_esc->data(), akey_esc->size());
        {
            std::shared_lock cache_read(jit_cache_mtx_);
            auto cache_it = jit_cache_.find(ir_fn.name);
            if (cache_it != jit_cache_.end()) {
                auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                // Issue #4073: last_seen_epoch_ != current mutation epoch
                // is a miss. Do not load the pre-mutate ScalarFn.
                // Issue #4569: assumption predicate (epoch behind flag).
                if (jit_entry_stale_(cache_it->second, akey, ir_fn.name, false)) {
                    // Unique re-probe erases. Shared lock cannot.
                } else if (!cache_it->second.has_shape_map && !cache_it->second.spec_declined &&
                           shape_profiler_.is_stable(fn_key)) {
                    // Drop shared lock; take unique for the
                    // #4567 specialization probe.
                } else if (jit_cache_shape_version_stale(cache_it->second, fn_key)) {
                    shape::jit_shape_miss_count.fetch_add(1, std::memory_order_relaxed);
                } else if (aura_jit_is_deopt_pending(ir_fn.name.c_str()) != 0) {
                    // Issue #3749: cache-hit must not invoke pre-mutate
                    // native while AuraJIT trackers are batch-deopted.
                } else {
                    fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
                    need_compile = false;
                    note_cache_hit(cache_it->second);
                }
            }
        }
        if (need_compile && fn_ptr == nullptr) {
            std::unique_lock cache_write(jit_cache_mtx_);
            auto cache_it = jit_cache_.find(ir_fn.name);
            auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
            // Issue #4073: same epoch predicate as the shared probe.
            // Issue #4569: assumption predicate (records the event).
            if (cache_it != jit_cache_.end() &&
                jit_entry_stale_(cache_it->second, akey, ir_fn.name, true)) {
                jit_cache_.erase(cache_it);
            } else if (cache_it != jit_cache_.end() && !cache_it->second.has_shape_map &&
                       !cache_it->second.spec_declined && shape_profiler_.is_stable(fn_key)) {
                // Issue #4567: compile_specialized is the live entry.
                // It returns null without a guarded per-arg map, so
                // keep the generic body. Do not erase.
                std::uint8_t no_map = 0;
                auto* spec = spec_jit_.compile_specialized(
                    ir_fn.name, &no_map, 0, cache_it->second.fn_ptr.load(std::memory_order_acquire),
                    ir_fn.arg_count, ir_fn.local_count);
                (void)spec;
                cache_it->second.spec_declined = true;
                fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
                need_compile = false;
                note_cache_hit(cache_it->second);
            } else if (cache_it != jit_cache_.end() &&
                       jit_cache_shape_version_stale(cache_it->second, fn_key)) {
                // Issue #605: drop stale shape-specialized entry.
                jit_cache_.erase(cache_it);
            } else if (cache_it != jit_cache_.end() &&
                       aura_jit_is_deopt_pending(ir_fn.name.c_str()) != 0) {
                // Issue #3749: unique-lock re-probe still refuses
                // batch-deopted native.
                jit_cache_.erase(cache_it);
            } else if (cache_it != jit_cache_.end()) {
                fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
                need_compile = false;
                note_cache_hit(cache_it->second);
            }
        }
        if (!fn_ptr) {
            // Issue #4567: do not stamp one dominant shape onto every
            // argument. final_shape_map stays null, so the generic body
            // is what runs. A real per-arg map is #4568 and is guarded
            // at entry by aura_check_shape_guard.
            const uint8_t* final_shape_map = nullptr;
            auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);

            // Build FlatFunction from IR function (with shape_id stamping)
            std::vector<std::vector<aura::jit::FlatInstruction>> flat_instrs(ir_fn.blocks.size());
            std::vector<aura::jit::FlatBlock> flat_blocks(ir_fn.blocks.size());
            for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
                auto& block = ir_fn.blocks[bi];
                for (auto& instr : block.instructions) {
                    std::uint32_t shape = 0;
                    if (final_shape_map && instr.operands[0] < ir_fn.local_count) {
                        shape = final_shape_map[instr.operands[0]];
                    }
                    flat_instrs[bi].push_back({static_cast<std::uint32_t>(instr.opcode),
                                               {instr.operands[0], instr.operands[1],
                                                instr.operands[2], instr.operands[3]},
                                               shape,
                                               instr.narrow_evidence,
                                               instr.type_id,
                                               instr.linear_ownership_state,
                                               0,
                                               // Issue #1610 / #1616: IR→JIT marker + provenance
                                               instr.source_marker,
                                               instr.provenance});
                }
                flat_blocks[bi] = {block.id, flat_instrs[bi].data(),
                                   static_cast<std::uint32_t>(flat_instrs[bi].size())};
            }
            // Use pre-computed escape maps when available (from EscapeAnalysisWrap pass)
            std::vector<std::uint8_t> escape_storage;
            if (g_use_arena) {
                if (escape_maps && ir_fn.id < escape_maps->size() &&
                    !(*escape_maps)[ir_fn.id].empty()) {
                    // Use pre-computed maps from the pass
                    escape_storage = (*escape_maps)[ir_fn.id];
                } else {
                    // Fallback: run escape analysis inline
                    escape_storage.resize(ir_fn.local_count, 0);
                    aura::jit::run_escape_analysis(flat_instrs, ir_fn.local_count, escape_storage);
                }
            }
            // Issue #4567: final_shape_map stays null (no synthetic stamp).

            aura::jit::FlatFunction flat_fn{ir_fn.name.c_str(),
                                            ir_fn.entry_block,
                                            ir_fn.local_count,
                                            ir_fn.arg_count,
                                            flat_blocks.data(),
                                            static_cast<std::uint32_t>(flat_blocks.size()),
                                            nullptr,
                                            0,
                                            final_shape_map};
            // Issue #4601: env prefix width for direct self-calls.
            flat_fn.env_count = static_cast<std::uint32_t>(ir_fn.free_vars.size());
            // Issue #5222: dotted rest parameter binding.
            flat_fn.variadic = ir_fn.variadic;
            // Set escape map from escape analysis
            if (!escape_storage.empty())
                flat_fn.escape_map = escape_storage.data();
            // Issue #2022: MacroIntroduced tag for try_jit path.
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

            // Issue #4568: per-arg feedback. The aggregate above passes
            // final_shape_map as const_tags (still null). The map, when
            // one exists, is shape_map only. Storage outlives compile.
            // Fresh --jit has an empty table, so this stays null.
            std::vector<std::uint8_t> feedback_shape;
            std::vector<aura::compiler::feedback::ArgSlot> fb_args;
            for (const auto& blk : ir_fn.blocks) {
                for (const auto& instr : blk.instructions) {
                    if (instr.opcode != aura::ir::IROpcode::Arg)
                        continue;
                    aura::compiler::feedback::ArgSlot slot;
                    slot.dest = instr.operands[0];
                    slot.arg_index = instr.operands[1];
                    fb_args.push_back(slot);
                }
            }
            if (aura::compiler::feedback::shape_map_from_feedback(
                    ir_fn.name, flat_fn.source_marker, ir_fn.local_count,
                    fb_args.empty() ? nullptr : fb_args.data(),
                    static_cast<std::uint32_t>(fb_args.size()), feedback_shape)) {
                flat_fn.shape_map = feedback_shape.data();
                for (auto& block : flat_instrs) {
                    for (auto& fi : block) {
                        if (fi.ops[0] < feedback_shape.size())
                            fi.shape_id = feedback_shape[fi.ops[0]];
                    }
                }
            }

            // Skip if already cached at the current mutation epoch
            // (prevents duplicate JIT symbols). Issue #4073: a stale
            // last_seen_epoch_ must not revive the pre-mutate ScalarFn.
            {
                std::shared_lock cache_read(jit_cache_mtx_);
                if (ir_fn.name != "__top__") {
                    auto cached = jit_cache_.find(ir_fn.name);
                    // Issue #4569: same assumption predicate as the probes.
                    if (cached != jit_cache_.end() &&
                        !jit_entry_stale_(cached->second, akey, ir_fn.name, false)) {
                        fn_ptr = cached->second.fn_ptr.load(std::memory_order_acquire);
                        note_cache_hit(cached->second);
                    }
                }
            }
            if (!fn_ptr) {
                fn_ptr = jit_.compile(flat_fn);
                // Issue #1512: mirror AuraJIT coverage masks.
                metrics_.jit_opcode_covered_mask.store(
                    jit_.metrics().opcode_covered_mask.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                metrics_.jit_opcode_unhandled_mask.store(
                    jit_.metrics().opcode_unhandled_mask.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                if (!fn_ptr) {
                    metrics_.jit_compile_misses.fetch_add(1, std::memory_order_relaxed);
                    metrics_.opcode_cov_unhandled_hot_total.fetch_add(1, std::memory_order_relaxed);
                    return std::nullopt;
                }
                metrics_.opcode_cov_hits_total.fetch_add(1, std::memory_order_relaxed);
                metrics_.jit_compilations.fetch_add(1, std::memory_order_relaxed);
                if (ir_fn.name != "__top__") {
                    std::unique_lock cache_write(jit_cache_mtx_);
                    auto [it, _ins] = jit_cache_.try_emplace(ir_fn.name);
                    it->second.fn_ptr.store(fn_ptr, std::memory_order_release);
                    it->second.local_count = ir_fn.local_count;
                    it->second.arg_count = ir_fn.arg_count;
                    it->second.env_count = env_count;
                    it->second.last_seen_epoch_ =
                        jit_sample_epoch; // #4338 sampled epoch, not insert-time current
                    // Issue #4569: assumptions this native code relies on.
                    it->second.fingerprint_ = akey.fp;
                    it->second.deps_ = jit_assume_snapshot_(akey, jit_sample_clock);
                    it->second.has_shape_map = flat_fn.shape_map != nullptr;
                    if (flat_fn.shape_map != nullptr) {
                        it->second.compiled_shape_version_ =
                            shape_profiler_.current_snapshot(fn_key).version;
                    }
                }
            }
        }

        // Issue #4388: production cache hit re-samples before publish.
        // The test hook stands in for the other fiber's facade.
        if (cache_hit && aura::compiler::typed_audit::production_defaults_active()) {
            if (try_jit_facade_skip_mode_for_test_ != 0) {
                const int mode = try_jit_facade_skip_mode_for_test_;
                try_jit_facade_skip_mode_for_test_ = 0;
                if (mode == 1)
                    aura::core::bump_mutation_epoch();
                {
                    std::unique_lock cache_write(jit_cache_mtx_);
                    jit_cache_.erase(ir_fn.name);
                }
                aura_drop_jit_fn_native_for_define(ir_fn.name.c_str());
            }
            if (!jit_cache_hit_still_current_(ir_fn.name, fn_ptr, hit_epoch, hit_drop_gen)) {
                if (ir_fn.id == ir_mod.entry_function_id) {
                    entry_cache_hit_refused = true;
                    try_jit_entry_hit_refused_for_test_ = 1;
                }
                fn_ptr = nullptr;
                cache_hit = false;
            }
        }

        // Register with runtime for closure calls
        // Issue #2022: preserve MacroIntroduced into native side-table.
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
                    ir_fn.id == ir_mod.entry_function_id) {
                    entry_cache_hit_refused = true;
                    try_jit_entry_hit_refused_for_test_ = 1;
                }
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

    // Find and execute entry function
    auto entry_it = std::find_if(
        ir_mod.functions.begin(), ir_mod.functions.end(),
        [&](const aura::ir::IRFunction& f) { return f.id == ir_mod.entry_function_id; });
    if (entry_it == ir_mod.functions.end())
        return std::nullopt;

    auto& entry = *entry_it;
    std::vector<std::int64_t> locals(entry.local_count, 0);
    // Issue #3758: same execute catalog as IRInterpreter::execute.
    // Soft-compiled / jit_cache_ hits have no typed-entry prologue;
    // persist-reject (would_allow==0) must not invoke ScalarFn.
    // Soft/Off: jit_execute_commit_readiness_blocked is false with
    // no commit_readiness load. Return nullopt → interpreter fallback
    // (owns linear_post_mutate_force_rollback_total).
    // Issue #4388: a cache hit that survived the load must re-sample
    // once more immediately before the direct ScalarFn call.
    if (entry_cache_hit_refused)
        return std::nullopt;
    if (entry_cache_hit && !jit_cache_hit_still_current_(entry.name, entry_hit_fn, entry_hit_epoch,
                                                         entry_hit_drop_gen)) {
        try_jit_entry_hit_refused_for_test_ = 1;
        return std::nullopt;
    }
    if (aura::compiler::typed_audit::jit_execute_commit_readiness_blocked())
        return std::nullopt;
    auto fn_ptr = jit_.get_function_ptr(entry.name.c_str());
    if (!fn_ptr && try_jit_scalar_override_for_test_)
        fn_ptr = reinterpret_cast<void*>(try_jit_scalar_override_for_test_);
    if (!fn_ptr)
        return std::nullopt;

    (void)aura_jit_take_prologue_deopt_sentinel();
    auto raw_result = reinterpret_cast<aura::jit::ScalarFn>(fn_ptr)(locals.data(), entry.arg_count);
    // Issue #4084: prologue deopt's fixnum 0 is not a successful value.
    // nullopt → interpreter runs the restored IR.
    if (aura_jit_take_prologue_deopt_sentinel() != 0)
        return std::nullopt;

    // ── Convert JIT result to proper EvalValue type ──
    types::EvalValue result_type;
    uint32_t ret_slot = std::numeric_limits<uint32_t>::max();

    for (auto& block : entry.blocks)
        for (auto& instr : block.instructions)
            if (instr.opcode == aura::ir::IROpcode::Return)
                ret_slot = instr.operands[0];

    // Follow data flow through OpLocal to find the actual producer
    if (ret_slot != std::numeric_limits<uint32_t>::max()) {
        uint32_t search_slot = ret_slot;
        bool chasing = true;
        while (chasing) {
            chasing = false;
            for (auto& block : entry.blocks) {
                for (auto& instr : block.instructions) {
                    if (instr.operands[0] == search_slot &&
                        instr.opcode != aura::ir::IROpcode::Return) {
                        if (instr.opcode == aura::ir::IROpcode::Local) {
                            search_slot = instr.operands[1];
                            chasing = true;
                            goto try_chain;
                        }
                        switch (instr.opcode) {
                            case aura::ir::IROpcode::ConstBool:
                            case aura::ir::IROpcode::Eq:
                            case aura::ir::IROpcode::Lt:
                            case aura::ir::IROpcode::Gt:
                            case aura::ir::IROpcode::Le:
                            case aura::ir::IROpcode::Ge:
                            case aura::ir::IROpcode::And:
                            case aura::ir::IROpcode::Or:
                            case aura::ir::IROpcode::Not:
                                result_type = types::make_bool(raw_result == 7);
                                goto result_done;
                            case aura::ir::IROpcode::ConstI64:
                            case aura::ir::IROpcode::HashRef:
                            case aura::ir::IROpcode::HashRemove:
                                result_type = types::EvalValue(raw_result);
                                goto result_done;
                            case aura::ir::IROpcode::ConstVoid:
                                result_type = raw_result == AURA_V_UNSPEC
                                                  ? types::make_unspecified()
                                                  : types::make_void(); // #4502
                                goto result_done;
                            case aura::ir::IROpcode::MakePair:
                                if (raw_result < 0)
                                    result_type =
                                        types::make_pair(static_cast<uint64_t>(-raw_result - 1));
                                else
                                    result_type =
                                        types::make_pair(static_cast<uint64_t>(raw_result >> 6));
                                goto result_done;
                            case aura::ir::IROpcode::NewCell:
                                result_type = types::make_cell(static_cast<uint64_t>(raw_result));
                                goto result_done;
                            case aura::ir::IROpcode::MakeClosure:
                                result_type =
                                    types::make_closure(static_cast<uint64_t>(raw_result));
                                goto result_done;
                            case aura::ir::IROpcode::ConstF64:
                                result_type = types::EvalValue(raw_result);
                                goto result_done;
                            case aura::ir::IROpcode::ConstString: {
                                auto* str_content = aura_jit_string_content(raw_result);
                                if (str_content) {
                                    auto& sh = evaluator_.primitives().string_heap();
                                    auto new_idx = sh.mint(str_content);
                                    result_type = types::make_string(new_idx);
                                } else {
                                    result_type = types::EvalValue(raw_result);
                                }
                                goto result_done;
                            }
                            case aura::ir::IROpcode::PrimCall:
                                result_type = types::EvalValue(raw_result);
                                goto result_done;
                            default:
                                break;
                        }
                        goto result_done;
                    }
                }
            }
            break;
        try_chain:;
        }
    }

result_done:
    if (result_type.val == 0 && raw_result != 0) {
        if (raw_result == 11)
            result_type = types::make_void();
        else if (raw_result == 3 || raw_result == 7)
            result_type = types::make_bool(raw_result == 7);
        else if ((raw_result & 1) == 0 /* #4482 */)
            result_type = types::EvalValue(raw_result);
        else
            result_type = types::EvalValue(raw_result);
    }
    return result_type;
}

void CompilerService::on_shape_deopt_hook(shape::FnKey fn_key, std::uint64_t version,
                                          std::uint32_t dirty_scope) noexcept {
    (void)version;
    if (dirty_scope == 0)
        return;

    std::string name;
    for (const auto& [n, _] : ir_cache_v2_) {
        if (shape::make_fn_key(session_id_, n) == fn_key) {
            name = n;
            break;
        }
    }
    if (name.empty()) {
        std::shared_lock cache_read(jit_cache_mtx_);
        for (const auto& [n, _] : jit_cache_) {
            if (shape::make_fn_key(session_id_, n) == fn_key) {
                name = n;
                break;
            }
        }
    }
    if (name.empty()) {
        std::shared_lock dep_read(dep_graph_mtx_);
        for (const auto& [n, _] : dep_graph_) {
            if (shape::make_fn_key(session_id_, n) == fn_key) {
                name = n;
                break;
            }
        }
    }

    if (dirty_scope == shape::kShapeDirtyScopeStabilityLoss) {
        metrics_.shape_changes_observed.fetch_add(1, std::memory_order_relaxed);
        shape::shape_deopt_storm_count.fetch_add(1, std::memory_order_relaxed);
        bump_shape_fiber_refresh();
    }

    // Issue #4510: under production defaults the hook must NOT publish a
    // process C-bridge or Mutation bump. The historical bare
    // bump_bridge_epoch() re-pointed g_current_bridge_epoch process-wide
    // while g_jit_fns kept the pre-hook ScalarFn: closures allocated after
    // the hook then passed three-key dual-fresh against the stale native
    // and ran it, and pre-hook peer closures failed dual-fresh (false
    // deopt). Soft / Off keep the historical dual-epoch bump (zero-cost
    // path, not this bug).
    // Split #5242 note: the probe is a hard extern symbol here, so the
    // historical address != nullptr test tripped -Werror=address once the
    // body left the interface. The call is the same.
    const bool production_active = aura_production_defaults_active_probe() != 0;

    // Issue #1521: ArenaCompact — coordinate IR dirty + fiber refresh
    // without counting as deopt storm / mutation churn.
    if (dirty_scope == shape::kShapeDirtyScopeArenaCompact) {
        metrics_.deopt_from_arena_compact_total.fetch_add(1, std::memory_order_relaxed);
        bump_shape_fiber_refresh();
        if (!name.empty()) {
            mark_shape_dirty_for_fn_key(fn_key);
            if (production_active) {
                // Issue #4510: owner-scoped demotion for the resolved
                // define. This arm must not return before the native
                // drop (it previously returned right after the bare
                // bump, before jit_.invalidate and any drop).
                // Issue #4578: name and name# rows go before the drop.
                // A surviving row is an epoch hit (this arm does not
                // move the mutation epoch) and try_jit republishes
                // the dropped ScalarFn. Lock is not held across demote.
                erase_shape_deopt_bundle_rows_(name);
                demote_jit_native_owner_scoped_for_shape_deopt_(name);
                metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
                shape_jit_pass::record_incremental_recompile_hit();
            } else {
                // Soft: dual-epoch bump so JIT shape_map guards recheck
                // (#1496: never advance bridge without defuse). Prefer
                // mark-dirty over full jit_cache_ erase when possible.
                bump_bridge_epoch();
                // Issue #4109: this dual-epoch bump re-definitions nothing —
                // ride still-valid captures up so the pure-counter advance
                // does not strand them behind the empty-Env contract.
                // Issue #4730: bump + ride under one shard barrier.
                (void)evaluator_.bump_defuse_and_ride_frames();
                metrics_.dep_graph_defuse_version_bumps.fetch_add(1, std::memory_order_relaxed);
                metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
                shape_jit_pass::record_incremental_recompile_hit();
            }
        }
        return;
    }

    if (!name.empty()) {
        mark_shape_dirty_for_fn_key(fn_key);
        if (production_active) {
            // Issue #4510: drop the installed ScalarFn BEFORE the
            // AuraJIT teardown (#4100 — AuraJIT::invalidate erases
            // fn_trackers_ but does not clear g_jit_fns), and publish
            // no process C-bridge / Mutation bump. The bundle erase
            // (#4578) drops name and name# so a bare-key miss cannot
            // leave name#pos epoch-fresh; the drop bumps the define
            // drop generation so a #4388 cache-hit republish refuses.
            erase_shape_deopt_bundle_rows_(name);
            demote_jit_native_owner_scoped_for_shape_deopt_(name);
            {
                std::unique_lock cache_write(jit_cache_mtx_);
                jit_.invalidate(name.c_str());
                jit_.invalidate_prefix(name.c_str());
                metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
            }
            shape_jit_pass::record_incremental_recompile_hit();
            evaluator_.bump_incremental_closure_jit_sync();
        } else {
            // Issue #1496: dual-epoch write — bare bump_bridge_epoch here
            // desynced defuse_version_ when invalidate_function already ran
            // atomic_bump_epochs_and_stamp_bridge (hard path lockstep fail).
            bump_bridge_epoch();
            // Issue #4109: this dual-epoch bump re-definitions nothing —
            // ride still-valid captures up so the pure-counter advance does
            // not strand them behind the empty-Env contract. The resync
            // walk below stays observability-only (#4099 no-wash).
            // Issue #4730: bump + ride under one shard barrier.
            (void)evaluator_.bump_defuse_and_ride_frames();
            metrics_.dep_graph_defuse_version_bumps.fetch_add(1, std::memory_order_relaxed);
            evaluator_.resync_live_closure_env_versions_on_invalidate();
            jit_.invalidate(name.c_str());
            jit_.invalidate_prefix(name.c_str());
            metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
            shape_jit_pass::record_incremental_recompile_hit();
            evaluator_.bump_incremental_closure_jit_sync();
            // Issue #4578: same name / name# walk. The dual-epoch
            // bump above stays (zero-cost Soft / Off contract).
            erase_shape_deopt_bundle_rows_(name);
        }
    }
}

} // namespace aura::compiler
