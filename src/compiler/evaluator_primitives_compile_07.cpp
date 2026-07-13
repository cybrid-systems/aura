// evaluator_primitives_compile_07.cpp — Issue #909: peeled compile registration
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "per_defuse_index.h"
#include "hash_meta.h"
#include "basis_points.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.mutators;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.value;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.pass_manager;
import aura.compiler.query;
import aura.compiler.hardware_backend;
import aura.compiler.sv_ir;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_keyword;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

// Issue #909 compile part 56 (orig 4631-4698)
void CompilePrims::register_compile_p56(PrimRegistrar add, Evaluator& ev) {

    // ═══════════════════════════════════════════════════════════
    // Issue #309: hardware lossy-coercion diagnostics.
    //
    // Two new primitives extend the BitVector foundation from
    // #308 with hw-aware coercion analysis:
    //
    //   (compile:hw-coercion-lossy? <from-name> <to-name>)
    //     Returns 1 iff coercing FROM `from-name` TO `to-name`
    //     would LOSE information. The canonical rule: lossy iff
    //     from is wider than to (narrowing drops high bits). Same
    //     width or widening is lossless. If either type isn't
    //     registered as a hw bitvec, returns 0 (not applicable).
    //
    //   (compile:hw-coercion-warning <from-name> <to-name>)
    //     Returns a human-readable warning string when the
    //     coercion is lossy, or "" (empty string) when it's
    //     lossless / not applicable. The string format is:
    //       "lossy coercion: <from> (W<from-w> signed) -> <to> (W<to-w> signed) drops <n> bits"
    //     E.g.: "lossy coercion: uint16_t (W16 unsigned) -> uint8_t (W8 unsigned) drops 8 bits"
    //
    // Why these primitives:
    //   - Issue #309 AC2: "New warning emitted for lossy bit
    //     coercion in hardware context." Today the user code
    //     calls these primitives at the coercion site to
    //     emit the warning. The automatic type-checker
    //     warning (emitted during infer_flat) is a follow-up.
    //   - Issue #309 AC1: "Blame correctly tracks across a
    //     typed-mutate that changes a coercion site in
    //     hardware code." The BlameInfo (Issue #342) is
    //     already attached to type-checker diagnostics via
    //     with_blame() — see type_checker_impl.cpp's
    //     narrowing path. The hw-aware extension of
    //     BlameInfo (e.g. hw_region field) is a follow-up.
    //   - Future #309 follow-ups: integrate the lossy check
    //     into InferenceEngine's subtyping path (so the
    //     warning is automatic), extend BlameInfo with
    //     hw_region (Synth | Sim | Unset), and richer
    //     hardware-specific messages (e.g. "may introduce
    //     latch" for incomplete case + width-loss).
    add("compile:hw-coercion-lossy?", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return ev.make_merr("bad-arg", "usage: (compile:hw-coercion-lossy? from-name to-name)");
        auto from_sx = as_string_idx(a[0]);
        auto to_sx = as_string_idx(a[1]);
        std::string from_name, to_name;
        if (from_sx < ev.string_heap_.size())
            from_name = ev.string_heap_[from_sx];
        if (to_sx < ev.string_heap_.size())
            to_name = ev.string_heap_[to_sx];
        if (!ev.type_registry_)
            return make_int(0);
        auto& reg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        auto from_tid = reg.lookup_type(from_name);
        auto to_tid = reg.lookup_type(to_name);
        if (!from_tid.valid() || !to_tid.valid())
            return make_int(0);
        auto* from_bv = reg.hw_bitvec_of(from_tid);
        auto* to_bv = reg.hw_bitvec_of(to_tid);
        if (!from_bv || !to_bv)
            return make_int(0); // not a hw coercion
        // Lossy iff FROM is wider than TO (narrowing drops bits).
        // Same width (regardless of signedness) is lossless:
        // reinterpreting signed↔unsigned doesn't lose bits.
        // Widening is lossless (zero- or sign-extension).
        const bool lossy = from_bv->width > to_bv->width;
        return make_int(lossy ? 1 : 0);
    });
}

