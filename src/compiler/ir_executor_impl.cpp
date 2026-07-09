module;
#include <bit>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include "runtime_shared.h"
#include "observability_logger.h"
#include "shape_jit_pass_closedloop_stats.h"
module aura.compiler.ir_executor;
import std;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.evaluator_pure;
// Issue #146 Phase 3: bring the pure is_truthy into the
// global namespace so existing call sites that use
// `is_truthy(x)` (unqualified) keep working.
using aura::compiler::pure::is_truthy;

// Shared runtime pair allocation (defined in aura_jit_runtime.cpp)
extern "C" std::int64_t aura_alloc_pair_arena(std::int64_t car, std::int64_t cdr);
extern "C" std::int64_t aura_alloc_pair(std::int64_t car, std::int64_t cdr);

namespace aura::compiler {

using namespace aura::ir;
using namespace aura::diag;
// Issue #918 Phase 1
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_void;

// Issue #638: bump linear ownership + GuardShape runtime safety
// counters when an instruction carries linear_ownership_state.
static void record_linear_runtime_safety(CompilerMetrics* metrics, bool mismatch) {
    if (!metrics)
        return;
    metrics->linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
    if (mismatch) {
        metrics->linear_deopt_on_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        metrics->linear_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
    } else {
        metrics->linear_check_pass_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

static void record_epoch_stale_steal_caught(CompilerMetrics* metrics) {
    if (!metrics)
        return;
    metrics->epoch_stale_steal_caught.fetch_add(1, std::memory_order_relaxed);
    metrics->linear_violation_prevented_epoch_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #740: linear JIT L2 safety counters on interpreter hot path.
static void record_linear_jit_safety(CompilerMetrics* metrics, IROpcode opcode) {
    if (!metrics)
        return;
    metrics->linear_jit_post_invalidate_total.fetch_add(1, std::memory_order_relaxed);
    switch (opcode) {
        case IROpcode::DropOp:
            metrics->linear_jit_drop_op_emitted_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case IROpcode::ArenaPop:
            metrics->linear_jit_arena_forced_post_mutate_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
            break;
        case IROpcode::GuardShape:
            metrics->linear_jit_arena_forced_post_mutate_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
            metrics->linear_jit_drop_op_emitted_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case IROpcode::Capture:
        case IROpcode::CaptureRef:
            metrics->linear_jit_gc_root_resync_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case IROpcode::MoveOp:
            metrics->linear_jit_drop_op_emitted_total.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

// Issue #61 Iter 4: deopt tracing switch. Off by default
// (perf: avoid a printf on every guard). Enable by setting
// AURA_DEOPT_TRACE=1 in the env before launching the process.
static const bool kDeoptTrace = []() {
    const char* e = std::getenv("AURA_DEOPT_TRACE");
    return e && (e[0] == '1' || e[0] == 't' || e[0] == 'T');
}();

// Issue #61 Iter 3: runtime shape of a value. Mirrors the
// encoding in aura_jit.h (SHAPE_INT=1, SHAPE_PAIR=10) and the
// service's set_shape_map encoder. Returns 0 for unknown/Dynamic.
static std::uint32_t runtime_shape_of(const EvalValue& v) {
    if (is_int(v))
        return 1; // SHAPE_INT
    if (is_bool(v))
        return 3; // SHAPE_BOOL (encoded as Int encoding in
                  // value but logically Bool shape)
    if (is_float(v))
        return 2; // SHAPE_FLOAT
    if (is_string(v))
        return 4; // SHAPE_STRING
    if (is_pair(v))
        return 10; // SHAPE_PAIR
    // Issue #160 sub-item #5: distinguish Closure and Vector
    // shapes for GuardShape precision. A specialized function
    // that takes a closure should not be confused with one
    // that takes a pair.
    if (is_closure(v))
        return 13; // SHAPE_CLOSURE
    return 0;      // SHAPE_DYNAMIC (default)
}

// Content-aware equality helper for strings (used by Eq instruction)
static bool eq_str_content(const EvalValue& a, const EvalValue& b,
                           std::vector<std::string>& string_heap) {
    if (a == b)
        return true;
    if (is_string(a) && is_string(b)) {
        auto ai = as_string_idx(a), bi = as_string_idx(b);
        if (ai < string_heap.size() && bi < string_heap.size())
            return string_heap[ai] == string_heap[bi];
        return false;
    }
    return false;
}

EvalResult IRInterpreter::execute() {
    if (module_.functions.empty())
        return std::unexpected(Diagnostic{ErrorKind::IRNoReturn, "empty module"});

    const auto& entry = module_.entry();
    auto local_count = entry.local_count + 64;
    call_stack_.clear();
    call_stack_.push_back({.func = &entry,
                           .current_block = entry.entry_block,
                           .locals = std::vector<EvalValue>(local_count, make_void()),
                           .args = {},
                           .resume_instr = 0,
                           .is_top_level = true,
                           .result_slot = 0,
                           .ex_depth_at_entry = ex_stack_.size()});

    while (!call_stack_.empty()) {
        auto& frame = call_stack_.back();
        auto result = run_function(*frame.func, frame.locals, frame.args);

        if (std::holds_alternative<PendingCall>(result)) {
            // Issue #252: PendingCall is the "actual closure call"
            // moment for the IR interpreter. Bump the IR counter
            // here (in addition to IROpcode::Call's bump, which
            // may or may not lead to a PendingCall — some Call
            // instructions resolve to primitives or other ops
            // before they reach this point).
            if (context_.metrics) {
                context_.metrics->closure_calls_total.fetch_add(1, std::memory_order_relaxed);
            }
            auto& pc = std::get<PendingCall>(result);
            auto new_count = pc.func->local_count + std::max(std::size_t(64), pc.args.size());
            std::vector<EvalValue> new_locals(new_count, make_void());
            call_stack_.push_back({.func = pc.func,
                                   .current_block = pc.func->entry_block,
                                   .locals = std::move(new_locals),
                                   .args = std::move(pc.args),
                                   .resume_instr = 0,
                                   .is_top_level = false,
                                   .result_slot = pc.result_slot,
                                   .ex_depth_at_entry = ex_stack_.size()});
            continue;
        }

        auto& eval_res = std::get<EvalResult>(result);
        if (!eval_res) {
            call_stack_.clear();
            return eval_res;
        }

        if (frame.is_top_level) {
            call_stack_.pop_back();
            return eval_res;
        }

        auto retval = *eval_res;
        call_stack_.pop_back();
        if (!call_stack_.empty()) {
            auto& caller_frame = call_stack_.back();
            if (frame.result_slot < caller_frame.locals.size())
                caller_frame.locals[frame.result_slot] = retval;
        }
    }

    return std::unexpected(Diagnostic{ErrorKind::IRCorruption, "stack underflow"});
}

EvalResult IRInterpreter::call_closure(std::uint64_t closure_id, std::span<const EvalValue> args) {
    auto it = runtime_closures_.find(closure_id);
    if (it == runtime_closures_.end()) {
        return std::unexpected(
            Diagnostic{ErrorKind::InvalidClosure, "unknown IR closure in call_closure"});
    }
    auto& closure = it->second;
    // Issue #681: bridge_epoch probe before IR dispatch.
    if (closure.bridge_epoch != 0 && context_.evaluator) {
        const auto cur = context_.evaluator->current_bridge_epoch();
        if (cur != 0 && context_.evaluator->is_bridge_stale(closure.bridge_epoch, cur)) {
            if (context_.metrics) {
                context_.metrics->compiler_closure_epoch_mismatch_hits.fetch_add(
                    1, std::memory_order_relaxed);
                context_.metrics->compiler_closure_safe_fallbacks.fetch_add(
                    1, std::memory_order_relaxed);
                context_.metrics->closure_stale_returns.fetch_add(1, std::memory_order_relaxed);
                record_epoch_stale_steal_caught(context_.metrics);
            }
            if (auto tw =
                    context_.evaluator->apply_closure(static_cast<ClosureId>(closure_id), args))
                return *tw;
            return std::unexpected(
                Diagnostic{ErrorKind::InvalidClosure,
                           "stale IR closure after mutation (bridge_epoch mismatch)"}
                    .with_suggestion("re-run (eval-current) after mutate"));
        }
        if (context_.metrics)
            context_.metrics->bridge_epoch_hit_count_.fetch_add(1, std::memory_order_relaxed);
    }
    if (closure.func_id >= module_.functions.size()) {
        return std::unexpected(
            Diagnostic{ErrorKind::IRCorruption, "invalid function id in IR closure"});
    }
    auto& callee_func = module_.functions[closure.func_id];
    std::vector<EvalValue> all_args;
    all_args.reserve(closure.env.size() + args.size());
    for (auto& ev : closure.env)
        all_args.push_back(ev);
    for (auto& a : args)
        all_args.push_back(a);

    if (context_.metrics) {
        context_.metrics->closure_calls_total.fetch_add(1, std::memory_order_relaxed);
        context_.metrics->closure_ir_calls.fetch_add(1, std::memory_order_relaxed);
    }

    call_stack_.clear();
    auto local_count = callee_func.local_count + std::max(std::size_t(64), all_args.size());
    call_stack_.push_back({.func = &callee_func,
                           .current_block = callee_func.entry_block,
                           .locals = std::vector<EvalValue>(local_count, make_void()),
                           .args = std::move(all_args),
                           .resume_instr = 0,
                           .is_top_level = true,
                           .result_slot = 0,
                           .ex_depth_at_entry = ex_stack_.size()});

    while (!call_stack_.empty()) {
        auto& frame = call_stack_.back();
        auto result = run_function(*frame.func, frame.locals, frame.args);

        if (std::holds_alternative<PendingCall>(result)) {
            if (context_.metrics) {
                context_.metrics->closure_calls_total.fetch_add(1, std::memory_order_relaxed);
            }
            auto& pc = std::get<PendingCall>(result);
            auto new_count = pc.func->local_count + std::max(std::size_t(64), pc.args.size());
            std::vector<EvalValue> new_locals(new_count, make_void());
            call_stack_.push_back({.func = pc.func,
                                   .current_block = pc.func->entry_block,
                                   .locals = std::move(new_locals),
                                   .args = std::move(pc.args),
                                   .resume_instr = 0,
                                   .is_top_level = false,
                                   .result_slot = pc.result_slot,
                                   .ex_depth_at_entry = ex_stack_.size()});
            continue;
        }

        auto& eval_res = std::get<EvalResult>(result);
        if (!eval_res) {
            call_stack_.clear();
            return eval_res;
        }

        if (frame.is_top_level) {
            call_stack_.pop_back();
            return eval_res;
        }

        auto retval = *eval_res;
        call_stack_.pop_back();
        if (!call_stack_.empty()) {
            auto& caller_frame = call_stack_.back();
            if (frame.result_slot < caller_frame.locals.size())
                caller_frame.locals[frame.result_slot] = retval;
        }
    }

    return std::unexpected(Diagnostic{ErrorKind::IRCorruption, "stack underflow"});
}

EvalResult IRInterpreter::execute_function(const IRFunction& func,
                                           std::span<const EvalValue> args) {
    auto local_count = func.local_count + std::max(std::size_t(64), args.size());
    std::vector<EvalValue> locals(local_count, make_void());
    auto result = run_function(func, locals, args);
    if (std::holds_alternative<EvalResult>(result))
        return std::get<EvalResult>(std::move(result));
    return std::unexpected(Diagnostic{ErrorKind::IRCorruption, "unexpected pending call"});
}

IRInterpreter::RunResult IRInterpreter::run_function(const IRFunction& func,
                                                     std::vector<EvalValue>& locals,
                                                     std::span<const EvalValue> args) {
    if (func.blocks.empty())
        return std::unexpected(Diagnostic{ErrorKind::IRNoReturn, "empty function"}.with_suggestion(
            "the function body is empty"));

    // ── Coercion helpers shared across arithmetic opcodes ────────
    // Report CastOp blame with source location and optional NodeId
    auto report_blame = [&](const char* expected_kind, const char* got_kind,
                            std::uint32_t blame_loc, std::uint32_t blame_node = 0) {
        std::string msg =
            std::string("runtime type mismatch: expected ") + expected_kind + ", got " + got_kind;
        if (blame_node != 0) {
            msg += " (node " + std::to_string(blame_node) + ")";
        }
        auto diag = Diagnostic(ErrorKind::TypeError, std::move(msg))
                        .with_blame(BlameInfo{BlameParty::Implicit, "", "runtime"});
        if (blame_loc != 0) {
            auto line = (blame_loc >> 16) & 0xFFFFu;
            auto col = blame_loc & 0xFFFFu;
            diag.location = SourceLocation{line, col, 0};
        }
        std::println(std::cerr, "{}", diag.format());
    };

    auto coerce_i = [&](const types::EvalValue& v) -> std::int64_t {
        if (is_int(v))
            return as_int(v);
        if (is_float(v))
            return static_cast<std::int64_t>(as_float(v));
        if (is_string(v)) {
            auto idx = as_string_idx(v);
            if (idx < string_heap_.size()) {
                try {
                    return static_cast<std::int64_t>(std::stoll(string_heap_[idx]));
                } catch (...) {
                    std::println(std::cerr, "error: type mismatch — expected Int, got String '{}'",
                                 string_heap_[idx]);
                    return 0;
                }
            }
        }
        if (is_bool(v))
            return as_bool(v) ? 1 : 0;
        return 0;
    };
    auto coerce_f = [&](const types::EvalValue& v) -> double {
        if (is_float(v))
            return as_float(v);
        return static_cast<double>(coerce_i(v));
    };

    std::uint32_t current = func.entry_block;

    while (current < func.blocks.size()) {
        auto& block = func.blocks[current];

        std::size_t resume_pos = call_stack_.empty() ? 0 : call_stack_.back().resume_instr;
        std::size_t start_idx = (resume_pos < block.instructions.size()) ? resume_pos : 0;
        for (std::size_t ii = start_idx; ii < block.instructions.size(); ++ii) {
            auto& instr = block.instructions[ii];
            auto& ops = instr.operands;

            // Issue #259: track type propagation coverage.
            // Bump total for every instruction; bump with_type
            // only when type_id was populated by lowering.
            // The ratio gives the propagation coverage %.
            if (context_.metrics) {
                context_.metrics->ir_instructions_total.fetch_add(1, std::memory_order_relaxed);
                if (instr.type_id != 0) {
                    context_.metrics->ir_instructions_with_type_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }

            // ── Operand validation via lookup_opcode ────────
            // Issue #217 Cycle 4: replaced direct kOpcodeInfo[idx]
            // access with the bounds-checked lookup_opcode helper
            // (declared in ir.ixx). The helper handles the bounds
            // check + array access in one call, returning nullptr
            // for invalid opcodes.
            if (auto* info = lookup_opcode(instr.opcode)) {
                if (info->has_result_slot && ops[0] >= locals.size()) {
                    return std::unexpected(Diagnostic{
                        ErrorKind::IRCorruption,
                        std::format("{}: result slot {} out of bounds", info->name, ops[0])});
                }
            }

            // Issue #910 Phase 2: class-table outer dispatch for hot
            // arithmetic/compare/logic (dense secondary switch, less
            // branch pressure than one 54-way switch on every instr).
            const auto oclass = opcode_class(instr.opcode);
            if (oclass == IROpcodeClass::Arith) {
                auto& a = locals[ops[1]];
                auto& b = locals[ops[2]];
                const bool fl = is_float(a) || is_float(b);
                switch (instr.opcode) {
                    case IROpcode::Add:
                        locals[ops[0]] = fl ? make_float(coerce_f(a) + coerce_f(b))
                                            : make_int(coerce_i(a) + coerce_i(b));
                        break;
                    case IROpcode::Sub:
                        locals[ops[0]] = fl ? make_float(coerce_f(a) - coerce_f(b))
                                            : make_int(coerce_i(a) - coerce_i(b));
                        break;
                    case IROpcode::Mul:
                        locals[ops[0]] = fl ? make_float(coerce_f(a) * coerce_f(b))
                                            : make_int(coerce_i(a) * coerce_i(b));
                        break;
                    case IROpcode::Div: {
                        if (fl) {
                            auto y = coerce_f(b);
                            if (y == 0.0)
                                return std::unexpected(
                                    Diagnostic{ErrorKind::DivisionByZero, "division by zero"}
                                        .with_suggestion("use a non-zero denominator"));
                            locals[ops[0]] = make_float(coerce_f(a) / y);
                        } else {
                            auto y = coerce_i(b);
                            if (y == 0)
                                return std::unexpected(
                                    Diagnostic{ErrorKind::DivisionByZero, "division by zero"}
                                        .with_suggestion("use a non-zero denominator"));
                            locals[ops[0]] = make_int(coerce_i(a) / y);
                        }
                        break;
                    }
                    default:
                        break;
                }
                continue; // next instruction
            }

            switch (instr.opcode) {
                case IROpcode::Nop:
                    break;
                // Arith handled above via kIROpcodeClass table (#910).
                case IROpcode::Add:
                case IROpcode::Sub:
                case IROpcode::Mul:
                case IROpcode::Div:
                    break;

                case IROpcode::ConstI64: {
                    std::int64_t val = (static_cast<std::int64_t>(ops[2]) << 32) | ops[1];
                    locals[ops[0]] = make_int(val);
                    break;
                }
                case IROpcode::ConstF64: {
                    std::uint64_t bits = static_cast<std::uint64_t>(ops[1]) |
                                         (static_cast<std::uint64_t>(ops[2]) << 32);
                    double val = std::bit_cast<double>(bits);
                    locals[ops[0]] = make_float(val);
                    break;
                }
                case IROpcode::ConstString: {
                    // Store string in both heaps: primitives (for primitives) and local (for
                    // coercion)
                    auto& prim_heap = context_.primitives.string_heap();
                    auto prim_idx = prim_heap.size();
                    if (ops[1] < module_.string_pool.size()) {
                        prim_heap.push_back(module_.string_pool[ops[1]]);
                        string_heap_.push_back(module_.string_pool[ops[1]]);
                    } else {
                        prim_heap.push_back("");
                        string_heap_.push_back("");
                    }
                    locals[ops[0]] = make_string(prim_idx);
                    break;
                }

                case IROpcode::Local:
                    locals[ops[0]] = locals[ops[1]];
                    break;

                case IROpcode::Arg:
                    if (ops[1] < args.size()) {
                        auto& val = args[ops[1]];
                        // Check if this is a cell reference (negative encoding in IR)
                        // Only treat as cell ref if the index is within the heap
                        if (is_int(val) && as_int(val) < 0) {
                            auto cell_slot = static_cast<std::size_t>(-as_int(val) - 1);
                            if (cell_slot < cell_heap_.size()) {
                                locals[ops[0]] = locals[cell_slot];
                            } else {
                                // Not a cell reference — actual negative int
                                locals[ops[0]] = val;
                            }
                        } else {
                            locals[ops[0]] = val;
                        }
                    } else
                        locals[ops[0]] = make_void();
                    break;

                    // Add/Sub/Mul/Div: handled by IROpcodeClass::Arith table path (#910).

                case IROpcode::Eq: {
                    // Content-aware equality (not just raw pointer comparison)
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    auto to_val = [](const EvalValue& v) -> double {
                        if (is_float(v))
                            return as_float(v);
                        if (is_int(v))
                            return static_cast<double>(as_int(v));
                        return 0.0;
                    };
                    // Float comparison
                    if (is_float(a) || is_float(b)) {
                        locals[ops[0]] = make_bool(to_val(a) == to_val(b));
                        break;
                    }
                    // String comparison by content
                    if (is_string(a) && is_string(b)) {
                        locals[ops[0]] = make_bool(eq_str_content(a, b, string_heap_));
                        break;
                    }
                    // Pair/list: fall back to raw equality (no pairs_ access here)
                    if (is_pair(a) && is_pair(b)) {
                        locals[ops[0]] = make_bool(a == b);
                        break;
                    }
                    // Default: raw EvalValue comparison
                    locals[ops[0]] = make_bool(a == b);
                    break;
                }
                case IROpcode::Lt: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    auto to_val = [](const EvalValue& v) -> double {
                        if (is_float(v))
                            return as_float(v);
                        if (is_int(v))
                            return static_cast<double>(as_int(v));
                        return 0.0; // non-numeric (pair, string, etc.) → 0
                    };
                    locals[ops[0]] = make_bool(to_val(a) < to_val(b));
                    break;
                }
                case IROpcode::Gt: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    auto to_val = [](const EvalValue& v) -> double {
                        if (is_float(v))
                            return as_float(v);
                        if (is_int(v))
                            return static_cast<double>(as_int(v));
                        return 0.0;
                    };
                    locals[ops[0]] = make_bool(to_val(a) > to_val(b));
                    break;
                }
                case IROpcode::Le: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    auto to_val = [](const EvalValue& v) -> double {
                        if (is_float(v))
                            return as_float(v);
                        if (is_int(v))
                            return static_cast<double>(as_int(v));
                        return 0.0;
                    };
                    locals[ops[0]] = make_bool(to_val(a) <= to_val(b));
                    break;
                }
                case IROpcode::Ge: {
                    auto& a = locals[ops[1]];
                    auto& b = locals[ops[2]];
                    auto to_val = [](const EvalValue& v) -> double {
                        if (is_float(v))
                            return as_float(v);
                        if (is_int(v))
                            return static_cast<double>(as_int(v));
                        return 0.0;
                    };
                    locals[ops[0]] = make_bool(to_val(a) >= to_val(b));
                    break;
                }

                case IROpcode::And:
                    locals[ops[0]] =
                        make_bool(is_truthy(locals[ops[1]]) && is_truthy(locals[ops[2]]));
                    break;
                case IROpcode::Or:
                    locals[ops[0]] =
                        make_bool(is_truthy(locals[ops[1]]) || is_truthy(locals[ops[2]]));
                    break;
                case IROpcode::Not:
                    locals[ops[0]] = make_bool(!is_truthy(locals[ops[1]]));
                    break;

                case IROpcode::CastOp: {
                    // CastOp: result_slot=ops[0], value_slot=ops[1], type_tag=ops[2]
                    // ops[3] = blame_loc packed (line<<16)|col (or 0)
                    // instr.type_id = blame_node (AST NodeId for error reporting)
                    // type_tag: 0=Int, 1=String, 2=Bool, 3+=Dynamic
                    // Issue #687: identity-cast fast-path. If the
                    // narrowing pass already proved source type
                    // matches target type, or if the source value
                    // is already of the target type at runtime,
                    // skip the cast entirely (Local copy). Pre-#687
                    // the interpreter always went through the
                    // 7-branch switch even for identity casts,
                    // costing 4-6 cycles per no-op. Matches the JIT
                    // fast-path at aura_jit.cpp:1435-1438.
                    if (ops[2] >= 3) {
                        // Dynamic target (type_tag >= 3 = passthrough).
                        // The CastOp default case is just `locals[ops[0]] = val`,
                        // so do that directly. Bumps zero-overhead_savings
                        // counter (the runtime version of the eliminated_cast_count).
                        locals[ops[0]] = locals[ops[1]];
                        if (metrics_) {
                            metrics_->dead_coercion_post_mutate_elim_hits_total.fetch_add(
                                1, std::memory_order_relaxed);
                            metrics_->dead_coercion_elision_runtime_check_savings_total.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        break;
                    }
                    auto& val = locals[ops[1]];
                    auto blame_loc = ops[3];
                    auto blame_node = instr.type_id;

                    switch (ops[2]) {
                        case 0: { // Coerce to Int
                            if (is_int(val)) {
                                locals[ops[0]] = val;
                            } else if (is_string(val)) {
                                auto idx = as_string_idx(val);
                                if (idx < string_heap_.size()) {
                                    try {
                                        locals[ops[0]] = make_int(static_cast<std::int64_t>(
                                            std::stoll(string_heap_[idx])));
                                    } catch (...) {
                                        locals[ops[0]] = make_int(0);
                                    }
                                } else {
                                    locals[ops[0]] = make_int(0);
                                }
                            } else if (is_bool(val)) {
                                locals[ops[0]] = make_int(as_bool(val) ? 1 : 0);
                            } else if (is_float(val)) {
                                locals[ops[0]] = make_int(static_cast<std::int64_t>(as_float(val)));
                            } else {
                                report_blame("Int", "unknown", blame_loc, blame_node);
                                locals[ops[0]] = make_int(0);
                            }
                            break;
                        }
                        case 1: { // Coerce to String
                            if (is_string(val)) {
                                locals[ops[0]] = val;
                            } else if (is_int(val)) {
                                auto s = std::to_string(as_int(val));
                                auto id = string_heap_.size();
                                string_heap_.push_back(std::move(s));
                                locals[ops[0]] = make_string(id);
                            } else if (is_bool(val)) {
                                auto s = as_bool(val) ? "#t" : "#f";
                                auto id = string_heap_.size();
                                string_heap_.push_back(std::move(s));
                                locals[ops[0]] = make_string(id);
                            } else if (is_float(val)) {
                                auto s = std::to_string(as_float(val));
                                auto id = string_heap_.size();
                                string_heap_.push_back(std::move(s));
                                locals[ops[0]] = make_string(id);
                            } else {
                                locals[ops[0]] = val;
                            }
                            break;
                        }
                        case 2: { // Coerce to Bool
                            locals[ops[0]] = make_bool(is_truthy(val));
                            break;
                        }
                        case 4: { // Coerce to Float
                            if (is_float(val)) {
                                locals[ops[0]] = val;
                            } else if (is_int(val)) {
                                locals[ops[0]] = make_float(static_cast<double>(as_int(val)));
                            } else if (is_string(val)) {
                                auto idx = as_string_idx(val);
                                if (idx < string_heap_.size()) {
                                    try {
                                        locals[ops[0]] = make_float(std::stod(string_heap_[idx]));
                                    } catch (...) {
                                        locals[ops[0]] = make_float(0.0);
                                    }
                                } else {
                                    locals[ops[0]] = make_float(0.0);
                                }
                            } else if (is_bool(val)) {
                                locals[ops[0]] = make_float(as_bool(val) ? 1.0 : 0.0);
                            } else {
                                report_blame("Float", "unknown", 0, blame_node);
                                locals[ops[0]] = make_float(0.0);
                            }
                            break;
                        }
                        default: // Dynamic / unknown: pass through
                            locals[ops[0]] = val;
                            break;
                    }
                    break;
                }

                case IROpcode::Branch:
                    current = is_truthy(locals[ops[0]]) ? ops[1] : ops[2];
                    goto next_block;

                // Issue #61 Iter 3: lazy-deopt guard. Compares the
                // runtime shape of ops[1] against the expected
                // shape (ops[2]). Writes 1 to ops[0] on match, 0 on
                // mismatch. The branch instruction that follows the
                // guard in the IR takes the mismatch to ops[3] (the
                // generic-trampoline block).
                case IROpcode::GuardShape: {
                    if (context_.evaluator)
                        (void)context_.evaluator->current_bridge_epoch();
                    if (instr.linear_ownership_state != 0)
                        record_linear_jit_safety(metrics_, IROpcode::GuardShape);
                    // Issue #684: deopt when SoA instruction_dirty_ says stale.
                    if (context_.is_instruction_dirty_fn &&
                        (instr.linear_ownership_state != 0 || instr.narrow_evidence != 0) &&
                        context_.is_instruction_dirty_fn(current, ii)) {
                        record_linear_runtime_safety(metrics_, true);
                        if (metrics_) {
                            metrics_->deopt_count.fetch_add(1, std::memory_order_relaxed);
                            metrics_->irsoa_cache_miss_reduction.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        locals[ops[0]] = types::make_bool(false);
                        break;
                    }
                    auto& val = locals[ops[1]];
                    auto expected = static_cast<std::uint32_t>(ops[2]);
                    // Issue #149 Phase 4: when occurrence-narrowing
                    // has produced the branch's type statically
                    // (narrow_evidence != 0 on the GuardShape
                    // instruction), trust the narrowed type over
                    // the runtime shape check. This is the hit-
                    // rate improvement the issue calls for: a
                    // narrowed value like (number? x) → Int
                    // skips the generic shape check (which would
                    // say "Dynamic" and deopt) and accepts the
                    // specialized path. The check is conservative
                    // (only enabled when narrow_evidence is set,
                    // which today is 0 at every instruction), so
                    // no behavior change until lowering starts
                    // populating narrow_evidence from real type
                    // info (Phase 2 follow-up).
                    if (instr.narrow_evidence != 0) {
                        // Narrowed: trust the inferred type
                        // implicitly. (The current interpreter
                        // doesn't track the narrowed type per-
                        // instruction; a follow-up adds a parallel
                        // type-table. For now, we just skip the
                        // shape check, accepting all narrowed
                        // values — equivalent to saying "the JIT
                        // can fast-path this".)
                        if (metrics_) {
                            metrics_->coercion_narrow_evidence_hits_total.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        locals[ops[0]] = types::make_bool(true);
                        break;
                    }
                    auto actual = runtime_shape_of(val);
                    // Issue #638: enforce linear_ownership_state on
                    // GuardShape when post-mutate shape may be stale.
                    if (instr.linear_ownership_state != 0) {
                        record_linear_runtime_safety(metrics_, actual != expected);
                    }
                    if (actual != expected) {
                        // Issue #62 Iter 1: increment the global
                        // deopt counter so --evo-explain can report it.
                        // Thread-safe via the atomic on the metrics
                        // struct.
                        if (metrics_) {
                            metrics_->deopt_count.fetch_add(1, std::memory_order_relaxed);
                        }
                        // Issue #744: GuardShape mismatch → speculative win lost.
                        aura::compiler::shape_jit_pass::record_speculative_win_lost();
                        // Issue #62 Iter 2: structured JSON log.
                        // Gated by AURA_OBS_LOG=1 (independent of the
                        // human-readable kDeoptTrace form).
                        const char* fn_name = "?";
                        if (module_.functions.size() > 0 && func.id < module_.functions.size())
                            fn_name = module_.functions[func.id].name.c_str();
                        log_event_deopt(fn_name, expected, static_cast<std::uint32_t>(actual),
                                        ops[3]);
                        if (kDeoptTrace) {
                            // Human-readable form (legacy)
                            std::fprintf(stderr,
                                         "[deopt] %s: shape mismatch (expected=%u actual=%u) "
                                         "→ deopt to generic block %u\n",
                                         fn_name, expected, actual, ops[3]);
                        }
                    }
                    locals[ops[0]] =
                        (actual == expected) ? types::make_bool(true) : types::make_bool(false);
                    break;
                }

                case IROpcode::Jump:
                    current = ops[0];
                    goto next_block;

                case IROpcode::MakePair: {
                    // Allocate pair via evaluator's cons (stores in pairs_ for
                    // format_value/display to find), then also register in
                    // g_pair_slots for arena/heap access by JIT runtime.
                    auto pfn = context_.primitives.lookup("cons");
                    if (pfn) {
                        locals[ops[0]] = (*pfn)({locals[ops[1]], locals[ops[2]]});
                        // Also push to shared g_pair_slots so JIT runtime + car/cdr
                        // fallback find this pair. Use arena alloc when non-escaping.
                        int64_t idx = static_cast<int64_t>(types::as_pair_idx(locals[ops[0]]));
                        bool use_arena = false;
                        if (g_use_arena && func.id < escape_maps_.size() &&
                            ops[0] < escape_maps_[func.id].size()) {
                            use_arena = (escape_maps_[func.id][ops[0]] == 0);
                        }
                        // Only push to g_pair_slots if this is a new pair ID
                        // (cons already created it in pairs_). Ensure g_pair_slots
                        // has enough space.
                        while (idx >= static_cast<int64_t>(g_pair_slots.size()))
                            g_pair_slots.push_back(nullptr);
                        if (!g_pair_slots[idx]) {
                            if (use_arena) {
                                auto* slot = (PairSlot*)tl_arena_alloc(
                                    &g_tl_arena, sizeof(PairSlot), alignof(PairSlot));
                                slot->car = locals[ops[1]].val;
                                slot->cdr = locals[ops[2]].val;
                                g_pair_slots[idx] = slot;
                            } else {
                                auto* slot = (PairSlot*)std::malloc(sizeof(PairSlot));
                                slot->car = locals[ops[1]].val;
                                slot->cdr = locals[ops[2]].val;
                                g_pair_slots[idx] = slot;
                                // Track heap allocation for process-exit
                                // cleanup (see g_owned_pair_slots_ in
                                // aura_jit_runtime.cpp). Without this,
                                // ASAN reports the slots as direct leaks
                                // at process exit since the static
                                // g_pair_slots vector only stores raw
                                // pointers, not ownership.
                                g_owned_pair_slots_.push_back(slot);
                            }
                        }
                    } else {
                        locals[ops[0]] = make_void();
                    }
                    break;
                }
                case IROpcode::Car: {
                    auto pfn = context_.primitives.lookup("car");
                    if (pfn)
                        locals[ops[0]] = (*pfn)({locals[ops[1]]});
                    else
                        locals[ops[0]] = make_void();
                    break;
                }
                case IROpcode::Cdr: {
                    auto pfn = context_.primitives.lookup("cdr");
                    if (pfn)
                        locals[ops[0]] = (*pfn)({locals[ops[1]]});
                    else
                        locals[ops[0]] = make_void();
                    break;
                }

                case IROpcode::Raise: {
                    // Issue #124: unwinds to the nearest TryBegin. If
                    // none is active, propagates the error up to
                    // the runtime (caller will see it as an EvalResult
                    // error).
                    if (ex_stack_.empty()) {
                        return std::unexpected(
                            Diagnostic{ErrorKind::UncaughtException, "uncaught exception"});
                    }
                    auto& top = ex_stack_.back();
                    // Store the cause in the handler's payload slot,
                    // then jump to the handler block.
                    auto cause_slot = (ops.size() > 1) ? ops[1] : 0;
                    locals[top.payload_slot] =
                        (cause_slot < locals.size()) ? locals[cause_slot] : make_void();
                    // The "result" slot will be filled in by the
                    // handler's own IsError/Local ops. We just set
                    // current (the local var in run_function's loop)
                    // to the handler.
                    current = top.handler_block;
                    continue; // restart the dispatch loop in the new block
                }

                case IROpcode::TryBegin: {
                    // ops[0] = handler_block
                    // ops[1] = result_slot (where to store caught value)
                    // ops[2] = payload_slot (temp slot for the cause)
                    ExHandler h;
                    h.handler_block = (ops.size() > 0) ? ops[0] : 0;
                    h.result_slot = (ops.size() > 1) ? ops[1] : 0;
                    h.payload_slot = (ops.size() > 2) ? ops[2] : 0;
                    ex_stack_.push_back(h);
                    break;
                }

                case IROpcode::TryEnd: {
                    // Pop the matching TryBegin (if any). The TryEnd
                    // marks the end of the try body; control flow after
                    // TryEnd is the "normal" path (i.e., no exception).
                    if (!ex_stack_.empty()) {
                        ex_stack_.pop_back();
                    }
                    // ops[0] is the result slot (we copy through whatever
                    // is in the matching slot if any, but for now this
                    // is mostly a no-op since the try body's result was
                    // already in a separate slot).
                    break;
                }
                    // Create an error value by calling the raise primitive
                case IROpcode::HashRef: {
                    auto pfn = context_.primitives.lookup("hash-ref");
                    if (pfn) {
                        auto& hash_val = locals[ops[1]];
                        auto& key_val = locals[ops[2]];
                        locals[ops[0]] = (*pfn)({hash_val, key_val});
                    } else {
                        locals[ops[0]] = make_void();
                    }
                    break;
                }
                case IROpcode::HashSet: {
                    auto pfn = context_.primitives.lookup("hash-set!");
                    if (pfn) {
                        auto& hash_val = locals[ops[1]];
                        auto& pair_val = locals[ops[2]];
                        auto cfn = context_.primitives.lookup("car");
                        auto dfn = context_.primitives.lookup("cdr");
                        if (cfn && dfn) {
                            auto key = (*cfn)({pair_val});
                            auto val = (*dfn)({pair_val});
                            (*pfn)({hash_val, key, val});
                        }
                    }
                    locals[ops[0]] = make_void();
                    break;
                }
                case IROpcode::HashRemove: {
                    auto pfn = context_.primitives.lookup("hash-remove!");
                    if (pfn) {
                        auto& hash_val = locals[ops[1]];
                        auto& key_val = locals[ops[2]];
                        locals[ops[0]] = (*pfn)({hash_val, key_val});
                    } else {
                        locals[ops[0]] = make_void();
                    }
                    break;
                }

                case IROpcode::IsError: {
                    // IsError: result_slot=ops[0], value_slot=ops[1]
                    locals[ops[0]] = make_bool(is_error(locals[ops[1]]));
                    break;
                }

                case IROpcode::PrimCall: {
                    // ops[0]=prim_id, ops[1]=arg_base, ops[2]=arg_count, ops[3]=result_slot
                    // Issue #891: no std::string temp; stack args for small arity;
                    // lookup_cstr avoids unordered_map key construction.
                    auto prim_id = static_cast<PrimId>(ops[0]);
                    auto arg_base = ops[1];
                    auto arg_count = ops[2];
                    auto result_slot = ops[3];
                    EvalValue stack_args[16];
                    std::vector<EvalValue> heap_args;
                    std::span<const EvalValue> pargs;
                    if (arg_count <= 16) {
                        for (std::uint32_t pi = 0; pi < arg_count; ++pi)
                            stack_args[pi] = locals[arg_base + pi];
                        pargs = std::span<const EvalValue>(stack_args, arg_count);
                    } else {
                        heap_args.reserve(arg_count);
                        for (std::uint32_t pi = 0; pi < arg_count; ++pi)
                            heap_args.push_back(locals[arg_base + pi]);
                        pargs = heap_args;
                    }
                    EvalResult presult = make_int(0);
                    auto idx = static_cast<std::size_t>(prim_id);
                    if (idx < std::size(kPrimNames)) {
                        auto pfn = context_.primitives.lookup_cstr(kPrimNames[idx]);
                        if (pfn)
                            presult = (*pfn)(pargs);
                    }
                    if (presult)
                        locals[result_slot] = *presult;
                    break;
                }

                case IROpcode::Call: {
                    // Issue #252: IR path closure dispatch counter.
                    // The IR has its own runtime_closures_ map
                    // (not the tree-walker's closures_), so this
                    // is a separate counter from apply_closure's.
                    // Bump the IR-specific counter on the shared
                    // CompilerMetrics (so snapshot() can sum them).
                    if (context_.metrics) {
                        context_.metrics->closure_ir_calls.fetch_add(1, std::memory_order_relaxed);
                        context_.metrics->closure_calls_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                    }
                    auto& callee_val = locals[ops[0]];
                    auto arg_base = ops[1];
                    auto arg_count = ops[2];

                    std::vector<EvalValue> call_args;
                    for (std::uint32_t i = 0; i < arg_count; ++i)
                        call_args.push_back(locals[arg_base + i]);

                    if (is_closure(callee_val)) {
                        auto closure_id = as_closure_id(callee_val);
                        auto it = runtime_closures_.find(closure_id);
                        if (it == runtime_closures_.end()) {
                            // Tree-walker closure passed into IR (e.g. lambda arg
                            // to a cached define). Delegate to evaluator bridge.
                            if (context_.evaluator) {
                                if (auto tw =
                                        context_.evaluator->apply_closure(closure_id, call_args))
                                    locals[ops[3]] = *tw;
                                else
                                    return std::unexpected(Diagnostic{ErrorKind::InvalidClosure,
                                                                      "invalid closure reference"});
                                break;
                            }
                            return std::unexpected(
                                Diagnostic{ErrorKind::InvalidClosure, "invalid closure reference"}
                                    .with_suggestion("the function was redefined (--hot-swap)"));
                        }

                        auto& closure = it->second;
                        // Issue #681: bridge_epoch version probe on apply.
                        if (closure.bridge_epoch != 0 && context_.evaluator) {
                            const auto cur = context_.evaluator->current_bridge_epoch();
                            if (cur != 0 &&
                                context_.evaluator->is_bridge_stale(closure.bridge_epoch, cur)) {
                                if (context_.metrics) {
                                    context_.metrics->compiler_closure_epoch_mismatch_hits
                                        .fetch_add(1, std::memory_order_relaxed);
                                    context_.metrics->compiler_closure_safe_fallbacks.fetch_add(
                                        1, std::memory_order_relaxed);
                                    record_epoch_stale_steal_caught(context_.metrics);
                                }
                                if (auto tw =
                                        context_.evaluator->apply_closure(closure_id, call_args))
                                    locals[ops[3]] = *tw;
                                else
                                    return std::unexpected(
                                        Diagnostic{ErrorKind::InvalidClosure,
                                                   "stale IR closure after mutation"});
                                break;
                            }
                        }
                        if (closure.func_id >= module_.functions.size())
                            return std::unexpected(
                                Diagnostic{ErrorKind::IRCorruption, "invalid closure function id"}
                                    .with_suggestion("internal error: IR module corrupted"));

                        auto& callee_func = module_.functions[closure.func_id];

                        std::vector<EvalValue> all_args;
                        for (auto& ev : closure.env)
                            all_args.push_back(ev);
                        for (auto& a : call_args)
                            all_args.push_back(a);

                        if (!call_stack_.empty())
                            call_stack_.back().resume_instr = &instr - &block.instructions[0] + 1;
                        return PendingCall{&callee_func, std::move(all_args), ops[3]};
                    } else if (is_primitive(callee_val)) {
                        // Primitive function call — look up and invoke
                        auto slot = as_primitive_slot(callee_val);
                        auto prim_name = context_.primitives.name_for_slot(slot);
                        auto pfn = context_.primitives.lookup(prim_name);
                        if (pfn) {
                            auto result = (*pfn)(call_args);
                            locals[ops[3]] = result;
                        } else {
                            locals[ops[3]] = callee_val;
                        }
                    } else {
                        locals[ops[3]] = callee_val;
                    }
                    break;
                }

                case IROpcode::Return:
                    return locals[ops[0]];

                case IROpcode::MakeClosure: {
                    auto id = next_closure_id_++;
                    IRClosure ircl;
                    ircl.func_id = ops[1];
                    ircl.env = std::vector<EvalValue>(ops[2], make_void());
                    // Copy bridge data from IRModule if available.
                    // Issue #224 Cycle 2: shared_ptr-based bridge.
                    // The copy bumps the refcount, so the closure
                    // independently keeps the FlatAST alive.
                    if (ops[1] < module_.closure_bridge.size()) {
                        auto& bd = module_.closure_bridge[ops[1]];
                        ircl.flat = bd.flat;
                        ircl.pool = bd.pool;
                        ircl.body_id = bd.body_id;
                        // Issue #223: capture the bridge's epoch so the
                        // IRClosure lifetime check can detect stale bridges.
                        ircl.bridge_epoch = bd.bridge_epoch;
                        if (ops[1] < module_.functions.size())
                            ircl.params = module_.functions[ops[1]].params;
                    }
                    runtime_closures_[id] = std::move(ircl);
                    locals[ops[0]] = make_closure(id);
                    break;
                }

                case IROpcode::Capture: {
                    if (instr.linear_ownership_state != 0)
                        record_linear_jit_safety(metrics_, IROpcode::Capture);
                    auto& closure_val = locals[ops[0]];
                    if (is_closure(closure_val)) {
                        auto closure_id = as_closure_id(closure_val);
                        auto it = runtime_closures_.find(closure_id);
                        if (it != runtime_closures_.end() && ops[1] < it->second.env.size())
                            it->second.env[ops[1]] = locals[ops[2]];
                    }
                    break;
                }

                case IROpcode::CaptureRef: {
                    if (instr.linear_ownership_state != 0)
                        record_linear_jit_safety(metrics_, IROpcode::CaptureRef);
                    auto& closure_val = locals[ops[0]];
                    if (is_closure(closure_val)) {
                        auto closure_id = as_closure_id(closure_val);
                        auto it = runtime_closures_.find(closure_id);
                        if (it != runtime_closures_.end() && ops[1] < it->second.env.size())
                            // Store negative offset to indicate cell reference
                            it->second.env[ops[1]] =
                                make_int(-1 - static_cast<std::int64_t>(ops[2]));
                    }
                    break;
                }

                case IROpcode::Apply: {
                    // Issue #252: IR path closure dispatch counter
                    // (same as IROpcode::Call above)
                    if (context_.metrics) {
                        context_.metrics->closure_ir_calls.fetch_add(1, std::memory_order_relaxed);
                        context_.metrics->closure_calls_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                    }
                    auto& closure_val = locals[ops[0]];
                    auto arg_count = ops[1];
                    std::vector<EvalValue> apply_args;
                    for (std::uint32_t i = 0; i < arg_count; ++i)
                        apply_args.push_back(locals[ops[0] + i + 1]);

                    if (is_closure(closure_val)) {
                        auto closure_id = as_closure_id(closure_val);
                        auto it = runtime_closures_.find(closure_id);
                        if (it == runtime_closures_.end()) {
                            if (context_.evaluator) {
                                if (auto tw =
                                        context_.evaluator->apply_closure(closure_id, apply_args))
                                    locals[ops[2]] = *tw;
                                else
                                    return std::unexpected(Diagnostic{ErrorKind::InvalidClosure,
                                                                      "invalid closure in apply"});
                                break;
                            }
                            return std::unexpected(
                                Diagnostic{ErrorKind::InvalidClosure, "invalid closure in apply"}
                                    .with_suggestion("the function was redefined (--hot-swap)"));
                        }

                        auto& closure = it->second;
                        if (closure.func_id >= module_.functions.size())
                            return std::unexpected(
                                Diagnostic{ErrorKind::IRCorruption, "invalid function id in apply"}
                                    .with_suggestion("internal error: IR module corrupted"));

                        auto& callee_func = module_.functions[closure.func_id];
                        std::vector<EvalValue> all_args;
                        for (auto& ev : closure.env)
                            all_args.push_back(ev);
                        for (auto& a : apply_args)
                            all_args.push_back(a);

                        if (!call_stack_.empty())
                            call_stack_.back().resume_instr = &instr - &block.instructions[0] + 1;
                        return PendingCall{&callee_func, std::move(all_args), ops[2]};
                    } else {
                        locals[ops[2]] = closure_val;
                    }
                    break;
                }

                case IROpcode::NewCell: {
                    auto cell_id = next_cell_id_++;
                    if (cell_heap_.size() <= cell_id)
                        cell_heap_.resize(cell_id + 1, make_void());
                    cell_heap_[cell_id] = make_void();
                    locals[ops[0]] = make_cell(cell_id);
                    break;
                }

                case IROpcode::CellSet: {
                    auto& cell_id_val = locals[ops[0]];
                    if (is_cell(cell_id_val)) {
                        auto cell_id = as_cell_id(cell_id_val);
                        if (cell_heap_.size() <= cell_id)
                            cell_heap_.resize(cell_id + 1, make_void());
                        cell_heap_[cell_id] = locals[ops[1]];
                    }
                    break;
                }

                case IROpcode::CellGet: {
                    auto& cell_id_val = locals[ops[1]];
                    if (is_cell(cell_id_val)) {
                        auto cell_id = as_cell_id(cell_id_val);
                        locals[ops[0]] =
                            (cell_id < cell_heap_.size()) ? cell_heap_[cell_id] : make_void();
                    } else {
                        locals[ops[0]] = make_void();
                    }
                    break;
                }

                case IROpcode::TopCellLoad: {
                    if (context_.evaluator && ops[1] < context_.evaluator->cells().size())
                        locals[ops[0]] = context_.evaluator->cells()[ops[1]];
                    else
                        locals[ops[0]] = make_void();
                    break;
                }

                case IROpcode::ConstBool: {
                    locals[ops[0]] = make_bool(ops[1] != 0);
                    break;
                }

                case IROpcode::ConstVoid: {
                    locals[ops[0]] = make_void();
                    break;
                }

                case IROpcode::Primitive: {
                    // Load a primitive value by slot index (#891: O(1) slot check, no name string)
                    auto prim_slot = ops[1];
                    if (context_.primitives.slot_lookup_fast(prim_slot)) {
                        locals[ops[0]] = types::make_primitive(prim_slot);
                    } else {
                        locals[ops[0]] = types::make_void();
                    }
                    break;
                }

                // ── M4 Linear ownership opcodes ────────────────────────
                case IROpcode::LinearWrap: {
                    // Wrap value in a linear container with refcount=1
                    auto inner = locals[ops[1]];
                    auto lin_id = next_linear_id_++;
                    if (linear_heap_.size() <= lin_id)
                        linear_heap_.resize(lin_id + 1);
                    linear_heap_[lin_id] = LinearEntry{inner, 1, true};
                    locals[ops[0]] = types::make_linear(lin_id);
                    break;
                }
                case IROpcode::MoveOp: {
                    if (instr.linear_ownership_state != 0)
                        record_linear_jit_safety(metrics_, IROpcode::MoveOp);
                    // Move ownership: decrement source refcount, pass value through
                    // Runtime check: double-move detection
                    // Issue #106: source invalidation. After a move,
                    // the source slot is zeroed so a subsequent
                    // DropOp on the source becomes a no-op (the
                    // runtime's drop pair/cell/closure helpers all
                    // bounds-check or check the IS_PAIR low-bit tag,
                    // and 0 fails both checks). For linear values
                    // the linear_heap_ entry is already erased /
                    // ref-count-decremented above; clearing the
                    // local slot makes the no-double-drop invariant
                    // explicit in the IR.
                    auto val = locals[ops[1]];
                    if (types::is_linear(val)) {
                        auto lin_id = types::as_linear_id(val);
                        if (lin_id < linear_heap_.size() && linear_heap_[lin_id].live) {
                            auto& entry = linear_heap_[lin_id];
                            if (entry.ref_count <= 0) {
                                std::println(std::cerr, "error: double move — value already moved");
                                if (instr.linear_ownership_state != 0)
                                    record_linear_runtime_safety(metrics_, true);
                                locals[ops[0]] = entry.value;
                            } else {
                                auto result = entry.value;
                                if (--entry.ref_count == 0)
                                    entry.live = false;
                                if (instr.linear_ownership_state != 0)
                                    record_linear_runtime_safety(metrics_, false);
                                locals[ops[0]] = result;
                            }
                        } else {
                            // Already moved/consumed — error
                            std::println(std::cerr,
                                         "error: use after move — value already consumed");
                            if (instr.linear_ownership_state != 0)
                                record_linear_runtime_safety(metrics_, true);
                            locals[ops[0]] = types::make_int(0);
                        }
                    } else {
                        locals[ops[0]] = val;
                    }
                    // Source slot is now invalid — clear it so a
                    // later DropOp on this slot is a no-op.
                    locals[ops[1]] = types::make_int(0);
                    break;
                }
                case IROpcode::BorrowOp: {
                    // Immutable borrow: increment refcount
                    // Runtime check: use-after-move detection
                    auto val = locals[ops[1]];
                    if (types::is_linear(val)) {
                        auto lin_id = types::as_linear_id(val);
                        if (lin_id < linear_heap_.size() && linear_heap_[lin_id].live) {
                            auto& entry = linear_heap_[lin_id];
                            if (entry.ref_count <= 0) {
                                std::println(std::cerr, "error: double move — value already moved");
                                if (instr.linear_ownership_state != 0)
                                    record_linear_runtime_safety(metrics_, true);
                                locals[ops[0]] = entry.value;
                            } else {
                                entry.ref_count++;
                                if (instr.linear_ownership_state != 0)
                                    record_linear_runtime_safety(metrics_, false);
                                locals[ops[0]] = entry.value;
                            }
                        } else {
                            std::println(std::cerr,
                                         "error: use after move — borrow of consumed value");
                            if (instr.linear_ownership_state != 0)
                                record_linear_runtime_safety(metrics_, true);
                            locals[ops[0]] = types::make_int(0);
                        }
                    } else {
                        locals[ops[0]] = val;
                    }
                    break;
                }
                case IROpcode::MutBorrowOp: {
                    // Mutable borrow: treat as move (exclusive access)
                    auto val = locals[ops[1]];
                    if (types::is_linear(val)) {
                        auto lin_id = types::as_linear_id(val);
                        if (lin_id < linear_heap_.size() && linear_heap_[lin_id].live) {
                            auto& entry = linear_heap_[lin_id];
                            if (entry.ref_count <= 0) {
                                std::println(std::cerr, "error: mut-borrow of moved value");
                                if (instr.linear_ownership_state != 0)
                                    record_linear_runtime_safety(metrics_, true);
                            } else {
                                if (instr.linear_ownership_state != 0)
                                    record_linear_runtime_safety(metrics_, false);
                            }
                            locals[ops[0]] = entry.value;
                        } else {
                            std::println(std::cerr, "error: mut-borrow of consumed value");
                            if (instr.linear_ownership_state != 0)
                                record_linear_runtime_safety(metrics_, true);
                            locals[ops[0]] = types::make_int(0);
                        }
                    } else {
                        locals[ops[0]] = val;
                    }
                    break;
                }
                case IROpcode::DropOp: {
                    if (instr.linear_ownership_state != 0)
                        record_linear_jit_safety(metrics_, IROpcode::DropOp);
                    // Explicit destruct: decrement refcount, erase if zero
                    // Runtime check: double-drop detection
                    auto val = locals[ops[0]];
                    if (types::is_linear(val)) {
                        auto lin_id = types::as_linear_id(val);
                        if (lin_id < linear_heap_.size() && linear_heap_[lin_id].live) {
                            auto& entry = linear_heap_[lin_id];
                            if (entry.ref_count <= 0) {
                                std::println(std::cerr,
                                             "error: double drop — value already dropped");
                            } else {
                                if (--entry.ref_count == 0)
                                    entry.live = false;
                            }
                        } else {
                            std::println(std::cerr, "error: double drop — value not in heap");
                        }
                    }
                    break;
                }
                case IROpcode::RefCountOp: {
                    // Runtime refcount operation: ops[2] = 1 for inc, 0 for dec
                    auto val = locals[ops[1]];
                    if (types::is_linear(val)) {
                        auto lin_id = types::as_linear_id(val);
                        if (lin_id < linear_heap_.size() && linear_heap_[lin_id].live) {
                            auto& entry = linear_heap_[lin_id];
                            if (ops[2] == 1)
                                entry.ref_count++;
                            else if (entry.ref_count > 0)
                                entry.ref_count--;
                        }
                    }
                    locals[ops[0]] = val;
                    break;
                }

                case IROpcode::ArenaPush: {
                    // ArenaPush(result_slot, size) — push frame, save offset
                    tl_arena_push(&g_tl_arena);
                    locals[ops[0]] = types::make_int(0);
                    break;
                }
                case IROpcode::ArenaPop: {
                    if (instr.linear_ownership_state != 0)
                        record_linear_jit_safety(metrics_, IROpcode::ArenaPop);
                    // ArenaPop(saved_slot) — pop frame
                    tl_arena_pop(&g_tl_arena);
                    break;
                }

                default:
                    break;
            }

            // ── Runtime type assertion (strict mode only) ─────────
            if (strict_mode_ && instr.type_id != 0) {
                // Issue #217 Cycle 4: use lookup_opcode
                // helper for the bounds check + access.
                if (auto* info = lookup_opcode(instr.opcode); info && info->has_result_slot) {
                    auto rv =
                        check_runtime_type(instr.type_id, locals[ops[0]], std::string(info->name));
                    if (rv)
                        return std::unexpected(*rv);
                }
            }
        }
        ++current;
    next_block:;
    }

    return std::unexpected(Diagnostic{ErrorKind::IRNoReturn, "no return"}.with_suggestion(
        "all code paths must reach a return"));
}

// ── Runtime type checking implementation ─────────────────────

std::optional<aura::core::TypeTag> IRInterpreter::value_type_tag(const EvalValue& val) {
    using namespace aura::core;
    if (types::is_int(val))
        return TypeTag::INT;
    if (types::is_float(val))
        return TypeTag::FLOAT;
    if (types::is_bool(val))
        return TypeTag::BOOL;
    if (types::is_string(val))
        return TypeTag::STRING;
    if (types::is_pair(val))
        return TypeTag::PAIR;
    if (types::is_vector(val))
        return TypeTag::VECTOR;
    if (types::is_closure(val))
        return TypeTag::CLOSURE;
    if (types::is_void(val))
        return TypeTag::VOID;
    if (types::is_hash(val))
        return TypeTag::HASH;
    return std::nullopt;
}

std::optional<aura::diag::Diagnostic> IRInterpreter::check_runtime_type(std::uint32_t type_id,
                                                                        const EvalValue& val,
                                                                        std::string_view context) {
    if (!strict_mode_ || !context_.type_registry || type_id == 0)
        return std::nullopt;

    auto runtime_tag = value_type_tag(val);
    if (!runtime_tag)
        return std::nullopt; // unknown runtime type, skip

    auto expected_tid = aura::core::TypeId{type_id, 1};
    if (!expected_tid.valid())
        return std::nullopt;
    auto expected_tag = context_.type_registry->tag_of(expected_tid);

    if (*runtime_tag == expected_tag)
        return std::nullopt; // match

    return aura::diag::Diagnostic{
        aura::diag::ErrorKind::TypeError,
        std::format("runtime type mismatch in {}: expected {} got value of different type", context,
                    context_.type_registry->name_of(expected_tid))};
}

// ── Runtime reflection implementation ─────────────────────────

std::optional<ClosureSnapshot> IRInterpreter::inspect_closure(std::uint64_t closure_id) const {
    auto it = runtime_closures_.find(closure_id);
    if (it == runtime_closures_.end())
        return std::nullopt;

    auto& closure = it->second;
    std::string fname;
    if (closure.func_id < module_.functions.size())
        fname = module_.functions[closure.func_id].name;
    else
        fname = "<unknown>";

    return make_snapshot(closure_id, closure);
}

std::vector<ClosureSnapshot> IRInterpreter::list_closures() const {
    std::vector<ClosureSnapshot> result;
    result.reserve(runtime_closures_.size());
    for (auto& [id, closure] : runtime_closures_) {
        result.push_back(make_snapshot(id, closure));
    }
    return result;
}

// Helper: build ClosureSnapshot from runtime closure data
ClosureSnapshot IRInterpreter::make_snapshot(std::uint64_t id, const IRClosure& closure) const {
    std::string fname;
    std::vector<std::string> params, free_vars, param_types;
    std::string return_type = "Any";
    if (closure.func_id < module_.functions.size()) {
        auto& f = module_.functions[closure.func_id];
        fname = f.name;
        params = f.params;
        free_vars = f.free_vars;
        // Populate param_types from IR function params (M3 §8.2)
        for (auto& p : f.params) {
            param_types.push_back("Any");
        }
        // Try to get return type from the last Return instruction
        for (auto& block : f.blocks) {
            for (auto& instr : block.instructions) {
                if (instr.opcode == aura::ir::IROpcode::Return && instr.type_id != 0) {
                    if (context_.type_registry) {
                        return_type = std::string(
                            context_.type_registry->name_of(aura::core::TypeId{instr.type_id, 1}));
                    } else {
                        return_type = "type:" + std::to_string(instr.type_id);
                    }
                }
            }
        }
    } else {
        fname = "<unknown>";
    }
    return ClosureSnapshot{.id = id,
                           .func_id = closure.func_id,
                           .func_name = std::move(fname),
                           .func_params = std::move(params),
                           .func_param_types = std::move(param_types),
                           .func_return_type = std::move(return_type),
                           .func_free_vars = std::move(free_vars),
                           .env = closure.env};
}

std::string IRInterpreter::type_of_closure(std::uint64_t closure_id) const {
    auto snap = inspect_closure(closure_id);
    if (!snap)
        return "<unknown>";

    std::string result = "(";
    for (std::size_t i = 0; i < snap->func_param_types.size(); i++) {
        if (i > 0)
            result += " ";
        result += snap->func_param_types[i];
    }
    result += " -> ";
    result += snap->func_return_type;
    result += ")";
    return result;
}

std::vector<CellSnapshot> IRInterpreter::list_cells() const {
    std::vector<CellSnapshot> result;
    result.reserve(cell_heap_.size());
    for (std::uint64_t id = 1; id < cell_heap_.size(); ++id) {
        result.push_back(CellSnapshot{.id = id, .value = cell_heap_[id]});
    }
    return result;
}

void IRInterpreter::collect_active_gc_roots(std::vector<std::int64_t>& closure_roots_out,
                                            std::uint64_t current_bridge_epoch) const {
    for (const auto& [id, ircl] : runtime_closures_) {
        if (ircl.bridge_epoch != 0 && ircl.bridge_epoch != current_bridge_epoch)
            continue;
        closure_roots_out.push_back(static_cast<std::int64_t>(id));
    }
}

} // namespace aura::compiler