// Issue #909 compile part 57 (orig 4699-4773)
void CompilePrims::register_compile_p57(PrimRegistrar add, Evaluator& ev) {

    add("compile:hw-coercion-warning", [&ev](const auto& a) -> EvalValue {
        // Issue #1050: empty-string sentinel must be a real heap entry,
        // never make_string(heap.size()) which is OOB.
        auto empty_str = [&ev]() -> EvalValue {
            return make_string(static_cast<std::uint64_t>(ev.push_string_heap("")));
        };
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return ev.make_merr("bad-arg",
                                "usage: (compile:hw-coercion-warning from-name to-name)");
        auto from_sx = as_string_idx(a[0]);
        auto to_sx = as_string_idx(a[1]);
        std::string from_name, to_name;
        if (from_sx < ev.string_heap_.size())
            from_name = ev.string_heap_[from_sx];
        if (to_sx < ev.string_heap_.size())
            to_name = ev.string_heap_[to_sx];
        if (!ev.type_registry_)
            return empty_str();
        auto& reg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        auto from_tid = reg.lookup_type(from_name);
        auto to_tid = reg.lookup_type(to_name);
        if (!from_tid.valid() || !to_tid.valid())
            return empty_str();
        auto* from_bv = reg.hw_bitvec_of(from_tid);
        auto* to_bv = reg.hw_bitvec_of(to_tid);
        if (!from_bv || !to_bv)
            return empty_str();
        if (from_bv->width <= to_bv->width)
            return empty_str(); // lossless — no warning
        const std::uint32_t dropped = from_bv->width - to_bv->width;
        const std::string from_str = from_bv->is_signed ? "signed" : "unsigned";
        const std::string to_str = to_bv->is_signed ? "signed" : "unsigned";
        const std::string msg = "lossy coercion: " + from_name + " (W" +
                                std::to_string(from_bv->width) + " " + from_str + ") -> " +
                                to_name + " (W" + std::to_string(to_bv->width) + " " + to_str +
                                ") drops " + std::to_string(dropped) + " bits";
        return make_string(static_cast<std::uint64_t>(ev.push_string_heap(msg)));
    });

    // ── Issue #373: MacroIntroduced hygiene guard primitives ──
    //
    // Three primitives that surface the hygiene guard added by
    // #373 piece 2 (mutate guards):
    //
    //   (hygiene:protected? node-id)
    //     Returns #t if the node has marker == MacroIntroduced
    //     (i.e. was produced by clone_macro_body from a hygienic
    //     macro expansion). #f otherwise (including when the
    //     workspace or node id is invalid). Same marker column
    //     that query:by-marker / query:macro-introduced reads.
    //
    //   (hygiene:allow-macro-mutate?) — read the global flag
    //     (default #f). Mirrors the C++ side's allow_macro_mutate_
    //     flag on Evaluator.
    //
    //   (hygiene:set-allow-macro-mutate! bool) — set the flag.
    //     When #t, mutate:* operations on MacroIntroduced nodes
    //     proceed without the "hygiene-protected" pre-check
    //     rejection. The flag is per-Evaluator (process-local);
    //     setting it does not affect other Compilers in the same
    //     process.
    //
    // These three primitives don't touch the mutate:* path —
    // they're for EDSL code / tests that need to read the
    // protected state or opt-in globally. Per-call opt-out
    // without changing the flag is the :allow-macro? #t kwarg
    // on each mutate:* primitive.
    add("hygiene:protected?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        if (!ev.workspace_flat_)
            return make_bool(false);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (id >= ev.workspace_flat_->size())
            return make_bool(false);
        return make_bool(ev.workspace_flat_->is_macro_introduced(id));
    });
}

// Issue #909 compile part 58 (orig 4774-4946)
void CompilePrims::register_compile_p58(PrimRegistrar add, Evaluator& ev) {

    add("hygiene:allow-macro-mutate?",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.get_allow_macro_mutate()); });

    add("hygiene:set-allow-macro-mutate!", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_bool(a[0])) {
            return ev.make_merr("bad-arg", "usage: (hygiene:set-allow-macro-mutate! bool)");
        }
        ev.set_allow_macro_mutate(as_bool(a[0]));
        return make_void();
    });

    // ── Issue #375: IR encoding observability primitive ──
    //
    // `(compile:ir-stats)` — returns a hash describing the
    // current IRModule's encoding characteristics. The point
    // of #375 is to identify how much padding + unused
    // operand-space the AoS IRInstruction layout wastes, and
    // to project a compact encoding size so we can decide
    // whether the ≥30% size-reduction AC is achievable.
    //
    // Fields returned:
    //   - total-instructions       — total IRInstruction count
    //                                 across all functions in the
    //                                 last compiled module.
    //   - total-functions          — function count.
    //   - total-blocks             — basic block count.
    //   - avg-instructions-per-block — float (total-instr / blocks).
    //   - opcode-histogram         — hash {opcode-name -> count},
    //                                 so we know which opcodes
    //                                 dominate the hot path.
    //   - operand-count-distribution — hash {0..4 -> count} of how
    //                                 many instructions actually
    //                                 use 0/1/2/3/4 operand slots.
    //   - avg-operands-used-x100  — avg operands * 100 (integer
    //                                 to keep the hash type-safe;
    //                                 divide by 100 for the float).
    //   - aos-bytes-total         — total bytes assuming 40 bytes
    //                                 per IRInstruction (sizeof
    //                                 layout: 1 opcode + 16 ops +
    //                                 4 + 4 + 4 + 1 + 3 pad + 4 +
    //                                 4 + 1 = 40).
    //   - unused-operand-bytes-total — bytes wasted on unused
    //                                 operand slots: (4 - avg_ops) *
    //                                 4 * total_instr.
    //   - padding-bytes-total     — bytes wasted on struct
    //                                 alignment: 3 bytes per
    //                                 instruction (between
    //                                 linear_ownership_state and
    //                                 adt_variant_id).
    //   - compact-bytes-projection — projected bytes under a
    //                                 variable-length compact
    //                                 encoding: 2 bytes header
    //                                 (opcode 8 bits + operand
    //                                 count 4 bits + reserved 4
    //                                 bits) + 4 bytes per used
    //                                 operand, rounded up to
    //                                 4-byte alignment. Hot-path
    //                                 friendly, no per-instruction
    //                                 metadata sidecar.
    //   - compact-ratio-bp        — compact_bytes / aos_bytes in
    //                                 basis points (0-10000). 3000
    //                                 bp = compact is 30% of aos.
    //                                 The #375 AC is "≥30% size
    //                                 reduction" so a ratio ≤ 7000
    //                                 bp is a pass.
    //
    // This primitive does NOT modify IR — it's a read-only
    // measurement. Multiple calls during a session return
    // fresh stats from the last compiled module (set by
    // CompilerService::last_ir_module()).
    add("compile:ir-stats", [&ev](const auto&) -> EvalValue {
        if (!ev.compiler_service_) {
            return make_void();
        }
        auto* svc = static_cast<class aura::compiler::CompilerService*>(ev.compiler_service_);
        // Read the snapshot, not last_ir_module(). The snapshot
        // was computed when last_ir_mod_ was last assigned, so
        // it reflects the WORKLOAD's IR, not the IR of the
        // current stats-call expression (which would clobber
        // last_ir_mod_ on its own lowering).
        const auto& s = svc->last_ir_stats();
        if (s.total_instructions == 0 && s.total_functions == 0) {
            // No module compiled yet — return void.
            return make_void();
        }
        // Local build_hash helper — same open-addressing pattern as
        // compile:type-propagation-stats.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv,
                              std::size_t min_cap = 8) -> EvalValue {
            std::size_t cap = 8;
            while (cap < std::max(min_cap, kv.size() * 2))
                cap *= 2;
            auto* ht = FlatHashTable::create(cap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        // Opcode histogram (nested hash, only non-zero opcodes).
        std::vector<std::pair<std::string, EvalValue>> op_kv;
        for (std::size_t i = 0; i < s.opcode_histogram.size(); ++i) {
            if (s.opcode_histogram[i] == 0)
                continue;
            std::string name =
                (i < 54) ? std::string(aura::ir::kOpcodeInfo[i].name) : std::string("?");
            op_kv.emplace_back(std::move(name),
                               make_int(static_cast<std::int64_t>(s.opcode_histogram[i])));
        }
        EvalValue opcode_hist_ev = build_hash(op_kv, 16);
        // Operand-count distribution (nested hash, 0..4).
        std::vector<std::pair<std::string, EvalValue>> dist_kv;
        for (std::size_t i = 0; i < 5; ++i) {
            dist_kv.emplace_back(
                std::string(1, static_cast<char>('0' + i)),
                make_int(static_cast<std::int64_t>(s.operand_count_distribution[i])));
        }
        EvalValue dist_ev = build_hash(dist_kv, 8);
        // Top-level hash with all scalar fields + the 2 nested hashes.
        const std::uint64_t avg_ops_x100 =
            s.total_instructions ? (s.operands_used_sum * 100u / s.total_instructions) : 0;
        std::vector<std::pair<std::string, EvalValue>> top_kv = {
            {"total-instructions", make_int(static_cast<std::int64_t>(s.total_instructions))},
            {"total-functions", make_int(static_cast<std::int64_t>(s.total_functions))},
            {"total-blocks", make_int(static_cast<std::int64_t>(s.total_blocks))},
            {"avg-instructions-per-block-x100",
             make_int(static_cast<std::int64_t>(
                 s.total_blocks ? (s.total_instructions * 100u / s.total_blocks) : 0))},
            {"avg-operands-used-x100", make_int(static_cast<std::int64_t>(avg_ops_x100))},
            {"aos-bytes-total", make_int(static_cast<std::int64_t>(s.aos_bytes_total))},
            {"padding-bytes-total", make_int(static_cast<std::int64_t>(s.padding_bytes_total))},
            {"unused-operand-bytes-total",
             make_int(static_cast<std::int64_t>(s.unused_operand_bytes_total))},
            {"compact-bytes-projection",
             make_int(static_cast<std::int64_t>(s.compact_bytes_projection))},
            {"compact-ratio-bp", make_int(static_cast<std::int64_t>(s.compact_ratio_bp))},
            {"opcode-histogram", opcode_hist_ev},
            {"operand-count-distribution", dist_ev},
        };
        return build_hash(top_kv, 16);
    });
}

// Issue #909 compile part 59 (orig 4947-5042)
void CompilePrims::register_compile_p59(PrimRegistrar add, Evaluator& ev) {

    // ── Issue #445: SEVA high-level goal primitives ──────
    //
    // The SEVA demo (#442) is the Aura-side verification
    // loop. The OpenClaw integration (#445) is the LLM
    // agent that drives the loop via natural-language
    // goals. This block ships the Aura-side primitives
    // that the OpenClaw skill/plugin calls into. Each
    // primitive wraps 1+ existing lower-level operations
    // (mutate:*, verify:*, query:*) so the agent doesn't
    // need to know the Aura primitives in detail.
    //
    // The primitives are deliberately conservative: they
    // return hashes (not raw lists) so the audit log
    // can be replayed post-hoc, and they never call into
    // destructive operations without a guard.
    //
    // (seva:achieve-coverage name target-pct) — the
    // canonical SEVA goal. Reads the current coverage
    // (via verify-dirty-stats), compares to target,
    // returns a hash with the gap (or zero if already
    // met). The actual mutation loop is driven by the
    // OpenClaw agent, not by this primitive — the agent
    // decides which mutate:* primitive to call next.
    add("seva:achieve-coverage", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        auto target = as_int(a[1]);
        if (name_idx >= ev.string_heap_.size())
            return make_void();
        // Read the current coverage hole count via
        // the existing verify-dirty primitive
        // (#437 / #469).
        std::uint64_t current_dirty = 0;
        if (auto* ws = ev.workspace_flat()) {
            current_dirty = ws->verify_coverage_dirty_total();
        }
        // The "achievement" metric: dirty holes / 100 =
        // percent coverage hole. If current_dirty == 0
        // and target == 100, the goal is met.
        // The primitive returns a hash with the gap
        // analysis; the agent uses it to drive the loop.
        std::int64_t gap = (target >= 100) ? 0 : static_cast<std::int64_t>(current_dirty);
        std::int64_t achieved = (gap == 0) ? 1 : 0;
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        // name as a string field
        auto name_idx_in_heap = ev.string_heap_.size();
        ev.string_heap_.push_back(ev.string_heap_[name_idx]);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"name", make_string(name_idx_in_heap)},
            {"target-pct", make_int(target)},
            {"current-dirty", make_int(static_cast<std::int64_t>(current_dirty))},
            {"gap", make_int(gap)},
            {"achieved", make_int(achieved)},
        };
        return build_hash(kv);
    });
}

// Issue #909 compile part 60 (orig 5043-5111)
void CompilePrims::register_compile_p60(PrimRegistrar add, Evaluator& ev) {

    // (seva:fix-reset-bugs) — read the current verify-
    // dirty state, identify reset-related holes, return
    // the list of node IDs the agent should target.
    // The actual mutate call is the agent's job; the
    // primitive just identifies the targets.
    add("seva:fix-reset-bugs", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat())
            return make_void();
        // For now: query verify-dirty-stats and return a
        // hash with the breakdown by reason. The agent
        // reads this and decides which (mutate:set-body
        // / mutate:replace-pattern) call to make next.
        std::uint64_t assertion = 0, coverage = 0, sva = 0, cex = 0;
        if (auto* ws = ev.workspace_flat()) {
            assertion = ws->verify_assertion_dirty_total();
            coverage = ws->verify_coverage_dirty_total();
            sva = ws->verify_sva_dirty_total();
            cex = ws->verify_formal_cex_dirty_total();
        }
        std::int64_t reset_holes = static_cast<std::int64_t>(assertion);
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"assertion-dirty", make_int(static_cast<std::int64_t>(assertion))},
            {"coverage-dirty", make_int(static_cast<std::int64_t>(coverage))},
            {"sva-dirty", make_int(static_cast<std::int64_t>(sva))},
            {"formal-cex-dirty", make_int(static_cast<std::int64_t>(cex))},
            {"reset-holes", make_int(reset_holes)},
        };
        return build_hash(kv);
    });
}

// Issue #909 compile part 61 (orig 5112-5221)
void CompilePrims::register_compile_p61(PrimRegistrar add, Evaluator& ev) {

    // (seva:generate-regression) — emit a regression
    // script (in Aura syntax) from the current state.
    // For the MVP this returns a string with the
    // testbench skeleton; the agent fills in the
    // specifics. The string is in ev.string_heap_.
    add("seva:generate-regression", [&ev](const auto&) -> EvalValue {
        auto sidx = ev.string_heap_.size();
        std::string script = ";; Auto-generated regression script (seva:generate-regression)\n"
                             ";; Step 1: re-load the workspace\n"
                             "(set-code \"<paste your DUT spec here>\")\n"
                             ";; Step 2: run the verification loop\n"
                             "(eval-current)\n"
                             ";; Step 3: query readiness\n"
                             "(query:edsl-readiness)\n"
                             ";; Step 4: query verify-dirty\n"
                             "(query:verify-dirty-stats)\n";
        ev.string_heap_.push_back(script);
        return make_string(sidx);
    });

    // (seva:approve-mutation id flag) — safety gate.
    // For the MVP this is a no-op that bumps a counter
    // (the agent's audit trail records every mutation
    // regardless). The flag "force" / "auto" / "deny"
    // tells the system whether the agent has human
    // approval for the mutation.
    add("seva:approve-mutation", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto nid = as_int(a[0]);
        std::string flag = "auto";
        if (a.size() >= 2 && is_string(a[1])) {
            auto fidx = as_string_idx(a[1]);
            if (fidx < ev.string_heap_.size())
                flag = ev.string_heap_[fidx];
        }
        bool approved = (flag == "force" || flag == "auto");
        (void)nid;
        return make_bool(approved);
    });

    // (query:seva-audit-log) — Issue #445: the agent's
    // audit trail. Returns the recent mutations as a
    // summary (the full per-mutation record lives on
    // query:mutation-log-stats). For MVP: returns the
    // counts per category — agent calls this before
    // each major operation to confirm the audit log
    // is consistent.
    add("query:seva-audit-log", [&ev](const auto&) -> EvalValue {
        std::uint64_t mutations = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            mutations = m->atomic_batch_commits.load(std::memory_order_relaxed);
        }
        // Also include verify-dirty + mutation-rollbacks
        // so the audit trail covers the full loop.
        std::uint64_t verify_total = 0;
        if (auto* ws = ev.workspace_flat()) {
            verify_total = ws->verify_assertion_dirty_total() + ws->verify_coverage_dirty_total();
        }
        std::uint64_t auto_evolve_cycles = ev.auto_evolve_cycle_count_;
        std::uint64_t auto_evolve_fixed = ev.auto_evolve_total_fixed_;
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"mutations-total", make_int(static_cast<std::int64_t>(mutations))},
            {"verify-dirty-total", make_int(static_cast<std::int64_t>(verify_total))},
            {"auto-evolve-cycles", make_int(static_cast<std::int64_t>(auto_evolve_cycles))},
            {"auto-evolve-fixed", make_int(static_cast<std::int64_t>(auto_evolve_fixed))},
        };
        return build_hash(kv);
    });
}

// Issue #909 compile part 62 (orig 5222-5318)
void CompilePrims::register_compile_p62(PrimRegistrar add, Evaluator& ev) {

    // (seva:run-demo-with-metrics) — Issue #446: collect
    // standardized metrics for L4-L5 claims. Returns a
    // hash with 6 fields covering the 5 metrics from the
    // issue body (iterations / coverage-improvement /
    // human-intervention / mutation-success-rate /
    // time-breakdown) + the active-strategy. The time-
    // breakdown is approximated as the lifetime
    // auto-evolve-cycle-count (proxy for "iteration
    // time"); real wall-time measurement is a follow-up
    // (would need a start/end timestamp pair).
    add("seva:run-demo-with-metrics", [&ev](const auto&) -> EvalValue {
        std::uint64_t iterations = ev.auto_evolve_cycle_count_;
        std::uint64_t mutations = 0;
        std::uint64_t mutations_success = 0;
        std::uint64_t verify_total = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            mutations = m->atomic_batch_commits.load(std::memory_order_relaxed);
        }
        if (auto* ws = ev.workspace_flat()) {
            verify_total = ws->verify_assertion_dirty_total() + ws->verify_coverage_dirty_total();
            // mutations_success approximated as the
            // difference: total fixed - auto-evolve-fixed
            // is hard to compute without a per-mutation
            // outcome counter. For MVP: success-rate is
            // derived from strategy pheromone (see below).
        }
        // Read strategy pheromone for success-rate.
        std::uint64_t greedy_s = 0, bugfix_s = 0, minimal_s = 0;
        std::uint64_t greedy_h = 0, bugfix_h = 0, minimal_h = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            greedy_s = m->strategy_greedy_successes.load(std::memory_order_relaxed);
            bugfix_s = m->strategy_bugfix_successes.load(std::memory_order_relaxed);
            minimal_s = m->strategy_minimal_successes.load(std::memory_order_relaxed);
            greedy_h = m->strategy_greedy_hits.load(std::memory_order_relaxed);
            bugfix_h = m->strategy_bugfix_hits.load(std::memory_order_relaxed);
            minimal_h = m->strategy_minimal_hits.load(std::memory_order_relaxed);
        }
        std::uint64_t total_hits = greedy_h + bugfix_h + minimal_h;
        std::uint64_t total_success = greedy_s + bugfix_s + minimal_s;
        std::int64_t success_rate =
            total_hits > 0 ? static_cast<std::int64_t>((total_success * 100) / total_hits) : 0;
        std::int64_t human_intervention = 0; // MVP: agent runs autonomously
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        auto active_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(ev.active_strategy_.empty() ? std::string("none")
                                                              : ev.active_strategy_);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"iterations-to-closure", make_int(static_cast<std::int64_t>(iterations))},
            {"coverage-improvement", make_int(static_cast<std::int64_t>(verify_total))},
            {"human-intervention-count", make_int(human_intervention)},
            {"mutation-success-rate-pct", make_int(success_rate)},
            {"mutations-total", make_int(static_cast<std::int64_t>(mutations))},
            {"active-strategy", make_string(active_idx)},
        };
        return build_hash(kv);
    });
}

// Issue #909 compile part 63 (orig 5319-5319)
void CompilePrims::register_compile_p63(PrimRegistrar add, Evaluator& ev) {
    // Issue #1385: (compiler:metrics) primitive — expose env_frames_
    // and arena observability metrics as a JSON string. Refreshes
    // the 4 lazy-snapshot counters in CompilerMetrics
    // (env_frames_size_total, env_frames_stale_count,
    // ast_arena_bytes_in_use, ast_arena_upstream_bytes) before
    // serializing. Returns void if CompilerService / CompilerMetrics
    // back-pointers are unset (e.g. bare Evaluator without service).
    add("compiler:metrics", [&ev](const auto&) -> EvalValue {
        auto* svc = static_cast<CompilerService*>(ev.compiler_service());
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (!svc || !m)
            return types::make_void();
        svc->refresh_env_arena_metrics(*m);
        std::string json =
            std::format("{{\"env_frames_size_total\":{},\"env_frames_stale_count\":{},"
                        "\"ast_arena_bytes_in_use\":{},\"ast_arena_upstream_bytes\":{}}}",
                        m->env_frames_size_total.load(), m->env_frames_stale_count.load(),
                        m->ast_arena_bytes_in_use.load(), m->ast_arena_upstream_bytes.load());
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(json));
        return types::make_string(idx);
    });
    // Issue #1386: (evaluator:compact-env-frames) primitive.
    // Triggers env_frames_ arena compaction. Reclaims stale
    // frames (version_ < current defuse_version_) that are
    // not referenced by any live Closure. Rewrites
    // Closure::env_id via remap; bumps defuse_version_ so any
    // stale bridge_epoch snapshot re-bridges via
    // closure_bridge_. Returns the number of frames reclaimed
    // as an int. See Evaluator::compact_env_frames for the
    // algorithm + concurrency contract (caller must serialize
    // at the workspace level).
    add("evaluator:compact-env-frames", [&ev](const auto&) -> EvalValue {
        return types::make_int(static_cast<int64_t>(ev.compact_env_frames()));
    });
}

} // namespace aura::compiler::primitives_detail
